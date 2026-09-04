/* =========================================================================
 * Claude-OS Shell — gestionnaire de fichiers
 *
 * Une fenetre ORDINAIRE, pas une surface layer-shell : elle se deplace, se
 * redimensionne et se reduit comme Chromium. C'est une application, pas un
 * morceau du bureau.
 *
 * DISPOSITION, A LA WINDOWS
 *
 *   +--------------------------------------------------------------+
 *   | < > ^ ⟳ | fil d'Ariane                     | recherche | vues |
 *   | Nouveau dossier · Copier · Couper · Coller · Renommer · Suppr |
 *   +-----------+--------------------------------------------------+
 *   | favoris   |  contenu : icones, liste ou details               |
 *   +-----------+--------------------------------------------------+
 *   | 42 éléments · 3 sélectionnés · 1,2 Mo                         |
 *   +--------------------------------------------------------------+
 *
 * LES VUES PARTAGENT TOUT
 *
 * Un seul magasin, un seul filtre, un seul tri, une seule selection --
 * chaque vue n'est qu'une facon de les dessiner. Changer de vue ne perd donc
 * ni la selection ni le tri, et le tri se regle en cliquant les en-tetes de
 * la vue Details, dont le trieur est celui de tout le monde.
 *
 * Les trois vues sont VIRTUELLES : elles ne construisent des widgets que
 * pour ce qui est a l'ecran. Un repertoire de dix mille fichiers ne coute
 * donc que dix mille petits objets.
 * ========================================================================= */

#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>

#include "config.h"
#include "fichiers.h"
#include "fichiers-ops.h"
#include "fichiers-lieux.h"

typedef enum { VUE_ICONES, VUE_LISTE, VUE_DETAILS } Vue;

static struct {
    GtkWidget          *fenetre;
    GtkWidget          *pile;        /* les trois vues                      */
    GtkWidget          *lieux;
    GtkWidget          *fil;         /* boite du fil d'Ariane               */
    GtkWidget          *fil_defil;   /* son defilement horizontal           */
    GtkWidget          *pile_adresse;/* fil d'Ariane ou champ de saisie     */
    GtkWidget          *adresse;     /* le champ, quand on tape un chemin   */
    GtkWidget          *recherche;
    GtkWidget          *etat;
    GtkWidget          *menu;
    GtkWidget          *precedent;
    GtkWidget          *suivant;
    GtkWidget          *parent;

    GListStore         *magasin;
    GtkFilterListModel *modele_filtre;
    GtkSortListModel   *modele_tri;
    GtkSelectionModel  *selection;
    GtkFilter          *filtre;
    GtkColumnView      *colonnes;
    GtkColumnViewColumn *col_nom, *col_date, *col_type, *col_taille;

    GFile              *dossier;
    GPtrArray          *histoire;    /* GFile*, du plus ancien au plus recent */
    int                 position;    /* index courant dans histoire         */

    GPtrArray          *presse;      /* GFile* copies ou coupes             */
    gboolean            couper;
    gboolean            montrer_caches;
} F;

static void naviguer (GFile *dossier, gboolean historiser);
static void maj_etat (void);

/* -------------------------------------------------------------------------
 * Selection
 * ------------------------------------------------------------------------- */

/* Les elements selectionnes, dans l'ordre affiche. La liste est a liberer,
 * les elements sont empruntes au modele. */
static GList *
choisis (void)
{
    GList *out = NULL;
    g_autoptr(GtkBitset) bits = gtk_selection_model_get_selection (F.selection);
    GtkBitsetIter it;
    guint i;

    if (!gtk_bitset_iter_init_first (&it, bits, &i))
        return NULL;

    do {
        gpointer o = g_list_model_get_item (G_LIST_MODEL (F.selection), i);
        if (o != NULL)
            out = g_list_prepend (out, o);      /* reference prise */
    } while (gtk_bitset_iter_next (&it, &i));

    return g_list_reverse (out);
}

/* Les GFile correspondants. Liste a liberer avec g_object_unref. */
static GList *
choisis_fichiers (void)
{
    GList *out = NULL;
    g_autolist(GObject) items = choisis ();

    for (GList *l = items; l != NULL; l = l->next) {
        FichierItem *it = l->data;
        out = g_list_prepend (out, g_object_ref (it->file));
    }
    return g_list_reverse (out);
}

static FichierItem *
premier_choisi (void)
{
    g_autolist(GObject) items = choisis ();
    return (items != NULL) ? g_object_ref (items->data) : NULL;
}

/* -------------------------------------------------------------------------
 * Ouverture
 * ------------------------------------------------------------------------- */
static void
signaler (const char *titre, const char *detail)
{
    GtkAlertDialog *d = gtk_alert_dialog_new ("%s", titre);
    if (detail != NULL)
        gtk_alert_dialog_set_detail (d, detail);
    gtk_alert_dialog_show (d, GTK_WINDOW (F.fenetre));
    g_object_unref (d);
}

static void
ouvrir_item (FichierItem *it)
{
    if (it == NULL)
        return;

    if (it->dossier) {
        naviguer (it->file, TRUE);
        return;
    }

    g_autofree char *uri = g_file_get_uri (it->file);
    g_autoptr(GError) e = NULL;

    if (!g_app_info_launch_default_for_uri (uri, NULL, &e))
        signaler ("Impossible d'ouvrir ce fichier", e->message);
}

static void
on_active (GtkWidget *vue, guint position, gpointer data)
{
    (void) vue; (void) data;
    g_autoptr(FichierItem) it = g_list_model_get_item (G_LIST_MODEL (F.selection),
                                                       position);
    ouvrir_item (it);
}

/* -------------------------------------------------------------------------
 * Navigation
 * ------------------------------------------------------------------------- */
/* Un chemin profond deborde de la barre. Sans cela le fil resterait cale a
 * gauche, sur « Ordinateur », et le dossier ou l'on se trouve -- la seule
 * chose qu'on cherche a lire -- serait hors champ. Le calage se fait apres
 * la mise en page : la largeur du contenu n'existe pas avant. */
static gboolean
caler_fil (gpointer data)
{
    (void) data;
    GtkAdjustment *a =
        gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (F.fil_defil));

    gtk_adjustment_set_value (a, gtk_adjustment_get_upper (a)
                                 - gtk_adjustment_get_page_size (a));
    return G_SOURCE_REMOVE;
}

static void
on_fil_clic (GtkButton *b, gpointer data)
{
    (void) data;
    naviguer (g_object_get_data (G_OBJECT (b), "fichier"), TRUE);
}

static void
maj_fil (void)
{
    GtkWidget *enfant;
    while ((enfant = gtk_widget_get_first_child (F.fil)) != NULL)
        gtk_box_remove (GTK_BOX (F.fil), enfant);

    /* On remonte jusqu'a la racine, puis on redescend : un fil d'Ariane se
     * lit de gauche a droite, et g_file_get_parent ne sait aller que dans
     * l'autre sens. */
    g_autoptr(GPtrArray) chaine = g_ptr_array_new_with_free_func (g_object_unref);
    for (GFile *f = g_object_ref (F.dossier); f != NULL; ) {
        g_ptr_array_insert (chaine, 0, f);
        f = g_file_get_parent (f);
    }

    for (guint i = 0; i < chaine->len; i++) {
        GFile *f = g_ptr_array_index (chaine, i);
        g_autofree char *nom = g_file_get_basename (f);

        /* « / » comme nom de bouton est illisible ; le dossier personnel se
         * nomme d'ailleurs par son role et non par son chemin. */
        const char *libelle = nom;
        g_autofree char *chemin = g_file_get_path (f);
        if (g_strcmp0 (chemin, "/") == 0)
            libelle = "Ordinateur";
        else if (g_strcmp0 (chemin, g_get_home_dir ()) == 0)
            libelle = "Dossier personnel";

        if (i > 0) {
            GtkWidget *sep = gtk_label_new ("›");
            gtk_widget_add_css_class (sep, "fil-separateur");
            gtk_box_append (GTK_BOX (F.fil), sep);
        }

        GtkWidget *b = gtk_button_new_with_label (libelle);
        gtk_widget_add_css_class (b, "fil-element");
        if (i == chaine->len - 1)
            gtk_widget_add_css_class (b, "courant");
        g_object_set_data_full (G_OBJECT (b), "fichier", g_object_ref (f), g_object_unref);
        g_signal_connect (b, "clicked", G_CALLBACK (on_fil_clic), NULL);
        gtk_box_append (GTK_BOX (F.fil), b);
    }

    g_idle_add (caler_fil, NULL);
}

