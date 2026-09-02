/* =========================================================================
 * Claude-OS Shell — dock central
 *
 * Barre d'icones centree en bas de l'ecran, ancree hors du flux des fenetres
 * par le protocole layer-shell. Inspiration : dock macOS pour la position et
 * l'agrandissement au survol, surfaces ChromeOS pour les couleurs.
 *
 * Principe d'energie : aucune minuterie, aucune boucle. Le dock ne fait rien
 * tant que l'utilisateur ne le touche pas ; les transitions sont portees par
 * le moteur CSS de GTK et ne s'executent que pendant le survol.
 * ========================================================================= */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
/* GDesktopAppInfo vit dans gio-unix, pas dans gio tout court. */
#include <gio/gdesktopappinfo.h>

#include "config.h"
#include "toplevels.h"
#include "visibility.h"

#define DOCK_ICON_SIZE  38         /* pictogramme dans un bouton de 52 px    */
#define HOVER_OPEN_MS  400         /* survol avant d'ouvrir la liste         */
#define HOVER_CLOSE_MS 250         /* sursis avant de la refermer            */

/* Etat du dock. Un seul par processus : il n'y a qu'un dock. */
static struct {
    ShellConfig *cfg;
    GtkWidget   *box;          /* conteneur des icones                      */
    char        *signature;    /* etat des fenetres deja affiche            */
} D;

/* -------------------------------------------------------------------------
 * Lancement d'une application
 * ------------------------------------------------------------------------- */
static GDesktopAppInfo *
app_info_for (const char *app_id)
{
    g_autofree char *desktop_id = g_strconcat (app_id, ".desktop", NULL);
    return g_desktop_app_info_new (desktop_id);
}

/* Premiere fenetre de cette application, ou NULL. Les fenetres reduites
 * viennent en dernier : ramener une fenetre visible est presque toujours ce
 * qu'on veut, et shell_toplevel_activate sait de toute facon retablir une
 * fenetre reduite si c'est la seule. */
static const ShellWindow *
first_window_of (const char *app_id)
{
    const GPtrArray *wins = shell_toplevels_get ();
    const ShellWindow *fallback = NULL;

    for (guint i = 0; wins != NULL && i < wins->len; i++) {
        ShellWindow *w = g_ptr_array_index (wins, i);
        if (!shell_app_id_matches (app_id, w->app_id))
            continue;
        if (!w->minimized)
            return w;
        if (fallback == NULL)
            fallback = w;
    }
    return fallback;
}

static void
on_item_clicked (GtkButton *button, gpointer user_data)
{
    const char *app_id = user_data;
    g_autoptr(GError) error = NULL;

    /* Application deja ouverte : on la ramene au premier plan plutot que
     * d'en lancer une seconde instance. C'est le comportement attendu d'un
     * dock ; pour une fenetre supplementaire, la liste au survol est la. */
    const ShellWindow *win = first_window_of (app_id);
    if (win != NULL) {
        shell_toplevel_activate (win);
        return;
    }

    /* g_app_info_launch gere le .desktop, les variables d'environnement et
     * le rattachement au bon cgroup. Bien preferable a un fork/exec brut. */
    g_autoptr(GDesktopAppInfo) info = app_info_for (app_id);

    if (info == NULL) {
        g_warning ("aucun fichier .desktop pour « %s »", app_id);
        return;
    }
    if (!g_app_info_launch (G_APP_INFO (info), NULL, NULL, &error))
        g_warning ("lancement de « %s » impossible : %s", app_id, error->message);

    (void) button;
}

/* GClosureNotify plutot qu'un transtypage de g_free : les deux signatures
 * different, et le transtypage fait a juste titre rouspeter le compilateur. */
static void
free_app_id (gpointer data, GClosure *closure)
{
    (void) closure;
    g_free (data);
}

