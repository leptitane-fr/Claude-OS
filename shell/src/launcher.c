/* =========================================================================
 * Claude-OS Shell — lanceur d'applications
 *
 * Une fenetre centrale qui liste ce qui est installe. Trois presentations
 * -- icones, liste, details -- une recherche, un tri.
 *
 * UN VOILE PLEIN ECRAN, SAUF LA BANDE DU DOCK
 *
 * Le lanceur de ChromeOS occupe tout l'ecran et se ferme au clic a cote.
 * C'est ce qu'on veut, et c'est en tension avec le glisser-deposer vers le
 * dock : un voile qui prend tout l'ecran capte aussi les clics destines au
 * dock, et la cible du depot devient inatteignable.
 *
 * La surface s'ancre donc aux QUATRE bords, avec une marge basse de la
 * hauteur du dock. Elle couvre tout le reste de l'ecran -- un clic n'importe
 * ou ferme le lanceur -- et laisse la bande du bas au dock, qui reste
 * cliquable et reste une cible de depot.
 *
 * Le voile est entierement transparent : rien ne change a l'ecran, il
 * n'existe que pour recevoir les clics.
 *
 * DISCIPLINE D'ENERGIE
 *
 * Aucune minuterie, aucune surveillance. Le lanceur ne fait rien tant qu'on
 * ne l'ouvre pas. La liste des applications est relue a chaque ouverture --
 * une centaine de fichiers .desktop, quelques millisecondes -- plutot que
 * surveillee en permanence : installer une application est rare, et un
 * moniteur de repertoire couterait plus cher, tout le temps, que cette
 * relecture ponctuelle.
 *
 * RESIDENCE
 *
 * Le processus reste vivant une fois lance, fenetre masquee. Il n'est
 * demarre par personne au demarrage de la session : le premier appui sur le
 * bouton du dock le lance, les suivants sont instantanes. Rien n'est paye
 * tant que le lanceur n'a pas servi.
 * ========================================================================= */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gio/gdesktopappinfo.h>

#include <string.h>            /* strlen */

#include "config.h"

#define ICONE_GRILLE   48
#define ICONE_LISTE    24
#define ICONE_DETAILS  32

/* Dimensions du panneau lui-meme, centre dans le voile. Elles tiennent sur
 * l'ecran 1920x1080 de la machine. */
#define LARGEUR        880
#define HAUTEUR        600

/* Hauteur laissee libre en bas, pour le dock et la barre d'etat.
 *
 * Le dock mesure 88 px : 52 pour l'icone, 8 de remplissage en haut et en
 * bas, le point d'etat, la bordure, et 12 px de marge sous la pilule. On
 * arrondit a 100 -- une bande un peu plus large ne coute rien, alors qu'une
 * bande trop courte rendrait le haut du dock insensible au clic. */
#define BANDE_DOCK     100

typedef enum { VUE_ICONES, VUE_LISTE, VUE_DETAILS } Vue;
typedef enum { TRI_NOM, TRI_NOM_INVERSE, TRI_CATEGORIE } Tri;

typedef struct {
    GDesktopAppInfo *info;
    char            *id;          /* « chromium » : ce que le dock epingle  */
    char            *nom;
    char            *description;
    const char      *categorie;   /* libelle francais, jamais NULL          */
    char            *botte;       /* nom + description + mots-cles + id     */
} App;

static struct {
    GtkWidget  *fenetre;
    GtkWidget  *cadre;            /* le panneau, au centre du voile         */
    GtkWidget  *recherche;
    GtkWidget  *defilement;
    GtkWidget  *menu;             /* GtkPopoverMenu du clic droit           */
    GtkWidget  *vide;             /* message « aucun resultat »             */
    GPtrArray  *apps;
    Vue         vue;
    Tri         tri;
} L;

/* -------------------------------------------------------------------------
 * Categories
 *
 * Les categories freedesktop sont des mots anglais destines aux menus. On
 * n'en garde que la premiere reconnue : une application en declare souvent
 * quatre, et les afficher toutes ne renseigne pas mieux qu'une seule.
 * ------------------------------------------------------------------------- */
static const struct { const char *xdg; const char *fr; } CATEGORIES[] = {
    { "AudioVideo",  "Son et vidéo" },
    { "Audio",       "Son et vidéo" },
    { "Video",       "Son et vidéo" },
    { "Development", "Développement" },
    { "Education",   "Éducation" },
    { "Game",        "Jeux" },
    { "Graphics",    "Graphisme" },
    { "Network",     "Internet" },
    { "Office",      "Bureautique" },
    { "Science",     "Sciences" },
    { "Settings",    "Réglages" },
    { "System",      "Système" },
    { "Utility",     "Accessoires" },
    { NULL, NULL }
};

