/* =========================================================================
 * Claude OS — écran de connexion
 *
 * Un seul utilisateur, un seul champ : le mot de passe. Pas de choix de
 * compte, pas de choix de session, pas de menu.
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

#define SESSION_DEFAUT "/usr/local/bin/claude-os-session"

static struct {
    GtkWidget *fenetre;
    GtkWidget *carte;
    GtkWidget *mdp;
    GtkWidget *erreur;
    GtkWidget *heure;
    GtkWidget *date;

    char      *utilisateur;   /* compte à ouvrir                            */
    char      *session;       /* commande lancée après authentification     */
    gboolean   apercu;        /* banc d'essai : aucun greetd derrière       */
    gboolean   en_cours;      /* une tentative est en vol                   */
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

/* Le thème du bureau, lu chez l'utilisateur.
 *
 * L'écran de connexion tourne sous le compte « _greetd », qui n'a pas de
 * configuration. Sans cette lecture, il resterait clair pendant que le
 * bureau est sombre — deux écrans qui se suivent et ne se ressemblent pas.
 * Une lecture, une seule clé, et un repli si le fichier est illisible. */
static char *
theme_de (const char *compte)
{
    struct passwd *p = getpwnam (compte);
    if (p == NULL || p->pw_dir == NULL)
        return g_strdup ("claude-sombre");

    g_autofree char *chemin = g_build_filename (p->pw_dir, ".config",
                                                "claude-os", "shell.conf", NULL);
    g_autoptr(GKeyFile) kf = g_key_file_new ();
    if (!g_key_file_load_from_file (kf, chemin, G_KEY_FILE_NONE, NULL))
        return g_strdup ("claude-sombre");

    char *t = g_key_file_get_string (kf, "appearance", "theme", NULL);
    return (t != NULL && *t != '\0') ? t : g_strdup ("claude-sombre");
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

/* --- la tentative, dans un fil séparé -----------------------------------
 *
 * PAM impose un délai de deux secondes après un mot de passe faux. Fait sur
 * le fil principal, ce délai figerait l'écran : le champ ne se viderait pas,
 * le message n'apparaîtrait pas, et on croirait la machine plantée. */
static void
tache_connexion (GTask *tache, gpointer source, gpointer donnees,
                 GCancellable *annulation)
{
    const char *mdp = donnees;
    g_autoptr(GError) err = NULL;
    (void) source; (void) annulation;

    if (C.apercu) {
        g_usleep (600 * 1000);
        g_task_return_boolean (tache, TRUE);
        return;
    }

    const char *chemin = g_getenv ("GREETD_SOCK");
    if (chemin == NULL) {
        g_task_return_new_error (tache, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                 "GREETD_SOCK absent : lancé hors de greetd ?");
        return;
    }

    g_autoptr(GSocket) s = g_socket_new (G_SOCKET_FAMILY_UNIX,
                                         G_SOCKET_TYPE_STREAM,
                                         G_SOCKET_PROTOCOL_DEFAULT, &err);
    if (s == NULL) { g_task_return_error (tache, g_steal_pointer (&err)); return; }

    g_autoptr(GSocketAddress) adresse = g_unix_socket_address_new (chemin);
    if (!g_socket_connect (s, adresse, NULL, &err)) {
        g_task_return_error (tache, g_steal_pointer (&err));
        return;
    }

    g_autoptr(JsonNode) demande =
        message_simple ("create_session", "username", C.utilisateur);
    if (!envoyer (s, demande, &err)) {
        g_task_return_error (tache, g_steal_pointer (&err));
        return;
    }

    /* PAM peut poser plusieurs questions. On répond au premier secret avec
     * le mot de passe ; toute question SUPPLÉMENTAIRE — second facteur,
     * changement de mot de passe imposé — n'a pas d'interface ici, et on le
     * dit plutôt que de répondre n'importe quoi. */
    gboolean mdp_donne = FALSE;

    for (;;) {
        g_autoptr(JsonParser) parser = NULL;
        JsonObject *o = recevoir (s, &parser, &err);
        if (o == NULL) { g_task_return_error (tache, g_steal_pointer (&err)); return; }

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
                g_task_return_new_error (tache, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                         "PAM demande autre chose : « %s »", texte);
                return;
            }
            /* info et error : rien à répondre, mais le texte est utile. */

            g_autoptr(JsonNode) rep =
                message_simple ("post_auth_message_response", "response", reponse);
            if (!envoyer (s, rep, &err)) {
                g_task_return_error (tache, g_steal_pointer (&err));
                return;
            }
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
                g_task_return_new_error (tache, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         "Mot de passe incorrect.");
            else
                g_task_return_new_error (tache, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         "%s", *desc ? desc : "échec de la connexion");
            return;
        }

        if (g_strcmp0 (type, "success") == 0) {
            if (mdp_donne)
                break;              /* authentifié : on peut démarrer */
            /* Un « success » avant toute question veut dire que la session
             * est déjà créée ; on continue d'écouter. */
            break;
        }
    }

    g_autoptr(JsonNode) demarrer = message_demarrage (C.session);
    if (!envoyer (s, demarrer, &err)) {
        g_task_return_error (tache, g_steal_pointer (&err));
        return;
    }

    g_autoptr(JsonParser) parser = NULL;
    JsonObject *o = recevoir (s, &parser, &err);
    if (o == NULL) { g_task_return_error (tache, g_steal_pointer (&err)); return; }

    if (g_strcmp0 (json_object_get_string_member_with_default (o, "type", ""),
                   "success") != 0) {
        g_task_return_new_error (tache, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
            json_object_get_string_member_with_default (o, "description",
                                                        "la session n'a pas démarré"));
        return;
    }

    g_task_return_boolean (tache, TRUE);
}

