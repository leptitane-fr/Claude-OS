/* =========================================================================
 * Claude-OS Shell — barre d'etat (bas a droite)
 *
 * Reseau, batterie et heure, dans une pilule au meme langage visuel que le
 * dock. Disposition inspiree de ChromeOS : les informations se regroupent
 * dans un coin plutot que de s'etaler sur toute la largeur.
 *
 * DISCIPLINE D'ENERGIE
 *
 * Une barre d'etat est le composant qui risque le plus de reveiller la
 * machine en permanence. Ici :
 *
 *  - une SEULE minuterie, alignee sur la minute, met a jour l'heure ET la
 *    batterie. Un reveil par minute, pas un par seconde ni un par element.
 *    La batterie n'a pas besoin d'etre plus fraiche que cela.
 *  - le reseau ne consulte rien : il reagit aux signaux D-Bus de
 *    NetworkManager, donc uniquement quand l'etat change reellement.
 *  - aucune animation au repos. La seule est la sortie par le bas, qui ne
 *    dure que le temps du mouvement (glissiere.c).
 *
 * A L'ECRAN OU NON, CE N'EST PAS LA BARRE QUI EN DECIDE : c'est le dock, qui
 * suit les fenetres (voir visibility.h). La barre recoit « afficher » ou
 * « masquer » sur le bus, et n'y deroge que pour ce qu'elle porte elle-meme :
 * une banniere a montrer, la Console ou le centre ouverts.
 * ========================================================================= */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

#include "config.h"
#include "glissiere.h"
#include "notifications.h"
#include "energie.h"
#include "batterie.h"
#include "preavis.h"
#include "panel.h"
#include "sysfs.h"

#include <stdlib.h>            /* atoi */

#define BAT_LOW_PERCENT 20      /* seuil d'alerte visuelle                   */

typedef struct {
    Notifs    *notifs;
    gboolean   ouvrir_centre;
    ShellGlissiere *glissiere;
    GtkWidget *console;     /* le popover de la pilule                     */
    gboolean   voulue;      /* ce que le dock demande                      */
    GtkWidget *pilule;      /* le bouton de la barre : donne sa hauteur   */
    int        hauteur_calee;  /* hauteur deja donnee a la cloche          */
    guint      calage;         /* recalage differe en attente              */
    GtkWidget *clock;
    GtkWidget *date;        /* au-dessus de l'heure                        */
    GtkWidget *mode_icon;   /* le mode de veille en vigueur                */
    GtkWidget *bat_icon;
    GtkWidget *bat_level;
    GtkWidget *net_icon;
} Status;

/* -------------------------------------------------------------------------
 * Batterie — la lecture de sysfs est partagee avec le panneau (sysfs.c).
 * ------------------------------------------------------------------------- */
static void
battery_update (Status *st)
{
    g_autofree char *dir = shell_battery_dir ();
    if (dir == NULL) {
        gtk_widget_set_visible (st->bat_icon, FALSE);
        gtk_widget_set_visible (st->bat_level, FALSE);
        return;
    }

    g_autofree char *cap_s    = shell_sysfs_read (dir, "capacity");
    g_autofree char *status_s = shell_sysfs_read (dir, "status");
    if (cap_s == NULL)
        return;

    int cap = atoi (cap_s);
    gboolean charging = (g_strcmp0 (status_s, "Charging") == 0
                      || g_strcmp0 (status_s, "Full") == 0);

    g_autofree char *label = g_strdup_printf ("%d%%", cap);
    gtk_label_set_text (GTK_LABEL (st->bat_level), label);

    /* Le theme d'icones fournit une famille battery-level-NNN : on arrondit
     * a la dizaine, ce que ces themes attendent. */
    int step = (cap + 5) / 10 * 10;
    if (step > 100) step = 100;
    g_autofree char *icon = g_strdup_printf ("battery-level-%d%s-symbolic",
                                             step, charging ? "-charging" : "");
    gtk_image_set_from_icon_name (GTK_IMAGE (st->bat_icon), icon);

    if (cap <= BAT_LOW_PERCENT && !charging)
        gtk_widget_add_css_class (st->bat_level, "low");
    else
        gtk_widget_remove_css_class (st->bat_level, "low");
}

