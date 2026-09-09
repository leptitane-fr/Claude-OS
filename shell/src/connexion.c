/* =========================================================================
 * Claude OS — écran de connexion
 *
 * Quatre volets, dans cet ordre d'apparition :
 *
 *   nom   -> mdp   -> choix-pin      la premiere fois
 *   pin                              toutes les fois suivantes
 *
 * Pas de choix de session, pas de menu. L'ecran reste ce qu'il etait : la
 * porte, et rien d'autre.
 *
 * LE CODE PIN N'EST PAS UN MOT DE PASSE DE RECHANGE
 *
 * /etc/pam.d/greetd charge pam_gnome_keyring.so : PAM doit recevoir le VRAI
 * mot de passe, sinon le trousseau de session reste ferme et les mots de
 * passe des lecteurs reseau deviennent inaccessibles -- une session qui
 * s'ouvre, et des partages qui ne repondent plus sans qu'on comprenne
 * pourquoi.
 *
 * Le PIN est donc une CLE DE COFFRE. claude-os-coffre garde le mot de passe
 * scelle par Argon2id ; le PIN l'ouvre ; le mot de passe part a greetd comme
 * s'il avait ete tape. Rien ne change en aval, et le trousseau suit.
 *
 * L'ECRAN NE S'ENFERME JAMAIS
 *
 * Trois regles, tenues par le code et non par la procedure :
 *   - le mot de passe reste atteignable depuis TOUS les volets ;
 *   - le PIN est facultatif, et se retire depuis cet ecran ;
 *   - coffre absent, muet ou en panne = volet mot de passe, avec la raison
 *     affichee. Aucun chemin ne mene a un ecran ou l'on ne peut rien faire.
 *
 * POURQUOI L'ÉCRIRE PLUTÔT QUE CONFIGURER CELUI DE LIGHTDM
 *
 * Le greeter de LightDM ne sait démarrer que sur un serveur X. C'était le
 * DERNIER usage de X sur cette machine : la session, elle, est Wayland de
 * bout en bout. Garder un serveur X entier — une centaine de mégaoctets, et
 * une pile de plus à tenir à jour — pour afficher un champ de mot de passe
 * ne se défend pas.
 *
 * greetd le remplace : un démon d'une poignée de kilo-octets qui ne fait
 * rien d'autre qu'ouvrir une session. Il ne dessine RIEN — c'est ce
 * programme qui dessine, sous labwc, avec la même feuille de style que le
 * reste du bureau.
 *
 * L'AUTHENTIFICATION N'EST PAS ICI
 *
 * Ce programme ne touche jamais à PAM, ne lit jamais /etc/shadow, et ne
 * tourne pas en root. Il relaie : greetd pose les questions de PAM, ce
 * programme les affiche, renvoie les réponses, et greetd décide. C'est
 * délibéré — un écran de connexion qui ferait lui-même l'authentification
 * serait un programme privilégié de plus à auditer.
 *
 * Protocole : greetd-ipc(7). Sur la socket nommée par GREETD_SOCK, chaque
 * message est une longueur sur quatre octets (boutisme de la machine) suivie
 * du JSON.
 * ========================================================================= */

/* getpwent et ses voisines ne sont pas dans le C strict, et le projet
 * compile en -std=c11 : sans cette ligne, glibc ne les déclare pas et le
 * compilateur en déduit un « int ». */
#define _DEFAULT_SOURCE

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>

#include <pwd.h>
#include <string.h>

#include "config.h"
#include "clavier.h"

#define SESSION_DEFAUT "/usr/local/bin/claude-os-session"

/* Les volets, dans l'ordre de la GtkStack. */
typedef enum {
    V_NOM,      /* nom d'utilisateur, premiere connexion                    */
    V_MDP,      /* mot de passe                                             */
    V_PIN,      /* code PIN, connexions suivantes                           */
    V_CHOIX,    /* choix d'un code PIN, apres un mot de passe accepte        */
    V_NOMBRE
} Volet;

static const char *const NOMS_VOLET[V_NOMBRE] = { "nom", "mdp", "pin", "choix" };

/* Six pastilles qui se remplissent : c'est le champ du code PIN. Un
 * GtkPasswordEntry ferait le meme travail, mais il montrerait un curseur,
 * accepterait n'importe quelle longueur, et n'aurait pas la lisibilite a
 * un metre qu'on attend d'un ecran tactile. */
typedef struct {
    GtkWidget *boite;
    GtkWidget *points[6];
    GString   *valeur;
} Pastilles;

static struct {
    GtkWidget *fenetre;
    GtkWidget *pile;                  /* les quatre volets                  */
    GtkWidget *nom_champ;
    GtkWidget *mdp;
    GtkWidget *message[V_NOMBRE];     /* un message par volet               */
    GtkWidget *lien_pin;              /* « Utiliser le code PIN », volet mdp */
    GtkWidget *bouton_oublier;        /* « Ne plus utiliser », volet choix   */
    GtkWidget *titre_mdp;             /* le nom d'usage, volet mdp          */
    GtkWidget *titre_pin;             /* le nom d'usage, volet PIN          */
    GtkWidget *titre_choix;
    GtkWidget *heure;
    GtkWidget *date;

    Pastilles *pin;                   /* volet PIN                          */
    Pastilles *choix;                 /* volet choix du PIN                 */
    char      *choix_premier;         /* premiere saisie, avant confirmation */

    char      *utilisateur;   /* compte à ouvrir                            */
    char      *session;       /* commande lancée après authentification     */
    gboolean   apercu;        /* banc d'essai : aucun greetd derrière       */
    const char *volet_force;  /* banc d'essai : volet d'ouverture imposé    */
    gboolean   en_cours;      /* une tentative est en vol                   */

    Volet      volet;
    gboolean   coffre_la;     /* le coffre a répondu au démarrage           */
    gboolean   a_un_pin;
    int        essais_restants;

    /* LA SOCKET GREETD, TENUE OUVERTE ENTRE L'AUTHENTIFICATION ET LE
     * DÉMARRAGE DE LA SESSION.
     *
     * greetd garde « la session en cours de configuration » dans le démon,
     * et une connexion qui se ferme l'annule. Or il faut justement s'arrêter
     * entre les deux — c'est là qu'on propose le code PIN, et c'est le seul
     * moment où l'on détient à la fois un mot de passe VALIDÉ par PAM et un
     * utilisateur devant l'écran pour choisir son code.
     *
     * D'où cette socket portée par l'état global plutôt que par la pile
     * d'une fonction. Elle est ouverte par l'authentification, et refermée
     * par le démarrage de la session ou par un abandon. */
    GSocket   *greetd;
    char      *mdp_valide;    /* le mot de passe accepté, le temps du scellement */
} C;

/* -------------------------------------------------------------------------
 * L'utilisateur
 * ------------------------------------------------------------------------- */

/* Le seul compte humain de la machine. Les comptes système sont sous 1000,
 * et « nobody » est à 65534 ; entre les deux, il ne doit rester que lui. */
static char *
utilisateur_unique (void)
{
    struct passwd *p;
    char *trouve = NULL;

    setpwent ();
    while ((p = getpwent ()) != NULL) {
        if (p->pw_uid < 1000 || p->pw_uid >= 60000)
            continue;
        if (trouve != NULL) {
            /* Plusieurs comptes : on garde le premier et on le dit, plutôt
             * que d'en choisir un au hasard sans prévenir. */
            g_message ("plusieurs comptes humains ; « %s » retenu", trouve);
            break;
        }
        trouve = g_strdup (p->pw_name);
    }
    endpwent ();
    return trouve;
}

/* Le nom d'usage, tiré du champ GECOS. « Stéphane » plutôt que « stef ». */
static char *
nom_affiche (const char *compte)
{
    struct passwd *p = getpwnam (compte);
    if (p == NULL || p->pw_gecos == NULL || *p->pw_gecos == '\0')
        return g_strdup (compte);

    /* GECOS est une liste séparée par des virgules ; le nom complet est le
     * premier champ. */
    g_auto(GStrv) champs = g_strsplit (p->pw_gecos, ",", 2);
    if (champs[0] == NULL || *champs[0] == '\0')
        return g_strdup (compte);
    return g_strdup (champs[0]);
}

