#include "energie.h"
#include "retroeclairage.h"
#include "sysfs.h"

#include <gtk/gtk.h>
#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>

#include <string.h>            /* strcmp */

#include "ext-idle-notify-v1-client-protocol.h"

typedef enum { ATTENUER = 0, ETEINDRE = 1, SUSPENDRE = 2, ETAGES = 3 } Etage;

static struct {
    struct ext_idle_notifier_v1     *notifier;
    struct wl_seat                  *seat;
    struct ext_idle_notification_v1 *notifs[ETAGES];

    int      delais[ETAGES];      /* secondes ; 0 = etage ferme            */
    int      niveau;              /* pourcent de l'etage « attenuer »      */
    gboolean suspendre_permis;
    gboolean actif;
    gboolean profil_secteur;      /* profil en vigueur, pas l'etat reel    */

    /* Luminosite d'avant l'attenuation, a restaurer. -1 : pas attenue.
     * Elle est relue dans sysfs plutot que gardee de la fois precedente :
     * l'utilisateur a pu bouger le curseur ou les touches entre-temps. */
    int      avant;
} E;

/* -------------------------------------------------------------------------
 * Les trois signaux
 *
 * Lus UNE FOIS, au moment ou un etage va se declencher. Jamais en boucle :
 * un detecteur qui scrute en permanence consommerait ce qu'on economise.
 * ------------------------------------------------------------------------- */

/* Un flux audio est-il en lecture ?
 *
 * De la musique sans fenetre video ne pose aucun inhibiteur -- le protocole
 * ne voit rien, et l'ecran s'eteindrait au milieu d'un morceau. Le noyau,
 * lui, le dit : le sous-flux passe a « RUNNING ». */
static gboolean
son_en_lecture (void)
{
    g_autoptr(GDir) cartes = g_dir_open ("/proc/asound", 0, NULL);
    if (cartes == NULL)
        return FALSE;

    const char *carte;
    while ((carte = g_dir_read_name (cartes)) != NULL) {
        if (!g_str_has_prefix (carte, "card"))
            continue;
        g_autofree char *dcarte = g_build_filename ("/proc/asound", carte, NULL);
        g_autoptr(GDir) pcms = g_dir_open (dcarte, 0, NULL);
        if (pcms == NULL)
            continue;

        const char *pcm;
        while ((pcm = g_dir_read_name (pcms)) != NULL) {
            /* « p » comme playback : une capture ouverte n'est pas une
             * raison de garder l'ecran allume. */
            if (!g_str_has_prefix (pcm, "pcm") || !g_str_has_suffix (pcm, "p"))
                continue;
            g_autofree char *st = g_build_filename (dcarte, pcm, "sub0",
                                                    "status", NULL);
            g_autofree char *txt = NULL;
            if (!g_file_get_contents (st, &txt, NULL, NULL))
                continue;
            if (strstr (txt, "RUNNING") != NULL)
                return TRUE;
        }
    }
    return FALSE;
}

/* La machine travaille-t-elle ?
 *
 * Une compilation ou un telechargement ne doit pas etre interrompu par une
 * suspension. Le seuil est volontairement haut : sur quatre coeurs, une
 * charge de 1,0 signifie qu'un coeur entier est occupe en continu, ce qui
 * n'arrive pas quand la machine ne fait qu'afficher une page. */
static gboolean
machine_occupee (void)
{
    g_autofree char *txt = NULL;
    if (!g_file_get_contents ("/proc/loadavg", &txt, NULL, NULL))
        return FALSE;
    return g_ascii_strtod (txt, NULL) >= 1.0;
}

/* -------------------------------------------------------------------------
 * Les etages
 * ------------------------------------------------------------------------- */

