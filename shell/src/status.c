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

#include "config.h"
#include "visibility.h"
#include "panel.h"
#include "sysfs.h"

#include <stdlib.h>            /* atoi */

#define BAT_LOW_PERCENT 20      /* seuil d'alerte visuelle                   */

typedef struct {
    GtkWidget *clock;
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

typedef struct {
    ShellConfig *cfg;   /* police, theme d'icones, clair ou sombre           */
    gboolean apercu;    /* valeurs fixes dans le panneau                     */
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
on_config_reloaded (ShellConfig *cfg, gpointer window)
{
    (void) window;
    shell_styles_load (cfg->theme);
    shell_config_apply (cfg);
    shell_config_free (cfg);
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
    /* ON_DEMAND, et pas NONE : le panneau de reglages doit pouvoir prendre
     * le clavier quand il s'ouvre, ne serait-ce que pour se fermer sur
     * Echap et pour rendre ses bascules atteignables au clavier. Hors
     * ouverture du panneau, la barre ne reclame rien. */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (window),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

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

    /* Toute la pilule est un bouton : c'est elle qu'on vise, pas une
     * poignee dediee. Le cadre par defaut de GtkMenuButton est retire, la
     * surface visible reste celle dessinee par .status. */
    GtkWidget *button = gtk_menu_button_new ();
    gtk_menu_button_set_has_frame (GTK_MENU_BUTTON (button), FALSE);
    gtk_menu_button_set_child (GTK_MENU_BUTTON (button), bar);
    gtk_menu_button_set_direction (GTK_MENU_BUTTON (button), GTK_ARROW_UP);
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), panel_new (opt->apercu));
    gtk_widget_add_css_class (button, "status");
    gtk_widget_set_halign (button, GTK_ALIGN_END);
    gtk_widget_set_valign (button, GTK_ALIGN_END);

    gtk_window_set_child (GTK_WINDOW (window), button);
    gtk_window_present (GTK_WINDOW (window));

    g_action_map_add_action_entries (G_ACTION_MAP (app), actions,
                                     G_N_ELEMENTS (actions), NULL);
    g_application_hold (G_APPLICATION (app));
    shell_visibility_init (on_visibilite, window);
    shell_config_watch (on_config_reloaded, window);

    if (opt->ouvrir)
        g_idle_add (open_panel_once, button);

    clock_update (st);
    battery_update (st);
    network_setup (st);
    schedule_next_minute (st);
}

int
main (int argc, char **argv)
{
    /* --ouvrir : ouvre le panneau au demarrage, avec les vraies sources.
     * --apercu : idem, mais avec des valeurs fixes, pour juger la mise en
     *            page quand aucun service n'est present. Une aide au banc
     *            d'essai, qui ne prouve rien du branchement D-Bus. */
    Options opt = { shell_config_load (), FALSE, FALSE };

    /* Les options de ligne de commande priment sur le fichier : pratique
     * pour essayer un theme sans toucher a sa configuration. */
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0 (argv[i], "--dark") == 0)   opt.cfg->dark = TRUE;
        if (g_strcmp0 (argv[i], "--light") == 0)  opt.cfg->dark = FALSE;
        if (g_strcmp0 (argv[i], "--ouvrir") == 0) opt.ouvrir = TRUE;
        if (g_strcmp0 (argv[i], "--apercu") == 0) opt.apercu = opt.ouvrir = TRUE;
    }

    GtkApplication *app = gtk_application_new ("os.claude.shell.status",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "startup",  G_CALLBACK (shell_styles_startup), opt.cfg);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), &opt);
    int status = g_application_run (G_APPLICATION (app), 0, NULL);
    g_object_unref (app);
    return status;
}
