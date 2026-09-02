#include "wifi.h"

#include <gio/gio.h>

#include <string.h>          /* strlen */

#define NM_BUS      "org.freedesktop.NetworkManager"
#define NM_PATH     "/org/freedesktop/NetworkManager"
#define NM_IFACE    "org.freedesktop.NetworkManager"
#define NM_DEV      "org.freedesktop.NetworkManager.Device"
#define NM_WIFI     "org.freedesktop.NetworkManager.Device.Wireless"
#define NM_AP       "org.freedesktop.NetworkManager.AccessPoint"

#define NM_DEVICE_TYPE_WIFI      2
#define NM_802_11_AP_FLAGS_PRIVACY 0x1

typedef struct {
    GtkWidget   *pile_interne;   /* liste / message d'indisponibilite       */
    GtkWidget   *liste;
    GtkWidget   *message;
    char        *device;         /* chemin de la carte Wi-Fi                */
    char        *ap_actif;       /* borne actuellement connectee            */
    GPtrArray   *reseaux;        /* Reseau *, en cours de collecte          */
    GHashTable  *par_ssid;       /* deduplication pendant la collecte       */
    guint        restants;       /* peripheriques dont on attend la reponse */
    guint        attendues;      /* bornes dont on attend la reponse        */
    gboolean     apercu;
} Wifi;

/* -------------------------------------------------------------------------
 * Petits acces D-Bus
 * ------------------------------------------------------------------------- */
/* Le SSID est un tableau d'octets, pas une chaine : il peut contenir
 * n'importe quoi, y compris des octets invalides en UTF-8. On le convertit
 * prudemment plutot que de le passer tel quel a GTK, qui refuserait. */
static char *
ssid_lisible (GVariant *ay)
{
    if (ay == NULL)
        return NULL;

    gsize n = 0;
    const guchar *octets = g_variant_get_fixed_array (ay, &n, 1);
    if (n == 0)
        return g_strdup ("(réseau masqué)");

    g_autofree char *brut = g_strndup ((const char *) octets, n);
    if (g_utf8_validate (brut, -1, NULL))
        return g_steal_pointer (&brut);

    return g_strdup_printf ("(nom illisible, %zu octets)", n);
}

/* -------------------------------------------------------------------------
 * Un reseau
 * ------------------------------------------------------------------------- */
typedef struct {
    char     *ssid;
    char     *ap;         /* chemin D-Bus du point d'acces                  */
    guint8    force;      /* 0-100                                          */
    gboolean  protege;
    gboolean  actif;
} Reseau;

static void
reseau_free (gpointer data)
{
    Reseau *r = data;
    g_free (r->ssid);
    g_free (r->ap);
    g_free (r);
}

static int
comparer_force (gconstpointer a, gconstpointer b)
{
    const Reseau *x = *(Reseau * const *) a;
    const Reseau *y = *(Reseau * const *) b;
    if (x->actif != y->actif)
        return x->actif ? -1 : 1;      /* le reseau connecte en tete        */
    return (int) y->force - (int) x->force;
}

static const char *
icone_force (guint8 f)
{
    return f >= 75 ? "network-wireless-signal-excellent-symbolic"
         : f >= 50 ? "network-wireless-signal-good-symbolic"
         : f >= 25 ? "network-wireless-signal-ok-symbolic"
                   : "network-wireless-signal-weak-symbolic";
}

/* -------------------------------------------------------------------------
 * Connexion
 * ------------------------------------------------------------------------- */
static void
on_appel_fini (GObject *src, GAsyncResult *res, gpointer data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) r =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);
    if (r == NULL)
        g_message ("Wi-Fi : %s", error->message);
    (void) data;
}

/* Reseau deja connu : « / » comme connexion demande a NetworkManager de
 * choisir lui-meme le profil enregistre qui convient au couple
 * peripherique + point d'acces. C'est ce que fait nmcli, et cela evite
 * d'enumerer tous les profils pour retrouver le bon. */
static void
connecter_connu (Wifi *w, const char *ap)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus == NULL)
        return;

    g_dbus_connection_call (bus, NM_BUS, NM_PATH, NM_IFACE, "ActivateConnection",
                            g_variant_new ("(ooo)", "/", w->device, ap),
                            NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
                            on_appel_fini, NULL);
}