/* -------------------------------------------------------------------------
 * Horloge — minuterie alignee sur la minute
 * ------------------------------------------------------------------------- */
/* LA DATE AU-DESSUS DE L'HEURE, demandee le 11 septembre 2026 : elle se
 * lit sans ouvrir quoi que ce soit, et ses deux lignes donnent a la pilule
 * la hauteur d'une cible qu'on touche au doigt sans viser.
 *
 * « ven. 11 sept. » : la forme abregee de la locale. « %-d » plutot que
 * « %e », qui pose une espace de chiffre devant les jours a un chiffre. Le
 * meme reveil que l'heure la met a jour -- minuit est une minute comme une
 * autre. */
static void
clock_update (Status *st)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_local ();
    g_autofree char *text = g_date_time_format (now, "%H:%M");
    g_autofree char *jour = g_date_time_format (now, "%a %-d %b");
    gtk_label_set_text (GTK_LABEL (st->clock), text);
    gtk_label_set_text (GTK_LABEL (st->date), jour);
}

/* Le mode de veille en vigueur, lu dans la configuration. Appele au
 * demarrage et a chaque relecture de shell.conf -- c'est la que la Console
 * ecrit le mode choisi : l'icone suit sans qu'on ait rien a lui dire. */
static void
mode_update (Status *st, const ShellConfig *cfg)
{
    const ShellModeEnergie *m = shell_energie_mode_actif (cfg);
    g_autoptr(GIcon) ic = shell_energie_mode_icone (m);
    gtk_image_set_from_gicon (GTK_IMAGE (st->mode_icon), ic);
    g_autofree char *bulle = g_strdup_printf ("Veille : mode %s", m->nom);
    gtk_widget_set_tooltip_text (st->mode_icon, bulle);
    gtk_widget_set_visible (st->mode_icon, cfg->energie_active);
}

static gboolean on_minute (gpointer data);

/* Replanifie exactement sur la seconde 0 de la minute suivante. Une minuterie
 * de 60 s glisserait peu a peu et changerait l'affichage a contretemps. */
static void
schedule_next_minute (Status *st)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_local ();
    guint delay_ms = (60 - g_date_time_get_second (now)) * 1000
                   - g_date_time_get_microsecond (now) / 1000;
    if (delay_ms < 500) delay_ms = 500;
    g_timeout_add (delay_ms, on_minute, st);
}

static gboolean
on_minute (gpointer data)
{
    Status *st = data;
    clock_update (st);
    battery_update (st);      /* meme reveil : rien de plus a payer */
    schedule_next_minute (st);
    return G_SOURCE_REMOVE;   /* on se replanifie soi-meme */
}

/* -------------------------------------------------------------------------
 * Reseau — signaux D-Bus de NetworkManager
 *
 * Aucune consultation periodique : NetworkManager previent quand son etat
 * change, ce qui est exactement ce qu'on veut.
 * ------------------------------------------------------------------------- */
static void
network_apply_state (Status *st, guint32 state)
{
    /* Etats NetworkManager : 70 = connecte au monde, 60 = connectivite
     * limitee, 50 = connexion locale, en dessous = deconnecte. */
    const char *icon = (state >= 70) ? "network-wireless-signal-excellent-symbolic"
                     : (state >= 50) ? "network-wireless-signal-weak-symbolic"
                                     : "network-offline-symbolic";
    gtk_image_set_from_icon_name (GTK_IMAGE (st->net_icon), icon);
}

static void
on_nm_properties (GDBusProxy *proxy, GVariant *changed,
                  const char * const *invalidated, gpointer data)
{
    Status *st = data;
    g_autoptr(GVariant) v = g_variant_lookup_value (changed, "State",
                                                    G_VARIANT_TYPE_UINT32);
    if (v != NULL)
        network_apply_state (st, g_variant_get_uint32 (v));

    (void) proxy; (void) invalidated;
}

