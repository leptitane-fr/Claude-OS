/* =========================================================================
 * Claude-OS Shell — claude-os-outils : lire, et jouer, le contrat des outils
 *
 * POURQUOI CET OUTIL EXISTE AVANT L'INTERFACE. Le contrat des outils
 * (outils.h) tient entre deux processus ; tant qu'un seul des deux est
 * écrit, rien ne prouve qu'il fonctionne. Ce programme est l'autre bout :
 * il lit ce qu'une application publie, et sait se faire passer pour le dock
 * — présentation et Prise comprises. Le protocole a donc été éprouvé avant
 * qu'une seule ligne d'interface ne soit dessinée.
 *
 * Il reste utile ensuite, et c'est sa vraie raison d'être : le jour où une
 * barre ne s'affiche pas, il dit lequel des deux côtés se tait.
 *
 *   claude-os-outils os.claude.shell.fichiers
 *       lit la barre publiée et l'imprime. Ne prend rien en charge : ce que
 *       fait l'application n'est pas modifié.
 *
 *   claude-os-outils --dock
 *       PREND LE NOM DU DOCK, et le vrai dock ne peut alors plus démarrer —
 *       ni l'inverse. Attend les présentations, imprime chaque barre reçue,
 *       et répond Prise(true). C'est le mode du banc d'essai.
 *
 *   claude-os-outils --temoin
 *       Publie la barre de référence du contrat — lieux, fil d'Ariane et un
 *       auvent de saisie — et imprime ce qu'on y déclenche. C'est
 *       l'exemple, et il compile : voir « L'application témoin » plus bas.
 *
 * Les trois ensemble se suffisent : « --temoin » dans un terminal,
 * « --dock » dans un second, et le protocole se joue en entier sans qu'un
 * seul pixel soit dessiné.
 *
 * SANS GTK, à dessein : un outil de diagnostic qui tire un runtime
 * graphique complet ne peut pas servir quand c'est le graphique qui est en
 * panne. GIO suffit — GDBusMenuModel et GDBusActionGroup y vivent.
 *
 * IL DIT CE QU'IL SAIT, ET SEULEMENT CELA. Les modèles D-Bus se remplissent
 * de façon asynchrone : une barre lue trop tôt paraît vide. Ce programme
 * attend donc, et quand il n'a rien reçu il écrit « rien n'est arrivé en
 * N ms » plutôt que « la barre est vide ». Deux faux négatifs ont déjà
 * coûté une séance à ce projet ; un outil qui ment coûte plus qu'il ne
 * rapporte.
 * ========================================================================= */

#include <locale.h>
#include <string.h>

#include <gio/gio.h>

#include "outils.h"

#define ATTENTE_MS 1500   /* le temps laissé au modèle pour arriver */

static GMainLoop *boucle;

/* En mode « --dock », le programme reste en poste : plusieurs applications
 * peuvent se présenter, et une même application se représente chaque fois
 * qu'elle est relancée. En mode lecture, il rend la main dès qu'il a dit ce
 * qu'il avait à dire. */
static gboolean en_poste;

/* -------------------------------------------------------------------------
 * Impression d'un modèle
 * ------------------------------------------------------------------------- */
static char *
attribut (GMenuModel *m, int i, const char *nom)
{
    g_autoptr(GVariant) v = g_menu_model_get_item_attribute_value (
        m, i, nom, G_VARIANT_TYPE_STRING);
    return v ? g_variant_dup_string (v, NULL) : NULL;
}

/* La cible d'une entrée peut être de n'importe quel type : on l'imprime
 * telle qu'elle est plutôt que de supposer une chaîne. */
static char *
cible (GMenuModel *m, int i)
{
    g_autoptr(GVariant) v = g_menu_model_get_item_attribute_value (
        m, i, G_MENU_ATTRIBUTE_TARGET, NULL);
    return v ? g_variant_print (v, FALSE) : NULL;
}

static void imprimer_modele (GMenuModel *m, GActionGroup *actions, int creux);

