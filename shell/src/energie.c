#include "energie.h"
#include "retroeclairage.h"
#include "sysfs.h"
#include "avis.h"
#include "logind.h"

#include <gtk/gtk.h>
#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>
#include <glib-unix.h>        /* g_unix_fd_add */

#include <string.h>            /* strcmp */
#include <unistd.h>           /* close : lever un inhibiteur */

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
    VERROU,            /* apres l'extinction, si le reglage le demande     */
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
    GPid     verrou;              /* le verrou d'ecran, 0 si aucun         */
    gboolean suspendre_permis;
    gboolean actif;
    const ShellModeEnergie *mode;

    /* Luminosite d'avant l'attenuation, a restaurer. -1 : pas attenue.
     * Elle est relue dans sysfs plutot que gardee de la fois precedente :
     * l'utilisateur a pu bouger le curseur ou les touches entre-temps. */
    int      avant;

    /* Verrouiller AVANT de dormir -- voir « La machine va dormir » plus bas.
     * L'inhibiteur « sleep » en mode delay tient logind le temps que le
     * verrou s'installe ; l'abonnement, lui, vit pour la duree du processus. */
    int      inhib_sommeil;       /* descripteur, ou -1                    */
    guint    abonnement_sommeil;
} E = { .inhib_sommeil = -1 };

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

/* Le verrou est un processus separe, et c'est deliberé : s'il tombe, la
 * barre d'etat ne tombe pas avec lui. C'est aussi ce que le protocole
 * impose -- le client du verrouillage doit vivre aussi longtemps que le
 * verrou tient. */
static void
verrou_termine (GPid pid, gint statut, gpointer donnee)
{
    (void) statut; (void) donnee;
    g_spawn_close_pid (pid);
    if (E.verrou == pid)
        E.verrou = 0;
    g_message ("energie : verrou d'ecran termine");
}

static void
verrouiller_ecran (void)
{
    char *argv[] = { (char *) "claude-os-verrou", NULL };
    g_autoptr(GError) err = NULL;

    if (!g_spawn_async (NULL, argv, NULL,
                        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                        NULL, NULL, &E.verrou, &err)) {
        g_message ("energie : verrou d'ecran indisponible — %s", err->message);
        E.verrou = 0;
        return;
    }
    g_child_watch_add (E.verrou, verrou_termine, NULL);
    g_message ("energie : verrou d'ecran lance");
}

/* A la demande, depuis la Console. Le MEME lanceur que l'etage de veille,
 * pour la meme garde : un second verrou pendant que le premier tient
 * ferait echouer le protocole (ext-session-lock n'en admet qu'un). */
void
shell_energie_verrouiller (void)
{
    if (E.verrou != 0) {
        g_message ("energie : verrou deja en place, rien a faire");
        return;
    }
    verrouiller_ecran ();
}

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

/* -------------------------------------------------------------------------
 * LA MACHINE VA DORMIR : ON VERROUILLE D'ABORD
 *
 * Demande de l'utilisateur, le 14 septembre 2026, apres le premier essai
 * reussi d'hibernation par le capot : la session etait revenue DEVERROUILLEE.
 * Une machine qu'on replie et qu'on emporte doit se retrouver verrouillee,
 * et cela ne se discute pas selon le chemin emprunte.
 *
 * D'OU L'ANCRAGE ICI, ET PAS DANS capot.c. Le capot n'est qu'une des facons
 * de s'endormir : il y a aussi l'etage « suspendre », le seuil de batterie,
 * et tout ce qu'un menu ou une autre application demandera un jour. logind
 * emet « PrepareForSleep(true) » pour TOUTES, et c'est donc le seul endroit
 * ou la regle s'ecrit une fois.
 *
 * CE N'EST PAS LE REGLAGE « Demander le code PIN au reveil ». Celui-la
 * decide du sursis apres l'extinction de l'ecran, quand la machine est
 * restee la, allumee, sous les yeux de son proprietaire. Dormir est autre
 * chose : on ferme, on emporte. Le verrouillage y est systematique.
 *
 * VERROUILLER AVANT PLUTOT QUE REVEILLER APRES : au reveil, l'ecran se
 * rallume sur ce qui etait affiche. Poser le verrou avant que la machine ne
 * parte, c'est garantir qu'il n'y a aucun instant ou le bureau est visible.
 * L'inhibiteur « sleep » en mode DELAY donne le temps de le faire -- logind
 * attend, au plus InhibitDelayMaxSec (cinq secondes ici).
 * ------------------------------------------------------------------------- */

/* Le sursis qu'on prend reellement, bien en deca des cinq secondes : le
 * temps que claude-os-verrou se connecte au compositeur et pose sa surface.
 * Le garder court importe -- c'est autant de retard a chaque fermeture de
 * capot, et l'utilisateur le voit. */
#define DELAI_POSE_VERROU_MS 400

static void
armer_inhibiteur_sommeil (void)
{
    if (E.inhib_sommeil >= 0)
        return;
    E.inhib_sommeil = shell_logind_inhiber (
        "sleep", "verrouiller l'écran avant que la machine ne dorme", "delay");
}

