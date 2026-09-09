#include "energie.h"
#include "retroeclairage.h"
#include "sysfs.h"
#include "preavis.h"

#include <gtk/gtk.h>
#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>
#include <glib-unix.h>        /* g_unix_fd_add */

#include <string.h>            /* strcmp */

#include "ext-idle-notify-v1-client-protocol.h"

/* DEUX PREAVIS, PAS UN.
 *
 * Le cadran annonce chaque baisse d'ecran : celle qui attenue, et celle qui
 * eteint. La seconde est la plus brutale -- on passe d'un ecran lisible a
 * un ecran noir -- et c'etait justement celle qui n'etait pas annoncee. */
typedef enum {
    PREAVIS_ATT = 0,   /* avant l'attenuation                              */
    ATTENUER,
    PREAVIS_ETE,       /* avant l'extinction                               */
    ETEINDRE,
    SUSPENDRE,
    ETAGES
} Etage;

static struct {
    /* NOTRE PROPRE CONNEXION AU COMPOSITEUR, ET C'EST DELIBERE.
     *
     * La premiere version accrochait ses objets a la connexion de GTK, par
     * gdk_wayland_display_get_wl_display(). Ils atterrissaient donc dans la
     * file d'evenements PAR DEFAUT, que GDK ne s'engage pas a vider : les
     * « idled » s'y accumulaient et n'etaient depiles que lorsqu'une
     * operation GTK provoquait incidemment un aller-retour.
     *
     * Le symptome, le 9 septembre 2026 : des etages atteints par salves
     * irregulieres puis plus rien du tout, pendant qu'un client d'essai
     * isole, lui, recevait ses evenements a la seconde. Meme compositeur,
     * meme instant -- ce qui a mis la faute hors du protocole et dans la
     * facon de s'y brancher.
     *
     * Une seconde connexion coute une socket. C'est le prix d'une file
     * qu'on vide soi-meme, depuis une source GLib branchee sur son propre
     * descripteur : plus aucune dependance au bon vouloir de GDK. */
    struct wl_display               *display;
    guint                            source;
    struct ext_idle_notifier_v1     *notifier;
    struct wl_seat                  *seat;
    struct ext_idle_notification_v1 *notifs[ETAGES];

    int      delais[ETAGES];      /* secondes ; 0 = etage ferme            */
    int      niveau;              /* pourcent de l'etage « attenuer »      */
    int      preavis_s;           /* duree du compte a rebours             */
    gboolean suspendre_permis;
    gboolean actif;
    const ShellModeEnergie *mode;

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
    case PREAVIS_ATT:
    case PREAVIS_ETE:
        /* On ne touche a rien : on previent. Le geste de l'utilisateur, s'il
         * vient, annulera la suite par « resumed ». */
        shell_preavis_montrer (E.preavis_s);
        break;

    case ATTENUER:
        shell_preavis_cacher ();
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
        shell_preavis_cacher ();
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

    /* Le decompte disparait avant tout le reste : c'est la reponse
     * immediate au geste de l'utilisateur, et la seule qu'il verra si
     * l'ecran n'avait pas encore baisse. */
    shell_preavis_cacher ();

    /* Chaque etage deja inactif emet son « resumed » : ce rappel arrive
     * plusieurs fois pour une seule touche pressee. Le garde ci-dessous le
     * rend idempotent. */
    if (E.avant >= 0) {
        shell_retro_ecrire (E.avant);
        E.avant = -1;
    }
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

    /* Sans ce flush, les requetes resteraient dans le tampon d'emission :
     * la boucle GLib ne l'ecrit que lorsqu'elle a autre chose a faire, et
     * sur un bureau au repos -- la situation meme qu'on veut detecter --
     * cela peut ne jamais arriver. */
    wl_display_flush (E.display);

    g_message ("energie : mode %s, %d etage(s) arme(s) — preavis %ds "
               "(a %ds et %ds), attenuer %ds, eteindre %ds, suspendre %ds%s",
               E.mode != NULL ? E.mode->id : "?", armes, E.preavis_s,
               E.delais[PREAVIS_ATT], E.delais[PREAVIS_ETE],
               E.delais[ATTENUER], E.delais[ETEINDRE], E.delais[SUSPENDRE],
               E.suspendre_permis ? "" : " (suspension verrouillee)");
}

