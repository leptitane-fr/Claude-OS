/* =========================================================================
 * Claude OS — le coffre
 *
 * Le correspondant privilégié de l'écran de connexion. Il garde le mot de
 * passe de l'utilisateur, scellé par son code PIN, et le rend contre ce
 * code. Il sert aussi le nom du thème du bureau.
 *
 * POURQUOI IL EXISTE
 *
 * L'écran de connexion tourne sous « _greetd », uid 102, sans groupe. Deux
 * choses lui sont donc hors de portée, et elles lui sont indispensables :
 *
 *   - le thème du bureau, qui est dans ~/.config/claude-os/shell.conf, sous
 *     un /home/<user> en 0700 ;
 *   - le secret du PIN, qui n'a rien à faire dans un fichier lisible par un
 *     compte non privilégié.
 *
 * Le greeter avait déjà du code pour lire le thème. Il ne pouvait pas
 * marcher : g_key_file_load_from_file échouait à chaque ouverture, sans un
 * mot, et le repli s'appliquait toujours. C'est ce trou-là que ce programme
 * bouche, en même temps qu'il apporte le PIN.
 *
 * POURQUOI LE PIN DÉVERROUILLE LE MOT DE PASSE AU LIEU DE LE REMPLACER
 *
 * /etc/pam.d/greetd charge pam_gnome_keyring.so. PAM doit donc recevoir le
 * VRAI mot de passe : c'est lui qui ouvre le trousseau de session, où sont
 * rangés les mots de passe des lecteurs réseau. Un module PAM maison qui
 * validerait le PIN authentifierait l'utilisateur et laisserait le trousseau
 * fermé — la session s'ouvrirait, et les partages seraient inaccessibles
 * sans qu'on comprenne pourquoi.
 *
 * Le PIN n'est donc pas un mot de passe de rechange. C'est une clé de
 * coffre, et le mot de passe est dedans.
 *
 * CE QU'IL NE FAIT PAS
 *
 * Il ne touche pas à PAM et ne vérifie aucun mot de passe. « enroler » scelle
 * ce qu'on lui donne. C'est délibéré : ajouter PAM ici recréerait le
 * programme privilégié que connexion.c s'est donné pour règle de ne pas
 * être. Le greeter n'appelle « enroler » qu'après un « success » de greetd,
 * donc après que PAM a validé.
 *
 * CE QUI LE PROTÈGE
 *
 * La socket, et elle seule : SocketUser=_greetd, SocketMode=0600. Pas de
 * setuid, pas de sudoers, aucun chemin d'exécution depuis un compte
 * ordinaire. Le contrôle SO_PEERCRED ci-dessous répète la règle DANS le
 * programme, pour qu'une unité systemd modifiée ne suffise pas à l'ouvrir.
 *
 * CE QU'IL AFFAIBLIT — À SAVOIR AVANT DE S'EN SERVIR
 *
 * Un PIN à six chiffres n'a qu'un million de combinaisons, et le disque de
 * cette machine n'est pas chiffré. Qui démonte l'eMMC obtient ce coffre et
 * peut l'attaquer hors ligne, là où /etc/shadow ne lui donnerait qu'un
 * hachage yescrypt bien plus coûteux. Argon2id et le compteur d'essais
 * rendent l'attaque chère, ils ne la rendent pas impossible. Le détail du
 * calcul est dans docs/09-code-pin.md ; le seul vrai remède est le
 * chiffrement du disque.
 *
 * PROTOCOLE — une requête JSON par ligne, une réponse JSON, puis fermeture.
 *
 *   {"verbe":"etat",    "utilisateur":"stef"}
 *       -> {"pin":true,"essais_restants":5,"theme":"sombre"}
 *   {"verbe":"ouvrir",  "utilisateur":"stef","pin":"123456"}
 *       -> {"mdp":"…"}  |  {"erreur":"pin_faux","essais_restants":3}
 *   {"verbe":"enroler", "utilisateur":"stef","mdp":"…","pin":"123456"}
 *       -> {"ok":true}
 *   {"verbe":"oublier", "utilisateur":"stef"}
 *       -> {"ok":true}
 * ========================================================================= */

#define _GNU_SOURCE

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <gcrypt.h>

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <pwd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define COFFRE_DIR   "/var/lib/claude-os/coffre"
#define ESSAIS_MAX   5
#define REQUETE_MAX  (64 * 1024)

/* PARAMÈTRES ARGON2ID — MESURÉS SUR MADOO, PAS RECOPIÉS.
 *
 * Pentium Silver N6000 à 1,10 GHz, le 9 septembre 2026, avec ces valeurs :
 * 0,63 s par tentative. Le tableau complet des mesures est dans docs/09.
 *
 * 128 Mio est le point où le coût devient sensible pour qui attaque par
 * carte graphique — Argon2id y est limité par la bande passante mémoire —
 * sans peser sur une machine qui n'a que 4 Go soudés : au moment où le
 * coffre travaille, l'écran de connexion est seul à l'écran.
 *
 * p=1 et non 4 : gcry_kdf_compute(h, NULL) déroule les voies en série. Un
 * p plus grand multiplierait le temps d'attente sans rien coûter de plus à
 * l'attaquant, qui, lui, paralléliserait. */