static void
maj_boutons (void)
{
    gtk_widget_set_sensitive (F.precedent, F.position > 0);
    gtk_widget_set_sensitive (F.suivant,
                              F.position + 1 < (int) F.histoire->len);

    g_autoptr(GFile) p = g_file_get_parent (F.dossier);
    gtk_widget_set_sensitive (F.parent, p != NULL);
}

static void
on_lu (GListStore *magasin, GError *erreur, gpointer data)
{
    (void) magasin; (void) data;

    if (erreur != NULL && !g_error_matches (erreur, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        signaler ("Impossible de lire ce dossier", erreur->message);

    maj_etat ();
}

static void
recharger (void)
{
    fichiers_lire (F.dossier, F.magasin, on_lu, NULL);
}

static void
naviguer (GFile *dossier, gboolean historiser)
{
    if (dossier == NULL)
        return;

    /* Une adresse illisible n'est pas une erreur a signaler tard : on la
     * refuse avant de perdre le dossier courant. */
    if (!g_file_query_exists (dossier, NULL)) {
        g_autofree char *chemin = g_file_get_parse_name (dossier);
        signaler ("Ce dossier n'existe pas", chemin);
        return;
    }

    if (historiser) {
        /* Naviguer depuis un point de l'historique efface ce qui suivait :
         * c'est ce que fait un navigateur, et l'alternative -- garder deux
         * branches -- n'a aucune representation a l'ecran. */
        for (int i = (int) F.histoire->len - 1; i > F.position; i--)
            g_ptr_array_remove_index (F.histoire, (guint) i);

        g_ptr_array_add (F.histoire, g_object_ref (dossier));
        F.position = (int) F.histoire->len - 1;
    }

    g_clear_object (&F.dossier);
    F.dossier = g_object_ref (dossier);

    gtk_editable_set_text (GTK_EDITABLE (F.recherche), "");
    maj_fil ();
    maj_boutons ();
    fichiers_lieux_suivre (F.lieux, F.dossier);
    recharger ();
}

static void
aller_a (int position)
{
    if (position < 0 || position >= (int) F.histoire->len)
        return;
    F.position = position;
    naviguer (g_ptr_array_index (F.histoire, (guint) position), FALSE);
}

static void
on_nav_lieux (GFile *dossier, gpointer data)
{
    (void) data;
    naviguer (dossier, TRUE);
}

/* -------------------------------------------------------------------------
 * Barre d'etat
 * ------------------------------------------------------------------------- */
static void
maj_etat (void)
{
    guint total = g_list_model_get_n_items (G_LIST_MODEL (F.selection));

    g_autoptr(GtkBitset) bits = gtk_selection_model_get_selection (F.selection);
    guint n = gtk_bitset_get_size (bits);

    GString *s = g_string_new (NULL);
    g_string_append_printf (s, "%u élément%s", total, total > 1 ? "s" : "");

    if (n > 0) {
        g_string_append_printf (s, " · %u sélectionné%s", n, n > 1 ? "s" : "");

        goffset somme = 0;
        g_autolist(GObject) items = choisis ();
        for (GList *l = items; l != NULL; l = l->next) {
            FichierItem *it = l->data;
            if (!it->dossier)
                somme += it->taille;
        }
        if (somme > 0) {
            g_autofree char *t = g_format_size ((guint64) somme);
            g_string_append_printf (s, " · %s", t);
        }
    }

    if (F.presse->len > 0)
        g_string_append_printf (s, "   ·   %u dans le presse-papier (%s)",
                                F.presse->len, F.couper ? "couper" : "copier");

    gtk_label_set_text (GTK_LABEL (F.etat), s->str);
    g_string_free (s, TRUE);
}

static void
on_selection_changed (GtkSelectionModel *m, guint p, guint n, gpointer d)
{
    (void) m; (void) p; (void) n; (void) d;
    maj_etat ();
}

static void
on_items_changed (GListModel *m, guint p, guint r, guint a, gpointer d)
{
    (void) m; (void) p; (void) r; (void) a; (void) d;
    maj_etat ();
}

/* -------------------------------------------------------------------------
 * Filtre : fichiers caches et recherche
 * ------------------------------------------------------------------------- */
static gboolean
retenu (gpointer objet, gpointer data)
{
    FichierItem *it = objet;
    (void) data;

    if (it->cache && !F.montrer_caches)
        return FALSE;

    const char *terme = gtk_editable_get_text (GTK_EDITABLE (F.recherche));
    if (terme == NULL || *terme == '\0')
        return TRUE;

    /* Casse et accents ignores : chercher « ete » doit trouver « Été.txt ». */
    return g_str_match_string (terme, it->nom, TRUE);
}

static void
refiltrer (void)
{
    gtk_filter_changed (F.filtre, GTK_FILTER_CHANGE_DIFFERENT);
    maj_etat ();
}

static void
on_recherche (GtkSearchEntry *e, gpointer d)
{
    (void) e; (void) d;
    refiltrer ();
}

/* -------------------------------------------------------------------------
 * Tri
 *
 * Les dossiers d'abord, toujours : c'est ce que fait tout gestionnaire de
 * fichiers, et melanger les deux rend une arborescence illisible.
 * ------------------------------------------------------------------------- */
#define DOSSIERS_D_ABORD(a, b) \
    do { if ((a)->dossier != (b)->dossier) return (a)->dossier ? -1 : 1; } while (0)

static int cmp_nom (gconstpointer x, gconstpointer y, gpointer d)
{
    const FichierItem *a = x, *b = y; (void) d;
    DOSSIERS_D_ABORD (a, b);
    return strcmp (a->cle_tri, b->cle_tri);
}

static int cmp_date (gconstpointer x, gconstpointer y, gpointer d)
{
    const FichierItem *a = x, *b = y; (void) d;
    DOSSIERS_D_ABORD (a, b);
    if (a->modifie != b->modifie)
        return (a->modifie < b->modifie) ? -1 : 1;
    return strcmp (a->cle_tri, b->cle_tri);
}

static int cmp_type (gconstpointer x, gconstpointer y, gpointer d)
{
    const FichierItem *a = x, *b = y; (void) d;
    DOSSIERS_D_ABORD (a, b);
    int c = g_utf8_collate (a->type_texte, b->type_texte);
    return (c != 0) ? c : strcmp (a->cle_tri, b->cle_tri);
}

static int cmp_taille (gconstpointer x, gconstpointer y, gpointer d)
{
    const FichierItem *a = x, *b = y; (void) d;
    DOSSIERS_D_ABORD (a, b);
    if (a->taille != b->taille)
        return (a->taille < b->taille) ? -1 : 1;
    return strcmp (a->cle_tri, b->cle_tri);
}

/* -------------------------------------------------------------------------
 * Fabriques de widgets, une par vue
 * ------------------------------------------------------------------------- */
static void on_clic_item (GtkGestureClick *g, int n, double x, double y, gpointer d);
static GdkContentProvider *on_item_drag (GtkDragSource *s, double x, double y, gpointer d);
static gboolean on_item_drop (GtkDropTarget *t, const GValue *v, double x, double y, gpointer d);

/* Le clic droit et le glisser sont poses une seule fois, a la construction
 * du widget, et lisent l'element courant a l'usage : les fabriques recyclent
 * leurs widgets, rebrancher un controleur a chaque liaison en empilerait un
 * par defilement. */
static void
armer (GtkWidget *w, GtkListItem *li)
{
    g_object_set_data (G_OBJECT (w), "list-item", li);

    GtkGestureClick *droit = GTK_GESTURE_CLICK (gtk_gesture_click_new ());
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (droit), GDK_BUTTON_SECONDARY);
    g_signal_connect (droit, "pressed", G_CALLBACK (on_clic_item), w);
    gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (droit));

    GtkDragSource *src = gtk_drag_source_new ();
    gtk_drag_source_set_actions (src, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect (src, "prepare", G_CALLBACK (on_item_drag), w);
    gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (src));

    /* Deposer sur un DOSSIER l'y range. Sur un fichier, la cible refuse et
     * l'evenement remonte a la vue, qui depose dans le dossier courant. */
    GtkDropTarget *dst = gtk_drop_target_new (GDK_TYPE_FILE_LIST,
                                              GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect (dst, "drop", G_CALLBACK (on_item_drop), w);
    gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (dst));
}