/* Reseau inconnu : on fabrique un profil minimal. NetworkManager complete
 * tout le reste -- uuid, adressage, autoconnexion. */
static void
connecter_nouveau (Wifi *w, const char *ap, const char *ssid, const char *mdp)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus == NULL)
        return;

    GVariantBuilder profil;
    g_variant_builder_init (&profil, G_VARIANT_TYPE ("a{sa{sv}}"));

    GVariantBuilder co;
    g_variant_builder_init (&co, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&co, "{sv}", "id",   g_variant_new_string (ssid));
    g_variant_builder_add (&co, "{sv}", "type", g_variant_new_string ("802-11-wireless"));
    g_variant_builder_add (&profil, "{sa{sv}}", "connection", &co);

    GVariantBuilder sans_fil;
    g_variant_builder_init (&sans_fil, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&sans_fil, "{sv}", "ssid",
        g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, ssid, strlen (ssid), 1));
    g_variant_builder_add (&profil, "{sa{sv}}", "802-11-wireless", &sans_fil);

    if (mdp != NULL && *mdp != '\0') {
        GVariantBuilder sec;
        g_variant_builder_init (&sec, G_VARIANT_TYPE ("a{sv}"));
        g_variant_builder_add (&sec, "{sv}", "key-mgmt", g_variant_new_string ("wpa-psk"));
        g_variant_builder_add (&sec, "{sv}", "psk",      g_variant_new_string (mdp));
        g_variant_builder_add (&profil, "{sa{sv}}", "802-11-wireless-security", &sec);
    }

    g_dbus_connection_call (bus, NM_BUS, NM_PATH, NM_IFACE,
                            "AddAndActivateConnection",
                            g_variant_new ("(@a{sa{sv}}oo)",
                                           g_variant_builder_end (&profil),
                                           w->device, ap),
                            NULL, G_DBUS_CALL_FLAGS_NONE, 20000, NULL,
                            on_appel_fini, NULL);
}

static void
deconnecter (Wifi *w)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus == NULL)
        return;

    g_dbus_connection_call (bus, NM_BUS, w->device, NM_DEV, "Disconnect", NULL,
                            NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                            on_appel_fini, NULL);
}

/* -------------------------------------------------------------------------
 * Construction de la liste
 * ------------------------------------------------------------------------- */
typedef struct {
    Wifi   *w;
    Reseau *r;
    GtkWidget *revelateur;   /* champ de mot de passe, cache par defaut     */
    GtkWidget *champ;
} Ligne;

/* La ligne devient proprietaire du Reseau : sans cela il faudrait le garder
 * dans un tableau parallele, qui se desynchroniserait a la premiere
 * reconstruction. */
static void
ligne_free (gpointer data, GClosure *closure)
{
    Ligne *l = data;
    (void) closure;
    reseau_free (l->r);
    g_free (l);
}

static void
on_valider_mdp (GtkWidget *source, gpointer data)
{
    Ligne *l = data;
    (void) source;
    connecter_nouveau (l->w, l->r->ap, l->r->ssid,
                       gtk_editable_get_text (GTK_EDITABLE (l->champ)));
    gtk_revealer_set_reveal_child (GTK_REVEALER (l->revelateur), FALSE);
}

static void
on_reseau_clic (GtkButton *b, gpointer data)
{
    Ligne *l = data;
    (void) b;

    if (l->r->actif) {
        deconnecter (l->w);
        return;
    }

    /* Reseau protege et inconnu : on demande le mot de passe. On ne sait pas
     * encore s'il est connu -- ActivateConnection le dira. On tente donc le
     * profil enregistre d'abord, et le champ ne s'ouvre que si l'appel
     * echoue... sauf qu'un appel asynchrone ne peut pas repondre a temps.
     *
     * Choix retenu : tenter le profil enregistre, ET reveler le champ pour
     * un reseau protege. Si la connexion aboutit, la liste se reconstruit et
     * le champ disparait ; sinon il est deja la, pret. Cela evite un
     * aller-retour ou l'utilisateur clique deux fois sans comprendre. */
    connecter_connu (l->w, l->r->ap);

    if (l->r->protege)
        gtk_revealer_set_reveal_child (GTK_REVEALER (l->revelateur), TRUE);
}

