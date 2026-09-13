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

typedef struct {
    GMappedFile *fichier;
    const char  *texte;
    gsize        taille;
} Table;

static struct {
    Table       mots;        /* sans accents \t mot \t fréquence          */
    Table       suites;      /* précédent \t mot \t compte                */
    GHashTable *appris;      /* mot -> compte                             */
    guint       ecriture;    /* minuterie d'écriture différée             */
} M;

/* Ce que pèse une suite face à une fréquence. Les deux échelles n'ont rien
 * à voir — centièmes de par-million d'un côté, comptes bruts sur 726 000
 * phrases de l'autre — et ce facteur les rapproche : après « comment »,
 * « vous » (248 suites) passe devant « voiture », sans que « vous » écrase
 * tout le dictionnaire dès qu'un mot rare le précède. */
#define POIDS_SUITE 400

/* QUI TAPE UN ACCENT LE VEUT. « eleve » propose « élève » — on ne tape pas
 * ses accents quand on cherche un mot au pouce —, mais « él » ne doit plus
 * proposer « elle » : l'accent tapé devient une exigence, pas un indice. */

/* -------------------------------------------------------------------------
 * Le dictionnaire projeté
 * ------------------------------------------------------------------------- */

/* Début de la ligne qui contient `p`. */
static const char *
debut_ligne (const Table *t, const char *p)
{
    while (p > t->texte && p[-1] != '\n')
        p--;
    return p;
}

/* La forme sans accents, en minuscules : la clé de recherche. œ et æ ne se
 * décomposent pas — ils se transcrivent, comme dans fabrique-mots.py. */
static char *
sans_accents (const char *mot)
{
    g_autofree char *bas = g_utf8_strdown (mot, -1);
    g_autofree char *oe = NULL;
    if (strstr (bas, "œ") != NULL || strstr (bas, "æ") != NULL) {
        g_auto(GStrv) m1 = g_strsplit (bas, "œ", -1);
        g_autofree char *t1 = g_strjoinv ("oe", m1);
        g_auto(GStrv) m2 = g_strsplit (t1, "æ", -1);
        oe = g_strjoinv ("ae", m2);
    }
    g_autofree char *nfd = g_utf8_normalize (oe != NULL ? oe : bas, -1, G_NORMALIZE_NFD);
    GString *sortie = g_string_new (NULL);
    for (const char *p = nfd; *p != '\0'; p = g_utf8_next_char (p)) {
        gunichar c = g_utf8_get_char (p);
        if (g_unichar_type (c) != G_UNICODE_NON_SPACING_MARK)
            g_string_append_unichar (sortie, c);
    }
    return g_string_free (sortie, FALSE);
}

/* Compare le début d'une ligne à un préfixe, comme memcmp : <0, 0, >0.
 * L'ordre du fichier est celui des octets, et UTF-8 garde l'ordre des
 * caractères : la dichotomie est donc valide sur des mots accentués. */
static int
compare_prefixe (const char *ligne, const char *prefixe, gsize n)
{
    for (gsize i = 0; i < n; i++) {
        char c = ligne[i];
        /* Une tabulation ferme le champ — SAUF si le préfixe en cherche une
         * à cette place : les suites se cherchent par « précédent\t », et
         * l'oublier faisait que la prédiction ne rendait jamais rien. */
        if (c == '\n' || (c == '\t' && prefixe[i] != '\t'))
            return -1;                      /* la ligne est plus courte */
        if (c != prefixe[i])
            return (unsigned char) c < (unsigned char) prefixe[i] ? -1 : 1;
    }
    return 0;
}

