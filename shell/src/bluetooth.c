#include "bluetooth.h"

#include <gio/gio.h>

#define BZ_BUS     "org.bluez"
#define BZ_ADAPTER "org.bluez.Adapter1"
#define BZ_DEVICE  "org.bluez.Device1"

typedef struct {
    GtkWidget *liste;
    GtkWidget *message;
    GtkWidget *pile_interne;
    char      *adapter;      /* chemin de l'adaptateur, ou NULL             */
    gboolean   decouverte;
    gboolean   apercu;
} Bt;

typedef struct {
    char     *chemin;
    char     *nom;
    char     *icone;
    gboolean  appaire;
    gboolean  connecte;
} Appareil;

static void
appareil_free (gpointer data)
{
    Appareil *a = data;
    g_free (a->chemin);
    g_free (a->nom);
    g_free (a->icone);
    g_free (a);
}

static void
on_appel_fini (GObject *src, GAsyncResult *res, gpointer data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) r =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);
    if (r == NULL)
        g_message ("Bluetooth : %s", error->message);
    (void) data;
}

static void
appeler (const char *chemin, const char *iface, const char *methode, int delai)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus == NULL)
        return;
    g_dbus_connection_call (bus, BZ_BUS, chemin, iface, methode, NULL, NULL,
                            G_DBUS_CALL_FLAGS_NONE, delai, NULL,
                            on_appel_fini, NULL);
}

/* -------------------------------------------------------------------------
 * Inventaire par le gestionnaire d'objets
 *
 * BlueZ ne publie ni chemin d'adaptateur fixe ni liste d'appareils : tout
 * passe par GetManagedObjects, qui rend l'arbre complet en un appel.
 * ------------------------------------------------------------------------- */
