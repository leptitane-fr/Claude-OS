/* Le capot — voir capot.h pour le raisonnement complet. */

/* O_CLOEXEC : meme raison que dans tablette.c. */
#define _GNU_SOURCE

#include "capot.h"
#include "energie.h"      /* shell_energie_verrouiller () */

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <linux/input.h>

/* Le peripherique dedie au capot, et lui seul. Voir capot.h : « cros_ec_buttons »
 * porte le meme commutateur mais aussi les boutons d'alimentation. */
#define NOM_COMMUTATEUR "Lid Switch"

#define BITS_LONG (8 * sizeof (unsigned long))

static struct {
    int      fd;
    guint    source;
    int      inhibiteur;        /* le descripteur rendu par logind, ou -1  */
    gboolean ferme;             /* dernier etat connu du capot             */
    const ShellCapotAction *action;
} C = { .fd = -1, .inhibiteur = -1 };

/* -------------------------------------------------------------------------
 * L'inhibiteur
 *
 * « block » et non « delay » : delay ne fait qu'accorder un sursis -- cinq
 * secondes ici, InhibitDelayMaxSec -- apres quoi logind agit quand meme. On
 * verrait alors la machine suspendre malgre le reglage choisi, cinq secondes
 * plus tard, ce qui est le genre de panne qu'on met une soiree a comprendre.
 *
 * Le descripteur rendu EST l'inhibiteur : le fermer le leve. On le garde
 * donc ouvert pour la vie du processus, et c'est voulu -- si la barre tombe,
 * logind reprend la main et le capot retrouve son comportement d'avant.
 * ------------------------------------------------------------------------- */
