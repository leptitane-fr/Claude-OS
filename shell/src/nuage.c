/* =========================================================================
 * Claude-OS Shell — lecteurs nuage : le modele
 *
 * Le raisonnement -- pourquoi un module a part, pourquoi aucun privilege,
 * pourquoi les jetons ne sont pas ici -- est en tete de nuage.h.
 * ========================================================================= */
#include "nuage.h"

#include <gio/gunixmounts.h>

/* ------------------------------------------------------------------------- */
static const struct {
    NuageFournisseur  f;
    const char       *id;
    const char       *nom;
    const char       *icone;
} FOURNISSEURS[] = {
    { NUAGE_DRIVE,    "drive",    "Google Drive",       "claude-os-nuage-drive-symbolic"    },
    { NUAGE_ONEDRIVE, "onedrive", "Microsoft OneDrive", "claude-os-nuage-onedrive-symbolic" },
};

const char *
nuage_fournisseur_id (NuageFournisseur f)
{
    for (guint i = 0; i < G_N_ELEMENTS (FOURNISSEURS); i++)
        if (FOURNISSEURS[i].f == f)
            return FOURNISSEURS[i].id;
    return "drive";
}

const char *
nuage_fournisseur_nom (NuageFournisseur f)
{
    for (guint i = 0; i < G_N_ELEMENTS (FOURNISSEURS); i++)
        if (FOURNISSEURS[i].f == f)
            return FOURNISSEURS[i].nom;
    return FOURNISSEURS[0].nom;
}

const char *
nuage_fournisseur_icone (NuageFournisseur f)
{
    for (guint i = 0; i < G_N_ELEMENTS (FOURNISSEURS); i++)
        if (FOURNISSEURS[i].f == f)
            return FOURNISSEURS[i].icone;
    return FOURNISSEURS[0].icone;
}

NuageFournisseur
nuage_fournisseur_lire (const char *id)
{
    for (guint i = 0; i < G_N_ELEMENTS (FOURNISSEURS); i++)
        if (g_strcmp0 (FOURNISSEURS[i].id, id) == 0)
            return FOURNISSEURS[i].f;
    return NUAGE_DRIVE;
}

/* ------------------------------------------------------------------------- */
void
nuage_free (LecteurNuage *l)
{
    if (l == NULL)
        return;
    g_free (l->id);
    g_free (l->nom);
    g_free (l->compte);
    g_free (l);
}

LecteurNuage *
nuage_copie (const LecteurNuage *l)
{
    LecteurNuage *c = g_new0 (LecteurNuage, 1);
    c->id          = g_strdup (l->id);
    c->nom         = g_strdup (l->nom);
    c->fournisseur = l->fournisseur;
    c->compte      = g_strdup (l->compte);
    c->automatique = l->automatique;
    return c;
}

/* ------------------------------------------------------------------------- */
static char *
chemin_config (void)
{
    return g_build_filename (g_get_user_config_dir (), "claude-os", "nuage", NULL);
}

/* Jamais NULL : les champs absents valent "". Un champ vide se distingue mal
 * d'un champ absent de toute facon, et cela evite un test a chaque usage. */
static char *
cle (GKeyFile *kf, const char *groupe, const char *nom)
{
    char *v = g_key_file_get_string (kf, groupe, nom, NULL);
    return (v != NULL) ? v : g_strdup ("");
}