/* -------------------------------------------------------------------------
 * Liste des fenetres au survol
 *
 * Une fenetre par ligne, cliquable pour la ramener au premier plan.
 *
 * Le panneau ne prend PAS le clavier : gtk_popover_set_autohide(FALSE).
 * Avec l'accrochage automatique, GTK poserait une saisie exclusive, et
 * survoler le dock volerait le focus a la fenetre dans laquelle on est en
 * train d'ecrire. Le prix a payer est qu'il faut gerer soi-meme la
 * fermeture, ce que font les deux minuteries ci-dessous.
 * ------------------------------------------------------------------------- */
typedef struct {
    char      *app_id;
    GtkWidget *popover;
    guint      open_timer;
    guint      close_timer;
} Hover;

static void
hover_free (gpointer data)
{
    Hover *h = data;
    if (h->open_timer  != 0) g_source_remove (h->open_timer);
    if (h->close_timer != 0) g_source_remove (h->close_timer);
    g_free (h->app_id);
    g_free (h);
}

/* Un popover attache par gtk_widget_set_parent doit etre detache a la main
 * avant que son parent ne disparaisse, sinon GTK signale un widget detruit
 * avec des enfants encore attaches. Le signal « destroy » est le bon moment :
 * il precede la liberation des donnees attachees a l'objet. */
static void
on_item_destroy (GtkWidget *button, gpointer data)
{
    Hover *h = data;
    (void) button;
    if (h->popover != NULL) {
        gtk_widget_unparent (h->popover);
        h->popover = NULL;
    }
}

static void on_hover_enter (GtkEventControllerMotion *c, double x, double y, gpointer data);
static void on_hover_leave (GtkEventControllerMotion *c, gpointer data);

/* Le survol se surveille sur le CONTENU du panneau, pas sur le GtkPopover
 * lui-meme : celui-ci est un conteneur de surface et ne recoit pas les
 * croisements de pointeur. Un controleur pose dessus ne se declenche jamais,
 * la fermeture differee l'emporte, et le panneau disparait pendant qu'on se
 * dirige vers lui -- constate a l'ecran, pointeur virtuel a l'appui. */
static void
watch_hover (GtkWidget *w, gpointer h)
{
    GtkEventControllerMotion *m =
        GTK_EVENT_CONTROLLER_MOTION (gtk_event_controller_motion_new ());
    g_signal_connect (m, "enter", G_CALLBACK (on_hover_enter), h);
    g_signal_connect (m, "leave", G_CALLBACK (on_hover_leave), h);
    gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (m));
}

static void
on_window_row_clicked (GtkButton *button, gpointer user_data)
{
    (void) button;
    shell_toplevel_activate (user_data);
}

/* Remplit le panneau avec les fenetres de cette application.
 * Renvoie le nombre de lignes : zero signifie qu'il n'y a rien a montrer. */
static guint
hover_fill (Hover *h)
{
    GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class (list, "dock-windows");

    const GPtrArray *wins = shell_toplevels_get ();
    guint count = 0;

    for (guint i = 0; wins != NULL && i < wins->len; i++) {
        ShellWindow *w = g_ptr_array_index (wins, i);
        if (!shell_app_id_matches (h->app_id, w->app_id))
            continue;

        /* Un titre vide arrive le temps que l'application le publie. Mieux
         * vaut une ligne sans nom qu'une fenetre absente de la liste. */
        const char *text = (w->title != NULL && *w->title != '\0')
                         ? w->title : "(sans titre)";

        GtkWidget *label = gtk_label_new (text);
        gtk_label_set_xalign (GTK_LABEL (label), 0.0);
        gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars (GTK_LABEL (label), 34);

        GtkWidget *row = gtk_button_new ();
        gtk_button_set_child (GTK_BUTTON (row), label);
        gtk_widget_add_css_class (row, "dock-window-row");
        if (w->activated)
            gtk_widget_add_css_class (row, "active");
        g_signal_connect (row, "clicked",
                          G_CALLBACK (on_window_row_clicked), w);

        gtk_box_append (GTK_BOX (list), row);
        count++;
    }

    watch_hover (list, h);
    gtk_popover_set_child (GTK_POPOVER (h->popover), list);
    return count;
}

