/* =========================================================================
 * Claude OS — centre de notifications : serveur freedesktop + affichage.
 * Voir notifications.h pour le pourquoi des choix d'architecture.
 * ========================================================================= */

#include "notifications.h"
#include "panel.h"

#include <gtk4-layer-shell.h>

#include <string.h>

/* Combien de temps la banniere reste a l'ecran quand l'emetteur ne dit rien.
 * Cinq secondes : le temps de lire deux lignes sans avoir a se depecher, et
 * pas au point de rester en travers du bureau. */
#define BANNIERE_DEFAUT_MS 5000

/* Plafond, meme si l'emetteur demande davantage. Une banniere n'est pas une
 * fenetre : ce qui doit durer va dans le centre, qui garde tout. */
#define BANNIERE_MAX_MS 20000

/* Ecart entre le centre de notifications et la Console quand les deux sont
 * ouverts. Le meme que celui qui separe la Console de la barre d'etat, pour
 * que l'empilement garde un rythme unique. */
#define ECART_ENTRE_PX PANEL_ECART_BARRE_PX

/* Au-dela, les plus anciennes sortent. Un centre qui garde tout finit par
 * couter plus de memoire que le reste du shell, et personne ne fait defiler
 * deux cents notifications. */
#define GARDE_MAX 50

/* Raisons de fermeture, telles que la specification les numerote. */
#define RAISON_EXPIREE   1
#define RAISON_REJETEE   2
#define RAISON_DEMANDEE  3

static const char INTROSPECTION[] =
  "<node>"
  "  <interface name='org.freedesktop.Notifications'>"
  "    <method name='Notify'>"
  "      <arg type='s' name='app_name'      direction='in'/>"
  "      <arg type='u' name='replaces_id'   direction='in'/>"
  "      <arg type='s' name='app_icon'      direction='in'/>"
  "      <arg type='s' name='summary'       direction='in'/>"
  "      <arg type='s' name='body'          direction='in'/>"
  "      <arg type='as' name='actions'      direction='in'/>"
  "      <arg type='a{sv}' name='hints'     direction='in'/>"
  "      <arg type='i' name='expire_timeout' direction='in'/>"
  "      <arg type='u' name='id'            direction='out'/>"
  "    </method>"
  "    <method name='CloseNotification'>"
  "      <arg type='u' name='id' direction='in'/>"
  "    </method>"
  "    <method name='GetCapabilities'>"
  "      <arg type='as' name='capabilities' direction='out'/>"
  "    </method>"
  "    <method name='GetServerInformation'>"
  "      <arg type='s' name='name'         direction='out'/>"
  "      <arg type='s' name='vendor'       direction='out'/>"
  "      <arg type='s' name='version'      direction='out'/>"
  "      <arg type='s' name='spec_version' direction='out'/>"
  "    </method>"
  "    <signal name='NotificationClosed'>"
  "      <arg type='u' name='id'/>"
  "      <arg type='u' name='reason'/>"
  "    </signal>"
  "    <signal name='ActionInvoked'>"
  "      <arg type='u' name='id'/>"
  "      <arg type='s' name='action_key'/>"
  "    </signal>"
  "  </interface>"
  "</node>";

/* ------------------------------------------------------------------------- */

typedef struct {
    guint32      id;
    char        *app;
    char        *resume;
    char        *corps;
    GdkPaintable *image;      /* NULL si l'emetteur n'en fournit aucune      */
    char        *icone_nom;   /* repli : un nom d'icone du theme             */
    char       **actions;     /* paires cle/libelle, terminees par NULL      */
    guint8       urgence;     /* 0 basse, 1 normale, 2 critique              */
    GDateTime   *date;
    gboolean     lue;
} Notif;

struct _Notifs {
    GPtrArray   *liste;        /* Notif *, la plus recente en tete           */
    guint32      prochain_id;
    gboolean     apercu;

    GDBusConnection *bus;
    guint            nom_id;
    guint            objet_id;

    GtkWidget   *cloche;
    GtkWidget   *centre;       /* GtkPopover : la liste complete             */
    GtkWidget   *centre_liste;
    GtkWidget   *centre_vide;
    GtkWidget   *banniere;     /* GtkPopover : l'arrivante                   */
    guint        banniere_timer;
    gboolean     banniere_attente;  /* prete, la barre n'est pas encore la   */
    int          banniere_duree;    /* ms ; 0 = jusqu'a ce qu'on l'ouvre      */

    NotifsBarreFunc barre;          /* la barre d'etat, qui peut etre partie */
    gpointer        barre_data;

    GtkWidget   *fenetre;      /* la barre d'etat : la nappe la laisse dehors */
    GtkWidget   *nappe;        /* recoit le clic a cote, centre ouvert       */
    int          hauteur_console;   /* 0 quand elle est fermee               */
    int          largeur_console;   /* imposee aux deux surfaces             */
    GtkWidget   *centre_boite;      /* contenu du centre, pour sa largeur    */
};

static void centre_reconstruire (Notifs *n);
static void positionner (Notifs *n);

/* Dit a la barre que ce qu'on lui demande a change. Rend ce qu'elle rend :
 * TRUE si elle est a sa place. Sans barre inscrite -- le banc d'essai --,
 * elle est reputee y etre. */
static gboolean
prevenir_barre (Notifs *n)
{
    return n->barre != NULL ? n->barre (n->barre_data) : TRUE;
}