/* Une entrée est allumée si l'état de son action vaut sa cible : c'est la
 * sémantique radio de GMenu, et c'est ainsi que le contrat dit « on est
 * ici » sans inventer d'attribut. */
static gboolean
est_courante (GActionGroup *actions, const char *action, GVariant *but)
{
    if (actions == NULL || action == NULL || but == NULL)
        return FALSE;

    const char *nu = strchr (action, '.');
    nu = nu ? nu + 1 : action;

    g_autoptr(GVariant) etat = g_action_group_get_action_state (actions, nu);
    return etat != NULL && g_variant_equal (etat, but);
}

static void
imprimer_entree (GMenuModel *m, int i, GActionGroup *actions, int creux)
{
    g_autofree char *label  = attribut (m, i, G_MENU_ATTRIBUTE_LABEL);
    g_autofree char *action = attribut (m, i, G_MENU_ATTRIBUTE_ACTION);
    g_autofree char *icone  = attribut (m, i, "icon");
    g_autofree char *forme  = attribut (m, i, SHELL_OUTILS_A_FORME);
    g_autofree char *ctrl   = attribut (m, i, SHELL_OUTILS_A_CONTROLE);
    g_autofree char *invite = attribut (m, i, SHELL_OUTILS_A_INVITE);
    g_autofree char *cle    = attribut (m, i, SHELL_OUTILS_A_CLE);
    g_autofree char *but    = cible (m, i);

    g_autoptr(GVariant) but_v = g_menu_model_get_item_attribute_value (
        m, i, G_MENU_ATTRIBUTE_TARGET, NULL);
    gboolean ici = est_courante (actions, action, but_v);

    g_print ("%*s%s %-22s forme=%-7s action=%-20s",
             creux * 2, "", ici ? "▶" : "·",
             label ? label : "(sans libellé)",
             forme ? forme : SHELL_OUTILS_BOUTON,
             action ? action : "—");
    if (but != NULL)   g_print (" cible=%s", but);
    if (icone != NULL) g_print (" icône=%s", icone);
    if (ctrl != NULL)  g_print (" contrôle=%s", ctrl);
    if (invite != NULL) g_print (" invite=« %s »", invite);
    if (cle != NULL)   g_print (" touche=%s", cle);
    g_print ("\n");

    /* Un sous-menu, s'il y en a un : le contrat ne s'en sert pas encore,
     * mais GMenuModel le permet et l'ignorer en silence serait mentir. */
    g_autoptr(GMenuModel) sous = g_menu_model_get_item_link (
        m, i, G_MENU_LINK_SUBMENU);
    if (sous != NULL)
        imprimer_modele (sous, actions, creux + 1);
}

static void
imprimer_modele (GMenuModel *m, GActionGroup *actions, int creux)
{
    int n = g_menu_model_get_n_items (m);

    for (int i = 0; i < n; i++) {
        g_autoptr(GMenuModel) section = g_menu_model_get_item_link (
            m, i, G_MENU_LINK_SECTION);

        if (section != NULL) {
            g_autofree char *zone  = attribut (m, i, SHELL_OUTILS_A_ZONE);
            g_autofree char *titre = attribut (m, i, G_MENU_ATTRIBUTE_LABEL);

            g_print ("%*s[ zone %-6s ] %s\n", creux * 2, "",
                     zone ? zone : "(aucune ⇒ outils)",
                     titre ? titre : "");
            imprimer_modele (section, actions, creux + 1);
            continue;
        }
        imprimer_entree (m, i, actions, creux);
    }
}

/* -------------------------------------------------------------------------
 * Lire une application
 * ------------------------------------------------------------------------- */
typedef struct {
    char         *nom;
    GMenuModel   *barre;
    GActionGroup *actions;
    guint         echeance;
    gboolean      vu;
} Lecture;