static gboolean
hover_open (gpointer data)
{
    Hover *h = data;
    h->open_timer = 0;

    if (hover_fill (h) > 0)
        gtk_popover_popup (GTK_POPOVER (h->popover));
    return G_SOURCE_REMOVE;
}

static gboolean
hover_close (gpointer data)
{
    Hover *h = data;
    h->close_timer = 0;
    gtk_popover_popdown (GTK_POPOVER (h->popover));
    return G_SOURCE_REMOVE;
}

static void
hover_cancel_close (Hover *h)
{
    if (h->close_timer != 0) {
        g_source_remove (h->close_timer);
        h->close_timer = 0;
    }
}

static void
on_hover_enter (GtkEventControllerMotion *c, double x, double y, gpointer data)
{
    Hover *h = data;
    (void) c; (void) x; (void) y;

    hover_cancel_close (h);

    /* Deja ouvert : ne rien relancer. Sans ce garde-fou, entrer dans le
     * panneau reprogrammait l'ouverture, qui en reconstruisait le contenu
     * toutes les 400 ms ; le pointeur perdait la ligne qu'il survolait a
     * chaque reconstruction, et le clic tombait dans le vide -- constate en
     * journalisant les croisements, pointeur virtuel a l'appui. */
    if (gtk_widget_get_visible (h->popover))
        return;

    if (h->open_timer == 0)
        h->open_timer = g_timeout_add (HOVER_OPEN_MS, hover_open, h);
}

static void
on_hover_leave (GtkEventControllerMotion *c, gpointer data)
{
    Hover *h = data;
    (void) c;

    if (h->open_timer != 0) {
        g_source_remove (h->open_timer);
        h->open_timer = 0;
    }
    /* Sursis plutot que fermeture immediate : sans lui, le trajet de la
     * souris entre l'icone et le panneau le ferait disparaitre. */
    if (h->close_timer == 0)
        h->close_timer = g_timeout_add (HOVER_CLOSE_MS, hover_close, h);
}

/* -------------------------------------------------------------------------
 * Reorganisation par glisser-deposer
 *
 * Seules les applications EPINGLEES se deplacent : les autres entrees sont
 * deduites des fenetres ouvertes, leur position n'a rien a ranger.
 *
 * L'ordre est ecrit dans shell.conf des le depot. Une reorganisation qu'il
 * faudrait penser a enregistrer serait une reorganisation perdue au
 * prochain demarrage.
 * ------------------------------------------------------------------------- */
static void dock_rebuild (void);

static int
pinned_index (const char *app_id)
{
    for (guint i = 0; D.cfg->pinned[i] != NULL; i++)
        if (g_strcmp0 (D.cfg->pinned[i], app_id) == 0)
            return (int) i;
    return -1;
}

static void
pinned_move (int from, int to)
{
    guint n = g_strv_length (D.cfg->pinned);
    if (from < 0 || to < 0 || from == to || (guint) from >= n || (guint) to >= n)
        return;

    char *moved = D.cfg->pinned[from];

    if (from < to)
        for (int i = from; i < to; i++)
            D.cfg->pinned[i] = D.cfg->pinned[i + 1];
    else
        for (int i = from; i > to; i--)
            D.cfg->pinned[i] = D.cfg->pinned[i - 1];

    D.cfg->pinned[to] = moved;

    g_autoptr(GError) error = NULL;
    if (!shell_config_save (D.cfg, &error))
        g_warning ("ordre du dock non enregistre : %s", error->message);

    /* On reconstruit sans attendre la relecture du fichier : l'utilisateur
     * vient de lacher l'icone, elle doit etre a sa place tout de suite. */
    dock_rebuild ();
}