#define ARGON2_M     131072   /* Kio, soit 128 Mio */
#define ARGON2_T     3
#define ARGON2_P     1
#define CLE_OCTETS   32       /* AES-256 */
#define SEL_OCTETS   16
#define NONCE_OCTETS 12       /* la taille naturelle de GCM */
#define TAG_OCTETS   16

/* Les thèmes que le shell connaît. Le coffre ne renvoie RIEN d'autre : il
 * lit un fichier du répertoire personnel en tant que root, et une liste
 * blanche est ce qui garantit qu'aucun contenu arbitraire ne peut remonter
 * de là vers « _greetd ». « light » et « dark » y figurent parce que
 * config.c les accepte encore — voir theme_par_id(). */
static const char *const THEMES[] = {
    "clair", "sombre", "claude-clair", "claude-sombre", "light", "dark", NULL
};

/* Le repli, quand shell.conf est illisible ou absent. « sombre » et non
 * « claude-sombre » : c'est le thème réellement en service sur cette
 * machine, et un écran de connexion qui ne ressemble pas au bureau qu'il
 * précède est exactement le défaut qu'on corrige ici. */
#define THEME_REPLI  "sombre"

static void
journal (const char *format, ...)
{
    va_list args;
    va_start (args, format);
    g_autofree char *texte = g_strdup_vprintf (format, args);
    va_end (args);

    /* stderr part au journal systemd : c'est la seule trace qu'un service
     * activé par socket puisse laisser.
     *
     * fputs ET NON g_printerr : celle-ci transcode vers l'encodage de la
     * locale, et un service systemd n'en a pas — LANG est vide, donc la
     * locale est « C ». Tous les accents et tous les guillemets de ces
     * messages ressortaient en « ? » dans journalctl. Un journal qu'on
     * relit mal est un journal qu'on ne relit pas. */
    fputs ("claude-os-coffre: ", stderr);
    fputs (texte, stderr);
    fputc ('\n', stderr);
}

/* -------------------------------------------------------------------------
 * Réponses
 * ------------------------------------------------------------------------- */
static void
emettre (JsonBuilder *b)
{
    g_autoptr(JsonNode) racine = json_builder_get_root (b);
    g_autoptr(JsonGenerator) gen = json_generator_new ();
    json_generator_set_root (gen, racine);

    gsize n = 0;
    g_autofree char *texte = json_generator_to_data (gen, &n);

    /* Une écriture partielle sur une socket est possible : on boucle. Un
     * write() dont on ne lit pas le retour est la façon la plus discrète de
     * perdre une réponse. */
    gsize ecrit = 0;
    while (ecrit < n) {
        gssize r = write (STDOUT_FILENO, texte + ecrit, n - ecrit);
        if (r <= 0) {
            if (errno == EINTR) continue;
            journal ("réponse tronquée : %s", g_strerror (errno));
            return;
        }
        ecrit += (gsize) r;
    }
    if (write (STDOUT_FILENO, "\n", 1) != 1)
        journal ("fin de ligne non écrite : %s", g_strerror (errno));
}

static void
repondre_erreur (const char *code, int essais_restants)
{
    g_autoptr(JsonBuilder) b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "erreur");
    json_builder_add_string_value (b, code);
    if (essais_restants >= 0) {
        json_builder_set_member_name (b, "essais_restants");
        json_builder_add_int_value (b, essais_restants);
    }
    json_builder_end_object (b);
    emettre (b);
}

static void
repondre_ok (void)
{
    g_autoptr(JsonBuilder) b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "ok");
    json_builder_add_boolean_value (b, TRUE);
    json_builder_end_object (b);
    emettre (b);
}

/* -------------------------------------------------------------------------
 * Validation des entrées
 *
 * Tout ce qui vient de la socket passe par ici avant de toucher un chemin
 * de fichier ou une fonction de chiffrement.
 * ------------------------------------------------------------------------- */

/* Le compte doit EXISTER et être humain. C'est aussi ce qui interdit la
 * traversée de répertoire : le nom retenu n'est pas celui reçu, c'est
 * pw_name tel que la base des comptes le rend. Un « ../../etc » ne
 * ressort pas de getpwnam. */
static char *
compte_valide (const char *demande)
{
    if (demande == NULL || *demande == '\0')
        return NULL;

    struct passwd *p = getpwnam (demande);
    if (p == NULL)
        return NULL;
    if (p->pw_uid < 1000 || p->pw_uid >= 60000)
        return NULL;

    /* Ceinture et bretelles : même venu de la base des comptes, un nom qui
     * contiendrait une barre oblique ne servira pas à fabriquer un chemin. */
    for (const char *c = p->pw_name; *c != '\0'; c++)
        if (!g_ascii_isalnum (*c) && *c != '.' && *c != '_' && *c != '-')
            return NULL;

    return g_strdup (p->pw_name);
}

/* Six chiffres, exactement. Ni cinq, ni sept, ni un chiffre unicode. */
static gboolean
pin_valide (const char *pin)
{
    if (pin == NULL || strlen (pin) != 6)
        return FALSE;
    for (int i = 0; i < 6; i++)
        if (pin[i] < '0' || pin[i] > '9')
            return FALSE;
    return TRUE;
}

static char *
chemin (const char *compte, const char *suffixe)
{
    g_autofree char *base = g_strconcat (compte, suffixe, NULL);
    return g_build_filename (COFFRE_DIR, base, NULL);
}