GPtrArray *
nuage_charger (void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) nuage_free);

    g_autoptr(GKeyFile) kf = g_key_file_new ();
    g_autofree char *chemin = chemin_config ();
    g_autoptr(GError) e = NULL;

    if (!g_key_file_load_from_file (kf, chemin, G_KEY_FILE_KEEP_COMMENTS, &e)) {
        /* Absent : le cas normal tant qu'aucun compte n'a ete connecte.
         * Toute autre erreur se dit -- un fichier mal forme ferait sinon
         * disparaitre les lecteurs sans un mot (invariant n°4). */
        if (!g_error_matches (e, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_warning ("lecteurs nuage illisibles (%s) : %s", chemin, e->message);
        return out;
    }

    gsize n = 0;
    g_auto(GStrv) groupes = g_key_file_get_groups (kf, &n);

    for (gsize i = 0; i < n; i++) {
        LecteurNuage *l = g_new0 (LecteurNuage, 1);
        l->id     = g_strdup (groupes[i]);
        l->nom    = cle (kf, groupes[i], "nom");
        l->compte = cle (kf, groupes[i], "compte");

        g_autofree char *f = cle (kf, groupes[i], "fournisseur");
        l->fournisseur = nuage_fournisseur_lire (f);

        /* Vrai par defaut, la ou il est faux pour les lecteurs reseau : un
         * service en ligne repond ou ne repond pas, la ou un NAS eteint fait
         * attendre un delai TCP entier. g_key_file_get_boolean rend FALSE
         * quand la cle manque, d'ou le test explicite. */
        l->automatique = g_key_file_has_key (kf, groupes[i], "auto", NULL)
            ? g_key_file_get_boolean (kf, groupes[i], "auto", NULL)
            : (g_key_file_has_key (kf, groupes[i], "automatique", NULL)
               ? g_key_file_get_boolean (kf, groupes[i], "automatique", NULL)
               : TRUE);

        if (*l->nom == '\0') {
            g_free (l->nom);
            l->nom = g_strdup (nuage_fournisseur_nom (l->fournisseur));
        }
        g_ptr_array_add (out, l);
    }
    return out;
}

gboolean
nuage_enregistrer (GPtrArray *lecteurs, GError **erreur)
{
    g_autoptr(GKeyFile) kf = g_key_file_new ();

    for (guint i = 0; i < lecteurs->len; i++) {
        const LecteurNuage *l = g_ptr_array_index (lecteurs, i);
        g_key_file_set_string  (kf, l->id, "nom",         l->nom);
        g_key_file_set_string  (kf, l->id, "fournisseur", nuage_fournisseur_id (l->fournisseur));
        if (l->compte != NULL && *l->compte != '\0')
            g_key_file_set_string (kf, l->id, "compte", l->compte);
        g_key_file_set_boolean (kf, l->id, "auto", l->automatique);
    }

    g_key_file_set_comment (kf, NULL, NULL,
        " Lecteurs nuage de Claude OS.\n"
        " Écrit par Réglages › Nuage. Une section par lecteur ; le nom de\n"
        " section sert de nom au répertoire de montage ET de nom de section\n"
        " dans la configuration de rclone.\n"
        " AUCUN JETON ICI : ils sont dans la configuration chiffrée de rclone,\n"
        " dont la phrase est au trousseau (voir nuage-phrase.c).",
        NULL);

    g_autofree char *chemin = chemin_config ();
    g_autofree char *dossier = g_path_get_dirname (chemin);
    g_mkdir_with_parents (dossier, 0700);

    return g_key_file_save_to_file (kf, chemin, erreur);
}

/* -------------------------------------------------------------------------
 * Etat
 * ------------------------------------------------------------------------- */

/* Sous $XDG_RUNTIME_DIR, et non sous /run comme les lecteurs reseau : le
 * montage appartient a l'utilisateur, root n'a rien a y faire. Le chemin est
 * calcule ici ET dans claude-os-nuage ; les deux doivent rester d'accord. */
const char *
nuage_base_montage (void)
{
    static char *base = NULL;
    if (base == NULL)
        base = g_build_filename (g_get_user_runtime_dir (), "claude-os", "nuage", NULL);
    return base;
}

char *
nuage_point_montage (const LecteurNuage *l)
{
    return g_build_filename (nuage_base_montage (), l->id, NULL);
}

gboolean
nuage_est_connecte (const LecteurNuage *l)
{
    g_autofree char *point = nuage_point_montage (l);
    GUnixMountEntry *e = g_unix_mount_entry_at (point, NULL);

    if (e == NULL)
        return FALSE;
    g_unix_mount_entry_free (e);
    return TRUE;
}

char *
nuage_nom_du_point (const char *chemin)
{
    /* Sortie immediate hors de l'arborescence : cette fonction est appelee a
     * chaque changement de dossier, et relire un fichier de configuration
     * pour afficher /home/stef/Documents serait du gaspillage. */
    g_autofree char *prefixe = g_strconcat (nuage_base_montage (), "/", NULL);
    if (chemin == NULL || !g_str_has_prefix (chemin, prefixe))
        return NULL;

    g_autoptr(GPtrArray) lecteurs = nuage_charger ();
    for (guint i = 0; i < lecteurs->len; i++) {
        const LecteurNuage *l = g_ptr_array_index (lecteurs, i);
        g_autofree char *point = nuage_point_montage (l);
        if (g_strcmp0 (point, chemin) == 0)
            return g_strdup (l->nom);
    }
    return NULL;
}

gboolean
nuage_outil_present (void)
{
    g_autofree char *p = g_find_program_in_path ("rclone");
    return p != NULL;
}

/* Le moniteur est un singleton GIO, garde vivant volontairement. Aucune
 * scrutation : il ecoute la notification du noyau sur mountinfo. */
static GUnixMountMonitor *
moniteur_montages (void)
{
    static GUnixMountMonitor *moniteur = NULL;
    if (moniteur == NULL)
        moniteur = g_unix_mount_monitor_get ();
    return moniteur;
}

void
nuage_surveiller (NuageChangeFunc cb, gpointer data)
{
    g_signal_connect_swapped (moniteur_montages (), "mounts-changed",
                              G_CALLBACK (cb), data);
}

void
nuage_ne_plus_surveiller (gpointer data)
{
    /* Le moniteur survit a ses abonnes : un abonne detruit doit se
     * debrancher, sinon le prochain changement appellerait un rappel sur une
     * structure liberee. */
    g_signal_handlers_disconnect_by_data (moniteur_montages (), data);
}

/* -------------------------------------------------------------------------
 * Connexion
 * ------------------------------------------------------------------------- */
typedef struct {
    LecteurNuage  *lecteur;      /* NOTRE copie : voir plus bas */
    NuageFiniFunc  fini;
    gpointer       data;
} Tache;

static void
tache_free (Tache *t)
{
    nuage_free (t->lecteur);
    g_free (t);
}

static void
rendre (Tache *t, GError *erreur)
{
    if (t->fini != NULL)
        t->fini (t->lecteur, erreur, t->data);
}

/* La sortie d'erreur de claude-os-nuage, debarrassee de ce qui n'aide pas.
 * Sans ce message, l'utilisateur ne peut pas distinguer un trousseau ferme
 * d'un compte revoque. */
static char *
extraire_cause (const char *err)
{
    if (err == NULL)
        return g_strdup ("");

    g_auto(GStrv) lignes = g_strsplit (err, "\n", -1);
    GString *out = g_string_new (NULL);

    for (guint i = 0; lignes[i] != NULL; i++) {
        const char *l = g_strstrip (lignes[i]);
        if (*l == '\0')
            continue;
        g_string_append_printf (out, "%s%s", out->len > 0 ? "\n" : "", l);
    }
    return g_string_free (out, FALSE);
}

static void
on_processus_fini (GObject *src, GAsyncResult *res, gpointer data)
{
    GSubprocess *proc = G_SUBPROCESS (src);
    Tache *t = data;
    g_autoptr(GError) e = NULL;
    g_autofree char *sortie = NULL;
    g_autofree char *err = NULL;

    if (!g_subprocess_communicate_utf8_finish (proc, res, &sortie, &err, &e)) {
        rendre (t, e);
        tache_free (t);
        return;
    }

    if (!g_subprocess_get_successful (proc)) {
        g_autofree char *msg = extraire_cause (err);
        g_autoptr(GError) echec = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                                               *msg != '\0' ? msg
                                                            : "le montage a échoué sans message");
        rendre (t, echec);
        tache_free (t);
        return;
    }

    rendre (t, NULL);
    tache_free (t);
}