static int
poser_inhibiteur (void)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) bus =
        g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (bus == NULL) {
        g_warning ("capot : bus systeme injoignable — %s", err->message);
        return -1;
    }

    g_autoptr(GUnixFDList) recu = NULL;
    g_autoptr(GVariant) rep = g_dbus_connection_call_with_unix_fd_list_sync (
        bus, "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "Inhibit",
        g_variant_new ("(ssss)", "handle-lid-switch", "Claude OS",
                       "le panneau Énergie décide ce que fait le capot",
                       "block"),
        G_VARIANT_TYPE ("(h)"), G_DBUS_CALL_FLAGS_NONE, -1,
        NULL, &recu, NULL, &err);

    if (rep == NULL) {
        g_warning ("capot : inhibiteur refuse — %s", err->message);
        return -1;
    }

    gint32 indice = -1;
    g_variant_get (rep, "(h)", &indice);
    int fd = g_unix_fd_list_get (recu, indice, &err);
    if (fd < 0) {
        g_warning ("capot : descripteur d'inhibiteur illisible — %s",
                   err->message);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------------- */

static void
agir (void)
{
    if (C.action->methode == NULL) {
        if (g_strcmp0 (C.action->id, "verrouiller") == 0) {
            g_message ("capot : ferme — verrouillage de l'ecran");
            shell_energie_verrouiller ();
        } else {
            g_message ("capot : ferme — action « rien »");
        }
        return;
    }

    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) bus =
        g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (bus == NULL) {
        g_warning ("capot : bus systeme injoignable — %s", err->message);
        return;
    }

    /* On demande d'abord si logind sait faire. Un « SuspendThenHibernate »
     * sur une machine qui ne peut pas hiberner rend une erreur que personne
     * ne lit, et le capot ne fait alors RIEN : la machine reste allumee,
     * repliee, et chauffe dans un sac. Se rabattre sur la suspension vaut
     * infiniment mieux -- c'est ce que logind aurait fait sans nous. */
    g_autofree char *question = g_strconcat ("Can", C.action->methode, NULL);
    g_autoptr(GVariant) peut = g_dbus_connection_call_sync (
        bus, "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", question, NULL,
        G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);

    const char *methode = C.action->methode;
    if (peut != NULL) {
        const char *r = NULL;
        g_variant_get (peut, "(&s)", &r);
        if (g_strcmp0 (r, "yes") != 0) {
            g_warning ("capot : « %s » indisponible (%s) — suspension simple",
                       methode, r ? r : "sans reponse");
            methode = "Suspend";
        }
    }

    g_message ("capot : ferme — %s", methode);
    g_dbus_connection_call (bus, "org.freedesktop.login1",
                            "/org/freedesktop/login1",
                            "org.freedesktop.login1.Manager", methode,
                            g_variant_new ("(b)", FALSE),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* L'etat courant, demande au noyau. Sert a l'ouverture, et apres
 * SYN_DROPPED : la file a deborde, et le seul etat fiable est celui qu'on
 * redemande. Copie du motif de tablette.c, pour SW_LID. */
static gboolean
lire_etat (int fd, gboolean *ferme)
{
    unsigned long bits[(SW_MAX + BITS_LONG) / BITS_LONG];
    memset (bits, 0, sizeof bits);
    if (ioctl (fd, EVIOCGSW (sizeof bits), bits) < 0)
        return FALSE;
    *ferme = (bits[SW_LID / BITS_LONG] >> (SW_LID % BITS_LONG)) & 1;
    return TRUE;
}

static gboolean
on_evenement (int fd, GIOCondition cond, gpointer data)
{
    (void) data;

    if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        g_warning ("capot : commutateur perdu (condition 0x%x) — logind "
                   "reprend la main", cond);
        close (C.fd);
        C.fd = -1;
        C.source = 0;
        /* On rend la main pour de bon : garder l'inhibiteur alors qu'on ne
         * lit plus le capot le rendrait inerte. */
        if (C.inhibiteur >= 0) {
            close (C.inhibiteur);
            C.inhibiteur = -1;
        }
        return G_SOURCE_REMOVE;
    }

    gboolean avant = C.ferme;
    struct input_event ev[16];
    ssize_t n;
    while ((n = read (fd, ev, sizeof ev)) > 0) {
        for (size_t i = 0; i < (size_t) n / sizeof ev[0]; i++) {
            if (ev[i].type == EV_SW && ev[i].code == SW_LID) {
                C.ferme = ev[i].value != 0;
            } else if (ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED) {
                gboolean f;
                if (lire_etat (fd, &f))
                    C.ferme = f;
            }
        }
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR)
        g_warning ("capot : lecture du commutateur : %s", g_strerror (errno));

    /* UNE SEULE DECISION PAR LOT, et seulement sur un vrai changement : un
     * capot qu'on rabat et rouvre dans le meme lot ne doit rien declencher.
     * On n'agit qu'a la FERMETURE -- l'ouverture est deja traitee par le
     * reveil de la machine, et n'a rien a declencher de plus. */
    if (C.ferme != avant) {
        g_message ("capot : %s", C.ferme ? "ferme" : "ouvert");
        if (C.ferme)
            agir ();
    }
    return G_SOURCE_CONTINUE;
}

static char *
trouver_noeud (void)
{
    g_autoptr(GDir) dir = g_dir_open ("/sys/class/input", 0, NULL);
    if (dir == NULL)
        return NULL;

    const char *e;
    while ((e = g_dir_read_name (dir)) != NULL) {
        if (!g_str_has_prefix (e, "event"))
            continue;
        g_autofree char *chemin =
            g_strdup_printf ("/sys/class/input/%s/device/name", e);
        g_autofree char *nom = NULL;
        if (!g_file_get_contents (chemin, &nom, NULL, NULL))
            continue;
        g_strstrip (nom);
        if (g_strcmp0 (nom, NOM_COMMUTATEUR) == 0)
            return g_strdup_printf ("/dev/input/%s", e);
    }
    return NULL;
}

void
shell_capot_init (const ShellConfig *cfg)
{
    C.action = shell_capot_action_active (cfg);

    g_autofree char *noeud = trouver_noeud ();
    if (noeud == NULL) {
        g_message ("capot : aucun « %s » sur cette machine — logind garde "
                   "la main", NOM_COMMUTATEUR);
        return;
    }

    C.fd = open (noeud, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (C.fd < 0) {
        g_warning ("capot : %s : %s — regle udev absente ou pas encore "
                   "appliquee ; logind garde la main",
                   noeud, g_strerror (errno));
        return;
    }

    if (!lire_etat (C.fd, &C.ferme))
        g_warning ("capot : etat initial illisible (%s), suppose ouvert",
                   g_strerror (errno));

    /* L'INHIBITEUR EN DERNIER, et seulement si tout le reste a marche.
     * Prendre la main sans savoir lire le capot le rendrait inerte : la
     * machine resterait allumee, repliee, sans que rien ne la suspende. */
    C.inhibiteur = poser_inhibiteur ();
    if (C.inhibiteur < 0) {
        g_warning ("capot : sans inhibiteur, logind garde la main — le "
                   "reglage du panneau restera sans effet");
        close (C.fd);
        C.fd = -1;
        return;
    }

    C.source = g_unix_fd_add (C.fd, G_IO_IN | G_IO_ERR | G_IO_HUP,
                              on_evenement, NULL);
    g_message ("capot : %s suivi, %s a l'ouverture, action « %s »",
               noeud, C.ferme ? "ferme" : "ouvert", C.action->id);
}

void
shell_capot_reconfigurer (const ShellConfig *cfg)
{
    const ShellCapotAction *a = shell_capot_action_active (cfg);
    if (a == C.action)
        return;
    C.action = a;
    g_message ("capot : action « %s »", C.action->id);
}
