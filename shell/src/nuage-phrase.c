/* =========================================================================
 * claude-os-nuage-phrase — la phrase qui ouvre la configuration rclone
 *
 * POURQUOI CE PROGRAMME EXISTE
 *
 * rclone garde les jetons de rafraichissement dans son fichier de
 * configuration. Un jeton de rafraichissement N'EST PAS un detail : il
 * rouvre le compte indefiniment, sans mot de passe et sans second facteur.
 * Il vaut donc exactement ce que vaut un mot de passe, et la regle du depot
 * s'applique telle quelle -- « ~/.config/claude-os/lecteurs ne contient rien
 * de secret », voir reseau.h.
 *
 * Le fichier de rclone est donc CHIFFRE, et la phrase qui l'ouvre vit au
 * trousseau, comme les mots de passe des lecteurs reseau.
 *
 * LA PHRASE NE PASSE NI PAR LA LIGNE DE COMMANDE NI PAR L'ENVIRONNEMENT
 *
 * RCLONE_CONFIG_PASS ferait l'affaire, mais l'environnement d'un processus
 * se lit dans /proc/<pid>/environ, et une phrase en argument se lirait dans
 * « ps » -- c'est le raisonnement deja tenu pour le mot de passe des
 * montages, en tete de claude-os-lecteur. rclone sait mieux faire :
 * --password-command execute ce programme et lit sa sortie. La phrase ne
 * quitte jamais le tube entre les deux.
 *
 * POURQUOI EN C, ET NON UN SCRIPT
 *
 * L'equivalent en Python coute 0,19 s de demarrage mesurees sur MADOO --
 * l'interpreteur et l'introspection GObject -- a chaque commande rclone.
 * Ici, libsecret est deja liee au shell : le programme ne coute que lui-meme.
 *
 *   claude-os-nuage-phrase --poser   < la phrase     l'enregistre
 *   claude-os-nuage-phrase                           l'affiche
 * ========================================================================= */
#include <libsecret/secret.h>

#include <stdio.h>
#include <string.h>

/* Les deux attributs suffisent a retrouver la phrase et a la distinguer de
 * ce que rangent les autres programmes. Meme grammaire que le schema des
 * lecteurs reseau, « org.claude-os.LecteurReseau ». */
static const SecretSchema *
schema_trousseau (void)
{
    static const SecretSchema s = {
        "org.claude-os.Nuage", SECRET_SCHEMA_NONE,
        {
            { "usage",   SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "fichier", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { NULL, 0 },
        },
        0, 0, 0, 0, 0, 0, 0, 0
    };
    return &s;
}

#define USAGE   "configuration-rclone"
#define FICHIER "rclone.conf"

static int
poser (void)
{
    char tampon[512];

    if (fgets (tampon, sizeof tampon, stdin) == NULL) {
        fputs ("claude-os-nuage-phrase : rien à lire sur l'entrée standard\n", stderr);
        return 2;
    }
    tampon[strcspn (tampon, "\r\n")] = '\0';

    if (tampon[0] == '\0') {
        fputs ("claude-os-nuage-phrase : phrase vide, rien enregistré\n", stderr);
        return 2;
    }

    GError *err = NULL;
    gboolean ok = secret_password_store_sync (
        schema_trousseau (), SECRET_COLLECTION_DEFAULT,
        "Claude OS — configuration des lecteurs nuage", tampon, NULL, &err,
        "usage", USAGE, "fichier", FICHIER, NULL);

    /* La phrase ne traine pas en memoire plus que necessaire. Ce n'est pas
     * une protection contre grand-chose -- le noyau a pu pagineriser la
     * pile -- mais cela coute une ligne. */
    memset (tampon, 0, sizeof tampon);

    if (!ok) {
        /* g_printerr transcode vers la locale, et un service systemd n'en a
         * pas : les accents ressortiraient en « ? ». Le piege est paye et
         * documente dans docs/09 ; on ecrit donc sur stderr directement. */
        fputs ("claude-os-nuage-phrase : enregistrement refusé par le trousseau : ", stderr);
        fputs (err != NULL ? err->message : "raison inconnue", stderr);
        fputc ('\n', stderr);
        g_clear_error (&err);
        return 1;
    }
    return 0;
}

static int
afficher (void)
{
    GError *err = NULL;
    char *phrase = secret_password_lookup_sync (
        schema_trousseau (), NULL, &err,
        "usage", USAGE, "fichier", FICHIER, NULL);

    if (phrase == NULL) {
        /* Un echec MUET ferait croire a une configuration corrompue alors
         * que le trousseau est seulement ferme -- et rclone, lui, dirait
         * « unable to decrypt configuration ». L'invariant n°4 du depot vaut
         * aussi pour un programme sans fenetre. */
        fputs ("claude-os-nuage-phrase : aucune phrase au trousseau ", stderr);
        if (err != NULL) {
            fputs ("(", stderr); fputs (err->message, stderr); fputs (")\n", stderr);
            g_clear_error (&err);
        } else {
            fputs ("— trousseau fermé, ou aucun compte nuage configuré\n", stderr);
        }
        return 1;
    }

    /* Sans saut de ligne : rclone prend la sortie entiere pour la phrase, et
     * un « \n » final en ferait partie sur certaines versions. */
    fputs (phrase, stdout);
    secret_password_free (phrase);
    return 0;
}

int
main (int argc, char **argv)
{
    if (argc > 1 && g_strcmp0 (argv[1], "--poser") == 0)
        return poser ();

    if (argc > 1) {
        fputs ("usage : claude-os-nuage-phrase [--poser]\n", stderr);
        return 2;
    }
    return afficher ();
}