/* -------------------------------------------------------------------------
 * Le coffre
 *
 * claude-os-coffre tourne en root, activé par socket, et répond à quatre
 * verbes. Voir shell/src/coffre.c pour ce qu'il garde et pourquoi.
 *
 * TOUT CE QUI SUIT PEUT ÉCHOUER SANS QUE CE SOIT GRAVE. Le coffre est un
 * confort : s'il ne répond pas, l'écran retombe sur le mot de passe. Aucune
 * de ces fonctions ne doit donc empêcher la connexion — elles renseignent un
 * GError, et l'appelant affiche.
 * ------------------------------------------------------------------------- */
#define COFFRE_SOCK "/run/claude-os/coffre.sock"

/* Généreux : Argon2id demande 0,7 s mesurées, et une machine chargée au
 * démarrage peut mettre le double. Un délai trop court se manifesterait par
 * un « code PIN refusé » sur un code juste — la pire des erreurs à
 * diagnostiquer. */
#define COFFRE_DELAI 15

/* Requête : verbe, puis des couples clé/valeur, terminés par NULL. */
static JsonNode *
coffre_requete (const char *verbe, ...)
{
    g_autoptr(JsonBuilder) b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "verbe");
    json_builder_add_string_value (b, verbe);
    json_builder_set_member_name (b, "utilisateur");
    json_builder_add_string_value (b, C.utilisateur);

    va_list args;
    va_start (args, verbe);
    for (;;) {
        const char *cle = va_arg (args, const char *);
        if (cle == NULL)
            break;
        const char *valeur = va_arg (args, const char *);
        json_builder_set_member_name (b, cle);
        json_builder_add_string_value (b, valeur != NULL ? valeur : "");
    }
    va_end (args);

    json_builder_end_object (b);
    return json_builder_get_root (b);
}

/* Un aller-retour. Rend l'objet de réponse, dont la durée de vie est celle
 * du parser rendu par « parser ». */
static JsonObject *
coffre_parler (JsonNode *requete, JsonParser **parser, GError **err)
{
    g_autoptr(GSocket) s = g_socket_new (G_SOCKET_FAMILY_UNIX,
                                         G_SOCKET_TYPE_STREAM,
                                         G_SOCKET_PROTOCOL_DEFAULT, err);
    if (s == NULL)
        return NULL;

    g_socket_set_timeout (s, COFFRE_DELAI);

    g_autoptr(GSocketAddress) adresse = g_unix_socket_address_new (COFFRE_SOCK);
    if (!g_socket_connect (s, adresse, NULL, err))
        return NULL;

    g_autoptr(JsonGenerator) gen = json_generator_new ();
    json_generator_set_root (gen, requete);
    gsize n = 0;
    g_autofree char *texte = json_generator_to_data (gen, &n);

    /* Le saut de ligne EST la trame. json-glib n'en émet aucun en mode
     * compact, donc une ligne vaut exactement une requête. */
    g_autofree char *ligne = g_strconcat (texte, "\n", NULL);
    gsize total = n + 1, envoye = 0;
    while (envoye < total) {
        gssize r = g_socket_send (s, ligne + envoye, total - envoye, NULL, err);
        if (r <= 0)
            return NULL;
        envoye += (gsize) r;
    }

    g_autoptr(GString) reponse = g_string_new (NULL);
    for (;;) {
        char c;
        gssize r = g_socket_receive (s, &c, 1, NULL, err);
        if (r <= 0) {
            if (reponse->len > 0)
                break;               /* fermeture après la réponse */
            if (err != NULL && *err == NULL)
                g_set_error (err, G_IO_ERROR, G_IO_ERROR_CLOSED,
                             "le coffre n'a rien répondu");
            return NULL;
        }
        if (c == '\n')
            break;
        g_string_append_c (reponse, c);
        if (reponse->len > 64 * 1024) {
            g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "réponse du coffre démesurée");
            return NULL;
        }
    }

    *parser = json_parser_new ();
    if (!json_parser_load_from_data (*parser, reponse->str,
                                     (gssize) reponse->len, err)) {
        g_clear_object (parser);
        return NULL;
    }

    JsonNode *racine = json_parser_get_root (*parser);
    if (racine == NULL || !JSON_NODE_HOLDS_OBJECT (racine)) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "réponse inattendue du coffre");
        g_clear_object (parser);
        return NULL;
    }
    return json_node_get_object (racine);
}

/* Les messages du coffre sont des identifiants ; ils deviennent ici des
 * phrases. Un écran de connexion qui affiche « pin_faux » a raison sur le
 * fond et tort sur la forme. */
static char *
coffre_phrase (const char *code, JsonObject *o)
{
    if (g_strcmp0 (code, "pin_faux") == 0) {
        gint64 n = json_object_get_int_member_with_default (o, "essais_restants", 0);
        return g_strdup_printf (
            n > 1 ? "Code incorrect. Encore %" G_GINT64_FORMAT " essais."
                  : "Code incorrect. Encore %" G_GINT64_FORMAT " essai.", n);
    }
    if (g_strcmp0 (code, "verrouille") == 0)
        return g_strdup ("Trop de codes faux : le code PIN a été supprimé. "
                         "Utilisez votre mot de passe.");
    if (g_strcmp0 (code, "absent") == 0)
        return g_strdup ("Aucun code PIN enregistré.");
    if (g_strcmp0 (code, "pin_invalide") == 0)
        return g_strdup ("Le code doit comporter six chiffres.");
    return g_strdup_printf ("Le coffre a refusé : %s", code);
}

/* L'état du compte, ET le thème du bureau — le coffre est le seul à pouvoir
 * lire l'un comme l'autre. Un seul aller-retour au démarrage.
 *
 * Ne rend jamais d'erreur : un thème de repli et « pas de code PIN » sont
 * toujours une réponse utilisable. Mais elle le DIT sur la sortie d'erreur,
 * qui va dans /var/log/claude-os-connexion.log. La version précédente de
 * cette lecture échouait en silence, et c'est exactement pour cela que
 * personne n'a vu pendant des semaines que le thème ne suivait pas. */
static void
coffre_etat (gboolean *a_un_pin, int *essais_restants, char **theme)
{
    *a_un_pin = FALSE;
    *essais_restants = 0;
    *theme = NULL;

    g_autoptr(JsonNode) requete = coffre_requete ("etat", NULL);
    g_autoptr(JsonParser) parser = NULL;
    g_autoptr(GError) err = NULL;

    JsonObject *o = coffre_parler (requete, &parser, &err);
    if (o == NULL) {
        g_printerr ("coffre injoignable (%s) : pas de code PIN, thème par "
                    "défaut. Vérifier claude-os-coffre.socket.\n", err->message);
        return;
    }

    const char *e = json_object_get_string_member_with_default (o, "erreur", NULL);
    if (e != NULL) {
        g_printerr ("le coffre refuse l'état du compte « %s » : %s\n",
                    C.utilisateur, e);
        return;
    }

    C.coffre_la = TRUE;
    *a_un_pin = json_object_get_boolean_member_with_default (o, "pin", FALSE);
    *essais_restants = (int) json_object_get_int_member_with_default (
        o, "essais_restants", 0);

    const char *t = json_object_get_string_member_with_default (o, "theme", NULL);
    if (t != NULL && *t != '\0')
        *theme = g_strdup (t);
}
/* -------------------------------------------------------------------------
 * Dialogue avec greetd
 * ------------------------------------------------------------------------- */
static gboolean
envoyer (GSocket *s, JsonNode *racine, GError **err)
{
    g_autoptr(JsonGenerator) gen = json_generator_new ();
    json_generator_set_root (gen, racine);

    gsize n = 0;
    g_autofree char *texte = json_generator_to_data (gen, &n);

    guint32 taille = (guint32) n;
    if (g_socket_send (s, (const char *) &taille, sizeof taille, NULL, err) < 0)
        return FALSE;

    gsize envoye = 0;
    while (envoye < n) {
        gssize r = g_socket_send (s, texte + envoye, n - envoye, NULL, err);
        if (r <= 0)
            return FALSE;
        envoye += (gsize) r;
    }
    return TRUE;
}

static gboolean
lire_tout (GSocket *s, char *tampon, gsize n, GError **err)
{
    gsize lu = 0;
    while (lu < n) {
        gssize r = g_socket_receive (s, tampon + lu, n - lu, NULL, err);
        if (r <= 0) {
            if (err != NULL && *err == NULL)
                g_set_error (err, G_IO_ERROR, G_IO_ERROR_CLOSED,
                             "greetd a fermé la connexion");
            return FALSE;
        }
        lu += (gsize) r;
    }
    return TRUE;
}