/* -------------------------------------------------------------------------
 * Écriture de fichier — atomique, et vraiment sur le disque
 *
 * Le compteur d'essais ne vaut que s'il SURVIT à une coupure de courant.
 * Sans fsync, il vit dans le cache de page : couper l'alimentation juste
 * après une tentative ratée le remettrait à zéro, et le compteur — la seule
 * vraie protection d'un PIN à six chiffres — deviendrait décoratif.
 * ------------------------------------------------------------------------- */
static gboolean
ecrire_scelle (const char *destination, const char *contenu)
{
    g_autofree char *temporaire = g_strconcat (destination, ".neuf", NULL);

    /* O_EXCL : si un fichier temporaire traîne, on ne le suit pas
     * aveuglément — il pourrait être un lien posé là. */
    g_unlink (temporaire);
    int fd = open (temporaire, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        journal ("création de %s : %s", temporaire, g_strerror (errno));
        return FALSE;
    }

    gsize n = strlen (contenu), ecrit = 0;
    while (ecrit < n) {
        gssize r = write (fd, contenu + ecrit, n - ecrit);
        if (r <= 0) {
            if (errno == EINTR) continue;
            journal ("écriture de %s : %s", temporaire, g_strerror (errno));
            close (fd);
            g_unlink (temporaire);
            return FALSE;
        }
        ecrit += (gsize) r;
    }

    if (fsync (fd) != 0) {
        journal ("fsync de %s : %s", temporaire, g_strerror (errno));
        close (fd);
        g_unlink (temporaire);
        return FALSE;
    }
    close (fd);

    if (g_rename (temporaire, destination) != 0) {
        journal ("renommage vers %s : %s", destination, g_strerror (errno));
        g_unlink (temporaire);
        return FALSE;
    }

    /* Le renommage lui-même doit atteindre le disque, sans quoi l'ancien
     * contenu peut réapparaître après une coupure. */
    int rep = open (COFFRE_DIR, O_RDONLY | O_DIRECTORY);
    if (rep >= 0) { fsync (rep); close (rep); }

    return TRUE;
}