/* Construction ASYNCHRONE. La variante synchrone n'accepte aucun delai et
 * attend les 25 secondes reglementaires si le service tarde : la barre
 * d'etat restait alors invisible tout ce temps, alors meme que sa fenetre
 * avait deja ete presentee. Mesure au banc d'essai contre un faux service.
 *
 * En attendant la reponse, la barre affiche « hors ligne » : un composant
 * d'etat qui n'affiche rien est plus deroutant qu'un composant qui annonce
 * une absence. */
static void
on_nm_proxy (GObject *src, GAsyncResult *res, gpointer data)
{
    Status *st = data;
    g_autoptr(GError) error = NULL;
    (void) src;

    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
    if (proxy == NULL) {
        g_message ("NetworkManager injoignable : %s", error->message);
        return;
    }

    g_autoptr(GVariant) state = g_dbus_proxy_get_cached_property (proxy, "State");
    network_apply_state (st, state ? g_variant_get_uint32 (state) : 0);

    g_signal_connect (proxy, "g-properties-changed",
                      G_CALLBACK (on_nm_properties), st);
}

static void
network_setup (Status *st)
{
    network_apply_state (st, 0);

    g_dbus_proxy_new_for_bus (
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager",
        NULL, on_nm_proxy, st);
}

/* -------------------------------------------------------------------------
 * Fenetre
 * ------------------------------------------------------------------------- */
static GtkWidget *
icon (const char *name, int size)
{
    GtkWidget *img = gtk_image_new_from_icon_name (name);
    gtk_image_set_pixel_size (GTK_IMAGE (img), size);
    gtk_widget_add_css_class (img, "status-item");
    return img;
}

/* -------------------------------------------------------------------------
 * A l'ecran ou non
 *
 * Le dock decide ; la barre suit, sauf quand elle porte quelque chose :
 *
 *   - une banniere a montrer. Barre partie, une notification arriverait
 *     sans rien afficher : la barre remonte le temps de la banniere, puis
 *     repart. La banniere n'est posee qu'une fois la barre immobile (voir
 *     notifications.h).
 *   - la Console ou le centre ouverts. On les a ouverts pour s'en servir ;
 *     la barre ne part pas de sous eux. Ils se ferment au clic a cote, et
 *     la barre repart alors d'elle-meme.
 *
 * Ce dernier point ne vaut pas contre un ordre du dock : « masquer » arrive
 * quand une application passe au premier plan, et ferme la Console et le
 * centre avec lui. La banniere, elle, va toujours a son terme.
 * ------------------------------------------------------------------------- */
static gboolean
barre_retenue (Status *st)
{
    return st->voulue
        || notifs_occupe (st->notifs)
        || (st->console != NULL && gtk_widget_get_visible (st->console));
}

static void
barre_evaluer (Status *st)
{
    if (barre_retenue (st))
        shell_glissiere_montrer (st->glissiere);
    else
        shell_glissiere_cacher (st->glissiere);
}

/* Le centre de notifications a besoin de la barre, ou n'en a plus besoin. */
static gboolean
on_notifs_barre (gpointer data)
{
    Status *st = data;
    barre_evaluer (st);
    return shell_glissiere_en_place (st->glissiere);
}

/* Fin d'un mouvement. Remontee : une banniere attendait peut-etre. */
static void
on_glissiere_fin (gboolean visible, gpointer data)
{
    Status *st = data;
    if (visible)
        notifs_barre_en_place (st->notifs);
}

/* La Console s'ouvre ou se ferme. Fermee par un clic a cote alors que le
 * dock ne veut plus de la barre : c'est le moment de partir. */
static void
on_console_visible (GObject *o, GParamSpec *p, gpointer data)
{
    (void) o; (void) p;
    barre_evaluer (data);
}

static void
barre_vouloir (Status *st, gboolean voulue)
{
    st->voulue = voulue;
    if (!voulue) {
        gtk_menu_button_popdown (GTK_MENU_BUTTON (st->pilule));
        notifs_fermer_centre (st->notifs);
    }
    barre_evaluer (st);
}

/* Les actions publiees sous os.claude.shell.status.
 *
 *   afficher, masquer   les ordres du dock ;
 *   basculer            la touche Loupe quand le dock ne repond pas --
 *                       claude-os-shell-basculer se rabat alors sur la
 *                       barre, qui bascule seule plutot que de rester
 *                       coincee dehors. */