/* Renvoie l'objet JSON reçu, à libérer avec json_node_unref sur sa racine.
 * Le JsonParser est rendu pour que l'appelant maîtrise la durée de vie. */
static JsonObject *
recevoir (GSocket *s, JsonParser **parser, GError **err)
{
    guint32 taille = 0;
    if (!lire_tout (s, (char *) &taille, sizeof taille, err))
        return NULL;

    /* Garde-fou : un message de greetd fait quelques centaines d'octets. Une
     * longueur aberrante signale une désynchronisation, pas un gros message. */
    if (taille == 0 || taille > 64 * 1024) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "longueur de message invalide (%u)", taille);
        return NULL;
    }

    g_autofree char *tampon = g_malloc (taille + 1);
    if (!lire_tout (s, tampon, taille, err))
        return NULL;
    tampon[taille] = '\0';

    *parser = json_parser_new ();
    if (!json_parser_load_from_data (*parser, tampon, (gssize) taille, err)) {
        g_clear_object (parser);
        return NULL;
    }

    JsonNode *racine = json_parser_get_root (*parser);
    if (racine == NULL || !JSON_NODE_HOLDS_OBJECT (racine)) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "réponse inattendue de greetd");
        g_clear_object (parser);
        return NULL;
    }
    return json_node_get_object (racine);
}

static JsonNode *
message_simple (const char *type, const char *cle, const char *valeur)
{
    g_autoptr(JsonBuilder) b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "type");
    json_builder_add_string_value (b, type);
    if (cle != NULL) {
        json_builder_set_member_name (b, cle);
        json_builder_add_string_value (b, valeur);
    }
    json_builder_end_object (b);
    return json_builder_get_root (b);
}

static JsonNode *
message_demarrage (const char *commande)
{
    g_autoptr(JsonBuilder) b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "type");
    json_builder_add_string_value (b, "start_session");
    json_builder_set_member_name (b, "cmd");
    json_builder_begin_array (b);
    json_builder_add_string_value (b, commande);
    json_builder_end_array (b);
    /* env vide : greetd pose déjà XDG_SESSION_TYPE, XDG_RUNTIME_DIR et le
     * reste. Y ajouter nos variables ici doublerait ce que labwc lit dans
     * /etc/xdg/labwc/environment. */
    json_builder_set_member_name (b, "env");
    json_builder_begin_array (b);
    json_builder_end_array (b);
    json_builder_end_object (b);
    return json_builder_get_root (b);
}

/* -------------------------------------------------------------------------
 * L'authentification, en deux temps
 *
 * Elle était d'un seul tenant : authentifier puis démarrer la session, dans
 * la même fonction et sur la même socket. Il faut désormais pouvoir
 * S'ARRÊTER ENTRE LES DEUX, pour proposer un code PIN — c'est le seul moment
 * où l'on tient à la fois un mot de passe validé par PAM et quelqu'un devant
 * l'écran.
 *
 * D'où deux fonctions, et une socket qui leur survit à toutes les deux.
 *
 * PAM impose un délai de deux secondes après un mot de passe faux, et le
 * coffre 0,7 s pour Argon2id. Les deux tournent dans un fil séparé : sur le
 * fil principal, l'écran se figerait — le champ ne se viderait pas, le
 * message n'apparaîtrait pas, et on croirait la machine plantée.
 * ------------------------------------------------------------------------- */

/* Authentifie, et LAISSE LA SOCKET OUVERTE dans « sortie ». */
static gboolean
greetd_authentifier (GSocket **sortie, const char *mdp, GError **err)
{
    *sortie = NULL;

    const char *chemin = g_getenv ("GREETD_SOCK");
    if (chemin == NULL) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "GREETD_SOCK absent : lancé hors de greetd ?");
        return FALSE;
    }

    g_autoptr(GSocket) s = g_socket_new (G_SOCKET_FAMILY_UNIX,
                                         G_SOCKET_TYPE_STREAM,
                                         G_SOCKET_PROTOCOL_DEFAULT, err);
    if (s == NULL)
        return FALSE;

    g_autoptr(GSocketAddress) adresse = g_unix_socket_address_new (chemin);
    if (!g_socket_connect (s, adresse, NULL, err))
        return FALSE;

    g_autoptr(JsonNode) demande =
        message_simple ("create_session", "username", C.utilisateur);
    if (!envoyer (s, demande, err))
        return FALSE;

    /* PAM peut poser plusieurs questions. On répond au premier secret avec
     * le mot de passe ; toute question SUPPLÉMENTAIRE — second facteur,
     * changement de mot de passe imposé — n'a pas d'interface ici, et on le
     * dit plutôt que de répondre n'importe quoi. */
    gboolean mdp_donne = FALSE;

    for (;;) {
        g_autoptr(JsonParser) parser = NULL;
        JsonObject *o = recevoir (s, &parser, err);
        if (o == NULL)
            return FALSE;

        const char *type = json_object_get_string_member_with_default (o, "type", "");

        if (g_strcmp0 (type, "auth_message") == 0) {
            const char *genre = json_object_get_string_member_with_default (
                o, "auth_message_type", "secret");
            const char *texte = json_object_get_string_member_with_default (
                o, "auth_message", "");

            const char *reponse = "";
            if (g_strcmp0 (genre, "secret") == 0 && !mdp_donne) {
                reponse = mdp;
                mdp_donne = TRUE;
            } else if (g_strcmp0 (genre, "secret") == 0
                    || g_strcmp0 (genre, "visible") == 0) {
                g_autoptr(JsonNode) annule = message_simple ("cancel_session", NULL, NULL);
                envoyer (s, annule, NULL);
                g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "PAM demande autre chose : « %s »", texte);
                return FALSE;
            }
            /* info et error : rien à répondre, mais le texte est utile. */

            g_autoptr(JsonNode) rep =
                message_simple ("post_auth_message_response", "response", reponse);
            if (!envoyer (s, rep, err))
                return FALSE;
            continue;
        }

        if (g_strcmp0 (type, "error") == 0) {
            const char *genre = json_object_get_string_member_with_default (
                o, "error_type", "error");
            const char *desc = json_object_get_string_member_with_default (
                o, "description", "");

            /* auth_error : mot de passe faux. C'est le cas courant, il
             * mérite un message compréhensible plutôt que le texte de PAM. */
            if (g_strcmp0 (genre, "auth_error") == 0)
                g_set_error (err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                             "Mot de passe incorrect.");
            else
                g_set_error (err, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "%s", *desc ? desc : "échec de la connexion");
            return FALSE;
        }

        if (g_strcmp0 (type, "success") == 0)
            break;
    }

    *sortie = g_steal_pointer (&s);
    return TRUE;
}

static gboolean
greetd_demarrer (GSocket *s, GError **err)
{
    g_autoptr(JsonNode) demarrer = message_demarrage (C.session);
    if (!envoyer (s, demarrer, err))
        return FALSE;

    g_autoptr(JsonParser) parser = NULL;
    JsonObject *o = recevoir (s, &parser, err);
    if (o == NULL)
        return FALSE;

    if (g_strcmp0 (json_object_get_string_member_with_default (o, "type", ""),
                   "success") != 0) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
            json_object_get_string_member_with_default (o, "description",
                                                        "la session n'a pas démarré"));
        return FALSE;
    }
    return TRUE;
}

/* Abandonne une session à demi configurée. Sans cela, greetd garderait un
 * état ouvert et refuserait la tentative suivante. */
static void
greetd_abandonner (void)
{
    if (C.greetd == NULL)
        return;
    g_autoptr(JsonNode) annule = message_simple ("cancel_session", NULL, NULL);
    envoyer (C.greetd, annule, NULL);
    g_clear_object (&C.greetd);
}

/* --- fil n°1 : le mot de passe ------------------------------------------ */
static void
tache_mdp (GTask *tache, gpointer source, gpointer donnees, GCancellable *a)
{
    const char *mdp = donnees;
    g_autoptr(GError) err = NULL;
    (void) source; (void) a;

    if (C.apercu) {
        g_usleep (600 * 1000);
        g_task_return_boolean (tache, TRUE);
        return;
    }

    GSocket *s = NULL;
    if (!greetd_authentifier (&s, mdp, &err)) {
        g_task_return_error (tache, g_steal_pointer (&err));
        return;
    }

    /* La socket voyage vers le fil principal : c'est elle qui sera reprise
     * au moment de démarrer la session. */
    g_task_return_pointer (tache, s, g_object_unref);
}