static void
rendre_compte (Lecture *L)
{
    if (L->echeance != 0) {
        g_source_remove (L->echeance);
        L->echeance = 0;
    }

    int n = g_menu_model_get_n_items (L->barre);
    if (n == 0) {
        g_print ("\nRien n'est arrivé en %d ms.\n"
                 "  Ce n'est PAS « la barre est vide » : c'est « aucun "
                 "contenu n'a été reçu ».\n"
                 "  L'application publie-t-elle bien un modèle non vide au "
                 "chemin %s ?\n", ATTENTE_MS, SHELL_OUTILS_CHEMIN);
        if (!en_poste)
            g_main_loop_quit (boucle);
        return;
    }

    g_print ("\nLa barre de « %s » :\n\n", L->nom);
    imprimer_modele (L->barre, L->actions, 0);

    g_autofree char **noms = g_action_group_list_actions (L->actions);
    g_print ("\nActions publiées (%u) :", noms ? g_strv_length (noms) : 0);
    for (guint i = 0; noms != NULL && noms[i] != NULL; i++) {
        g_autoptr(GVariant) etat = g_action_group_get_action_state (L->actions,
                                                                    noms[i]);
        g_autofree char *e = etat ? g_variant_print (etat, FALSE) : NULL;
        g_print ("\n  %s%s%s", noms[i], e ? " = " : "", e ? e : "");
    }
    g_print ("\n");
    if (!en_poste)
        g_main_loop_quit (boucle);
}

static gboolean
sur_echeance (gpointer data)
{
    Lecture *L = data;
    L->echeance = 0;
    rendre_compte (L);
    return G_SOURCE_REMOVE;
}

/* Le modèle arrive en plusieurs fois : la première salve dit qu'il y a
 * quelque chose, les suivantes complètent les sections. On n'imprime donc
 * pas au premier signal -- on laisse passer un souffle. */
static void
sur_contenu (GMenuModel *m, int pos, int retires, int ajoutes, gpointer data)
{
    Lecture *L = data;
    (void) m; (void) pos; (void) retires; (void) ajoutes;

    if (L->vu)
        return;
    L->vu = TRUE;

    if (L->echeance != 0)
        g_source_remove (L->echeance);
    L->echeance = g_timeout_add (200, sur_echeance, L);
}

static int
lire (GDBusConnection *bus, const char *nom)
{
    g_autoptr(GError) err = NULL;

    /* La propriété d'abord, et de façon SYNCHRONE : elle prouve que
     * quelqu'un répond à ce nom et parle ce contrat. Sans cette étape, une
     * application absente et une application muette se ressembleraient. */
    g_autoptr(GVariant) rep = g_dbus_connection_call_sync (
        bus, nom, SHELL_OUTILS_CHEMIN, "org.freedesktop.DBus.Properties",
        "GetAll", g_variant_new ("(s)", SHELL_OUTILS_IFACE),
        G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
        2000, NULL, &err);

    if (rep == NULL) {
        g_printerr ("« %s » ne publie pas d'outils : %s\n", nom, err->message);
        return 1;
    }

    g_autoptr(GVariant) dict = g_variant_get_child_value (rep, 0);
    guint32 contrat = 0;
    const char *titre = NULL;
    g_autoptr(GVariantDict) d = g_variant_dict_new (dict);
    g_variant_dict_lookup (d, "Contrat", "u", &contrat);
    g_variant_dict_lookup (d, "Titre", "&s", &titre);

    g_print ("%s — contrat %u%s, titre « %s »\n", nom, contrat,
             contrat == SHELL_OUTILS_CONTRAT ? "" : " (INCONNU DE CET OUTIL)",
             titre ? titre : "");

    Lecture *L = g_new0 (Lecture, 1);
    L->nom     = g_strdup (nom);
    L->barre   = G_MENU_MODEL (g_dbus_menu_model_get (bus, nom,
                                                      SHELL_OUTILS_CHEMIN));
    L->actions = G_ACTION_GROUP (g_dbus_action_group_get (bus, nom,
                                                          SHELL_OUTILS_CHEMIN));

    /* Amorce : GDBusActionGroup ne demande rien au serveur tant qu'on ne
     * lui a rien demandé. Sans cet appel, la liste reste vide, et l'état
     * des actions -- donc le lieu courant -- ne serait jamais connu. */
    g_strfreev (g_action_group_list_actions (L->actions));

    g_signal_connect (L->barre, "items-changed", G_CALLBACK (sur_contenu), L);
    /* Amorce du menu, de même : la lecture du nombre d'entrées déclenche
     * l'abonnement au serveur. */
    g_menu_model_get_n_items (L->barre);

    L->echeance = g_timeout_add (ATTENTE_MS, sur_echeance, L);
    return -1;   /* la boucle prendra la suite */
}

