/* =========================================================================
 * Claude OS — les suggestions de mots. Voir mots.h pour le pourquoi.
 * ========================================================================= */
#include "mots.h"

#include <gio/gio.h>
#include <string.h>

/* Un mot appris pèse autant que cette fréquence, par usage. 5 000
 * centièmes = 50 par million : au-dessus de la moitié du dictionnaire dès
 * le premier emploi, au niveau des mots courants après quelques-uns. Un
 * clavier qui n'apprendrait qu'imperceptiblement n'apprend pas. */
#define POIDS_APPRIS 5000

/* On n'écrit les mots appris qu'après ce silence : taper cinquante mots ne
 * doit pas faire cinquante écritures sur l'eMMC. */
#define DELAI_ECRITURE_S 20

static struct {
    GMappedFile *fichier;
    const char  *texte;      /* le dictionnaire projeté, trié par mot     */
    gsize        taille;
    GHashTable  *appris;     /* mot -> compte                             */
    guint        ecriture;   /* minuterie d'écriture différée             */
} M;

/* -------------------------------------------------------------------------
 * Le dictionnaire projeté
 * ------------------------------------------------------------------------- */

/* Début de la ligne qui contient `p`. */
static const char *
debut_ligne (const char *p)
{
    while (p > M.texte && p[-1] != '\n')
        p--;
    return p;
}

/* Compare le début d'une ligne à un préfixe, comme memcmp : <0, 0, >0.
 * L'ordre du fichier est celui des octets, et UTF-8 garde l'ordre des
 * caractères : la dichotomie est donc valide sur des mots accentués. */
static int
compare_prefixe (const char *ligne, const char *prefixe, gsize n)
{
    for (gsize i = 0; i < n; i++) {
        char c = ligne[i];
        if (c == '\n' || c == '\t')
            return -1;                      /* la ligne est plus courte */
        if (c != prefixe[i])
            return (unsigned char) c < (unsigned char) prefixe[i] ? -1 : 1;
    }
    return 0;
}

/* Première ligne dont le mot commence par `prefixe`, ou NULL. */
static const char *
premiere_ligne (const char *prefixe, gsize n)
{
    gsize bas = 0, haut = M.taille;
    const char *trouve = NULL;
    while (bas < haut) {
        gsize milieu = (bas + haut) / 2;
        const char *ligne = debut_ligne (M.texte + milieu);
        int c = compare_prefixe (ligne, prefixe, n);
        if (c < 0) {
            /* avancer à la ligne suivante, sinon on boucle */
            const char *fin = memchr (ligne, '\n', M.texte + M.taille - ligne);
            gsize suivante = fin != NULL ? (gsize) (fin + 1 - M.texte) : M.taille;
            if (suivante <= bas)
                break;
            bas = suivante;
        } else {
            trouve = c == 0 ? ligne : trouve;
            gsize ici = ligne - M.texte;
            if (ici == 0)
                break;
            haut = ici;
        }
    }
    return trouve;
}

/* -------------------------------------------------------------------------
 * Les mots appris
 * ------------------------------------------------------------------------- */
static char *
chemin_appris (void)
{
    return g_build_filename (g_get_user_state_dir (), "claude-os", "mots-appris.ini", NULL);
}

