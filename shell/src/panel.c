/* =========================================================================
 * Claude-OS Shell — panneau de reglages rapides
 *
 * Contenu : deux bascules (Wi-Fi, Bluetooth) et une carte batterie qui
 * donne la consommation instantanee en watts.
 *
 * DISCIPLINE D'ENERGIE
 *
 * Le panneau ne coute rien tant qu'il est ferme :
 *
 *  - les bascules ne consultent jamais. Elles lisent l'etat une fois a la
 *    construction, puis suivent les signaux de NetworkManager et de BlueZ.
 *  - la minuterie des watts ne tourne QUE pendant que le panneau est
 *    ouvert. C'est la seule chose ici qui reveille la machine
 *    periodiquement, et elle s'arrete a la fermeture.
 *
 * ETAT DES BASCULES
 *
 * Un geste sur l'interrupteur ne bascule pas l'affichage : il demande le
 * changement, et l'affichage suit le signal renvoye par le service. Si
 * polkit refuse, ou si le materiel est bloque par un interrupteur physique,
 * la bascule reste visiblement dans son etat reel au lieu de mentir.
 * ========================================================================= */

#include "panel.h"
#include "bluetooth.h"
#include "console.h"
#include "sysfs.h"
#include "wifi.h"

/* GDesktopAppInfo vit dans gio-unix, pas dans gio tout court. */
#include <gio/gdesktopappinfo.h>

#include <stdlib.h>            /* atoi */

#define WATT_REFRESH_MS 2000    /* uniquement panneau ouvert                 */

/* Cadence de relecture du son et de la luminosite, panneau ouvert seulement.
 *
 * Les touches du clavier passent par labwc, qui appelle wpctl et
 * brightnessctl sans rien nous dire — aucun de ces deux services n'emet de
 * signal que l'on puisse ecouter. La seule facon de suivre, c'est de relire.
 *
 * 400 ms : sous ~500 ms le curseur semble suivre la touche, au-dela le
 * decalage se voit.
 *
 * CE QUE CELA COUTE, MESURE SUR CETTE MACHINE. La luminosite est un simple
 * fichier sysfs, negligeable. Le son lance « wpctl get-volume », et wpctl
 * n'est pas gratuit : 40 appels en 1,71 s, soit 43 ms d'horloge et 35 ms de
 * processeur chacun. A 400 ms de cadence cela represente environ 9 % d'un
 * coeur — pendant les quelques secondes ou la Console est ouverte, et rien
 * du tout une fois refermee.
 *
 * On paie donc un peu, en echange d'un curseur qui ne ment pas. Faire mieux
 * demanderait d'ecouter PipeWire directement, donc de lier libpipewire au
 * shell pour lire un nombre — exactement ce que console.h refuse de faire
 * pour libpulse. Le compromis est assume, pas subi. */
#define SUIVI_REFRESH_MS 400

/* Depliage de la colonne de detail. Assez court pour qu'on n'attende pas,
 * assez long pour qu'on voie d'ou elle sort — sans quoi le panneau semble
 * avoir toujours ete large, et l'on cherche ce qui a change. */
#define REVEAL_MS 160

/* L'ecart avec la barre d'etat est defini dans panel.h : le centre de
 * notifications s'en sert aussi, et deux valeurs qui doivent rester egales
 * ne s'ecrivent pas deux fois. */

/* Largeur de la colonne de detail. La meme que la Console — « .qs
 * { min-width: 296px } » dans shell.css — pour que le panneau deplie ait
 * deux colonnes de meme largeur plutot qu'un assemblage bancal. */
#define LARGEUR_COLONNE_PX 296

/* ------------------------------------------------------------------------- */

typedef struct _Panel Panel;

typedef struct {
    GtkWidget  *button;         /* ouvre le volet de detail                  */
    GtkWidget  *icon;
    GtkWidget  *interrupteur;   /* GtkSwitch : allume et eteint le module    */
    const char *nom;            /* « Wi-Fi » — pour l'infobulle et l'a11y    */
    const char *page;           /* nom de sa page dans la pile               */
    const char *icon_on;
    const char *icon_off;
    gboolean    on;
    gboolean    pose_en_cours;  /* l'interrupteur bouge par programme        */
    Panel      *panel;

    GDBusProxy *proxy;          /* NULL si le service est absent             */
    const char *iface;          /* interface portant la propriete            */
    const char *prop;
} Tile;

struct _Panel {
    GtkStack  *pile;
    GtkWidget *page_wifi;
    GtkWidget *page_bt;
    Tile       wifi;
    Tile       bluetooth;
    GtkWidget *reveleur;        /* colonne de detail, a gauche de la Console */
    GtkWidget *popover;
    GtkWidget *bat_pct;
    GtkWidget *bat_detail;
    GtkWidget *bat_icon;
    GtkWidget *son;             /* rangee volume                             */
    GtkWidget *lumiere;         /* rangee luminosite                         */
    GtkWidget *energie;         /* rangee veille de l'ecran                  */
    guint      watt_timer;      /* 0 quand le panneau est ferme              */
    guint      suivi_timer;     /* idem : son et luminosite                  */
    gboolean   services_sondes; /* NetworkManager et BlueZ deja contactes ?  */
    gboolean   apercu;

    GtkWidget     *rangee;      /* enfant du popover : sa hauteur fait foi   */
    GtkWidget     *box_console; /* la Console seule : sa largeur fait foi    */
    gboolean       ouverte;
    PanelGeometrieFn geo_fn;
    gpointer         geo_data;
    int              hauteur_dite;  /* dernieres valeurs annoncees           */
    int              largeur_dite;
};