static GdkContentProvider *
on_drag_prepare (GtkDragSource *src, double x, double y, gpointer data)
{
    (void) src; (void) x; (void) y;
    return gdk_content_provider_new_typed (G_TYPE_STRING, (const char *) data);
}

static void
on_drag_begin (GtkDragSource *src, GdkDrag *drag, gpointer data)
{
    (void) drag;

    /* L'icone suit le curseur pendant le deplacement : sans elle, on
     * deplacerait un objet invisible. */
    GtkWidget *button = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (src));
    GtkWidget *image  = gtk_button_get_child (GTK_BUTTON (button));
    if (GTK_IS_IMAGE (image)) {
        GdkPaintable *p = gtk_image_get_paintable (GTK_IMAGE (image));
        if (p != NULL)
            gtk_drag_source_set_icon (src, p, DOCK_ICON_SIZE / 2, DOCK_ICON_SIZE / 2);
    }

    /* Une liste de fenetres restee ouverte pendant le deplacement flotterait
     * au-dessus du dock sans plus correspondre a rien. */
    Hover *h = g_object_get_data (G_OBJECT (button), "hover");
    if (h != NULL && h->popover != NULL)
        gtk_popover_popdown (GTK_POPOVER (h->popover));

    (void) data;
}

static gboolean
on_drop (GtkDropTarget *target, const GValue *value, double x, double y,
         gpointer data)
{
    (void) target; (void) x; (void) y;

    if (!G_VALUE_HOLDS_STRING (value))
        return FALSE;

    pinned_move (pinned_index (g_value_get_string (value)),
                 pinned_index ((const char *) data));
    return TRUE;
}

