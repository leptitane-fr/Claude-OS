/* =========================================================================
 * Claude OS — la rotation de l'écran. Voir rotation.h pour le pourquoi.
 * ========================================================================= */
#include "rotation.h"

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <math.h>

#define IIO           "/sys/bus/iio/devices"
#define ETIQUETTE     "accel-display"

/* 1 g en unités brutes : 9,81 / 0,000598550 (in_accel_scale), mesuré à
 * 13840/9088 écran incliné de 33°, soit 16 560 en norme. */
#define UN_G          16384.0

/* Le sens de x, que la sonde n'a pas établi (voir rotation.h). S'il est
 * faux, les deux portraits sont échangés — et c'est la seule ligne à
 * changer. */
#define SENS_X        (+1)

#define PERIODE_MS    500

/* Délai avant le premier échantillon, en entrant en mode tablette.
 *
 * VU LE 11 SEPTEMBRE 2026 : l'écran est passé en « 180 » une demi-seconde
 * après l'entrée en mode tablette, sans qu'on l'ait tenu tête en bas. Ce
 * n'est pas une erreur du capteur : pendant le retournement, base à plat,
 * l'écran passe réellement sous la charnière — son bord haut pointe vers le
 * sol entre 180 et 360°. Le commutateur bascule vers 200°, AVANT la fin du
 * geste. On laisse donc au geste le temps de finir. */
#define POSE_MS       1500

/* En deçà, la tablette est trop à plat pour dire où est le haut : on garde
 * l'orientation courante. 0,4 g, c'est une tablette tenue à 24° de
 * l'horizontale — posée sur les genoux, elle reste lisible sans tourner. */
#define SEUIL_PLAN    (0.4 * UN_G)

/* Hystérésis : pour quitter une orientation, le haut doit s'être éloigné de
 * plus de 45 + 20 degrés de son axe. Sans elle, une tablette tenue de
 * biais à 45° hésiterait entre deux orientations à chaque échantillon. */
#define HYSTERESIS    20.0

/* Échantillons concordants avant de tourner : une seconde. Un geste
 * brusque, ou la tablette qu'on passe à quelqu'un, ne doit pas faire
 * tourner l'image en chemin. */
#define CONFIRMATIONS 2

/* Les quatre orientations, dans l'ordre de wl_output.transform. Le centre
 * est l'angle du HAUT de la pièce, mesuré dans le plan de l'écran depuis
 * son bord supérieur, positif vers la droite.
 *
 * Un écran tourné dans le sens des aiguilles d'une montre (bord gauche en
 * haut) demande « 90 » : c'est la convention de wlroots, où la
 * transformation dit comment l'IMAGE est tournée pour paraître droite. */
typedef struct {
    const char *transform;
    double      centre;
} Orientation;

static const Orientation ORIENTATIONS[4] = {
    { "normal",   0.0 },
    { "90",     -90.0 },   /* bord gauche en haut */
    { "180",    180.0 },
    { "270",     90.0 },   /* bord droit en haut  */
};

/* LES PORTRAITS SONT FERMÉS, À LA DEMANDE DE L'UTILISATEUR.
 *
 * Éprouvés le 11 septembre 2026 sur MADOO : ils marchent — sens juste,
 * doigt juste. Mais la dalle de cette machine n'est pas faite pour : ses
 * angles de vision en portrait rendent la lecture pénible, et l'utilisateur
 * ne s'en sert jamais. Le 180° — la position chevalet, pour les vidéos — est
 * la seule rotation utile ici.
 *
 * Tablette tenue en portrait, l'image reste donc où elle est : le module
 * n'envisage que les orientations permises, et une position qui désigne un
 * portrait est traitée comme une tablette à plat — sans avis. */
static const gboolean PERMISE[4] = { TRUE, FALSE, TRUE, FALSE };