/* -------------------------------------------------------------------------
 * La nappe
 *
 * Ce qui recoit le clic « a cote » tant que le centre est ouvert. Le centre
 * n'a pas de saisie a lui (voir notifs_ancrer) : sans nappe, un clic sur une
 * application le laisserait ouvert.
 *
 * UNE SURFACE A PART, ET SURTOUT PAS LA BARRE ELARGIE.
 *
 * La premiere version etendait la fenetre de la barre a tout l'ecran. Elle
 * n'a jamais servi -- l'appel qui la deployait manquait -- et, le jour ou il
 * a ete ajoute (11 septembre 2026), le centre s'est mis a sauter a gauche de
 * l'ecran des son ouverture. Mesure au banc dans la trace Wayland : la
 * surface de la barre passe de 216x42 a 1920x1080, GTK redemande la position
 * du popover (anchor_rect 1740,1038), et labwc 0.8.3 la calcule depuis
 * l'ancienne origine de la surface -- configure(-151, ...). Le centre, et la
 * Console avec lui si elle est ouverte, partent hors de l'ecran.
 *
 * REGLE : ON NE REDIMENSIONNE JAMAIS UNE SURFACE QUI PORTE UN POPOVER OUVERT.
 *
 * La nappe est donc sa propre fenetre, plein ecran, qui laisse la barre
 * dehors : sa zone d'entree exclut le coin qu'occupe la fenetre de la barre.
 * La pilule et la cloche restent cliquables a travers elle -- ouvrir la
 * Console pendant que le centre est ouvert marche toujours. Le centre et la
 * Console sont des popovers, que labwc empile au-dessus de toutes les
 * couches : la nappe ne les couvre pas.
 *
 * PRESQUE TRANSPARENTE : une fenetre GTK entierement transparente et vide ne
 * recoit aucun appui. Meme constat, meme remede que la bande du bord du
 * dock (dock.c, bord_creer).
 * ------------------------------------------------------------------------- */
static void
nappe_zone (GtkDrawingArea *zone, int largeur, int hauteur, gpointer data)
{
    Notifs *n = data;
    (void) zone;

    GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (n->nappe));
    if (surface == NULL)
        return;

    cairo_rectangle_int_t tout = { 0, 0, largeur, hauteur };
    cairo_region_t *entree = cairo_region_create_rectangle (&tout);

    /* La barre est ancree en bas a droite : son coin se deduit de sa taille. */
    if (n->fenetre != NULL) {
        int lb = gtk_widget_get_width (n->fenetre);
        int hb = gtk_widget_get_height (n->fenetre);
        cairo_rectangle_int_t barre = { largeur - lb, hauteur - hb, lb, hb };
        cairo_region_subtract_rectangle (entree, &barre);
    }
    gdk_surface_set_input_region (surface, entree);
    cairo_region_destroy (entree);
}

static void centre_fermer (Notifs *n);

/* A LA FIN DU GESTE, PAS A L'APPUI.
 *
 * Fermer le centre retire la nappe. Retiree pendant que le bouton est
 * encore enfonce, elle ne recevait jamais le relachement, et GTK s'en
 * plaignait a chaque fois dans le journal : « Broken accounting of active
 * state » -- releve au banc, geste par geste. « end » vient apres le
 * relachement, et aussi quand le doigt a glisse au lieu de toucher : un
 * glisser a cote ferme le centre comme un clic. */
static void
on_nappe_fin (GtkGesture *g, GdkEventSequence *seq, gpointer data)
{
    Notifs *n = data;
    (void) g; (void) seq;
    if (gtk_widget_get_visible (n->centre))
        centre_fermer (n);
}