static char *
lire_fichier (const char *source)
{
    int fd = open (source, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return NULL;

    struct stat st;
    if (fstat (fd, &st) != 0 || !S_ISREG (st.st_mode)
        || st.st_size <= 0 || st.st_size > REQUETE_MAX) {
        close (fd);
        return NULL;
    }

    g_autofree char *tampon = g_malloc0 ((gsize) st.st_size + 1);
    gsize lu = 0;
    while (lu < (gsize) st.st_size) {
        gssize r = read (fd, tampon + lu, (gsize) st.st_size - lu);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        lu += (gsize) r;
    }
    close (fd);

    if (lu != (gsize) st.st_size)
        return NULL;
    return g_steal_pointer (&tampon);
}

/* -------------------------------------------------------------------------
 * Le compteur d'essais
 * ------------------------------------------------------------------------- */
static int
essais_lus (const char *compte)
{
    g_autofree char *c = chemin (compte, ".essais");
    g_autofree char *texte = lire_fichier (c);
    if (texte == NULL)
        return 0;

    gint64 n = g_ascii_strtoll (texte, NULL, 10);
    if (n < 0)             return 0;
    if (n > ESSAIS_MAX)    return ESSAIS_MAX;
    return (int) n;
}

static gboolean
essais_ecrits (const char *compte, int n)
{
    g_autofree char *c = chemin (compte, ".essais");
    g_autofree char *texte = g_strdup_printf ("%d\n", n);
    return ecrire_scelle (c, texte);
}

static void
essais_remis_a_zero (const char *compte)
{
    g_autofree char *c = chemin (compte, ".essais");
    if (g_unlink (c) != 0 && errno != ENOENT)
        journal ("le compteur d'essais de « %s » n'a pas pu être effacé : %s",
                 compte, g_strerror (errno));
}

/* Le PIN n'est pas « suspendu » : il cesse d'exister. Un PIN désactivé mais
 * conservé serait un secret de plus à garder, pour aucun bénéfice — le
 * mot de passe reste, lui, toujours accepté. */
static void
pin_efface (const char *compte)
{
    g_autofree char *c = chemin (compte, ".pin");
    if (g_unlink (c) != 0 && errno != ENOENT)
        journal ("le coffre de « %s » n'a pas pu être effacé : %s",
                 compte, g_strerror (errno));
    essais_remis_a_zero (compte);
}

/* -------------------------------------------------------------------------
 * Chiffrement
 * ------------------------------------------------------------------------- */

/* Ce qui est authentifié SANS être chiffré, et qui doit donc être exact
 * pour que le déchiffrement réussisse : le nom du compte et les paramètres
 * du KDF.
 *
 * Le compte, pour qu'un coffre recopié d'un utilisateur à l'autre ne
 * s'ouvre pas. Les paramètres, pour qu'on ne puisse pas abaisser le coût
 * d'Argon2 dans le fichier — mettre m=8 et t=1 — afin de rendre l'attaque
 * hors ligne mille fois plus rapide. Modifier l'un ou l'autre casse le tag
 * GCM, et le coffre refuse. */
static char *
authentifie (const char *compte, unsigned long m, unsigned long t, unsigned long p)
{
    return g_strdup_printf ("claude-os-coffre-v1|%s|argon2id|m=%lu|t=%lu|p=%lu",
                            compte, m, t, p);
}

/* Argon2id, par gcry_kdf_open/compute/final.
 *
 * PIÈGE : gcry_kdf_derive() — la fonction évidente, celle dont le nom dit
 * qu'elle dérive une clé — NE SAIT PAS faire Argon2. Elle rend
 * « Invalid value » sans plus d'explication. Argon2 n'existe que dans
 * l'API à poignée, avec ses quatre paramètres dans un ordre que l'en-tête
 * ne documente pas.
 *
 * Cet ordre — {taglen, t, m, p} — a été ÉTABLI, non supposé : les quatre
 * ordres plausibles ont été essayés contre le vecteur de test Argon2id de
 * la RFC 9106, §5.3, et un seul le reproduit. */
static gboolean
deriver (const char *pin, const guchar *sel, gsize sel_n,
         const char *aad, guchar *cle_out)
{
    const unsigned long param[4] = { CLE_OCTETS, ARGON2_T, ARGON2_M, ARGON2_P };

    gcry_kdf_hd_t h = NULL;
    gcry_error_t e = gcry_kdf_open (&h, GCRY_KDF_ARGON2, GCRY_KDF_ARGON2ID,
                                    param, G_N_ELEMENTS (param),
                                    pin, strlen (pin), sel, sel_n,
                                    NULL, 0, aad, strlen (aad));
    if (e != 0) {
        journal ("gcry_kdf_open : %s", gcry_strerror (e));
        return FALSE;
    }

    /* NULL : pas de fil d'exécution auxiliaire. Avec p=1 il n'y a rien à
     * paralléliser, et le calcul se fait ici même. */
    e = gcry_kdf_compute (h, NULL);
    if (e == 0)
        e = gcry_kdf_final (h, CLE_OCTETS, cle_out);
    gcry_kdf_close (h);

    if (e != 0) {
        journal ("dérivation Argon2id : %s", gcry_strerror (e));
        return FALSE;
    }
    return TRUE;
}

/* Scelle le mot de passe. Rend le contenu du fichier .pin, ou NULL. */
static char *
sceller (const char *compte, const char *pin, const char *mdp)
{
    guchar sel[SEL_OCTETS], nonce[NONCE_OCTETS], cle[CLE_OCTETS];

    gcry_create_nonce (sel, sizeof sel);
    gcry_create_nonce (nonce, sizeof nonce);

    g_autofree char *aad = authentifie (compte, ARGON2_M, ARGON2_T, ARGON2_P);
    if (!deriver (pin, sel, sizeof sel, aad, cle))
        return NULL;

    gcry_cipher_hd_t c = NULL;
    gcry_error_t e = gcry_cipher_open (&c, GCRY_CIPHER_AES256,
                                       GCRY_CIPHER_MODE_GCM, 0);
    if (e == 0) e = gcry_cipher_setkey (c, cle, sizeof cle);
    if (e == 0) e = gcry_cipher_setiv  (c, nonce, sizeof nonce);
    if (e == 0) e = gcry_cipher_authenticate (c, aad, strlen (aad));

    gsize mdp_n = strlen (mdp);
    g_autofree guchar *scelle = g_malloc0 (mdp_n + TAG_OCTETS);

    if (e == 0) e = gcry_cipher_encrypt (c, scelle, mdp_n, mdp, mdp_n);
    if (e == 0) e = gcry_cipher_gettag  (c, scelle + mdp_n, TAG_OCTETS);

    if (c != NULL)
        gcry_cipher_close (c);
    explicit_bzero (cle, sizeof cle);

    if (e != 0) {
        journal ("scellement : %s", gcry_strerror (e));
        return NULL;
    }

    g_autofree char *sel64    = g_base64_encode (sel, sizeof sel);
    g_autofree char *nonce64  = g_base64_encode (nonce, sizeof nonce);
    g_autofree char *scelle64 = g_base64_encode (scelle, mdp_n + TAG_OCTETS);
    explicit_bzero (scelle, mdp_n + TAG_OCTETS);

    /* Format de groupe INI plutôt qu'une suite de « clé=valeur » nues :
     * GKeyFile sait le relire, et un analyseur de moins à écrire est un
     * analyseur de moins à se tromper. La version est en tête pour qu'un
     * format futur se reconnaisse au lieu de se deviner. */
    return g_strdup_printf (
        "[coffre]\n"
        "version=1\n"
        "kdf=argon2id\n"
        "m=%d\n"
        "t=%d\n"
        "p=%d\n"
        "sel=%s\n"
        "nonce=%s\n"
        "scelle=%s\n",
        ARGON2_M, ARGON2_T, ARGON2_P, sel64, nonce64, scelle64);
}

/* Ouvre le coffre. Rend le mot de passe, ou NULL si le PIN est faux ou le
 * fichier abîmé — les deux se ressemblent volontairement du dehors. */
static char *
desceller (const char *compte, const char *pin, const char *contenu)
{
    g_autoptr(GKeyFile) kf = g_key_file_new ();
    if (!g_key_file_load_from_data (kf, contenu, strlen (contenu),
                                    G_KEY_FILE_NONE, NULL)) {
        journal ("le coffre de « %s » est illisible", compte);
        return NULL;
    }

    int version = g_key_file_get_integer (kf, "coffre", "version", NULL);
    if (version != 1) {
        journal ("coffre de « %s » en version %d, inconnue", compte, version);
        return NULL;
    }

    g_autofree char *kdf = g_key_file_get_string (kf, "coffre", "kdf", NULL);
    if (g_strcmp0 (kdf, "argon2id") != 0) {
        journal ("coffre de « %s » : kdf « %s » inconnu", compte, kdf ? kdf : "");
        return NULL;
    }

    /* Les paramètres sont RELUS DU FICHIER et non pris dans les constantes :
     * un coffre scellé avant un changement de réglage doit continuer à
     * s'ouvrir. Ils entrent dans l'AAD, donc ils ne peuvent pas avoir été
     * abaissés en douce. */
    unsigned long m = (unsigned long) g_key_file_get_integer (kf, "coffre", "m", NULL);
    unsigned long t = (unsigned long) g_key_file_get_integer (kf, "coffre", "t", NULL);
    unsigned long p = (unsigned long) g_key_file_get_integer (kf, "coffre", "p", NULL);

    /* Un plancher tout de même : un fichier remplacé en entier, AAD compris,
     * par un attaquant qui aurait l'écriture sur /var/lib pourrait demander
     * m=8. Il a alors déjà root, mais refuser coûte trois lignes. */
    if (m < 8192 || t < 1 || p < 1 || p > 16) {
        journal ("coffre de « %s » : paramètres hors bornes (m=%lu t=%lu p=%lu)",
                 compte, m, t, p);
        return NULL;
    }

    gsize sel_n = 0, nonce_n = 0, scelle_n = 0;
    g_autofree char *sel64    = g_key_file_get_string (kf, "coffre", "sel", NULL);
    g_autofree char *nonce64  = g_key_file_get_string (kf, "coffre", "nonce", NULL);
    g_autofree char *scelle64 = g_key_file_get_string (kf, "coffre", "scelle", NULL);
    if (sel64 == NULL || nonce64 == NULL || scelle64 == NULL)
        return NULL;

    g_autofree guchar *sel    = g_base64_decode (sel64, &sel_n);
    g_autofree guchar *nonce  = g_base64_decode (nonce64, &nonce_n);
    g_autofree guchar *scelle = g_base64_decode (scelle64, &scelle_n);

    if (sel_n != SEL_OCTETS || nonce_n != NONCE_OCTETS || scelle_n <= TAG_OCTETS) {
        journal ("coffre de « %s » : tailles incohérentes", compte);
        return NULL;
    }
    gsize mdp_n = scelle_n - TAG_OCTETS;

    guchar cle[CLE_OCTETS];
    g_autofree char *aad = authentifie (compte, m, t, p);

    /* Les paramètres du fichier, pas les constantes : voir plus haut. */
    const unsigned long param[4] = { CLE_OCTETS, t, m, p };
    gcry_kdf_hd_t h = NULL;
    gcry_error_t e = gcry_kdf_open (&h, GCRY_KDF_ARGON2, GCRY_KDF_ARGON2ID,
                                    param, G_N_ELEMENTS (param),
                                    pin, strlen (pin), sel, sel_n,
                                    NULL, 0, aad, strlen (aad));
    if (e == 0) e = gcry_kdf_compute (h, NULL);
    if (e == 0) e = gcry_kdf_final (h, CLE_OCTETS, cle);
    if (h != NULL) gcry_kdf_close (h);
    if (e != 0) {
        journal ("dérivation Argon2id : %s", gcry_strerror (e));
        return NULL;
    }

    /* Le clair va en mémoire verrouillée : c'est un mot de passe, et cette
     * machine a du swap (mmcblk1p3 et zram). */
    char *mdp = gcry_malloc_secure (mdp_n + 1);
    if (mdp == NULL) {
        explicit_bzero (cle, sizeof cle);
        journal ("mémoire sécurisée épuisée");
        return NULL;
    }

    gcry_cipher_hd_t c = NULL;
    e = gcry_cipher_open (&c, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, 0);
    if (e == 0) e = gcry_cipher_setkey (c, cle, sizeof cle);
    if (e == 0) e = gcry_cipher_setiv  (c, nonce, nonce_n);
    if (e == 0) e = gcry_cipher_authenticate (c, aad, strlen (aad));
    if (e == 0) e = gcry_cipher_decrypt (c, mdp, mdp_n, scelle, mdp_n);
    /* LE TAG EST CE QUI DIT SI LE PIN ÉTAIT BON. Sans cette vérification,
     * un PIN faux rendrait des octets aléatoires, qui partiraient à PAM
     * comme mot de passe : le compteur d'essais ne verrait jamais d'échec,
     * et le coffre n'en serait plus un. */
    if (e == 0) e = gcry_cipher_checktag (c, scelle + mdp_n, TAG_OCTETS);

    if (c != NULL) gcry_cipher_close (c);
    explicit_bzero (cle, sizeof cle);

    if (e != 0) {
        gcry_free (mdp);
        return NULL;          /* PIN faux, ou fichier modifié */
    }

    mdp[mdp_n] = '\0';
    return mdp;               /* à libérer par gcry_free */
}

/* -------------------------------------------------------------------------
 * Le thème du bureau
 *
 * Lu en root, puisque /home/<user> est en 0700 et que « _greetd » n'y entre
 * pas. C'est le seul fichier du répertoire personnel que ce programme
 * ouvre, et il n'en ressort qu'un nom de thème pris dans une liste fermée.
 *
 * CETTE LISTE BLANCHE EST LA GARANTIE, pas le chemin. Les composants
 * intermédiaires du chemin appartiennent à l'utilisateur, qui pourrait donc
 * les remplacer par des liens et faire lire à root un autre fichier. Il n'en
 * tirerait rien : ce qui sort d'ici est l'un des six identifiants ci-dessus,
 * ou le repli. Aucun contenu de fichier ne remonte vers « _greetd ».
 *
 * O_NONBLOCK est là pour un cas précis : si « shell.conf » était un tube
 * nommé, open() bloquerait avant même qu'on puisse vérifier sa nature, et
 * l'écran de connexion attendrait indéfiniment. Le contrôle S_ISREG qui
 * suit écarte tout ce qui n'est pas un fichier ordinaire.
 * ------------------------------------------------------------------------- */
static char *
theme_de (const char *compte)
{
    struct passwd *p = getpwnam (compte);
    if (p == NULL || p->pw_dir == NULL) {
        journal ("« %s » n'a pas de répertoire personnel ; thème de repli", compte);
        return g_strdup (THEME_REPLI);
    }

    g_autofree char *c = g_build_filename (p->pw_dir, ".config", "claude-os",
                                           "shell.conf", NULL);

    int fd = open (c, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        journal ("thème : %s illisible (%s) ; « %s » utilisé",
                 c, g_strerror (errno), THEME_REPLI);
        return g_strdup (THEME_REPLI);
    }

    struct stat st;
    if (fstat (fd, &st) != 0 || !S_ISREG (st.st_mode)
        || st.st_size <= 0 || st.st_size > REQUETE_MAX) {
        close (fd);
        journal ("thème : %s n'est pas un fichier ordinaire exploitable ; "
                 "« %s » utilisé", c, THEME_REPLI);
        return g_strdup (THEME_REPLI);
    }

    g_autofree char *tampon = g_malloc0 ((gsize) st.st_size + 1);
    gsize lu = 0;
    while (lu < (gsize) st.st_size) {
        gssize r = read (fd, tampon + lu, (gsize) st.st_size - lu);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        lu += (gsize) r;
    }
    close (fd);

    g_autoptr(GKeyFile) kf = g_key_file_new ();
    if (!g_key_file_load_from_data (kf, tampon, lu, G_KEY_FILE_NONE, NULL)) {
        journal ("thème : %s ne s'analyse pas ; « %s » utilisé", c, THEME_REPLI);
        return g_strdup (THEME_REPLI);
    }

    g_autofree char *lu_theme = g_key_file_get_string (kf, "appearance",
                                                       "theme", NULL);
    if (lu_theme == NULL || *lu_theme == '\0') {
        journal ("thème : aucun réglage dans %s ; « %s » utilisé", c, THEME_REPLI);
        return g_strdup (THEME_REPLI);
    }

    for (int i = 0; THEMES[i] != NULL; i++)
        if (g_strcmp0 (THEMES[i], lu_theme) == 0)
            return g_strdup (lu_theme);

    journal ("thème « %s » hors liste ; « %s » utilisé", lu_theme, THEME_REPLI);
    return g_strdup (THEME_REPLI);
}

/* -------------------------------------------------------------------------
 * Les quatre verbes
 * ------------------------------------------------------------------------- */
static void
verbe_etat (const char *compte)
{
    g_autofree char *c = chemin (compte, ".pin");
    gboolean a_un_pin = g_file_test (c, G_FILE_TEST_IS_REGULAR);
    g_autofree char *theme = theme_de (compte);

    g_autoptr(JsonBuilder) b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "pin");
    json_builder_add_boolean_value (b, a_un_pin);
    json_builder_set_member_name (b, "essais_restants");
    json_builder_add_int_value (b, a_un_pin ? ESSAIS_MAX - essais_lus (compte) : 0);
    json_builder_set_member_name (b, "theme");
    json_builder_add_string_value (b, theme);
    json_builder_end_object (b);
    emettre (b);
}

