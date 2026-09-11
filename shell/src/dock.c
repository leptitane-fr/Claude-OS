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
 *
 * Le dock sort de l'ecran quand on travaille dans une application, et c'est
 * lui qui en decide pour la barre d'etat aussi : voir visibility.h pour la
 * regle, et « A l'ecran ou non » plus bas pour la mecanique.
 * ========================================================================= */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
/* GDesktopAppInfo vit dans gio-unix, pas dans gio tout court. */
#include <gio/gdesktopappinfo.h>

#include <math.h>

#include "config.h"
#include "glissiere.h"
#include "toplevels.h"
#include "visibility.h"

#define DOCK_ICON_SIZE  38         /* pictogramme dans un bouton de 52 px    */
#define HOVER_OPEN_MS  400         /* survol avant d'ouvrir la liste         */
#define HOVER_CLOSE_MS 250         /* sursis avant de la refermer            */

/* Etat du dock. Un seul par processus : il n'y a qu'un dock. */
static struct {
    ShellConfig    *cfg;
    GtkApplication *app;
    GtkWidget      *fenetre;      /* la surface du dock                     */
    ShellGlissiere *glissiere;    /* ce qui la fait sortir par le bas       */
    GtkWidget      *box;          /* conteneur des icones                   */
    GtkWidget      *menu;         /* menu du clic droit, parente a box      */
    char           *signature;    /* etat des fenetres deja affiche         */
    gboolean        nappe;        /* fenetre tendue a tout l'ecran          */
    gboolean        declenche;    /* le glisser en cours a deja rappele     */
    int             barre_vue;    /* dernier ordre donne a la barre, -1 aucun */
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
        /* Si c'etait deja la fenetre active, le compositeur ne signale rien
         * -- rien n'a change pour lui. Le dock rappele resterait alors en
         * travers : on le congedie nous-memes. */
        shell_visibility_congedier ();
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
    shell_visibility_congedier ();      /* meme raison que on_item_clicked */
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