static gboolean
laisser_dormir (gpointer donnee)
{
    (void) donnee;
    /* Fermer le descripteur LEVE l'inhibiteur : logind peut alors endormir
     * la machine. Tant qu'on le tient, elle attend. */
    if (E.inhib_sommeil >= 0) {
        close (E.inhib_sommeil);
        E.inhib_sommeil = -1;
    }
    return G_SOURCE_REMOVE;
}

static void
on_prepare_for_sleep (GDBusConnection *bus, const char *emetteur,
                      const char *chemin, const char *iface,
                      const char *signal, GVariant *params, gpointer donnee)
{
    (void) bus; (void) emetteur; (void) chemin; (void) iface;
    (void) signal; (void) donnee;

    gboolean debut = FALSE;
    g_variant_get (params, "(b)", &debut);

    if (debut) {
        g_message ("energie : la machine va dormir — verrouillage");
        shell_energie_verrouiller ();       /* idempotent : voir sa garde */
        g_timeout_add (DELAI_POSE_VERROU_MS, laisser_dormir, NULL);
    } else {
        /* Au reveil : on se rearme pour la prochaine fois. L'inhibiteur a
         * ete relache avant de dormir, il n'existe plus. */
        g_message ("energie : reveil");
        armer_inhibiteur_sommeil ();
    }
}

static void
suivre_le_sommeil (void)
{
    g_autoptr(GError) err = NULL;
    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (bus == NULL) {
        g_warning ("energie : bus systeme injoignable, l'ecran ne sera pas "
                   "verrouille avant de dormir — %s", err->message);
        return;
    }

    /* La connexion n'est pas liberee : l'abonnement doit vivre aussi
     * longtemps que le processus, et c'est tout ce qu'on lui demande. */
    E.abonnement_sommeil = g_dbus_connection_signal_subscribe (
        bus, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
        "PrepareForSleep", "/org/freedesktop/login1", NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_prepare_for_sleep, NULL, NULL);

    armer_inhibiteur_sommeil ();
    g_message ("energie : verrouillage avant sommeil arme%s",
               E.inhib_sommeil >= 0 ? "" : " (sans inhibiteur : le verrou "
               "pourrait arriver apres le sommeil)");
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
        shell_avis_cadran (E.preavis_s);
        break;

    case ATTENUER:
        shell_avis_cacher ();
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
        shell_avis_cacher ();
        if (son_en_lecture ())
            break;
        if (E.avant < 0)
            E.avant = shell_retro_lire ();   /* etage 1 ferme : noter ici */
        shell_retro_ecrire (0);
        break;

    case VERROU:
        /* UN SEUL VERROU A LA FOIS. L'utilisateur qui touche le clavier pour
         * voir l'ecran de saisie declenche « resumed » : les etages se
         * rearment, et sans ce garde-fou un second verrou serait lance par
         * dessus le premier -- que le compositeur refuserait, en laissant
         * une trace incomprehensible dans le journal. */
        if (E.verrou != 0)
            break;
        verrouiller_ecran ();
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
    shell_avis_cacher ();

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
               "(a %ds et %ds), attenuer %ds, eteindre %ds, verrou %ds, "
               "suspendre %ds%s",
               E.mode != NULL ? E.mode->id : "?", armes, E.preavis_s,
               E.delais[PREAVIS_ATT], E.delais[PREAVIS_ETE],
               E.delais[ATTENUER], E.delais[ETEINDRE], E.delais[VERROU],
               E.delais[SUSPENDRE],
               E.suspendre_permis ? "" : " (suspension verrouillee)");
}

static void
appliquer_config (const ShellConfig *cfg)
{
    E.actif            = cfg->energie_active;
    E.niveau           = cfg->energie_niveau;
    E.suspendre_permis = cfg->energie_suspendre_permis;
    E.mode             = shell_energie_mode_actif (cfg);
    shell_avis_opacite (cfg->energie_opacite);

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

    /* Le verrou s'ancre sur l'extinction, jamais sur l'inactivite brute :
     * « zero » verrouille au moment ou l'ecran s'eteint, une valeur
     * positive laisse ce sursis. Sans etage « eteindre », pas de verrou. */
    E.delais[VERROU] = (cfg->energie_verrou && ete > 0)
                       ? ete + MAX (cfg->energie_verrou_delai, 0) : 0;

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

    /* AVANT TOUT RETOUR ANTICIPE. Le verrouillage avant sommeil ne doit rien
     * a la veille progressive : il vaut sur une machine sans retroeclairage
     * pilotable comme sur un compositeur sans ext-idle-notify. Le placer
     * plus bas le ferait disparaitre en silence sur ces machines-la, et
     * c'est la session qui repartirait deverrouillee. */
    suivre_le_sommeil ();

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
    shell_avis_cacher ();
    if (E.avant >= 0) {
        shell_retro_ecrire (E.avant);
        E.avant = -1;
    }
    appliquer_config (cfg);
    reconstruire ();
}