static GtkWidget *
nappe_creer (Notifs *n)
{
    static GtkCssProvider *presque = NULL;
    if (presque == NULL) {
        presque = gtk_css_provider_new ();
        gtk_css_provider_load_from_string (presque,
            "window.claude-os-nappe { background-color: rgba(0, 0, 0, 0.01); }");
        gtk_style_context_add_provider_for_display (gdk_display_get_default (),
            GTK_STYLE_PROVIDER (presque), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    GtkWidget *nappe = gtk_window_new ();
    gtk_widget_add_css_class (nappe, "claude-os-nappe");
    gtk_layer_init_for_window (GTK_WINDOW (nappe));
    /* OVERLAY, comme la barre : au-dessus des fenetres plein ecran. */
    gtk_layer_set_layer (GTK_WINDOW (nappe), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace (GTK_WINDOW (nappe), "claude-os-nappe-centre");
    for (int bord = 0; bord < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; bord++)
        gtk_layer_set_anchor (GTK_WINDOW (nappe), bord, TRUE);
    gtk_layer_set_exclusive_zone (GTK_WINDOW (nappe), -1);
    gtk_layer_set_keyboard_mode (GTK_WINDOW (nappe),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    /* Une zone de dessin pour son signal « resize » : c'est la qu'on connait
     * enfin la taille de l'ecran, donc la zone d'entree a poser. */
    GtkWidget *zone = gtk_drawing_area_new ();
    g_signal_connect (zone, "resize", G_CALLBACK (nappe_zone), n);
    gtk_window_set_child (GTK_WINDOW (nappe), zone);

    GtkGesture *g = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (g), 0);   /* tous les boutons */
    g_signal_connect (g, "end", G_CALLBACK (on_nappe_fin), n);
    gtk_widget_add_controller (nappe, GTK_EVENT_CONTROLLER (g));
    return nappe;
}

/* Affichee a l'ouverture du centre, retiree a sa fermeture -- retiree et non
 * vide : une surface plein ecran est composee a chaque image de ce qui bouge
 * dessous, et celle-ci avalerait les clics destines aux applications. */
static void
nappe_deployer (Notifs *n, gboolean deployee)
{
    if (n->fenetre == NULL)
        return;                 /* banc d'essai sans barre */
    if (deployee && n->nappe == NULL)
        n->nappe = nappe_creer (n);
    if (n->nappe != NULL)
        gtk_widget_set_visible (n->nappe, deployee);
}

static void
centre_fermer (Notifs *n)
{
    gtk_popover_popdown (GTK_POPOVER (n->centre));
    nappe_deployer (n, FALSE);
    prevenir_barre (n);
}

void
notifs_nappe (Notifs *n, GtkWidget *fenetre)
{
    n->fenetre = fenetre;
}

/* -------------------------------------------------------------------------
 * Une notification
 * ------------------------------------------------------------------------- */
static void
notif_free (gpointer data)
{
    Notif *x = data;
    g_free (x->app);
    g_free (x->resume);
    g_free (x->corps);
    g_free (x->icone_nom);
    g_strfreev (x->actions);
    g_clear_object (&x->image);
    g_clear_pointer (&x->date, g_date_time_unref);
    g_free (x);
}

static Notif *
notif_par_id (Notifs *n, guint32 id)
{
    for (guint i = 0; i < n->liste->len; i++) {
        Notif *x = g_ptr_array_index (n->liste, i);
        if (x->id == id)
            return x;
    }
    return NULL;
}

static guint
non_lues (Notifs *n)
{
    guint c = 0;
    for (guint i = 0; i < n->liste->len; i++)
        if (!((Notif *) g_ptr_array_index (n->liste, i))->lue)
            c++;
    return c;
}

/* La cloche s'allume des qu'il reste quelque chose a lire, et s'eteint
 * autrement. C'est le seul etat qu'elle porte : un compteur chiffre dans une
 * barre deja dense se lit mal, et l'information « il y a du neuf » suffit a
 * decider si l'on clique. */
static void
cloche_rafraichir (Notifs *n)
{
    if (n->cloche == NULL)
        return;
    if (non_lues (n) > 0)
        gtk_widget_add_css_class (n->cloche, "nouvelles");
    else
        gtk_widget_remove_css_class (n->cloche, "nouvelles");
}

/* -------------------------------------------------------------------------
 * Images fournies par l'emetteur
 * ------------------------------------------------------------------------- */
/* « image-data » transporte les pixels bruts : (largeur, hauteur, pas,
 * alpha, bits par composante, composantes, octets). Chromium s'en sert pour
 * la favicon du site emetteur, et sans lui toutes ses notifications se
 * ressemblent.
 *
 * On refuse tout ce qui n'est pas 8 bits par composante : le reste est
 * legal dans la specification mais introuvable en pratique, et le convertir
 * a l'aveugle produirait des couleurs fausses plutot qu'une erreur. */
static GdkPaintable *
image_depuis_donnees (GVariant *v)
{
    gint32 largeur, hauteur, pas;
    gboolean alpha;
    gint32 bits, canaux;
    g_autoptr(GVariant) octets = NULL;

    if (!g_variant_check_format_string (v, "(iiibiiay)", FALSE))
        return NULL;
    g_variant_get (v, "(iiibii@ay)", &largeur, &hauteur, &pas, &alpha,
                   &bits, &canaux, &octets);

    if (largeur <= 0 || hauteur <= 0 || bits != 8
        || (canaux != 3 && canaux != 4)
        || pas < largeur * canaux)
        return NULL;

    gsize taille = 0;
    const guchar *donnees = g_variant_get_fixed_array (octets, &taille, 1);
    if (donnees == NULL || taille < (gsize) pas * (gsize) (hauteur - 1) + (gsize) largeur * canaux)
        return NULL;

    g_autoptr(GBytes) b = g_bytes_new (donnees, taille);
    GdkTexture *t = gdk_memory_texture_new (
        largeur, hauteur,
        canaux == 4 ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
        b, pas);
    return GDK_PAINTABLE (t);
}

/* -------------------------------------------------------------------------
 * Une carte dans le centre
 * ------------------------------------------------------------------------- */
typedef struct {
    Notifs *n;
    guint32 id;
    char   *action;          /* NULL pour le simple rejet                    */
} Geste;

static void
geste_free (gpointer data, GClosure *c)
{
    Geste *g = data;
    (void) c;
    g_free (g->action);
    g_free (g);
}

static void
fermer (Notifs *n, guint32 id, guint raison)
{
    Notif *x = notif_par_id (n, id);
    if (x == NULL)
        return;

    if (n->bus != NULL)
        g_dbus_connection_emit_signal (n->bus, NULL,
            "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
            "NotificationClosed", g_variant_new ("(uu)", id, raison), NULL);

    g_ptr_array_remove (n->liste, x);   /* free par la fonction de liberation */
    cloche_rafraichir (n);
    centre_reconstruire (n);
}

static void
on_rejeter (GtkButton *b, gpointer data)
{
    Geste *g = data;
    (void) b;
    fermer (g->n, g->id, RAISON_REJETEE);
}

/* Un clic sur le corps de la carte declenche l'action « default » quand
 * l'emetteur en propose une — c'est ce qui ouvre l'onglet Chromium ou la
 * conversation Claude Desktop d'ou vient la notification. Sans action par
 * defaut, le clic ne fait que ranger la carte. */
static void
on_agir (GtkButton *b, gpointer data)
{
    Geste *g = data;
    (void) b;

    if (g->action != NULL && g->n->bus != NULL)
        g_dbus_connection_emit_signal (g->n->bus, NULL,
            "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
            "ActionInvoked", g_variant_new ("(us)", g->id, g->action), NULL);

    fermer (g->n, g->id, RAISON_REJETEE);
}

static Geste *
geste_new (Notifs *n, guint32 id, const char *action)
{
    Geste *g = g_new0 (Geste, 1);
    g->n = n; g->id = id;
    g->action = g_strdup (action);
    return g;
}

/* « il y a 3 min » plutot qu'une heure absolue : dans un centre qu'on ouvre
 * pour savoir ce qu'on a manque, l'anciennete se lit mieux que l'horaire. */
static char *
depuis (GDateTime *quand)
{
    g_autoptr(GDateTime) maintenant = g_date_time_new_now_local ();
    GTimeSpan d = g_date_time_difference (maintenant, quand) / G_TIME_SPAN_SECOND;

    if (d < 60)      return g_strdup ("à l’instant");
    if (d < 3600)    return g_strdup_printf ("il y a %d min", (int) (d / 60));
    if (d < 86400)   return g_strdup_printf ("il y a %d h", (int) (d / 3600));
    return g_date_time_format (quand, "%d/%m à %H:%M");
}

/* `compacte` : la banniere ne montre qu'une notification et n'a pas de
 * bouton de rejet — elle s'efface seule. Le centre, lui, se range a la
 * main. Une seule fonction pour les deux, sinon les deux dessins divergent
 * a la premiere retouche. */
static GtkWidget *
carte (Notifs *n, Notif *x, gboolean compacte)
{
    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class (boite, "notif-carte");
    if (x->urgence >= 2)
        gtk_widget_add_css_class (boite, "critique");

    /* --- en-tete : image, application, anciennete, rejet --- */
    GtkWidget *vignette;
    if (x->image != NULL) {
        vignette = gtk_image_new_from_paintable (x->image);
    } else {
        vignette = gtk_image_new_from_icon_name (
            x->icone_nom ? x->icone_nom : "dialog-information-symbolic");
    }
    gtk_image_set_pixel_size (GTK_IMAGE (vignette), 18);
    gtk_widget_add_css_class (vignette, "notif-icone");
    gtk_widget_set_valign (vignette, GTK_ALIGN_START);

    GtkWidget *app = gtk_label_new (x->app && *x->app ? x->app : "Notification");
    gtk_widget_add_css_class (app, "notif-app");
    gtk_label_set_xalign (GTK_LABEL (app), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (app), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (app), 16);
    gtk_widget_set_hexpand (app, TRUE);

    g_autofree char *age = depuis (x->date);
    GtkWidget *quand = gtk_label_new (age);
    gtk_widget_add_css_class (quand, "notif-quand");

    GtkWidget *entete = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append (GTK_BOX (entete), vignette);
    gtk_box_append (GTK_BOX (entete), app);
    gtk_box_append (GTK_BOX (entete), quand);

    if (!compacte) {
        GtkWidget *rejet = gtk_button_new_from_icon_name ("window-close-symbolic");
        gtk_widget_add_css_class (rejet, "notif-rejet");
        gtk_widget_set_valign (rejet, GTK_ALIGN_START);
        gtk_widget_set_tooltip_text (rejet, "Effacer");
        g_signal_connect_data (rejet, "clicked", G_CALLBACK (on_rejeter),
                               geste_new (n, x->id, NULL), geste_free, 0);
        gtk_box_append (GTK_BOX (entete), rejet);
    }
    gtk_box_append (GTK_BOX (boite), entete);

    /* --- le texte ---
     *
     * En texte brut, et jamais en balisage. « body-markup » n'est pas
     * annonce dans les capacites, donc aucun emetteur correct n'envoie de
     * balises ; les passer a Pango malgre tout ferait disparaitre une
     * notification entiere sur une esperluette mal echappee. */
    if (x->resume && *x->resume) {
        GtkWidget *l = gtk_label_new (x->resume);
        gtk_widget_add_css_class (l, "notif-resume");
        gtk_label_set_xalign (GTK_LABEL (l), 0.0);
        gtk_label_set_wrap (GTK_LABEL (l), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (l), 30);
        gtk_label_set_lines (GTK_LABEL (l), 2);
        gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
        gtk_box_append (GTK_BOX (boite), l);
    }
    if (x->corps && *x->corps) {
        GtkWidget *l = gtk_label_new (x->corps);
        gtk_widget_add_css_class (l, "notif-corps");
        gtk_label_set_xalign (GTK_LABEL (l), 0.0);
        gtk_label_set_wrap (GTK_LABEL (l), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (l), 30);
        gtk_label_set_lines (GTK_LABEL (l), compacte ? 2 : 4);
        gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
        gtk_box_append (GTK_BOX (boite), l);
    }

    /* --- les actions proposees par l'emetteur --- */
    const char *defaut = NULL;
    if (x->actions != NULL) {
        GtkWidget *rang = NULL;
        for (guint i = 0; x->actions[i] != NULL && x->actions[i + 1] != NULL; i += 2) {
            const char *cle = x->actions[i], *libelle = x->actions[i + 1];
            if (g_strcmp0 (cle, "default") == 0) {
                defaut = cle;      /* pas de bouton : c'est le clic du corps */
                continue;
            }
            if (compacte)
                continue;          /* la banniere ne propose rien : elle passe */
            if (rang == NULL) {
                rang = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
                gtk_widget_add_css_class (rang, "notif-actions");
            }
            GtkWidget *b = gtk_button_new_with_label (libelle);
            gtk_widget_add_css_class (b, "notif-action");
            gtk_widget_set_hexpand (b, TRUE);
            g_signal_connect_data (b, "clicked", G_CALLBACK (on_agir),
                                   geste_new (n, x->id, cle), geste_free, 0);
            gtk_box_append (GTK_BOX (rang), b);
        }
        if (rang != NULL)
            gtk_box_append (GTK_BOX (boite), rang);
    }

    /* Le corps entier devient cliquable s'il y a une action par defaut. Sinon
     * on renvoie la boite telle quelle : un bouton qui ne fait rien invite a
     * un clic qui ne repond pas. */
    if (defaut != NULL && !compacte) {
        GtkWidget *b = gtk_button_new ();
        gtk_button_set_child (GTK_BUTTON (b), boite);
        gtk_widget_add_css_class (b, "notif-cliquable");
        g_signal_connect_data (b, "clicked", G_CALLBACK (on_agir),
                               geste_new (n, x->id, defaut), geste_free, 0);
        return b;
    }
    return boite;
}

/* -------------------------------------------------------------------------
 * Le centre
 * ------------------------------------------------------------------------- */
static void
vider_boite (GtkWidget *boite)
{
    GtkWidget *e;
    while ((e = gtk_widget_get_first_child (boite)) != NULL)
        gtk_box_remove (GTK_BOX (boite), e);
}

static void
centre_reconstruire (Notifs *n)
{
    if (n->centre_liste == NULL)
        return;

    vider_boite (n->centre_liste);
    for (guint i = 0; i < n->liste->len; i++)
        gtk_box_append (GTK_BOX (n->centre_liste),
                        carte (n, g_ptr_array_index (n->liste, i), FALSE));

    gboolean vide = (n->liste->len == 0);
    gtk_widget_set_visible (n->centre_vide, vide);
    gtk_widget_set_visible (n->centre_liste, !vide);

    /* La hauteur du centre vient de changer : s'il est ouvert au-dessus de
     * la Console, sa position doit etre reprise. */
    if (gtk_widget_get_visible (n->centre))
        positionner (n);
}

static void
on_tout_effacer (GtkButton *b, gpointer data)
{
    Notifs *n = data;
    (void) b;

    /* On previent les emetteurs un par un avant de vider : une application
     * qui suit ses propres notifications doit apprendre leur disparition,
     * sans quoi elle croira les siennes encore affichees. */
    for (guint i = 0; i < n->liste->len; i++) {
        Notif *x = g_ptr_array_index (n->liste, i);
        if (n->bus != NULL)
            g_dbus_connection_emit_signal (n->bus, NULL,
                "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                "NotificationClosed", g_variant_new ("(uu)", x->id, RAISON_REJETEE),
                NULL);
    }
    g_ptr_array_set_size (n->liste, 0);
    cloche_rafraichir (n);
    centre_reconstruire (n);
}

/* -------------------------------------------------------------------------
 * Position : au-dessus de la Console, jamais dessus
 * ------------------------------------------------------------------------- */
/* Le decalage vertical est le SEUL calcul de placement. Le bord droit et la
 * largeur viennent de l'ancre, partagee avec la Console.
 *
 * Console fermee, le centre se pose exactement ou elle se poserait. Ouverte,
 * il monte de sa hauteur plus un ecart. C'est la meme formule dans les deux
 * sens, ce qui garantit que « ouvrir la Console pendant que le centre est
 * ouvert » et « ouvrir le centre pendant que la Console est ouverte »
 * aboutissent au meme ecran — sans quoi l'un des deux ordres aurait fini par
 * produire un empilement different. */
static int
decalage (Notifs *n)
{
    int d = PANEL_ECART_BARRE_PX;
    if (n->hauteur_console > 0)
        d += n->hauteur_console + ECART_ENTRE_PX;
    return -d;
}

static void
positionner (Notifs *n)
{
    int d = decalage (n);
    if (n->centre != NULL)
        gtk_popover_set_offset (GTK_POPOVER (n->centre), 0, d);
    if (n->banniere != NULL)
        gtk_popover_set_offset (GTK_POPOVER (n->banniere), 0, d);
}

/* La largeur de la Console est imposee aux deux surfaces, plutot que
 * redecrite dans la feuille de style : c'est la seule facon d'etre sur
 * qu'elles restent egales quand le contenu de la Console change. */
static void
on_console_geometrie (int largeur, int hauteur, gpointer data)
{
    Notifs *n = data;
    n->hauteur_console = hauteur;
    if (largeur > 0) {
        if (n->centre_boite != NULL)
            gtk_widget_set_size_request (n->centre_boite, largeur, -1);
        n->largeur_console = largeur;
        if (n->banniere != NULL) {
            GtkWidget *c = gtk_popover_get_child (GTK_POPOVER (n->banniere));
            if (c != NULL)
                gtk_widget_set_size_request (c, largeur, -1);
        }
    }
    positionner (n);
}

void
notifs_suivre_console (Notifs *n, GtkWidget *console)
{
    panel_observer_geometrie (console, on_console_geometrie, n);
}

/* -------------------------------------------------------------------------
 * La banniere
 * ------------------------------------------------------------------------- */
static gboolean
banniere_masquer (gpointer data)
{
    Notifs *n = data;
    n->banniere_timer = 0;
    if (n->banniere != NULL)
        gtk_popover_popdown (GTK_POPOVER (n->banniere));
    prevenir_barre (n);        /* elle peut repartir */
    return G_SOURCE_REMOVE;
}

/* La banniere prete, et la barre a sa place : on l'affiche. La duree court
 * a partir d'ici, pas de l'arrivee -- le temps que la barre remonte ne doit
 * pas etre pris sur le temps de lecture. */
static void
banniere_afficher (Notifs *n)
{
    n->banniere_attente = FALSE;
    positionner (n);
    gtk_popover_popup (GTK_POPOVER (n->banniere));

    /* Urgence critique : la banniere reste jusqu'a ce qu'on s'en occupe.
     * C'est le seul cas ou la specification demande de ne pas expirer, et
     * c'est aussi le seul ou l'ignorer serait grave. */
    if (n->banniere_duree == 0)
        return;
    n->banniere_timer = g_timeout_add (n->banniere_duree, banniere_masquer, n);
}

static void
banniere_montrer (Notifs *n, Notif *x, int duree_ms)
{
    if (n->banniere == NULL)
        return;

    /* Rien a annoncer si le centre est deja ouvert : la notification vient
     * d'y apparaitre, sous les yeux de qui l'a ouvert. Deux surfaces disant
     * la meme chose au meme endroit se recouvriraient. */
    if (gtk_widget_get_visible (n->centre))
        return;

    if (n->banniere_timer != 0) {
        g_source_remove (n->banniere_timer);
        n->banniere_timer = 0;   /* sinon un identifiant mort survit a l'attente */
    }

    GtkWidget *ancien = gtk_popover_get_child (GTK_POPOVER (n->banniere));
    if (ancien != NULL)
        gtk_popover_set_child (GTK_POPOVER (n->banniere), NULL);
    GtkWidget *c = carte (n, x, TRUE);
    if (n->largeur_console > 0)
        gtk_widget_set_size_request (c, n->largeur_console, -1);
    gtk_popover_set_child (GTK_POPOVER (n->banniere), c);

    n->banniere_duree   = (x->urgence >= 2 && duree_ms == 0) ? 0 : duree_ms;
    n->banniere_attente = TRUE;

    /* « Encore en attente » verifie APRES le rappel : si les animations sont
     * coupees, la barre arrive a sa place pendant le rappel meme, et a deja
     * appele notifs_barre_en_place(). L'afficher une seconde fois ici
     * poserait deux minuteries pour une banniere. */
    if (prevenir_barre (n) && n->banniere_attente)
        banniere_afficher (n);
}

/* -------------------------------------------------------------------------
 * Le bus
 * ------------------------------------------------------------------------- */
static guint8
hint_octet (GVariant *hints, const char *nom, guint8 defaut)
{
    g_autoptr(GVariant) v = g_variant_lookup_value (hints, nom, G_VARIANT_TYPE_BYTE);
    return v ? g_variant_get_byte (v) : defaut;
}

static void
methode (GDBusConnection *bus, const char *emetteur, const char *chemin,
         const char *iface, const char *nom, GVariant *params,
         GDBusMethodInvocation *appel, gpointer data)
{
    Notifs *n = data;
    (void) bus; (void) emetteur; (void) chemin; (void) iface;

    if (g_strcmp0 (nom, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value (appel,
            g_variant_new ("(ssss)", "claude-os-notifications", "Claude OS",
                           "1.0", "1.2"));
        return;
    }

    if (g_strcmp0 (nom, "GetCapabilities") == 0) {
        /* ON N'ANNONCE QUE CE QU'ON SAIT FAIRE.
         *
         * « body-markup » est volontairement absent : les libelles sont
         * rendus en texte brut, et l'annoncer inviterait les emetteurs a
         * poser des balises que l'on afficherait telles quelles.
         * « persistence » dit que rien ne se perd quand la banniere
         * s'efface — c'est le role du centre. */
        const char *caps[] = { "body", "actions", "icon-static",
                               "persistence", NULL };
        g_dbus_method_invocation_return_value (appel,
            g_variant_new ("(^as)", caps));
        return;
    }

    if (g_strcmp0 (nom, "CloseNotification") == 0) {
        guint32 id;
        g_variant_get (params, "(u)", &id);
        fermer (n, id, RAISON_DEMANDEE);
        g_dbus_method_invocation_return_value (appel, NULL);
        return;
    }

    if (g_strcmp0 (nom, "Notify") != 0) {
        g_dbus_method_invocation_return_error (appel, G_DBUS_ERROR,
            G_DBUS_ERROR_UNKNOWN_METHOD, "methode inconnue : %s", nom);
        return;
    }

    const char *app, *icone, *resume, *corps;
    guint32 remplace;
    g_autoptr(GVariant) actions = NULL;
    g_autoptr(GVariant) hints = NULL;
    gint32 expiration;

    g_variant_get (params, "(&su&s&s&s@as@a{sv}i)", &app, &remplace, &icone,
                   &resume, &corps, &actions, &hints, &expiration);

    /* « replaces_id » remplace en place : c'est ce qui permet a une barre de
     * progression ou a un lecteur de musique de se mettre a jour sans
     * empiler dix cartes. On garde la position dans la liste. */
    Notif *x = remplace != 0 ? notif_par_id (n, remplace) : NULL;
    gboolean neuve = (x == NULL);

    if (neuve) {
        x = g_new0 (Notif, 1);
        x->id = n->prochain_id++;
        g_ptr_array_insert (n->liste, 0, x);
    } else {
        g_free (x->app); g_free (x->resume); g_free (x->corps);
        g_free (x->icone_nom); g_strfreev (x->actions);
        g_clear_object (&x->image);
        g_clear_pointer (&x->date, g_date_time_unref);
        x->actions = NULL; x->image = NULL;
    }

    x->app       = g_strdup (app);
    x->resume    = g_strdup (resume);
    x->corps     = g_strdup (corps);
    x->icone_nom = (icone && *icone) ? g_strdup (icone) : NULL;
    x->actions   = g_variant_dup_strv (actions, NULL);
    x->urgence   = hint_octet (hints, "urgency", 1);
    x->date      = g_date_time_new_now_local ();
    x->lue       = FALSE;

    g_autoptr(GVariant) img = g_variant_lookup_value (hints, "image-data", NULL);
    if (img == NULL)
        img = g_variant_lookup_value (hints, "image_data", NULL);
    if (img == NULL)
        img = g_variant_lookup_value (hints, "icon_data", NULL);
    if (img != NULL)
        x->image = image_depuis_donnees (img);

    while (n->liste->len > GARDE_MAX)
        g_ptr_array_remove_index (n->liste, n->liste->len - 1);

    cloche_rafraichir (n);
    centre_reconstruire (n);

    int duree = expiration < 0 ? BANNIERE_DEFAUT_MS
              : expiration == 0 ? (x->urgence >= 2 ? 0 : BANNIERE_DEFAUT_MS)
              : MIN (expiration, BANNIERE_MAX_MS);
    banniere_montrer (n, x, duree);

    g_dbus_method_invocation_return_value (appel, g_variant_new ("(u)", x->id));
}

static const GDBusInterfaceVTable VTABLE = { methode, NULL, NULL, { 0 } };

static void
on_bus (GObject *src, GAsyncResult *res, gpointer data)
{
    Notifs *n = data;
    g_autoptr(GError) err = NULL;
    (void) src;

    n->bus = g_bus_get_finish (res, &err);
    if (n->bus == NULL) {
        g_message ("notifications : bus de session injoignable : %s", err->message);
        return;
    }

    g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml (INTROSPECTION, &err);
    if (info == NULL) {
        g_message ("notifications : introspection illisible : %s", err->message);
        return;
    }

    n->objet_id = g_dbus_connection_register_object (
        n->bus, "/org/freedesktop/Notifications", info->interfaces[0],
        &VTABLE, n, NULL, &err);
    if (n->objet_id == 0) {
        g_message ("notifications : objet non enregistre : %s", err->message);
        return;
    }

    /* SANS REMPLACEMENT ET SANS ATTENTE. Si un autre demon tient deja le
     * nom, on le lui laisse et l'on se tait : deux serveurs qui se disputent
     * org.freedesktop.Notifications produisent des notifications qui
     * arrivent une fois sur deux, panne bien plus deroutante que l'absence
     * de cloche allumee. */
    n->nom_id = g_bus_own_name_on_connection (
        n->bus, "org.freedesktop.Notifications",
        G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
}

/* -------------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------------- */
static void
on_cloche (GtkButton *b, gpointer data)
{
    Notifs *n = data;
    (void) b;

    if (gtk_widget_get_visible (n->centre)) {
        centre_fermer (n);
        return;
    }

    /* Ouvrir, c'est lire. La cloche s'eteint donc a l'ouverture et non a la
     * fermeture : rouvrir pour verifier ne doit pas rallumer ce qu'on vient
     * de consulter. */
    for (guint i = 0; i < n->liste->len; i++)
        ((Notif *) g_ptr_array_index (n->liste, i))->lue = TRUE;
    cloche_rafraichir (n);

    /* La banniere s'efface : ce qu'elle annoncait est desormais sous les
     * yeux, et elle occuperait la place du centre. */
    if (n->banniere_timer != 0) {
        g_source_remove (n->banniere_timer);
        n->banniere_timer = 0;
    }
    n->banniere_attente = FALSE;
    gtk_popover_popdown (GTK_POPOVER (n->banniere));

    centre_reconstruire (n);
    positionner (n);
    gtk_popover_popup (GTK_POPOVER (n->centre));

    /* LA NAPPE SE DEPLOIE A L'OUVERTURE. Ecrite le 8 septembre 2026 avec son
     * repli, elle n'etait jamais deployee : seul l'appel a FALSE existait,
     * et le clic a cote ne fermait pas le centre. */
    nappe_deployer (n, TRUE);
    prevenir_barre (n);
}

static GtkWidget *
centre_construire (Notifs *n)
{
    GtkWidget *titre = gtk_label_new ("Notifications");
    gtk_widget_add_css_class (titre, "qs-titre-page");
    gtk_label_set_xalign (GTK_LABEL (titre), 0.0);
    gtk_widget_set_hexpand (titre, TRUE);

    GtkWidget *effacer = gtk_button_new_with_label ("Tout effacer");
    gtk_widget_add_css_class (effacer, "notif-effacer");
    g_signal_connect (effacer, "clicked", G_CALLBACK (on_tout_effacer), n);

    GtkWidget *entete = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class (entete, "qs-entete");
    gtk_box_append (GTK_BOX (entete), titre);
    gtk_box_append (GTK_BOX (entete), effacer);

    n->centre_liste = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);

    GtkWidget *defil = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (defil),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (defil), n->centre_liste);
    /* Hauteur bornee, pour la meme raison que la liste des reseaux : une
     * pile de notifications ne doit pas pousser le centre hors de l'ecran.
     * `propagate-natural-height` la laisse plus courte quand il y en a peu,
     * sans quoi trois cartes flotteraient dans un cadre de 320 px. */
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (defil), TRUE);
    gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (defil), 320);

    n->centre_vide = gtk_label_new ("Aucune notification");
    gtk_widget_add_css_class (n->centre_vide, "qs-vide");
    gtk_widget_set_margin_top (n->centre_vide, 18);
    gtk_widget_set_margin_bottom (n->centre_vide, 18);

    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class (boite, "qs");
    gtk_widget_add_css_class (boite, "notif-centre");
    gtk_box_append (GTK_BOX (boite), entete);
    gtk_box_append (GTK_BOX (boite), defil);
    gtk_box_append (GTK_BOX (boite), n->centre_vide);
    n->centre_boite = boite;
    return boite;
}

void
notifs_ancrer (Notifs *n, GtkWidget *ancre)
{
    /* LES DEUX SURFACES SONT ACCROCHEES A L'ANCRE DE LA CONSOLE.
     *
     * C'est ce qui leur donne son bord droit et sa largeur sans le moindre
     * calcul. `halign END` reprend le reglage de la Console, pour la meme
     * raison qu'elle : la barre etant collee au bord de l'ecran, un panneau
     * centre deborderait. */
    n->centre = gtk_popover_new ();
    gtk_popover_set_has_arrow (GTK_POPOVER (n->centre), FALSE);
    /* VERS LE HAUT, ET DIT EXPLICITEMENT.
     *
     * Un popover s'ouvre vers le bas par defaut. Le notre nait a trois
     * pixels du bord bas de l'ecran : GTK le retournait donc pour le faire
     * tenir — et ce retournement annulait le decalage vertical. Mesure au
     * banc : offset -12 donnait popup_y=-391, offset -387 donnait -390. Un
     * ecart d'un pixel pour un decalage de 375, autant dire aucun effet.
     *
     * En annoncant GTK_POS_TOP, plus rien n'est a retourner, et le decalage
     * s'applique tel qu'on l'a calcule. */
    gtk_popover_set_position (GTK_POPOVER (n->centre), GTK_POS_TOP);
    gtk_widget_add_css_class (n->centre, "qs-popover");
    gtk_widget_set_halign (n->centre, GTK_ALIGN_END);
    gtk_popover_set_child (GTK_POPOVER (n->centre), centre_construire (n));
    gtk_widget_set_parent (n->centre, ancre);

    /* AUTOHIDE DESACTIVE, ET C'EST LA CONDITION DE TOUT L'EMPILEMENT.
     *
     * Un popover qui se cache tout seul prend une saisie du pointeur : la
     * Console se refermerait en ouvrant le centre, et l'inverse aussi. Or
     * l'exigence est precisement que les deux coexistent, l'un au-dessus de
     * l'autre. Le prix a payer est qu'un clic a cote ne referme pas le
     * centre : la cloche le referme, et c'est le meme geste qui l'a ouvert. */
    gtk_popover_set_autohide (GTK_POPOVER (n->centre), FALSE);

    n->banniere = gtk_popover_new ();
    gtk_popover_set_has_arrow (GTK_POPOVER (n->banniere), FALSE);
    gtk_popover_set_position (GTK_POPOVER (n->banniere), GTK_POS_TOP);
    gtk_widget_add_css_class (n->banniere, "qs-popover");
    gtk_widget_add_css_class (n->banniere, "notif-banniere");
    gtk_widget_set_halign (n->banniere, GTK_ALIGN_END);
    gtk_popover_set_autohide (GTK_POPOVER (n->banniere), FALSE);
    gtk_widget_set_parent (n->banniere, ancre);

    positionner (n);
    centre_reconstruire (n);
}

GtkWidget *
notifs_cloche (Notifs *n)
{
    return n->cloche;
}

Notifs *
notifs_new (gboolean apercu)
{
    Notifs *n = g_new0 (Notifs, 1);
    n->liste = g_ptr_array_new_with_free_func (notif_free);
    n->prochain_id = 1;
    n->apercu = apercu;

    /* L'icone a 20 px, pas aux 16 par defaut : la cloche prend la hauteur
     * de la pilule (status.c, cloche_caler), et la pilule a grandi avec la
     * date. Une icone de 16 dans un rond de 48 s'y perdait. */
    GtkWidget *pictogramme = gtk_image_new_from_icon_name ("claude-os-cloche-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (pictogramme), 20);
    n->cloche = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (n->cloche), pictogramme);
    gtk_widget_add_css_class (n->cloche, "cloche");
    gtk_widget_set_valign (n->cloche, GTK_ALIGN_END);
    gtk_widget_set_tooltip_text (n->cloche, "Notifications");
    g_signal_connect (n->cloche, "clicked", G_CALLBACK (on_cloche), n);

    if (apercu) {
        /* Le banc d'essai ne touche pas au bus : il ne doit ni dependre d'un
         * service reel, ni disputer le nom a la barre de la vraie session. */
        const char *jeu[][3] = {
            { "Chromium",       "Réunion dans 10 minutes", "Point hebdomadaire — salle 2" },
            { "Claude Desktop", "Tâche terminée",          "La compilation du shell s’est terminée sans erreur." },
            { "Chromium",       "Téléchargement terminé",  "debian-13.iso" },
        };
        for (guint i = 0; i < G_N_ELEMENTS (jeu); i++) {
            Notif *x = g_new0 (Notif, 1);
            x->id     = n->prochain_id++;
            x->app    = g_strdup (jeu[i][0]);
            x->resume = g_strdup (jeu[i][1]);
            x->corps  = g_strdup (jeu[i][2]);
            x->date   = g_date_time_new_now_local ();
            x->urgence = 1;
            g_ptr_array_insert (n->liste, 0, x);
        }
        cloche_rafraichir (n);
        return n;
    }

    g_bus_get (G_BUS_TYPE_SESSION, NULL, on_bus, n);
    return n;
}

/* -------------------------------------------------------------------------
 * La barre qui peut etre partie -- voir notifications.h
 * ------------------------------------------------------------------------- */
void
notifs_suivre_barre (Notifs *n, NotifsBarreFunc f, gpointer user_data)
{
    n->barre      = f;
    n->barre_data = user_data;
}

void
notifs_barre_en_place (Notifs *n)
{
    if (n->banniere_attente)
        banniere_afficher (n);
}

gboolean
notifs_occupe (Notifs *n)
{
    return n->banniere_attente
        || (n->banniere != NULL && gtk_widget_get_visible (n->banniere))
        || (n->centre   != NULL && gtk_widget_get_visible (n->centre));
}

void
notifs_fermer_centre (Notifs *n)
{
    if (n->centre != NULL && gtk_widget_get_visible (n->centre))
        centre_fermer (n);
}