static void
appliquer_config (const ShellConfig *cfg)
{
    E.actif            = cfg->energie_active;
    E.niveau           = cfg->energie_niveau;
    E.suspendre_permis = cfg->energie_suspendre_permis;
    E.mode             = shell_energie_mode_actif (cfg);
    shell_preavis_opacite (cfg->energie_opacite);

    int pre, att, ete, sus;
    shell_energie_delais (cfg, &pre, &att, &ete, &sus);

    /* Le preavis est un etage a part entiere, arme AVANT l'attenuation.
     * S'il ne tient pas dans le delai -- preavis plus long que le delai
     * lui-meme -- on l'abandonne plutot que de l'afficher a l'envers. */
    E.preavis_s = (att > pre) ? pre : 0;

    E.delais[PREAVIS_ATT] = (E.preavis_s > 0) ? att - E.preavis_s : 0;
    E.delais[ATTENUER]    = att;

    /* Le second preavis n'est arme que s'il TIENT entre les deux etages :
     * un compte a rebours qui commencerait avant l'attenuation annoncerait
     * l'extinction pendant que l'ecran baisse encore, et les deux se
     * marcheraient dessus. Delais serres : on renonce au second, pas au
     * premier. */
    E.delais[PREAVIS_ETE] = (E.preavis_s > 0 && ete > 0
                             && ete - E.preavis_s > att)
                            ? ete - E.preavis_s : 0;
    E.delais[ETEINDRE]    = ete;
    E.delais[SUSPENDRE]   = sus;
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
    /* Le siege vient de NOTRE registre, pas de GDK : un objet d'une autre
     * connexion ne peut pas etre passe en argument d'une requete. */
    else if (strcmp (interface, wl_seat_interface.name) == 0 && E.seat == NULL)
        E.seat = wl_registry_bind (registry, name, &wl_seat_interface, 1);
}

static void
on_global_remove (void *d, struct wl_registry *r, uint32_t n)
{ (void) d; (void) r; (void) n; }

static const struct wl_registry_listener registry_listener = {
    on_global, on_global_remove,
};

/* Le descripteur est lisible : wl_display_dispatch() ne bloquera donc pas.
 * Une erreur ici n'est pas rattrapable -- le compositeur a ferme la
 * connexion -- on le dit et on rend la source. */
static gboolean
on_wayland_lisible (gint fd, GIOCondition cond, gpointer data)
{
    (void) fd; (void) data;

    if (cond & (G_IO_ERR | G_IO_HUP)) {
        g_message ("energie : connexion Wayland rompue, module inactif");
        E.source = 0;
        return G_SOURCE_REMOVE;
    }
    if (wl_display_dispatch (E.display) < 0) {
        g_message ("energie : lecture Wayland en echec, module inactif");
        E.source = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

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

    E.display = wl_display_connect (NULL);
    if (E.display == NULL) {
        g_message ("energie : seconde connexion Wayland refusee, module inactif");
        return;
    }

    struct wl_registry *registry = wl_display_get_registry (E.display);
    wl_registry_add_listener (registry, &registry_listener, NULL);
    wl_display_roundtrip (E.display);

    if (E.notifier == NULL || E.seat == NULL) {
        g_message ("energie : le compositeur n'annonce pas %s, module inactif",
                   E.notifier == NULL ? "ext_idle_notifier_v1" : "wl_seat");
        wl_display_disconnect (E.display);
        E.display = NULL;
        return;
    }

    /* Une source GLib sur NOTRE descripteur : la boucle principale nous
     * reveille quand le compositeur parle, et nous depilons nous-memes. Ce
     * n'est pas de la scrutation -- la source dort tant que rien n'arrive. */
    E.source = g_unix_fd_add (wl_display_get_fd (E.display), G_IO_IN,
                              on_wayland_lisible, NULL);

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
    shell_preavis_cacher ();
    if (E.avant >= 0) {
        shell_retro_ecrire (E.avant);
        E.avant = -1;
    }
    appliquer_config (cfg);
    reconstruire ();
}
