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
 * Un clic ne bascule pas l'affichage : il demande le changement, et
 * l'affichage suit le signal renvoye par le service. Si polkit refuse, ou
 * si le materiel est bloque par un interrupteur physique, la bascule reste
 * visiblement dans son etat reel au lieu de mentir.
 * ========================================================================= */

#include "panel.h"
#include "bluetooth.h"
#include "sysfs.h"
#include "wifi.h"

#include <stdlib.h>            /* atoi */

#define WATT_REFRESH_MS 2000    /* uniquement panneau ouvert                 */

/* ------------------------------------------------------------------------- */

typedef struct {
    GtkWidget  *button;
    GtkWidget  *chevron;
    GtkWidget  *icon;
    GtkWidget  *state;          /* libelle secondaire : « Active » / ...     */
    const char *icon_on;
    const char *icon_off;
    gboolean    on;

    GDBusProxy *proxy;          /* NULL si le service est absent             */
    const char *iface;          /* interface portant la propriete            */
    const char *prop;
} Tile;

typedef struct {
    GtkStack  *pile;
    GtkWidget *page_wifi;
    GtkWidget *page_bt;
    Tile       wifi;
    Tile       bluetooth;
    GtkWidget *bat_pct;
    GtkWidget *bat_detail;
    GtkWidget *bat_icon;
    guint      watt_timer;      /* 0 quand le panneau est ferme              */
    gboolean   services_sondes; /* NetworkManager et BlueZ deja contactes ?  */
    gboolean   apercu;
} Panel;

/* -------------------------------------------------------------------------
 * Bascules
 * ------------------------------------------------------------------------- */
static void
tile_apply (Tile *t, gboolean on, gboolean available)
{
    t->on = on;
    gtk_image_set_from_icon_name (GTK_IMAGE (t->icon), on ? t->icon_on : t->icon_off);
    gtk_label_set_text (GTK_LABEL (t->state),
                        !available ? "Indisponible" : on ? "Activé" : "Désactivé");

    if (on)
        gtk_widget_add_css_class (t->button, "on");
    else
        gtk_widget_remove_css_class (t->button, "on");

    gtk_widget_set_sensitive (t->button, available);
}

/* Ecrit la propriete via org.freedesktop.DBus.Properties.Set.
 *
 * En asynchrone : un appel bloquant sur le bus systeme gele l'interface si
 * le service tarde, et polkit peut prendre plusieurs centaines de
 * millisecondes pour trancher. */
static void
on_set_done (GObject *src, GAsyncResult *res, gpointer data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (src), res, &error);
    if (reply == NULL)
        g_message ("bascule refusee : %s", error->message);
    (void) data;
}

static void
on_tile_clicked (GtkButton *button, gpointer data)
{
    Tile *t = data;
    (void) button;

    if (t->proxy == NULL)
        return;

    g_dbus_proxy_call (t->proxy,
                       "org.freedesktop.DBus.Properties.Set",
                       g_variant_new ("(ssv)", t->iface, t->prop,
                                      g_variant_new_boolean (!t->on)),
                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                       on_set_done, NULL);
    /* Volontairement, on ne change rien a l'ecran ici : c'est le signal de
     * changement de propriete qui fera foi. */
}

static GtkWidget *
tile_build (Tile *t, const char *name, const char *icon_on, const char *icon_off)
{
    t->icon_on  = icon_on;
    t->icon_off = icon_off;

    t->icon = gtk_image_new_from_icon_name (icon_off);
    gtk_image_set_pixel_size (GTK_IMAGE (t->icon), 20);
    gtk_widget_add_css_class (t->icon, "qs-tile-icon");

    GtkWidget *title = gtk_label_new (name);
    gtk_widget_add_css_class (title, "qs-tile-name");
    gtk_widget_set_halign (title, GTK_ALIGN_START);

    t->state = gtk_label_new ("Indisponible");
    gtk_widget_add_css_class (t->state, "qs-tile-state");
    gtk_widget_set_halign (t->state, GTK_ALIGN_START);

    GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (texts), title);
    gtk_box_append (GTK_BOX (texts), t->state);

    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append (GTK_BOX (row), t->icon);
    gtk_box_append (GTK_BOX (row), texts);

    t->button = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (t->button), row);
    gtk_widget_add_css_class (t->button, "qs-tile");
    gtk_widget_set_hexpand (t->button, TRUE);
    gtk_widget_set_sensitive (t->button, FALSE);
    g_signal_connect (t->button, "clicked", G_CALLBACK (on_tile_clicked), t);

    /* Deux gestes distincts sur une meme pastille, comme sur ChromeOS : le
     * corps allume et eteint, le chevron ouvre la liste. Un seul bouton qui
     * ferait les deux obligerait a choisir entre les deux usages. */
    t->chevron = gtk_button_new_from_icon_name ("go-next-symbolic");
    gtk_widget_add_css_class (t->chevron, "qs-chevron");
    gtk_widget_set_valign (t->chevron, GTK_ALIGN_CENTER);

    GtkWidget *paire = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (paire, "qs-tile-paire");
    gtk_widget_set_hexpand (paire, TRUE);
    gtk_box_append (GTK_BOX (paire), t->button);
    gtk_box_append (GTK_BOX (paire), t->chevron);
    return paire;
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