static FichierItem *
item_de (GtkWidget *w)
{
    GtkListItem *li = g_object_get_data (G_OBJECT (w), "list-item");
    return (li != NULL) ? gtk_list_item_get_item (li) : NULL;
}

/* --- grille --------------------------------------------------------------- */
static void
grille_setup (GtkListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void) f; (void) d;

    GtkWidget *img = gtk_image_new ();
    gtk_image_set_pixel_size (GTK_IMAGE (img), 48);

    GtkWidget *nom = gtk_label_new (NULL);
    gtk_widget_add_css_class (nom, "fichiers-nom");
    gtk_label_set_wrap (GTK_LABEL (nom), TRUE);
    gtk_label_set_lines (GTK_LABEL (nom), 2);
    gtk_label_set_ellipsize (GTK_LABEL (nom), PANGO_ELLIPSIZE_END);
    gtk_label_set_justify (GTK_LABEL (nom), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars (GTK_LABEL (nom), 14);

    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class (boite, "fichiers-case");
    gtk_widget_set_halign (boite, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (boite), img);
    gtk_box_append (GTK_BOX (boite), nom);

    g_object_set_data (G_OBJECT (boite), "image", img);
    g_object_set_data (G_OBJECT (boite), "nom", nom);
    armer (boite, li);
    gtk_list_item_set_child (li, boite);
}

static void
case_bind (GtkListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void) f; (void) d;
    GtkWidget   *boite = gtk_list_item_get_child (li);
    FichierItem *it    = gtk_list_item_get_item (li);
    if (boite == NULL || it == NULL)
        return;

    gtk_image_set_from_gicon (GTK_IMAGE (g_object_get_data (G_OBJECT (boite), "image")),
                              it->icone);
    gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (boite), "nom")),
                        it->nom);

    /* Un fichier coupe s'affiche estompe, comme dans Windows : on voit d'un
     * coup d'oeil ce qui partira au collage. */
    gboolean coupe = FALSE;
    for (guint i = 0; i < F.presse->len && F.couper && !coupe; i++)
        coupe = g_file_equal (g_ptr_array_index (F.presse, i), it->file);
    gtk_widget_set_opacity (boite, coupe ? 0.45 : 1.0);
}

/* --- liste ---------------------------------------------------------------- */
static void
liste_setup (GtkListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void) f; (void) d;

    GtkWidget *img = gtk_image_new ();
    gtk_image_set_pixel_size (GTK_IMAGE (img), 20);

    GtkWidget *nom = gtk_label_new (NULL);
    gtk_widget_add_css_class (nom, "fichiers-nom");
    gtk_label_set_ellipsize (GTK_LABEL (nom), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign (nom, GTK_ALIGN_START);

    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class (boite, "fichiers-ligne");
    /* La boite prend toute la largeur de sa cellule. Sans cela elle s'arrete
     * a la fin du nom, et le clic droit trois centimetres plus loin -- sur la
     * meme ligne, en apparence -- ouvrait le menu du FOND : « Nouveau
     * dossier » la ou on visait « Renommer ». Constate au banc d'essai. */
    gtk_widget_set_hexpand (boite, TRUE);
    gtk_box_append (GTK_BOX (boite), img);
    gtk_box_append (GTK_BOX (boite), nom);

    g_object_set_data (G_OBJECT (boite), "image", img);
    g_object_set_data (G_OBJECT (boite), "nom", nom);
    armer (boite, li);
    gtk_list_item_set_child (li, boite);
}

/* --- colonnes -------------------------------------------------------------- */
static void
colonne_nom_setup (GtkListItemFactory *f, GtkListItem *li, gpointer d)
{
    liste_setup (f, li, d);
}

typedef char *(*TexteFunc) (FichierItem *it);

static void
texte_setup (GtkListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void) f; (void) d;
    GtkWidget *l = gtk_label_new (NULL);
    gtk_widget_add_css_class (l, "fichiers-detail");
    gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
    /* FILL et xalign 0 : le texte reste cale a gauche, mais l'etiquette
     * occupe sa cellule, et le clic droit y porte. */
    gtk_widget_set_halign (l, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand (l, TRUE);
    gtk_label_set_xalign (GTK_LABEL (l), 0.0);
    /* Meme armement que la colonne du nom : dans la vue Details, une ligne
     * se clique et se glisse sur toute sa longueur, colonnes comprises. */
    armer (l, li);
    gtk_list_item_set_child (li, l);
}

static void
texte_bind (GtkListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void) f;
    TexteFunc fn = d;
    FichierItem *it = gtk_list_item_get_item (li);
    GtkWidget *l = gtk_list_item_get_child (li);
    if (it == NULL || l == NULL)
        return;

    g_autofree char *t = fn (it);
    gtk_label_set_text (GTK_LABEL (l), t);
}

static char *type_de (FichierItem *it) { return g_strdup (it->type_texte); }

static GtkListItemFactory *
fabrique (GCallback setup, GCallback bind, gpointer data)
{
    GtkListItemFactory *f = gtk_signal_list_item_factory_new ();
    g_signal_connect (f, "setup", setup, data);
    g_signal_connect (f, "bind",  bind,  data);
    return f;
}

/* `largeur` a -1 laisse la colonne prendre la place restante ; une valeur
 * fixe la borne. Sans bornes, les colonnes se dimensionnent sur leur contenu
 * et la derniere depasse la fenetre -- « 240,0 ko » tronque a droite, vu au
 * banc d'essai. Elles restent redimensionnables a la main. */
static GtkColumnViewColumn *
colonne (const char *titre, GtkListItemFactory *f, GtkSorter *tri, int largeur)
{
    GtkColumnViewColumn *c = gtk_column_view_column_new (titre, f);
    gtk_column_view_column_set_sorter (c, tri);
    gtk_column_view_column_set_resizable (c, TRUE);

    if (largeur < 0)
        gtk_column_view_column_set_expand (c, TRUE);
    else
        gtk_column_view_column_set_fixed_width (c, largeur);

    g_object_unref (tri);
    gtk_column_view_append_column (F.colonnes, c);
    return c;
}

/* -------------------------------------------------------------------------
 * Presse-papier
 *
 * Interne : c'est lui qui fait foi pour coller. Le presse-papier du systeme
 * recoit en plus la liste des adresses, pour qu'un autre programme puisse
 * s'en servir. Lire CE QU'UN AUTRE PROGRAMME y a mis n'est pas encore fait.
 * ------------------------------------------------------------------------- */
static void
publier_presse_papier (void)
{
    if (F.presse->len == 0)
        return;

    GString *uris = g_string_new (NULL);
    for (guint i = 0; i < F.presse->len; i++) {
        g_autofree char *u = g_file_get_uri (g_ptr_array_index (F.presse, i));
        g_string_append_printf (uris, "%s\n", u);
    }

    GdkClipboard *cb = gtk_widget_get_clipboard (F.fenetre);
    gdk_clipboard_set_text (cb, uris->str);
    g_string_free (uris, TRUE);
}

static void
presse_remplir (gboolean couper)
{
    g_ptr_array_set_size (F.presse, 0);
    F.couper = couper;

    g_autolist(GObject) fichiers = choisis_fichiers ();
    for (GList *l = fichiers; l != NULL; l = l->next)
        g_ptr_array_add (F.presse, g_object_ref (l->data));

    publier_presse_papier ();
    /* Le gris des fichiers coupes ne se pose qu'a la reconstruction des
     * lignes : on force un nouveau passage du filtre, qui les relie. */
    refiltrer ();
}

/* -------------------------------------------------------------------------
 * Actions
 * ------------------------------------------------------------------------- */
