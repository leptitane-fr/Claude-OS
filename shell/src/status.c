/* =========================================================================
 * Claude-OS Shell — le processus resident du bureau
 *
 * IL S'APPELAIT « barre d'etat », ET IL N'EN PORTE PLUS.
 *
 * Ce fichier dessinait une pilule opaque en bas a droite : heure, date,
 * reseau, batterie, mode de veille, et un clic qui ouvrait la Console. Le
 * 15 septembre 2026 la pilule a laisse la place au COIN (coin.c) -- des
 * traces clairs poses sur le fond d'ecran, permanents, insensibles au clic
 * -- et ce qu'on y reglait est passe dans le TIROIR du bord droit
 * (tiroir.c).
 *
 * Ce qui reste ici est ce qui n'a jamais ete de l'affichage : les SOURCES.
 * L'horloge, la batterie, NetworkManager, et la mise en route des modules
 * residents -- veille de l'ecran, surveillance de la charge, capot, centre
 * de notifications. Le coin ne lit rien lui-meme ; on lui dit.
 *
 * DISCIPLINE D'ENERGIE, inchangee et toujours le sujet principal :
 *
 *  - une SEULE minuterie, alignee sur la minute, met a jour l'heure ET la
 *    batterie. Un reveil par minute, pas un par seconde ni un par element.
 *  - le reseau ne consulte rien : il reagit aux signaux D-Bus de
 *    NetworkManager, donc uniquement quand l'etat change reellement.
 *  - aucune animation au repos.
 *
 * CE QUI A DISPARU AVEC LA BARRE, et qu'il ne faut pas chercher : la
 * glissiere qui la faisait sortir par le bas, les actions « afficher » et
 * « masquer » du bus, et le suivi du dock. Le coin ne s'absente jamais --
 * c'est ce qu'on lui demande --, il n'a donc rien a suivre.
 * ========================================================================= */

#include <gtk/gtk.h>

#include "config.h"
#include "notifications.h"
#include "energie.h"
#include "batterie.h"
#include "capot.h"
#include "panel.h"
#include "coin.h"
#include "toplevels.h"
#include "tiroir.h"
#include "sysfs.h"

#include <stdlib.h>            /* atoi */

typedef struct {
    Notifs *notifs;
} Status;

typedef struct {
    ShellConfig *cfg;
    gboolean     ouvrir;     /* ouvrir le tiroir au demarrage      */
    gboolean     apercu;
} Options;

/* -------------------------------------------------------------------------
 * Batterie — la lecture de sysfs est partagee avec le panneau (sysfs.c).
 * ------------------------------------------------------------------------- */
static void
battery_update (void)
{
    g_autofree char *dir = shell_battery_dir ();
    if (dir == NULL)
        return;

    g_autofree char *cap_s    = shell_sysfs_read (dir, "capacity");
    g_autofree char *status_s = shell_sysfs_read (dir, "status");
    if (cap_s == NULL)
        return;

    int cap = atoi (cap_s);
    gboolean charging = (g_strcmp0 (status_s, "Charging") == 0
                      || g_strcmp0 (status_s, "Full") == 0);

    /* Le theme d'icones fournit une famille battery-level-NNN : on arrondit
     * a la dizaine, ce que ces themes attendent. Les vingt-deux crans sont
     * engendres par tools/fabrique-icones.py -- un cran manquant retombe en
     * silence sur Papirus. */
    int step = (cap + 5) / 10 * 10;
    if (step > 100) step = 100;
    g_autofree char *icon = g_strdup_printf ("battery-level-%d%s-symbolic",
                                             step, charging ? "-charging" : "");

    /* LA FICHE SECTEUR N'EST PAS « EN CHARGE ». Une batterie pleine et
     * branchee rapporte « Full » et ne charge plus, mais la prise est bien
     * la : c'est shell_sur_secteur() qui fait foi pour le temoin, pas
     * l'etat de la cellule. */
    shell_coin_batterie (cap, shell_sur_secteur (), icon);
}

/* -------------------------------------------------------------------------
 * Horloge — minuterie alignee sur la minute
 * ------------------------------------------------------------------------- */
/* « mardi 15 septembre » : la forme LONGUE, contrairement a la barre qui
 * abregeait faute de place. Le coin n'a plus de pilule a faire tenir, et une
 * date en toutes lettres se lit sans etre dechiffree. « %-d » plutot que
 * « %e », qui pose une espace de chiffre devant les jours a un chiffre. Le
 * meme reveil que l'heure la met a jour -- minuit est une minute comme une
 * autre. */