#define CATEGORIE_AUTRE "Autres"

static const char *
categorie_de (GDesktopAppInfo *info)
{
    const char *brut = g_desktop_app_info_get_categories (info);
    if (brut == NULL)
        return CATEGORIE_AUTRE;

    g_auto(GStrv) parts = g_strsplit (brut, ";", -1);

    /* On parcourt la TABLE, pas la liste declaree : cela impose notre ordre
     * de preference. « Network;Application » doit donner Internet, quel que
     * soit l'ordre dans lequel le fichier .desktop les a ecrites. */
    for (guint i = 0; CATEGORIES[i].xdg != NULL; i++)
        for (guint j = 0; parts[j] != NULL; j++)
            if (g_strcmp0 (parts[j], CATEGORIES[i].xdg) == 0)
                return CATEGORIES[i].fr;

    return CATEGORIE_AUTRE;
}

/* -------------------------------------------------------------------------
 * Inventaire
 * ------------------------------------------------------------------------- */
static void
app_free (gpointer data)
{
    App *a = data;
    g_object_unref (a->info);
    g_free (a->id);
    g_free (a->nom);
    g_free (a->description);
    g_free (a->botte);
    g_free (a);
}

static int
comparer (gconstpointer x, gconstpointer y)
{
    const App *a = *(App * const *) x;
    const App *b = *(App * const *) y;

    if (L.tri == TRI_CATEGORIE) {
        int c = g_utf8_collate (a->categorie, b->categorie);
        if (c != 0)
            return c;
        /* A categorie egale, le nom : sans second critere l'ordre a
         * l'interieur d'un groupe changerait d'une ouverture a l'autre. */
    }

    int c = g_utf8_collate (a->nom, b->nom);
    return (L.tri == TRI_NOM_INVERSE) ? -c : c;
}

static void
inventaire_relire (void)
{
    g_ptr_array_set_size (L.apps, 0);

    GList *tous = g_app_info_get_all ();
    for (GList *l = tous; l != NULL; l = l->next) {
        GAppInfo *info = l->data;

        /* should_show ecarte NoDisplay et les entrees reservees a un autre
         * bureau. C'est la regle freedesktop, pas notre jugement. */
        if (!g_app_info_should_show (info) || !G_IS_DESKTOP_APP_INFO (info))
            continue;

        const char *fichier = g_app_info_get_id (info);
        if (fichier == NULL)
            continue;

        App *a = g_new0 (App, 1);
        a->info = g_object_ref (G_DESKTOP_APP_INFO (info));

        /* « chromium.desktop » -> « chromium » : c'est cette forme que la
         * liste des epinglees emploie, et celle que le dock compare aux
         * identifiants de fenetres. */
        if (g_str_has_suffix (fichier, ".desktop"))
            a->id = g_strndup (fichier, strlen (fichier) - strlen (".desktop"));
        else
            a->id = g_strdup (fichier);

        a->nom         = g_strdup (g_app_info_get_display_name (info));
        a->description = g_strdup (g_app_info_get_description (info));
        a->categorie   = categorie_de (a->info);

        /* Une seule chaine a fouiller, construite une fois. Chercher dans
         * quatre champs a chaque frappe couterait quatre fois plus pour le
         * meme resultat. Les mots-cles y sont : ils existent precisement
         * pour qu'on trouve « Chromium » en tapant « navigateur ». */
        g_autofree char *mots = g_desktop_app_info_get_string (a->info, "Keywords");
        a->botte = g_strjoin (" ", a->nom,
                              a->description ? a->description : "",
                              mots ? mots : "",
                              a->id, NULL);

        g_ptr_array_add (L.apps, a);
    }
    g_list_free_full (tous, g_object_unref);

    g_ptr_array_sort (L.apps, comparer);
}

/* -------------------------------------------------------------------------
 * Epinglage
 *
 * Comme le panneau de reglages, le lanceur NE GARDE AUCUN ETAT : il relit
 * shell.conf, modifie la seule cle concernee, reecrit. Le dock ecrit dans ce
 * meme fichier quand on y reordonne les icones ; garder une configuration en
 * memoire ecraserait une reorganisation faite entre-temps.
 * ------------------------------------------------------------------------- */