static void
on_op_finie (GError *erreur, gpointer data)
{
    (void) data;

    if (erreur != NULL && !g_error_matches (erreur, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        signaler ("L'opération a échoué", erreur->message);

    recharger ();
}

static void
act_coller (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    if (F.presse->len == 0)
        return;

    GList *sources = NULL;
    for (guint i = 0; i < F.presse->len; i++)
        sources = g_list_prepend (sources, g_ptr_array_index (F.presse, i));
    sources = g_list_reverse (sources);

    fichiers_op (F.couper ? OP_DEPLACER : OP_COPIER, sources, F.dossier,
                 GTK_WINDOW (F.fenetre), on_op_finie, NULL);
    g_list_free (sources);

    /* Un couper ne vaut qu'une fois : le second collage n'aurait plus rien a
     * deplacer, et laisserait croire le contraire. */
    if (F.couper) {
        g_ptr_array_set_size (F.presse, 0);
        F.couper = FALSE;
    }
    maj_etat ();
}

static void act_copier (GSimpleAction *a, GVariant *p, gpointer d)
{ (void) a; (void) p; (void) d; presse_remplir (FALSE); }

static void act_couper (GSimpleAction *a, GVariant *p, gpointer d)
{ (void) a; (void) p; (void) d; presse_remplir (TRUE); }

static void
act_corbeille (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    g_autolist(GObject) f = choisis_fichiers ();
    if (f != NULL)
        fichiers_op (OP_CORBEILLE, f, NULL, GTK_WINDOW (F.fenetre), on_op_finie, NULL);
}

static void
on_confirme_suppression (GObject *src, GAsyncResult *res, gpointer data)
{
    g_autoptr(GError) e = NULL;
    GList *fichiers = data;

    int choix = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (src), res, &e);

    if (choix == 1)      /* « Supprimer » est le second bouton */
        fichiers_op (OP_SUPPRIMER, fichiers, NULL, GTK_WINDOW (F.fenetre),
                     on_op_finie, NULL);

    g_list_free_full (fichiers, g_object_unref);
}

static void
act_supprimer (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    GList *f = choisis_fichiers ();
    if (f == NULL)
        return;

    /* Une suppression definitive se confirme. La corbeille, non : elle se
     * defait. */
    guint n = g_list_length (f);
    g_autofree char *q = g_strdup_printf (
        "Supprimer définitivement %u élément%s ?", n, n > 1 ? "s" : "");

    GtkAlertDialog *dlg = gtk_alert_dialog_new ("%s", q);
    gtk_alert_dialog_set_detail (dlg, "Cette action est irréversible.");
    gtk_alert_dialog_set_buttons (dlg, (const char *[]) { "Annuler", "Supprimer", NULL });
    gtk_alert_dialog_set_cancel_button (dlg, 0);
    gtk_alert_dialog_set_default_button (dlg, 0);
    gtk_alert_dialog_choose (dlg, GTK_WINDOW (F.fenetre), NULL,
                             on_confirme_suppression, f);
    g_object_unref (dlg);
}

/* --- petite boite de saisie, pour renommer et creer ----------------------- */
typedef void (*SaisieFunc) (const char *texte, gpointer data);

typedef struct {
    GtkWidget  *fenetre;
    GtkWidget  *champ;
    SaisieFunc  valide;
    gpointer    data;
} Saisie;

static void
saisie_valider (GtkWidget *w, gpointer data)
{
    Saisie *s = data;
    (void) w;

    const char *t = gtk_editable_get_text (GTK_EDITABLE (s->champ));
    if (t != NULL && *t != '\0')
        s->valide (t, s->data);

    gtk_window_destroy (GTK_WINDOW (s->fenetre));
}

static void
saisie_fermee (GtkWidget *w, gpointer data)
{
    (void) w;
    g_free (data);
}

static void
demander (const char *titre, const char *depart, SaisieFunc valide, gpointer data)
{
    Saisie *s = g_new0 (Saisie, 1);
    s->valide = valide;
    s->data   = data;

    s->champ = gtk_entry_new ();
    gtk_entry_set_activates_default (GTK_ENTRY (s->champ), TRUE);
    if (depart != NULL)
        gtk_editable_set_text (GTK_EDITABLE (s->champ), depart);

    GtkWidget *ok = gtk_button_new_with_label ("Valider");
    gtk_widget_add_css_class (ok, "suggested-action");
    g_signal_connect (ok, "clicked", G_CALLBACK (saisie_valider), s);

    GtkWidget *annuler = gtk_button_new_with_label ("Annuler");

    GtkWidget *boutons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign (boutons, GTK_ALIGN_END);
    gtk_box_append (GTK_BOX (boutons), annuler);
    gtk_box_append (GTK_BOX (boutons), ok);

    GtkWidget *pile = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class (pile, "fichiers-saisie");
    gtk_box_append (GTK_BOX (pile), s->champ);
    gtk_box_append (GTK_BOX (pile), boutons);

    s->fenetre = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (s->fenetre), titre);
    gtk_window_set_modal (GTK_WINDOW (s->fenetre), TRUE);
    gtk_window_set_resizable (GTK_WINDOW (s->fenetre), FALSE);
    gtk_window_set_transient_for (GTK_WINDOW (s->fenetre), GTK_WINDOW (F.fenetre));
    gtk_window_set_default_size (GTK_WINDOW (s->fenetre), 380, -1);
    gtk_widget_add_css_class (s->fenetre, "shell");
    gtk_widget_add_css_class (s->fenetre, "fichiers-dialogue");
    gtk_window_set_child (GTK_WINDOW (s->fenetre), pile);

    g_signal_connect_swapped (annuler, "clicked",
                              G_CALLBACK (gtk_window_destroy), s->fenetre);
    g_signal_connect (s->fenetre, "destroy", G_CALLBACK (saisie_fermee), s);
    g_signal_connect (s->champ, "activate", G_CALLBACK (saisie_valider), s);

    gtk_window_present (GTK_WINDOW (s->fenetre));
    gtk_widget_grab_focus (s->champ);
}

static void
faire_dossier (const char *nom, gpointer data)
{
    (void) data;
    g_autofree char *libre = fichiers_nom_libre (F.dossier, nom);
    g_autoptr(GFile) f = g_file_get_child (F.dossier, libre);
    g_autoptr(GError) e = NULL;

    if (!g_file_make_directory (f, NULL, &e))
        signaler ("Impossible de créer ce dossier", e->message);
    recharger ();
}

static void
act_nouveau (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    demander ("Nouveau dossier", "Nouveau dossier", faire_dossier, NULL);
}

static void
faire_renommer (const char *nom, gpointer data)
{
    g_autoptr(GFile) src = data;
    g_autoptr(GError) e = NULL;

    /* set_display_name renvoie le nouveau GFile ; on n'en fait rien, la
     * relecture du dossier suffit. */
    g_autoptr(GFile) nouveau = g_file_set_display_name (src, nom, NULL, &e);
    if (nouveau == NULL)
        signaler ("Impossible de renommer", e->message);
    recharger ();
}

static void
act_renommer (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    g_autoptr(FichierItem) it = premier_choisi ();
    if (it == NULL)
        return;
    demander ("Renommer", it->nom, faire_renommer, g_object_ref (it->file));
}

static void
act_tout (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    gtk_selection_model_select_all (F.selection);
}

static void
act_caches (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    F.montrer_caches = !F.montrer_caches;
    refiltrer ();
}

static void act_recharger (GSimpleAction *a, GVariant *p, gpointer d)
{ (void) a; (void) p; (void) d; recharger (); }

static void act_precedent (GSimpleAction *a, GVariant *p, gpointer d)
{ (void) a; (void) p; (void) d; aller_a (F.position - 1); }

static void act_suivant (GSimpleAction *a, GVariant *p, gpointer d)
{ (void) a; (void) p; (void) d; aller_a (F.position + 1); }

static void
act_parent (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    g_autoptr(GFile) up = g_file_get_parent (F.dossier);
    if (up != NULL)
        naviguer (up, TRUE);
}

static void
act_ouvrir (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    g_autoptr(FichierItem) it = premier_choisi ();
    ouvrir_item (it);
}

static void
act_favori (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    g_autoptr(FichierItem) it = premier_choisi ();
    GFile *cible = (it != NULL && it->dossier) ? it->file : F.dossier;

    if (fichiers_lieux_est_favori (F.lieux, cible))
        fichiers_lieux_retirer (F.lieux, cible);
    else
        fichiers_lieux_ajouter (F.lieux, cible);
}

/* --- proprietes ----------------------------------------------------------- */
static void
ligne_prop (GtkWidget *grille, int rang, const char *cle, const char *valeur)
{
    GtkWidget *k = gtk_label_new (cle);
    gtk_widget_add_css_class (k, "prop-cle");
    gtk_widget_set_halign (k, GTK_ALIGN_END);

    GtkWidget *v = gtk_label_new (valeur);
    gtk_widget_add_css_class (v, "prop-valeur");
    gtk_widget_set_halign (v, GTK_ALIGN_START);
    gtk_label_set_selectable (GTK_LABEL (v), TRUE);
    gtk_label_set_wrap (GTK_LABEL (v), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (v), 40);

    gtk_grid_attach (GTK_GRID (grille), k, 0, rang, 1, 1);
    gtk_grid_attach (GTK_GRID (grille), v, 1, rang, 1, 1);
}