/* -------------------------------------------------------------------------
 * Interface
 * ------------------------------------------------------------------------- */
static void
dire (const char *texte, gboolean faute)
{
    gtk_label_set_text (GTK_LABEL (C.erreur), texte ? texte : "");
    if (faute)
        gtk_widget_add_css_class (C.erreur, "faute");
    else
        gtk_widget_remove_css_class (C.erreur, "faute");
}

static void
on_connexion_finie (GObject *source, GAsyncResult *res, gpointer data)
{
    g_autoptr(GError) err = NULL;
    (void) source; (void) data;

    C.en_cours = FALSE;
    gtk_widget_set_sensitive (C.mdp, TRUE);

    if (g_task_propagate_boolean (G_TASK (res), &err)) {
        /* greetd remplace ce programme par la session : on ne revient pas
         * de là. Le message ne sert que le temps de la bascule. */
        dire ("Ouverture de la session…", FALSE);
        gtk_widget_set_sensitive (C.mdp, FALSE);
        return;
    }

    dire (err->message, TRUE);
    gtk_editable_set_text (GTK_EDITABLE (C.mdp), "");
    gtk_widget_grab_focus (C.mdp);
}

static void
tenter (void)
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

    GTask *t = g_task_new (NULL, NULL, on_connexion_finie, NULL);
    g_task_set_task_data (t, g_strdup (mdp), g_free);
    g_task_run_in_thread (t, tache_connexion);
    g_object_unref (t);
}

static void on_valider (GtkEntry *e, gpointer d) { (void) e; (void) d; tenter (); }

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

    /* --- la carte centrale --- */
    g_autofree char *nom = nom_affiche (C.utilisateur);
    GtkWidget *etiquette = gtk_label_new (nom);
    gtk_widget_add_css_class (etiquette, "connexion-nom");

    C.mdp = gtk_password_entry_new ();
    gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (C.mdp), TRUE);
    gtk_widget_add_css_class (C.mdp, "connexion-mdp");
    gtk_editable_set_width_chars (GTK_EDITABLE (C.mdp), 22);
    /* GtkPasswordEntry n'est PAS un GtkEntry — il implémente GtkEditable
     * sans en dériver. gtk_entry_set_placeholder_text échouait donc sur son
     * assertion de type, en silence à l'écran. On passe par la propriété. */
    g_object_set (C.mdp, "placeholder-text", "Mot de passe", NULL);
    g_signal_connect (C.mdp, "activate", G_CALLBACK (on_valider), NULL);

    C.erreur = gtk_label_new ("");
    gtk_widget_add_css_class (C.erreur, "connexion-message");
    gtk_label_set_wrap (GTK_LABEL (C.erreur), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (C.erreur), 34);
    gtk_label_set_justify (GTK_LABEL (C.erreur), GTK_JUSTIFY_CENTER);

    C.carte = gtk_box_new (GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class (C.carte, "connexion-carte");
    gtk_widget_set_halign (C.carte, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (C.carte, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (C.carte), etiquette);
    gtk_box_append (GTK_BOX (C.carte), C.mdp);
    gtk_box_append (GTK_BOX (C.carte), C.erreur);

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
    gtk_overlay_set_child (GTK_OVERLAY (pile), C.carte);
    gtk_overlay_add_overlay (GTK_OVERLAY (pile), coin);

    gtk_window_set_child (GTK_WINDOW (C.fenetre), pile);

    /* Au banc d'essai, la couche layer-shell est bouchonnée : sans ancrage,
     * la fenêtre se réduirait à la carte et la composition d'ensemble —
     * carte centrée, horloge au coin — ne se verrait pas. */
    if (C.apercu)
        gtk_window_set_default_size (GTK_WINDOW (C.fenetre), 1400, 900);

    gtk_window_present (GTK_WINDOW (C.fenetre));
    gtk_widget_grab_focus (C.mdp);

    maj_horloge ();
    replanifier ();
}

int
main (int argc, char **argv)
{
    C.session = g_strdup (SESSION_DEFAUT);

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0 (argv[i], "--apercu") == 0) {
            C.apercu = TRUE;
        } else if (g_strcmp0 (argv[i], "--utilisateur") == 0 && i + 1 < argc) {
            C.utilisateur = g_strdup (argv[++i]);
        } else if (g_strcmp0 (argv[i], "--session") == 0 && i + 1 < argc) {
            g_free (C.session);
            C.session = g_strdup (argv[++i]);
        }
    }

    if (C.utilisateur == NULL)
        C.utilisateur = utilisateur_unique ();
    if (C.utilisateur == NULL) {
        g_printerr ("Aucun compte humain trouvé, et --utilisateur non fourni.\n");
        return 1;
    }

    /* Le thème suit celui du bureau de cet utilisateur. */
    ShellConfig *cfg = shell_config_load ();
    g_free (cfg->theme);
    cfg->theme = theme_de (C.utilisateur);

    GtkApplication *app = gtk_application_new ("os.claude.shell.connexion",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "startup",  G_CALLBACK (shell_styles_startup), cfg);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), cfg);

    int status = g_application_run (G_APPLICATION (app), 0, NULL);
    g_object_unref (app);
    return status;
}
