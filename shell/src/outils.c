/* =========================================================================
 * Claude-OS Shell — le contrat des outils, côté application
 *
 * Le POURQUOI et le protocole complet sont dans outils.h ; ici, la seule
 * mécanique. Trois exports au même chemin, une surveillance de nom, et un
 * aller-retour de présentation.
 * ========================================================================= */

#include "outils.h"

/* L'interface de découverte. Volontairement minuscule : tout ce qui a du
 * contenu passe par org.gtk.Menus et org.gtk.Actions, qui sont spécifiés
 * ailleurs et que GTK sait déjà parler des deux côtés.
 *
 * « Contrat » est une propriété et non une constante compilée dans le dock :
 * les deux processus sont déployés ensemble aujourd'hui, mais rien ne le
 * garantit demain -- une application tierce, un clone, une version en cours
 * d'essai. Le dock DEMANDE, et refuse ce qu'il ne sait pas lire. */
static const char INTROSPECTION[] =
    "<node>"
    "  <interface name='os.claude.shell.Outils'>"
    "    <property name='Contrat' type='u' access='read'/>"
    "    <property name='Titre'   type='s' access='read'/>"
    "    <method name='Prise'>"
    "      <arg name='prise' type='b' direction='in'/>"
    "    </method>"
    "  </interface>"
    "</node>";

struct _ShellOutils {
    GDBusConnection *bus;          /* empruntée à l'application             */
    char            *id;           /* notre nom de bus : l'app_id           */
    char            *titre;

    guint            id_objet;     /* os.claude.shell.Outils                */
    guint            id_actions;   /* org.gtk.Actions                       */
    guint            id_menu;      /* org.gtk.Menus                         */
    guint            veille;       /* surveillance du nom du dock           */

    /* Le propriétaire courant du nom du dock, ou NULL. Sert à REFUSER un
     * Prise qui ne vient pas de lui : sans cette vérification, n'importe
     * quel programme du bus de session pourrait faire disparaître le volet
     * de repli d'une application, et la laisser sans outils du tout. */
    char            *dock;

    gboolean         prise;
    ShellOutilsPriseFunc cb;
    gpointer         data;
};

static GDBusNodeInfo *noeud;   /* analysé une fois pour le processus */

/* -------------------------------------------------------------------------
 * L'interface de découverte
 * ------------------------------------------------------------------------- */
static void
dire_prise (ShellOutils *o, gboolean prise)
{
    if (o->prise == prise)
        return;                    /* jamais un rappel pour rien */
    o->prise = prise;
    if (o->cb != NULL)
        o->cb (prise, o->data);
}

static void
sur_appel (GDBusConnection *bus, const char *appelant, const char *chemin,
           const char *iface, const char *methode, GVariant *params,
           GDBusMethodInvocation *inv, gpointer data)
{
    ShellOutils *o = data;
    (void) bus; (void) chemin; (void) iface;

    if (!g_str_equal (methode, "Prise")) {
        g_dbus_method_invocation_return_error (inv, G_DBUS_ERROR,
                                               G_DBUS_ERROR_UNKNOWN_METHOD,
                                               "méthode inconnue : %s", methode);
        return;
    }

    /* Le dock, et lui seul. Un refus se dit, il ne se tait pas : une
     * application qui ne verrait pas ses outils pris en charge sans savoir
     * pourquoi coûterait une soirée de recherche. */
    if (o->dock == NULL || appelant == NULL || !g_str_equal (appelant, o->dock)) {
        g_message ("outils : « Prise » refusé, il vient de %s et le dock est %s",
                   appelant ? appelant : "(personne)",
                   o->dock ? o->dock : "absent");
        g_dbus_method_invocation_return_error (inv, G_DBUS_ERROR,
                                               G_DBUS_ERROR_ACCESS_DENIED,
                                               "seul le dock prend les outils");
        return;
    }

    gboolean prise;
    g_variant_get (params, "(b)", &prise);
    dire_prise (o, prise);
    g_dbus_method_invocation_return_value (inv, NULL);
}

static GVariant *
sur_propriete (GDBusConnection *bus, const char *appelant, const char *chemin,
               const char *iface, const char *nom, GError **erreur, gpointer data)
{
    ShellOutils *o = data;
    (void) bus; (void) appelant; (void) chemin; (void) iface;

    if (g_str_equal (nom, "Contrat"))
        return g_variant_new_uint32 (SHELL_OUTILS_CONTRAT);
    if (g_str_equal (nom, "Titre"))
        return g_variant_new_string (o->titre ? o->titre : "");

    g_set_error (erreur, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                 "propriété inconnue : %s", nom);
    return NULL;
}

static const GDBusInterfaceVTable VTABLE = { sur_appel, sur_propriete, NULL, { 0 } };

/* -------------------------------------------------------------------------
 * La présentation au dock
 * ------------------------------------------------------------------------- */

/* AVEC un rappel qui lit l'erreur. Un appel asynchrone sans rappel perd ses
 * erreurs sans un mot ; c'est ce qui a coûté une séance au module de veille
 * le 9 septembre 2026, et le dock a retenu la leçon pour la barre d'état. */