static gboolean
est_epinglee (const char *id)
{
    g_autoptr(ShellConfig) cfg = shell_config_load ();
    for (guint i = 0; cfg->pinned[i] != NULL; i++)
        if (g_strcmp0 (cfg->pinned[i], id) == 0)
            return TRUE;
    return FALSE;
}

static void
epingler (const char *id, gboolean ajouter)
{
    g_autoptr(ShellConfig) cfg = shell_config_load ();
    g_autoptr(GPtrArray) liste = g_ptr_array_new_with_free_func (g_free);

    gboolean present = FALSE;
    for (guint i = 0; cfg->pinned[i] != NULL; i++) {
        if (g_strcmp0 (cfg->pinned[i], id) == 0) {
            present = TRUE;
            if (!ajouter)
                continue;          /* on la retire en ne la recopiant pas */
        }
        g_ptr_array_add (liste, g_strdup (cfg->pinned[i]));
    }

    if (ajouter && !present)
        g_ptr_array_add (liste, g_strdup (id));
    else if (ajouter || !present)
        return;                    /* rien a changer : on n'ecrit pas */

    g_ptr_array_add (liste, NULL);

    g_strfreev (cfg->pinned);
    cfg->pinned = (char **) g_ptr_array_free (g_steal_pointer (&liste), FALSE);

    g_autoptr(GError) error = NULL;
    if (!shell_config_save (cfg, &error))
        g_warning ("épinglage non enregistré : %s", error->message);
    /* Le dock surveille shell.conf : il se reconstruit tout seul. */
}

/* -------------------------------------------------------------------------
 * Ouverture et fermeture du lanceur
 * ------------------------------------------------------------------------- */
static void construire_contenu (void);

static void
fermer (void)
{
    gtk_widget_set_visible (L.fenetre, FALSE);
}

static void
ouvrir (void)
{
    /* Relecture a chaque ouverture : une application installee pendant que
     * la session tourne doit apparaitre sans redemarrer le lanceur. */
    inventaire_relire ();

    /* La recherche repart vide. Retrouver le filtre de la veille en
     * rouvrant, avec une liste apparemment amputee, est deroutant. */
    gtk_editable_set_text (GTK_EDITABLE (L.recherche), "");
    construire_contenu ();

    gtk_widget_set_visible (L.fenetre, TRUE);
    gtk_widget_grab_focus (L.recherche);
}

static void
basculer (void)
{
    if (gtk_widget_get_visible (L.fenetre))
        fermer ();
    else
        ouvrir ();
}

static void
lancer (App *a)
{
    g_autoptr(GError) error = NULL;

    /* On ferme AVANT de lancer : le lanceur ne doit pas rester par-dessus la
     * fenetre qui s'ouvre. */
    fermer ();

    if (!g_app_info_launch (G_APP_INFO (a->info), NULL, NULL, &error))
        g_warning ("lancement de « %s » impossible : %s", a->id, error->message);
}

/* -------------------------------------------------------------------------
 * Menu contextuel
 * ------------------------------------------------------------------------- */
static void
on_action_ouvrir (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) data;
    const char *id = g_variant_get_string (param, NULL);

    for (guint i = 0; i < L.apps->len; i++) {
        App *a = g_ptr_array_index (L.apps, i);
        if (g_strcmp0 (a->id, id) == 0) {
            lancer (a);
            return;
        }
    }
}

static void
on_action_epingler (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) data;
    epingler (g_variant_get_string (param, NULL), TRUE);
    construire_contenu ();   /* la marque « épinglée » change */
}

static void
on_action_detacher (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) data;
    epingler (g_variant_get_string (param, NULL), FALSE);
    construire_contenu ();
}

static const GActionEntry actions_lanceur[] = {
    { "ouvrir",   on_action_ouvrir,   "s", NULL, NULL, { 0 } },
    { "epingler", on_action_epingler, "s", NULL, NULL, { 0 } },
    { "detacher", on_action_detacher, "s", NULL, NULL, { 0 } },
};