/* -------------------------------------------------------------------------
 * Construction d'une icone du dock
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_dock_item (const char *app_id, gboolean running, gboolean active,
                 gboolean pinned)
{
    g_autoptr(GDesktopAppInfo) info = app_info_for (app_id);

    /* Icone et libelle proviennent du .desktop : c'est la source d'autorite,
     * et cela evite de maintenir une table parallele qui se desynchronise. */
    g_autofree char *icon_from_desktop = NULL;
    const char *label = app_id;
    if (info != NULL) {
        GIcon *gicon = g_app_info_get_icon (G_APP_INFO (info));
        if (gicon != NULL)
            icon_from_desktop = g_icon_to_string (gicon);
        const char *name = g_app_info_get_display_name (G_APP_INFO (info));
        if (name != NULL)
            label = name;
    }
    const char *wanted_icon = icon_from_desktop ? icon_from_desktop : app_id;

    GtkWidget *box    = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *button = gtk_button_new ();

    /* Repli explicite : sans lui, une icone absente du theme affiche un carre
     * barre, ce qui est bien plus laid qu'un pictogramme generique. */
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
    const char *icon_name = gtk_icon_theme_has_icon (theme, wanted_icon)
                          ? wanted_icon
                          : "application-x-executable";
    GtkWidget *image = gtk_image_new_from_icon_name (icon_name);

    gtk_image_set_pixel_size (GTK_IMAGE (image), DOCK_ICON_SIZE);
    gtk_button_set_child (GTK_BUTTON (button), image);
    gtk_widget_add_css_class (button, "dock-item");
    if (running)
        gtk_widget_add_css_class (button, "running");

    /* L'identifiant est duplique : les entrees non epinglees viennent de la
     * liste des fenetres, qui change sous nos pieds a chaque evenement. */
    g_signal_connect_data (button, "clicked", G_CALLBACK (on_item_clicked),
                           g_strdup (app_id), free_app_id, 0);

    /* Point d'etat sous l'icone : present pour toutes les entrees afin que
     * la hauteur du dock ne change pas selon les applications ouvertes --
     * un dock qui grandit et retrecit est desagreable a l'usage. */
    GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (dot, "dock-indicator");
    if (active)
        gtk_widget_add_css_class (dot, "active");
    gtk_widget_set_opacity (dot, running ? 1.0 : 0.0);
    gtk_widget_set_halign (dot, GTK_ALIGN_CENTER);

    gtk_box_append (GTK_BOX (box), button);
    gtk_box_append (GTK_BOX (box), dot);

    if (pinned) {
        /* La chaine remise aux rappels doit survivre a la reconstruction du
         * dock : elle est dupliquee et liberee avec le bouton. */
        char *id = g_strdup (app_id);
        g_object_set_data_full (G_OBJECT (button), "app-id", id, g_free);

        GtkDragSource *src = gtk_drag_source_new ();
        gtk_drag_source_set_actions (src, GDK_ACTION_MOVE);
        g_signal_connect (src, "prepare", G_CALLBACK (on_drag_prepare), id);
        g_signal_connect (src, "drag-begin", G_CALLBACK (on_drag_begin), id);
        gtk_widget_add_controller (button, GTK_EVENT_CONTROLLER (src));

        GtkDropTarget *dst = gtk_drop_target_new (G_TYPE_STRING, GDK_ACTION_MOVE);
        g_signal_connect (dst, "drop", G_CALLBACK (on_drop), id);
        gtk_widget_add_controller (button, GTK_EVENT_CONTROLLER (dst));
    }

    /* Le survol ne liste que ce qui est ouvert : inutile de brancher quoi
     * que ce soit sur une application au repos. L'infobulle du nom reste,
     * elle, toujours utile. */
    if (!running) {
        gtk_widget_set_tooltip_text (button, label);
        return box;
    }

    Hover *h = g_new0 (Hover, 1);
    h->app_id  = g_strdup (app_id);
    h->popover = gtk_popover_new ();
    gtk_popover_set_autohide (GTK_POPOVER (h->popover), FALSE);
    gtk_popover_set_has_arrow (GTK_POPOVER (h->popover), FALSE);
    gtk_popover_set_position (GTK_POPOVER (h->popover), GTK_POS_TOP);
    gtk_widget_add_css_class (h->popover, "dock-windows-popover");
    gtk_widget_set_parent (h->popover, button);
    g_object_set_data_full (G_OBJECT (button), "hover", h, hover_free);
    g_signal_connect (button, "destroy", G_CALLBACK (on_item_destroy), h);

    watch_hover (button, h);
    return box;
}

/* -------------------------------------------------------------------------
 * Reconstruction du dock
 *
 * Le compositeur signale le moindre changement d'etat, y compris un simple
 * changement de titre -- un onglet change dans Chromium en emet un. Tout
 * reconstruire a chaque fois ferait clignoter le dock et refermerait la
 * liste ouverte sous le curseur.
 *
 * On compare donc une signature : quelles applications sont ouvertes, et
 * laquelle est active. C'est tout ce que le dock affiche ; les titres, eux,
 * ne sont lus qu'a l'ouverture de la liste.
 * ------------------------------------------------------------------------- */
static char *
windows_signature (void)
{
    GString *sig = g_string_new (NULL);
    const GPtrArray *wins = shell_toplevels_get ();

    for (guint i = 0; wins != NULL && i < wins->len; i++) {
        ShellWindow *w = g_ptr_array_index (wins, i);
        g_string_append_printf (sig, "%s%c|",
                                w->app_id ? w->app_id : "",
                                w->activated ? '*' : '-');
    }
    return g_string_free (sig, FALSE);
}

/* Cette application a-t-elle une fenetre, et l'une d'elles est-elle active ? */
static void
app_state (const char *app_id, gboolean *running, gboolean *active)
{
    const GPtrArray *wins = shell_toplevels_get ();
    *running = FALSE;
    *active  = FALSE;

    for (guint i = 0; wins != NULL && i < wins->len; i++) {
        ShellWindow *w = g_ptr_array_index (wins, i);
        if (!shell_app_id_matches (app_id, w->app_id))
            continue;
        *running = TRUE;
        if (w->activated)
            *active = TRUE;
    }
}