/* --- fil n°2 : le code PIN ----------------------------------------------
 *
 * D'un seul tenant, celui-là : rien à demander entre le déverrouillage et
 * l'ouverture de la session, donc rien à interrompre. */
static void
tache_pin (GTask *tache, gpointer source, gpointer donnees, GCancellable *a)
{
    const char *pin = donnees;
    g_autoptr(GError) err = NULL;
    (void) source; (void) a;

    if (C.apercu) {
        g_usleep (600 * 1000);
        if (g_strcmp0 (pin, "123456") != 0) {
            g_task_return_new_error (tache, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                     "Code incorrect. Encore 4 essais. (aperçu)");
            return;
        }
        g_task_return_boolean (tache, TRUE);
        return;
    }

    g_autoptr(JsonNode) requete = coffre_requete ("ouvrir", "pin", pin, NULL);
    g_autoptr(JsonParser) parser = NULL;
    JsonObject *o = coffre_parler (requete, &parser, &err);
    if (o == NULL) {
        g_task_return_error (tache, g_steal_pointer (&err));
        return;
    }

    const char *e = json_object_get_string_member_with_default (o, "erreur", NULL);
    if (e != NULL) {
        g_autofree char *phrase = coffre_phrase (e, o);
        /* PAS DE g_task_set_task_data ICI : la donnée de tâche est le code
         * PIN, que ce fil est en train de lire. L'écraser serait un usage
         * après libération. Ce que le fil principal doit savoir — « le PIN
         * est mort, passe au mot de passe » — tient dans le CODE de
         * l'erreur, qui voyage déjà avec elle. */
        g_task_return_new_error (tache, G_IO_ERROR,
                                 g_strcmp0 (e, "verrouille") == 0
                                   ? G_IO_ERROR_PERMISSION_DENIED
                                   : G_IO_ERROR_INVALID_ARGUMENT,
                                 "%s", phrase);
        return;
    }

    const char *mdp = json_object_get_string_member_with_default (o, "mdp", "");
    if (*mdp == '\0') {
        g_task_return_new_error (tache, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "Le coffre n'a pas rendu de mot de passe.");
        return;
    }

    GSocket *s = NULL;
    if (!greetd_authentifier (&s, mdp, &err)) {
        g_task_return_error (tache, g_steal_pointer (&err));
        return;
    }
    if (!greetd_demarrer (s, &err)) {
        g_object_unref (s);
        g_task_return_error (tache, g_steal_pointer (&err));
        return;
    }
    g_object_unref (s);
    g_task_return_boolean (tache, TRUE);
}

/* --- fil n°3 : enrôler puis démarrer -------------------------------------
 *
 * « pin » peut être NULL : c'est le cas de « Plus tard », où l'on démarre
 * sans rien sceller. */
typedef struct {
    char     *pin;       /* NULL : ne rien enrôler */
    char     *mdp;
    gboolean  oublier;   /* retirer le code PIN existant */
} Demarrage;

static void
demarrage_libere (gpointer p)
{
    Demarrage *d = p;
    if (d->pin != NULL) { explicit_bzero (d->pin, strlen (d->pin)); g_free (d->pin); }
    if (d->mdp != NULL) { explicit_bzero (d->mdp, strlen (d->mdp)); g_free (d->mdp); }
    g_free (d);
}

/* Scelle le mot de passe, et dit FRANCHEMENT si cela n'a pas marché.
 * Rend FALSE sans jamais lever d'erreur bloquante : voir tache_demarrer. */
static gboolean
enroler (const char *pin, const char *mdp)
{
    g_autoptr(GError) err = NULL;

    g_autoptr(JsonNode) requete =
        coffre_requete ("enroler", "pin", pin, "mdp", mdp, NULL);
    g_autoptr(JsonParser) parser = NULL;
    JsonObject *o = coffre_parler (requete, &parser, &err);

    if (o == NULL) {
        g_printerr ("enrôlement du code PIN impossible : %s\n", err->message);
        return FALSE;
    }
    const char *e = json_object_get_string_member_with_default (o, "erreur", NULL);
    if (e != NULL) {
        g_printerr ("le coffre a refusé l'enrôlement : %s\n", e);
        return FALSE;
    }

    /* ON RELIT CE QU'ON VIENT D'ÉCRIRE, et ce n'est pas de la méfiance de
     * principe : un scellement silencieusement faux ne se verrait qu'au
     * réveil suivant, sous la forme d'un « mot de passe incorrect » sur un
     * code juste — impossible à rattacher à son origine des heures plus
     * tard. Le contrôle coûte 0,7 s, une seule fois, et il a lieu pendant
     * que l'utilisateur est encore devant l'écran. */
    g_autoptr(JsonNode) controle = coffre_requete ("ouvrir", "pin", pin, NULL);
    g_autoptr(JsonParser) p2 = NULL;
    JsonObject *o2 = coffre_parler (controle, &p2, &err);
    const char *relu = (o2 != NULL)
        ? json_object_get_string_member_with_default (o2, "mdp", "") : "";

    if (g_strcmp0 (relu, mdp) != 0) {
        g_printerr ("le code PIN venait d'être écrit et ne se relit pas : "
                    "il est retiré plutôt que laissé inutilisable.\n");
        g_autoptr(JsonNode) retrait = coffre_requete ("oublier", NULL);
        g_autoptr(JsonParser) p3 = NULL;
        coffre_parler (retrait, &p3, NULL);
        return FALSE;
    }
    return TRUE;
}

static void
tache_demarrer (GTask *tache, gpointer source, gpointer donnees, GCancellable *a)
{
    Demarrage *d = donnees;
    g_autoptr(GError) err = NULL;
    (void) source; (void) a;

    /* L'ENRÔLEMENT NE PEUT PAS FAIRE ÉCHOUER L'OUVERTURE DE SESSION.
     *
     * Poser un code PIN est un confort ; ouvrir la session, non. Un coffre
     * en panne à cet instant laisserait l'utilisateur devant un écran de
     * connexion, avec un mot de passe pourtant accepté par PAM — et sans
     * moyen de comprendre.
     *
     * Tout échec part donc dans /var/log/claude-os-connexion.log, le code
     * PIN est abandonné, et l'on continue. Aucun PIN n'étant alors
     * enregistré, l'écran reproposera d'en choisir un à la prochaine
     * ouverture : la panne se rattrape d'elle-même. */
    if (d->oublier && !C.apercu) {
        g_autoptr(JsonNode) retrait = coffre_requete ("oublier", NULL);
        g_autoptr(JsonParser) p = NULL;
        g_autoptr(GError) e2 = NULL;
        if (coffre_parler (retrait, &p, &e2) == NULL)
            g_printerr ("le code PIN n'a pas pu être retiré : %s\n", e2->message);
    } else if (d->pin != NULL && !C.apercu) {
        enroler (d->pin, d->mdp);
    }

    if (C.apercu) {
        g_task_return_boolean (tache, TRUE);
        return;
    }

    if (!greetd_demarrer (C.greetd, &err)) {
        g_task_return_error (tache, g_steal_pointer (&err));
        return;
    }
    g_task_return_boolean (tache, TRUE);
}
/* -------------------------------------------------------------------------
 * Interface
 * ------------------------------------------------------------------------- */
static void aller_a (Volet v);

/* Le message du volet affiché. Chaque volet a le sien : un message unique
 * partagé réapparaîtrait sous un autre volet que celui qui l'a produit. */
static void
dire (const char *texte, gboolean faute)
{
    GtkWidget *l = C.message[C.volet];
    if (l == NULL)
        return;
    gtk_label_set_text (GTK_LABEL (l), texte != NULL ? texte : "");
    if (faute)
        gtk_widget_add_css_class (l, "faute");
    else
        gtk_widget_remove_css_class (l, "faute");
}

/* --- les six pastilles du code PIN -------------------------------------- */
static void
pastilles_maj (Pastilles *p)
{
    for (int i = 0; i < 6; i++) {
        if ((int) p->valeur->len > i)
            gtk_widget_add_css_class (p->points[i], "pleine");
        else
            gtk_widget_remove_css_class (p->points[i], "pleine");
    }
}