static void
on_clic_droit (GtkGestureClick *geste, int n, double x, double y, gpointer data)
{
    App *a = data;
    (void) n;

    GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (geste));

    g_autoptr(GMenu) menu = g_menu_new ();

    /* g_menu_append accepte la forme « action::cible » pour une action a
     * parametre chaine. Les identifiants .desktop ne contiennent que des
     * lettres, chiffres, points et tirets : rien qui puisse etre relu comme
     * un litteral GVariant. */
    g_autofree char *ouvrir_a = g_strdup_printf ("lanceur.ouvrir::%s", a->id);
    g_menu_append (menu, "Ouvrir", ouvrir_a);

    if (est_epinglee (a->id)) {
        g_autofree char *d = g_strdup_printf ("lanceur.detacher::%s", a->id);
        g_menu_append (menu, "Retirer du dock", d);
    } else {
        g_autofree char *e = g_strdup_printf ("lanceur.epingler::%s", a->id);
        g_menu_append (menu, "Épingler au dock", e);
    }

    gtk_popover_menu_set_menu_model (GTK_POPOVER_MENU (L.menu), G_MENU_MODEL (menu));

    /* Le menu est parente a la fenetre : les coordonnees du clic, qui sont
     * relatives a l'element, doivent y etre traduites. */
    graphene_point_t p;
    if (!gtk_widget_compute_point (widget, L.fenetre,
                                   &GRAPHENE_POINT_INIT ((float) x, (float) y), &p))
        return;

    gtk_popover_set_pointing_to (GTK_POPOVER (L.menu),
                                 &(GdkRectangle) { (int) p.x, (int) p.y, 1, 1 });
    gtk_popover_popup (GTK_POPOVER (L.menu));
}

/* -------------------------------------------------------------------------
 * Glisser-deposer vers le dock
 *
 * On transporte l'identifiant .desktop en simple chaine : c'est deja ce que
 * le dock echange avec lui-meme quand on y reordonne les icones, et rien de
 * plus n'est necessaire pour epingler.
 *
 * COPY et non MOVE : deposer une application sur le dock ne la retire pas du
 * lanceur. La difference n'est pas cosmetique -- avec MOVE, GTK attend de la
 * source qu'elle supprime l'original, et le dock, lui, propose les deux
 * actions.
 * ------------------------------------------------------------------------- */
static GdkContentProvider *
on_drag_prepare (GtkDragSource *src, double x, double y, gpointer data)
{
    (void) src; (void) x; (void) y;
    return gdk_content_provider_new_typed (G_TYPE_STRING, (const char *) data);
}

static void
on_drag_begin (GtkDragSource *src, GdkDrag *drag, gpointer data)
{
    (void) drag; (void) data;

    GtkWidget *bouton = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (src));
    GtkWidget *image  = g_object_get_data (G_OBJECT (bouton), "image");

    if (GTK_IS_IMAGE (image)) {
        GdkPaintable *p = gtk_image_get_paintable (GTK_IMAGE (image));
        if (p != NULL)
            gtk_drag_source_set_icon (src, p, ICONE_LISTE, ICONE_LISTE);
    }
}

static void
on_drag_end (GtkDragSource *src, GdkDrag *drag, gboolean delete_data, gpointer data)
{
    (void) src; (void) drag; (void) delete_data; (void) data;

    /* Le lanceur se retire une fois le depot fait : on vient de designer
     * une place dans le dock, c'est le dock qu'on veut voir. */
    fermer ();
}

/* -------------------------------------------------------------------------
 * Un element de la liste
 * ------------------------------------------------------------------------- */
static void
on_item_clicked (GtkButton *b, gpointer data)
{
    (void) b;
    lancer (data);
}

static GtkWidget *
image_pour (App *a, int taille)
{
    GIcon *gicon = g_app_info_get_icon (G_APP_INFO (a->info));
    GtkWidget *image;

    /* Repli explicite : une icone absente du theme afficherait un carre
     * barre, plus laid qu'un pictogramme generique. */
    if (gicon != NULL) {
        image = gtk_image_new_from_gicon (gicon);
    } else {
        image = gtk_image_new_from_icon_name ("application-x-executable");
    }
    gtk_image_set_pixel_size (GTK_IMAGE (image), taille);
    return image;
}

static GtkWidget *
etiquette (const char *texte, const char *classe, int largeur_max, gboolean centree)
{
    GtkWidget *l = gtk_label_new (texte);
    gtk_widget_add_css_class (l, classe);
    gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (l), largeur_max);
    gtk_label_set_xalign (GTK_LABEL (l), centree ? 0.5f : 0.0f);
    gtk_widget_set_halign (l, centree ? GTK_ALIGN_CENTER : GTK_ALIGN_START);
    return l;
}

