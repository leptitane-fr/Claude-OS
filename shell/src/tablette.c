/* =========================================================================
 * Claude OS — le mode tablette. Voir tablette.h pour le pourquoi.
 * ========================================================================= */

/* O_CLOEXEC : meson compile en c11 strict, qui ne l'expose pas. Sans lui, le
 * descripteur du commutateur passerait à chaque application lancée depuis le
 * dock. */
#define _GNU_SOURCE

#include "tablette.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Le seul périphérique ouvert. Voir 70-claude-os-tablette.rules : les deux
 * autres qui portent le commutateur exposeraient plus que lui. */
#define NOM_COMMUTATEUR "Tablet Mode Switch"

static struct {
    ShellTabletteFunc    cb;
    gpointer             donnees;
    int                  fd;
    guint                source;
    gboolean             commutateur;   /* ce que dit le matériel         */
    ShellTabletteForcage forcage;
    gboolean             annonce;       /* le dernier mode effectif dit   */
} T = { .fd = -1, .forcage = SHELL_TABLETTE_SUIVRE };

static gboolean
effectif (void)
{
    return T.forcage == SHELL_TABLETTE_SUIVRE ? T.commutateur
                                              : T.forcage == SHELL_TABLETTE_TABLETTE;
}

/* N'appelle le rappel que si le mode effectif a changé : un forçage qui
 * confirme le commutateur, ou un retour au commutateur qui dit la même
 * chose, ne doit rien remuer à l'écran. */
static void
annoncer (void)
{
    gboolean m = effectif ();
    if (m == T.annonce)
        return;
    T.annonce = m;
    g_message ("mode tablette : %s%s", m ? "oui" : "non",
               T.forcage == SHELL_TABLETTE_SUIVRE ? "" : " (forcé)");
    if (T.cb != NULL)
        T.cb (m, T.donnees);
}

/* L'état courant, demandé au noyau. Sert à l'ouverture, et après
 * SYN_DROPPED : la file du noyau a débordé, des événements sont perdus, et
 * le seul état fiable est celui qu'on redemande. */
static gboolean
lire_etat (int fd, gboolean *tablette)
{
    unsigned long bits[(SW_MAX + 8 * sizeof (unsigned long)) / (8 * sizeof (unsigned long))];
    memset (bits, 0, sizeof bits);
    if (ioctl (fd, EVIOCGSW (sizeof bits), bits) < 0)
        return FALSE;
    *tablette = (bits[SW_TABLET_MODE / (8 * sizeof (unsigned long))]
                 >> (SW_TABLET_MODE % (8 * sizeof (unsigned long)))) & 1;
    return TRUE;
}

static gboolean
on_evenement (int fd, GIOCondition cond, gpointer data)
{
    (void) data;

    if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        /* Périphérique retiré : pilote déchargé, reprise ratée. Le dire, et
         * rester sur le dernier état connu plutôt que de basculer sur une
         * supposition. */
        g_warning ("mode tablette : commutateur perdu (condition 0x%x), "
                   "le mode reste %s jusqu'au redémarrage du dock",
                   cond, T.commutateur ? "tablette" : "portable");
        close (T.fd);
        T.fd = -1;
        T.source = 0;
        return G_SOURCE_REMOVE;
    }

    struct input_event ev[16];
    ssize_t n;
    while ((n = read (fd, ev, sizeof ev)) > 0) {
        for (size_t i = 0; i < (size_t) n / sizeof ev[0]; i++) {
            if (ev[i].type == EV_SW && ev[i].code == SW_TABLET_MODE) {
                T.commutateur = ev[i].value != 0;
            } else if (ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED) {
                gboolean t;
                if (lire_etat (fd, &t))
                    T.commutateur = t;
                else
                    g_warning ("mode tablette : état illisible après débordement : %s",
                               g_strerror (errno));
            }
        }
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR)
        g_warning ("mode tablette : lecture du commutateur : %s", g_strerror (errno));

    /* Une seule annonce par lot : un aller-retour dans le même lot ne
     * remue rien. */
    annoncer ();
    return G_SOURCE_CONTINUE;
}

/* Le nœud /dev/input/eventN dont le parent s'appelle NOM_COMMUTATEUR. */
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
        g_autofree char *chemin = g_strdup_printf ("/sys/class/input/%s/device/name", e);
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
shell_tablette_init (ShellTabletteFunc cb, gpointer user_data)
{
    T.cb = cb;
    T.donnees = user_data;

    g_autofree char *noeud = trouver_noeud ();
    if (noeud == NULL) {
        g_message ("mode tablette : aucun « %s » sur cette machine, "
                   "mode portable permanent", NOM_COMMUTATEUR);
        return;
    }

    T.fd = open (noeud, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (T.fd < 0) {
        /* EACCES : la règle udev n'est pas déployée, ou udev ne l'a pas
         * rejouée sur un périphérique déjà présent. C'est le cas le plus
         * probable après une mise à jour : le dire en ces termes. */
        g_warning ("mode tablette : %s : %s — règle "
                   "/etc/udev/rules.d/70-claude-os-tablette.rules absente ou "
                   "pas encore appliquée ; mode portable",
                   noeud, g_strerror (errno));
        return;
    }

    if (!lire_etat (T.fd, &T.commutateur))
        g_warning ("mode tablette : état initial illisible (%s), "
                   "supposé portable", g_strerror (errno));

    T.annonce = effectif ();
    T.source = g_unix_fd_add (T.fd, G_IO_IN | G_IO_ERR | G_IO_HUP,
                              on_evenement, NULL);
    g_message ("mode tablette : %s suivi, état initial %s",
               noeud, T.commutateur ? "tablette" : "portable");
}

gboolean
shell_tablette_active (void)
{
    return effectif ();
}

gboolean
shell_tablette_commutateur (void)
{
    return T.commutateur;
}

void
shell_tablette_forcer (ShellTabletteForcage forcage)
{
    T.forcage = forcage;
    annoncer ();
}