static void
verbe_ouvrir (const char *compte, const char *pin)
{
    if (!pin_valide (pin)) {
        repondre_erreur ("pin_invalide", -1);
        return;
    }

    g_autofree char *c = chemin (compte, ".pin");
    g_autofree char *contenu = lire_fichier (c);
    if (contenu == NULL) {
        repondre_erreur ("absent", -1);
        return;
    }

    int faits = essais_lus (compte);
    if (faits >= ESSAIS_MAX) {
        pin_efface (compte);
        journal ("« %s » : quota d'essais déjà atteint, coffre effacé", compte);
        repondre_erreur ("verrouille", 0);
        return;
    }

    /* LE COMPTEUR MONTE AVANT LA TENTATIVE, JAMAIS APRÈS.
     *
     * L'incrémenter après coup laisserait une échappatoire triviale : couper
     * l'alimentation pendant les 0,6 s d'Argon2 rendrait la tentative
     * gratuite, et un million d'essais redeviendrait envisageable. Le
     * compteur redescend à zéro sur un succès, et sur lui seul. */
    if (!essais_ecrits (compte, faits + 1)) {
        journal ("compteur non enregistré : tentative refusée pour « %s »", compte);
        repondre_erreur ("interne", ESSAIS_MAX - faits);
        return;
    }

    char *mdp = desceller (compte, pin, contenu);
    if (mdp == NULL) {
        int restants = ESSAIS_MAX - (faits + 1);
        if (restants <= 0) {
            pin_efface (compte);
            journal ("« %s » : %d essais faux, le code PIN est supprimé",
                     compte, ESSAIS_MAX);
            repondre_erreur ("verrouille", 0);
        } else {
            journal ("« %s » : code PIN faux, %d essai(s) restant(s)",
                     compte, restants);
            repondre_erreur ("pin_faux", restants);
        }
        return;
    }

    essais_remis_a_zero (compte);

    g_autoptr(JsonBuilder) b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "mdp");
    json_builder_add_string_value (b, mdp);
    json_builder_end_object (b);
    emettre (b);

    gcry_free (mdp);
}