static void
act_proprietes (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    g_autoptr(FichierItem) it = premier_choisi ();
    if (it == NULL)
        return;

    GtkWidget *grille = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grille), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grille), 16);
    gtk_widget_add_css_class (grille, "fichiers-proprietes");

    g_autofree char *taille = fichier_item_taille_texte (it);
    g_autofree char *date   = fichier_item_date_texte (it);
    g_autofree char *ou     = g_file_get_path (F.dossier);

    int r = 0;
    ligne_prop (grille, r++, "Nom",           it->nom);
    ligne_prop (grille, r++, "Type",          it->type_texte);
    ligne_prop (grille, r++, "Emplacement",   ou ? ou : "");
    if (!it->dossier)
        ligne_prop (grille, r++, "Taille",    taille);
    ligne_prop (grille, r++, "Modifié le",    date);
    if (it->lien)
        ligne_prop (grille, r++, "Nature",    "Lien symbolique");

    /* Les droits, lus a la demande : ce sont trois appels systeme de plus,
     * qui n'ont pas leur place dans l'enumeration d'un dossier entier. */
    g_autoptr(GFileInfo) info = g_file_query_info (
        it->file,
        G_FILE_ATTRIBUTE_ACCESS_CAN_READ ","
        G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE ","
        G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (info != NULL) {
        GString *droits = g_string_new (NULL);
        if (g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ))
            g_string_append (droits, "lecture ");
        if (g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
            g_string_append (droits, "écriture ");
        if (g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))
            g_string_append (droits, "exécution");
        ligne_prop (grille, r++, "Droits", droits->str);
        g_string_free (droits, TRUE);
    }

    GtkWidget *w = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (w), "Propriétés");
    gtk_window_set_transient_for (GTK_WINDOW (w), GTK_WINDOW (F.fenetre));
    gtk_window_set_resizable (GTK_WINDOW (w), FALSE);
    gtk_widget_add_css_class (w, "shell");
    gtk_widget_add_css_class (w, "fichiers-dialogue");
    gtk_window_set_child (GTK_WINDOW (w), grille);
    gtk_window_present (GTK_WINDOW (w));
}

/* --- tri, depuis le menu -------------------------------------------------- */
static void
act_trier (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) d;
    const char *quoi = g_variant_get_string (p, NULL);

    GtkColumnViewColumn *c =
        g_strcmp0 (quoi, "date")   == 0 ? F.col_date
      : g_strcmp0 (quoi, "type")   == 0 ? F.col_type
      : g_strcmp0 (quoi, "taille") == 0 ? F.col_taille
                                        : F.col_nom;

    gtk_column_view_sort_by_column (F.colonnes, c, GTK_SORT_ASCENDING);
}

/* Action A ETAT, et pas seulement a parametre : c'est l'etat qui fait
 * apparaitre enfonce le bouton de la vue courante. Sans lui, les trois
 * segments resteraient au meme niveau, et rien ne dirait ce qu'on regarde. */
static void
act_vue (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) d;
    g_simple_action_set_state (a, g_variant_ref (p));
    gtk_stack_set_visible_child_name (GTK_STACK (F.pile),
                                      g_variant_get_string (p, NULL));
}

static const GActionEntry actions[] = {
    { "ouvrir",     act_ouvrir,     NULL, NULL, NULL, { 0 } },
    { "copier",     act_copier,     NULL, NULL, NULL, { 0 } },
    { "couper",     act_couper,     NULL, NULL, NULL, { 0 } },
    { "coller",     act_coller,     NULL, NULL, NULL, { 0 } },
    { "corbeille",  act_corbeille,  NULL, NULL, NULL, { 0 } },
    { "supprimer",  act_supprimer,  NULL, NULL, NULL, { 0 } },
    { "renommer",   act_renommer,   NULL, NULL, NULL, { 0 } },
    { "nouveau",    act_nouveau,    NULL, NULL, NULL, { 0 } },
    { "proprietes", act_proprietes, NULL, NULL, NULL, { 0 } },
    { "favori",     act_favori,     NULL, NULL, NULL, { 0 } },
    { "tout",       act_tout,       NULL, NULL, NULL, { 0 } },
    { "caches",     act_caches,     NULL, NULL, NULL, { 0 } },
    { "recharger",  act_recharger,  NULL, NULL, NULL, { 0 } },
    { "precedent",  act_precedent,  NULL, NULL, NULL, { 0 } },
    { "suivant",    act_suivant,    NULL, NULL, NULL, { 0 } },
    { "parent",     act_parent,     NULL, NULL, NULL, { 0 } },
    { "trier",      act_trier,      "s",  NULL, NULL, { 0 } },
    { "vue",        act_vue,        "s",  "'icones'", NULL, { 0 } },
};

/* -------------------------------------------------------------------------
 * Menus contextuels
 * ------------------------------------------------------------------------- */
static void
montrer_menu (GMenu *modele, GtkWidget *ancre, double x, double y)
{
    gtk_popover_menu_set_menu_model (GTK_POPOVER_MENU (F.menu),
                                     G_MENU_MODEL (modele));

    graphene_point_t pt;
    if (!gtk_widget_compute_point (ancre, gtk_widget_get_parent (F.menu),
                                   &GRAPHENE_POINT_INIT ((float) x, (float) y), &pt))
        return;

    gtk_popover_set_pointing_to (GTK_POPOVER (F.menu),
                                 &(GdkRectangle) { (int) pt.x, (int) pt.y, 1, 1 });
    gtk_popover_popup (GTK_POPOVER (F.menu));
}

static void
on_clic_item (GtkGestureClick *g, int n, double x, double y, gpointer data)
{
    GtkWidget *w = data;
    (void) n;

    FichierItem *it = item_de (w);
    GtkListItem *li = g_object_get_data (G_OBJECT (w), "list-item");
    if (it == NULL || li == NULL)
        return;

    /* On PREND la sequence. Sans cela, le geste du fond -- pose sur la pile,
     * donc en amont -- se declenchait juste apres et remplacait le menu de
     * l'element par le sien : le clic droit sur un fichier proposait
     * « Nouveau dossier ». Un GtkGestureClick ne bloque pas la propagation
     * de lui-meme, il faut la reclamer. Constate au banc d'essai. */
    gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);

    /* Le clic droit sur un element HORS selection selectionne celui-la seul.
     * Sans cela, « Supprimer » agirait sur ce qui etait selectionne ailleurs,
     * et pas sur l'element vise -- une erreur silencieuse et couteuse. */
    guint pos = gtk_list_item_get_position (li);
    if (!gtk_selection_model_is_selected (F.selection, pos))
        gtk_selection_model_select_item (F.selection, pos, TRUE);

    g_autoptr(GMenu) menu = g_menu_new ();
    g_autoptr(GMenu) s1 = g_menu_new ();
    g_menu_append (s1, "Ouvrir", "fichiers.ouvrir");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (s1));

    g_autoptr(GMenu) s2 = g_menu_new ();
    g_menu_append (s2, "Couper", "fichiers.couper");
    g_menu_append (s2, "Copier", "fichiers.copier");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (s2));

    g_autoptr(GMenu) s3 = g_menu_new ();
    g_menu_append (s3, "Renommer…", "fichiers.renommer");
    g_menu_append (s3, "Mettre à la corbeille", "fichiers.corbeille");
    g_menu_append (s3, "Supprimer définitivement…", "fichiers.supprimer");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (s3));

    g_autoptr(GMenu) s4 = g_menu_new ();
    if (it->dossier)
        g_menu_append (s4, fichiers_lieux_est_favori (F.lieux, it->file)
                           ? "Retirer des favoris" : "Ajouter aux favoris",
                       "fichiers.favori");
    g_menu_append (s4, "Propriétés", "fichiers.proprietes");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (s4));

    montrer_menu (menu, w, x, y);
}