static GtkWidget *
element_nouveau (App *a, gboolean epinglee)
{
    GtkWidget *image = NULL;
    GtkWidget *corps;

    if (L.vue == VUE_ICONES) {
        image = image_pour (a, ICONE_GRILLE);
        corps = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_halign (corps, GTK_ALIGN_CENTER);
        gtk_box_append (GTK_BOX (corps), image);
        /* Deux lignes, centrees : « Éditeur de texte » ne tient pas sur une
         * seule a cette largeur, et le tronquer rendrait plusieurs
         * applications indistinguables. */
        GtkWidget *nom = etiquette (a->nom, "lanceur-nom", 14, TRUE);
        gtk_label_set_wrap (GTK_LABEL (nom), TRUE);
        gtk_label_set_lines (GTK_LABEL (nom), 2);
        gtk_label_set_justify (GTK_LABEL (nom), GTK_JUSTIFY_CENTER);
        gtk_box_append (GTK_BOX (corps), nom);
    } else if (L.vue == VUE_LISTE) {
        image = image_pour (a, ICONE_LISTE);
        corps = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_append (GTK_BOX (corps), image);
        gtk_box_append (GTK_BOX (corps), etiquette (a->nom, "lanceur-nom", 48, FALSE));
    } else {
        image = image_pour (a, ICONE_DETAILS);
        GtkWidget *textes = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_hexpand (textes, TRUE);
        gtk_box_append (GTK_BOX (textes), etiquette (a->nom, "lanceur-nom", 40, FALSE));
        gtk_box_append (GTK_BOX (textes),
                        etiquette (a->description ? a->description : "—",
                                   "lanceur-detail", 60, FALSE));

        corps = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_box_append (GTK_BOX (corps), image);
        gtk_box_append (GTK_BOX (corps), textes);

        GtkWidget *cat = etiquette (a->categorie, "lanceur-categorie", 20, FALSE);
        gtk_widget_set_valign (cat, GTK_ALIGN_CENTER);
        gtk_box_append (GTK_BOX (corps), cat);
    }

    GtkWidget *bouton = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (bouton), corps);
    gtk_widget_add_css_class (bouton, "lanceur-item");
    gtk_widget_add_css_class (bouton,
        L.vue == VUE_ICONES ? "grille" : L.vue == VUE_LISTE ? "liste" : "details");
    if (epinglee)
        gtk_widget_add_css_class (bouton, "epinglee");

    g_object_set_data (G_OBJECT (bouton), "image", image);
    g_signal_connect (bouton, "clicked", G_CALLBACK (on_item_clicked), a);

    GtkGestureClick *droit = GTK_GESTURE_CLICK (gtk_gesture_click_new ());
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (droit), GDK_BUTTON_SECONDARY);
    g_signal_connect (droit, "pressed", G_CALLBACK (on_clic_droit), a);
    gtk_widget_add_controller (bouton, GTK_EVENT_CONTROLLER (droit));

    GtkDragSource *src = gtk_drag_source_new ();
    gtk_drag_source_set_actions (src, GDK_ACTION_COPY);
    g_signal_connect (src, "prepare",    G_CALLBACK (on_drag_prepare), a->id);
    g_signal_connect (src, "drag-begin", G_CALLBACK (on_drag_begin),   a->id);
    g_signal_connect (src, "drag-end",   G_CALLBACK (on_drag_end),     a->id);
    gtk_widget_add_controller (bouton, GTK_EVENT_CONTROLLER (src));

    return bouton;
}

/* -------------------------------------------------------------------------
 * Construction du contenu
 *
 * Tout est reconstruit a chaque frappe, chaque changement de vue et chaque
 * changement de tri. Sur cette machine le menu compte quelques dizaines
 * d'entrees : un modele avec fabrique de widgets serait plus de machinerie
 * que le contenu n'en merite, pour un gain invisible.
 * ------------------------------------------------------------------------- */
static gboolean
retenue (const App *a, const char *terme)
{
    if (terme == NULL || *terme == '\0')
        return TRUE;

    /* g_str_match_string decoupe en mots, ignore la casse ET les accents.
     * Ecrire cette comparaison a la main donnerait un lanceur qui ne trouve
     * pas « Éditeur » quand on tape « editeur ». */
    return g_str_match_string (terme, a->botte, TRUE);
}