static void
clock_update (void)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_local ();
    g_autofree char *heure = g_date_time_format (now, "%H:%M");
    g_autofree char *jour  = g_date_time_format (now, "%A %-d %B");
    shell_coin_horloge (heure, jour);
}

static gboolean on_minute (gpointer data);

/* Replanifie exactement sur la seconde 0 de la minute suivante. Une minuterie
 * de 60 s glisserait peu a peu et changerait l'affichage a contretemps. */
static void
schedule_next_minute (void)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_local ();
    guint delay_ms = (60 - g_date_time_get_second (now)) * 1000
                   - g_date_time_get_microsecond (now) / 1000;
    if (delay_ms < 500) delay_ms = 500;
    g_timeout_add (delay_ms, on_minute, NULL);
}

static gboolean
on_minute (gpointer data)
{
    (void) data;
    clock_update ();
    battery_update ();        /* meme reveil : rien de plus a payer */
    schedule_next_minute ();
    return G_SOURCE_REMOVE;   /* on se replanifie soi-meme */
}

/* -------------------------------------------------------------------------
 * Reseau — signaux D-Bus de NetworkManager
 *
 * Aucune consultation periodique : NetworkManager previent quand son etat
 * change, ce qui est exactement ce qu'on veut.
 * ------------------------------------------------------------------------- */
static void
network_apply_state (guint32 state)
{
    /* Etats NetworkManager : 70 = connecte au monde, 60 = connectivite
     * limitee, 50 = connexion locale, en dessous = deconnecte. */
    const char *icon = (state >= 70) ? "network-wireless-signal-excellent-symbolic"
                     : (state >= 50) ? "network-wireless-signal-weak-symbolic"
                                     : "network-wireless-offline-symbolic";
    shell_coin_reseau (icon);
}

static void
on_nm_properties (GDBusProxy *proxy, GVariant *changed,
                  const char * const *invalidated, gpointer data)
{
    (void) proxy; (void) invalidated; (void) data;
    g_autoptr(GVariant) v = g_variant_lookup_value (changed, "State",
                                                    G_VARIANT_TYPE_UINT32);
    if (v != NULL)
        network_apply_state (g_variant_get_uint32 (v));
}

/* Construction ASYNCHRONE. La variante synchrone n'accepte aucun delai et
 * attend les 25 secondes reglementaires si le service tarde. Mesure au banc
 * d'essai contre un faux service.
 *
 * En attendant la reponse, le coin affiche « hors ligne » : un temoin qui
 * n'affiche rien est plus deroutant qu'un temoin qui annonce une absence. */
static void
on_nm_proxy (GObject *src, GAsyncResult *res, gpointer data)
{
    g_autoptr(GError) error = NULL;
    (void) src; (void) data;

    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
    if (proxy == NULL) {
        g_message ("NetworkManager injoignable : %s", error->message);
        return;
    }

    g_autoptr(GVariant) state = g_dbus_proxy_get_cached_property (proxy, "State");
    network_apply_state (state ? g_variant_get_uint32 (state) : 0);

    g_signal_connect (proxy, "g-properties-changed",
                      G_CALLBACK (on_nm_properties), NULL);
}

static void
network_setup (void)
{
    network_apply_state (0);

    g_dbus_proxy_new_for_bus (
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager",
        NULL, on_nm_proxy, NULL);
}

/* ------------------------------------------------------------------------- */
/* La batterie vient d'etre relue -- par l'intervalle calcule, ou par
 * l'uevent d'un cable qu'on branche. Dans le second cas, c'est ce rappel
 * qui evite d'attendre la minute suivante pour voir la fiche apparaitre. */
static void
on_batterie_lue (gpointer data)
{
    (void) data;
    battery_update ();
}

static void
on_non_lu (gboolean il_y_en_a, gpointer data)
{
    (void) data;
    shell_coin_non_lu (il_y_en_a);
}

static void
on_config_reloaded (ShellConfig *cfg, gpointer data)
{
    (void) data;
    shell_styles_load (cfg);
    shell_config_apply (cfg);
    /* Le mode de veille vient peut-etre d'etre change dans la Console : le
     * coin le montre en grand, il doit suivre. */
    shell_coin_mode (cfg);
    shell_coin_apparence (cfg);
    /* Avant de liberer : le panneau de reglages ecrit shell.conf, et les
     * delais de veille doivent suivre sans qu'on relance quoi que ce soit. */
    shell_energie_reconfigurer (cfg);
    shell_batterie_reconfigurer (cfg);
    shell_capot_reconfigurer (cfg);
    shell_config_free (cfg);
}