/* Prend possession de `r`. */
static GtkWidget *
ligne_reseau (Wifi *w, Reseau *r)
{
    Ligne *l = g_new0 (Ligne, 1);
    l->w = w;
    l->r = r;

    GtkWidget *icone = gtk_image_new_from_icon_name (icone_force (r->force));
    gtk_image_set_pixel_size (GTK_IMAGE (icone), 16);

    GtkWidget *nom = gtk_label_new (r->ssid);
    gtk_label_set_xalign (GTK_LABEL (nom), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (nom), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (nom, TRUE);

    GtkWidget *rang = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append (GTK_BOX (rang), icone);
    gtk_box_append (GTK_BOX (rang), nom);

    if (r->actif) {
        GtkWidget *etat = gtk_label_new ("Connecté");
        gtk_widget_add_css_class (etat, "reseau-etat");
        gtk_box_append (GTK_BOX (rang), etat);
    }
    if (r->protege) {
        GtkWidget *cadenas = gtk_image_new_from_icon_name ("channel-secure-symbolic");
        gtk_image_set_pixel_size (GTK_IMAGE (cadenas), 14);
        gtk_widget_add_css_class (cadenas, "reseau-cadenas");
        gtk_box_append (GTK_BOX (rang), cadenas);
    }

    GtkWidget *bouton = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (bouton), rang);
    gtk_widget_add_css_class (bouton, "reseau-ligne");
    if (r->actif)
        gtk_widget_add_css_class (bouton, "actif");
    g_signal_connect_data (bouton, "clicked", G_CALLBACK (on_reseau_clic),
                           l, ligne_free, 0);

    /* Champ de mot de passe, replie sous la ligne. */
    l->champ = gtk_password_entry_new ();
    gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (l->champ), TRUE);
    gtk_widget_add_css_class (l->champ, "reseau-mdp");
    g_signal_connect (l->champ, "activate", G_CALLBACK (on_valider_mdp), l);

    GtkWidget *valider = gtk_button_new_with_label ("Se connecter");
    gtk_widget_add_css_class (valider, "reseau-valider");
    g_signal_connect (valider, "clicked", G_CALLBACK (on_valider_mdp), l);

    GtkWidget *saisie = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_hexpand (l->champ, TRUE);
    gtk_box_append (GTK_BOX (saisie), l->champ);
    gtk_box_append (GTK_BOX (saisie), valider);

    l->revelateur = gtk_revealer_new ();
    gtk_revealer_set_child (GTK_REVEALER (l->revelateur), saisie);
    gtk_revealer_set_transition_duration (GTK_REVEALER (l->revelateur), 120);

    GtkWidget *bloc = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (bloc), bouton);
    gtk_box_append (GTK_BOX (bloc), l->revelateur);
    return bloc;
}

/* -------------------------------------------------------------------------
 * Lecture, entierement ASYNCHRONE
 *
 * Aucun appel bloquant sur la boucle principale, jamais. Un appel synchrone
 * y gele l'interface entiere le temps du delai D-Bus, et ce delai n'est pas
 * theorique : mesure au banc d'essai contre un service muet, la barre
 * d'etat restait invisible vingt-cinq secondes.
 *
 * Deroulement : les proprietes du service donnent la liste des
 * peripheriques, celles de chaque peripherique disent lequel est la carte
 * Wi-Fi, celles de la carte donnent les bornes, et chaque borne est
 * interrogee a son tour. A chaque etage on compte les reponses en attente :
 * l'etage suivant ne part qu'une fois la derniere arrivee, sinon la liste
 * sauterait sous le curseur.
 * ------------------------------------------------------------------------- */
static void construire_liste (Wifi *w);
static void lire_bornes (Wifi *w);

/* Contexte d'un appel : le chemin doit voyager avec la reponse, sans quoi
 * elle ne dirait pas de quel objet elle parle. */
typedef struct {
    Wifi *w;
    char *chemin;
} Appel;

static void
appel_free (Appel *a)
{
    g_free (a->chemin);
    g_free (a);
}