static Pastilles *
pastilles_neuves (void)
{
    Pastilles *p = g_new0 (Pastilles, 1);
    p->valeur = g_string_new (NULL);

    p->boite = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_halign (p->boite, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (p->boite, "connexion-pastilles");

    for (int i = 0; i < 6; i++) {
        /* UNE BOÎTE, ET NON UNE ÉTIQUETTE VIDE. Un GtkLabel sans texte
         * réserve tout de même la hauteur d'une ligne de sa police : les
         * pastilles sortaient ovales, plus hautes que larges, et aucune
         * valeur de min-height ne pouvait le corriger — c'est un plancher,
         * pas une consigne. Une GtkBox n'a pas de métrique de texte. */
        p->points[i] = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request (p->points[i], 15, 15);
        gtk_widget_set_valign (p->points[i], GTK_ALIGN_CENTER);
        gtk_widget_add_css_class (p->points[i], "connexion-pastille");
        gtk_box_append (GTK_BOX (p->boite), p->points[i]);
    }
    return p;
}

static void
pastilles_vider (Pastilles *p)
{
    g_string_truncate (p->valeur, 0);
    pastilles_maj (p);
}

/* --- le mot de passe ----------------------------------------------------- */
static void
on_demarrage_fini (GObject *source, GAsyncResult *res, gpointer data)
{
    g_autoptr(GError) err = NULL;
    (void) source; (void) data;

    C.en_cours = FALSE;

    if (g_task_propagate_boolean (G_TASK (res), &err)) {
        /* greetd remplace ce programme par la session : on ne revient pas
         * de là. Le message ne sert que le temps de la bascule. */
        dire ("Ouverture de la session…", FALSE);
        return;
    }

    /* La session n'a pas démarré alors que PAM avait dit oui. La socket
     * greetd ne vaut plus rien : on repart du mot de passe. */
    greetd_abandonner ();
    aller_a (V_MDP);
    dire (err->message, TRUE);
}

static void
lancer_demarrage (const char *pin, gboolean oublier)
{
    if (C.en_cours)
        return;
    C.en_cours = TRUE;
    dire (pin != NULL ? "Enregistrement du code…" : "Ouverture de la session…", FALSE);

    Demarrage *d = g_new0 (Demarrage, 1);
    d->pin = (pin != NULL) ? g_strdup (pin) : NULL;
    d->mdp = (C.mdp_valide != NULL) ? g_strdup (C.mdp_valide) : NULL;
    d->oublier = oublier;

    GTask *t = g_task_new (NULL, NULL, on_demarrage_fini, NULL);
    g_task_set_task_data (t, d, demarrage_libere);
    g_task_run_in_thread (t, tache_demarrer);
    g_object_unref (t);
}

static void
on_mdp_fini (GObject *source, GAsyncResult *res, gpointer data)
{
    g_autoptr(GError) err = NULL;
    (void) source; (void) data;

    C.en_cours = FALSE;
    gtk_widget_set_sensitive (C.mdp, TRUE);

    /* DEUX FORMES DE RÉSULTAT POUR UNE MÊME TÂCHE, et un branchement plutôt
     * que deux appels : au banc d'essai il n'y a pas de socket greetd à
     * rendre, donc un booléen. g_task_propagate_* ne doit être appelée
     * QU'UNE FOIS par tâche — en appeler deux consommerait le résultat déjà
     * pris et rendrait une erreur inventée. */
    if (C.apercu) {
        if (!g_task_propagate_boolean (G_TASK (res), &err)) {
            dire (err != NULL ? err->message : "Échec (aperçu).", TRUE);
            return;
        }
    } else {
        GSocket *s = g_task_propagate_pointer (G_TASK (res), &err);
        if (s == NULL) {
            /* Le garde sur « err » n'est pas de la superstition : c'est le
             * seul message que verra quelqu'un devant une porte fermée, et
             * un déréférencement nul ferait disparaître le greeter — donc
             * l'écran, donc tout moyen d'entrer. */
            dire (err != NULL ? err->message
                              : "L'authentification a échoué sans motif.", TRUE);
            gtk_editable_set_text (GTK_EDITABLE (C.mdp), "");
            gtk_widget_grab_focus (C.mdp);
            return;
        }
        g_clear_object (&C.greetd);
        C.greetd = s;
    }

    /* Le mot de passe est bon. On le garde le temps de le sceller — et pas
     * une seconde de plus : il est effacé dès la session lancée. */
    g_free (C.mdp_valide);
    C.mdp_valide = g_strdup (gtk_editable_get_text (GTK_EDITABLE (C.mdp)));
    gtk_editable_set_text (GTK_EDITABLE (C.mdp), "");

    /* Sans coffre, il n'y a rien à proposer : on ouvre la session. */
    if (!C.coffre_la) {
        lancer_demarrage (NULL, FALSE);
        return;
    }
    aller_a (V_CHOIX);
}

static void
tenter_mdp (void)
{
    if (C.en_cours)
        return;

    const char *mdp = gtk_editable_get_text (GTK_EDITABLE (C.mdp));
    if (mdp == NULL || *mdp == '\0') {
        dire ("Saisir le mot de passe.", TRUE);
        return;
    }

    C.en_cours = TRUE;
    gtk_widget_set_sensitive (C.mdp, FALSE);
    dire ("Vérification…", FALSE);

    GTask *t = g_task_new (NULL, NULL, on_mdp_fini, NULL);
    g_task_set_task_data (t, g_strdup (mdp), g_free);
    g_task_run_in_thread (t, tache_mdp);
    g_object_unref (t);
}

/* --- le code PIN --------------------------------------------------------- */
static void
on_pin_fini (GObject *source, GAsyncResult *res, gpointer data)
{
    g_autoptr(GError) err = NULL;
    (void) source; (void) data;

    C.en_cours = FALSE;

    if (g_task_propagate_boolean (G_TASK (res), &err)) {
        dire ("Ouverture de la session…", FALSE);
        return;
    }

    pastilles_vider (C.pin);

    /* PERMISSION_DENIED signifie ici « ce code ne servira plus » : soit les
     * cinq essais sont épuisés et le coffre s'est effacé, soit le mot de
     * passe qu'il gardait n'est plus le bon — un changement de mot de passe
     * depuis l'enrôlement, par exemple. Dans les deux cas, insister sur le
     * code PIN ferait perdre du temps : on passe au mot de passe. */
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
        C.a_un_pin = FALSE;
        gtk_widget_set_visible (C.lien_pin, FALSE);
        aller_a (V_MDP);
        dire (err->message, TRUE);
        return;
    }

    dire (err->message, TRUE);
}

static void
tenter_pin (void)
{
    if (C.en_cours)
        return;

    C.en_cours = TRUE;
    dire ("Vérification…", FALSE);

    GTask *t = g_task_new (NULL, NULL, on_pin_fini, NULL);
    g_task_set_task_data (t, g_strdup (C.pin->valeur->str), g_free);
    g_task_run_in_thread (t, tache_pin);
    g_object_unref (t);
}

/* Une touche du pavé, ou du clavier physique : les deux passent ici, et
 * c'est ce qui garantit qu'ils se comportent pareil. */
static void
saisir_pin (char c, gpointer donnees)
{
    (void) donnees;
    if (C.en_cours)
        return;

    if (c == '\b') {
        if (C.pin->valeur->len > 0) {
            g_string_truncate (C.pin->valeur, C.pin->valeur->len - 1);
            pastilles_maj (C.pin);
        }
        dire ("", FALSE);
        return;
    }

    if (C.pin->valeur->len >= 6)
        return;

    g_string_append_c (C.pin->valeur, c);
    pastilles_maj (C.pin);

    /* Le sixième chiffre valide tout seul : un geste de moins au doigt, et
     * il n'y a de toute façon rien d'autre à faire d'un code complet. */
    if (C.pin->valeur->len == 6)
        tenter_pin ();
}

/* --- le choix d'un code PIN ---------------------------------------------- */
static void
choix_recommencer (const char *pourquoi)
{
    g_clear_pointer (&C.choix_premier, g_free);
    pastilles_vider (C.choix);
    gtk_label_set_text (GTK_LABEL (C.titre_choix),
                        "Choisissez un code à six chiffres");
    if (pourquoi != NULL)
        dire (pourquoi, TRUE);
}