/* Première ligne dont le mot commence par `prefixe`, ou NULL. */
static const char *
premiere_ligne (const Table *t, const char *prefixe, gsize n)
{
    if (t->texte == NULL)
        return NULL;
    gsize bas = 0, haut = t->taille;
    const char *trouve = NULL;
    while (bas < haut) {
        gsize milieu = (bas + haut) / 2;
        const char *ligne = debut_ligne (t, t->texte + milieu);
        int c = compare_prefixe (ligne, prefixe, n);
        if (c < 0) {
            /* avancer à la ligne suivante, sinon on boucle */
            const char *fin = memchr (ligne, '\n', t->texte + t->taille - ligne);
            gsize suivante = fin != NULL ? (gsize) (fin + 1 - t->texte) : t->taille;
            if (suivante <= bas)
                break;
            bas = suivante;
        } else {
            trouve = c == 0 ? ligne : trouve;
            gsize ici = ligne - t->texte;
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

/* Les suites du mot précédent, dans une table : le bloc fait au plus trente
 * lignes, on le lit une fois plutôt qu'une dichotomie par candidat. */
static GHashTable *
suites_de (const char *precedent)
{
    GHashTable *h = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    if (precedent == NULL || *precedent == '\0')
        return h;
    g_autofree char *cle = g_strdup_printf ("%s\t", precedent);
    gsize n = strlen (cle);
    for (const char *ligne = premiere_ligne (&M.suites, cle, n); ligne != NULL; ) {
        const char *fin = memchr (ligne, '\n', M.suites.texte + M.suites.taille - ligne);
        if (fin == NULL || compare_prefixe (ligne, cle, n) != 0)
            break;
        const char *mot = ligne + n;
        const char *tab = memchr (mot, '\t', fin - mot);
        if (tab != NULL)
            g_hash_table_insert (h, g_strndup (mot, tab - mot),
                                 GINT_TO_POINTER (atoi (tab + 1)));
        ligne = fin + 1;
        if (ligne >= M.suites.texte + M.suites.taille)
            break;
    }
    return h;
}

/* La prédiction : rien n'est tapé, on propose ce qui suit d'ordinaire. Le
 * bloc des suites est déjà trié par compte décroissant — il suffit de le
 * lire dans l'ordre. */
static guint
predire (const char *precedent, char **sortie, guint max)
{
    g_autofree char *cle = g_strdup_printf ("%s\t", precedent != NULL ? precedent : "^");
    gsize n = strlen (cle);
    guint trouves = 0;
    for (const char *ligne = premiere_ligne (&M.suites, cle, n);
         ligne != NULL && trouves < max; ) {
        const char *fin = memchr (ligne, '\n', M.suites.texte + M.suites.taille - ligne);
        if (fin == NULL || compare_prefixe (ligne, cle, n) != 0)
            break;
        const char *mot = ligne + n;
        const char *tab = memchr (mot, '\t', fin - mot);
        if (tab != NULL)
            sortie[trouves++] = g_strndup (mot, tab - mot);
        ligne = fin + 1;
        if (ligne >= M.suites.texte + M.suites.taille)
            break;
    }
    return trouves;
}

guint
shell_mots_suggerer (const char *prefixe, const char *precedent,
                     char **sortie, guint max)
{
    for (guint i = 0; i < max; i++)
        sortie[i] = NULL;
    if (M.mots.texte == NULL)
        return 0;
    if (prefixe == NULL || *prefixe == '\0')
        return predire (precedent, sortie, max);

    g_autofree char *tape = g_utf8_strdown (prefixe, -1);
    g_autofree char *cle = sans_accents (prefixe);
    gsize n = strlen (cle), n_tape = strlen (tape);
    gboolean accentue = strcmp (tape, cle) != 0;
    g_autoptr(GHashTable) suites = suites_de (precedent);
    g_autofree Candidat *tete = g_new0 (Candidat, max);

    /* Les mots appris d'abord : leur poids doit pouvoir dépasser celui du
     * dictionnaire. Eux se comparent sur la forme tapée, accents compris. */
    GHashTableIter it;
    gpointer c, v;
    g_hash_table_iter_init (&it, M.appris);
    while (g_hash_table_iter_next (&it, &c, &v)) {
        const char *mot = c;
        g_autofree char *sans = sans_accents (mot);
        if (strncmp (sans, cle, n) == 0 && strcmp (mot, tape) != 0)
            retenir (tete, max, mot, strlen (mot),
                     (gint64) GPOINTER_TO_INT (v) * POIDS_APPRIS);
    }

    for (const char *ligne = premiere_ligne (&M.mots, cle, n); ligne != NULL; ) {
        const char *fin = memchr (ligne, '\n', M.mots.texte + M.mots.taille - ligne);
        if (fin == NULL || compare_prefixe (ligne, cle, n) != 0)
            break;

        /* sans-accents \t mot \t fréquence */
        const char *t1 = memchr (ligne, '\t', fin - ligne);
        const char *mot = t1 != NULL ? t1 + 1 : NULL;
        const char *t2 = mot != NULL ? memchr (mot, '\t', fin - mot) : NULL;
        if (t2 != NULL) {
            gsize taille_mot = t2 - mot;
            gint64 poids = g_ascii_strtoll (t2 + 1, NULL, 10);
            g_autofree char *copie = g_strndup (mot, taille_mot);
            poids += (gint64) POIDS_SUITE
                   * GPOINTER_TO_INT (g_hash_table_lookup (suites, copie));
            gboolean pareil = taille_mot >= n_tape && strncmp (mot, tape, n_tape) == 0;
            if (accentue && !pareil)
                poids = 0;                 /* l'accent tapé est une exigence */
            if (poids > 0 && !(taille_mot == n_tape && pareil))
                retenir (tete, max, mot, taille_mot, poids);
        }
        ligne = fin + 1;
        if (ligne >= M.mots.texte + M.mots.taille)
            break;
    }

    guint n_sorties = 0;
    for (guint i = 0; i < max; i++)
        if (tete[i].mot != NULL)
            sortie[n_sorties++] = tete[i].mot;
    return n_sorties;
}

static gboolean
projeter (Table *t, const char *nom)
{
    g_autofree char *chemin = g_build_filename (SHELL_DATA_DIR, nom, NULL);
    g_autoptr(GError) err = NULL;
    t->fichier = g_mapped_file_new (chemin, FALSE, &err);
    if (t->fichier == NULL) {
        g_warning ("suggestions : %s — le clavier écrira sans elles", err->message);
        return FALSE;
    }
    t->texte = g_mapped_file_get_contents (t->fichier);
    t->taille = g_mapped_file_get_length (t->fichier);
    return TRUE;
}

gboolean
shell_mots_init (void)
{
    lire_appris ();

    if (!projeter (&M.mots, "mots-fr.txt"))
        return FALSE;
    /* Les suites sont un confort, pas une condition : sans elles le clavier
     * propose encore, mais sans tenir compte de ce qui précède. */
    projeter (&M.suites, "suites-fr.txt");
    g_message ("suggestions : %.1f Mo de mots, %.1f Mo de suites",
               M.mots.taille / 1048576.0, M.suites.taille / 1048576.0);
    return TRUE;
}