/* Lance « claude-os-nuage <verbe> <id> ». SANS claude-os-root, et c'est le
 * point : rclone monte sous le compte de l'utilisateur. Voir nuage.h. */
static void
lancer (const LecteurNuage *l, const char *verbe, NuageFiniFunc fini, gpointer data)
{
    /* NOTRE copie du lecteur, et cela n'est pas une precaution de style :
     * dans fichiers-lieux.c, tout ce qui vient de la liste meurt au prochain
     * reconstruire(). Trois usages apres liberation ont ete payes ainsi du
     * cote des lecteurs reseau (docs/08) ; on ne les rejoue pas ici. */
    Tache *t = g_new0 (Tache, 1);
    t->lecteur = nuage_copie (l);
    t->fini    = fini;
    t->data    = data;

    const char *argv[] = { "claude-os-nuage", verbe, t->lecteur->id, NULL };

    g_autoptr(GError) e = NULL;
    g_autoptr(GSubprocess) proc = g_subprocess_newv (
        argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &e);

    if (proc == NULL) {
        rendre (t, e);
        tache_free (t);
        return;
    }
    g_subprocess_communicate_utf8_async (proc, NULL, NULL, on_processus_fini, t);
}

void
nuage_connecter (const LecteurNuage *l, NuageFiniFunc fini, gpointer data)
{
    lancer (l, "monter", fini, data);
}

void
nuage_deconnecter (const LecteurNuage *l, NuageFiniFunc fini, gpointer data)
{
    lancer (l, "demonter", fini, data);
}