static gboolean
is_pinned (const char *app_id)
{
    for (guint i = 0; D.cfg->pinned[i] != NULL; i++)
        if (shell_app_id_matches (D.cfg->pinned[i], app_id))
            return TRUE;
    return FALSE;
}

static void
dock_rebuild (void)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child (D.box)) != NULL)
        gtk_box_remove (GTK_BOX (D.box), child);

    for (guint i = 0; D.cfg->pinned[i] != NULL; i++) {
        gboolean running, active;
        app_state (D.cfg->pinned[i], &running, &active);
        gtk_box_append (GTK_BOX (D.box),
                        build_dock_item (D.cfg->pinned[i], running, active, TRUE));
    }

    /* Applications ouvertes mais non epinglees : elles apparaissent apres un
     * separateur, sinon une fenetre ouverte depuis un terminal serait
     * invisible dans le dock et impossible a retrouver. */
    g_autoptr(GHashTable) vues = g_hash_table_new (g_str_hash, g_str_equal);
    const GPtrArray *wins = shell_toplevels_get ();
    gboolean separateur = FALSE;

    for (guint i = 0; wins != NULL && i < wins->len; i++) {
        ShellWindow *w = g_ptr_array_index (wins, i);
        if (w->app_id == NULL || *w->app_id == '\0')
            continue;
        if (is_pinned (w->app_id) || g_hash_table_contains (vues, w->app_id))
            continue;
        g_hash_table_add (vues, w->app_id);

        if (!separateur) {
            GtkWidget *sep = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
            gtk_widget_add_css_class (sep, "dock-separator");
            gtk_box_append (GTK_BOX (D.box), sep);
            separateur = TRUE;
        }

        gboolean running, active;
        app_state (w->app_id, &running, &active);
        gtk_box_append (GTK_BOX (D.box),
                        build_dock_item (w->app_id, running, active, FALSE));
    }
}

/* Rejoue tout ce qui depend de la configuration. Le dock est reconstruit de
 * toute facon : c'est le plus simple, et il ne compte qu'une dizaine
 * d'icones. */
static void
on_config_reloaded (ShellConfig *cfg, gpointer window)
{
    shell_config_free (D.cfg);
    D.cfg = cfg;

    shell_styles_load (cfg->theme);
    shell_config_apply (cfg);

    gtk_layer_set_exclusive_zone (GTK_WINDOW (window),
                                  cfg->reserve_space ? 86 : 0);
    dock_rebuild ();
}

static void
on_windows_changed (gpointer user_data)
{
    (void) user_data;

    g_autofree char *sig = windows_signature ();
    if (g_strcmp0 (sig, D.signature) == 0)
        return;

    g_free (D.signature);
    D.signature = g_steal_pointer (&sig);
    dock_rebuild ();
}

/* -------------------------------------------------------------------------
 * Bascule manuelle de la visibilite
 *
 * La bascule est exposee comme action GTK : chaque composant la publie sur
 * le bus de session sous son identifiant d'application, et le raccourci
 * clavier du compositeur l'appelle par « gapplication action ». Aucun
 * demon, aucune socket a nous, aucune chasse au numero de processus.
 * ------------------------------------------------------------------------- */
static void
on_visibilite (gboolean visible, gpointer window)
{
    /* Demasquer et masquer la surface, plutot que l'animer : deplacer une
     * surface layer-shell demanderait un reveil par image, pour un
     * mouvement de quelques dixiemes de seconde. Sur une machine dont
     * l'autonomie est la raison d'etre, l'apparition instantanee est le bon
     * compromis. */
    gtk_widget_set_visible (GTK_WIDGET (window), visible);
}

static void
on_action_basculer (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) param; (void) data;
    shell_visibility_toggle ();
}

static const GActionEntry actions[] = {
    { "basculer", on_action_basculer, NULL, NULL, NULL, { 0 } },
};