static void
suspendre_la_machine (void)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) bus =
        g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (bus == NULL) {
        g_message ("energie : bus systeme injoignable — %s", err->message);
        return;
    }
    /* FALSE : on ne force pas. logind interroge les inhibiteurs, et une
     * application qui a demande a ne pas etre interrompue l'emporte. */
    g_dbus_connection_call (bus, "org.freedesktop.login1",
                            "/org/freedesktop/login1",
                            "org.freedesktop.login1.Manager", "Suspend",
                            g_variant_new ("(b)", FALSE),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
on_idled (void *data, struct ext_idle_notification_v1 *n)
{
    (void) n;
    Etage e = (Etage) GPOINTER_TO_INT (data);
    g_message ("energie : etage %d atteint", (int) e);

    switch (e) {
    case ATTENUER:
        /* On note la luminosite AVANT de la baisser, et une seule fois :
         * un second passage enregistrerait la valeur attenuee et la
         * « restauration » laisserait l'ecran sombre. */
        if (E.avant < 0) {
            E.avant = shell_retro_lire ();
            if (E.avant < 0)
                g_message ("energie : luminosite illisible, etage sans effet");
            else if (E.avant <= E.niveau)
                E.avant = -1;   /* deja plus sombre que la cible : ne rien faire */
            else
                shell_retro_ecrire (E.niveau);
        }
        break;

    case ETEINDRE:
        if (son_en_lecture ())
            break;
        if (E.avant < 0)
            E.avant = shell_retro_lire ();   /* etage 1 ferme : noter ici */
        shell_retro_ecrire (0);
        break;

    case SUSPENDRE:
        if (!E.suspendre_permis)
            break;
        if (son_en_lecture () || machine_occupee ())
            break;
        suspendre_la_machine ();
        break;

    default:
        break;
    }
}

static void reconstruire (void);

static void
on_resumed (void *data, struct ext_idle_notification_v1 *n)
{
    (void) data; (void) n;

    /* Chaque etage deja inactif emet son « resumed » : ce rappel arrive
     * plusieurs fois pour une seule touche pressee. Le garde ci-dessous le
     * rend idempotent. */
    if (E.avant >= 0) {
        shell_retro_ecrire (E.avant);
        E.avant = -1;
    }

    /* La source d'alimentation est reevaluee ICI, et nulle part ailleurs.
     *
     * Surveiller /sys/class/power_supply demanderait une scrutation, et
     * UPower un demon de plus -- sysfs.h explique pourquoi ce projet s'en
     * passe. Or « resumed » arrive a chaque interaction : en pratique le
     * profil est rafraichi en permanence, pour zero cout.
     *
     * Consequence assumee : brancher le secteur pendant que la machine est
     * deja inactive ne change rien avant la prochaine interaction. */
    if (shell_sur_secteur () != E.profil_secteur)
        reconstruire ();
}

static const struct ext_idle_notification_v1_listener ecouteur = {
    on_idled, on_resumed,
};

/* -------------------------------------------------------------------------
 * Construction des minuteries
 * ------------------------------------------------------------------------- */

static void
reconstruire (void)
{
    for (int i = 0; i < ETAGES; i++)
        g_clear_pointer (&E.notifs[i], ext_idle_notification_v1_destroy);

    if (!E.actif || E.notifier == NULL || E.seat == NULL)
        return;

    E.profil_secteur = shell_sur_secteur ();

    int armes = 0;
    for (int i = 0; i < ETAGES; i++) {
        if (E.delais[i] <= 0)
            continue;
        E.notifs[i] = ext_idle_notifier_v1_get_idle_notification (
            E.notifier, (uint32_t) E.delais[i] * 1000u, E.seat);
        ext_idle_notification_v1_add_listener (E.notifs[i], &ecouteur,
                                               GINT_TO_POINTER (i));
        armes++;
    }

    /* Les requetes partent dans la file par defaut. GTK vide cette file a
     * chaque tour de boucle, mais l'init a lieu AVANT que la boucle ne
     * tourne : sans ce flush, les notifications ne seraient creees qu'au
     * premier evenement recu par GTK -- c'est-a-dire, sur un bureau au
     * repos, peut-etre jamais. */
    GdkDisplay *gdk = gdk_display_get_default ();
    if (GDK_IS_WAYLAND_DISPLAY (gdk))
        wl_display_flush (gdk_wayland_display_get_wl_display (
                              GDK_WAYLAND_DISPLAY (gdk)));

    g_message ("energie : profil %s, %d etage(s) arme(s) — "
               "attenuer %ds, eteindre %ds, suspendre %ds%s",
               E.profil_secteur ? "secteur" : "batterie", armes,
               E.delais[ATTENUER], E.delais[ETEINDRE], E.delais[SUSPENDRE],
               E.suspendre_permis ? "" : " (suspension verrouillee)");
}

static void
appliquer_config (const ShellConfig *cfg)
{
    E.actif            = cfg->energie_active;
    E.niveau           = cfg->energie_niveau;
    E.suspendre_permis = cfg->energie_suspendre_permis;

    /* Le mode force le profil ; « auto » suit la prise. */
    gboolean secteur = shell_sur_secteur ();
    if (g_strcmp0 (cfg->energie_mode, "normal") == 0)
        secteur = TRUE;
    else if (g_strcmp0 (cfg->energie_mode, "econome") == 0)
        secteur = FALSE;

    if (secteur) {
        E.delais[ATTENUER]  = cfg->energie_secteur_attenuer;
        E.delais[ETEINDRE]  = cfg->energie_secteur_eteindre;
        E.delais[SUSPENDRE] = cfg->energie_secteur_suspendre;
    } else {
        E.delais[ATTENUER]  = cfg->energie_batterie_attenuer;
        E.delais[ETEINDRE]  = cfg->energie_batterie_eteindre;
        E.delais[SUSPENDRE] = cfg->energie_batterie_suspendre;
    }
}

/* -------------------------------------------------------------------------
 * Accrochage a Wayland
 * ------------------------------------------------------------------------- */

static void
on_global (void *data, struct wl_registry *registry, uint32_t name,
           const char *interface, uint32_t version)
{
    (void) data; (void) version;
    if (strcmp (interface, ext_idle_notifier_v1_interface.name) == 0)
        E.notifier = wl_registry_bind (registry, name,
                                       &ext_idle_notifier_v1_interface, 1);
}

static void
on_global_remove (void *d, struct wl_registry *r, uint32_t n)
{ (void) d; (void) r; (void) n; }

static const struct wl_registry_listener registry_listener = {
    on_global, on_global_remove,
};

void
shell_energie_init (const ShellConfig *cfg)
{
    E.avant = -1;

    /* Sans retroeclairage pilotable, les deux premiers etages n'ont aucun
     * effet et le troisieme est ferme par defaut : ne rien accrocher. */
    if (!shell_retro_disponible ()) {
        g_message ("energie : aucun retroeclairage pilotable, module inactif");
        return;
    }

    /* Ces deux sorties etaient MUETTES dans la premiere version, et cela a
     * coute une seance : le module ne faisait rien, le journal ne disait
     * rien, et il a fallu un essai differe pour s'apercevoir que le
     * protocole, lui, fonctionnait. Invariant n^o 4. */
    GdkDisplay *gdk = gdk_display_get_default ();
    if (!GDK_IS_WAYLAND_DISPLAY (gdk)) {
        g_message ("energie : l'affichage n'est pas Wayland, module inactif");
        return;
    }

    GdkSeat *gdk_seat = gdk_display_get_default_seat (gdk);
    if (gdk_seat == NULL) {
        g_message ("energie : aucun siege GDK, module inactif");
        return;
    }
    E.seat = gdk_wayland_seat_get_wl_seat (GDK_WAYLAND_SEAT (gdk_seat));
    if (E.seat == NULL) {
        g_message ("energie : le siege GDK n'a pas de wl_seat, module inactif");
        return;
    }

    struct wl_display  *display  =
        gdk_wayland_display_get_wl_display (GDK_WAYLAND_DISPLAY (gdk));
    struct wl_registry *registry = wl_display_get_registry (display);
    wl_registry_add_listener (registry, &registry_listener, NULL);
    wl_display_roundtrip (display);

    if (E.notifier == NULL) {
        g_message ("energie : le compositeur n'annonce pas "
                   "ext_idle_notifier_v1, module inactif");
        return;
    }

    appliquer_config (cfg);
    reconstruire ();
}

void
shell_energie_reconfigurer (const ShellConfig *cfg)
{
    if (E.notifier == NULL)
        return;

    /* Un changement de reglage pendant que l'ecran est attenue laisserait
     * l'utilisateur dans le noir : on remonte d'abord. */
    if (E.avant >= 0) {
        shell_retro_ecrire (E.avant);
        E.avant = -1;
    }
    appliquer_config (cfg);
    reconstruire ();
}