static GVariant *
objets (void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus == NULL)
        return NULL;

    g_autoptr(GVariant) reponse = g_dbus_connection_call_sync (
        bus, BZ_BUS, "/", "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects", NULL, G_VARIANT_TYPE ("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
    if (reponse == NULL) {
        g_message ("Bluetooth : BlueZ injoignable : %s", error->message);
        return NULL;
    }
    return g_variant_get_child_value (reponse, 0);
}

static gboolean
lire_bool (GVariant *props, const char *nom)
{
    g_autoptr(GVariant) v = g_variant_lookup_value (props, nom, G_VARIANT_TYPE_BOOLEAN);
    return v != NULL && g_variant_get_boolean (v);
}

static char *
lire_chaine (GVariant *props, const char *nom)
{
    g_autoptr(GVariant) v = g_variant_lookup_value (props, nom, G_VARIANT_TYPE_STRING);
    return v != NULL ? g_variant_dup_string (v, NULL) : NULL;
}

static int
comparer_appareils (gconstpointer x, gconstpointer y)
{
    const Appareil *a = *(Appareil * const *) x;
    const Appareil *b = *(Appareil * const *) y;
    if (a->connecte != b->connecte) return a->connecte ? -1 : 1;
    if (a->appaire  != b->appaire)  return a->appaire  ? -1 : 1;
    return g_utf8_collate (a->nom, b->nom);
}

static GPtrArray *
lire_appareils (Bt *bt)
{
    GPtrArray *liste = g_ptr_array_new_with_free_func (appareil_free);

    if (bt->apercu) {
        struct { const char *n, *i; gboolean p, c; } faux[] = {
            { "Pixel 8 de Stef",   "phone-symbolic",      TRUE,  TRUE  },
            { "WH-1000XM4",        "audio-headphones-symbolic", TRUE, FALSE },
            { "Clavier K380",      "input-keyboard-symbolic", TRUE, FALSE },
            { "JBL Flip 6",        "audio-speakers-symbolic", FALSE, FALSE },
        };
        for (guint i = 0; i < G_N_ELEMENTS (faux); i++) {
            Appareil *a = g_new0 (Appareil, 1);
            a->chemin = g_strdup ("/apercu");
            a->nom = g_strdup (faux[i].n);
            a->icone = g_strdup (faux[i].i);
            a->appaire = faux[i].p;
            a->connecte = faux[i].c;
            g_ptr_array_add (liste, a);
        }
        return liste;
    }

    g_autoptr(GVariant) tout = objets ();
    if (tout == NULL)
        return liste;

    GVariantIter it;
    const char *chemin;
    GVariant *ifaces;
    g_variant_iter_init (&it, tout);
    while (g_variant_iter_loop (&it, "{&o@a{sa{sv}}}", &chemin, &ifaces)) {
        g_autoptr(GVariant) adapt = g_variant_lookup_value (ifaces, BZ_ADAPTER, NULL);
        if (adapt != NULL && bt->adapter == NULL)
            bt->adapter = g_strdup (chemin);

        g_autoptr(GVariant) dev = g_variant_lookup_value (ifaces, BZ_DEVICE, NULL);
        if (dev == NULL)
            continue;

        Appareil *a = g_new0 (Appareil, 1);
        a->chemin   = g_strdup (chemin);
        a->nom      = lire_chaine (dev, "Alias");
        if (a->nom == NULL) a->nom = lire_chaine (dev, "Name");
        if (a->nom == NULL) a->nom = lire_chaine (dev, "Address");
        if (a->nom == NULL) a->nom = g_strdup ("(sans nom)");
        a->icone    = lire_chaine (dev, "Icon");
        a->appaire  = lire_bool (dev, "Paired");
        a->connecte = lire_bool (dev, "Connected");
        g_ptr_array_add (liste, a);
    }
    return liste;
}

/* -------------------------------------------------------------------------
 * Lignes
 * ------------------------------------------------------------------------- */
typedef struct { Bt *bt; Appareil *a; } LigneBt;

static void
ligne_bt_free (gpointer data, GClosure *c)
{
    LigneBt *l = data;
    (void) c;
    appareil_free (l->a);
    g_free (l);
}

static void
on_appareil_clic (GtkButton *b, gpointer data)
{
    LigneBt *l = data;
    (void) b;

    if (l->a->connecte)
        appeler (l->a->chemin, BZ_DEVICE, "Disconnect", 10000);
    else if (l->a->appaire)
        appeler (l->a->chemin, BZ_DEVICE, "Connect", 20000);
    else
        /* Sans agent enregistre, BlueZ n'aboutit que sur les appareils qui
         * n'exigent aucune confirmation. L'echec eventuel part dans le
         * journal plutot que de rester muet. */
        appeler (l->a->chemin, BZ_DEVICE, "Pair", 30000);
}

static GtkWidget *
ligne_appareil (Bt *bt, Appareil *a)
{
    LigneBt *l = g_new0 (LigneBt, 1);
    l->bt = bt;
    l->a  = a;

    const char *nom_icone = (a->icone != NULL && *a->icone != '\0')
                          ? a->icone : "bluetooth-symbolic";
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
    if (!gtk_icon_theme_has_icon (theme, nom_icone))
        nom_icone = "bluetooth-symbolic";

    GtkWidget *icone = gtk_image_new_from_icon_name (nom_icone);
    gtk_image_set_pixel_size (GTK_IMAGE (icone), 16);

    GtkWidget *nom = gtk_label_new (a->nom);
    gtk_label_set_xalign (GTK_LABEL (nom), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (nom), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (nom, TRUE);

    GtkWidget *rang = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append (GTK_BOX (rang), icone);
    gtk_box_append (GTK_BOX (rang), nom);

    const char *etat = a->connecte ? "Connecté"
                     : a->appaire  ? "Appairé"
                                   : "Appairer";
    GtkWidget *e = gtk_label_new (etat);
    gtk_widget_add_css_class (e, "reseau-etat");
    gtk_box_append (GTK_BOX (rang), e);

    GtkWidget *bouton = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (bouton), rang);
    gtk_widget_add_css_class (bouton, "reseau-ligne");
    if (a->connecte)
        gtk_widget_add_css_class (bouton, "actif");
    g_signal_connect_data (bouton, "clicked", G_CALLBACK (on_appareil_clic),
                           l, ligne_bt_free, 0);
    return bouton;
}

static void
reconstruire_bt (Bt *bt)
{
    GtkWidget *enfant;
    while ((enfant = gtk_widget_get_first_child (bt->liste)) != NULL)
        gtk_box_remove (GTK_BOX (bt->liste), enfant);

    g_autoptr(GPtrArray) appareils = lire_appareils (bt);
    g_ptr_array_sort (appareils, comparer_appareils);

    guint n = appareils->len;
    g_ptr_array_set_free_func (appareils, NULL);
    for (guint i = 0; i < n; i++)
        gtk_box_append (GTK_BOX (bt->liste),
                        ligne_appareil (bt, g_ptr_array_index (appareils, i)));

    gtk_label_set_text (GTK_LABEL (bt->message),
                        bt->adapter == NULL && !bt->apercu
                        ? "Aucun adaptateur Bluetooth détecté."
                        : "Aucun appareil pour l'instant. La recherche est en cours.");
    gtk_stack_set_visible_child_name (GTK_STACK (bt->pile_interne),
                                      n > 0 ? "liste" : "vide");
}

/* -------------------------------------------------------------------------
 * Page
 * ------------------------------------------------------------------------- */
static void
on_retour_bt (GtkButton *b, gpointer data)
{
    (void) b;
    GtkStack *pile = g_object_get_data (data, "pile");
    gtk_stack_set_visible_child_name (pile, g_object_get_data (data, "retour"));
}

/* La liste ne se rafraichit que pendant que la page est ouverte, et une fois
 * toutes les deux secondes : la decouverte fait apparaitre des appareils au
 * fil de l'eau, mais reconstruire plus souvent ferait sauter la liste sous
 * le curseur. */
static gboolean
on_battement (gpointer data)
{
    reconstruire_bt (data);
    return G_SOURCE_CONTINUE;
}

void
bluetooth_page_ouverte (GtkWidget *page)
{
    Bt *bt = g_object_get_data (G_OBJECT (page), "bt");
    if (bt == NULL)
        return;

    reconstruire_bt (bt);       /* remplit adapter au passage */

    if (bt->adapter != NULL && !bt->decouverte) {
        appeler (bt->adapter, BZ_ADAPTER, "StartDiscovery", 5000);
        bt->decouverte = TRUE;
    }
    if (g_object_get_data (G_OBJECT (page), "battement") == NULL)
        g_object_set_data (G_OBJECT (page), "battement",
                           GUINT_TO_POINTER (g_timeout_add (2000, on_battement, bt)));
}

void
bluetooth_page_fermee (GtkWidget *page)
{
    Bt *bt = g_object_get_data (G_OBJECT (page), "bt");
    if (bt == NULL)
        return;

    /* La decouverte est le poste de consommation le plus lourd de cette
     * page : la radio balaie en continu. Elle s'arrete des qu'on quitte. */
    if (bt->adapter != NULL && bt->decouverte) {
        appeler (bt->adapter, BZ_ADAPTER, "StopDiscovery", 5000);
        bt->decouverte = FALSE;
    }
    guint id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (page), "battement"));
    if (id != 0) {
        g_source_remove (id);
        g_object_set_data (G_OBJECT (page), "battement", NULL);
    }
}