/* -------------------------------------------------------------------------
 * Jouer le dock
 * ------------------------------------------------------------------------- */
static GDBusConnection *bus_dock;

static void
sur_presentation (GSimpleAction *a, GVariant *params, gpointer data)
{
    (void) a; (void) data;

    /* org.gtk.Actions.Activate remet les paramètres tels quels : notre
     * action en attend un, une chaîne, le nom de bus de l'application. */
    if (params == NULL || !g_variant_is_of_type (params, G_VARIANT_TYPE_STRING)) {
        g_printerr ("présentation sans nom de bus — ignorée\n");
        return;
    }
    const char *nom = g_variant_get_string (params, NULL);
    g_print ("\n« %s » se présente.\n", nom);

    /* Prendre les outils en charge, comme le fera le dock. L'application
     * escamote alors ses replis : c'est la moitié du protocole que seul ce
     * mode sait éprouver. */
    g_dbus_connection_call (bus_dock, nom, SHELL_OUTILS_CHEMIN,
                            SHELL_OUTILS_IFACE, "Prise",
                            g_variant_new ("(b)", TRUE), NULL,
                            G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL,
                            NULL, NULL);

    lire (bus_dock, nom);
}

static const GActionEntry ACTIONS_DOCK[] = {
    { SHELL_OUTILS_PRESENTER, sur_presentation, "s", NULL, NULL, { 0 } },
};

static void
nom_pris (GDBusConnection *bus, const char *nom, gpointer data)
{
    (void) data;
    g_autoptr(GError) err = NULL;

    GSimpleActionGroup *g = g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (g), ACTIONS_DOCK,
                                     G_N_ELEMENTS (ACTIONS_DOCK), NULL);

    if (g_dbus_connection_export_action_group (bus, "/os/claude/shell/dock",
                                               G_ACTION_GROUP (g), &err) == 0)
        g_printerr ("actions du faux dock non publiées : %s\n", err->message);
    else
        g_print ("« %s » tenu. En attente des présentations — Ctrl-C pour "
                 "rendre la main.\n", nom);
}

static void
nom_perdu (GDBusConnection *bus, const char *nom, gpointer data)
{
    (void) bus; (void) data;
    g_printerr ("« %s » n'a pas pu être pris : le vrai dock tourne-t-il ?\n",
                nom);
    g_main_loop_quit (boucle);
}

/* -------------------------------------------------------------------------
 * L'application témoin
 *
 * LA BARRE DE RÉFÉRENCE DU CONTRAT. Elle n'a pas de fenêtre et ne fait
 * rien : elle publie, et imprime ce qu'on lui demande. Deux usages, et le
 * second est le plus durable :
 *
 *   - éprouver le dock sans dépendre d'une vraie application ;
 *   - MONTRER À QUOI RESSEMBLE UNE DÉCLARATION CORRECTE. Le code ci-dessous
 *     est l'exemple que docs/14 cite ; il compile, il tourne, il ne peut
 *     donc pas se périmer en silence comme se périme un exemple recopié
 *     dans un document.
 * ------------------------------------------------------------------------- */