    /* Le dock est en train de descendre : le pointeur qui le traverse ne
     * doit pas ouvrir une liste accrochee a une icone qui s'en va. */
    if (shell_visibility_etat () == SHELL_VIS_CACHE)
        return;

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

/* Enregistre la liste des epinglees puis reconstruit sans attendre la
 * relecture du fichier : l'utilisateur vient d'agir, le dock doit suivre
 * tout de suite. */
static void
pinned_commit (void)
{
    g_autoptr(GError) error = NULL;
    if (!shell_config_save (D.cfg, &error))
        g_warning ("dock non enregistre : %s", error->message);
    dock_rebuild ();
}

/* Insere une application a la position voulue. `at` hors bornes place en
 * fin de liste -- c'est le cas du depot sur le fond du dock. */
static void
pinned_insert (const char *app_id, int at)
{
    if (pinned_index (app_id) >= 0)
        return;                      /* deja la : un depot n'est pas un doublon */

    guint n = g_strv_length (D.cfg->pinned);
    if (at < 0 || (guint) at > n)
        at = (int) n;

    char **liste = g_new0 (char *, n + 2);
    for (int i = 0; i < at; i++)
        liste[i] = D.cfg->pinned[i];
    liste[at] = g_strdup (app_id);
    for (guint i = at; i < n; i++)
        liste[i + 1] = D.cfg->pinned[i];

    /* g_free et non g_strfreev : les chaines ont ete reprises telles quelles
     * dans la nouvelle liste, les liberer ici laisserait des pointeurs morts. */
    g_free (D.cfg->pinned);
    D.cfg->pinned = liste;
    pinned_commit ();
}

static void
pinned_remove (const char *app_id)
{
    int at = pinned_index (app_id);
    if (at < 0)
        return;

    g_free (D.cfg->pinned[at]);
    for (guint i = at; D.cfg->pinned[i] != NULL; i++)
        D.cfg->pinned[i] = D.cfg->pinned[i + 1];

    pinned_commit ();
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
    pinned_commit ();
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

/* Un depot vient soit du dock lui-meme -- on reordonne -- soit du lanceur --
 * on epingle. La difference se lit sur la source : une application deja
 * epinglee se deplace, une autre s'insere.
 *
 * `data` porte l'identifiant de l'icone visee, ou NULL pour un depot sur le
 * fond du dock, qui ajoute en fin de liste. */
static gboolean
on_drop (GtkDropTarget *target, const GValue *value, double x, double y,
         gpointer data)
{
    (void) target; (void) x; (void) y;

    if (!G_VALUE_HOLDS_STRING (value))
        return FALSE;

    const char *depose = g_value_get_string (value);
    if (depose == NULL || *depose == '\0')
        return FALSE;

    int vers = (data != NULL) ? pinned_index ((const char *) data) : -1;
    int depuis = pinned_index (depose);

    if (depuis >= 0)
        pinned_move (depuis, vers);
    else
        pinned_insert (depose, vers);
    return TRUE;
}

/* -------------------------------------------------------------------------
 * Menu du clic droit
 *
 * Un dock sans clic droit oblige a passer par le panneau de reglages pour
 * retirer une icone, ce que personne ne devine. Les entrees dependent de
 * l'etat : on ne propose pas de fermer ce qui ne tourne pas, ni de detacher
 * ce qui n'est pas epingle.
 * ------------------------------------------------------------------------- */
static guint
count_windows (const char *app_id)
{
    const GPtrArray *wins = shell_toplevels_get ();
    guint n = 0;

    for (guint i = 0; wins != NULL && i < wins->len; i++) {
        ShellWindow *w = g_ptr_array_index (wins, i);
        if (shell_app_id_matches (app_id, w->app_id))
            n++;
    }
    return n;
}

static void
on_action_ouvrir (GSimpleAction *a, GVariant *param, gpointer data)
{
    (void) a; (void) data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GDesktopAppInfo) info =
        app_info_for (g_variant_get_string (param, NULL));

    if (info == NULL)
        return;
    if (!g_app_info_launch (G_APP_INFO (info), NULL, NULL, &error))
        g_warning ("lancement impossible : %s", error->message);
}

static void
on_action_fermer (GSimpleAction *a, GVariant *param, gpointer data)
{
    (void) a; (void) data;
    const char *app_id = g_variant_get_string (param, NULL);
    const GPtrArray *wins = shell_toplevels_get ();

    /* Toutes les fenetres de l'application : n'en fermer qu'une, sans dire
     * laquelle, serait imprevisible des qu'il y en a plusieurs. La fermeture
     * reste une demande -- chaque application garde la main. */
    for (guint i = 0; wins != NULL && i < wins->len; i++) {
        ShellWindow *w = g_ptr_array_index (wins, i);
        if (shell_app_id_matches (app_id, w->app_id))
            shell_toplevel_close (w);
    }
}

static void
on_action_epingler (GSimpleAction *a, GVariant *param, gpointer data)
{
    (void) a; (void) data;
    pinned_insert (g_variant_get_string (param, NULL), -1);
}

static void
on_action_detacher (GSimpleAction *a, GVariant *param, gpointer data)
{
    (void) a; (void) data;
    pinned_remove (g_variant_get_string (param, NULL));
}

static const GActionEntry actions_dock[] = {
    { "ouvrir",   on_action_ouvrir,   "s", NULL, NULL, { 0 } },
    { "fermer",   on_action_fermer,   "s", NULL, NULL, { 0 } },
    { "epingler", on_action_epingler, "s", NULL, NULL, { 0 } },
    { "detacher", on_action_detacher, "s", NULL, NULL, { 0 } },
};

static void
on_clic_droit (GtkGestureClick *geste, int n, double x, double y, gpointer data)
{
    const char *app_id = data;
    (void) n;

    GtkWidget *widget =
        gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (geste));

    guint fenetres = count_windows (app_id);
    g_autoptr(GMenu) menu = g_menu_new ();

    /* Forme « action::cible » : les identifiants .desktop ne contiennent que
     * lettres, chiffres, points et tirets, rien qui puisse etre relu comme
     * un litteral GVariant. */
    if (fenetres == 0) {
        g_autofree char *d = g_strdup_printf ("dock.ouvrir::%s", app_id);
        g_menu_append (menu, "Ouvrir", d);
    } else {
        g_autofree char *d = g_strdup_printf ("dock.ouvrir::%s", app_id);
        /* Certaines applications reutilisent leur fenetre au lieu d'en
         * ouvrir une seconde : c'est leur decision, pas la notre. */
        g_menu_append (menu, "Nouvelle fenêtre", d);
    }

    if (pinned_index (app_id) >= 0) {
        g_autofree char *d = g_strdup_printf ("dock.detacher::%s", app_id);
        g_menu_append (menu, "Retirer du dock", d);
    } else {
        g_autofree char *d = g_strdup_printf ("dock.epingler::%s", app_id);
        g_menu_append (menu, "Épingler au dock", d);
    }

    if (fenetres > 0) {
        g_autofree char *libelle = (fenetres == 1)
            ? g_strdup ("Fermer")
            : g_strdup_printf ("Fermer les %u fenêtres", fenetres);
        g_autofree char *d = g_strdup_printf ("dock.fermer::%s", app_id);
        g_menu_append (menu, libelle, d);
    }

    gtk_popover_menu_set_menu_model (GTK_POPOVER_MENU (D.menu), G_MENU_MODEL (menu));