/* -------------------------------------------------------------------------
 * Cycle de vie du panneau : la minuterie ne vit qu'entre l'ouverture et la
 * fermeture. C'est tout l'interet de n'afficher ces valeurs qu'au clic.
 * ------------------------------------------------------------------------- */
/* Le chevron sait quelle page ouvrir a la pastille qui le porte. */
static void
on_ouvrir_page (GtkButton *b, gpointer data)
{
    Panel *p = data;
    gtk_stack_set_visible_child_name (
        p->pile,
        GTK_WIDGET (b) == p->wifi.chevron ? "wifi" : "bluetooth");
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

    battery_refresh (p);
    if (p->watt_timer == 0)
        p->watt_timer = g_timeout_add (WATT_REFRESH_MS, on_watt_tick, p);
}

static void
on_panel_closed (GtkPopover *popover, gpointer data)
{
    Panel *p = data;
    (void) popover;

    /* Toujours rouvrir sur la page principale : retrouver le panneau la ou
     * on l'avait laisse trois heures plus tot serait deroutant. Et cela
     * garantit l'arret de la decouverte Bluetooth. */
    bluetooth_page_fermee (p->page_bt);
    gtk_stack_set_visible_child_name (p->pile, "principal");

    if (p->watt_timer != 0) {
        g_source_remove (p->watt_timer);
        p->watt_timer = 0;
    }
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

    /* --- bascules, cote a cote --- */
    GtkWidget *tiles = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append (GTK_BOX (tiles),
                    tile_build (&p->wifi, "Wi-Fi",
                                "network-wireless-signal-excellent-symbolic",
                                "network-wireless-offline-symbolic"));
    gtk_box_append (GTK_BOX (tiles),
                    tile_build (&p->bluetooth, "Bluetooth",
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

    if (apercu) {
        tile_apply (&p->wifi, TRUE, TRUE);
        tile_apply (&p->bluetooth, FALSE, TRUE);
    }
    battery_refresh (p);

    /* Les pages detaillees vivent dans le MEME popover : ouvrir une fenetre
     * separee pour choisir un reseau ferait perdre le fil, et obligerait a
     * gerer son placement. */
    GtkWidget *pile = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (pile),
                                   GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration (GTK_STACK (pile), 140);
    /* Sans cela la pile prend la hauteur de sa plus grande page, et la page
     * principale traine 240 px de vide sous la carte batterie. */
    gtk_stack_set_vhomogeneous (GTK_STACK (pile), FALSE);
    gtk_stack_set_hhomogeneous (GTK_STACK (pile), FALSE);
    gtk_stack_add_named (GTK_STACK (pile), box, "principal");

    p->pile      = GTK_STACK (pile);
    p->page_wifi = wifi_page_new (p->pile, "principal", apercu);
    p->page_bt   = bluetooth_page_new (p->pile, "principal", apercu);
    gtk_stack_add_named (GTK_STACK (pile), p->page_wifi, "wifi");
    gtk_stack_add_named (GTK_STACK (pile), p->page_bt,   "bluetooth");

    g_signal_connect (p->wifi.chevron, "clicked",
                      G_CALLBACK (on_ouvrir_page), p);
    g_signal_connect (p->bluetooth.chevron, "clicked",
                      G_CALLBACK (on_ouvrir_page), p);
    g_signal_connect (pile, "notify::visible-child-name",
                      G_CALLBACK (on_page_changee), p);

    GtkWidget *popover = gtk_popover_new ();
    gtk_popover_set_child (GTK_POPOVER (popover), pile);
    gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
    gtk_widget_add_css_class (popover, "qs-popover");
    /* Aligne le panneau sur le bord droit de la barre plutot que sur son
     * centre : sinon il deborderait de l'ecran, la barre etant deja collee
     * au bord. */
    gtk_widget_set_halign (popover, GTK_ALIGN_END);

    g_signal_connect (popover, "show",   G_CALLBACK (on_panel_show),   p);
    g_signal_connect (popover, "closed", G_CALLBACK (on_panel_closed), p);
    g_object_set_data_full (G_OBJECT (popover), "panel", p, panel_free);

    return popover;
}