static void
on_clic_fond (GtkGestureClick *g, int n, double x, double y, gpointer data)
{
    GtkWidget *w = data;
    (void) g; (void) n;

    g_autoptr(GMenu) menu = g_menu_new ();
    g_autoptr(GMenu) s1 = g_menu_new ();
    g_menu_append (s1, "Nouveau dossier…", "fichiers.nouveau");
    if (F.presse->len > 0)
        g_menu_append (s1, "Coller", "fichiers.coller");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (s1));

    g_autoptr(GMenu) s2 = g_menu_new ();
    g_menu_append (s2, "Trier par nom",    "fichiers.trier::nom");
    g_menu_append (s2, "Trier par date",   "fichiers.trier::date");
    g_menu_append (s2, "Trier par type",   "fichiers.trier::type");
    g_menu_append (s2, "Trier par taille", "fichiers.trier::taille");
    g_menu_append_section (menu, "Affichage", G_MENU_MODEL (s2));

    g_autoptr(GMenu) s3 = g_menu_new ();
    g_menu_append (s3, F.montrer_caches ? "Masquer les fichiers cachés"
                                        : "Afficher les fichiers cachés",
                   "fichiers.caches");
    g_menu_append (s3, fichiers_lieux_est_favori (F.lieux, F.dossier)
                       ? "Retirer ce dossier des favoris"
                       : "Ajouter ce dossier aux favoris", "fichiers.favori");
    g_menu_append (s3, "Actualiser", "fichiers.recharger");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (s3));

    montrer_menu (menu, w, x, y);
}

/* -------------------------------------------------------------------------
 * Glisser-deposer
 * ------------------------------------------------------------------------- */
static GdkContentProvider *
on_item_drag (GtkDragSource *s, double x, double y, gpointer data)
{
    GtkWidget *w = data;
    (void) s; (void) x; (void) y;

    FichierItem *it = item_de (w);
    if (it == NULL)
        return NULL;

    /* Toute la selection part, pas seulement l'element saisi : c'est ce
     * qu'on attend quand on a pris la peine d'en selectionner plusieurs. */
    GtkListItem *li = g_object_get_data (G_OBJECT (w), "list-item");
    if (li != NULL && !gtk_selection_model_is_selected (F.selection,
                                                        gtk_list_item_get_position (li)))
        gtk_selection_model_select_item (F.selection,
                                         gtk_list_item_get_position (li), TRUE);

    g_autolist(GObject) fichiers = choisis_fichiers ();
    if (fichiers == NULL)
        return NULL;

    /* GdkFileList veut une GSList ; le reste du fichier travaille en GList,
     * qui se prete mieux au parcours. La conversion tient en trois lignes. */
    GSList *simple = NULL;
    for (GList *l = fichiers; l != NULL; l = l->next)
        simple = g_slist_prepend (simple, l->data);
    simple = g_slist_reverse (simple);

    /* GDK_TYPE_FILE_LIST est le type que comprennent les autres programmes :
     * deposer dans Chromium ou dans un editeur marche donc aussi. */
    GdkFileList *liste = gdk_file_list_new_from_list (simple);
    g_slist_free (simple);

    /* new_typed copie la valeur boxee : la notre est a nous de liberer.
     * GdkFileList n'expose pas de fonction dediee, on passe donc par la
     * liberation generique des types boxes. */
    GdkContentProvider *p = gdk_content_provider_new_typed (GDK_TYPE_FILE_LIST, liste);
    g_boxed_free (GDK_TYPE_FILE_LIST, liste);
    return p;
}

/* Deplace vers `cible`. Deposer un dossier dans lui-meme n'a pas de sens et
 * detruirait l'arborescence : on refuse. */
static gboolean
deposer_vers (const GValue *v, GFile *cible)
{
    if (!G_VALUE_HOLDS (v, GDK_TYPE_FILE_LIST) || cible == NULL)
        return FALSE;

    GSList *fichiers = gdk_file_list_get_files (g_value_get_boxed (v));
    GList  *sources = NULL;

    for (GSList *l = fichiers; l != NULL; l = l->next) {
        GFile *f = l->data;
        g_autoptr(GFile) parent = g_file_get_parent (f);

        if (g_file_equal (f, cible) || g_file_has_prefix (cible, f))
            continue;                       /* dans lui-meme : refuse */
        if (parent != NULL && g_file_equal (parent, cible))
            continue;                       /* deja la : rien a faire */

        sources = g_list_prepend (sources, f);
    }
    g_slist_free (fichiers);

    if (sources == NULL)
        return FALSE;

    sources = g_list_reverse (sources);
    fichiers_op (OP_DEPLACER, sources, cible, GTK_WINDOW (F.fenetre),
                 on_op_finie, NULL);
    g_list_free (sources);
    return TRUE;
}

static gboolean
on_item_drop (GtkDropTarget *t, const GValue *v, double x, double y, gpointer data)
{
    GtkWidget *w = data;
    (void) t; (void) x; (void) y;

    FichierItem *it = item_de (w);
    /* Sur un fichier, on refuse : l'evenement remonte a la vue, qui range
     * dans le dossier courant. C'est le comportement de Windows. */
    if (it == NULL || !it->dossier)
        return FALSE;

    return deposer_vers (v, it->file);
}

static gboolean
on_vue_drop (GtkDropTarget *t, const GValue *v, double x, double y, gpointer data)
{
    (void) t; (void) x; (void) y; (void) data;
    return deposer_vers (v, F.dossier);
}

/* -------------------------------------------------------------------------
 * Barre d'adresse
 * ------------------------------------------------------------------------- */
static void
on_adresse_validee (GtkEntry *e, gpointer data)
{
    (void) data;
    const char *t = gtk_editable_get_text (GTK_EDITABLE (e));
    g_autoptr(GFile) f = g_file_parse_name (t);

    gtk_stack_set_visible_child_name (GTK_STACK (F.pile_adresse), "fil");
    naviguer (f, TRUE);
}

static void
act_adresse (GtkWidget *w, gpointer data)
{
    (void) w; (void) data;

    if (g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (F.pile_adresse)),
                   "champ") == 0) {
        gtk_stack_set_visible_child_name (GTK_STACK (F.pile_adresse), "fil");
        return;
    }

    g_autofree char *chemin = g_file_get_parse_name (F.dossier);
    gtk_editable_set_text (GTK_EDITABLE (F.adresse), chemin);
    gtk_stack_set_visible_child_name (GTK_STACK (F.pile_adresse), "champ");
    gtk_widget_grab_focus (F.adresse);
}

/* -------------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------------- */
/* Repli explicite quand le theme d'icones ne fournit pas le pictogramme.
 *
 * GTK embarque environ 270 icones et sait toujours dessiner celles-la ; tout
 * le reste depend du theme installe, et d'un chargeur SVG present pour le
 * lire. A defaut, GTK affiche une page barree d'un triangle -- constate au
 * banc d'essai sur « document-edit-symbolic ». Mieux vaut une icone voisine
 * qu'un avertissement. */
static GtkWidget *
bouton_barre (const char *icone, const char *infobulle, const char *action)
{
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
    GtkWidget *b = gtk_button_new_from_icon_name (
        gtk_icon_theme_has_icon (theme, icone) ? icone : "go-jump-symbolic");
    gtk_widget_add_css_class (b, "fichiers-outil");
    gtk_widget_set_tooltip_text (b, infobulle);
    if (action != NULL)
        gtk_actionable_set_action_name (GTK_ACTIONABLE (b), action);
    return b;
}

static GtkWidget *
bouton_texte (const char *libelle, const char *action)
{
    GtkWidget *b = gtk_button_new_with_label (libelle);
    gtk_widget_add_css_class (b, "fichiers-action");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (b), action);
    return b;
}

static void
raccourci (GtkEventController *ctrl, const char *touches, const char *action)
{
    GtkShortcut *s = gtk_shortcut_new (gtk_shortcut_trigger_parse_string (touches),
                                       gtk_named_action_new (action));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (ctrl), s);
}

/* Ctrl+L bascule la barre d'adresse, Echap revient au fil d'Ariane et vide
 * la recherche. Ni l'un ni l'autre n'est une action du groupe : ils ne
 * touchent qu'a l'affichage, et n'ont donc rien a faire dans un menu. */