static void
verbe_enroler (const char *compte, const char *pin, const char *mdp)
{
    if (!pin_valide (pin)) {
        repondre_erreur ("pin_invalide", -1);
        return;
    }
    if (mdp == NULL || *mdp == '\0') {
        repondre_erreur ("mdp_vide", -1);
        return;
    }

    g_autofree char *contenu = sceller (compte, pin, mdp);
    if (contenu == NULL) {
        repondre_erreur ("interne", -1);
        return;
    }

    g_autofree char *c = chemin (compte, ".pin");
    if (!ecrire_scelle (c, contenu)) {
        repondre_erreur ("interne", -1);
        return;
    }

    essais_remis_a_zero (compte);
    journal ("code PIN enregistré pour « %s »", compte);
    repondre_ok ();
}

static void
verbe_oublier (const char *compte)
{
    pin_efface (compte);
    journal ("code PIN supprimé pour « %s »", compte);
    repondre_ok ();
}

/* -------------------------------------------------------------------------
 * L'entrée
 * ------------------------------------------------------------------------- */

/* Qui parle ? La socket est déjà en 0600 pour « _greetd », mais cette
 * vérification-là vit DANS le programme : une unité systemd modifiée, ou
 * recopiée ailleurs avec d'autres droits, ne suffit alors pas à ouvrir le
 * coffre à n'importe qui. Deux verrous valent mieux qu'un quand le second
 * coûte quinze lignes.
 *
 * root est admis pour que le coffre puisse être éprouvé sans l'écran de
 * connexion — c'est ce qui permet de le tester seul, avant de toucher à la
 * porte de la machine. */