GtkWidget *
bluetooth_page_new (GtkStack *pile, const char *retour, gboolean apercu)
{
    Bt *bt = g_new0 (Bt, 1);
    bt->apercu = apercu;

    GtkWidget *fleche = gtk_button_new_from_icon_name ("go-previous-symbolic");
    gtk_widget_add_css_class (fleche, "qs-retour");

    GtkWidget *titre = gtk_label_new ("Bluetooth");
    gtk_widget_add_css_class (titre, "qs-titre-page");
    gtk_widget_set_hexpand (titre, TRUE);
    gtk_label_set_xalign (GTK_LABEL (titre), 0.0);

    GtkWidget *entete = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class (entete, "qs-entete");
    gtk_box_append (GTK_BOX (entete), fleche);
    gtk_box_append (GTK_BOX (entete), titre);

    bt->liste = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

    GtkWidget *defil = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (defil),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (defil), bt->liste);
    gtk_widget_set_size_request (defil, -1, 240);

    bt->message = gtk_label_new ("");
    gtk_widget_add_css_class (bt->message, "qs-vide");
    gtk_label_set_wrap (GTK_LABEL (bt->message), TRUE);
    gtk_widget_set_valign (bt->message, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request (bt->message, -1, 240);

    bt->pile_interne = gtk_stack_new ();
    gtk_stack_add_named (GTK_STACK (bt->pile_interne), defil, "liste");
    gtk_stack_add_named (GTK_STACK (bt->pile_interne), bt->message, "vide");

    GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class (page, "qs");
    gtk_box_append (GTK_BOX (page), entete);
    gtk_box_append (GTK_BOX (page), bt->pile_interne);

    g_object_set_data_full (G_OBJECT (page), "bt", bt, g_free);
    g_object_set_data (G_OBJECT (page), "pile", pile);
    g_object_set_data_full (G_OBJECT (page), "retour", g_strdup (retour), g_free);
    g_signal_connect (fleche, "clicked", G_CALLBACK (on_retour_bt), page);

    return page;
}