static struct {
    char    *dir;           /* /sys/bus/iio/devices/iio:deviceN, ou NULL   */
    char    *sortie;        /* eDP-1                                       */
    gboolean actif;
    gboolean verrou;
    guint    minuteur;
    int      courante;      /* indice dans ORIENTATIONS                    */
    int      appliquee;     /* ce que wlr-randr a accepté, -1 inconnu      */
    int      candidate;
    int      confirmations;
    gboolean prete;         /* recherche du capteur faite                  */
} R = { .appliquee = -1, .candidate = -1 };

/* -------------------------------------------------------------------------
 * Le capteur
 * ------------------------------------------------------------------------- */
static void
preparer (void)
{
    if (R.prete)
        return;
    R.prete = TRUE;

    g_autoptr(GDir) dir = g_dir_open (IIO, 0, NULL);
    const char *e;
    while (dir != NULL && (e = g_dir_read_name (dir)) != NULL) {
        g_autofree char *chemin = g_build_filename (IIO, e, "label", NULL);
        g_autofree char *etiquette = NULL;
        if (g_file_get_contents (chemin, &etiquette, NULL, NULL)
            && g_strcmp0 (g_strstrip (etiquette), ETIQUETTE) == 0) {
            R.dir = g_build_filename (IIO, e, NULL);
            break;
        }
    }
    if (R.dir == NULL)
        g_message ("rotation : aucun accéléromètre « %s », l'écran ne tournera pas",
                   ETIQUETTE);

    /* La sortie interne, par son connecteur. Par GDK et non en dur : un
     * écran externe branché en mode tablette ne doit pas être tourné. */
    GListModel *moniteurs = gdk_display_get_monitors (gdk_display_get_default ());
    for (guint i = 0; i < g_list_model_get_n_items (moniteurs); i++) {
        g_autoptr(GdkMonitor) m = g_list_model_get_item (moniteurs, i);
        const char *c = gdk_monitor_get_connector (m);
        if (c != NULL && g_str_has_prefix (c, "eDP")) {
            R.sortie = g_strdup (c);
            break;
        }
    }
    if (R.sortie == NULL)
        g_message ("rotation : aucune sortie eDP, l'écran ne tournera pas");
}

static gboolean
lire_axe (const char *axe, double *v)
{
    g_autofree char *chemin = g_strdup_printf ("%s/in_accel_%s_raw", R.dir, axe);
    g_autofree char *texte = NULL;
    g_autoptr(GError) err = NULL;
    if (!g_file_get_contents (chemin, &texte, NULL, &err)) {
        g_warning ("rotation : %s", err->message);
        return FALSE;
    }
    *v = g_ascii_strtod (texte, NULL);
    return TRUE;
}

/* -------------------------------------------------------------------------
 * Appliquer
 * ------------------------------------------------------------------------- */
static void
on_randr_fini (GObject *src, GAsyncResult *res, gpointer data)
{
    int voulue = GPOINTER_TO_INT (data);
    g_autoptr(GError) err = NULL;
    g_autofree char *sortie = NULL;

    /* La sortie de wlr-randr est LUE, réussite ou non : c'est elle qui dit
     * pourquoi le compositeur a refusé (invariant n°4). */
    if (!g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &sortie, NULL, &err)) {
        g_warning ("rotation : wlr-randr : %s", err->message);
        return;
    }
    if (!g_subprocess_get_successful (G_SUBPROCESS (src))) {
        g_warning ("rotation : wlr-randr a refusé « %s » (code %d) : %s",
                   ORIENTATIONS[voulue].transform,
                   g_subprocess_get_exit_status (G_SUBPROCESS (src)),
                   sortie != NULL ? g_strstrip (sortie) : "");
        return;
    }
    R.appliquee = voulue;
    g_message ("rotation : écran en « %s »", ORIENTATIONS[voulue].transform);
}

static void
appliquer (int o)
{
    if (o == R.appliquee || R.sortie == NULL)
        return;

    g_autoptr(GError) err = NULL;
    g_autoptr(GSubprocess) p = g_subprocess_new (
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, &err,
        "wlr-randr", "--output", R.sortie,
        "--transform", ORIENTATIONS[o].transform, NULL);
    if (p == NULL) {
        g_warning ("rotation : impossible de lancer wlr-randr : %s", err->message);
        return;
    }
    g_subprocess_communicate_utf8_async (p, NULL, NULL, on_randr_fini,
                                         GINT_TO_POINTER (o));
}