/* -------------------------------------------------------------------------
 * Hauteur annoncee au centre de notifications
 * ------------------------------------------------------------------------- */
/* La hauteur NATURELLE de l'enfant, et non celle du popover : la surface du
 * popover englobe l'ombre portee — mesuree a 768x491 pour une Console qui
 * n'occupe que 400 px a l'ecran — et poser le centre de notifications
 * au-dessus de l'ombre l'aurait decolle de la Console de pres de cent
 * pixels. La naturelle, elle, ne depend pas de l'allocation, donc pas du
 * moment ou on la demande. */
static void
annoncer_hauteur (Panel *p)
{
    if (p->geo_fn == NULL)
        return;

    int ignore, largeur = 0, hauteur = 0;
    /* La largeur se mesure toujours, ouverte ou fermee : le centre de
     * notifications s'aligne dessus meme quand la Console est repliee. */
    gtk_widget_measure (p->box_console, GTK_ORIENTATION_HORIZONTAL, -1,
                        &ignore, &largeur, &ignore, &ignore);
    if (p->ouverte)
        gtk_widget_measure (p->rangee, GTK_ORIENTATION_VERTICAL, -1,
                            &ignore, &hauteur, &ignore, &ignore);

    if (hauteur == p->hauteur_dite && largeur == p->largeur_dite)
        return;                     /* rien de neuf : pas de repositionnement */
    p->hauteur_dite = hauteur;
    p->largeur_dite = largeur;
    p->geo_fn (largeur, hauteur, p->geo_data);
}

void
panel_observer_geometrie (GtkWidget *popover, PanelGeometrieFn fn, gpointer data)
{
    Panel *p = g_object_get_data (G_OBJECT (popover), "panel");
    g_return_if_fail (p != NULL);
    p->geo_fn       = fn;
    p->geo_data     = data;
    p->hauteur_dite = -1;           /* force la premiere annonce             */
    p->largeur_dite = -1;
    annoncer_hauteur (p);
}

/* -------------------------------------------------------------------------
 * Bascules
 * ------------------------------------------------------------------------- */
static void replier_si_ouverte (Panel *p, const char *page);

static void
tile_apply (Tile *t, gboolean on, gboolean available)
{
    t->on = on;
    gtk_image_set_from_icon_name (GTK_IMAGE (t->icon), on ? t->icon_on : t->icon_off);

    /* L'INTERRUPTEUR SUIT LE SERVICE, PAS LE DOIGT. « state » est ce que le
     * service a confirme, « active » ce que le doigt a demande : les poser
     * ensemble ici, a chaque signal, remet l'interrupteur d'accord avec la
     * realite -- y compris quand polkit a refuse. */
    t->pose_en_cours = TRUE;
    gtk_switch_set_active (GTK_SWITCH (t->interrupteur), on);
    gtk_switch_set_state (GTK_SWITCH (t->interrupteur), on);
    t->pose_en_cours = FALSE;
    gtk_widget_set_sensitive (t->interrupteur, available);

    /* ETEINT, LE BOUTON EST GRISE ET INOPERANT, comme demande le 11 septembre
     * 2026 : ouvrir la liste des reseaux d'une radio coupee ne montrerait
     * rien, et laisserait croire a une panne. Si son volet etait ouvert, il
     * se replie. */
    gtk_widget_set_sensitive (t->button, on && available);
    if (!on && t->panel != NULL)
        replier_si_ouverte (t->panel, t->page);

    /* L'infobulle porte le nom ET l'etat : c'est aussi le nom que lit le
     * lecteur d'ecran, et « indisponible » ne se distingue d'« eteint » que
     * par la sensibilite de l'interrupteur. */
    g_autofree char *bulle = g_strdup_printf (
        "%s — %s", t->nom,
        !available ? "indisponible" : on ? "activé" : "désactivé");
    gtk_widget_set_tooltip_text (t->button, bulle);
    gtk_widget_set_tooltip_text (t->interrupteur, bulle);
}

/* Ecrit la propriete via org.freedesktop.DBus.Properties.Set.
 *
 * En asynchrone : un appel bloquant sur le bus systeme gele l'interface si
 * le service tarde, et polkit peut prendre plusieurs centaines de
 * millisecondes pour trancher. */
static void
on_set_done (GObject *src, GAsyncResult *res, gpointer data)
{
    Tile *t = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (src), res, &error);
    if (reply == NULL) {
        g_message ("%s : bascule refusee : %s", t->nom, error->message);
        /* Refuse : aucun signal ne viendra. On remet l'interrupteur sur
         * l'etat reel nous-memes, sinon il resterait sur la demande. */
        tile_apply (t, t->on, gtk_widget_get_sensitive (t->interrupteur));
    }
}

/* « state-set » plutot que « notify::active » : il permet de NE PAS changer
 * l'etat tout de suite (on rend TRUE), et d'attendre que le service le
 * confirme. L'interrupteur montre la demande, l'etat suit la realite. */
static gboolean
on_interrupteur (GtkSwitch *sw, gboolean voulu, gpointer data)
{
    Tile *t = data;
    (void) sw;

    if (t->pose_en_cours)
        return FALSE;               /* c'est nous : laisser GTK poser l'etat */
    if (t->proxy == NULL || voulu == t->on)
        return TRUE;

    g_dbus_proxy_call (t->proxy,
                       "org.freedesktop.DBus.Properties.Set",
                       g_variant_new ("(ssv)", t->iface, t->prop,
                                      g_variant_new_boolean (voulu)),
                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                       on_set_done, t);
    return TRUE;
}

