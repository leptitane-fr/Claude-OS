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
 *  - aucune animation, aucun rafraichissement au repos.
 * ========================================================================= */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

#define BAT_LOW_PERCENT 20      /* seuil d'alerte visuelle                   */

typedef struct {
    GtkWidget *clock;
    GtkWidget *bat_icon;
    GtkWidget *bat_level;
    GtkWidget *net_icon;
} Status;

/* -------------------------------------------------------------------------
 * Batterie — lecture directe de sysfs
 *
 * Pas de dependance a UPower : un demon de plus en memoire pour une valeur
 * qui tient dans deux fichiers texte ne se justifie pas ici.
 * ------------------------------------------------------------------------- */
static char *
sysfs_read (const char *dir, const char *file)
{
    g_autofree char *path = g_build_filename (dir, file, NULL);
    char *content = NULL;
    if (!g_file_get_contents (path, &content, NULL, NULL))
        return NULL;
    return g_strstrip (content);
}

/* Trouve la premiere batterie. Le nom varie selon les machines : BAT0 sur
 * beaucoup de portables, BAT1 sur ce Vivobook, BATC ailleurs. */
static char *
battery_dir (void)
{
    const char *base = "/sys/class/power_supply";
    g_autoptr(GDir) dir = g_dir_open (base, 0, NULL);
    if (dir == NULL)
        return NULL;

    const char *name;
    while ((name = g_dir_read_name (dir)) != NULL) {
        if (!g_str_has_prefix (name, "BAT"))
            continue;
        return g_build_filename (base, name, NULL);
    }
    return NULL;
}

static void
battery_update (Status *st)
{
    g_autofree char *dir = battery_dir ();
    if (dir == NULL) {
        gtk_widget_set_visible (st->bat_icon, FALSE);
        gtk_widget_set_visible (st->bat_level, FALSE);
        return;
    }

    g_autofree char *cap_s    = sysfs_read (dir, "capacity");
    g_autofree char *status_s = sysfs_read (dir, "status");
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
static void
clock_update (Status *st)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_local ();
    g_autofree char *text = g_date_time_format (now, "%H:%M");
    gtk_label_set_text (GTK_LABEL (st->clock), text);
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

static void
network_setup (Status *st)
{
    g_autoptr(GError) error = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync (
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager",
        NULL, &error);

    if (proxy == NULL) {
        /* NetworkManager absent : on affiche « hors ligne » plutot que rien.
         * Un composant d'etat qui disparait est plus deroutant qu'un
         * composant qui annonce une absence. */
        g_message ("NetworkManager injoignable : %s", error->message);
        network_apply_state (st, 0);
        return;
    }

    g_autoptr(GVariant) state = g_dbus_proxy_get_cached_property (proxy, "State");
    network_apply_state (st, state ? g_variant_get_uint32 (state) : 0);

    g_signal_connect (proxy, "g-properties-changed",
                      G_CALLBACK (on_nm_properties), st);
}

/* -------------------------------------------------------------------------
 * Styles et fenetre
 * ------------------------------------------------------------------------- */
static void
load_styles (void)
{
    const char *files[] = { "style/tokens.css", "style/shell.css" };
    for (guint i = 0; i < G_N_ELEMENTS (files); i++) {
        GtkCssProvider *p = gtk_css_provider_new ();
        g_autofree char *path = g_build_filename (SHELL_DATA_DIR, files[i], NULL);
        gtk_css_provider_load_from_path (p, path);
        gtk_style_context_add_provider_for_display (
            gdk_display_get_default (), GTK_STYLE_PROVIDER (p),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref (p);
    }
}

static GtkWidget *
icon (const char *name, int size)
{
    GtkWidget *img = gtk_image_new_from_icon_name (name);
    gtk_image_set_pixel_size (GTK_IMAGE (img), size);
    gtk_widget_add_css_class (img, "status-item");
    return img;
}

static void
on_activate (GtkApplication *app, gpointer user_data)
{
    gboolean dark = GPOINTER_TO_INT (user_data);
    Status *st = g_new0 (Status, 1);

    GtkWidget *window = gtk_application_window_new (app);
    gtk_widget_add_css_class (window, "shell");
    if (dark)
        gtk_widget_add_css_class (window, "dark");

    gtk_layer_init_for_window (GTK_WINDOW (window));
    gtk_layer_set_layer (GTK_WINDOW (window), GTK_LAYER_SHELL_LAYER_TOP);
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
    gtk_layer_set_keyboard_mode (GTK_WINDOW (window),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (bar, "status");
    gtk_widget_set_halign (bar, GTK_ALIGN_END);
    gtk_widget_set_valign (bar, GTK_ALIGN_END);

    st->net_icon  = icon ("network-offline-symbolic", 16);
    st->bat_icon  = icon ("battery-level-100-symbolic", 16);
    st->bat_level = gtk_label_new ("--%");
    gtk_widget_add_css_class (st->bat_level, "status-battery-level");
    st->clock     = gtk_label_new ("--:--");
    gtk_widget_add_css_class (st->clock, "status-item");
    gtk_widget_add_css_class (st->clock, "status-clock");

    GtkWidget *sep = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (sep, "status-sep");

    gtk_box_append (GTK_BOX (bar), st->net_icon);
    gtk_box_append (GTK_BOX (bar), st->bat_icon);
    gtk_box_append (GTK_BOX (bar), st->bat_level);
    gtk_box_append (GTK_BOX (bar), sep);
    gtk_box_append (GTK_BOX (bar), st->clock);

    gtk_window_set_child (GTK_WINDOW (window), bar);
    gtk_window_present (GTK_WINDOW (window));

    clock_update (st);
    battery_update (st);
    network_setup (st);
    schedule_next_minute (st);
}

int
main (int argc, char **argv)
{
    gboolean dark = FALSE;
    for (int i = 1; i < argc; i++)
        if (g_strcmp0 (argv[i], "--dark") == 0)
            dark = TRUE;

    GtkApplication *app = gtk_application_new ("os.claude.shell.status",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "startup",  G_CALLBACK (load_styles), NULL);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate),
                      GINT_TO_POINTER (dark));
    int status = g_application_run (G_APPLICATION (app), 0, NULL);
    g_object_unref (app);
    return status;
}