/* LE COIN S'EFFACE SOUS UNE FENETRE PLEIN ECRAN.
 *
 * Le dock, lui, decide de sa visibilite d'apres la fenetre ACTIVE
 * (visibility.h) ; le coin n'a pas cette finesse a avoir. La question qu'il
 * pose est plus simple -- « quelque chose couvre-t-il l'ecran » -- et la
 * reponse tient dans toplevels.c, qui ne consulte rien : le compositeur
 * previent quand un etat change, et se tait le reste du temps. */
static void
on_fenetres_changees (gpointer data)
{
    (void) data;
    shell_coin_plein_ecran (shell_toplevels_plein_ecran ());
}

/* Banc d'essai seulement. */
static gboolean
ouvrir_tiroir_une_fois (gpointer data)
{
    (void) data;
    shell_tiroir_ouvrir ();
    return G_SOURCE_REMOVE;
}

static void
on_activate (GtkApplication *app, gpointer user_data)
{
    Options *opt = user_data;
    Status *st = g_new0 (Status, 1);

    /* Meme configuration que le dock, et appliquee au meme moment : sans
     * cela, « theme=dark » dans shell.conf donnait un dock sombre et un
     * coin clair cote a cote. */
    shell_config_apply (opt->cfg);

    /* LE COIN D'ABORD : le tiroir et le centre de notifications s'y
     * accrochent, et l'horloge ecrit dedans des la premiere image. */
    shell_coin_init (app, opt->cfg, opt->apercu);

    /* LA CONSOLE VIT DANS LE TIROIR, et c'est le tiroir qu'elle referme
     * avant d'eteindre ou d'ouvrir les Reglages -- d'ou le rappel passe en
     * argument plutot qu'un widget. */
    GtkWidget *console = panel_new (opt->apercu, shell_tiroir_fermer, NULL);
    shell_tiroir_init (app, console, opt->apercu);

    /* LE CENTRE DE NOTIFICATIONS RESTE LE SERVEUR DU BUS, et sa banniere
     * s'accroche desormais au coin.
     *
     * CE QUI N'EST PLUS BRANCHE, ET C'EST UN CHOIX : le centre lui-meme n'a
     * plus d'entree. La cloche de la barre l'ouvrait ; le temoin du coin,
     * lui, ne s'ouvre pas -- le coin ne recoit aucun clic. L'historique des
     * notifications attend donc qu'un widget du volet haut du tiroir lui
     * redonne une porte. La banniere, elle, continue d'annoncer ce qui
     * arrive : c'est la moitie qu'on ne pouvait pas perdre.
     *
     * Ni notifs_nappe() ni notifs_suivre_barre() : la premiere servait a
     * laisser le coin de la barre cliquable sous la nappe du centre, la
     * seconde a attendre que la barre soit remontee. Le coin ne se clique
     * pas et ne descend jamais. */
    st->notifs = notifs_new (opt->apercu);
    notifs_ancrer (st->notifs, shell_coin_ancre ());
    notifs_sur_non_lu (st->notifs, on_non_lu, NULL);

    /* Heure et batterie AVANT la premiere image : le coin se pose
     * directement a sa taille, sans grandir sous les yeux. */
    clock_update ();

    g_application_hold (G_APPLICATION (app));
    shell_config_watch (on_config_reloaded, NULL);

    shell_energie_init (opt->cfg);
    shell_batterie_sur_lecture (on_batterie_lue, NULL);
    shell_batterie_init (opt->cfg);
    shell_capot_init (opt->cfg);

    if (opt->ouvrir)
        g_idle_add (ouvrir_tiroir_une_fois, NULL);

    /* APRES gtk_window_present du coin : la connexion Wayland de GTK doit
     * deja exister pour que le registre reponde. */
    if (!opt->apercu)
        shell_toplevels_init (on_fenetres_changees, NULL);

    battery_update ();
    network_setup ();
    schedule_next_minute ();
}

int
main (int argc, char **argv)
{
    /* --ouvrir : ouvre le tiroir au demarrage, avec les vraies sources.
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