    /* Les coordonnees du clic sont relatives au bouton ; le menu, lui, est
     * parente au conteneur du dock. */
    graphene_point_t pt;
    if (!gtk_widget_compute_point (widget, D.box,
                                   &GRAPHENE_POINT_INIT ((float) x, (float) y), &pt))
        return;

    gtk_popover_set_pointing_to (GTK_POPOVER (D.menu),
                                 &(GdkRectangle) { (int) pt.x, (int) pt.y, 1, 1 });
    gtk_popover_popup (GTK_POPOVER (D.menu));
}

/* -------------------------------------------------------------------------
 * Bouton du lanceur
 *
 * Le lanceur n'est demarre par personne au demarrage de la session : ce
 * bouton le lance la premiere fois, et comme il n'admet qu'une instance, les
 * appels suivants se contentent de le faire basculer. Rien n'est donc paye
 * en memoire tant qu'on ne s'en est pas servi.
 * ------------------------------------------------------------------------- */
static void
on_lanceur_clicked (GtkButton *button, gpointer data)
{
    (void) button; (void) data;
    g_autoptr(GError) error = NULL;

    if (!g_spawn_command_line_async ("claude-os-lanceur", &error))
        g_warning ("lanceur indisponible : %s", error->message);
}

static GtkWidget *
build_lanceur_item (void)
{
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
    const char *nom = gtk_icon_theme_has_icon (theme, "view-app-grid-symbolic")
                    ? "view-app-grid-symbolic"
                    : "applications-other";

    GtkWidget *image = gtk_image_new_from_icon_name (nom);
    gtk_image_set_pixel_size (GTK_IMAGE (image), DOCK_ICON_SIZE - 6);

    GtkWidget *button = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (button), image);
    gtk_widget_add_css_class (button, "dock-item");
    gtk_widget_add_css_class (button, "dock-lanceur");
    gtk_widget_set_tooltip_text (button, "Applications");
    g_signal_connect (button, "clicked", G_CALLBACK (on_lanceur_clicked), NULL);

    /* Meme enveloppe verticale que les autres entrees, point d'etat compris
     * mais invisible : sans elle le bouton ne serait pas aligne sur la meme
     * ligne de base que les icones d'applications. */
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (dot, "dock-indicator");
    gtk_widget_set_opacity (dot, 0.0);
    gtk_widget_set_halign (dot, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (box), button);
    gtk_box_append (GTK_BOX (box), dot);
    return box;
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

    /* La chaine remise aux rappels doit survivre a la reconstruction du
     * dock : elle est dupliquee et liberee avec le bouton. */
    char *id = g_strdup (app_id);
    g_object_set_data_full (G_OBJECT (button), "app-id", id, g_free);

    /* Le clic droit vaut pour TOUTE icone, epinglee ou non : c'est le seul
     * endroit d'ou l'on peut epingler une application ouverte depuis un
     * terminal, ou en detacher une. */
    GtkGestureClick *droit = GTK_GESTURE_CLICK (gtk_gesture_click_new ());
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (droit), GDK_BUTTON_SECONDARY);
    g_signal_connect (droit, "pressed", G_CALLBACK (on_clic_droit), id);
    gtk_widget_add_controller (button, GTK_EVENT_CONTROLLER (droit));

    if (pinned) {
        GtkDragSource *src = gtk_drag_source_new ();
        gtk_drag_source_set_actions (src, GDK_ACTION_MOVE);
        g_signal_connect (src, "prepare", G_CALLBACK (on_drag_prepare), id);
        g_signal_connect (src, "drag-begin", G_CALLBACK (on_drag_begin), id);
        gtk_widget_add_controller (button, GTK_EVENT_CONTROLLER (src));

        /* COPY en plus de MOVE : le dock se reordonne en MOVE, mais ce qui
         * vient du lanceur arrive en COPY -- deposer une application sur le
         * dock ne la retire pas de la liste des applications. Sans les deux
         * actions ici, GTK refuserait le depot venu du lanceur. */
        GtkDropTarget *dst = gtk_drop_target_new (G_TYPE_STRING,
                                                  GDK_ACTION_MOVE | GDK_ACTION_COPY);
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
    /* Le menu du clic droit est parente au conteneur sans y avoir ete
     * ajoute comme enfant de boite. Le passer a gtk_box_remove ferait
     * rouspeter GTK a chaque reconstruction. */
    GtkWidget *child = gtk_widget_get_first_child (D.box);
    while (child != NULL) {
        GtkWidget *suivant = gtk_widget_get_next_sibling (child);
        if (child != D.menu)
            gtk_box_remove (GTK_BOX (D.box), child);
        child = suivant;
    }

    /* Le lanceur en premier, comme l'etagere de ChromeOS : c'est le point
     * d'entree vers tout ce qui n'est pas epingle. */
    gtk_box_append (GTK_BOX (D.box), build_lanceur_item ());
    GtkWidget *sep_lanceur = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (sep_lanceur, "dock-separator");
    gtk_box_append (GTK_BOX (D.box), sep_lanceur);

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
                                  (cfg->reserve_space && !D.nappe) ? 86 : 0);
    dock_rebuild ();
}