static void
on_action_afficher (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p;
    barre_vouloir (data, TRUE);
}

static void
on_action_masquer (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p;
    barre_vouloir (data, FALSE);
}

static void
on_action_basculer (GSimpleAction *a, GVariant *p, gpointer data)
{
    Status *st = data;
    (void) a; (void) p;
    barre_vouloir (st, !st->voulue);
}

static const GActionEntry actions[] = {
    { "afficher", on_action_afficher, NULL, NULL, NULL, { 0 } },
    { "masquer",  on_action_masquer,  NULL, NULL, NULL, { 0 } },
    { "basculer", on_action_basculer, NULL, NULL, NULL, { 0 } },
};

/* Au demarrage, demander au dock ou il en est.
 *
 * A l'ouverture de session les deux s'accordent d'eux-memes : il n'y a pas
 * encore de fenetre, le dock est affiche, la barre aussi. Mais une barre
 * relancee seule pendant qu'on travaille s'afficherait par-dessus
 * l'application, et y resterait jusqu'au prochain changement de fenetre.
 * Le dock repond en redisant son dernier ordre.
 *
 * Qu'il ne reponde pas n'a rien d'anormal a l'ouverture de session -- les
 * deux demarrent ensemble --, et la barre reste alors affichee, ce qui est
 * juste. Le journal le dit quand meme. */
static void
on_dock_repond (GObject *src, GAsyncResult *res, gpointer data)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) r =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
    (void) data;
    if (r == NULL)
        g_message ("dock injoignable au demarrage, la barre reste affichee : %s",
                   err->message);
}

static void
demander_au_dock (GApplication *app)
{
    GDBusConnection *bus = g_application_get_dbus_connection (app);
    if (bus == NULL)
        return;
    g_dbus_connection_call (bus, "os.claude.shell.dock", "/os/claude/shell/dock",
                            "org.gtk.Actions", "Activate",
                            g_variant_new ("(sava{sv})", "annoncer", NULL, NULL),
                            NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL,
                            on_dock_repond, NULL);
}

typedef struct {
    ShellConfig *cfg;   /* police, theme d'icones, clair ou sombre           */
    gboolean apercu;    /* valeurs fixes dans le panneau                     */
    gboolean centre;    /* ouvre le centre de notifications au demarrage      */
    gboolean ouvrir;    /* ouvre le panneau au demarrage                     */
} Options;

/* En mode apercu seulement : ouvre le panneau tout seul pour que le banc
 * d'essai puisse le capturer. */
static gboolean
open_panel_once (gpointer button)
{
    gtk_menu_button_popup (GTK_MENU_BUTTON (button));
    return G_SOURCE_REMOVE;
}

static void
on_config_reloaded (ShellConfig *cfg, gpointer data)
{
    Status *st = data;
    shell_styles_load (cfg->theme);
    mode_update (st, cfg);      /* la Console vient peut-etre d'en changer */
    shell_config_apply (cfg);
    /* Avant de liberer : le panneau de reglages ecrit shell.conf, et les
     * delais de veille doivent suivre sans qu'on relance quoi que ce soit. */
    shell_energie_reconfigurer (cfg);
    shell_batterie_reconfigurer (cfg);
    shell_config_free (cfg);
}

/* LA CLOCHE PREND EXACTEMENT LA HAUTEUR DE LA PILULE.
 *
 * Mesuree, et non ecrite dans la feuille de style. La hauteur de la pilule
 * depend de sa police — l'heure est son element le plus haut — et la police
 * se regle dans shell.conf : un « min-height » en dur aurait ete juste sur
 * cette machine et faux des qu'on change de corps.
 *
 * L'allocation plutot que la mesure naturelle : elle exclut les marges CSS,
 * alors que gtk_widget_measure() les inclut. C'est bien la hauteur peinte
 * qu'on veut egaler, pas l'encombrement.
 *
 * Carree, donc, puisque le rayon de la cloche depasse la moitie : elle reste
 * ronde quelle que soit la valeur. */
