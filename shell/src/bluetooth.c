#include "bluetooth.h"

#include <gio/gio.h>

#include <string.h>          /* strrchr, sscanf */
#include <stdio.h>

#define BZ_BUS     "org.bluez"
#define BZ_ADAPTER "org.bluez.Adapter1"
#define BZ_DEVICE  "org.bluez.Device1"

/* UUID du profil NAP : c'est lui que le telephone annonce quand il sait
 * partager sa connexion. Sans ce profil, se « connecter » a l'appareil
 * n'apporterait aucun reseau. */
#define UUID_NAP "00001116-0000-1000-8000-00805f9b34fb"

#define NM_BUS   "org.freedesktop.NetworkManager"
#define NM_PATH  "/org/freedesktop/NetworkManager"
#define NM_IFACE "org.freedesktop.NetworkManager"

typedef struct {
    GtkWidget *etat;         /* ce qui vient de se passer, a l'ecran        */
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
    char     *adresse;       /* « AA:BB:CC:DD:EE:FF »                       */
    gboolean  appaire;
    gboolean  connecte;
    gboolean  nap;           /* sait partager sa connexion                  */
} Appareil;

static void
appareil_free (gpointer data)
{
    Appareil *a = data;
    g_free (a->chemin);
    g_free (a->nom);
    g_free (a->icone);
    g_free (a->adresse);
    g_free (a);
}

/* Un appel en cours : ce qu'on tentait, et ou le dire. */
typedef struct {
    Bt   *bt;
    char *quoi;
} Tentative;

static void
etat_dire (Bt *bt, const char *texte)
{
    if (bt != NULL && bt->etat != NULL) {
        gtk_label_set_text (GTK_LABEL (bt->etat), texte);
        gtk_widget_set_visible (bt->etat, texte != NULL && *texte != '\0');
    }
}

/* Le message de D-Bus est prefixe du nom d'erreur complet, illisible :
 * « GDBus.Error:org.bluez.Error.AuthenticationCanceled: ... ». On garde ce
 * qui suit le dernier « : ». */
static const char *
message_utile (const GError *e)
{
    const char *deux = strrchr (e->message, ':');
    return (deux != NULL && *(deux + 1) != '\0') ? deux + 2 : e->message;
}

static void
on_appel_fini (GObject *src, GAsyncResult *res, gpointer data)
{
    Tentative *t = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) r =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);

    if (r == NULL) {
        g_autofree char *texte =
            g_strdup_printf ("%s : %s", t->quoi, message_utile (error));
        g_message ("Bluetooth : %s", texte);
        etat_dire (t->bt, texte);
    } else {
        g_autofree char *texte = g_strdup_printf ("%s : c'est fait.", t->quoi);
        etat_dire (t->bt, texte);
    }

    g_free (t->quoi);
    g_free (t);
}

/* Appel generique. `quoi` decrit l'action en francais : c'est ce que
 * l'utilisateur lira, en cas de succes comme d'echec. Un clic qui ne produit
 * ni effet ni explication est le pire des deux mondes -- constate a
 * l'usage. */
static void
appeler_dit (Bt *bt, const char *quoi, const char *nom_bus, const char *chemin,
             const char *iface, const char *methode, GVariant *params, int delai)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus == NULL) {
        etat_dire (bt, "Le bus système n'est pas joignable.");
        if (params != NULL)
            g_variant_unref (g_variant_ref_sink (params));
        return;
    }

    Tentative *t = g_new0 (Tentative, 1);
    t->bt = bt;
    t->quoi = g_strdup (quoi);

    etat_dire (bt, quoi);
    g_dbus_connection_call (bus, nom_bus, chemin, iface, methode, params, NULL,
                            G_DBUS_CALL_FLAGS_NONE, delai, NULL,
                            on_appel_fini, t);
}