static void
construire_contenu (void)
{
    const char *terme = gtk_editable_get_text (GTK_EDITABLE (L.recherche));

    g_ptr_array_sort (L.apps, comparer);

    g_autoptr(ShellConfig) cfg = shell_config_load ();

    GtkWidget *conteneur;
    if (L.vue == VUE_ICONES) {
        conteneur = gtk_flow_box_new ();
        gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (conteneur), GTK_SELECTION_NONE);
        gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (conteneur), TRUE);
        gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (conteneur), 6);
        gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (conteneur), 4);
    } else {
        conteneur = gtk_list_box_new ();
        gtk_list_box_set_selection_mode (GTK_LIST_BOX (conteneur), GTK_SELECTION_NONE);
    }

    /* START et non le FILL par defaut. Le conteneur occupe toute la hauteur
     * du defilement ; sans cela sa premiere rangee s'etirait sur cette
     * hauteur, et le survol comme la marque « epinglee » peignaient une
     * colonne de 480 px sous l'icone -- vu au banc d'essai, capture a
     * l'appui.
     *
     * C'est bien le CONTENEUR qu'on contraint, pas les elements : a
     * l'interieur d'une rangee, la boite homogene leur donne a tous la
     * meme hauteur, et le survol garde la meme allure d'une icone a
     * l'autre, que son nom tienne sur une ligne ou deux. */
    gtk_widget_set_valign (conteneur, GTK_ALIGN_START);
    gtk_widget_add_css_class (conteneur, "lanceur-contenu");

    guint montrees = 0;
    for (guint i = 0; i < L.apps->len; i++) {
        App *a = g_ptr_array_index (L.apps, i);
        if (!retenue (a, terme))
            continue;

        gboolean epinglee = FALSE;
        for (guint j = 0; cfg->pinned[j] != NULL && !epinglee; j++)
            epinglee = (g_strcmp0 (cfg->pinned[j], a->id) == 0);

        GtkWidget *el = element_nouveau (a, epinglee);
        if (L.vue == VUE_ICONES)
            gtk_flow_box_append (GTK_FLOW_BOX (conteneur), el);
        else
            gtk_list_box_append (GTK_LIST_BOX (conteneur), el);
        montrees++;
    }

    if (montrees == 0) {
        /* Une liste vide sans un mot laisse croire a une panne. */
        GtkWidget *rien = gtk_label_new ("Aucune application ne correspond.");
        gtk_widget_add_css_class (rien, "lanceur-vide");
        gtk_widget_set_valign (rien, GTK_ALIGN_CENTER);
        gtk_widget_set_vexpand (rien, TRUE);
        gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (L.defilement), rien);
        return;
    }

    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (L.defilement), conteneur);
}

/* -------------------------------------------------------------------------
 * Barre d'outils
 * ------------------------------------------------------------------------- */
static void
on_recherche (GtkSearchEntry *e, gpointer data)
{
    (void) e; (void) data;
    construire_contenu ();
}

static void
on_tri (GObject *dd, GParamSpec *ps, gpointer data)
{
    (void) ps; (void) data;
    guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (dd));
    if (i == GTK_INVALID_LIST_POSITION)
        return;
    L.tri = (Tri) i;
    construire_contenu ();
}

static void
on_vue (GtkToggleButton *b, gpointer data)
{
    /* Le groupe emet aussi pour le bouton qui vient d'etre relache. Sans ce
     * garde-fou, chaque changement de vue reconstruirait le contenu deux
     * fois, dont une pour l'ancienne vue. */
    if (!gtk_toggle_button_get_active (b))
        return;

    L.vue = (Vue) GPOINTER_TO_INT (data);
    construire_contenu ();
}

static GtkWidget *
bouton_vue (const char *libelle, Vue vue, GtkWidget *groupe)
{
    GtkWidget *b = gtk_toggle_button_new_with_label (libelle);
    gtk_widget_add_css_class (b, "lanceur-vue");
    if (groupe != NULL)
        gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (b), GTK_TOGGLE_BUTTON (groupe));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (b), vue == L.vue);
    g_signal_connect (b, "toggled", G_CALLBACK (on_vue), GINT_TO_POINTER (vue));
    return b;
}

/* -------------------------------------------------------------------------
 * Clavier
 * ------------------------------------------------------------------------- */
