/* O_NOFOLLOW n'est pas dans C11 seul : « -std=c11 » pose __STRICT_ANSI__, et
 * les extensions POSIX de fcntl.h disparaissent avec lui. */
#define _GNU_SOURCE

#include "reseau.h"

#include <gio/gunixmounts.h>
#include <glib/gstdio.h>
#include <libsecret/secret.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* Le point de montage vit sous /run : un tmpfs. Un redemarrage efface donc
 * l'arborescence entiere, et aucun repertoire mort ne s'accumule -- ce que
 * /media ou /mnt ne garantissent pas. */
#define RESEAU_BASE "/run/claude-os/reseau"

/* Le schema du trousseau. Les deux attributs suffisent a retrouver un secret
 * et a le distinguer de ceux des autres applications. */
static const SecretSchema *
schema_trousseau (void)
{
    static const SecretSchema s = {
        "org.claude-os.LecteurReseau", SECRET_SCHEMA_NONE,
        {
            { "lecteur", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "serveur", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { NULL, 0 },
        },
        0, 0, 0, 0, 0, 0, 0, 0
    };
    return &s;
}

/* ------------------------------------------------------------------------- */
static const struct {
    ReseauProtocole p;
    const char     *id;
    const char     *nom;
} PROTOCOLES[] = {
    { RESEAU_SMB,  "smb",  "Partage Windows / NAS (SMB)" },
    { RESEAU_NFS,  "nfs",  "Partage Unix (NFS)"          },
    { RESEAU_SFTP, "sftp", "Dossier distant par SSH (SFTP)" },
    { RESEAU_DAV,  "dav",  "WebDAV (Nextcloud, ownCloud)"   },
};

const char *
reseau_protocole_id (ReseauProtocole p)
{
    for (guint i = 0; i < G_N_ELEMENTS (PROTOCOLES); i++)
        if (PROTOCOLES[i].p == p)
            return PROTOCOLES[i].id;
    return "smb";
}

const char *
reseau_protocole_nom (ReseauProtocole p)
{
    for (guint i = 0; i < G_N_ELEMENTS (PROTOCOLES); i++)
        if (PROTOCOLES[i].p == p)
            return PROTOCOLES[i].nom;
    return PROTOCOLES[0].nom;
}

ReseauProtocole
reseau_protocole_lire (const char *id)
{
    for (guint i = 0; i < G_N_ELEMENTS (PROTOCOLES); i++)
        if (g_strcmp0 (PROTOCOLES[i].id, id) == 0)
            return PROTOCOLES[i].p;
    return RESEAU_SMB;
}

/* ------------------------------------------------------------------------- */
void
lecteur_free (Lecteur *l)
{
    if (l == NULL)
        return;
    g_free (l->id);
    g_free (l->nom);
    g_free (l->serveur);
    g_free (l->partage);
    g_free (l->utilisateur);
    g_free (l->domaine);
    g_free (l->schema);
    g_free (l->options);
    g_free (l);
}

Lecteur *
lecteur_copie (const Lecteur *l)
{
    Lecteur *c = g_new0 (Lecteur, 1);
    c->id          = g_strdup (l->id);
    c->nom         = g_strdup (l->nom);
    c->protocole   = l->protocole;
    c->serveur     = g_strdup (l->serveur);
    c->partage     = g_strdup (l->partage);
    c->utilisateur = g_strdup (l->utilisateur);
    c->domaine     = g_strdup (l->domaine);
    c->schema      = g_strdup (l->schema);
    c->options     = g_strdup (l->options);
    c->automatique = l->automatique;
    return c;
}

/* ------------------------------------------------------------------------- */
static char *
chemin_config (void)
{
    return g_build_filename (g_get_user_config_dir (), "claude-os", "lecteurs", NULL);
}

/* Une chaine du fichier, jamais NULL : les champs absents valent "". Cela
 * evite un test de nullite a chaque usage, et un champ vide se distingue mal
 * d'un champ absent de toute facon. */
static char *
cle (GKeyFile *kf, const char *groupe, const char *nom)
{
    char *v = g_key_file_get_string (kf, groupe, nom, NULL);
    return (v != NULL) ? v : g_strdup ("");
}

GPtrArray *
reseau_charger (void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) lecteur_free);

    g_autoptr(GKeyFile) kf = g_key_file_new ();
    g_autofree char *chemin = chemin_config ();
    g_autoptr(GError) e = NULL;

    if (!g_key_file_load_from_file (kf, chemin, G_KEY_FILE_KEEP_COMMENTS, &e)) {
        /* Absent : c'est le cas normal tant qu'aucun lecteur n'a ete declare.
         * Toute autre erreur se dit -- un fichier illisible ou mal forme
         * ferait autrement disparaitre les lecteurs sans un mot. */
        if (!g_error_matches (e, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_warning ("lecteurs réseau illisibles (%s) : %s", chemin, e->message);
        return out;
    }

    gsize n = 0;
    g_auto(GStrv) groupes = g_key_file_get_groups (kf, &n);

    for (gsize i = 0; i < n; i++) {
        Lecteur *l = g_new0 (Lecteur, 1);
        l->id          = g_strdup (groupes[i]);
        l->nom         = cle (kf, groupes[i], "nom");
        l->serveur     = cle (kf, groupes[i], "serveur");
        l->partage     = cle (kf, groupes[i], "partage");
        l->utilisateur = cle (kf, groupes[i], "utilisateur");
        l->domaine     = cle (kf, groupes[i], "domaine");
        l->schema      = cle (kf, groupes[i], "schema");
        l->options     = cle (kf, groupes[i], "options");

        g_autofree char *p = cle (kf, groupes[i], "protocole");
        l->protocole   = reseau_protocole_lire (p);
        l->automatique = g_key_file_get_boolean (kf, groupes[i], "auto", NULL);

        if (*l->nom == '\0') {
            g_free (l->nom);
            l->nom = g_strdup (l->id);
        }
        g_ptr_array_add (out, l);
    }
    return out;
}

gboolean
reseau_enregistrer (GPtrArray *lecteurs, GError **erreur)
{
    g_autoptr(GKeyFile) kf = g_key_file_new ();

    for (guint i = 0; i < lecteurs->len; i++) {
        const Lecteur *l = g_ptr_array_index (lecteurs, i);
        g_key_file_set_string  (kf, l->id, "nom",         l->nom);
        g_key_file_set_string  (kf, l->id, "protocole",   reseau_protocole_id (l->protocole));
        g_key_file_set_string  (kf, l->id, "serveur",     l->serveur);
        g_key_file_set_string  (kf, l->id, "partage",     l->partage);
        g_key_file_set_string  (kf, l->id, "utilisateur", l->utilisateur);
        if (*l->domaine != '\0')
            g_key_file_set_string (kf, l->id, "domaine", l->domaine);
        if (l->protocole == RESEAU_DAV)
            g_key_file_set_string (kf, l->id, "schema", *l->schema ? l->schema : "https");
        if (*l->options != '\0')
            g_key_file_set_string (kf, l->id, "options", l->options);
        g_key_file_set_boolean (kf, l->id, "auto", l->automatique);
    }

    g_key_file_set_comment (kf, NULL, NULL,
        " Lecteurs réseau de Claude OS.\n"
        " Écrit par Réglages › Lecteurs réseau. Une section par lecteur ;\n"
        " le nom de section sert de nom au répertoire de montage.\n"
        " AUCUN MOT DE PASSE ICI : ils sont dans le trousseau (gnome-keyring).",
        NULL);

    g_autofree char *chemin = chemin_config ();
    g_autofree char *dossier = g_path_get_dirname (chemin);
    g_mkdir_with_parents (dossier, 0700);

    return g_key_file_save_to_file (kf, chemin, erreur);
}

char *
reseau_id_depuis_nom (const char *nom, GPtrArray *existants)
{
    GString *s = g_string_new (NULL);

    /* L'identifiant devient un nom de repertoire et une cle de section : on
     * le reduit a ce qui ne pose de question ni au shell ni au systeme de
     * fichiers. Le libelle affiche, lui, garde ses accents et ses espaces. */
    for (const char *p = nom; *p != '\0'; p = g_utf8_next_char (p)) {
        gunichar c = g_utf8_get_char (p);
        if (g_ascii_isalnum ((char) c))
            g_string_append_c (s, g_ascii_tolower ((char) c));
        else if (s->len > 0 && s->str[s->len - 1] != '-')
            g_string_append_c (s, '-');
    }
    while (s->len > 0 && s->str[s->len - 1] == '-')
        g_string_truncate (s, s->len - 1);
    if (s->len == 0)
        g_string_assign (s, "lecteur");

    /* Unicite : deux lecteurs de meme identifiant se monteraient au meme
     * endroit et partageraient leur mot de passe. */
    g_autofree char *base = g_strdup (s->str);
    for (guint n = 2; existants != NULL; n++) {
        gboolean pris = FALSE;
        for (guint i = 0; i < existants->len; i++)
            if (g_strcmp0 (((Lecteur *) g_ptr_array_index (existants, i))->id, s->str) == 0)
                pris = TRUE;
        if (!pris)
            break;
        g_string_printf (s, "%s-%u", base, n);
    }
    return g_string_free (s, FALSE);
}

/* ------------------------------------------------------------------------- */
const char *
reseau_base_montage (void)
{
    return RESEAU_BASE;
}

char *
reseau_point_montage (const Lecteur *l)
{
    return g_build_filename (RESEAU_BASE, l->id, NULL);
}

gboolean
reseau_est_connecte (const Lecteur *l)
{
    g_autofree char *point = reseau_point_montage (l);
    GUnixMountEntry *e = g_unix_mount_entry_at (point, NULL);

    if (e == NULL)
        return FALSE;
    g_unix_mount_entry_free (e);
    return TRUE;
}

/* Le moniteur est un singleton GIO, garde vivant volontairement : il vit
 * aussi longtemps que le programme. Aucune scrutation -- il ecoute la
 * notification du noyau sur /proc/self/mountinfo. */
static GUnixMountMonitor *
moniteur_montages (void)
{
    static GUnixMountMonitor *moniteur = NULL;
    if (moniteur == NULL)
        moniteur = g_unix_mount_monitor_get ();
    return moniteur;
}

char *
reseau_nom_du_point (const char *chemin)
{
    /* Sortie immediate hors de l'arborescence des lecteurs : cette fonction
     * est appelee a chaque changement de dossier, et il ne faut pas relire
     * un fichier de configuration pour afficher /home/stef/Documents. */
    if (chemin == NULL || !g_str_has_prefix (chemin, RESEAU_BASE "/"))
        return NULL;

    g_autoptr(GPtrArray) lecteurs = reseau_charger ();
    for (guint i = 0; i < lecteurs->len; i++) {
        const Lecteur *l = g_ptr_array_index (lecteurs, i);
        g_autofree char *point = reseau_point_montage (l);
        if (g_strcmp0 (point, chemin) == 0)
            return g_strdup (l->nom);
    }
    return NULL;
}

void
reseau_surveiller (ReseauChangeFunc cb, gpointer data)
{
    g_signal_connect_swapped (moniteur_montages (), "mounts-changed",
                              G_CALLBACK (cb), data);
}

void
reseau_ne_plus_surveiller (gpointer data)
{
    /* Le moniteur survivant a ses abonnes, un abonne detruit doit se
     * debrancher lui-meme : sinon le prochain changement de montage
     * appellerait un rappel sur une structure liberee. */
    g_signal_handlers_disconnect_by_data (moniteur_montages (), data);
}

/* -------------------------------------------------------------------------
 * Connexion
 * ------------------------------------------------------------------------- */
typedef struct {
    Lecteur        *lecteur;
    /* Facultatif. Certains appels sont des « pose et oublie » -- deconnecter
     * un lecteur qu'on supprime -- et n'ont personne a prevenir. */
    ReseauFiniFunc  fini;
    gpointer        data;
    char           *identifiants;   /* fichier a effacer, ou NULL           */
    char           *mot_de_passe;   /* a retenir apres succes, ou NULL      */
} Tache;

/* Previent l'appelant, s'il a demande a l'etre. */
static void
rendre (Tache *t, GError *erreur)
{
    if (t->fini != NULL)
        t->fini (t->lecteur, erreur, t->data);
    else if (erreur != NULL)
        g_warning ("lecteur « %s » : %s", t->lecteur->nom, erreur->message);
}

static void
tache_free (Tache *t)
{
    /* Le fichier d'identifiants dispara3it des que mount l'a lu. Le laisser
     * trainer, meme en 0600 dans un tmpfs, serait une fenetre inutile. */
    if (t->identifiants != NULL) {
        if (g_unlink (t->identifiants) != 0 && errno != ENOENT)
            g_warning ("identifiants non effacés (%s) : %s",
                       t->identifiants, g_strerror (errno));
        g_free (t->identifiants);
    }
    if (t->mot_de_passe != NULL) {
        secret_password_wipe (t->mot_de_passe);
        g_free (t->mot_de_passe);
    }
    lecteur_free (t->lecteur);
    g_free (t);
}

static void
on_secret_ecrit (GObject *src, GAsyncResult *res, gpointer data)
{
    (void) src; (void) data;
    g_autoptr(GError) e = NULL;
    if (!secret_password_store_finish (res, &e))
        g_warning ("mot de passe non retenu dans le trousseau : %s", e->message);
}

/* Ne garde, dans la sortie d'erreur, que ce qui vient du montage. Les lignes
 * du guichet et les renvois a la page de manuel n'apprennent rien a qui lit
 * une boite de dialogue. */
static char *
extraire_cause (const char *brut)
{
    if (brut == NULL)
        return g_strdup ("");

    g_auto(GStrv) lignes = g_strsplit (brut, "\n", -1);
    GString *out = g_string_new (NULL);

    for (guint i = 0; lignes[i] != NULL; i++) {
        const char *l = g_strstrip (lignes[i]);
        if (*l == '\0')
            continue;
        if (g_str_has_prefix (l, "claude-os-root"))
            continue;
        if (g_str_has_prefix (l, "Refer to the "))
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
        /* Le message de mount est la seule chose qui dise POURQUOI : sans
         * lui, l'utilisateur ne peut pas distinguer un mot de passe faux
         * d'un serveur eteint. On le remonte, debarrasse de ce qui n'est
         * pas de lui.
         *
         * Le guichet ecrit sa propre banniere sur la meme sortie d'erreur --
         * « aucun instantane pris… », qui parle de btrfs et non du montage.
         * Collee en tete du message, elle occupait la premiere ligne de la
         * boite de dialogue et repoussait la vraie cause hors de vue. */
        g_autofree char *msg = extraire_cause (err);
        g_autoptr(GError) echec = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                                               *msg != '\0' ? msg
                                                            : "le montage a échoué sans message");
        rendre (t, echec);
        tache_free (t);
        return;
    }

    if (t->mot_de_passe != NULL) {
        g_autofree char *libelle = g_strdup_printf ("Lecteur réseau « %s »", t->lecteur->nom);
        secret_password_store (schema_trousseau (), SECRET_COLLECTION_DEFAULT,
                               libelle, t->mot_de_passe, NULL,
                               on_secret_ecrit, NULL,
                               "lecteur", t->lecteur->id,
                               "serveur", t->lecteur->serveur, NULL);
    }

    rendre (t, NULL);
    tache_free (t);
}