static void
appel_props (Wifi *w, const char *chemin, const char *iface,
             GAsyncReadyCallback fini)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);

    Appel *a = g_new0 (Appel, 1);
    a->w = w;
    a->chemin = g_strdup (chemin);

    if (bus == NULL) {
        g_message ("Wi-Fi : bus systeme injoignable : %s", error->message);
        fini (NULL, NULL, a);
        return;
    }
    g_dbus_connection_call (bus, NM_BUS, chemin,
                            "org.freedesktop.DBus.Properties", "GetAll",
                            g_variant_new ("(s)", iface),
                            G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
                            4000, NULL, fini, a);
}

/* Recupere le lot de proprietes. NULL si l'appel a echoue. */
static GVariant *
props_finies (GObject *src, GAsyncResult *res, const char *quoi)
{
    if (src == NULL || res == NULL)
        return NULL;

    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reponse =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);
    if (reponse == NULL) {
        g_message ("Wi-Fi : %s illisible : %s", quoi, error->message);
        return NULL;
    }
    return g_variant_get_child_value (reponse, 0);
}

static GVariant *
val (GVariant *lot, const char *nom)
{
    return lot != NULL ? g_variant_lookup_value (lot, nom, NULL) : NULL;
}

/* --- etage 4 : une borne --- */
static void
une_borne_lue (GObject *src, GAsyncResult *res, gpointer data)
{
    Appel *a = data;
    Wifi *w = a->w;
    g_autoptr(GVariant) lot = props_finies (src, res, "un point d'acces");

    g_autoptr(GVariant) v_ssid  = val (lot, "Ssid");
    g_autoptr(GVariant) v_force = val (lot, "Strength");
    g_autoptr(GVariant) v_flags = val (lot, "Flags");
    g_autoptr(GVariant) v_rsn   = val (lot, "RsnFlags");
    g_autoptr(GVariant) v_wpa   = val (lot, "WpaFlags");

    g_autofree char *ssid = ssid_lisible (v_ssid);
    if (ssid != NULL) {
        guint8 force = v_force ? g_variant_get_byte (v_force) : 0;

        /* Un meme reseau est souvent porte par plusieurs bornes : on ne
         * garde que la plus forte, sinon la liste se remplit de doublons. */
        Reseau *deja = g_hash_table_lookup (w->par_ssid, ssid);
        if (deja != NULL) {
            if (force > deja->force) {
                deja->force = force;
                g_free (deja->ap);
                deja->ap = g_strdup (a->chemin);
                deja->actif = (g_strcmp0 (a->chemin, w->ap_actif) == 0);
            }
        } else {
            Reseau *r = g_new0 (Reseau, 1);
            r->ssid    = g_steal_pointer (&ssid);
            r->ap      = g_strdup (a->chemin);
            r->force   = force;
            r->protege = (v_flags && (g_variant_get_uint32 (v_flags) & NM_802_11_AP_FLAGS_PRIVACY))
                      || (v_rsn   && g_variant_get_uint32 (v_rsn) != 0)
                      || (v_wpa   && g_variant_get_uint32 (v_wpa) != 0);
            r->actif   = (g_strcmp0 (r->ap, w->ap_actif) == 0);
            g_hash_table_insert (w->par_ssid, r->ssid, r);
            g_ptr_array_add (w->reseaux, r);
        }
    }

    appel_free (a);
    if (--w->attendues == 0)
        construire_liste (w);
}

/* --- etage 3 : la carte --- */
static void
carte_lue (GObject *src, GAsyncResult *res, gpointer data)
{
    Appel *a = data;
    Wifi *w = a->w;
    g_autoptr(GVariant) lot = props_finies (src, res, "la carte Wi-Fi");
    appel_free (a);

    g_autoptr(GVariant) liste = val (lot, "AccessPoints");
    g_autoptr(GVariant) actif = val (lot, "ActiveAccessPoint");

    g_free (w->ap_actif);
    w->ap_actif = actif ? g_variant_dup_string (actif, NULL) : g_strdup ("/");

    g_ptr_array_set_size (w->reseaux, 0);
    g_hash_table_remove_all (w->par_ssid);
    w->attendues = 0;

    if (liste != NULL) {
        GVariantIter it;
        const char *ap;
        g_variant_iter_init (&it, liste);
        while (g_variant_iter_next (&it, "&o", &ap))
            w->attendues++;

        g_variant_iter_init (&it, liste);
        while (g_variant_iter_next (&it, "&o", &ap))
            appel_props (w, ap, NM_AP, une_borne_lue);
    }

    if (w->attendues == 0)
        construire_liste (w);
}