static gboolean
on_touche (GtkEventControllerKey *c, guint keyval, guint code,
           GdkModifierType mods, gpointer data)
{
    (void) c; (void) code; (void) mods; (void) data;

    if (keyval == GDK_KEY_Escape) {
        fermer ();
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

/* Un clic hors du panneau ferme le lanceur.
 *
 * En phase de bulle : ce qu'un bouton du panneau a deja traite n'arrive
 * jamais ici. Ce qui arrive est teste contre les limites du cadre -- cliquer
 * sur le fond du panneau, entre deux icones, ne doit rien fermer. */
static void
on_clic_voile (GtkGestureClick *geste, int n, double x, double y, gpointer data)
{
    (void) geste; (void) n; (void) data;

    graphene_rect_t limites;
    if (!gtk_widget_compute_bounds (L.cadre, L.fenetre, &limites))
        return;

    if (!graphene_rect_contains_point (&limites,
                                       &GRAPHENE_POINT_INIT ((float) x, (float) y)))
        fermer ();
}

/* -------------------------------------------------------------------------
 * Fenetre
 * ------------------------------------------------------------------------- */
static void
on_config_reloaded (ShellConfig *cfg, gpointer data)
{
    (void) data;
    shell_styles_load (cfg->theme);
    shell_config_apply (cfg);
    shell_config_free (cfg);
}

static void
construire_fenetre (GtkApplication *app, ShellConfig *cfg)
{
    shell_config_apply (cfg);

    L.fenetre = gtk_application_window_new (app);
    gtk_widget_add_css_class (L.fenetre, "shell");
    gtk_widget_add_css_class (L.fenetre, "lanceur");

    /* Ancrage aux QUATRE bords : la surface couvre l'ecran. C'est ce qui
     * permet de fermer au clic a cote -- sans surface sous le pointeur, il
     * n'y a aucun clic « a cote » a recevoir. Une fenetre ordinaire ne
     * pourrait de toute facon pas se placer elle-meme : sous Wayland, un
     * client ne choisit pas ses coordonnees. */
    gtk_layer_init_for_window (GTK_WINDOW (L.fenetre));
    /* OVERLAY, au-dessus du dock qui est en TOP : le lanceur doit passer
     * devant lui, sans quoi ses dernieres lignes seraient masquees. */
    gtk_layer_set_layer (GTK_WINDOW (L.fenetre), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace (GTK_WINDOW (L.fenetre), "claude-os-lanceur");
    for (int bord = 0; bord < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; bord++)
        gtk_layer_set_anchor (GTK_WINDOW (L.fenetre), bord, TRUE);
    /* La bande du bas reste au dock : voir l'en-tete. */
    gtk_layer_set_margin (GTK_WINDOW (L.fenetre),
                          GTK_LAYER_SHELL_EDGE_BOTTOM, BANDE_DOCK);
    /* -1 : le voile ignore les zones reservees par les autres surfaces. Sa
     * geometrie est decidee ici, par la marge, et ne doit pas dependre du
     * reglage « reserver la place du dock ». */
    gtk_layer_set_exclusive_zone (GTK_WINDOW (L.fenetre), -1);
    /* EXCLUSIVE : on ouvre le lanceur pour taper. Attendre un clic dans le
     * champ avant que la frappe y arrive rendrait la recherche inutilisable. */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (L.fenetre),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    GtkEventControllerKey *touches =
        GTK_EVENT_CONTROLLER_KEY (gtk_event_controller_key_new ());
    g_signal_connect (touches, "key-pressed", G_CALLBACK (on_touche), NULL);
    gtk_widget_add_controller (L.fenetre, GTK_EVENT_CONTROLLER (touches));

    GtkGestureClick *clic = GTK_GESTURE_CLICK (gtk_gesture_click_new ());
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (clic), 0);   /* tout bouton */
    g_signal_connect (clic, "pressed", G_CALLBACK (on_clic_voile), NULL);
    gtk_widget_add_controller (L.fenetre, GTK_EVENT_CONTROLLER (clic));

    /* --- barre d'outils --- */
    L.recherche = gtk_search_entry_new ();
    gtk_widget_add_css_class (L.recherche, "lanceur-recherche");
    gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (L.recherche), "Rechercher");
    gtk_widget_set_hexpand (L.recherche, TRUE);
    g_signal_connect (L.recherche, "search-changed", G_CALLBACK (on_recherche), NULL);

    GtkWidget *tri_lib = gtk_label_new ("Trier par");
    gtk_widget_add_css_class (tri_lib, "lanceur-libelle");

    GtkStringList *tris = gtk_string_list_new (
        (const char *[]) { "Nom (A → Z)", "Nom (Z → A)", "Catégorie", NULL });
    GtkWidget *tri = gtk_drop_down_new (G_LIST_MODEL (tris), NULL);
    gtk_widget_add_css_class (tri, "lanceur-tri");
    gtk_drop_down_set_selected (GTK_DROP_DOWN (tri), (guint) L.tri);
    g_signal_connect (tri, "notify::selected", G_CALLBACK (on_tri), NULL);

    GtkWidget *aff_lib = gtk_label_new ("Afficher");
    gtk_widget_add_css_class (aff_lib, "lanceur-libelle");

    /* Trois boutons soudes, un seul enfonce : le meme geste que les
     * segments de ChromeOS, avec des mots plutot que des pictogrammes --
     * « liste » et « details » n'ont pas d'icone que tout le monde lise
     * pareil. */
    GtkWidget *vues = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (vues, "linked");
    gtk_widget_add_css_class (vues, "lanceur-vues");
    GtkWidget *b_icones = bouton_vue ("Icônes",  VUE_ICONES,  NULL);
    gtk_box_append (GTK_BOX (vues), b_icones);
    gtk_box_append (GTK_BOX (vues), bouton_vue ("Liste",   VUE_LISTE,   b_icones));
    gtk_box_append (GTK_BOX (vues), bouton_vue ("Détails", VUE_DETAILS, b_icones));

    GtkWidget *barre = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class (barre, "lanceur-barre");
    gtk_box_append (GTK_BOX (barre), L.recherche);
    gtk_box_append (GTK_BOX (barre), tri_lib);
    gtk_box_append (GTK_BOX (barre), tri);
    gtk_box_append (GTK_BOX (barre), aff_lib);
    gtk_box_append (GTK_BOX (barre), vues);

    /* --- contenu --- */
    L.defilement = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (L.defilement),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (L.defilement, TRUE);

    GtkWidget *cadre = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class (cadre, "lanceur-cadre");
    gtk_widget_set_size_request (cadre, LARGEUR, HAUTEUR);
    /* Centre dans le voile, qui couvre l'ecran : c'est ce centrage-la qui
     * place le panneau, et non plus le compositeur. */
    gtk_widget_set_halign (cadre, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (cadre, GTK_ALIGN_CENTER);
    L.cadre = cadre;
    gtk_box_append (GTK_BOX (cadre), barre);
    gtk_box_append (GTK_BOX (cadre), L.defilement);

    /* Le menu contextuel est parente a la fenetre, une seule fois : un menu
     * par element serait recree a chaque frappe, et il faudrait le detacher
     * a la main avant chaque reconstruction. */
    L.menu = gtk_popover_menu_new_from_model (NULL);
    gtk_widget_add_css_class (L.menu, "lanceur-menu");
    gtk_widget_set_parent (L.menu, cadre);
    gtk_popover_set_has_arrow (GTK_POPOVER (L.menu), FALSE);

    gtk_window_set_child (GTK_WINDOW (L.fenetre), cadre);

    GSimpleActionGroup *groupe = g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (groupe), actions_lanceur,
                                     G_N_ELEMENTS (actions_lanceur), NULL);
    gtk_widget_insert_action_group (L.fenetre, "lanceur", G_ACTION_GROUP (groupe));
    g_object_unref (groupe);
}

/* Le lanceur ne s'ouvre pas au demarrage : le premier appel l'a lance, les
 * suivants -- GtkApplication n'admet qu'une instance -- retombent ici et le
 * font basculer. */
static void
on_activate (GtkApplication *app, gpointer user_data)
{
    if (L.fenetre == NULL) {
        construire_fenetre (app, user_data);

        /* Sans cela, masquer la seule fenetre ferait sortir GApplication de
         * sa boucle : le lanceur se fermerait pour de bon, et la prochaine
         * ouverture repaierait le demarrage complet. */
        g_application_hold (G_APPLICATION (app));
        shell_config_watch (on_config_reloaded, NULL);

        ouvrir ();
        return;
    }
    basculer ();
}

static void
on_action_basculer (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) param; (void) data;
    if (L.fenetre != NULL)
        basculer ();
}

static const GActionEntry actions_app[] = {
    { "basculer", on_action_basculer, NULL, NULL, NULL, { 0 } },
};

int
main (int argc, char **argv)
{
    (void) argc; (void) argv;

    L.apps = g_ptr_array_new_with_free_func (app_free);
    L.vue  = VUE_ICONES;    /* « en icones par defaut » */
    L.tri  = TRI_NOM;

    ShellConfig *cfg = shell_config_load ();

    GtkApplication *app = gtk_application_new ("os.claude.shell.lanceur",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "startup",  G_CALLBACK (shell_styles_startup), cfg);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), cfg);
    g_action_map_add_action_entries (G_ACTION_MAP (app), actions_app,
                                     G_N_ELEMENTS (actions_app), NULL);

    int status = g_application_run (G_APPLICATION (app), 0, NULL);
    g_object_unref (app);
    return status;
}