/* UNE PASTILLE, DEUX OBJETS SUPERPOSES.
 *
 * Le 11 septembre 2026, l'utilisateur a juge l'ancienne pastille peu
 * pratique : tout son corps allumait et eteignait, et le volet de detail
 * s'ouvrait par un chevron de 24 px pose dans un coin -- trop petit, mal
 * place, et le geste le plus frequent (choisir un reseau) etait le plus dur
 * a viser. Les roles sont donc inverses :
 *
 *   - le BOUTON, grand, ouvre le volet ;
 *   - l'INTERRUPTEUR, en dessous, allume et eteint le module.
 *
 * Deux objets distincts plutot qu'un bouton qui ferait deux choses selon
 * l'endroit du clic : chacun se voit, chacun se vise. */
static GtkWidget *
tile_build (Tile *t, Panel *p, const char *name, const char *page,
            const char *icon_on, const char *icon_off)
{
    t->icon_on  = icon_on;
    t->icon_off = icon_off;
    t->nom      = name;
    t->page     = page;
    t->panel    = p;

    t->icon = gtk_image_new_from_icon_name (icon_off);
    gtk_image_set_pixel_size (GTK_IMAGE (t->icon), 20);
    gtk_widget_add_css_class (t->icon, "qs-tile-icon");

    GtkWidget *nom = gtk_label_new (name);
    gtk_widget_add_css_class (nom, "qs-tile-name");
    gtk_widget_set_hexpand (nom, TRUE);
    gtk_widget_set_halign (nom, GTK_ALIGN_START);

    /* LA FLECHE POINTE VERS OU LE VOLET S'OUVRE : a gauche de la Console.
     * Elle n'est plus un bouton, seulement une indication dans le bouton. */
    GtkWidget *fleche = gtk_image_new_from_icon_name ("go-previous-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (fleche), 14);
    gtk_widget_add_css_class (fleche, "qs-tile-state");

    GtkWidget *ligne = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append (GTK_BOX (ligne), fleche);
    gtk_box_append (GTK_BOX (ligne), t->icon);
    gtk_box_append (GTK_BOX (ligne), nom);

    t->button = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (t->button), ligne);
    gtk_widget_add_css_class (t->button, "qs-tile");
    gtk_widget_set_hexpand (t->button, TRUE);
    gtk_widget_set_sensitive (t->button, FALSE);

    t->interrupteur = gtk_switch_new ();
    gtk_widget_add_css_class (t->interrupteur, "qs-interrupteur");
    gtk_widget_set_halign (t->interrupteur, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive (t->interrupteur, FALSE);   /* service pas encore joint */
    g_signal_connect (t->interrupteur, "state-set", G_CALLBACK (on_interrupteur), t);

    GtkWidget *colonne = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand (colonne, TRUE);
    gtk_box_append (GTK_BOX (colonne), t->button);
    gtk_box_append (GTK_BOX (colonne), t->interrupteur);
    return colonne;
}

/* Lit une propriete booleenne du cache du proxy. */
static gboolean
proxy_bool (GDBusProxy *proxy, const char *name, gboolean fallback)
{
    g_autoptr(GVariant) v = g_dbus_proxy_get_cached_property (proxy, name);
    return v ? g_variant_get_boolean (v) : fallback;
}

/* --- Wi-Fi : NetworkManager --------------------------------------------- */
static void
wifi_refresh (Tile *t)
{
    /* WirelessHardwareEnabled reflete l'interrupteur materiel (rfkill dur).
     * Quand il est a faux, aucun logiciel ne peut rallumer la radio : la
     * bascule doit alors etre grisee, pas simplement « desactivee ». */
    tile_apply (t,
                proxy_bool (t->proxy, "WirelessEnabled", FALSE),
                proxy_bool (t->proxy, "WirelessHardwareEnabled", TRUE));
}

static void
on_wifi_props (GDBusProxy *proxy, GVariant *changed,
               const char * const *invalidated, gpointer data)
{
    (void) proxy; (void) changed; (void) invalidated;
    wifi_refresh (data);
}

/* Construction ASYNCHRONE, et ce n'est pas un raffinement : la variante
 * synchrone attend le delai D-Bus complet quand le service tarde a repondre.
 * Mesure au banc d'essai contre un service muet : 25 secondes pendant
 * lesquelles la barre d'etat n'apparaissait pas du tout. La barre s'affiche
 * desormais tout de suite, et les bascules se remplissent quand les services
 * repondent. */
static void
on_wifi_proxy (GObject *src, GAsyncResult *res, gpointer data)
{
    Tile *t = data;
    g_autoptr(GError) error = NULL;
    (void) src;

    t->proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
    if (t->proxy == NULL) {
        g_message ("Wi-Fi : NetworkManager injoignable : %s", error->message);
        return;
    }
    g_signal_connect (t->proxy, "g-properties-changed",
                      G_CALLBACK (on_wifi_props), t);
    wifi_refresh (t);
}

static void
wifi_setup (Tile *t)
{
    t->iface = "org.freedesktop.NetworkManager";
    t->prop  = "WirelessEnabled";
    g_dbus_proxy_new_for_bus (
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager",
        t->iface, NULL, on_wifi_proxy, t);
}

/* --- Bluetooth : BlueZ --------------------------------------------------- */
static void
bt_refresh (Tile *t)
{
    tile_apply (t, proxy_bool (t->proxy, "Powered", FALSE), TRUE);
}

static void
on_bt_props (GDBusProxy *proxy, GVariant *changed,
             const char * const *invalidated, gpointer data)
{
    (void) proxy; (void) changed; (void) invalidated;
    bt_refresh (data);
}

/* BlueZ ne publie pas de chemin fixe : l'adaptateur est /org/bluez/hci0 la
 * plupart du temps, mais rien ne le garantit. On interroge le gestionnaire
 * d'objets et on prend le premier qui porte org.bluez.Adapter1. */
/* Appel direct plutot qu'un proxy : g_dbus_proxy_new_for_bus_sync n'accepte
 * AUCUN delai et attend les 25 secondes reglementaires quand le service
 * tarde. Mesure au banc d'essai : 25 s pendant lesquelles le shell entier
 * etait fige, sans rien afficher. Un appel direct, lui, se borne. */
static char *
bluez_adapter_path (void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus == NULL) {
        g_message ("Bluetooth : bus systeme injoignable : %s", error->message);
        return NULL;
    }

    g_autoptr(GVariant) reply = g_dbus_connection_call_sync (
        bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects", NULL, G_VARIANT_TYPE ("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (reply == NULL) {
        g_message ("Bluetooth : liste des objets illisible : %s", error->message);
        return NULL;
    }

    /* Attention au piege : g_variant_iter_loop libere lui-meme les valeurs
     * qu'il a posees, au debut de l'iteration suivante. Un g_autoptr sur
     * l'une d'elles les libererait une seconde fois. On reste donc sur des
     * pointeurs nus, et on ne libere a la main que si l'on sort en cours de
     * route -- ce que la boucle ne fait alors plus pour nous. */
    g_autoptr(GVariant) objects = g_variant_get_child_value (reply, 0);

    GVariantIter it;
    g_variant_iter_init (&it, objects);

    const char *path;
    GVariant   *ifaces;
    while (g_variant_iter_loop (&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        g_autoptr(GVariant) adapter =
            g_variant_lookup_value (ifaces, "org.bluez.Adapter1", NULL);
        if (adapter == NULL)
            continue;

        char *found = g_strdup (path);
        g_variant_unref (ifaces);      /* sortie anticipee : a nous de jouer */
        return found;
    }
    return NULL;
}

static void
on_bt_proxy (GObject *src, GAsyncResult *res, gpointer data)
{
    Tile *t = data;
    g_autoptr(GError) error = NULL;
    (void) src;

    t->proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
    if (t->proxy == NULL) {
        g_message ("Bluetooth : adaptateur injoignable : %s", error->message);
        return;
    }
    g_signal_connect (t->proxy, "g-properties-changed",
                      G_CALLBACK (on_bt_props), t);
    bt_refresh (t);
}

/* La recherche de l'adaptateur reste synchrone, mais differee au premier
 * affichage du panneau : au demarrage de la barre, elle attendrait pour
 * rien un service dont personne ne regarde encore l'etat. */
static void
bluetooth_setup (Tile *t)
{
    g_autofree char *path = bluez_adapter_path ();
    if (path == NULL)
        return;

    t->iface = "org.bluez.Adapter1";
    t->prop  = "Powered";
    g_dbus_proxy_new_for_bus (
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.bluez", path, t->iface, NULL, on_bt_proxy, t);
}

/* -------------------------------------------------------------------------
 * Batterie — consommation instantanee
 *
 * Deux conventions coexistent dans sysfs selon le pilote ACPI :
 *
 *   power_now                        en microwatts   (le plus simple)
 *   current_now x voltage_now        en microamperes x microvolts
 *
 * Ce Vivobook expose la seconde. Le produit vaut 10^12 fois des watts,
 * d'ou la division. Mesure de reference relevee sur la machine :
 * 0,247 A x 12,363 V = 3,05 W au repos, ecran allume.
 *
 * En double : le signe de current_now n'est pas normalise entre pilotes,
 * on prend la valeur absolue et c'est « status » qui dit le sens.
 * ------------------------------------------------------------------------- */
static gboolean
battery_watts (const char *dir, double *watts)
{
    g_autofree char *p = shell_sysfs_read (dir, "power_now");
    if (p != NULL) {
        *watts = ABS (g_ascii_strtod (p, NULL)) / 1e6;
        return TRUE;
    }

    g_autofree char *i = shell_sysfs_read (dir, "current_now");
    g_autofree char *u = shell_sysfs_read (dir, "voltage_now");
    if (i == NULL || u == NULL)
        return FALSE;

    *watts = ABS (g_ascii_strtod (i, NULL) * g_ascii_strtod (u, NULL)) / 1e12;
    return TRUE;
}

/* Autonomie restante, en heures. Ne vaut que batterie en decharge et
 * consommation non nulle : sinon le calcul divise par zero ou annonce une
 * duree qui n'a aucun sens. */
static gboolean
battery_hours (const char *dir, double watts, double *hours)
{
    if (watts <= 0.01)
        return FALSE;

    g_autofree char *e = shell_sysfs_read (dir, "energy_now");     /* µWh   */
    if (e != NULL) {
        *hours = g_ascii_strtod (e, NULL) / 1e6 / watts;
        return TRUE;
    }

    g_autofree char *c = shell_sysfs_read (dir, "charge_now");     /* µAh   */
    g_autofree char *u = shell_sysfs_read (dir, "voltage_now");    /* µV    */
    if (c == NULL || u == NULL)
        return FALSE;

    *hours = g_ascii_strtod (c, NULL) * g_ascii_strtod (u, NULL) / 1e12 / watts;
    return TRUE;
}

static void
battery_refresh (Panel *p)
{
    if (p->apercu) {
        gtk_label_set_text (GTK_LABEL (p->bat_pct), "78 %");
        gtk_label_set_text (GTK_LABEL (p->bat_detail),
                            "Sur batterie · 3,05 W · 9 h 12 restantes");
        gtk_image_set_from_icon_name (GTK_IMAGE (p->bat_icon),
                                      "battery-level-80-symbolic");
        return;
    }

    g_autofree char *dir = shell_battery_dir ();
    if (dir == NULL) {
        gtk_label_set_text (GTK_LABEL (p->bat_pct), "Secteur");
        gtk_label_set_text (GTK_LABEL (p->bat_detail), "Aucune batterie détectée");
        return;
    }

    g_autofree char *cap_s = shell_sysfs_read (dir, "capacity");
    g_autofree char *sta_s = shell_sysfs_read (dir, "status");
    int cap = cap_s ? atoi (cap_s) : 0;

    g_autofree char *pct = g_strdup_printf ("%d %%", cap);
    gtk_label_set_text (GTK_LABEL (p->bat_pct), pct);

    gboolean charging = (g_strcmp0 (sta_s, "Charging") == 0);
    gboolean full     = (g_strcmp0 (sta_s, "Full") == 0);
    int step = (cap + 5) / 10 * 10;
    if (step > 100) step = 100;
    g_autofree char *icon = g_strdup_printf ("battery-level-%d%s-symbolic", step,
                                             (charging || full) ? "-charging" : "");
    gtk_image_set_from_icon_name (GTK_IMAGE (p->bat_icon), icon);

    GString *detail = g_string_new (full     ? "Chargée"
                                  : charging ? "En charge"
                                             : "Sur batterie");
    double watts;
    if (battery_watts (dir, &watts) && watts > 0.005) {
        /* Deux decimales : au repos la machine tient autour de 3 W, et
         * une seule decimale masquerait justement les ecarts qu'on cherche
         * a observer quand on traque la consommation. */
        g_string_append_printf (detail, " · %.2f W", watts);

        double hours;
        if (!charging && !full && battery_hours (dir, watts, &hours))
            g_string_append_printf (detail, " · %d h %02d restantes",
                                    (int) hours, (int) ((hours - (int) hours) * 60));
    }
    gtk_label_set_text (GTK_LABEL (p->bat_detail), detail->str);
    g_string_free (detail, TRUE);
}

static gboolean
on_watt_tick (gpointer data)
{
    battery_refresh (data);
    return G_SOURCE_CONTINUE;
}

/* Suit les touches du clavier pendant que la Console est ouverte. Les deux
 * fonctions appelees se taisent d'elles-memes si l'utilisateur vient de
 * toucher au curseur correspondant — voir le gel dans console.c. */
static gboolean
on_suivi_tick (gpointer data)
{
    Panel *p = data;
    console_son_relire (p->son);
    console_lumiere_relire (p->lumiere);
    return G_SOURCE_CONTINUE;
}

/* -------------------------------------------------------------------------
 * Reglages
 *
 * Le panneau complet s'ouvre d'ici, et non depuis le dock : c'est le geste
 * de ChromeOS, et cela libere une place dans le dock pour ce qu'on ouvre
 * vraiment souvent. Wi-Fi, batterie et reglages se tiennent ainsi au meme
 * endroit.
 * ------------------------------------------------------------------------- */
static void
on_reglages (GtkButton *b, gpointer data)
{
    Panel *p = data;
    (void) b;

    /* Refermer d'abord : le panneau est une surface layer-shell posee
     * par-dessus tout, la fenetre de reglages s'ouvrirait derriere. */
    if (p->popover != NULL)
        gtk_popover_popdown (GTK_POPOVER (p->popover));

    g_autoptr(GError) error = NULL;
    g_autoptr(GDesktopAppInfo) info =
        g_desktop_app_info_new ("claude-os-reglages.desktop");

    if (info == NULL) {
        g_warning ("claude-os-reglages.desktop introuvable");
        return;
    }
    if (!g_app_info_launch (G_APP_INFO (info), NULL, NULL, &error))
        g_warning ("ouverture des réglages impossible : %s", error->message);
}

static GtkWidget *
reglages_build (Panel *p)
{
    GtkWidget *icone = gtk_image_new_from_icon_name ("preferences-system-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (icone), 18);
    gtk_widget_add_css_class (icone, "qs-tile-icon");

    GtkWidget *nom = gtk_label_new ("Réglages");
    gtk_widget_add_css_class (nom, "qs-tile-name");
    gtk_widget_set_halign (nom, GTK_ALIGN_START);
    gtk_widget_set_hexpand (nom, TRUE);

    GtkWidget *chevron = gtk_image_new_from_icon_name ("go-next-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (chevron), 14);
    gtk_widget_add_css_class (chevron, "qs-tile-state");

    GtkWidget *ligne = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append (GTK_BOX (ligne), icone);
    gtk_box_append (GTK_BOX (ligne), nom);
    gtk_box_append (GTK_BOX (ligne), chevron);

    GtkWidget *bouton = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (bouton), ligne);
    gtk_widget_add_css_class (bouton, "qs-reglages");
    g_signal_connect (bouton, "clicked", G_CALLBACK (on_reglages), p);
    return bouton;
}

/* -------------------------------------------------------------------------
 * Cycle de vie du panneau : la minuterie ne vit qu'entre l'ouverture et la
 * fermeture. C'est tout l'interet de n'afficher ces valeurs qu'au clic.
 * ------------------------------------------------------------------------- */
/* Le bouton sait quelle page ouvrir a la pastille qui le porte.
 *
 * La page ne remplace plus la Console : elle se deplie a sa gauche. Passer
 * d'un bouton a l'autre alors que la colonne est deja ouverte ne fait donc
 * que changer la page a l'interieur, sans replier ni deplier. */
static void
on_ouvrir_page (GtkButton *b, gpointer data)
{
    Panel *p = data;
    const char *voulue   = GTK_WIDGET (b) == p->wifi.button ? "wifi" : "bluetooth";
    const char *courante = gtk_stack_get_visible_child_name (p->pile);

    /* Le meme bouton ouvre ET referme. Un bouton qui se contentait d'ouvrir
     * laissait la fleche « retour » comme unique sortie : on cliquait a
     * nouveau la ou l'on venait de cliquer, il ne se passait rien, et le
     * geste evident semblait cassé. On repasse par « vide » plutot que de
     * replier directement : c'est on_page_changee qui replie, et cela
     * arrete du meme coup la decouverte Bluetooth. */
    if (gtk_revealer_get_reveal_child (GTK_REVEALER (p->reveleur))
        && g_strcmp0 (courante, voulue) == 0) {
        gtk_stack_set_visible_child_name (p->pile, "vide");
        return;
    }

    gtk_stack_set_visible_child_name (p->pile, voulue);
    gtk_revealer_set_reveal_child (GTK_REVEALER (p->reveleur), TRUE);
}

/* Un module qu'on eteint alors que son volet est ouvert : le volet se
 * replie, il n'aurait plus rien a montrer. */
static void
replier_si_ouverte (Panel *p, const char *page)
{
    if (p->pile == NULL)
        return;
    if (g_strcmp0 (gtk_stack_get_visible_child_name (p->pile), page) == 0)
        gtk_stack_set_visible_child_name (p->pile, "vide");
}

/* Ce qui tourne ne tourne QUE sur la page visible : balayage Wi-Fi a
 * l'ouverture, decouverte Bluetooth tant qu'on y reste. */
static void
on_page_changee (GObject *pile, GParamSpec *ps, gpointer data)
{
    Panel *p = data;
    (void) ps;

    const char *page = gtk_stack_get_visible_child_name (GTK_STACK (pile));

    if (g_strcmp0 (page, "wifi") == 0)
        wifi_page_ouverte (p->page_wifi);

    if (g_strcmp0 (page, "bluetooth") == 0)
        bluetooth_page_ouverte (p->page_bt);
    else
        bluetooth_page_fermee (p->page_bt);

    /* LE RETOUR ARRIERE PASSE PAR ICI, ET C'EST VOULU.
     *
     * Les fleches « retour » de wifi.c et bluetooth.c ne savent qu'une
     * chose : demander a la pile la page nommee a leur construction. En les
     * faisant pointer vers « vide », on replie la colonne sans toucher a une
     * ligne de ces deux fichiers — ils continuent de ne rien savoir de la
     * mise en page, ce qui est leur contrat. */
    if (g_strcmp0 (page, "vide") == 0)
        gtk_revealer_set_reveal_child (GTK_REVEALER (p->reveleur), FALSE);

    /* La colonne de detail est plus haute que la Console seule : la deplier
     * la fait grandir vers le haut, et le centre de notifications doit
     * suivre sous peine de se retrouver recouvert. */
    annoncer_hauteur (p);
}

static void
on_panel_show (GtkWidget *popover, gpointer data)
{
    Panel *p = data;
    (void) popover;

    /* Les services ne sont contactes qu'a la premiere ouverture du panneau.
     * Rien de tout cela n'interesse quelqu'un qui n'a pas encore clique. */
    if (!p->apercu && !p->services_sondes) {
        p->services_sondes = TRUE;
        wifi_setup (&p->wifi);
        bluetooth_setup (&p->bluetooth);
    }

    /* Le son et la luminosite changent par les touches du clavier et par les
     * applications, sans que la Console en soit avertie. Une lecture a
     * l'ouverture ne suffit donc pas : tant que le panneau reste ouvert, les
     * touches continuent d'agir et les curseurs restaient figes sur la valeur
     * qu'ils avaient au moment du clic. D'ou la lecture immediate ci-dessous,
     * puis la minuterie qui prend le relais jusqu'a la fermeture. */
    console_son_relire (p->son);
    console_lumiere_relire (p->lumiere);

    /* Le profil de veille, lui, ne bouge pas tout seul : il est relu ICI,
     * a l'ouverture, et PAS dans la minuterie de suivi. Relire shell.conf
     * deux fois par seconde pour un reglage que seul un clic modifie
     * serait exactement la scrutation que ce projet s'interdit. */
    console_energie_relire (p->energie);

    battery_refresh (p);
    if (p->watt_timer == 0)
        p->watt_timer = g_timeout_add (WATT_REFRESH_MS, on_watt_tick, p);
    if (!p->apercu && p->suivi_timer == 0)
        p->suivi_timer = g_timeout_add (SUIVI_REFRESH_MS, on_suivi_tick, p);

    p->ouverte = TRUE;
    annoncer_hauteur (p);
}

static void
on_panel_closed (GtkPopover *popover, gpointer data)
{
    Panel *p = data;
    (void) popover;

    /* Toujours rouvrir colonne repliee : retrouver le panneau la ou on
     * l'avait laisse trois heures plus tot serait deroutant. Et cela
     * garantit l'arret de la decouverte Bluetooth.
     *
     * Le repli est instantane, sans animation : la Console vient de
     * disparaitre, animer ce qu'on ne voit plus ne ferait que retarder le
     * retour a l'etat de repos. */
    bluetooth_page_fermee (p->page_bt);
    gtk_revealer_set_transition_duration (GTK_REVEALER (p->reveleur), 0);
    gtk_revealer_set_reveal_child (GTK_REVEALER (p->reveleur), FALSE);
    gtk_stack_set_visible_child_name (p->pile, "vide");
    gtk_revealer_set_transition_duration (GTK_REVEALER (p->reveleur),
                                          REVEAL_MS);

    if (p->watt_timer != 0) {
        g_source_remove (p->watt_timer);
        p->watt_timer = 0;
    }
    if (p->suivi_timer != 0) {
        g_source_remove (p->suivi_timer);
        p->suivi_timer = 0;
    }

    p->ouverte = FALSE;
    annoncer_hauteur (p);
}

static void
panel_free (gpointer data)
{
    Panel *p = data;
    g_clear_object (&p->wifi.proxy);
    g_clear_object (&p->bluetooth.proxy);
    g_free (p);
}

/* ------------------------------------------------------------------------- */
GtkWidget *
panel_new (gboolean apercu)
{
    Panel *p = g_new0 (Panel, 1);
    p->apercu = apercu;

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class (box, "qs");
    p->box_console = box;

    /* --- ce qu'on regle le plus souvent, donc en premier ---
     *
     * Le son et la luminosite sont les deux reglages qu'on vient chercher
     * plusieurs fois par jour. Ils sont en haut, atteignables sans lire le
     * reste, et ce sont les seuls elements de la Console qu'on manipule au
     * doigt plutot qu'au clic. */
    p->son = console_son_new (apercu);
    p->lumiere = console_lumiere_new (apercu);
    gtk_box_append (GTK_BOX (box), p->son);
    gtk_box_append (GTK_BOX (box), p->lumiere);

    /* --- bascules, cote a cote --- */
    GtkWidget *tiles = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous (GTK_BOX (tiles), TRUE);
    gtk_box_append (GTK_BOX (tiles),
                    tile_build (&p->wifi, p, "Wi-Fi", "wifi",
                                "network-wireless-signal-excellent-symbolic",
                                "network-wireless-offline-symbolic"));
    gtk_box_append (GTK_BOX (tiles),
                    tile_build (&p->bluetooth, p, "Bluetooth", "bluetooth",
                                "bluetooth-active-symbolic",
                                "bluetooth-disabled-symbolic"));
    gtk_box_append (GTK_BOX (box), tiles);

    /* --- carte batterie --- */
    p->bat_icon = gtk_image_new_from_icon_name ("battery-level-100-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (p->bat_icon), 24);
    gtk_widget_add_css_class (p->bat_icon, "qs-battery-icon");

    p->bat_pct = gtk_label_new ("-- %");
    gtk_widget_add_css_class (p->bat_pct, "qs-battery-pct");
    gtk_widget_set_halign (p->bat_pct, GTK_ALIGN_START);

    p->bat_detail = gtk_label_new ("");
    gtk_widget_add_css_class (p->bat_detail, "qs-battery-detail");
    gtk_widget_set_halign (p->bat_detail, GTK_ALIGN_START);

    GtkWidget *bat_texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (bat_texts), p->bat_pct);
    gtk_box_append (GTK_BOX (bat_texts), p->bat_detail);

    GtkWidget *card = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class (card, "qs-card");
    gtk_box_append (GTK_BOX (card), p->bat_icon);
    gtk_box_append (GTK_BOX (card), bat_texts);
    gtk_box_append (GTK_BOX (box), card);

    gtk_box_append (GTK_BOX (box), reglages_build (p));

    /* L'alimentation ferme la Console, en bas, apres un separateur : c'est
     * le seul endroit ou un clic ne se rattrape pas, et il ne doit pas se
     * trouver sur le chemin du pouce qui vise le volume. */
    GtkWidget *trait = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class (trait, "qs-trait");
    gtk_box_append (GTK_BOX (box), trait);

    if (apercu) {
        tile_apply (&p->wifi, TRUE, TRUE);
        tile_apply (&p->bluetooth, FALSE, TRUE);
    }
    battery_refresh (p);

    /* Les pages detaillees vivent dans le MEME popover : ouvrir une fenetre
     * separee pour choisir un reseau ferait perdre le fil, et obligerait a
     * gerer son placement.
     *
     * ELLES NE REMPLACENT PLUS LA CONSOLE, ELLES LA PROLONGENT.
     *
     * Auparavant la pile contenait aussi la page principale, et choisir un
     * reseau escamotait tout le reste : le volume, la batterie et l'heure
     * disparaissaient le temps de lire une liste de SSID. On perdait de vue
     * l'etat de la machine au moment precis ou l'on agit dessus.
     *
     * La pile ne contient donc plus que les pages de detail, et vit dans un
     * revelateur pose a GAUCHE de la Console, qui reste entiere a cote. La
     * page « vide » est le repos : une boite sans contenu, vers laquelle
     * pointent les fleches « retour » des deux pages. */
    GtkWidget *pile = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (pile),
                                   GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration (GTK_STACK (pile), 140);
    /* Sans cela la pile prend la hauteur de sa plus grande page, et la page
     * principale traine 240 px de vide sous la carte batterie. */
    gtk_stack_set_vhomogeneous (GTK_STACK (pile), FALSE);
    gtk_stack_set_hhomogeneous (GTK_STACK (pile), FALSE);
    gtk_stack_add_named (GTK_STACK (pile), gtk_box_new (GTK_ORIENTATION_VERTICAL, 0),
                         "vide");

    p->pile      = GTK_STACK (pile);
    p->page_wifi = wifi_page_new (p->pile, "vide", apercu);
    p->page_bt   = bluetooth_page_new (p->pile, "vide", apercu);
    gtk_stack_add_named (GTK_STACK (pile), p->page_wifi, "wifi");
    gtk_stack_add_named (GTK_STACK (pile), p->page_bt,   "bluetooth");
    gtk_stack_set_visible_child_name (GTK_STACK (pile), "vide");

    /* UNE LARGEUR, POUR LES DEUX PAGES.
     *
     * GTK place le popover d'apres la taille NATURELLE de son contenu. Les
     * deux pages n'ayant pas la meme — les libelles d'action du Bluetooth
     * sont plus longs que ceux du Wi-Fi — le popover se posait ailleurs
     * selon la page ouverte : mesure au banc, a taille de surface pourtant
     * identique (768x491), popup_x valait -545 sur le Wi-Fi et -618 sur le
     * Bluetooth. Soit 73 px de glissement vers la gauche, et une Console qui
     * n'etait plus alignee sur la barre d'etat.
     *
     * La demande de taille fixe la largeur minimale ; le plafond en
     * caracteres pose sur les noms, dans wifi.c et bluetooth.c, borne la
     * naturelle. Entre les deux, la colonne ne bouge plus. */
    gtk_widget_set_size_request (pile, LARGEUR_COLONNE_PX, -1);

    /* Le revelateur donne la largeur, pas la pile : replie il mesure zero,
     * et le popover reprend exactement la largeur de la Console seule. */
    p->reveleur = gtk_revealer_new ();
    gtk_revealer_set_child (GTK_REVEALER (p->reveleur), pile);
    /* SLIDE_RIGHT : le contenu entre par la gauche et glisse vers la
     * Console. Le popover etant aligne sur le bord droit de la barre, c'est
     * lui qui s'etend vers la gauche pendant que la colonne s'ouvre — le
     * bord droit, celui que l'oeil suit, ne bouge pas d'un pixel. */
    gtk_revealer_set_transition_type (GTK_REVEALER (p->reveleur),
                                      GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
    gtk_revealer_set_transition_duration (GTK_REVEALER (p->reveleur), REVEAL_MS);
    gtk_revealer_set_reveal_child (GTK_REVEALER (p->reveleur), FALSE);

    GtkWidget *rangee = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    p->rangee = rangee;
    gtk_box_append (GTK_BOX (rangee), p->reveleur);
    gtk_box_append (GTK_BOX (rangee), box);
    /* La colonne de detail est plus haute que la Console (une liste de
     * reseaux fait 240 px a elle seule). Sans cela elle etirerait la Console
     * a sa hauteur, et l'alimentation se retrouverait a flotter en bas. */
    gtk_widget_set_valign (box, GTK_ALIGN_START);
    gtk_widget_set_valign (p->reveleur, GTK_ALIGN_START);

    g_signal_connect (p->wifi.button, "clicked",
                      G_CALLBACK (on_ouvrir_page), p);
    g_signal_connect (p->bluetooth.button, "clicked",
                      G_CALLBACK (on_ouvrir_page), p);
    g_signal_connect (pile, "notify::visible-child-name",
                      G_CALLBACK (on_page_changee), p);

    GtkWidget *popover = gtk_popover_new ();
    p->popover = popover;
    gtk_popover_set_child (GTK_POPOVER (popover), rangee);
    gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
    gtk_widget_add_css_class (popover, "qs-popover");
    /* Aligne le panneau sur le bord droit de la barre plutot que sur son
     * centre : sinon il deborderait de l'ecran, la barre etant deja collee
     * au bord. */
    gtk_widget_set_halign (popover, GTK_ALIGN_END);
    /* DEGAGER LA BARRE D'ETAT.
     *
     * Sans decalage, GTK colle le bas du popover au haut du bouton qui
     * l'ouvre : mesure au banc d'essai, le popover finissait a y=1037 et la
     * barre commencait a y=1038. Zero pixel entre les deux, et l'ombre
     * portee de la Console — 12 px de decalage, 36 px de flou — retombait
     * en plein sur la barre, dont les coins arrondis semblaient coupes.
     *
     * 12 px, parce que c'est deja l'ecart que le dock et la barre gardent
     * avec le bord de l'ecran (« margin: 0 12px 12px 0 » dans shell.css).
     * La Console se pose donc sur la meme trame que le reste du bureau. */
    gtk_popover_set_offset (GTK_POPOVER (popover), 0, -PANEL_ECART_BARRE_PX);

    /* La veille juste avant l'alimentation : ce sont deux facons de gerer
     * la meme chose -- ce que la machine fait quand on ne s'en sert plus. */
    p->energie = console_energie_new (apercu);
    gtk_box_append (GTK_BOX (box), p->energie);

    gtk_box_append (GTK_BOX (box), console_alimentation_new (popover, apercu));

    g_signal_connect (popover, "show",   G_CALLBACK (on_panel_show),   p);
    g_signal_connect (popover, "closed", G_CALLBACK (on_panel_closed), p);
    g_object_set_data_full (G_OBJECT (popover), "panel", p, panel_free);

    return popover;
}