static void
on_windows_changed (gpointer user_data)
{
    (void) user_data;

    /* Avant la signature, et a chaque lot : c'est ici que le dock apprend
     * qu'une application vient de passer au premier plan. */
    shell_visibility_fenetre_active (shell_toplevels_serie_active ());

    g_autofree char *sig = windows_signature ();
    if (g_strcmp0 (sig, D.signature) == 0)
        return;

    g_free (D.signature);
    D.signature = g_steal_pointer (&sig);
    dock_rebuild ();
}

/* -------------------------------------------------------------------------
 * A l'ecran ou non
 *
 * La regle est dans visibility.h. Ici, ce qu'il faut pour l'appliquer :
 * trois surfaces et un relais.
 *
 *   - LA FENETRE DU DOCK, qui glisse (glissiere.c) et qui, rappelee
 *     par-dessus une application, se tend a tout l'ecran pour recevoir le
 *     clic « a cote » : la nappe.
 *   - LA BANDE DU BORD, dix pixels au ras du bas de l'ecran, qui guette le
 *     doigt.
 *   - LA BARRE D'ETAT, un autre processus, a qui l'on dit « afficher » ou
 *     « masquer » sur le bus.
 *
 * TOUT EST EN COUCHE OVERLAY, ET C'EST MESURE. labwc 0.8.3 eteint la couche
 * TOP entiere des qu'une fenetre plein ecran n'a rien au-dessus d'elle
 * (desktop_update_top_layer_visibility) : constate au banc le 11 septembre
 * 2026, un dock en TOP disparait sous un « foot --fullscreen ». En TOP, ni
 * la touche Loupe ni le doigt ne pourraient rappeler le dock pendant une
 * video. OVERLAY passe au-dessus du plein ecran ; seuls le verrou, le
 * selecteur de fenetres et les menus du compositeur passent encore devant.
 *
 * L'EMPILEMENT. Dans une meme couche, labwc empile dans l'ordre de
 * creation des surfaces -- et gtk4-layer-shell en recree une a CHAQUE
 * reapparition d'une fenetre (lu dans la trace Wayland : trois
 * get_layer_surface « claude-os-dock » pour trois apparitions). Ce qui
 * reparait passe donc devant. Consequences :
 *
 *   - la bande, jamais retiree, reste au fond : le dock et la barre
 *     passent toujours devant elle ;
 *   - la barre, prevenue par le bus apres que le dock a reparu, passe en
 *     general devant lui. Mais pas toujours : deja affichee pour une
 *     banniere, elle ne reparait pas, et le dock rappele passe devant elle.
 *     C'est pourquoi la nappe ne reclame pas la bande du bas (nappe_zone).
 * ------------------------------------------------------------------------- */

/* La bande du bord.
 *
 * POURQUOI UNE SURFACE. labwc 0.8.3 ne connait aucun geste de bord : un
 * client ne recoit que les contacts poses sur ses propres surfaces. Pour
 * voir un doigt qui entre par le bas, il faut donc etre sous ce doigt au
 * moment ou il touche l'ecran.
 *
 * CE QU'ELLE COUTE. Dock cache, elle prend les appuis des dix derniers
 * pixels de l'ecran -- 1,6 mm sur cette dalle, qui fait 310 mm pour
 * 1920 px d'apres son EDID. Ils n'atteignent plus l'application dessous.
 * Plus haute, elle en volerait davantage ; plus basse, un doigt venu du
 * cadre risquerait de la manquer : le premier contact rapporte par la dalle
 * n'est pas forcement au dernier pixel. A ajuster a l'usage.
 *
 * GLISSER_PX : un trajet court, comme demande -- 5 mm. Le contact reste a
 * la bande pendant tout le geste, meme hors de ses dix pixels (le
 * compositeur reserve la suite d'un contact a la surface qui l'a recu) :
 * c'est ce qui permet de mesurer un trajet qui la quitte aussitot. */
#define BORD_PX    10
#define GLISSER_PX 32

static void
on_bord_debut (GtkGestureDrag *g, double x, double y, gpointer data)
{
    (void) g; (void) x; (void) y; (void) data;
    D.declenche = FALSE;
}

static void
on_bord_glisse (GtkGestureDrag *g, double dx, double dy, gpointer data)
{
    (void) g; (void) data;

    /* Des le seuil franchi, sans attendre qu'on leve le doigt : le dock
     * doit monter pendant que le geste se fait, pas apres. Plus vertical
     * qu'horizontal, pour qu'un doigt qui longe le bord ne le rappelle pas. */
    if (D.declenche || -dy < GLISSER_PX || -dy < fabs (dx))
        return;
    D.declenche = TRUE;
    shell_visibility_convoquer ();
}