static uid_t    appelant_uid;
static gboolean appelant_privilegie;   /* root ou _greetd : tous les coffres */

static gboolean
appelant_admis (void)
{
    struct ucred pair;
    socklen_t n = sizeof pair;

    if (getsockopt (STDIN_FILENO, SOL_SOCKET, SO_PEERCRED, &pair, &n) != 0) {
        /* Pas une socket : lancé à la main hors de systemd. On refuse, mais
         * on le DIT, sans quoi le diagnostic ressemblerait à une panne. */
        journal ("entrée standard sans identité d'appelant (%s) : "
                 "ce programme s'active par socket, voir claude-os-coffre.socket",
                 g_strerror (errno));
        return FALSE;
    }

    if (pair.uid == 0) {
        appelant_uid = 0;
        appelant_privilegie = TRUE;
        return TRUE;
    }

    struct passwd *g = getpwnam ("_greetd");
    if (g != NULL && pair.uid == g->pw_uid) {
        appelant_uid = pair.uid;
        appelant_privilegie = TRUE;
        return TRUE;
    }

    /* LA SESSION DE L'UTILISATEUR EST ADMISE, MAIS POUR SON SEUL COFFRE.
     *
     * Le verrou d'écran tourne sous le compte de l'utilisateur : il lui faut
     * cette porte. On ne l'ouvre pas en grand pour autant. Un compte
     * ordinaire est accepté ici, puis « utilisateur_permis » lui interdit
     * de nommer quelqu'un d'autre que lui-même — c'est cette seconde règle
     * qui fait le travail, la première ne fait que laisser entrer.
     *
     * Ce que cela change, à savoir : un programme tournant déjà sous ce
     * compte peut tenter un PIN. Le compteur d'essais le borne à cinq. Un
     * tel programme pouvait de toute façon enregistrer la frappe du mot de
     * passe ; l'élargissement est réel mais il n'ouvre pas une porte qui
     * était fermée. */
    if (pair.uid >= 1000) {
        appelant_uid = pair.uid;
        appelant_privilegie = FALSE;
        return TRUE;
    }

    journal ("appelant refusé : uid %u", (unsigned) pair.uid);
    return FALSE;
}

/* L'appelant a-t-il le droit de parler de CE compte ?
 *
 * Sans cette règle, ouvrir la socket au compte de la session reviendrait à
 * laisser n'importe quel programme de la session tenter le PIN de
 * n'importe quel utilisateur de la machine. */