static void
sur_reponse (GObject *src, GAsyncResult *res, gpointer data)
{
    g_autofree char *id = data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) r =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);

    if (r == NULL)
        g_message ("outils : « %s » ne s'est pas présenté au dock : %s",
                   id, err->message);
}

static void
se_presenter (ShellOutils *o)
{
    GVariantBuilder args;
    g_variant_builder_init (&args, G_VARIANT_TYPE ("av"));
    g_variant_builder_add (&args, "v", g_variant_new_string (o->id));

    /* Notre nom BIEN CONNU, pas le nom unique : c'est lui que le dock
     * rapproche de l'app_id que le compositeur donne à nos fenêtres. */
    g_dbus_connection_call (o->bus, SHELL_OUTILS_DOCK, "/os/claude/shell/dock",
                            "org.gtk.Actions", "Activate",
                            g_variant_new ("(sava{sv})",
                                           SHELL_OUTILS_PRESENTER, &args, NULL),
                            NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL,
                            sur_reponse, g_strdup (o->id));
}

static void
dock_parait (GDBusConnection *bus, const char *nom, const char *proprietaire,
             gpointer data)
{
    ShellOutils *o = data;
    (void) bus; (void) nom;

    g_free (o->dock);
    o->dock = g_strdup (proprietaire);
    se_presenter (o);
}

static void
dock_disparait (GDBusConnection *bus, const char *nom, gpointer data)
{
    ShellOutils *o = data;
    (void) bus; (void) nom;

    g_clear_pointer (&o->dock, g_free);
    /* Le dock relancé se signalera par « dock_parait », et nous nous
     * représenterons : rien à retenir entre les deux. */
    dire_prise (o, FALSE);
}

/* -------------------------------------------------------------------------
 * Publier, retirer
 * ------------------------------------------------------------------------- */
ShellOutils *
shell_outils_publier (GApplication *app, const char *titre,
                      GActionGroup *actions, GMenuModel *barre,
                      ShellOutilsPriseFunc sur_prise, gpointer data)
{
    g_return_val_if_fail (G_IS_APPLICATION (app), NULL);
    g_return_val_if_fail (G_IS_ACTION_GROUP (actions), NULL);
    g_return_val_if_fail (G_IS_MENU_MODEL (barre), NULL);

    GDBusConnection *bus = g_application_get_dbus_connection (app);
    const char *id = g_application_get_application_id (app);

    if (bus == NULL || id == NULL) {
        g_message ("outils : pas de bus de session, la barre reste dans la "
                   "fenêtre");
        return NULL;
    }

    g_autoptr(GError) err = NULL;
    if (noeud == NULL) {
        noeud = g_dbus_node_info_new_for_xml (INTROSPECTION, &err);
        if (noeud == NULL) {
            g_warning ("outils : introspection illisible : %s", err->message);
            return NULL;
        }
    }

    ShellOutils *o = g_new0 (ShellOutils, 1);
    o->bus   = bus;
    o->id    = g_strdup (id);
    o->titre = g_strdup (titre);
    o->cb    = sur_prise;
    o->data  = data;

    /* Les trois au même chemin : D-Bus place plusieurs interfaces sur un
     * même objet, et GDBus enregistre chacune de son côté. */
    o->id_objet = g_dbus_connection_register_object (
        bus, SHELL_OUTILS_CHEMIN, noeud->interfaces[0], &VTABLE, o, NULL, &err);
    if (o->id_objet == 0)
        g_warning ("outils : « %s » non publié : %s", SHELL_OUTILS_IFACE,
                   err->message);
    g_clear_error (&err);

    o->id_actions = g_dbus_connection_export_action_group (
        bus, SHELL_OUTILS_CHEMIN, actions, &err);
    if (o->id_actions == 0)
        g_warning ("outils : actions non publiées : %s", err->message);
    g_clear_error (&err);

    o->id_menu = g_dbus_connection_export_menu_model (
        bus, SHELL_OUTILS_CHEMIN, barre, &err);
    if (o->id_menu == 0)
        g_warning ("outils : barre non publiée : %s", err->message);
    g_clear_error (&err);

    /* NONE et pas AUTO_START : le dock ne se lance pas parce qu'une
     * application s'ouvre. S'il n'est pas là, c'est que la session est
     * ailleurs -- au banc d'essai, ou en panne -- et l'application doit
     * s'en accommoder, pas le ressusciter. */
    o->veille = g_bus_watch_name_on_connection (
        bus, SHELL_OUTILS_DOCK, G_BUS_NAME_WATCHER_FLAGS_NONE,
        dock_parait, dock_disparait, o, NULL);

    return o;
}

void
shell_outils_retirer (ShellOutils *o)
{
    if (o == NULL)
        return;

    if (o->veille != 0)
        g_bus_unwatch_name (o->veille);
    if (o->id_menu != 0)
        g_dbus_connection_unexport_menu_model (o->bus, o->id_menu);
    if (o->id_actions != 0)
        g_dbus_connection_unexport_action_group (o->bus, o->id_actions);
    if (o->id_objet != 0)
        g_dbus_connection_unregister_object (o->bus, o->id_objet);

    g_free (o->dock);
    g_free (o->titre);
    g_free (o->id);
    g_free (o);
}

gboolean
shell_outils_prise (ShellOutils *o)
{
    return o != NULL && o->prise;
}