static void
lire_bornes (Wifi *w)
{
    if (w->device != NULL)
        appel_props (w, w->device, NM_WIFI, carte_lue);
    else
        construire_liste (w);
}

/* --- etage 2 : un peripherique --- */
static void
un_peripherique_lu (GObject *src, GAsyncResult *res, gpointer data)
{
    Appel *a = data;
    Wifi *w = a->w;
    g_autoptr(GVariant) lot = props_finies (src, res, "un peripherique");
    g_autoptr(GVariant) type = val (lot, "DeviceType");

    if (w->device == NULL && type != NULL
        && g_variant_get_uint32 (type) == NM_DEVICE_TYPE_WIFI)
        w->device = g_strdup (a->chemin);

    appel_free (a);
    if (--w->restants == 0)
        lire_bornes (w);
}

/* --- etage 1 : le service --- */
static void
peripheriques_lus (GObject *src, GAsyncResult *res, gpointer data)
{
    Appel *a = data;
    Wifi *w = a->w;
    g_autoptr(GVariant) lot = props_finies (src, res, "la liste des peripheriques");
    appel_free (a);

    g_autoptr(GVariant) devices = val (lot, "Devices");
    if (devices == NULL) {
        construire_liste (w);
        return;
    }

    GVariantIter it;
    const char *d;
    w->restants = 0;
    g_variant_iter_init (&it, devices);
    while (g_variant_iter_next (&it, "&o", &d))
        w->restants++;

    if (w->restants == 0) {
        construire_liste (w);
        return;
    }

    g_variant_iter_init (&it, devices);
    while (g_variant_iter_next (&it, "&o", &d))
        appel_props (w, d, NM_DEV, un_peripherique_lu);
}

/* Rendu final, une fois toutes les reponses arrivees. */
static void
construire_liste (Wifi *w)
{
    GtkWidget *enfant;
    while ((enfant = gtk_widget_get_first_child (w->liste)) != NULL)
        gtk_box_remove (GTK_BOX (w->liste), enfant);

    if (w->apercu) {
        struct { const char *s; guint8 f; gboolean p, a; } faux[] = {
            { "Livebox-4A2F",   92, TRUE,  TRUE  },
            { "Freebox-Invite", 71, FALSE, FALSE },
            { "iPhone de Stef", 64, TRUE,  FALSE },
            { "FAI-Public",     38, FALSE, FALSE },
            { "NEUF_8821",      21, TRUE,  FALSE },
        };
        for (guint i = 0; i < G_N_ELEMENTS (faux); i++) {
            Reseau *r = g_new0 (Reseau, 1);
            r->ssid = g_strdup (faux[i].s);
            r->ap = g_strdup ("/apercu");
            r->force = faux[i].f;
            r->protege = faux[i].p;
            r->actif = faux[i].a;
            gtk_box_append (GTK_BOX (w->liste), ligne_reseau (w, r));
        }
        gtk_stack_set_visible_child_name (GTK_STACK (w->pile_interne), "liste");
        return;
    }

    g_ptr_array_sort (w->reseaux, comparer_force);

    /* Les lignes prennent possession des Reseau : le tableau ne doit donc
     * pas les liberer. Il est vide sans free_func a la lecture suivante. */
    for (guint i = 0; i < w->reseaux->len; i++)
        gtk_box_append (GTK_BOX (w->liste),
                        ligne_reseau (w, g_ptr_array_index (w->reseaux, i)));
    guint n = w->reseaux->len;
    g_ptr_array_set_size (w->reseaux, 0);
    g_hash_table_remove_all (w->par_ssid);

    gtk_label_set_text (GTK_LABEL (w->message),
                        w->device == NULL
                        ? "Aucune carte Wi-Fi détectée, ou NetworkManager ne répond pas."
                        : "Aucun réseau visible pour l'instant.");
    gtk_stack_set_visible_child_name (GTK_STACK (w->pile_interne),
                                      n > 0 ? "liste" : "vide");
}