static void
saisir_choix (char c, gpointer donnees)
{
    (void) donnees;
    if (C.en_cours)
        return;

    if (c == '\b') {
        if (C.choix->valeur->len > 0) {
            g_string_truncate (C.choix->valeur, C.choix->valeur->len - 1);
            pastilles_maj (C.choix);
        }
        return;
    }

    if (C.choix->valeur->len >= 6)
        return;

    g_string_append_c (C.choix->valeur, c);
    pastilles_maj (C.choix);

    if (C.choix->valeur->len < 6)
        return;

    if (C.choix_premier == NULL) {
        /* DEUX SAISIES, ET NON UNE. Un code choisi à six chiffres et jamais
         * relu s'oublie d'autant plus vite qu'il vient d'être inventé ; une
         * faute de frappe ne se verrait qu'au prochain démarrage, et
         * coûterait cinq essais avant de rendre la main au mot de passe. */
        C.choix_premier = g_strdup (C.choix->valeur->str);
        pastilles_vider (C.choix);
        gtk_label_set_text (GTK_LABEL (C.titre_choix),
                            "Saisissez-le une seconde fois");
        dire ("", FALSE);
        return;
    }

    if (g_strcmp0 (C.choix_premier, C.choix->valeur->str) != 0) {
        choix_recommencer ("Les deux codes ne correspondent pas. On recommence.");
        return;
    }

    lancer_demarrage (C.choix_premier, FALSE);
}

/* --- la frappe physique sur les volets sans champ de saisie ---------------
 *
 * Les volets PIN n'ont pas de GtkEntry : le code vit dans six pastilles.
 * Sans ce contrôleur, le clavier physique ne servirait à rien sur ces
 * écrans-là, et l'utilisateur a demandé les DEUX modes de saisie.
 *
 * Phase de bouillonnement, et non de capture : sur les volets « nom » et
 * « mot de passe », le champ a le focus et reçoit la frappe le premier.
 * En capture, ce contrôleur la lui volerait. */