static gboolean
on_touche (GtkEventControllerKey *c, guint touche, guint code,
           GdkModifierType mods, gpointer data)
{
    (void) c; (void) code; (void) data;

    if (touche == GDK_KEY_l && (mods & GDK_CONTROL_MASK)) {
        act_adresse (NULL, NULL);
        return GDK_EVENT_STOP;
    }
    if (touche == GDK_KEY_Escape) {
        gtk_stack_set_visible_child_name (GTK_STACK (F.pile_adresse), "fil");
        gtk_editable_set_text (GTK_EDITABLE (F.recherche), "");
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static void
on_config_reloaded (ShellConfig *cfg, gpointer data)
{
    (void) data;
    shell_styles_load (cfg->theme);
    shell_config_apply (cfg);
    g_object_set (gtk_settings_get_default (),
                  "gtk-application-prefer-dark-theme", cfg->dark, NULL);
    shell_config_free (cfg);
}

static void
on_activate (GtkApplication *app, gpointer user_data)
{
    ShellConfig *cfg = user_data;

    shell_config_apply (cfg);
    /* Les widgets GTK ordinaires -- champs, en-tetes de colonnes, boites de
     * dialogue -- ne sont pas redessines par notre feuille de style. Sans
     * cela ils resteraient clairs dans une fenetre sombre. */
    g_object_set (gtk_settings_get_default (),
                  "gtk-application-prefer-dark-theme", cfg->dark, NULL);

    F.histoire = g_ptr_array_new_with_free_func (g_object_unref);
    F.presse   = g_ptr_array_new_with_free_func (g_object_unref);
    F.position = -1;

    F.fenetre = gtk_application_window_new (app);
    gtk_widget_add_css_class (F.fenetre, "shell");
    gtk_widget_add_css_class (F.fenetre, "fichiers");
    gtk_window_set_title (GTK_WINDOW (F.fenetre), "Fichiers");
    gtk_window_set_default_size (GTK_WINDOW (F.fenetre), 1040, 660);

    /* --- le modele, partage par les trois vues --- */
    F.magasin = g_list_store_new (FICHIERS_TYPE_ITEM);
    F.filtre  = GTK_FILTER (gtk_custom_filter_new (retenu, NULL, NULL));
    F.modele_filtre = gtk_filter_list_model_new (
        G_LIST_MODEL (g_object_ref (F.magasin)), g_object_ref (F.filtre));
    F.modele_tri = gtk_sort_list_model_new (
        G_LIST_MODEL (F.modele_filtre), NULL);
    F.selection = GTK_SELECTION_MODEL (
        gtk_multi_selection_new (G_LIST_MODEL (F.modele_tri)));

    g_signal_connect (F.selection, "selection-changed",
                      G_CALLBACK (on_selection_changed), NULL);
    g_signal_connect (F.selection, "items-changed",
                      G_CALLBACK (on_items_changed), NULL);

    /* --- vue Details, construite en premier : son trieur sert a tous --- */
    F.colonnes = GTK_COLUMN_VIEW (gtk_column_view_new (NULL));
    gtk_column_view_set_show_row_separators (F.colonnes, FALSE);
    gtk_widget_add_css_class (GTK_WIDGET (F.colonnes), "fichiers-colonnes");

    F.col_nom = colonne ("Nom",
        fabrique (G_CALLBACK (colonne_nom_setup), G_CALLBACK (case_bind), NULL),
        GTK_SORTER (gtk_custom_sorter_new (cmp_nom, NULL, NULL)), -1);
    F.col_date = colonne ("Modifié le",
        fabrique (G_CALLBACK (texte_setup), G_CALLBACK (texte_bind),
                  fichier_item_date_texte),
        GTK_SORTER (gtk_custom_sorter_new (cmp_date, NULL, NULL)), 150);
    F.col_type = colonne ("Type",
        fabrique (G_CALLBACK (texte_setup), G_CALLBACK (texte_bind), type_de),
        GTK_SORTER (gtk_custom_sorter_new (cmp_type, NULL, NULL)), 170);
    F.col_taille = colonne ("Taille",
        fabrique (G_CALLBACK (texte_setup), G_CALLBACK (texte_bind),
                  fichier_item_taille_texte),
        GTK_SORTER (gtk_custom_sorter_new (cmp_taille, NULL, NULL)), 100);

    /* Le trieur de la vue Details est LE trieur : cliquer un en-tete
     * reordonne aussi les vues Icones et Liste, et le menu « Trier par »
     * n'est qu'une autre facon d'agir sur les memes colonnes. Une seule
     * source de verite, aucun etat a synchroniser. */
    gtk_sort_list_model_set_sorter (F.modele_tri,
                                    gtk_column_view_get_sorter (F.colonnes));
    gtk_column_view_sort_by_column (F.colonnes, F.col_nom, GTK_SORT_ASCENDING);
    gtk_column_view_set_model (F.colonnes, F.selection);

    /* --- vues Icones et Liste --- */
    GtkWidget *grille = gtk_grid_view_new (
        g_object_ref (F.selection),
        fabrique (G_CALLBACK (grille_setup), G_CALLBACK (case_bind), NULL));
    gtk_grid_view_set_max_columns (GTK_GRID_VIEW (grille), 12);
    gtk_grid_view_set_min_columns (GTK_GRID_VIEW (grille), 3);
    gtk_widget_add_css_class (grille, "fichiers-grille");

    GtkWidget *liste = gtk_list_view_new (
        g_object_ref (F.selection),
        fabrique (G_CALLBACK (liste_setup), G_CALLBACK (case_bind), NULL));
    gtk_widget_add_css_class (liste, "fichiers-liste");

    g_signal_connect (grille, "activate", G_CALLBACK (on_active), NULL);
    g_signal_connect (liste,  "activate", G_CALLBACK (on_active), NULL);
    g_signal_connect (F.colonnes, "activate", G_CALLBACK (on_active), NULL);

    GtkWidget *d_grille = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (d_grille), grille);
    GtkWidget *d_liste = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (d_liste), liste);
    GtkWidget *d_col = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (d_col), GTK_WIDGET (F.colonnes));

    F.pile = gtk_stack_new ();
    gtk_stack_add_named (GTK_STACK (F.pile), d_grille, "icones");
    gtk_stack_add_named (GTK_STACK (F.pile), d_liste,  "liste");
    gtk_stack_add_named (GTK_STACK (F.pile), d_col,    "details");
    gtk_widget_set_hexpand (F.pile, TRUE);
    gtk_widget_set_vexpand (F.pile, TRUE);

    /* Clic droit sur le fond, et depot dans le dossier courant : poses sur
     * la pile, ils valent pour les trois vues d'un coup. */
    GtkGestureClick *fond = GTK_GESTURE_CLICK (gtk_gesture_click_new ());
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (fond), GDK_BUTTON_SECONDARY);
    g_signal_connect (fond, "pressed", G_CALLBACK (on_clic_fond), F.pile);
    gtk_widget_add_controller (F.pile, GTK_EVENT_CONTROLLER (fond));

    GtkDropTarget *depot = gtk_drop_target_new (GDK_TYPE_FILE_LIST,
                                                GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect (depot, "drop", G_CALLBACK (on_vue_drop), NULL);
    gtk_widget_add_controller (F.pile, GTK_EVENT_CONTROLLER (depot));

    /* --- barre de navigation --- */
    F.precedent = bouton_barre ("go-previous-symbolic", "Précédent",
                                "fichiers.precedent");
    F.suivant   = bouton_barre ("go-next-symbolic", "Suivant", "fichiers.suivant");
    F.parent    = bouton_barre ("go-up-symbolic", "Dossier parent",
                                "fichiers.parent");
    GtkWidget *actualiser = bouton_barre ("view-refresh-symbolic", "Actualiser",
                                          "fichiers.recharger");

    F.fil = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class (F.fil, "fil");
    F.fil_defil = gtk_scrolled_window_new ();
    /* EXTERNAL plutot qu'AUTOMATIC : une barre de defilement sous le fil
     * d'Ariane le ferait sauter de quelques pixels selon la longueur du
     * chemin. On defile a la molette et au calage automatique. */
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (F.fil_defil),
                                    GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (F.fil_defil), F.fil);

    F.adresse = gtk_entry_new ();
    gtk_widget_add_css_class (F.adresse, "fichiers-adresse");
    g_signal_connect (F.adresse, "activate", G_CALLBACK (on_adresse_validee), NULL);

    F.pile_adresse = gtk_stack_new ();
    gtk_stack_add_named (GTK_STACK (F.pile_adresse), F.fil_defil, "fil");
    gtk_stack_add_named (GTK_STACK (F.pile_adresse), F.adresse, "champ");
    gtk_widget_set_hexpand (F.pile_adresse, TRUE);
    gtk_widget_add_css_class (F.pile_adresse, "fichiers-barre-adresse");

    GtkWidget *editer = bouton_barre ("document-edit-symbolic",
                                      "Saisir un chemin (Ctrl+L)", NULL);
    g_signal_connect (editer, "clicked", G_CALLBACK (act_adresse), NULL);

    F.recherche = gtk_search_entry_new ();
    gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (F.recherche),
                                           "Rechercher ici");
    gtk_widget_add_css_class (F.recherche, "fichiers-recherche");
    gtk_widget_set_size_request (F.recherche, 200, -1);
    g_signal_connect (F.recherche, "search-changed", G_CALLBACK (on_recherche), NULL);

    GtkWidget *vues = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (vues, "fichiers-vues");
    const struct { const char *nom, *cible; } v[] = {
        { "Icônes", "icones" }, { "Liste", "liste" }, { "Détails", "details" }
    };
    for (guint i = 0; i < G_N_ELEMENTS (v); i++) {
        GtkWidget *b = gtk_toggle_button_new_with_label (v[i].nom);
        gtk_widget_add_css_class (b, "fichiers-vue");
        gtk_actionable_set_action_name (GTK_ACTIONABLE (b), "fichiers.vue");
        gtk_actionable_set_action_target (GTK_ACTIONABLE (b), "s", v[i].cible);
        gtk_box_append (GTK_BOX (vues), b);
    }

    GtkWidget *barre = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class (barre, "fichiers-barre");
    gtk_box_append (GTK_BOX (barre), F.precedent);
    gtk_box_append (GTK_BOX (barre), F.suivant);
    gtk_box_append (GTK_BOX (barre), F.parent);
    gtk_box_append (GTK_BOX (barre), actualiser);
    gtk_box_append (GTK_BOX (barre), F.pile_adresse);
    gtk_box_append (GTK_BOX (barre), editer);
    gtk_box_append (GTK_BOX (barre), F.recherche);
    gtk_box_append (GTK_BOX (barre), vues);

    /* --- barre d'actions --- */
    GtkWidget *actions_barre = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class (actions_barre, "fichiers-actions");
    gtk_box_append (GTK_BOX (actions_barre),
                    bouton_texte ("Nouveau dossier", "fichiers.nouveau"));
    gtk_box_append (GTK_BOX (actions_barre), bouton_texte ("Copier", "fichiers.copier"));
    gtk_box_append (GTK_BOX (actions_barre), bouton_texte ("Couper", "fichiers.couper"));
    gtk_box_append (GTK_BOX (actions_barre), bouton_texte ("Coller", "fichiers.coller"));
    gtk_box_append (GTK_BOX (actions_barre), bouton_texte ("Renommer", "fichiers.renommer"));
    gtk_box_append (GTK_BOX (actions_barre),
                    bouton_texte ("Corbeille", "fichiers.corbeille"));

    /* --- assemblage --- */
    F.lieux = fichiers_lieux_new (on_nav_lieux, NULL);

    GtkWidget *volets = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child (GTK_PANED (volets), F.lieux);
    gtk_paned_set_end_child (GTK_PANED (volets), F.pile);
    gtk_paned_set_position (GTK_PANED (volets), 190);
    gtk_paned_set_shrink_start_child (GTK_PANED (volets), FALSE);
    gtk_paned_set_resize_start_child (GTK_PANED (volets), FALSE);

    F.etat = gtk_label_new ("");
    gtk_widget_add_css_class (F.etat, "fichiers-etat");
    /* FILL et xalign 0, et non halign START : le fond de la barre doit
     * courir sur toute la largeur, seul le texte est cale a gauche. */
    gtk_widget_set_halign (F.etat, GTK_ALIGN_FILL);
    gtk_label_set_xalign (GTK_LABEL (F.etat), 0.0);

    GtkWidget *pile = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (pile), barre);
    gtk_box_append (GTK_BOX (pile), actions_barre);
    gtk_box_append (GTK_BOX (pile), volets);
    gtk_box_append (GTK_BOX (pile), F.etat);
    gtk_window_set_child (GTK_WINDOW (F.fenetre), pile);

    /* Le menu contextuel est parente a la boite racine, et NON a la pile des
     * vues : un GtkStack n'alloue que son enfant visible, et le popover qu'on
     * lui attachait se voyait donner une hauteur trop courte -- son dernier
     * element manquait, « Propriétés » en l'occurrence. Le defaut ne se
     * voyait qu'a partir de huit entrees ; il a fallu compter les lignes
     * d'une capture pour le trouver. */
    F.menu = gtk_popover_menu_new_from_model (NULL);
    gtk_widget_add_css_class (F.menu, "fichiers-menu");
    gtk_popover_set_has_arrow (GTK_POPOVER (F.menu), FALSE);
    gtk_widget_set_parent (F.menu, pile);

    /* --- actions et raccourcis --- */
    GSimpleActionGroup *groupe = g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (groupe), actions,
                                     G_N_ELEMENTS (actions), NULL);
    gtk_widget_insert_action_group (F.fenetre, "fichiers", G_ACTION_GROUP (groupe));
    g_object_unref (groupe);

    GtkEventController *ctrl = gtk_shortcut_controller_new ();
    gtk_shortcut_controller_set_scope (GTK_SHORTCUT_CONTROLLER (ctrl),
                                       GTK_SHORTCUT_SCOPE_GLOBAL);
    raccourci (ctrl, "<Control>c",       "fichiers.copier");
    raccourci (ctrl, "<Control>x",       "fichiers.couper");
    raccourci (ctrl, "<Control>v",       "fichiers.coller");
    raccourci (ctrl, "<Control>a",       "fichiers.tout");
    raccourci (ctrl, "<Control>h",       "fichiers.caches");
    raccourci (ctrl, "<Control><Shift>n","fichiers.nouveau");
    raccourci (ctrl, "F2",               "fichiers.renommer");
    raccourci (ctrl, "F5",               "fichiers.recharger");
    raccourci (ctrl, "Delete",           "fichiers.corbeille");
    raccourci (ctrl, "<Shift>Delete",    "fichiers.supprimer");
    raccourci (ctrl, "<Alt>Left",        "fichiers.precedent");
    raccourci (ctrl, "<Alt>Right",       "fichiers.suivant");
    raccourci (ctrl, "<Alt>Up",          "fichiers.parent");
    raccourci (ctrl, "BackSpace",        "fichiers.parent");
    raccourci (ctrl, "<Alt>Return",      "fichiers.proprietes");
    gtk_widget_add_controller (F.fenetre, ctrl);

    /* Ctrl+L bascule la barre d'adresse : ce n'est pas une action du groupe,
     * elle ne touche qu'a l'affichage. */
    GtkEventControllerKey *k =
        GTK_EVENT_CONTROLLER_KEY (gtk_event_controller_key_new ());
    g_signal_connect (k, "key-pressed", G_CALLBACK (on_touche), NULL);
    gtk_widget_add_controller (F.fenetre, GTK_EVENT_CONTROLLER (k));

    gtk_window_present (GTK_WINDOW (F.fenetre));

    shell_config_watch (on_config_reloaded, NULL);

    /* On ouvre sur le dossier personnel. Un dossier passe en argument
     * arrive par le signal « open », qui navigue ensuite. */
    g_autoptr(GFile) accueil = g_file_new_for_path (g_get_home_dir ());
    naviguer (accueil, TRUE);
}

static void
on_open (GApplication *app, GFile **fichiers, int n, const char *hint,
         gpointer data)
{
    (void) hint; (void) data;

    /* « Ouvrir avec Fichiers » sur un dossier passe par ici. On active
     * d'abord la fenetre, puis on s'y rend : sans cela il n'y aurait rien
     * ou naviguer. */
    if (F.fenetre == NULL)
        g_application_activate (app);
    if (n > 0)
        naviguer (fichiers[0], TRUE);

    gtk_window_present (GTK_WINDOW (F.fenetre));
}

int
main (int argc, char **argv)
{
    ShellConfig *cfg = shell_config_load ();

    GtkApplication *app = gtk_application_new ("os.claude.shell.fichiers",
                                               G_APPLICATION_HANDLES_OPEN);
    g_signal_connect (app, "startup",  G_CALLBACK (shell_styles_startup), cfg);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), cfg);
    g_signal_connect (app, "open",     G_CALLBACK (on_open), NULL);

    int status = g_application_run (G_APPLICATION (app), argc, argv);
    g_object_unref (app);
    return status;
}