/* -------------------------------------------------------------------------
 * Page
 * ------------------------------------------------------------------------- */
static void
on_retour (GtkButton *b, gpointer data)
{
    (void) b;
    GtkStack *pile = g_object_get_data (data, "pile");
    gtk_stack_set_visible_child_name (pile, g_object_get_data (data, "retour"));
}

void
wifi_page_ouverte (GtkWidget *page)
{
    Wifi *w = g_object_get_data (G_OBJECT (page), "wifi");
    if (w == NULL)
        return;

    if (w->apercu) {
        construire_liste (w);
        return;
    }

    /* Un balayage a l'ouverture, et un seul. NetworkManager en refait de
     * lui-meme tant qu'une application ecoute ; en redemander a intervalle
     * fixe ne ferait que reveiller la radio pour rien. */
    if (w->device != NULL) {
        g_autoptr(GError) error = NULL;
        g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
        if (bus != NULL)
            g_dbus_connection_call (bus, NM_BUS, w->device, NM_WIFI, "RequestScan",
                                    g_variant_new ("(a{sv})", NULL),
                                    NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                                    on_appel_fini, NULL);
        lire_bornes (w);
        return;
    }

    /* Premiere ouverture : on part de la liste des peripheriques. */
    appel_props (w, NM_PATH, NM_IFACE, peripheriques_lus);
}

GtkWidget *
wifi_page_new (GtkStack *pile, const char *retour, gboolean apercu)
{
    Wifi *w = g_new0 (Wifi, 1);
    w->apercu   = apercu;
    w->reseaux  = g_ptr_array_new ();
    w->par_ssid = g_hash_table_new (g_str_hash, g_str_equal);

    /* AUCUN appel D-Bus ici. La page est construite au demarrage de la barre
     * d'etat, bien avant qu'on la regarde : un appel synchrone vers un
     * service lent ou bloque empecherait la barre d'apparaitre du tout.
     * Constate au banc d'essai contre un faux NetworkManager muet -- le
     * processus tournait, l'ecran restait vide.
     *
     * Tout se fait a la premiere ouverture de la page. */

    /* --- en-tete --- */
    GtkWidget *fleche = gtk_button_new_from_icon_name ("go-previous-symbolic");
    gtk_widget_add_css_class (fleche, "qs-retour");

    GtkWidget *titre = gtk_label_new ("Wi-Fi");
    gtk_widget_add_css_class (titre, "qs-titre-page");
    gtk_widget_set_hexpand (titre, TRUE);
    gtk_label_set_xalign (GTK_LABEL (titre), 0.0);

    GtkWidget *entete = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class (entete, "qs-entete");
    gtk_box_append (GTK_BOX (entete), fleche);
    gtk_box_append (GTK_BOX (entete), titre);

    /* --- liste --- */
    w->liste = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

    GtkWidget *defil = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (defil),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (defil), w->liste);
    /* Hauteur bornee : la liste ne doit pas pousser le panneau hors de
     * l'ecran quand une vingtaine de reseaux sont visibles. */
    gtk_widget_set_size_request (defil, -1, 240);

    w->message = gtk_label_new ("");
    gtk_widget_add_css_class (w->message, "qs-vide");
    gtk_label_set_wrap (GTK_LABEL (w->message), TRUE);
    gtk_widget_set_valign (w->message, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request (w->message, -1, 240);

    w->pile_interne = gtk_stack_new ();
    gtk_stack_add_named (GTK_STACK (w->pile_interne), defil, "liste");
    gtk_stack_add_named (GTK_STACK (w->pile_interne), w->message, "vide");

    GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class (page, "qs");
    gtk_box_append (GTK_BOX (page), entete);
    gtk_box_append (GTK_BOX (page), w->pile_interne);

    g_object_set_data_full (G_OBJECT (page), "wifi", w, g_free);
    g_object_set_data (G_OBJECT (page), "pile", pile);
    g_object_set_data_full (G_OBJECT (page), "retour", g_strdup (retour), g_free);
    g_signal_connect (fleche, "clicked", G_CALLBACK (on_retour), page);

    return page;
}
