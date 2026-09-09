#include "retroeclairage.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

static struct {
    gboolean    tente;         /* init deja passee                          */
    char       *dir;           /* /sys/class/backlight/<qqch>               */
    char       *nom;           /* « intel_backlight »                       */
    int         maxi;
    GDBusProxy *logind;
    gboolean    logind_utilisable;
    gboolean    sysfs_ouvert;
} R;

static char *
lire (const char *dir, const char *fichier)
{
    g_autofree char *chemin = g_build_filename (dir, fichier, NULL);
    char *contenu = NULL;
    if (!g_file_get_contents (chemin, &contenu, NULL, NULL))
        return NULL;
    return g_strstrip (contenu);
}

void
shell_retro_init (void)
{
    if (R.tente)
        return;
    R.tente = TRUE;

    /* Le premier ecran retroeclaire declare par le noyau. Sur MADOO c'est
     * « intel_backlight » ; le nom n'est pas garanti. */
    static const char *base = "/sys/class/backlight";
    g_autoptr(GDir) d = g_dir_open (base, 0, NULL);
    if (d != NULL) {
        const char *nom = g_dir_read_name (d);
        if (nom != NULL) {
            R.nom = g_strdup (nom);
            R.dir = g_build_filename (base, nom, NULL);
        }
    }
    if (R.dir == NULL)
        return;

    g_autofree char *m = lire (R.dir, "max_brightness");
    R.maxi = (m != NULL) ? (int) g_ascii_strtoll (m, NULL, 10) : 0;
    if (R.maxi <= 0) {
        g_clear_pointer (&R.dir, g_free);
        g_clear_pointer (&R.nom, g_free);
        return;
    }

    g_autoptr(GError) err = NULL;
    R.logind = g_dbus_proxy_new_for_bus_sync (
        G_BUS_TYPE_SYSTEM,
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES
            | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
        NULL, "org.freedesktop.login1",
        "/org/freedesktop/login1/session/self",
        "org.freedesktop.login1.Session", NULL, &err);
    if (R.logind != NULL) {
        g_autofree char *proprio = g_dbus_proxy_get_name_owner (R.logind);
        if (proprio == NULL)
            g_clear_object (&R.logind);
    } else {
        g_message ("retroeclairage : logind injoignable — %s", err->message);
    }
    R.logind_utilisable = (R.logind != NULL);

    g_autofree char *chemin = g_build_filename (R.dir, "brightness", NULL);
    R.sysfs_ouvert = (g_access (chemin, W_OK) == 0);
}

gboolean
shell_retro_disponible (void)
{
    shell_retro_init ();
    return R.dir != NULL && (R.logind_utilisable || R.sysfs_ouvert);
}

int
shell_retro_lire (void)
{
    shell_retro_init ();
    if (R.dir == NULL || R.maxi <= 0)
        return -1;
    g_autofree char *v = lire (R.dir, "brightness");
    if (v == NULL)
        return -1;
    double brut = (double) g_ascii_strtoll (v, NULL, 10);
    return (int) (brut * 100.0 / R.maxi + 0.5);
}

/* ON N'ECRIT PAS DANS SYSFS AVEC g_file_set_contents().
 *
 * Elle ecrit de facon ATOMIQUE : elle cree un fichier temporaire a cote de
 * la cible, y ecrit, puis renomme. Or on ne cree pas de fichier dans sysfs.
 * L'echec ne dit meme pas cela -- il parle d'un « brightness.79UMV3 »
 * introuvable, ce qui envoie chercher du cote des droits.
 *
 * Constate le 9 septembre 2026 : le fichier etait pourtant root:video en
 * g+w et le compte bien dans « video ». Rien a voir avec les permissions.
 *
 * Un open/write/close sur le fichier existant, donc. C'est aussi ce que
 * sysfs attend : une seule ecriture, pas de troncature prealable. */
static void
ecrire_sysfs (int valeur)
{
    if (!R.sysfs_ouvert) {
        g_message ("retroeclairage : sysfs n'est pas inscriptible, "
                   "luminosite inchangee");
        return;
    }
    g_autofree char *chemin = g_build_filename (R.dir, "brightness", NULL);
    g_autofree char *texte  = g_strdup_printf ("%d\n", valeur);

    int fd = open (chemin, O_WRONLY);
    if (fd < 0) {
        g_message ("retroeclairage : ouverture de %s refusee — %s",
                   chemin, g_strerror (errno));
        return;
    }
    if (write (fd, texte, strlen (texte)) < 0)
        g_message ("retroeclairage : ecriture refusee — %s",
                   g_strerror (errno));
    close (fd);
}

/* LIRE LA REPONSE DE LOGIND N'EST PAS FACULTATIF.
 *
 * La premiere version passait NULL comme rappel : « personne ne regarde,
 * inutile de traiter l'echec ». Cela a coute une seance entiere le
 * 9 septembre 2026. Le module recevait bien ses evenements d'inactivite --
 * le journal le prouvait -- et l'ecran ne bougeait pas, sans un mot.
 *
 * La cause : logind n'accepte SetBrightness QUE de la session active du
 * siege. Un claude-os-status relance a la main depuis un terminal d'une
 * autre portee (app.slice, et non session-N.scope) se le voit refuser. Le
 * refus arrivait, et partait a la poubelle.
 *
 * Invariant n^o 4 : une commande qui peut echouer doit parler, et son code
 * de retour doit etre lu. Il vaut aussi pour un appel D-Bus asynchrone. */
static void
on_logind_repond (GObject *src, GAsyncResult *res, gpointer data)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) rep =
        g_dbus_proxy_call_finish (G_DBUS_PROXY (src), res, &err);
    if (rep != NULL)
        return;

    /* Un refus de logind n'est pas passager : session inactive, ou politique
     * qui ne changera pas d'ici la prochaine ouverture. On cesse de le
     * solliciter et on tente l'autre voie, comme le fait console.c. */
    R.logind_utilisable = FALSE;
    g_message ("retroeclairage : logind refuse (%s) — repli sur sysfs",
               err->message);
    ecrire_sysfs (GPOINTER_TO_INT (data));
}

void
shell_retro_ecrire (int pourcent)
{
    shell_retro_init ();
    if (R.dir == NULL || R.maxi <= 0) {
        g_message ("retroeclairage : aucun ecran pilotable");
        return;
    }
    if (pourcent < 0)   pourcent = 0;
    if (pourcent > 100) pourcent = 100;

    int valeur = (int) (pourcent * (double) R.maxi / 100.0 + 0.5);

    /* En asynchrone : cet appel part d'un rappel Wayland, dans la boucle de
     * la barre. Un aller-retour synchrone sur le bus systeme la figerait si
     * logind tardait -- et un etage de veille qui gele la barre une seconde
     * est pire que pas d'etage du tout. Mais asynchrone ne veut pas dire
     * aveugle : voir on_logind_repond. */
    if (R.logind_utilisable) {
        g_dbus_proxy_call (R.logind, "SetBrightness",
                           g_variant_new ("(ssu)", "backlight", R.nom,
                                          (guint32) valeur),
                           G_DBUS_CALL_FLAGS_NONE, 2000, NULL,
                           on_logind_repond, GINT_TO_POINTER (valeur));
        return;
    }

    ecrire_sysfs (valeur);
}