/* Banc d'essai seulement : ouvre le centre comme le ferait un clic. */
static gboolean
ouvrir_centre_une_fois (gpointer data)
{
    g_signal_emit_by_name (notifs_cloche ((Notifs *) data), "clicked");
    return G_SOURCE_REMOVE;
}

/* LA HAUTEUR PEINTE, bordure comprise : c'est elle que l'oeil compare.
 * gtk_widget_get_height() rend la boite de contenu, sans la bordure de la
 * pilule -- deux pixels de moins, mesures au banc (42 contre 44). */
static int
hauteur_peinte (GtkWidget *w)
{
    graphene_rect_t r;
    if (!gtk_widget_compute_bounds (w, w, &r))
        return 0;
    return (int) (r.size.height + 0.5);
}

static gboolean
cloche_caler (gpointer data)
{
    Status *st = data;
    st->calage = 0;
    int h = hauteur_peinte (st->pilule);
    if (h > 0 && h != st->hauteur_calee) {
        st->hauteur_calee = h;
        gtk_widget_set_size_request (notifs_cloche (st->notifs), h, h);
    }
    return G_SOURCE_REMOVE;
}

/* A CHAQUE MISE EN PAGE, ET PAS UNE FOIS AU DEMARRAGE.
 *
 * ET SUR UNE CLOCHE SANS MARGE. Sous GTK 4, la taille demandee englobe les
 * marges CSS : la cloche demandait 42 x 42, ses marges (10 a droite, 12 en
 * bas) et sa bordure etaient prises dessus, et elle se peignait en 30 x 28
 * -- mesure au banc. Les marges sont donc portees par un socle
 * (« .cloche-socle »), et la cloche, sans marge, prend exactement ce qu'on
 * lui demande.
 *
 * La mesure unique, faite a la premiere image, tombait avant que la date ne
 * soit ecrite : la pilule passait ensuite de 30 a 42 px, et la cloche
 * restait a 32 -- vu au banc le 11 septembre 2026, le jour ou la date est
 * arrivee. La glissiere previent a chaque allocation ; on ne recale que si
 * la hauteur a change, et en differe : changer une taille demandee pendant
 * une allocation relancerait la mise en page au milieu d'elle-meme. */
static void
on_barre_allouee (int largeur, int hauteur, gpointer data)
{
    Status *st = data;
    (void) largeur; (void) hauteur;
    if (st->calage == 0 && hauteur_peinte (st->pilule) != st->hauteur_calee)
        st->calage = g_idle_add (cloche_caler, st);
}