/* Lance « claude-os-root claude-os-lecteur <verbe> <id> [identifiants] ».
 * L'interface ne compose jamais elle-meme une commande de montage : voir la
 * tete de claude-os-lecteur pour le raisonnement. */
static void
lancer (Tache *t, const char *verbe)
{
    const char *argv[8];
    guint n = 0;
    argv[n++] = "claude-os-root";
    argv[n++] = "--pourquoi";
    /* Le motif part dans /var/log/claude-os/actions.log a cote de la
     * commande : six mois plus tard, la trace se relit. */
    g_autofree char *motif = g_strdup_printf ("lecteur réseau « %s » : %s",
                                              t->lecteur->nom, verbe);
    argv[n++] = motif;
    argv[n++] = "claude-os-lecteur";
    argv[n++] = verbe;
    argv[n++] = t->lecteur->id;
    if (t->identifiants != NULL)
        argv[n++] = t->identifiants;
    argv[n] = NULL;

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

/* Ecrit le fichier que mount.cifs attend. En 0600 dans /run/user, c'est-a-
 * dire dans un tmpfs prive : le mot de passe ne touche jamais le disque. */
static char *
ecrire_identifiants (const Lecteur *l, const char *mot_de_passe, GError **erreur)
{
    g_autofree char *dossier = g_build_filename (g_get_user_runtime_dir (),
                                                 "claude-os", NULL);
    if (g_mkdir_with_parents (dossier, 0700) != 0) {
        g_set_error (erreur, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "%s : %s", dossier, g_strerror (errno));
        return NULL;
    }

    g_autofree char *chemin = g_build_filename (dossier, l->id, NULL);
    g_autofree char *contenu = g_strdup_printf (
        "username=%s\npassword=%s\n%s%s%s",
        l->utilisateur, mot_de_passe,
        (*l->domaine != '\0') ? "domain=" : "", l->domaine,
        (*l->domaine != '\0') ? "\n" : "");

    /* Cree en 0600 DES L'OUVERTURE. Passer par g_file_set_contents puis
     * chmod laisserait le fichier lisible par tous entre les deux appels --
     * bref, mais suffisant. */
    int fd = g_open (chemin, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        g_set_error (erreur, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "%s : %s", chemin, g_strerror (errno));
        return NULL;
    }

    gsize reste = strlen (contenu);
    const char *p = contenu;
    while (reste > 0) {
        gssize ecrit = write (fd, p, reste);
        if (ecrit < 0) {
            if (errno == EINTR)
                continue;
            g_set_error (erreur, G_FILE_ERROR, g_file_error_from_errno (errno),
                         "écriture des identifiants : %s", g_strerror (errno));
            close (fd);
            g_unlink (chemin);
            return NULL;
        }
        p += ecrit; reste -= (gsize) ecrit;
    }
    close (fd);
    secret_password_wipe (contenu);
    return g_steal_pointer (&chemin);
}

static void
connecter_avec (Tache *t, const char *mot_de_passe)
{
    if (mot_de_passe != NULL && *mot_de_passe != '\0'
        && (t->lecteur->protocole == RESEAU_SMB || t->lecteur->protocole == RESEAU_DAV)) {
        g_autoptr(GError) e = NULL;
        t->identifiants = ecrire_identifiants (t->lecteur, mot_de_passe, &e);
        if (t->identifiants == NULL) {
            rendre (t, e);
            tache_free (t);
            return;
        }
    }
    lancer (t, "monter");
}

static void
on_secret_lu (GObject *src, GAsyncResult *res, gpointer data)
{
    (void) src;
    Tache *t = data;
    g_autoptr(GError) e = NULL;
    char *secret = secret_password_lookup_finish (res, &e);

    if (e != NULL)
        g_warning ("trousseau illisible pour « %s » : %s", t->lecteur->id, e->message);

    /* Aucun secret retenu : on tente quand meme. Beaucoup de partages sont
     * ouverts en invite, et echouer sans avoir essaye serait faux. */
    connecter_avec (t, secret);
    if (secret != NULL) {
        secret_password_wipe (secret);
        secret_password_free (secret);
    }
}

void
reseau_connecter (const Lecteur *l, const char *mot_de_passe,
                  gboolean retenir, ReseauFiniFunc fini, gpointer data)
{
    Tache *t = g_new0 (Tache, 1);
    t->lecteur = lecteur_copie (l);
    t->fini    = fini;
    t->data    = data;

    if (mot_de_passe != NULL) {
        if (retenir)
            t->mot_de_passe = g_strdup (mot_de_passe);
        connecter_avec (t, mot_de_passe);
        return;
    }

    secret_password_lookup (schema_trousseau (), NULL, on_secret_lu, t,
                            "lecteur", l->id,
                            "serveur", l->serveur, NULL);
}

void
reseau_deconnecter (const Lecteur *l, ReseauFiniFunc fini, gpointer data)
{
    Tache *t = g_new0 (Tache, 1);
    t->lecteur = lecteur_copie (l);
    t->fini    = fini;
    t->data    = data;
    lancer (t, "demonter");
}

/* ------------------------------------------------------------------------- */
void
reseau_secret_present (const Lecteur *l, GAsyncReadyCallback cb, gpointer data)
{
    secret_password_lookup (schema_trousseau (), NULL, cb, data,
                            "lecteur", l->id,
                            "serveur", l->serveur, NULL);
}

gboolean
reseau_secret_present_fin (GAsyncResult *res)
{
    g_autoptr(GError) e = NULL;
    char *secret = secret_password_lookup_finish (res, &e);

    if (e != NULL)
        g_warning ("trousseau illisible : %s", e->message);
    if (secret == NULL)
        return FALSE;

    secret_password_wipe (secret);
    secret_password_free (secret);
    return TRUE;
}

static void
on_secret_efface (GObject *src, GAsyncResult *res, gpointer data)
{
    (void) src; (void) data;
    g_autoptr(GError) e = NULL;
    if (!secret_password_clear_finish (res, &e))
        g_warning ("mot de passe non effacé du trousseau : %s", e->message);
}

void
reseau_secret_effacer (const Lecteur *l)
{
    secret_password_clear (schema_trousseau (), NULL, on_secret_efface, NULL,
                           "lecteur", l->id,
                           "serveur", l->serveur, NULL);
}