static void
appeler (const char *chemin, const char *iface, const char *methode, int delai)
{
    appeler_dit (NULL, "", BZ_BUS, chemin, iface, methode, NULL, delai);
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
        struct { const char *n, *i; gboolean p, c, nap; } faux[] = {
            { "Pixel 8 de Stef", "phone-symbolic",            TRUE,  FALSE, TRUE  },
            { "WH-1000XM4",      "audio-headphones-symbolic", TRUE,  TRUE,  FALSE },
            { "Clavier K380",    "input-keyboard-symbolic",   TRUE,  FALSE, FALSE },
            { "JBL Flip 6",      "audio-speakers-symbolic",   FALSE, FALSE, FALSE },
        };
        for (guint i = 0; i < G_N_ELEMENTS (faux); i++) {
            Appareil *a = g_new0 (Appareil, 1);
            a->chemin = g_strdup ("/apercu");
            a->nom = g_strdup (faux[i].n);
            a->icone = g_strdup (faux[i].i);
            a->adresse = g_strdup ("AA:BB:CC:DD:EE:FF");
            a->appaire = faux[i].p;
            a->connecte = faux[i].c;
            a->nap = faux[i].nap;
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
        a->adresse  = lire_chaine (dev, "Address");
        a->appaire  = lire_bool (dev, "Paired");
        a->connecte = lire_bool (dev, "Connected");

        /* L'appareil annonce-t-il le profil NAP ? C'est lui, et lui seul, qui
         * permet le partage de connexion. */
        g_autoptr(GVariant) uuids =
            g_variant_lookup_value (dev, "UUIDs", G_VARIANT_TYPE_STRING_ARRAY);
        if (uuids != NULL) {
            GVariantIter it2;
            const char *u;
            g_variant_iter_init (&it2, uuids);
            while (g_variant_iter_next (&it2, "&s", &u))
                if (g_ascii_strcasecmp (u, UUID_NAP) == 0) {
                    a->nap = TRUE;
                    break;
                }
        }
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

/* Une adresse « AA:BB:CC:DD:EE:FF » en six octets, comme NetworkManager
 * l'attend pour bluetooth.bdaddr. */
static GVariant *
adresse_en_octets (const char *adresse)
{
    guint v[6];
    if (adresse == NULL
        || sscanf (adresse, "%x:%x:%x:%x:%x:%x",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return NULL;

    guint8 octets[6];
    for (int i = 0; i < 6; i++)
        octets[i] = (guint8) v[i];

    return g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, octets, 6, 1);
}

/* Partage de connexion.
 *
 * Ce n'est PAS org.bluez.Device1.Connect : celui-la active les profils par
 * defaut de l'appareil -- audio, entree -- et n'apporte aucun reseau. Le
 * partage passe par NetworkManager, qui monte la liaison PAN puis y fait
 * tourner DHCP. Notre role se borne a lui decrire le profil voulu.
 *
 * « panu » : nous sommes le client, le telephone est le point d'acces. */
static void
partager_connexion (Bt *bt, Appareil *a)
{
    GVariant *bdaddr = adresse_en_octets (a->adresse);
    if (bdaddr == NULL) {
        etat_dire (bt, "Adresse Bluetooth illisible.");
        return;
    }

    GVariantBuilder profil;
    g_variant_builder_init (&profil, G_VARIANT_TYPE ("a{sa{sv}}"));

    g_autofree char *id = g_strdup_printf ("%s (Bluetooth)", a->nom);
    GVariantBuilder co;
    g_variant_builder_init (&co, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&co, "{sv}", "id",   g_variant_new_string (id));
    g_variant_builder_add (&co, "{sv}", "type", g_variant_new_string ("bluetooth"));
    g_variant_builder_add (&profil, "{sa{sv}}", "connection", &co);

    GVariantBuilder bl;
    g_variant_builder_init (&bl, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&bl, "{sv}", "bdaddr", bdaddr);
    g_variant_builder_add (&bl, "{sv}", "type",   g_variant_new_string ("panu"));
    g_variant_builder_add (&profil, "{sa{sv}}", "bluetooth", &bl);

    /* Peripherique « / » : NetworkManager choisit lui-meme celui qui convient
     * au profil, ce qui evite d'avoir a retrouver son chemin. */
    appeler_dit (bt, "Partage de connexion", NM_BUS, NM_PATH, NM_IFACE,
                 "AddAndActivateConnection",
                 g_variant_new ("(@a{sa{sv}}oo)",
                                g_variant_builder_end (&profil), "/", "/"),
                 45000);
}

static void
on_appareil_clic (GtkButton *b, gpointer data)
{
    LigneBt *l = data;
    Appareil *a = l->a;
    (void) b;

    if (!a->appaire) {
        /* Sans agent enregistre, BlueZ n'aboutit que sur les appareils qui
         * n'exigent aucune confirmation. L'echec est desormais affiche. */
        appeler_dit (l->bt, "Appairage", BZ_BUS, a->chemin, BZ_DEVICE,
                     "Pair", NULL, 30000);
        return;
    }

    if (a->nap) {
        partager_connexion (l->bt, a);
        return;
    }

    if (a->connecte)
        appeler_dit (l->bt, "Déconnexion", BZ_BUS, a->chemin, BZ_DEVICE,
                     "Disconnect", NULL, 10000);
    else
        appeler_dit (l->bt, "Connexion", BZ_BUS, a->chemin, BZ_DEVICE,
                     "Connect", NULL, 20000);
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

    /* Le libelle annonce ce que le clic FERA, pas seulement l'etat : « Appairé »
     * seul ne disait pas qu'il fallait cliquer, ni ce qui arriverait. */
    const char *etat = !a->appaire  ? "Appairer"
                     : a->nap       ? "Partager la connexion"
                     : a->connecte  ? "Déconnecter"
                                    : "Connecter";
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

/* La reconstruction periodique remplace les lignes ; elle ne doit pas
 * effacer le message de la derniere action, qui est justement ce que
 * l'utilisateur est en train de lire. */

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

    /* Ce qui vient de se passer. Cache tant qu'il n'y a rien a dire, mais
     * jamais un clic sans retour : un appairage refuse ou un partage
     * impossible doivent se lire ici, pas dans un journal. */
    bt->etat = gtk_label_new ("");
    gtk_widget_add_css_class (bt->etat, "qs-etat");
    gtk_label_set_wrap (GTK_LABEL (bt->etat), TRUE);
    gtk_label_set_xalign (GTK_LABEL (bt->etat), 0.0);
    gtk_widget_set_visible (bt->etat, FALSE);

    GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class (page, "qs");
    gtk_box_append (GTK_BOX (page), entete);
    gtk_box_append (GTK_BOX (page), bt->pile_interne);
    gtk_box_append (GTK_BOX (page), bt->etat);

    g_object_set_data_full (G_OBJECT (page), "bt", bt, g_free);
    g_object_set_data (G_OBJECT (page), "pile", pile);
    g_object_set_data_full (G_OBJECT (page), "retour", g_strdup (retour), g_free);
    g_signal_connect (fleche, "clicked", G_CALLBACK (on_retour_bt), page);

    return page;
}