static void
on_activate (GtkApplication *app, gpointer user_data)
{
    Options *opt = user_data;
    Status *st = g_new0 (Status, 1);

    /* Meme configuration que le dock, et appliquee au meme moment : sans
     * cela, « theme=dark » dans shell.conf donnait un dock sombre et une
     * barre claire cote a cote. */
    shell_config_apply (opt->cfg);

    GtkWidget *window = gtk_application_window_new (app);
    gtk_widget_add_css_class (window, "shell");

    gtk_layer_init_for_window (GTK_WINDOW (window));
    /* OVERLAY, comme le dock : en TOP, labwc l'eteindrait sous une fenetre
     * plein ecran, et la touche Loupe ne la ferait plus paraitre pendant une
     * video. Voir « A l'ecran ou non » dans dock.c. */
    gtk_layer_set_layer (GTK_WINDOW (window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor (GTK_WINDOW (window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (window), GTK_LAYER_SHELL_EDGE_RIGHT,  TRUE);
    gtk_layer_set_namespace (GTK_WINDOW (window), "claude-os-status");

    /* -1 : la barre IGNORE les zones reservees par les autres surfaces.
     *
     * Avec 0, elle se serait posee au-dessus des 86 px reserves par le dock
     * et aurait flotte plus haut que lui -- verifie a l'ecran. Avec -1 elle
     * s'ancre au vrai bord de l'ecran et partage la ligne de base du dock,
     * ce qui est la disposition voulue : dock centre, informations a droite,
     * sur le meme niveau.
     *
     * Elle ne reserve rien pour elle-meme : seul le dock repousse les
     * fenetres, sinon on perdrait deux fois de la hauteur utile. */
    gtk_layer_set_exclusive_zone (GTK_WINDOW (window), -1);
    /* ON_DEMAND, et pas NONE : le panneau de reglages doit pouvoir prendre
     * le clavier quand il s'ouvre, ne serait-ce que pour se fermer sur
     * Echap et pour rendre ses bascules atteignables au clavier. Hors
     * ouverture du panneau, la barre ne reclame rien. */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (window),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    /* Centree verticalement : la pilule prend la hauteur des deux lignes
     * date et heure, les icones se posent au milieu. */
    gtk_widget_set_valign (bar, GTK_ALIGN_CENTER);

    /* 18 px et non plus 16 : la pilule a grandi avec la date, des icones
     * restees a l'ancienne taille y paraissaient perdues. */
    st->mode_icon = icon ("power-profile-balanced-symbolic", 18);
    gtk_widget_add_css_class (st->mode_icon, "status-mode");
    st->net_icon  = icon ("network-offline-symbolic", 18);
    st->bat_icon  = icon ("battery-level-100-symbolic", 18);
    st->bat_level = gtk_label_new ("--%");
    gtk_widget_add_css_class (st->bat_level, "status-battery-level");

    st->date  = gtk_label_new ("");
    gtk_widget_add_css_class (st->date, "status-date");
    st->clock = gtk_label_new ("--:--");
    gtk_widget_add_css_class (st->clock, "status-clock");

    GtkWidget *temps = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (temps, "status-temps");
    gtk_widget_set_valign (temps, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (temps), st->date);
    gtk_box_append (GTK_BOX (temps), st->clock);

    GtkWidget *sep = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (sep, "status-sep");

    gtk_box_append (GTK_BOX (bar), st->mode_icon);
    gtk_box_append (GTK_BOX (bar), st->net_icon);
    gtk_box_append (GTK_BOX (bar), st->bat_icon);
    gtk_box_append (GTK_BOX (bar), st->bat_level);
    gtk_box_append (GTK_BOX (bar), sep);
    gtk_box_append (GTK_BOX (bar), temps);

    /* Toute la pilule est un bouton : c'est elle qu'on vise, pas une
     * poignee dediee. Le cadre par defaut de GtkMenuButton est retire, la
     * surface visible reste celle dessinee par .status. */
    GtkWidget *button = gtk_menu_button_new ();
    gtk_menu_button_set_has_frame (GTK_MENU_BUTTON (button), FALSE);
    gtk_menu_button_set_child (GTK_MENU_BUTTON (button), bar);
    gtk_menu_button_set_direction (GTK_MENU_BUTTON (button), GTK_ARROW_UP);
    GtkWidget *console = panel_new (opt->apercu);
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), console);
    gtk_widget_add_css_class (button, "status");
    gtk_widget_set_halign (button, GTK_ALIGN_END);
    gtk_widget_set_valign (button, GTK_ALIGN_END);

    /* LA CLOCHE, A GAUCHE ET DETACHEE.
     *
     * Elle ne fait pas partie de la pilule : ronde, separee par un ecart,
     * elle se lit comme un objet distinct — ce qu'elle est. Cliquer la
     * pilule ouvre la Console, cliquer la cloche ouvre les notifications, et
     * rien dans le dessin ne laisse croire que les deux gestes se
     * confondent.
     *
     * Les deux surfaces du centre sont accrochees a `button`, l'ancre de la
     * Console, et non a la cloche : c'est ce qui leur donne exactement son
     * bord droit. Accrochees a la cloche, elles se seraient alignees sur le
     * bord gauche de la barre. */
    st->pilule = button;
    st->notifs = notifs_new (opt->apercu);
    notifs_ancrer (st->notifs, button);
    notifs_suivre_console (st->notifs, console);
    notifs_nappe (st->notifs, window);

    GtkWidget *rangee = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign (rangee, GTK_ALIGN_END);
    gtk_widget_set_valign (rangee, GTK_ALIGN_END);
    GtkWidget *socle = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (socle, "cloche-socle");
    gtk_widget_set_valign (socle, GTK_ALIGN_END);
    gtk_box_append (GTK_BOX (socle), notifs_cloche (st->notifs));
    gtk_box_append (GTK_BOX (rangee), socle);
    gtk_box_append (GTK_BOX (rangee), button);

    /* La glissiere entre la fenetre et la rangee : c'est elle qui fait
     * sortir la barre par le bas, et retire la fenetre une fois en bas. */
    GtkWidget *glissiere = shell_glissiere_new (rangee);
    st->glissiere = SHELL_GLISSIERE (glissiere);
    st->console   = console;
    st->voulue    = TRUE;        /* ouverture de session : le dock l'est aussi */
    shell_glissiere_sur_fin (st->glissiere, on_glissiere_fin, st);
    shell_glissiere_sur_allocation (st->glissiere, on_barre_allouee, st);
    notifs_suivre_barre (st->notifs, on_notifs_barre, st);
    g_signal_connect (console, "notify::visible",
                      G_CALLBACK (on_console_visible), st);

    gtk_window_set_child (GTK_WINDOW (window), glissiere);
    /* Date, heure et mode AVANT la premiere image : la pilule se pose
     * directement a sa hauteur, sans grandir sous les yeux une image plus
     * tard. */
    clock_update (st);
    mode_update (st, opt->cfg);

    gtk_window_present (GTK_WINDOW (window));

    g_action_map_add_action_entries (G_ACTION_MAP (app), actions,
                                     G_N_ELEMENTS (actions), st);
    /* La fenetre de la barre se retire quand elle sort de l'ecran : sans
     * cela, GApplication sortirait de sa boucle avec elle, et les
     * notifications et la veille de l'ecran s'arreteraient. */
    g_application_hold (G_APPLICATION (app));
    if (!opt->apercu)
        demander_au_dock (G_APPLICATION (app));
    shell_config_watch (on_config_reloaded, st);

    /* Le cadran du preavis prend la largeur de la pilule -- la barre sans
     * sa cloche. C'est ici, et nulle part ailleurs, qu'on sait quel widget
     * c'est. */
    shell_preavis_reference (button);

    shell_energie_init (opt->cfg);
    shell_batterie_init (opt->cfg);

    if (opt->ouvrir)
        g_idle_add (open_panel_once, button);
    if (opt->centre)
        g_idle_add (ouvrir_centre_une_fois, st->notifs);

    battery_update (st);
    network_setup (st);
    schedule_next_minute (st);
}

int
main (int argc, char **argv)
{
    /* --ouvrir : ouvre la Console au demarrage, avec les vraies sources.
     * --centre : ouvre le centre de notifications, meme usage.
     * --apercu : idem, mais avec des valeurs fixes, pour juger la mise en
     *            page quand aucun service n'est present. Une aide au banc
     *            d'essai, qui ne prouve rien du branchement D-Bus. */
    Options opt = { shell_config_load (), FALSE, FALSE, FALSE };

    /* Les options de ligne de commande priment sur le fichier : pratique
     * pour essayer un theme sans toucher a sa configuration. */
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0 (argv[i], "--dark") == 0)   opt.cfg->dark = TRUE;
        if (g_strcmp0 (argv[i], "--light") == 0)  opt.cfg->dark = FALSE;
        if (g_strcmp0 (argv[i], "--ouvrir") == 0) opt.ouvrir = TRUE;
        if (g_strcmp0 (argv[i], "--apercu") == 0) opt.apercu = opt.ouvrir = TRUE;
        /* --centre : ouvre le centre de notifications au demarrage. Meme
         * usage que --ouvrir pour la Console — juger l'empilement des deux
         * surfaces au banc, ou l'on ne peut pas cliquer. */
        if (g_strcmp0 (argv[i], "--centre") == 0) opt.centre = TRUE;
    }

    GtkApplication *app = gtk_application_new ("os.claude.shell.status",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "startup",  G_CALLBACK (shell_styles_startup), opt.cfg);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), &opt);
    int status = g_application_run (G_APPLICATION (app), 0, NULL);
    g_object_unref (app);
    return status;
}