static void
lire_appris (void)
{
    M.appris = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_autofree char *chemin = chemin_appris ();
    g_autoptr(GKeyFile) kf = g_key_file_new ();
    g_autoptr(GError) err = NULL;
    if (!g_key_file_load_from_file (kf, chemin, G_KEY_FILE_NONE, &err)) {
        if (!g_error_matches (err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_warning ("suggestions : %s : %s", chemin, err->message);
        return;
    }
    g_auto(GStrv) cles = g_key_file_get_keys (kf, "mots", NULL, NULL);
    for (guint i = 0; cles != NULL && cles[i] != NULL; i++) {
        int n = g_key_file_get_integer (kf, "mots", cles[i], NULL);
        if (n > 0)
            g_hash_table_insert (M.appris, g_strdup (cles[i]), GINT_TO_POINTER (n));
    }
    g_message ("suggestions : %u mot(s) appris relus", g_hash_table_size (M.appris));
}

static gboolean
ecrire_appris (gpointer data)
{
    (void) data;
    M.ecriture = 0;
    g_autofree char *chemin = chemin_appris ();
    g_autofree char *dossier = g_path_get_dirname (chemin);
    g_autoptr(GKeyFile) kf = g_key_file_new ();
    GHashTableIter it;
    gpointer cle, val;
    g_hash_table_iter_init (&it, M.appris);
    while (g_hash_table_iter_next (&it, &cle, &val))
        g_key_file_set_integer (kf, "mots", cle, GPOINTER_TO_INT (val));

    g_autoptr(GError) err = NULL;
    if (g_mkdir_with_parents (dossier, 0700) != 0
        || !g_key_file_save_to_file (kf, chemin, &err))
        g_warning ("suggestions : mots appris non gardés (%s) : %s", chemin,
                   err != NULL ? err->message : "mkdir");
    return G_SOURCE_REMOVE;
}

void
shell_mots_apprendre (const char *mot)
{
    if (M.appris == NULL || mot == NULL || *mot == '\0')
        return;
    /* Une clé de GKeyFile ne peut contenir ni « = » ni « [ » ; nos mots
     * n'en ont pas, mais une saisie inattendue ne doit pas corrompre le
     * fichier. */
    if (strpbrk (mot, "=[]\n\t ") != NULL)
        return;

    gpointer n = NULL;
    if (g_hash_table_lookup_extended (M.appris, mot, NULL, &n))
        g_hash_table_replace (M.appris, g_strdup (mot),
                              GINT_TO_POINTER (GPOINTER_TO_INT (n) + 1));
    else
        g_hash_table_insert (M.appris, g_strdup (mot), GINT_TO_POINTER (1));

    if (M.ecriture != 0)
        g_source_remove (M.ecriture);
    M.ecriture = g_timeout_add_seconds (DELAI_ECRITURE_S, ecrire_appris, NULL);
}

/* -------------------------------------------------------------------------
 * Suggérer
 * ------------------------------------------------------------------------- */
typedef struct { char *mot; gint64 poids; } Candidat;

static void
retenir (Candidat *tete, guint max, const char *mot, gsize n, gint64 poids)
{
    guint place = max;
    for (guint i = 0; i < max; i++) {
        if (tete[i].mot != NULL && strncmp (tete[i].mot, mot, n) == 0
            && strlen (tete[i].mot) == n)
            return;                                  /* déjà là (appris + dico) */
        if (place == max && (tete[i].mot == NULL || tete[i].poids < poids))
            place = i;
    }
    if (place == max)
        return;
    g_free (tete[max - 1].mot);
    for (guint i = max - 1; i > place; i--)
        tete[i] = tete[i - 1];
    tete[place].mot = g_strndup (mot, n);
    tete[place].poids = poids;
}

guint
shell_mots_suggerer (const char *prefixe, char **sortie, guint max)
{
    for (guint i = 0; i < max; i++)
        sortie[i] = NULL;
    if (prefixe == NULL || *prefixe == '\0' || M.texte == NULL)
        return 0;

    g_autofree char *bas = g_utf8_strdown (prefixe, -1);
    gsize n = strlen (bas);
    g_autofree Candidat *tete = g_new0 (Candidat, max);

    /* Les mots appris d'abord : ils sont peu nombreux, et leur poids doit
     * pouvoir dépasser celui du dictionnaire. */
    GHashTableIter it;
    gpointer cle, val;
    g_hash_table_iter_init (&it, M.appris);
    while (g_hash_table_iter_next (&it, &cle, &val)) {
        const char *mot = cle;
        if (strncmp (mot, bas, n) == 0 && strcmp (mot, bas) != 0)
            retenir (tete, max, mot, strlen (mot),
                     (gint64) GPOINTER_TO_INT (val) * POIDS_APPRIS);
    }

    for (const char *ligne = premiere_ligne (bas, n); ligne != NULL; ) {
        const char *fin = memchr (ligne, '\n', M.texte + M.taille - ligne);
        if (fin == NULL)
            break;
        if (compare_prefixe (ligne, bas, n) != 0)
            break;
        const char *tab = memchr (ligne, '\t', fin - ligne);
        if (tab != NULL && (gsize) (tab - ligne) != n) {     /* pas le mot déjà tapé */
            gint64 poids = g_ascii_strtoll (tab + 1, NULL, 10);
            retenir (tete, max, ligne, tab - ligne, poids);
        }
        ligne = fin + 1;
        if (ligne >= M.texte + M.taille)
            break;
    }

    guint n_sorties = 0;
    for (guint i = 0; i < max; i++)
        if (tete[i].mot != NULL)
            sortie[n_sorties++] = tete[i].mot;
    return n_sorties;
}

gboolean
shell_mots_init (void)
{
    lire_appris ();

    g_autofree char *chemin = g_build_filename (SHELL_DATA_DIR, "mots-fr.txt", NULL);
    g_autoptr(GError) err = NULL;
    M.fichier = g_mapped_file_new (chemin, FALSE, &err);
    if (M.fichier == NULL) {
        g_warning ("suggestions : %s — le clavier écrira sans elles", err->message);
        return FALSE;
    }
    M.texte = g_mapped_file_get_contents (M.fichier);
    M.taille = g_mapped_file_get_length (M.fichier);
    g_message ("suggestions : dictionnaire projeté, %.1f Mo", M.taille / 1048576.0);
    return TRUE;
}