static gboolean
utilisateur_permis (const char *utilisateur)
{
    if (appelant_privilegie)
        return TRUE;

    struct passwd *p = getpwuid (appelant_uid);
    if (p != NULL && g_strcmp0 (p->pw_name, utilisateur) == 0)
        return TRUE;

    journal ("uid %u a demandé le coffre de « %s » : refusé",
             (unsigned) appelant_uid, utilisateur != NULL ? utilisateur : "?");
    return FALSE;
}

/* Une ligne, plafonnée. Le protocole tient sur une ligne parce que
 * json-glib n'émet aucun saut de ligne en mode compact : un mot de passe
 * qui en contiendrait un est échappé par JSON, il ne coupe pas la trame. */
static char *
lire_requete (void)
{
    GString *ligne = g_string_new (NULL);
    char c;

    for (;;) {
        gssize r = read (STDIN_FILENO, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;                 /* fin de flux */
        if (c == '\n') break;
        g_string_append_c (ligne, c);
        if (ligne->len > REQUETE_MAX) {
            journal ("requête trop longue, abandonnée");
            g_string_free (ligne, TRUE);
            return NULL;
        }
    }

    if (ligne->len == 0) {
        g_string_free (ligne, TRUE);
        return NULL;
    }
    return g_string_free (ligne, FALSE);
}

int
main (void)
{
    /* libgcrypt refuse de travailler tant qu'on ne l'a pas déclarée prête.
     * La mémoire sécurisée est verrouillée en RAM : c'est là que le mot de
     * passe déchiffré est posé, et cette machine a du swap. 32 Kio suffisent
     * largement — il n'y transite qu'une chaîne à la fois. */
    if (gcry_check_version (GCRYPT_VERSION) == NULL) {
        journal ("libgcrypt trop ancienne (il faut au moins %s)", GCRYPT_VERSION);
        return 1;
    }
    gcry_control (GCRYCTL_SUSPEND_SECMEM_WARN);
    gcry_control (GCRYCTL_INIT_SECMEM, 32 * 1024, 0);
    gcry_control (GCRYCTL_RESUME_SECMEM_WARN);
    gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

    if (!appelant_admis ())
        return 1;

    /* Le répertoire est créé ici plutôt que par StateDirectory= : une seule
     * source pour son mode, et le coffre reste utilisable si l'unité est
     * remplacée. 0700 — root seul, jamais « _greetd ». */
    if (g_mkdir_with_parents (COFFRE_DIR, 0700) != 0) {
        journal ("%s : %s", COFFRE_DIR, g_strerror (errno));
        repondre_erreur ("interne", -1);
        return 1;
    }
    if (g_chmod (COFFRE_DIR, 0700) != 0)
        journal ("mode de %s non appliqué : %s", COFFRE_DIR, g_strerror (errno));

    g_autofree char *ligne = lire_requete ();
    if (ligne == NULL) {
        repondre_erreur ("requete_vide", -1);
        return 1;
    }

    g_autoptr(JsonParser) parser = json_parser_new ();
    g_autoptr(GError) err = NULL;
    if (!json_parser_load_from_data (parser, ligne, -1, &err)) {
        journal ("requête illisible : %s", err->message);
        repondre_erreur ("json_invalide", -1);
        return 1;
    }

    JsonNode *racine = json_parser_get_root (parser);
    if (racine == NULL || !JSON_NODE_HOLDS_OBJECT (racine)) {
        repondre_erreur ("json_invalide", -1);
        return 1;
    }
    JsonObject *o = json_node_get_object (racine);

    const char *verbe = json_object_get_string_member_with_default (o, "verbe", "");
    const char *demande = json_object_get_string_member_with_default (o, "utilisateur", "");

    g_autofree char *compte = compte_valide (demande);
    if (compte == NULL) {
        journal ("compte refusé : « %s »", demande);
        repondre_erreur ("compte_inconnu", -1);
        return 1;
    }

    if (!utilisateur_permis (compte)) {
        repondre_erreur ("interdit", -1);
        return 1;
    }

    /* MOINDRE PRIVILEGE : un appelant ordinaire consulte et ouvre, rien de
     * plus. Sceller ou effacer reste au greeter, qui n'appelle « enroler »
     * qu'après un « success » de greetd. Un programme de la session ne peut
     * donc ni remplacer le coffre par un scellé de son choix, ni l'effacer
     * pour forcer le retour au mot de passe. */
    if (!appelant_privilegie
        && g_strcmp0 (verbe, "etat") != 0
        && g_strcmp0 (verbe, "ouvrir") != 0) {
        journal ("uid %u a tenté « %s » : réservé au greeter",
                 (unsigned) appelant_uid, verbe);
        repondre_erreur ("interdit", -1);
        return 1;
    }

    if (g_strcmp0 (verbe, "etat") == 0) {
        verbe_etat (compte);
    } else if (g_strcmp0 (verbe, "ouvrir") == 0) {
        verbe_ouvrir (compte,
            json_object_get_string_member_with_default (o, "pin", ""));
    } else if (g_strcmp0 (verbe, "enroler") == 0) {
        verbe_enroler (compte,
            json_object_get_string_member_with_default (o, "pin", ""),
            json_object_get_string_member_with_default (o, "mdp", ""));
    } else if (g_strcmp0 (verbe, "oublier") == 0) {
        verbe_oublier (compte);
    } else {
        journal ("verbe inconnu : « %s »", verbe);
        repondre_erreur ("verbe_inconnu", -1);
        return 1;
    }

    return 0;
}