static GMenuItem *
entree (const char *label, const char *action, const char *but,
        const char *icone, const char *forme)
{
    GMenuItem *it = g_menu_item_new (label, NULL);

    /* L'action et sa cible se posent ensemble : une cible sans action ne
     * veut rien dire, et GMenuItem ne le vérifie pas. */
    if (action != NULL && but != NULL)
        g_menu_item_set_action_and_target_value (it, action,
                                                 g_variant_new_string (but));
    else if (action != NULL)
        g_menu_item_set_action_and_target_value (it, action, NULL);

    if (icone != NULL)
        g_menu_item_set_attribute (it, "icon", "s", icone);
    if (forme != NULL)
        g_menu_item_set_attribute (it, SHELL_OUTILS_A_FORME, "s", forme);
    return it;
}

static void
section (GMenu *barre, const char *zone, const char *titre, GMenu *contenu)
{
    GMenuItem *sec = g_menu_item_new_section (titre, G_MENU_MODEL (contenu));
    g_menu_item_set_attribute (sec, SHELL_OUTILS_A_ZONE, "s", zone);
    g_menu_append_item (barre, sec);
    g_object_unref (sec);
}

static void
sur_aller (GSimpleAction *a, GVariant *but, gpointer data)
{
    (void) data;
    /* Poser l'état, c'est DIRE OÙ L'ON EST : le dock allume l'entrée dont
     * la cible vaut cet état. Une vraie application le ferait après avoir
     * navigué, pas avant. */
    g_simple_action_set_state (a, g_variant_ref (but));
    g_print ("→ aller %s\n", g_variant_get_string (but, NULL));
}

static void
sur_chercher (GSimpleAction *a, GVariant *texte, gpointer data)
{
    (void) a; (void) data;
    const char *t = g_variant_get_string (texte, NULL);
    g_print ("→ chercher « %s »%s\n", t, *t ? "" : "  (auvent refermé)");
}

static const GActionEntry ACTIONS_TEMOIN[] = {
    { "aller",    sur_aller,    "s", "'file:///home'", NULL, { 0 } },
    { "chercher", sur_chercher, "s", NULL,             NULL, { 0 } },
};

static void
sur_prise_temoin (gboolean prise, gpointer data)
{
    (void) data;
    g_print ("%s\n", prise
             ? "Le dock a pris les outils : une vraie application escamoterait "
               "ici son volet de repli."
             : "Pas de dock : une vraie application montrerait ici son volet "
               "de repli.");
}