static gboolean
on_frappe (GtkEventControllerKey *c, guint keyval, guint code,
           GdkModifierType etat, gpointer donnees)
{
    (void) c; (void) code; (void) donnees;

    /* LA PORTE DE SORTIE DU BANC D'ESSAI.
     *
     * Cet écran s'ancre sur les quatre bords en couche OVERLAY et prend le
     * clavier en mode EXCLUSIVE : c'est ce qu'il faut pour un écran de
     * connexion, et c'est un piège pour qui lance « --apercu » depuis sa
     * session ouverte. La fenêtre recouvre tout le bureau, capte chaque
     * touche, et RIEN ne permet d'en sortir — ni Alt-Tab, ni Alt-F4, que
     * la configuration labwc du greeter ne définit pas non plus.
     *
     * Ctrl-Q, et seulement en aperçu. Le vrai écran de connexion n'a pas
     * de raccourci de sortie, et n'en aura jamais : ce serait une porte
     * ouverte avant authentification. */
    if (C.apercu && keyval == GDK_KEY_q && (etat & GDK_CONTROL_MASK)) {
        GtkApplication *app = gtk_window_get_application (GTK_WINDOW (C.fenetre));
        if (app != NULL)
            g_application_quit (G_APPLICATION (app));
        return GDK_EVENT_STOP;
    }

    if (C.volet != V_PIN && C.volet != V_CHOIX)
        return GDK_EVENT_PROPAGATE;

    void (*saisir) (char, gpointer) = (C.volet == V_PIN) ? saisir_pin : saisir_choix;

    if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9) {
        saisir ((char) ('0' + (keyval - GDK_KEY_0)), NULL);
        return GDK_EVENT_STOP;
    }
    if (keyval >= GDK_KEY_KP_0 && keyval <= GDK_KEY_KP_9) {
        saisir ((char) ('0' + (keyval - GDK_KEY_KP_0)), NULL);
        return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_BackSpace || keyval == GDK_KEY_Delete) {
        saisir ('\b', NULL);
        return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_Escape && C.volet == V_PIN) {
        aller_a (V_MDP);
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

/* -------------------------------------------------------------------------
 * Les quatre volets
 *
 * Chacun est bâti pareil : une carte, et sous elle un clavier. La carte
 * porte ce qu'on lit, le clavier ce qu'on touche — et cette séparation est
 * ce qui permet au clavier de faire toute la largeur qu'il lui faut sans
 * étirer la carte.
 * ------------------------------------------------------------------------- */
static GtkWidget *
carte_neuve (void)
{
    GtkWidget *c = gtk_box_new (GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class (c, "connexion-carte");
    gtk_widget_set_halign (c, GTK_ALIGN_CENTER);
    return c;
}

static GtkWidget *
message_neuf (void)
{
    GtkWidget *l = gtk_label_new ("");
    gtk_widget_add_css_class (l, "connexion-message");
    gtk_label_set_wrap (GTK_LABEL (l), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (l), 34);
    gtk_label_set_justify (GTK_LABEL (l), GTK_JUSTIFY_CENTER);
    return l;
}

static GtkWidget *
titre_neuf (const char *texte)
{
    GtkWidget *l = gtk_label_new (texte);
    gtk_widget_add_css_class (l, "connexion-nom");
    return l;
}

/* Un bouton qui se lit comme un lien : pas de fond, pas de bordure. Les
 * sorties de secours de cet écran — « utiliser le mot de passe », « plus
 * tard » — doivent être visibles sans concurrencer l'action principale. */
static GtkWidget *
lien_neuf (const char *texte, GCallback rappel)
{
    GtkWidget *b = gtk_button_new_with_label (texte);
    gtk_widget_add_css_class (b, "connexion-lien");
    gtk_widget_set_can_focus (b, FALSE);
    gtk_widget_set_focus_on_click (b, FALSE);
    gtk_widget_set_halign (b, GTK_ALIGN_CENTER);
    g_signal_connect (b, "clicked", rappel, NULL);
    return b;
}

static GtkWidget *
volet_neuf (GtkWidget *carte, GtkWidget *clavier)
{
    GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 22);
    gtk_widget_set_halign (v, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (v, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (v), carte);
    if (clavier != NULL)
        gtk_box_append (GTK_BOX (v), clavier);
    return v;
}

/* --- rappels des boutons ------------------------------------------------- */
static void on_nom_suivant (gpointer d);
static void on_clic_suivant   (GtkButton *b, gpointer d) { (void) b; (void) d; on_nom_suivant (NULL); }
static void on_entree_mdp     (gpointer d)               { (void) d; tenter_mdp (); }
static void on_active_mdp     (GtkWidget *w, gpointer d) { (void) w; (void) d; tenter_mdp (); }
static void on_clic_vers_mdp  (GtkButton *b, gpointer d) { (void) b; (void) d; aller_a (V_MDP); }
static void on_clic_vers_pin  (GtkButton *b, gpointer d) { (void) b; (void) d; aller_a (V_PIN); }
static void on_clic_plus_tard (GtkButton *b, gpointer d) { (void) b; (void) d; lancer_demarrage (NULL, FALSE); }
static void on_clic_oublier   (GtkButton *b, gpointer d) { (void) b; (void) d; lancer_demarrage (NULL, TRUE); }

static void
on_nom_suivant (gpointer d)
{
    (void) d;
    if (C.en_cours)
        return;

    const char *saisi = gtk_editable_get_text (GTK_EDITABLE (C.nom_champ));
    if (saisi == NULL || *saisi == '\0') {
        dire ("Saisir le nom d'utilisateur.", TRUE);
        return;
    }

    /* Le compte est vérifié ICI plutôt que laissé à PAM. Sur une machine
     * personnelle, répondre « mot de passe incorrect » à un nom mal tapé
     * envoie chercher une faute là où il n'y en a pas. Le secret que
     * protégerait un message uniforme — l'existence du compte — n'en est
     * pas un devant l'écran de sa propre machine. */
    struct passwd *p = getpwnam (saisi);
    if (p == NULL || p->pw_uid < 1000 || p->pw_uid >= 60000) {
        dire ("Compte inconnu.", TRUE);
        return;
    }

    g_free (C.utilisateur);
    C.utilisateur = g_strdup (p->pw_name);
    aller_a (V_MDP);
}

/* --- construction -------------------------------------------------------- */
static GtkWidget *
volet_nom (void)
{
    GtkWidget *carte = carte_neuve ();

    gtk_box_append (GTK_BOX (carte), titre_neuf ("Ouvrir une session"));

    C.nom_champ = gtk_entry_new ();
    gtk_widget_add_css_class (C.nom_champ, "connexion-mdp");
    gtk_editable_set_width_chars (GTK_EDITABLE (C.nom_champ), 22);
    gtk_entry_set_placeholder_text (GTK_ENTRY (C.nom_champ), "Nom d'utilisateur");
    gtk_entry_set_input_purpose (GTK_ENTRY (C.nom_champ), GTK_INPUT_PURPOSE_NAME);
    /* Entrée mène au volet suivant, et non à une tentative : le mot de passe
     * n'est pas encore saisi. */
    g_signal_connect (C.nom_champ, "activate", G_CALLBACK (on_clic_suivant), NULL);
    gtk_box_append (GTK_BOX (carte), C.nom_champ);

    C.message[V_NOM] = message_neuf ();
    gtk_box_append (GTK_BOX (carte), C.message[V_NOM]);

    GtkWidget *suivant = gtk_button_new_with_label ("Suivant");
    gtk_widget_add_css_class (suivant, "connexion-valider");
    gtk_widget_set_can_focus (suivant, FALSE);
    gtk_widget_set_focus_on_click (suivant, FALSE);
    g_signal_connect (suivant, "clicked", G_CALLBACK (on_clic_suivant), NULL);
    gtk_box_append (GTK_BOX (carte), suivant);

    GtkWidget *clavier = shell_clavier_azerty (GTK_EDITABLE (C.nom_champ),
                                               on_nom_suivant, NULL);
    gtk_widget_set_size_request (clavier, 680, 246);

    return volet_neuf (carte, clavier);
}

static GtkWidget *
volet_mdp (void)
{
    GtkWidget *carte = carte_neuve ();

    C.titre_mdp = titre_neuf ("");
    gtk_box_append (GTK_BOX (carte), C.titre_mdp);

    C.mdp = gtk_password_entry_new ();
    gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (C.mdp), TRUE);
    gtk_widget_add_css_class (C.mdp, "connexion-mdp");
    gtk_editable_set_width_chars (GTK_EDITABLE (C.mdp), 22);
    /* GtkPasswordEntry n'est PAS un GtkEntry — il implémente GtkEditable
     * sans en dériver. gtk_entry_set_placeholder_text échouait donc sur son
     * assertion de type, en silence à l'écran. On passe par la propriété. */
    g_object_set (C.mdp, "placeholder-text", "Mot de passe", NULL);
    g_signal_connect (C.mdp, "activate", G_CALLBACK (on_active_mdp), NULL);
    gtk_box_append (GTK_BOX (carte), C.mdp);

    C.message[V_MDP] = message_neuf ();
    gtk_box_append (GTK_BOX (carte), C.message[V_MDP]);

    C.lien_pin = lien_neuf ("Utiliser le code PIN", G_CALLBACK (on_clic_vers_pin));
    gtk_box_append (GTK_BOX (carte), C.lien_pin);

    GtkWidget *clavier = shell_clavier_azerty (GTK_EDITABLE (C.mdp),
                                               on_entree_mdp, NULL);
    gtk_widget_set_size_request (clavier, 680, 246);

    return volet_neuf (carte, clavier);
}

static GtkWidget *
volet_pin (void)
{
    GtkWidget *carte = carte_neuve ();

    C.titre_pin = titre_neuf ("");
    gtk_box_append (GTK_BOX (carte), C.titre_pin);

    C.pin = pastilles_neuves ();
    gtk_box_append (GTK_BOX (carte), C.pin->boite);

    C.message[V_PIN] = message_neuf ();
    gtk_box_append (GTK_BOX (carte), C.message[V_PIN]);

    /* LA SORTIE DE SECOURS, ET ELLE EST TOUJOURS LÀ. Un écran de connexion
     * qui n'accepterait qu'un code à six chiffres enfermerait dehors le jour
     * où il est oublié. */
    gtk_box_append (GTK_BOX (carte),
                    lien_neuf ("Utiliser le mot de passe",
                               G_CALLBACK (on_clic_vers_mdp)));

    GtkWidget *pave = shell_clavier_pave (saisir_pin, NULL);
    gtk_widget_set_size_request (pave, 264, 336);
    gtk_widget_set_halign (pave, GTK_ALIGN_CENTER);

    return volet_neuf (carte, pave);
}

static GtkWidget *
volet_choix (void)
{
    GtkWidget *carte = carte_neuve ();

    C.titre_choix = titre_neuf ("Choisissez un code à six chiffres");
    gtk_label_set_wrap (GTK_LABEL (C.titre_choix), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (C.titre_choix), 26);
    gtk_label_set_justify (GTK_LABEL (C.titre_choix), GTK_JUSTIFY_CENTER);
    gtk_box_append (GTK_BOX (carte), C.titre_choix);

    C.choix = pastilles_neuves ();
    gtk_box_append (GTK_BOX (carte), C.choix->boite);

    C.message[V_CHOIX] = message_neuf ();
    gtk_box_append (GTK_BOX (carte), C.message[V_CHOIX]);

    gtk_box_append (GTK_BOX (carte),
                    lien_neuf ("Plus tard", G_CALLBACK (on_clic_plus_tard)));

    C.bouton_oublier = lien_neuf ("Ne plus utiliser de code PIN",
                                  G_CALLBACK (on_clic_oublier));
    gtk_box_append (GTK_BOX (carte), C.bouton_oublier);

    GtkWidget *pave = shell_clavier_pave (saisir_choix, NULL);
    gtk_widget_set_size_request (pave, 264, 336);
    gtk_widget_set_halign (pave, GTK_ALIGN_CENTER);

    return volet_neuf (carte, pave);
}

/* --- la navigation ------------------------------------------------------- */
static void
aller_a (Volet v)
{
    C.volet = v;
    gtk_stack_set_visible_child_name (GTK_STACK (C.pile), NOMS_VOLET[v]);
    dire ("", FALSE);

    g_autofree char *nom = nom_affiche (C.utilisateur);

    switch (v) {
    case V_NOM:
        gtk_widget_grab_focus (C.nom_champ);
        break;

    case V_MDP:
        gtk_label_set_text (GTK_LABEL (C.titre_mdp), nom);
        gtk_widget_set_visible (C.lien_pin, C.a_un_pin);
        gtk_editable_set_text (GTK_EDITABLE (C.mdp), "");
        gtk_widget_set_sensitive (C.mdp, TRUE);
        gtk_widget_grab_focus (C.mdp);
        break;

    case V_PIN:
        gtk_label_set_text (GTK_LABEL (C.titre_pin), nom);
        pastilles_vider (C.pin);
        if (C.essais_restants > 0 && C.essais_restants < 5) {
            g_autofree char *reste = g_strdup_printf (
                C.essais_restants > 1 ? "Encore %d essais avant que le code "
                                        "PIN ne soit supprimé."
                                      : "Encore %d essai avant que le code "
                                        "PIN ne soit supprimé.",
                C.essais_restants);
            dire (reste, FALSE);
        }
        break;

    case V_CHOIX:
        /* « Ne plus utiliser » n'a de sens que s'il y a quelque chose à
         * retirer. Proposer de supprimer ce qui n'existe pas est le genre de
         * détail qui fait douter du reste de l'écran. */
        gtk_widget_set_visible (C.bouton_oublier, C.a_un_pin);
        choix_recommencer (NULL);
        break;

    default:
        break;
    }
}
/* --- horloge, alignée sur la minute comme celle de la barre d'état ------- */
static gboolean on_minute (gpointer data);

static void
replanifier (void)
{
    g_autoptr(GDateTime) maintenant = g_date_time_new_now_local ();
    guint delai = (60 - g_date_time_get_second (maintenant)) * 1000;
    g_timeout_add (delai < 500 ? 500 : delai, on_minute, NULL);
}

/* La date est écrite ici, et non par g_date_time_format avec %A et %B, qui
 * suivent la locale du système. Sur une machine dont la locale n'aurait pas
 * été mise en français, le premier écran affiché dirait « Saturday
 * 5 September » au milieu d'une interface française. Le reste du bureau ne
 * dépend d'aucune locale ; celui-ci ne doit pas faire exception. */
static const char *const JOURS[] = {
    "dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"
};
static const char *const MOIS[] = {
    "janvier", "février", "mars", "avril", "mai", "juin",
    "juillet", "août", "septembre", "octobre", "novembre", "décembre"
};

static void
maj_horloge (void)
{
    g_autoptr(GDateTime) maintenant = g_date_time_new_now_local ();
    g_autofree char *h = g_date_time_format (maintenant, "%H:%M");

    /* g_date_time_get_day_of_week rend 1 pour lundi et 7 pour dimanche. */
    int jour = g_date_time_get_day_of_week (maintenant) % 7;
    int mois = g_date_time_get_month (maintenant) - 1;

    g_autofree char *j = g_strdup_printf ("%s %d %s", JOURS[jour],
                                          g_date_time_get_day_of_month (maintenant),
                                          MOIS[mois]);

    gtk_label_set_text (GTK_LABEL (C.heure), h);
    gtk_label_set_text (GTK_LABEL (C.date), j);
}

static gboolean
on_minute (gpointer data)
{
    (void) data;
    maj_horloge ();
    replanifier ();
    return G_SOURCE_REMOVE;
}


/* ------------------------------------------------------------------------- */
static void
on_activate (GtkApplication *app, gpointer user_data)
{
    ShellConfig *cfg = user_data;
    shell_config_apply (cfg);

    C.fenetre = gtk_application_window_new (app);
    gtk_widget_add_css_class (C.fenetre, "shell");
    gtk_widget_add_css_class (C.fenetre, "connexion");

    gtk_layer_init_for_window (GTK_WINDOW (C.fenetre));
    gtk_layer_set_layer (GTK_WINDOW (C.fenetre), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace (GTK_WINDOW (C.fenetre), "claude-os-connexion");
    for (int bord = 0; bord < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; bord++)
        gtk_layer_set_anchor (GTK_WINDOW (C.fenetre), bord, TRUE);
    gtk_layer_set_exclusive_zone (GTK_WINDOW (C.fenetre), -1);
    /* EXCLUSIVE : la frappe doit arriver au champ sans qu'on ait à cliquer
     * dedans. C'est un écran de connexion, il n'y a rien d'autre à viser. */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (C.fenetre),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    /* --- les volets --- */
    C.pile = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (C.pile),
                                   GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration (GTK_STACK (C.pile), 140);
    gtk_widget_set_halign (C.pile, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (C.pile, GTK_ALIGN_CENTER);

    gtk_stack_add_named (GTK_STACK (C.pile), volet_nom (),   NOMS_VOLET[V_NOM]);
    gtk_stack_add_named (GTK_STACK (C.pile), volet_mdp (),   NOMS_VOLET[V_MDP]);
    gtk_stack_add_named (GTK_STACK (C.pile), volet_pin (),   NOMS_VOLET[V_PIN]);
    gtk_stack_add_named (GTK_STACK (C.pile), volet_choix (), NOMS_VOLET[V_CHOIX]);

    /* --- l'heure, en bas à droite, comme la barre d'état --- */
    C.heure = gtk_label_new ("--:--");
    gtk_widget_add_css_class (C.heure, "connexion-heure");
    gtk_widget_set_halign (C.heure, GTK_ALIGN_END);

    C.date = gtk_label_new ("");
    gtk_widget_add_css_class (C.date, "connexion-date");
    gtk_widget_set_halign (C.date, GTK_ALIGN_END);

    GtkWidget *coin = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (coin, "connexion-coin");
    gtk_widget_set_halign (coin, GTK_ALIGN_END);
    gtk_widget_set_valign (coin, GTK_ALIGN_END);
    gtk_box_append (GTK_BOX (coin), C.heure);
    gtk_box_append (GTK_BOX (coin), C.date);

    GtkWidget *pile = gtk_overlay_new ();
    /* Le dégradé va sur ce conteneur, PAS sur la fenêtre : « window.shell »
     * la déclare transparente — c'est ce qu'il faut pour une surface qui
     * flotte — et cette règle l'emporterait. Le fond d'écran du bureau fait
     * exactement pareil, sur sa pile. */
    gtk_widget_add_css_class (pile, "fond-degrade");
    gtk_overlay_set_child (GTK_OVERLAY (pile), C.pile);
    gtk_overlay_add_overlay (GTK_OVERLAY (pile), coin);

    gtk_window_set_child (GTK_WINDOW (C.fenetre), pile);

    /* La frappe physique des volets sans champ de saisie. Voir on_frappe. */
    GtkEventController *clavier = gtk_event_controller_key_new ();
    g_signal_connect (clavier, "key-pressed", G_CALLBACK (on_frappe), NULL);
    gtk_widget_add_controller (C.fenetre, clavier);

    /* Au banc d'essai, la couche layer-shell est bouchonnée : sans ancrage,
     * la fenêtre se réduirait à la carte et la composition d'ensemble —
     * carte centrée, horloge au coin — ne se verrait pas. */
    if (C.apercu)
        gtk_window_set_default_size (GTK_WINDOW (C.fenetre), 1366, 768);

    gtk_window_present (GTK_WINDOW (C.fenetre));

    /* Un code PIN enregistré : c'est le cas courant, et l'écran s'ouvre
     * dessus. Sinon, la première connexion — le nom, puis le mot de passe. */
    Volet depart = C.a_un_pin ? V_PIN : V_NOM;
    if (C.volet_force != NULL)
        for (int v = 0; v < V_NOMBRE; v++)
            if (g_strcmp0 (NOMS_VOLET[v], C.volet_force) == 0)
                depart = (Volet) v;
    aller_a (depart);

    maj_horloge ();
    replanifier ();
}

int
main (int argc, char **argv)
{
    C.session = g_strdup (SESSION_DEFAUT);
    const char *theme_force = NULL;
    const char *volet_force = NULL;

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0 (argv[i], "--apercu") == 0) {
            C.apercu = TRUE;
        } else if (g_strcmp0 (argv[i], "--utilisateur") == 0 && i + 1 < argc) {
            C.utilisateur = g_strdup (argv[++i]);
        } else if (g_strcmp0 (argv[i], "--session") == 0 && i + 1 < argc) {
            g_free (C.session);
            C.session = g_strdup (argv[++i]);
        } else if (g_str_has_prefix (argv[i], "--volet=")) {
            /* Banc d'essai : ouvrir directement sur un volet donné. Sans
             * cela, « choix » et « mot de passe » ne s'atteignent qu'en
             * cliquant, donc jamais dans une capture automatique — et les
             * volets qu'on ne regarde pas sont ceux qui cassent. */
            volet_force = argv[i] + strlen ("--volet=");
        } else if (g_str_has_prefix (argv[i], "--theme=")) {
            /* Pour le banc d'essai : parcourir les quatre thèmes sans avoir
             * à changer celui du bureau. */
            theme_force = argv[i] + strlen ("--theme=");
        }
    }

    if (C.utilisateur == NULL)
        C.utilisateur = utilisateur_unique ();
    if (C.utilisateur == NULL) {
        g_printerr ("Aucun compte humain trouvé, et --utilisateur non fourni.\n");
        return 1;
    }

    ShellConfig *cfg = shell_config_load ();
    g_autofree char *theme = NULL;

    if (C.apercu) {
        /* Le coffre n'est pas sollicité au banc d'essai : il écrirait dans
         * /var/lib pour un écran qui ne sert qu'à regarder. On simule un
         * compte qui a déjà un code PIN, ce qui rend les quatre volets
         * atteignables — PIN, puis « utiliser le mot de passe », puis le
         * choix d'un code après authentification. Le code d'aperçu est
         * 123456. */
        C.coffre_la = TRUE;
        C.a_un_pin = TRUE;
        C.essais_restants = 5;
    } else {
        coffre_etat (&C.a_un_pin, &C.essais_restants, &theme);
    }

    /* LE THÈME SUIT CELUI DU BUREAU, ET IL FAUT POSER « dark » AVEC LUI.
     *
     * L'ancienne version écrivait cfg->theme directement : la feuille de
     * style suivait, mais cfg->dark restait à FALSE, et les widgets natifs
     * de GTK se dessinaient clairs sur un fond sombre. shell_config_set_theme
     * pose les deux ensemble — c'est la raison d'être de cette fonction.
     *
     * Et elle échoue bruyamment : un identifiant inconnu laisse le thème par
     * défaut ET le dit. Le repli muet de la version précédente est ce qui a
     * caché des semaines durant que la lecture ne marchait pas du tout. */
    const char *voulu = (theme_force != NULL) ? theme_force : theme;
    if (voulu != NULL && !shell_config_set_theme (cfg, voulu))
        g_printerr ("thème « %s » inconnu ; « %s » conservé.\n", voulu, cfg->theme);

    C.volet_force = volet_force;

    GtkApplication *app = gtk_application_new ("os.claude.shell.connexion",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "startup",  G_CALLBACK (shell_styles_startup), cfg);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), cfg);

    int status = g_application_run (G_APPLICATION (app), 0, NULL);
    g_object_unref (app);

    /* Le mot de passe ne traîne pas dans un tas qu'on rendra au système. */
    if (C.mdp_valide != NULL) {
        explicit_bzero (C.mdp_valide, strlen (C.mdp_valide));
        g_clear_pointer (&C.mdp_valide, g_free);
    }
    g_clear_object (&C.greetd);
    return status;
}