/* -------------------------------------------------------------------------
 * Décider
 * ------------------------------------------------------------------------- */

/* Écart angulaire, ramené dans [0, 180]. */
static double
ecart (double a, double b)
{
    double d = fmod (fabs (a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

static gboolean
on_echantillon (gpointer data)
{
    (void) data;
    double x, y;
    if (!lire_axe ("x", &x) || !lire_axe ("y", &y))
        return G_SOURCE_CONTINUE;

    x *= SENS_X;
    if (hypot (x, y) < SEUIL_PLAN) {
        R.confirmations = 0;
        return G_SOURCE_CONTINUE;
    }

    /* L'accéléromètre lit +1 g sur l'axe qui pointe vers le HAUT : (x, y)
     * est donc la direction du haut dans le plan de l'écran. */
    double haut = atan2 (x, y) * 180.0 / G_PI;

    if (ecart (haut, ORIENTATIONS[R.courante].centre) <= 45.0 + HYSTERESIS) {
        R.confirmations = 0;
        return G_SOURCE_CONTINUE;
    }

    int proche = 0;
    for (int i = 1; i < 4; i++)
        if (ecart (haut, ORIENTATIONS[i].centre) < ecart (haut, ORIENTATIONS[proche].centre))
            proche = i;

    /* Le plus proche des QUATRE, et non des seules permises : tenue en
     * portrait, la tablette ne doit pas basculer vers le paysage le moins
     * éloigné — elle ne dit rien, et l'image ne bouge pas. */
    if (!PERMISE[proche]) {
        R.confirmations = 0;
        return G_SOURCE_CONTINUE;
    }

    if (proche != R.candidate) {
        R.candidate = proche;
        R.confirmations = 1;
    } else if (++R.confirmations >= CONFIRMATIONS) {
        R.courante = proche;
        R.confirmations = 0;
        appliquer (R.courante);
    }
    return G_SOURCE_CONTINUE;
}

static void
arreter_minuteur (void)
{
    if (R.minuteur != 0) {
        g_source_remove (R.minuteur);
        R.minuteur = 0;
    }
    R.confirmations = 0;
    R.candidate = -1;
}

/* La pose est finie : l'échantillonnage régulier prend le relais. Le même
 * identifiant de minuteur sert aux deux, pour qu'un seul arrêt suffise. */
static gboolean
on_pose (gpointer data)
{
    (void) data;
    R.minuteur = g_timeout_add (PERIODE_MS, on_echantillon, NULL);
    on_echantillon (NULL);
    return G_SOURCE_REMOVE;
}

/* Un seul endroit décide s'il faut échantillonner : en mode tablette, sans
 * verrou, avec un capteur. */
static void
reevaluer (void)
{
    gboolean echantillonner = R.actif && !R.verrou && R.dir != NULL;

    if (echantillonner && R.minuteur == 0)
        R.minuteur = g_timeout_add (POSE_MS, on_pose, NULL);
    else if (!echantillonner)
        arreter_minuteur ();
}

void
shell_rotation_suivre (gboolean actif)
{
    preparer ();
    R.actif = actif;

    /* Capot ouvert : paysage, toujours, verrou ou non. Appliqué aussi au
     * premier appel, où l'état réel est inconnu : un dock tombé écran
     * tourné, puis relancé capot ouvert, laisserait sinon l'écran de biais. */
    if (!actif) {
        R.courante = 0;
        appliquer (0);
    }
    reevaluer ();
}

void
shell_rotation_verrouiller (gboolean verrou)
{
    R.verrou = verrou;
    g_message ("rotation : %s", verrou ? "verrouillée" : "libre");
    reevaluer ();
}

gboolean
shell_rotation_verrouillee (void)
{
    return R.verrou;
}