static void
temoin_demarre (GApplication *app, gpointer data)
{
    (void) data;

    GSimpleActionGroup *actions = g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (actions), ACTIONS_TEMOIN,
                                     G_N_ELEMENTS (ACTIONS_TEMOIN), NULL);

    GMenu *barre = g_menu_new ();

    GMenu *perso = g_menu_new ();
    g_menu_append_item (perso, entree ("Accueil", "outils.aller",
                                       "file:///home/stef",
                                       "user-home-symbolic", SHELL_OUTILS_LIEU));
    g_menu_append_item (perso, entree ("Documents", "outils.aller",
                                       "file:///home/stef/Documents",
                                       "folder-documents-symbolic",
                                       SHELL_OUTILS_LIEU));
    g_menu_append_item (perso, entree ("Images", "outils.aller",
                                       "file:///home/stef/Images",
                                       "folder-pictures-symbolic",
                                       SHELL_OUTILS_LIEU));
    section (barre, SHELL_OUTILS_ZONE_LIEUX, "Personnel", perso);

    GMenu *outils = g_menu_new ();

    /* LE CHEMIN EST UN BOUTON A AUVENT depuis le contrat 2 : la zone « fil »
     * n'existe plus, l'etabli ne porte que des boutons. Les etapes sont le
     * SOUS-MENU de cette entree. */
    GMenu *fil = g_menu_new ();
    g_menu_append_item (fil, entree ("Accueil", "outils.aller",
                                     "file:///home/stef", NULL, NULL));
    g_menu_append_item (fil, entree ("Images", "outils.aller",
                                     "file:///home/stef/Images", NULL, NULL));

    GMenuItem *chemin = g_menu_item_new ("Chemin", NULL);
    g_menu_item_set_attribute (chemin, SHELL_OUTILS_A_FORME, "s", SHELL_OUTILS_AUVENT);
    g_menu_item_set_attribute (chemin, SHELL_OUTILS_A_CONTROLE, "s", SHELL_OUTILS_LISTE);
    g_menu_item_set_attribute (chemin, "icon", "s", "view-list-symbolic");
    g_menu_item_set_link (chemin, G_MENU_LINK_SUBMENU, G_MENU_MODEL (fil));
    g_menu_append_item (outils, chemin);
    g_object_unref (chemin);
    g_object_unref (fil);
    GMenuItem *loupe = entree ("Rechercher", "outils.chercher", NULL,
                               "system-search-symbolic", SHELL_OUTILS_AUVENT);
    g_menu_item_set_attribute (loupe, SHELL_OUTILS_A_CONTROLE, "s",
                               SHELL_OUTILS_SAISIE);
    g_menu_item_set_attribute (loupe, SHELL_OUTILS_A_INVITE, "s",
                               "Nom du fichier…");
    g_menu_item_set_attribute (loupe, SHELL_OUTILS_A_CLE, "s", "Ctrl+F");
    g_menu_append_item (outils, loupe);
    g_object_unref (loupe);
    section (barre, SHELL_OUTILS_ZONE_OUTILS, NULL, outils);

    if (shell_outils_publier (app, "Témoin", G_ACTION_GROUP (actions),
                              G_MENU_MODEL (barre), sur_prise_temoin,
                              NULL) == NULL)
        g_printerr ("le témoin n'a rien pu publier\n");
    else
        g_print ("Témoin publié sous « %s ».\n"
                 "  Lire sa barre :  claude-os-outils %s\n"
                 "  Ctrl-C pour arrêter.\n",
                 g_application_get_application_id (app),
                 g_application_get_application_id (app));

    g_object_unref (perso);
    g_object_unref (outils);
}

static int
temoin (void)
{
    /* Un GApplication tout court, pas un GtkApplication : le témoin n'a pas
     * de fenêtre, et le contrat ne demande rien de graphique. C'est la
     * preuve, au passage, qu'une application non-GTK peut le porter. */
    g_autoptr(GApplication) app = g_application_new ("os.claude.shell.temoin",
                                                     G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "activate", G_CALLBACK (temoin_demarre), NULL);
    g_application_hold (app);
    return g_application_run (app, 0, NULL);
}

/* ------------------------------------------------------------------------- */
int
main (int argc, char **argv)
{
    g_autoptr(GError) err = NULL;

    /* SANS CECI, TOUT ACCENT SORT EN « ? ». Un programme C reste en locale
     * « C » tant qu'il ne demande pas celle de l'environnement ; g_print
     * convertit alors vers l'ASCII et remplace ce qu'il ne sait pas écrire.
     * GTK appelle setlocale pour ses applications, et c'est ce qui masque le
     * probleme partout ailleurs dans ce depot -- ici, il n'y a pas de GTK.
     * Constate le 16 septembre 2026, sur la sortie de ce programme meme. */
    setlocale (LC_ALL, "");

    if (argc != 2) {
        g_printerr ("usage : %s <nom-de-bus> | --dock | --temoin\n", argv[0]);
        return 2;
    }

    if (g_str_equal (argv[1], "--temoin"))
        return temoin ();

    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &err);
    if (bus == NULL) {
        g_printerr ("pas de bus de session : %s\n", err->message);
        return 1;
    }

    boucle = g_main_loop_new (NULL, FALSE);

    if (g_str_equal (argv[1], "--dock")) {
        bus_dock  = bus;
        en_poste  = TRUE;
        g_bus_own_name_on_connection (bus, SHELL_OUTILS_DOCK,
                                      G_BUS_NAME_OWNER_FLAGS_NONE,
                                      nom_pris, nom_perdu, NULL, NULL);
    } else {
        int r = lire (bus, argv[1]);
        if (r >= 0)
            return r;
    }

    g_main_loop_run (boucle);
    return 0;
}