static GtkWidget *
bord_creer (GtkApplication *app)
{
    GtkWidget *bord = gtk_application_window_new (app);

    /* PRESQUE TRANSPARENTE, ET SURTOUT PAS TOUT A FAIT.
     *
     * Une fenetre GTK entierement transparente et vide ne recoit AUCUN
     * appui : le compositeur ne lui envoie rien, sans une erreur nulle part.
     * Mesure au banc le 11 septembre 2026 (GTK 4.18.6, labwc 0.8.3), sur
     * une bande isolee : fond « transparent », pas un evenement ; fond a 1 %,
     * l'appui et tout le glisser arrivent. La bande a d'abord ete ecrite avec
     * la classe « shell » des autres surfaces, et le geste ne marchait pas.
     *
     * Rien a voir avec la nappe du dock ou celle de la barre : elles portent
     * un contenu, GTK dessine donc une vraie image, transparente autour.
     *
     * 1 % de noir sur dix pixels au ras du cadre ne se voit pas. La regle
     * est ecrite ici et non dans shell.css : ce n'est pas une couleur, c'est
     * une condition de fonctionnement, et un « transparent » pose un jour
     * par souci d'harmonie casserait le geste en silence. */
    static GtkCssProvider *presque = NULL;
    if (presque == NULL) {
        presque = gtk_css_provider_new ();
        gtk_css_provider_load_from_string (presque,
            "window.claude-os-bord { background-color: rgba(0, 0, 0, 0.01); }");
        gtk_style_context_add_provider_for_display (gdk_display_get_default (),
            GTK_STYLE_PROVIDER (presque), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_widget_add_css_class (bord, "claude-os-bord");

    gtk_layer_init_for_window (GTK_WINDOW (bord));
    gtk_layer_set_layer (GTK_WINDOW (bord), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace (GTK_WINDOW (bord), "claude-os-bord");
    gtk_layer_set_anchor (GTK_WINDOW (bord), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (bord), GTK_LAYER_SHELL_EDGE_LEFT,   TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (bord), GTK_LAYER_SHELL_EDGE_RIGHT,  TRUE);
    /* -1 : au vrai bord de l'ecran, quoi que les autres surfaces reservent,
     * et sans rien reserver elle-meme. */
    gtk_layer_set_exclusive_zone (GTK_WINDOW (bord), -1);
    gtk_layer_set_keyboard_mode (GTK_WINDOW (bord),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GtkWidget *plage = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request (plage, -1, BORD_PX);
    gtk_window_set_child (GTK_WINDOW (bord), plage);

    /* Doigt et pointeur : un glisser a la souris depuis le bord marche
     * aussi, et c'est ce qui permet de l'eprouver au banc, ou il n'y a pas
     * d'ecran tactile. */
    GtkGesture *g = gtk_gesture_drag_new ();
    g_signal_connect (g, "drag-begin",  G_CALLBACK (on_bord_debut),  NULL);
    g_signal_connect (g, "drag-update", G_CALLBACK (on_bord_glisse), NULL);
    gtk_widget_add_controller (bord, GTK_EVENT_CONTROLLER (g));

    /* Toujours affichee. Dock visible, elle est dessous et ne gene rien :
     * la pilule se tient a 12 px du bord, au-dessus de ses 10. L'afficher
     * et la retirer a chaque mouvement du dock ne gagnerait rien. */
    gtk_window_present (GTK_WINDOW (bord));
    return bord;
}

/* La nappe.
 *
 * Rappele par-dessus une application, le dock doit partir au premier clic
 * a cote. Or un clic sur une autre fenetre ne dit rien au dock : si c'est
 * la fenetre qui etait deja active, le compositeur n'a meme rien a signaler
 * -- labwc ne desactive pas une fenetre quand le clavier passe a une surface
 * layer-shell (focus_change_notify dans seat.c de labwc 0.8.3 : « Prevent
 * focus switch to non-view surface ... from updating view state »). Cliquer
 * la Console puis revenir a l'application ne produit donc aucun evenement.
 *
 * La fenetre du dock se tend donc a tout l'ecran, transparente, et recoit
 * le clic. C'est le geste classique du panneau qu'on ferme en cliquant a
 * cote -- et comme lui, ce clic-la ne va pas plus loin : il renvoie le dock,
 * il n'atteint pas l'application. Meme mecanique que la nappe du centre de
 * notifications (notifications.c).
 *
 * Tendue a la demande seulement : une surface plein ecran, meme vide, est
 * composee a chaque image de ce qui bouge dessous. */
static void
nappe_tendre (gboolean tendre)
{
    if (D.nappe == tendre)
        return;
    D.nappe = tendre;

    gtk_layer_set_anchor (GTK_WINDOW (D.fenetre), GTK_LAYER_SHELL_EDGE_LEFT,  tendre);
    gtk_layer_set_anchor (GTK_WINDOW (D.fenetre), GTK_LAYER_SHELL_EDGE_RIGHT, tendre);
    gtk_layer_set_anchor (GTK_WINDOW (D.fenetre), GTK_LAYER_SHELL_EDGE_TOP,   tendre);
    /* Tendue, elle ne reserve rien : elle couvre tout. Rendue a sa pilule,
     * elle reprend le reglage de shell.conf. */
    gtk_layer_set_exclusive_zone (GTK_WINDOW (D.fenetre),
                                  (!tendre && D.cfg->reserve_space) ? 86 : 0);
}

/* LA NAPPE NE RECLAME PAS LA BANDE DU BAS -- sauf la pilule du dock.
 *
 * La barre d'etat vit dans cette bande, et c'est un autre processus : selon
 * qui des deux atteint le compositeur le premier a l'ouverture de session,
 * elle est empilee au-dessus du dock ou en dessous. Dessous, une nappe
 * pleine la recouvrirait, et la toucher renverrait tout au lieu d'ouvrir la
 * Console. En rendant la bande a ce qui est dessous, la barre reste
 * atteignable quel que soit l'ordre.
 *
 * Contrepartie : un clic sur l'application dans ces quelque 86 pixels du
 * bas ne renvoie pas le dock, il atteint l'application. C'est un moindre
 * mal qu'une barre qu'on ne peut plus toucher.
 *
 * Recalculee a chaque allocation : la pilule bouge pendant qu'elle monte,
 * et change de largeur quand une application s'ouvre. */
static void
nappe_zone (int largeur, int hauteur, gpointer data)
{
    (void) data;

    GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (D.fenetre));
    if (surface == NULL)
        return;

    cairo_rectangle_int_t tout = { 0, 0, largeur, hauteur };
    cairo_region_t *zone;

    if (!D.nappe) {
        zone = cairo_region_create_rectangle (&tout);
    } else {
        int bande = 0;
        gtk_widget_measure (D.box, GTK_ORIENTATION_VERTICAL, largeur,
                            NULL, &bande, NULL, NULL);
        cairo_rectangle_int_t haut = { 0, 0, largeur, MAX (0, hauteur - bande) };
        zone = cairo_region_create_rectangle (&haut);

        graphene_rect_t r;
        if (gtk_widget_compute_bounds (D.box, D.fenetre, &r)) {
            cairo_rectangle_int_t pilule = {
                (int) floorf (r.origin.x), (int) floorf (r.origin.y),
                (int) ceilf (r.size.width), (int) ceilf (r.size.height),
            };
            cairo_region_union_rectangle (zone, &pilule);
        }
    }
    gdk_surface_set_input_region (surface, zone);
    cairo_region_destroy (zone);
}

/* Un appui recu par la fenetre elle-meme tombe a cote de tout ce qui se
 * clique : les icones revendiquent leurs appuis avant qu'ils ne remontent
 * jusqu'a elle. Reste le fond de la pilule, entre deux icones -- ce n'est
 * pas « a cote », on l'ecarte. */
static void
on_nappe_appui (GtkGestureClick *g, int n, double x, double y, gpointer data)
{
    (void) g; (void) n; (void) data;

    if (shell_visibility_etat () != SHELL_VIS_CONVOQUE)
        return;

    graphene_rect_t r;
    if (gtk_widget_compute_bounds (D.box, D.fenetre, &r)
        && graphene_rect_contains_point (&r, &GRAPHENE_POINT_INIT ((float) x, (float) y)))
        return;

    shell_visibility_congedier ();
}

/* Les listes au survol et le menu du clic droit sont des surfaces a part :
 * elles resteraient en l'air pendant que le dock descend. */
static void
dock_fermer_surfaces (void)
{
    gtk_popover_popdown (GTK_POPOVER (D.menu));

    for (GtkWidget *e = gtk_widget_get_first_child (D.box); e != NULL;
         e = gtk_widget_get_next_sibling (e)) {
        GtkWidget *bouton = gtk_widget_get_first_child (e);
        Hover *h = bouton ? g_object_get_data (G_OBJECT (bouton), "hover") : NULL;
        if (h == NULL)
            continue;
        if (h->open_timer != 0) {
            g_source_remove (h->open_timer);
            h->open_timer = 0;
        }
        hover_cancel_close (h);
        if (h->popover != NULL)
            gtk_popover_popdown (GTK_POPOVER (h->popover));
    }
}

/* Le relais vers la barre d'etat.
 *
 * Par l'interface d'actions que GApplication publie deja -- celle-la meme
 * qu'emprunte « gapplication action ». Un ordre explicite, jamais une
 * bascule : la barre ne tient aucun etat qu'on devrait deviner.
 *
 * Asynchrone, AVEC un rappel qui lit l'erreur. Un appel sans rappel perd
 * ses erreurs sans un mot ; c'est ce qui a coute une seance au module de
 * veille le 9 septembre 2026. Si la barre ne tourne pas, le journal le dit
 * a chaque mouvement du dock -- c'est la verite, et elle est rare. */
static void
on_barre_repond (GObject *src, GAsyncResult *res, gpointer data)
{
    g_autofree char *action = data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) r =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
    if (r == NULL)
        g_message ("barre d'etat : « %s » non transmis : %s", action, err->message);
}

static void
barre_suivre (gboolean visible, gboolean redire)
{
    if (!redire && D.barre_vue == (visible ? 1 : 0))
        return;
    D.barre_vue = visible ? 1 : 0;

    GDBusConnection *bus = g_application_get_dbus_connection (G_APPLICATION (D.app));
    if (bus == NULL) {
        g_message ("barre d'etat : pas de bus de session, elle ne suivra pas le dock");
        return;
    }

    const char *action = visible ? "afficher" : "masquer";
    /* NULL pour un tableau : GVariant en fait un tableau vide. */
    g_dbus_connection_call (bus, "os.claude.shell.status", "/os/claude/shell/status",
                            "org.gtk.Actions", "Activate",
                            g_variant_new ("(sava{sv})", action, NULL, NULL),
                            NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL,
                            on_barre_repond, g_strdup (action));
}

static const char *
nom_etat (ShellVisEtat etat)
{
    switch (etat) {
    case SHELL_VIS_CACHE:    return "cache";
    case SHELL_VIS_BUREAU:   return "bureau";
    case SHELL_VIS_CONVOQUE: return "convoque";
    }
    return "?";
}

static void
on_etat (ShellVisEtat etat, gpointer data)
{
    (void) data;
    /* En debogage seulement (G_MESSAGES_DEBUG=all) : un message par Alt-Tab
     * remplirait shell.log en une journee. */
    g_debug ("visibilite : %s", nom_etat (etat));

    switch (etat) {
    case SHELL_VIS_CACHE:
        dock_fermer_surfaces ();
        /* Repliee tout de suite, pas a la fin de la descente : la pilule
         * est au meme endroit dans les deux formes, rien ne saute, et un
         * clic donne pendant la descente atteint deja l'application. */
        nappe_tendre (FALSE);
        shell_glissiere_cacher (D.glissiere);
        break;
    case SHELL_VIS_BUREAU:
        /* Replier la nappe REDIMENSIONNE la fenetre, affichee : une liste au
         * survol ou le menu du clic droit ouverts partiraient hors de
         * l'ecran -- labwc recalcule leur place depuis l'ancienne origine de
         * la surface (vu au banc sur le centre de notifications, voir
         * notifications.c, « La nappe »). On les ferme d'abord. */
        if (D.nappe)
            dock_fermer_surfaces ();
        nappe_tendre (FALSE);
        shell_glissiere_montrer (D.glissiere);
        break;
    case SHELL_VIS_CONVOQUE:
        nappe_tendre (TRUE);
        shell_glissiere_montrer (D.glissiere);
        break;
    }
    barre_suivre (etat != SHELL_VIS_CACHE, FALSE);
}

/* Les actions publiees sur le bus de session, sous os.claude.shell.dock.
 *
 *   basculer   la touche Loupe (claude-os-shell-basculer)
 *   afficher   rappelle, comme le doigt
 *   masquer    renvoie, quel que soit l'etat
 *   annoncer   redit son etat a la barre -- elle le demande en demarrant,
 *              pour le cas ou elle aurait ete relancee seule */
static void
on_action_basculer (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    shell_visibility_basculer ();
}

static void
on_action_afficher (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    shell_visibility_convoquer ();
}

static void
on_action_masquer (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    shell_visibility_cacher ();
}

static void
on_action_annoncer (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    barre_suivre (shell_visibility_etat () != SHELL_VIS_CACHE, TRUE);
}

static const GActionEntry actions[] = {
    { "basculer", on_action_basculer, NULL, NULL, NULL, { 0 } },
    { "afficher", on_action_afficher, NULL, NULL, NULL, { 0 } },
    { "masquer",  on_action_masquer,  NULL, NULL, NULL, { 0 } },
    { "annoncer", on_action_annoncer, NULL, NULL, NULL, { 0 } },
};

/* -------------------------------------------------------------------------
 * Fenetre du dock
 * ------------------------------------------------------------------------- */
static void
on_activate (GtkApplication *app, gpointer user_data)
{
    ShellConfig *cfg = user_data;

    shell_config_apply (cfg);

    D.app       = app;
    D.cfg       = cfg;
    D.barre_vue = -1;

    /* La bande du bord AVANT le dock : meme couche, et labwc empile dans
     * l'ordre de creation. Creee apres, elle passerait devant la pilule. */
    bord_creer (app);

    GtkWidget *window = gtk_application_window_new (app);
    gtk_widget_add_css_class (window, "shell");
    D.fenetre = window;

    /* --- Ancrage layer-shell ---------------------------------------------
     * Sans cela, le dock serait une fenetre ordinaire : elle passerait
     * derriere les autres et apparaitrait dans la liste des fenetres.
     * OVERLAY et non TOP : voir « A l'ecran ou non ». */
    gtk_layer_init_for_window (GTK_WINDOW (window));
    gtk_layer_set_layer (GTK_WINDOW (window), GTK_LAYER_SHELL_LAYER_OVERLAY);
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
     * qui prefere que rien ne passe sous le dock. Depuis que le dock sort de
     * l'ecran des qu'on travaille, c'est un choix peu utile : la place ne
     * serait reservee que pendant qu'il est rappele. */
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

    D.box = dock;

    /* Un seul menu, parente au conteneur et repositionne a chaque clic
     * droit : un menu par icone serait recree a chaque reconstruction du
     * dock, c'est-a-dire a chaque ouverture de fenetre. */
    D.menu = gtk_popover_menu_new_from_model (NULL);
    gtk_widget_add_css_class (D.menu, "dock-menu");
    gtk_popover_set_has_arrow (GTK_POPOVER (D.menu), FALSE);
    gtk_popover_set_position (GTK_POPOVER (D.menu), GTK_POS_TOP);
    gtk_widget_set_parent (D.menu, dock);

    /* Deposer sur le FOND du dock ajoute en fin de liste. Sans cette cible,
     * lacher une application ailleurs que pile sur une icone ne ferait
     * rien, sans dire pourquoi. */
    GtkDropTarget *fond = gtk_drop_target_new (G_TYPE_STRING,
                                               GDK_ACTION_MOVE | GDK_ACTION_COPY);
    g_signal_connect (fond, "drop", G_CALLBACK (on_drop), NULL);
    gtk_widget_add_controller (dock, GTK_EVENT_CONTROLLER (fond));

    dock_rebuild ();

    /* La glissiere entre la fenetre et la pilule : c'est elle qui la fait
     * descendre hors de l'ecran, et qui retire la fenetre une fois en bas. */
    GtkWidget *glissiere = shell_glissiere_new (dock);
    D.glissiere = SHELL_GLISSIERE (glissiere);
    shell_glissiere_sur_allocation (D.glissiere, nappe_zone, NULL);
    gtk_window_set_child (GTK_WINDOW (window), glissiere);

    /* Le clic « a cote », quand la nappe est tendue. */
    GtkGesture *nappe = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (nappe), 0);   /* tout bouton */
    g_signal_connect (nappe, "pressed", G_CALLBACK (on_nappe_appui), NULL);
    gtk_widget_add_controller (window, GTK_EVENT_CONTROLLER (nappe));

    /* Affiche au demarrage, dans l'etat BUREAU : a l'ouverture de session il
     * n'y a encore aucune fenetre. Et affiche de toute facon, meme si le dock
     * est relance au milieu d'une session : c'est la premiere presentation
     * qui fixe sa place dans la pile, et elle doit avoir lieu maintenant --
     * apres la bande, et si possible avant la barre. S'il y a deja une
     * application active, le premier lot du compositeur le renverra. */
    gtk_window_present (GTK_WINDOW (window));

    g_action_map_add_action_entries (G_ACTION_MAP (app), actions,
                                     G_N_ELEMENTS (actions), NULL);

    /* Les actions du menu contextuel vivent sur la fenetre, sous le prefixe
     * « dock » : elles portent une cible (l'identifiant de l'application) et
     * n'ont rien a faire sur le bus de session, contrairement a la bascule
     * de visibilite. */
    GSimpleActionGroup *groupe = g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (groupe), actions_dock,
                                     G_N_ELEMENTS (actions_dock), NULL);
    gtk_widget_insert_action_group (window, "dock", G_ACTION_GROUP (groupe));
    g_object_unref (groupe);

    /* Le dock doit survivre a sa fenetre retiree. La bande du bord suffit
     * aujourd'hui a garder GApplication en vie, mais le jour ou elle ne
     * serait plus la, le dock disparaitrait pour de bon au lieu de se
     * cacher -- sans rien dire. */
    g_application_hold (G_APPLICATION (app));

    shell_visibility_init (on_etat, NULL);
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