/* -------------------------------------------------------------------------
 * Fenetre du dock
 * ------------------------------------------------------------------------- */
static void
on_activate (GtkApplication *app, gpointer user_data)
{
    ShellConfig *cfg = user_data;

    shell_config_apply (cfg);

    GtkWidget *window = gtk_application_window_new (app);
    gtk_widget_add_css_class (window, "shell");

    /* --- Ancrage layer-shell ---------------------------------------------
     * Sans cela, le dock serait une fenetre ordinaire : elle passerait
     * derriere les autres et apparaitrait dans la liste des fenetres. */
    gtk_layer_init_for_window (GTK_WINDOW (window));
    gtk_layer_set_layer (GTK_WINDOW (window), GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor (GTK_WINDOW (window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_namespace (GTK_WINDOW (window), "claude-os-dock");

    /* Zone reservee : par defaut AUCUNE.
     *
     * Reserver 86 px faisait retrecir toute fenetre maximisee de la hauteur
     * du dock -- mesure : 1920x1114 dock affiche, 1920x1200 dock masque. La
     * fenetre se redimensionnait donc a chaque appui sur la touche Windows,
     * et la bande de fond d'ecran laissee entre elle et le bas de l'ecran
     * etait visible autour de la pilule. Constate sur la machine, puis
     * reproduit ici.
     *
     * Le dock passe desormais par-dessus : afficher ou masquer une surface
     * ne doit pas remettre en page ce qu'il y a dessous.
     *
     * Les fenetres PLEIN ECRAN n'etaient, elles, jamais concernees : leur
     * geometrie se calcule sur la resolution de l'ecran, pas sur la zone
     * utile (labwc, view_apply_fullscreen_geometry) -- mesure a 1920x1200
     * dans les deux cas.
     *
     * reserve_space=true dans shell.conf retablit l'ancien comportement pour
     * qui prefere que rien ne passe sous le dock. */
    gtk_layer_set_exclusive_zone (GTK_WINDOW (window),
                                  cfg->reserve_space ? 86 : 0);

    /* Le dock ne prend le clavier a aucun moment : la saisie continue d'aller
     * a la fenetre active meme quand la souris le survole. */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (window),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GtkWidget *dock = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (dock, "dock");
    gtk_widget_set_halign (dock, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (dock, GTK_ALIGN_END);

    D.cfg = cfg;
    D.box = dock;
    dock_rebuild ();

    gtk_window_set_child (GTK_WINDOW (window), dock);
    gtk_window_present (GTK_WINDOW (window));

    g_action_map_add_action_entries (G_ACTION_MAP (app), actions,
                                     G_N_ELEMENTS (actions), NULL);

    /* Sans cela, masquer la seule fenetre ferait sortir GApplication de sa
     * boucle : le dock disparaitrait pour de bon au lieu de se cacher. */
    g_application_hold (G_APPLICATION (app));

    shell_visibility_init (on_visibilite, window);
    shell_toplevels_init (on_windows_changed, NULL);
    shell_config_watch (on_config_reloaded, window);
}

int
main (int argc, char **argv)
{
    ShellConfig *cfg = shell_config_load ();

    /* Les options de ligne de commande priment sur le fichier : pratique
     * pour essayer un theme sans toucher a sa configuration. */
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0 (argv[i], "--dark") == 0)  cfg->dark = TRUE;
        if (g_strcmp0 (argv[i], "--light") == 0) cfg->dark = FALSE;
    }

    GtkApplication *app = gtk_application_new ("os.claude.shell.dock",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "startup",  G_CALLBACK (shell_styles_startup), cfg);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), cfg);

    /* Les arguments sont deja traites ci-dessus ; on n'en passe aucun a GTK
     * pour eviter qu'il ne rejette --dark comme option inconnue. */
    int status = g_application_run (G_APPLICATION (app), 0, NULL);
    g_object_unref (app);
    return status;
}
