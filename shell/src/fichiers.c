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
 * Les quatre vues sont VIRTUELLES : elles ne construisent des widgets que
 * pour ce qui est a l'ecran. Un repertoire de dix mille fichiers ne coute
 * donc que dix mille petits objets.
 * ========================================================================= */

#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>

#include "config.h"
#include "fichiers.h"
#include "fichiers-ops.h"
#include "fichiers-lieux.h"
#include "fichiers-apercu.h"
#include "reseau.h"

static struct {
    GtkWidget          *fenetre;
    GtkWidget          *pile;        /* les quatre vues                     */
    GtkWidget          *grille;      /* vue Icones                          */
    GtkWidget          *apercu;      /* vue Apercu                          */
    GtkWidget          *liste;       /* vue Liste                           */
    gboolean            tactile;     /* derniere saisie au doigt            */
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
    GtkColumnViewColumn *col_bourrage;

    GFile              *dossier;
    GPtrArray          *histoire;    /* GFile*, du plus ancien au plus recent */
    int                 position;    /* index courant dans histoire         */

    GPtrArray          *presse;      /* GFile* copies ou coupes             */
    gboolean            couper;
    gboolean            montrer_caches;
} F;

static void naviguer (GFile *dossier, gboolean historiser);
static void naviguer_vraiment (GFile *dossier, gboolean historiser);
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

        /* LE FIL S'ARRETE A LA RACINE D'UN LECTEUR RESEAU.
         *
         * Sans cela il afficherait « Ordinateur › run › claude-os › reseau ›
         * nas-videos » : quatre elements qui ne mènent nulle part d'utile et
         * qui étalent un detail d'implementation. On remonte donc jusqu'au
         * lecteur, et pas au-dela. */
        g_autofree char *p = g_file_get_path (f);
        g_autofree char *lecteur = reseau_nom_du_point (p);
        if (lecteur != NULL)
            break;

        f = g_file_get_parent (f);
    }

    for (guint i = 0; i < chaine->len; i++) {
        GFile *f = g_ptr_array_index (chaine, i);
        g_autofree char *nom = g_file_get_basename (f);

        /* « / » comme nom de bouton est illisible ; le dossier personnel se
         * nomme d'ailleurs par son role et non par son chemin. */
        const char *libelle = nom;
        g_autofree char *chemin = g_file_get_path (f);
        g_autofree char *nom_lecteur = reseau_nom_du_point (chemin);
        if (nom_lecteur != NULL)
            libelle = nom_lecteur;
        else if (g_strcmp0 (chemin, "/") == 0)
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
    /* Les vignettes en attente visent les elements qu'on va jeter : ceux
     * de la relecture sont neufs, et les redemanderont eux-memes. */
    fichiers_apercu_abandonner ();
    fichiers_lire (F.dossier, F.magasin, on_lu, NULL);
}

/* La verification d'existence, une fois qu'elle a repondu. */
static void
naviguer_vraiment (GFile *dossier, gboolean historiser)
{
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

/* -------------------------------------------------------------------------
 * L'existence du dossier se verifie SANS BLOQUER
 *
 * La version precedente appelait g_file_query_exists, qui est synchrone.
 * Sur un dossier local cela ne coute rien, et cela s'est donc vu passer.
 * Sur un lecteur reseau, non : un serveur eteint, un Wi-Fi coupe, et
 * l'appel ne rend la main qu'au bout du delai TCP -- pendant lequel la
 * fenetre entiere est gelee, boutons compris. C'est exactement le moment ou
 * l'utilisateur veut pouvoir revenir en arriere.
 *
 * Le controle est conserve -- il evite de perdre le dossier courant sur une
 * adresse fautive -- mais il est pose en asynchrone, et une demande plus
 * recente annule celle qui attendait encore.
 * ------------------------------------------------------------------------- */
static GCancellable *nav_en_cours = NULL;

typedef struct {
    GFile    *dossier;
    gboolean  historiser;
} DemandeNav;

static void
on_nav_verifiee (GObject *src, GAsyncResult *res, gpointer data)
{
    DemandeNav *d = data;
    g_autoptr(GError) e = NULL;
    g_autoptr(GFileInfo) info = g_file_query_info_finish (G_FILE (src), res, &e);

    if (info == NULL) {
        /* Annulee : une autre navigation a pris la place, et c'est elle qui
         * parlera. Se plaindre ici afficherait une erreur pour un dossier
         * que l'utilisateur a deja quitte. */
        if (!g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_autofree char *chemin = g_file_get_parse_name (d->dossier);
            signaler ("Ce dossier n'est pas accessible", 
                      e != NULL ? e->message : chemin);
        }
    } else {
        naviguer_vraiment (d->dossier, d->historiser);
    }

    g_object_unref (d->dossier);
    g_free (d);
}

static void
naviguer (GFile *dossier, gboolean historiser)
{
    if (dossier == NULL)
        return;

    g_cancellable_cancel (nav_en_cours);
    g_clear_object (&nav_en_cours);
    nav_en_cours = g_cancellable_new ();

    DemandeNav *d = g_new0 (DemandeNav, 1);
    d->dossier    = g_object_ref (dossier);
    d->historiser = historiser;

    g_file_query_info_async (dossier, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                             G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                             nav_en_cours, on_nav_verifiee, d);
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

/* UNE VUE QUI ETAIT EN HAUT Y RESTE.
 *
 * Les vues de GTK gardent « accroche » l'element du haut quand la liste
 * change. Or le dossier arrive dans l'ordre du disque et le tri range chaque
 * element a sa place : le premier recu -- « notes.txt », disons -- restait
 * accroche en haut, et les dossiers, tries avant lui, s'inseraient AU-DESSUS,
 * hors de vue. Un dossier s'ouvrait donc deja defile, et « Sous-dossier »
 * n'apparaissait qu'en remontant. Vu au banc d'essai, dans les trois vues
 * d'alors.
 *
 * Connecte APRES les vues (g_signal_connect_after) : elles ont deja pris
 * acte du changement, et l'ajustement, lui, n'a pas encore bouge -- la mise
 * en page vient plus tard. Sa valeur dit donc ou l'on etait AVANT. */
static void
garder_en_haut (GListModel *m, guint p, guint r, guint a, gpointer d)
{
    (void) p; (void) r; (void) a; (void) d;
    if (g_list_model_get_n_items (m) == 0)
        return;

    GtkWidget *vues[] = { F.grille, F.apercu, F.liste, GTK_WIDGET (F.colonnes) };
    for (guint i = 0; i < G_N_ELEMENTS (vues); i++) {
        if (vues[i] == NULL)
            continue;
        GtkWidget *defil = gtk_widget_get_ancestor (vues[i], GTK_TYPE_SCROLLED_WINDOW);
        if (defil == NULL)
            continue;
        GtkAdjustment *v =
            gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (defil));
        if (gtk_adjustment_get_value (v) > 0.0)
            continue;

        if (GTK_IS_GRID_VIEW (vues[i]))
            gtk_grid_view_scroll_to (GTK_GRID_VIEW (vues[i]), 0, GTK_LIST_SCROLL_NONE, NULL);
        else if (GTK_IS_LIST_VIEW (vues[i]))
            gtk_list_view_scroll_to (GTK_LIST_VIEW (vues[i]), 0, GTK_LIST_SCROLL_NONE, NULL);
        else
            gtk_column_view_scroll_to (GTK_COLUMN_VIEW (vues[i]), 0, NULL,
                                       GTK_LIST_SCROLL_NONE, NULL);
    }
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
static void on_appui_long_item (GtkGestureLongPress *g, double x, double y, gpointer d);
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

    /* Au doigt, l'appui simple OUVRE (voir on_saisie) : il faut donc une
     * autre facon de designer un element sans l'ouvrir. L'appui long le
     * selectionne et montre son menu -- le clic droit du doigt, comme sur
     * une tablette Windows ou un telephone. Tactile seulement : a la souris,
     * rester appuye sert a glisser, pas a ouvrir un menu. */
    GtkGesture *long_ = gtk_gesture_long_press_new ();
    gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (long_), TRUE);
    g_signal_connect (long_, "pressed", G_CALLBACK (on_appui_long_item), w);
    gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (long_));

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

/* --- grilles : Icones et Apercu ------------------------------------------- */

/* UNE CASE DE LARGEUR FIXE, et c'est elle qui porte la selection.
 *
 * Auparavant la case suivait son nom, et le surlignage etait celui de la
 * cellule de la grille. Or GtkGridView donne a TOUTES ses colonnes la
 * largeur minimale de la plus large : un seul nom sans espace
 * (« README.md.bash_completion.gz ») que le retour a la ligne par mots ne
 * savait pas couper imposait 280 px a chaque colonne, et la selection d'une
 * icone de 48 px s'etalait sur toute cette largeur -- trois colonnes dans
 * une fenetre qui en loge huit. Mesure au banc d'essai.
 *
 * Desormais : largeur fixe, coupe au caractere quand le mot ne tient pas,
 * deux lignes au plus, et le surlignage sur la case. La grille range alors
 * autant de colonnes que la fenetre en loge. */
static GtkWidget *
case_nouvelle (GtkListItem *li, int cote_image, int largeur, int car_max)
{
    GtkWidget *img = gtk_image_new ();
    gtk_image_set_pixel_size (GTK_IMAGE (img), cote_image);
    /* Un carre constant : dans l'Apercu, une icone de 96 px et une vignette
     * de 128 occupent la meme place, et les noms restent alignes. */
    gtk_widget_set_size_request (img, cote_image, cote_image);

    GtkWidget *nom = gtk_label_new (NULL);
    gtk_widget_add_css_class (nom, "fichiers-nom");
    gtk_label_set_wrap (GTK_LABEL (nom), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (nom), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines (GTK_LABEL (nom), 2);
    gtk_label_set_ellipsize (GTK_LABEL (nom), PANGO_ELLIPSIZE_END);
    gtk_label_set_justify (GTK_LABEL (nom), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars (GTK_LABEL (nom), car_max);

    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class (boite, "fichiers-case");
    gtk_widget_set_size_request (boite, largeur, -1);
    gtk_widget_set_halign (boite, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (boite), img);
    gtk_box_append (GTK_BOX (boite), nom);

    g_object_set_data (G_OBJECT (boite), "image", img);
    g_object_set_data (G_OBJECT (boite), "nom", nom);
    armer (boite, li);
    gtk_list_item_set_child (li, boite);
    return boite;
}

static void
grille_setup (GtkListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void) f; (void) d;
    case_nouvelle (li, 48, 110, 14);
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

/* --- apercu ---------------------------------------------------------------- */
#define APERCU_COTE 128        /* la vignette, en pixels logiques          */
#define APERCU_ICONE 96        /* l'icone, quand il n'y a pas de vignette  */

static void
apercu_setup (GtkListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void) f; (void) d;
    GtkWidget *boite = case_nouvelle (li, APERCU_COTE, APERCU_COTE + 20, 16);
    gtk_widget_add_css_class (boite, "fichiers-case-apercu");
}

static void
apercu_poser (GtkWidget *boite, FichierItem *it)
{
    GtkImage *img = GTK_IMAGE (g_object_get_data (G_OBJECT (boite), "image"));
    GdkTexture *t = fichiers_apercu_obtenir (it);

    if (t != NULL) {
        gtk_image_set_from_paintable (img, GDK_PAINTABLE (t));
        gtk_image_set_pixel_size (img, APERCU_COTE);
    } else {
        gtk_image_set_from_gicon (img, it->icone);
        gtk_image_set_pixel_size (img, APERCU_ICONE);
    }
}

/* La vignette arrive apres la liaison. La case verifie qu'elle montre
 * toujours cet element : entre la demande et la reponse, le defilement a pu
 * la recycler pour un autre. */
static void
on_apercu_pret (FichierItem *it, gpointer data)
{
    GtkWidget   *boite = data;
    GtkListItem *li = g_object_get_data (G_OBJECT (boite), "list-item");
    if (li != NULL && gtk_list_item_get_item (li) == (gpointer) it)
        apercu_poser (boite, it);
}

static void
apercu_bind (GtkListItemFactory *f, GtkListItem *li, gpointer d)
{
    case_bind (f, li, d);

    GtkWidget   *boite = gtk_list_item_get_child (li);
    FichierItem *it    = gtk_list_item_get_item (li);
    if (boite == NULL || it == NULL)
        return;

    fichiers_apercu_regler (gtk_widget_get_scale_factor (boite));
    g_signal_connect (it, "apercu-pret", G_CALLBACK (on_apercu_pret), boite);
    apercu_poser (boite, it);
}

static void
apercu_unbind (GtkListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void) f; (void) d;
    GtkWidget   *boite = gtk_list_item_get_child (li);
    FichierItem *it    = gtk_list_item_get_item (li);
    if (boite == NULL || it == NULL)
        return;

    g_signal_handlers_disconnect_by_func (it, on_apercu_pret, boite);
    fichiers_apercu_oublier (it);
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
    /* L'etiquette occupe tout le reste de la ligne, texte cale a gauche :
     * l'ajustement au double clic (ajuster_colonne) deduit la marge d'une
     * cellule en retranchant la largeur de l'etiquette a celle de la
     * colonne, ce qui suppose qu'elle ait tout recu. */
    gtk_widget_set_hexpand (nom, TRUE);
    gtk_label_set_xalign (GTK_LABEL (nom), 0.0);

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

/* La cellule de la colonne de bourrage : vide, mais armee comme les autres,
 * pour que la ligne se clique et se glisse jusqu'au bord droit. */
static void
bourrage_setup (GtkListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void) f; (void) d;
    GtkWidget *vide = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand (vide, TRUE);
    armer (vide, li);
    gtk_list_item_set_child (li, vide);
}

static void
bourrage_bind (GtkListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void) f; (void) li; (void) d;
}

static GtkListItemFactory *
fabrique (GCallback setup, GCallback bind, gpointer data)
{
    GtkListItemFactory *f = gtk_signal_list_item_factory_new ();
    g_signal_connect (f, "setup", setup, data);
    g_signal_connect (f, "bind",  bind,  data);
    return f;
}

/* TOUTES LES COLONNES ONT UNE LARGEUR FIXE, « Nom » comprise.
 *
 * Sans bornes, les colonnes se dimensionnent sur leur contenu et la
 * derniere depasse la fenetre -- « 240,0 ko » tronque a droite, vu au banc
 * d'essai. Elles restent redimensionnables a la main, et un double clic sur
 * la bordure droite d'un en-tete les ramene a la largeur de leur contenu :
 * c'est GTK qui le fait (header_pressed, gtkcolumnview.c), en rendant
 * fixed-width a -1.
 *
 * « Nom » etait auparavant en expand, et c'etait le defaut : GTK donne tout
 * l'espace libre aux colonnes en expand, PAR-DESSUS leur largeur naturelle.
 * La reduire rendait aussitot l'espace libere a elle-meme -- les colonnes de
 * droite ne pouvaient pas glisser vers la gauche -- et une marge enorme
 * separait la fin des noms de « Modifié le ». Voir colonne_bourrage(). */
static GtkColumnViewColumn *
colonne (const char *titre, GtkListItemFactory *f, GtkSorter *tri, int largeur)
{
    GtkColumnViewColumn *c = gtk_column_view_column_new (titre, f);
    gtk_column_view_column_set_sorter (c, tri);
    gtk_column_view_column_set_resizable (c, TRUE);
    gtk_column_view_column_set_fixed_width (c, largeur);

    g_object_unref (tri);
    gtk_column_view_append_column (F.colonnes, c);
    return c;
}

/* LA COLONNE DE BOURRAGE, invisible, toujours en dernier.
 *
 * GTK ne donne JAMAIS de poignee a la derniere colonne (« i + 1 < n » dans
 * header_drag_begin et header_pressed) : « Taille », derniere, ne se
 * redimensionnait pas, ni a la main ni au double clic. Et il faut bien que
 * l'espace libre a droite aille quelque part : sans colonne en expand, les
 * lignes s'arreteraient a « Taille », et le surlignage avec elles.
 *
 * Cette colonne sans titre ni tri prend donc l'espace restant. Toutes les
 * vraies colonnes ont une poignee, et quand l'une se reduit ou s'ajuste au
 * double clic, celles de sa droite suivent -- le bourrage absorbe la
 * difference. */
static void
on_colonnes_changees (GListModel *m, guint p, guint r, guint a, gpointer d)
{
    (void) p; (void) r; (void) a; (void) d;
    static gboolean en_cours = FALSE;

    /* Les en-tetes se reordonnent au glisser : si l'on y deplace le
     * bourrage, il retourne au bout. Le repositionnement rappelle ce
     * gestionnaire, d'ou le verrou. */
    guint n = g_list_model_get_n_items (m);
    if (en_cours || F.col_bourrage == NULL || n == 0)
        return;

    g_autoptr(GtkColumnViewColumn) dernier = g_list_model_get_item (m, n - 1);
    if (dernier == F.col_bourrage)
        return;

    en_cours = TRUE;
    gtk_column_view_insert_column (F.colonnes, n - 1, F.col_bourrage);
    en_cours = FALSE;
}

/* L'AJUSTEMENT AU DOUBLE CLIC MESURE TOUT LE DOSSIER.
 *
 * GTK sait deja ramener une colonne a son contenu (header_pressed), mais il
 * ne mesure que les cellules CONSTRUITES -- les vues sont virtuelles, donc
 * seulement les lignes a l'ecran. Le nom le plus long, trois lignes plus
 * bas, restait tronque apres l'ajustement. Vu au banc d'essai.
 *
 * On prend donc le double clic avant lui (capture), et l'on mesure le texte
 * de chaque element du modele avec la police de la vue. La marge d'une
 * cellule -- icone, espacements, rembourrages du theme -- n'est pas devinee :
 * elle est lue sur une ligne affichee, colonne moins etiquette. */

static GtkWidget *
enfant_nomme (GtkWidget *parent, const char *css, guint rang)
{
    for (GtkWidget *c = gtk_widget_get_first_child (parent); c != NULL;
         c = gtk_widget_get_next_sibling (c)) {
        if (g_strcmp0 (gtk_widget_get_css_name (c), css) != 0)
            continue;
        if (rang-- == 0)
            return c;
    }
    return NULL;
}

static void
ajuster_colonne (guint rang, GtkWidget *titre)
{
    g_autoptr(GtkColumnViewColumn) col =
        g_list_model_get_item (gtk_column_view_get_columns (F.colonnes), rang);
    if (col == NULL)
        return;

    /* Une ligne affichee, et l'etiquette de cette colonne dans cette ligne. */
    GtkWidget *vue    = enfant_nomme (GTK_WIDGET (F.colonnes), "listview", 0);
    GtkWidget *ligne  = vue ? enfant_nomme (vue, "row", 0) : NULL;
    GtkWidget *cell   = ligne ? enfant_nomme (ligne, "cell", rang) : NULL;
    GtkWidget *contenu = cell ? gtk_widget_get_first_child (cell) : NULL;
    GtkWidget *etiquette = NULL;
    if (contenu != NULL)
        etiquette = GTK_IS_LABEL (contenu)
                  ? contenu : g_object_get_data (G_OBJECT (contenu), "nom");

    if (etiquette == NULL) {
        /* Rien d'affiche a quoi prendre la police : le comportement de GTK,
         * sur ce qu'il a construit, vaut mieux que rien. */
        gtk_column_view_column_set_fixed_width (col, -1);
        return;
    }

    /* La BOITE DE BORDURE du titre, qui est la largeur de la colonne :
     * gtk_widget_get_width() rend la boite de contenu, rembourrage exclu --
     * 299 px pour une colonne de 320, et le nom le plus long restait tronque
     * d'autant. L'etiquette, elle, n'a ni bordure ni rembourrage. */
    graphene_rect_t b;
    if (!gtk_widget_compute_bounds (titre, titre, &b))
        return;
    int marge = (int) b.size.width - gtk_widget_get_width (etiquette);

    TexteFunc fn = col == F.col_date   ? fichier_item_date_texte
                 : col == F.col_type   ? type_de
                 : col == F.col_taille ? fichier_item_taille_texte
                                       : NULL;

    PangoLayout *mise = gtk_widget_create_pango_layout (etiquette, NULL);
    int large = 0;
    guint n = g_list_model_get_n_items (G_LIST_MODEL (F.selection));
    for (guint i = 0; i < n; i++) {
        g_autoptr(FichierItem) it = g_list_model_get_item (G_LIST_MODEL (F.selection), i);
        g_autofree char *t = (fn != NULL) ? fn (it) : g_strdup (it->nom);
        int w;
        pango_layout_set_text (mise, t, -1);
        pango_layout_get_pixel_size (mise, &w, NULL);
        large = MAX (large, w);
    }
    g_object_unref (mise);

    /* Jamais plus etroite que son titre : l'en-tete doit rester lisible,
     * et c'est aussi la regle de GTK. */
    int titre_nat;
    gtk_widget_measure (titre, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &titre_nat,
                        NULL, NULL);

    /* +2 : l'arrondi des mesures Pango, faute de quoi le dernier caractere
     * se voit parfois remplace par des points de suspension. */
    gtk_column_view_column_set_fixed_width (col, MAX (titre_nat, large + marge + 2));
}

static void
on_double_clic_entete (GtkGestureClick *g, int n_press, double x, double y,
                       gpointer data)
{
    (void) data; (void) n_press;

    /* Le double clic est compte ICI, et non par n_press : le premier appui
     * sur une bordure, GTK le reclame pour son glisser de redimensionnement,
     * ce geste-ci perd la sequence, et son compteur repart a un. */
    static gint64 dernier = 0;
    static double dernier_x = -100;
    int delai_ms = 400;
    g_object_get (gtk_widget_get_settings (GTK_WIDGET (F.colonnes)),
                  "gtk-double-click-time", &delai_ms, NULL);
    gint64 maintenant = g_get_monotonic_time ();
    gboolean double_clic = maintenant - dernier <= (gint64) delai_ms * 1000
                        && ABS (x - dernier_x) <= 6;
    dernier   = double_clic ? 0 : maintenant;
    dernier_x = x;
    if (!double_clic)
        return;

    GtkWidget *entete = enfant_nomme (GTK_WIDGET (F.colonnes), "header", 0);
    graphene_point_t p;
    if (entete == NULL
        || !gtk_widget_compute_point (GTK_WIDGET (F.colonnes), entete,
                                      &GRAPHENE_POINT_INIT ((float) x, (float) y), &p)
        || p.y < 0 || p.y > gtk_widget_get_height (entete))
        return;

    /* La bordure droite de chaque titre, a 4 px pres -- la zone de saisie de
     * GTK (DRAG_WIDTH, 8 px centres sur le bord). Le bourrage, dernier, n'a
     * pas de bordure a saisir. */
    guint rang = 0;
    for (GtkWidget *t = gtk_widget_get_first_child (entete); t != NULL;
         t = gtk_widget_get_next_sibling (t), rang++) {
        if (gtk_widget_get_next_sibling (t) == NULL)
            break;
        graphene_rect_t r;
        if (!gtk_widget_compute_bounds (t, entete, &r))
            continue;
        double bord = r.origin.x + r.size.width;
        if (p.x >= bord - 4 && p.x <= bord + 4) {
            gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
            ajuster_colonne (rang, t);
            return;
        }
    }
}

static void
colonne_bourrage (void)
{
    F.col_bourrage = gtk_column_view_column_new (
        NULL, fabrique (G_CALLBACK (bourrage_setup), G_CALLBACK (bourrage_bind), NULL));
    gtk_column_view_column_set_expand (F.col_bourrage, TRUE);
    gtk_column_view_column_set_resizable (F.col_bourrage, FALSE);
    gtk_column_view_append_column (F.colonnes, F.col_bourrage);

    g_signal_connect (gtk_column_view_get_columns (F.colonnes), "items-changed",
                      G_CALLBACK (on_colonnes_changees), NULL);

    GtkGesture *dbl = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (dbl), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (dbl),
                                                GTK_PHASE_CAPTURE);
    g_signal_connect (dbl, "pressed", G_CALLBACK (on_double_clic_entete), NULL);
    gtk_widget_add_controller (GTK_WIDGET (F.colonnes), GTK_EVENT_CONTROLLER (dbl));
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

/* --- terminal ------------------------------------------------------------- */

/* Le terminal du bureau d'abord -- celui du dock, que claude-os-theme
 * habille -- puis l'alternative Debian, puis celui de secours de labwc.
 *
 * Le dossier est passe DEUX fois : en repertoire courant du processus, que
 * tout terminal herite, et par --working-directory pour xfce4-terminal. Ce
 * dernier est mono-instance : un second lancement confie la fenetre au
 * processus deja ouvert, dont le repertoire courant est celui de SON
 * lancement. Seule l'option voyage jusqu'a lui. */
static void
ouvrir_terminal (GFile *dossier)
{
    g_autofree char *chemin = g_file_get_path (dossier);
    if (chemin == NULL) {
        signaler ("Impossible d'ouvrir un terminal ici",
                  "Ce dossier n'a pas de chemin sur le disque.");
        return;
    }

    static const char *const TERMINAUX[] = {
        "xfce4-terminal", "x-terminal-emulator", "foot", NULL
    };

    g_autofree char *prog = NULL;
    const char *nom = NULL;
    for (int i = 0; TERMINAUX[i] != NULL && prog == NULL; i++) {
        prog = g_find_program_in_path (TERMINAUX[i]);
        nom  = TERMINAUX[i];
    }
    if (prog == NULL) {
        signaler ("Aucun terminal n'est installé",
                  "Ni xfce4-terminal, ni x-terminal-emulator, ni foot.");
        return;
    }

    g_autoptr(GSubprocessLauncher) l =
        g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_set_cwd (l, chemin);

    g_autoptr(GError) e = NULL;
    g_autoptr(GSubprocess) p = NULL;
    if (g_strcmp0 (nom, "xfce4-terminal") == 0)
        p = g_subprocess_launcher_spawn (l, &e, prog, "--working-directory", chemin, NULL);
    else
        p = g_subprocess_launcher_spawn (l, &e, prog, NULL);

    /* GSubprocess recolte lui-meme le processus a sa fin : le relacher ici
     * ne laisse pas de zombie. */
    if (p == NULL)
        signaler ("Impossible d'ouvrir un terminal", e->message);
}

static void
act_terminal (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    ouvrir_terminal (F.dossier);
}

/* Depuis le menu d'un DOSSIER : le terminal s'ouvre dans ce dossier-la, et
 * non dans celui qui le contient. */
static void
act_terminal_element (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    g_autoptr(FichierItem) it = premier_choisi ();
    ouvrir_terminal ((it != NULL && it->dossier) ? it->file : F.dossier);
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
    { "terminal",   act_terminal,   NULL, NULL, NULL, { 0 } },
    { "terminal-element", act_terminal_element, NULL, NULL, NULL, { 0 } },
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

/* Le menu d'un element, au clic droit comme a l'appui long du doigt. */
static void
menu_element (GtkGesture *g, GtkWidget *w, double x, double y)
{
    FichierItem *it = item_de (w);
    GtkListItem *li = g_object_get_data (G_OBJECT (w), "list-item");
    if (it == NULL || li == NULL)
        return;

    /* On PREND la sequence. Sans cela, le geste du fond -- pose sur la pile,
     * donc en amont -- se declenchait juste apres et remplacait le menu de
     * l'element par le sien : le clic droit sur un fichier proposait
     * « Nouveau dossier ». Un GtkGestureClick ne bloque pas la propagation
     * de lui-meme, il faut la reclamer. Constate au banc d'essai.
     *
     * Au doigt, la reclamer a un second effet, voulu : le geste de l'element
     * de liste se voit refuser la sequence, et le doigt qu'on leve apres
     * l'appui long n'ouvre pas le fichier. */
    gtk_gesture_set_state (g, GTK_EVENT_SEQUENCE_CLAIMED);

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
    if (it->dossier) {
        g_menu_append (s4, "Ouvrir dans un terminal", "fichiers.terminal-element");
        g_menu_append (s4, fichiers_lieux_est_favori (F.lieux, it->file)
                           ? "Retirer des favoris" : "Ajouter aux favoris",
                       "fichiers.favori");
    }
    g_menu_append (s4, "Propriétés", "fichiers.proprietes");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (s4));

    montrer_menu (menu, w, x, y);
}

static void
on_clic_item (GtkGestureClick *g, int n, double x, double y, gpointer data)
{
    (void) n;
    menu_element (GTK_GESTURE (g), data, x, y);
}

static void
on_appui_long_item (GtkGestureLongPress *g, double x, double y, gpointer data)
{
    menu_element (GTK_GESTURE (g), data, x, y);
}

/* -------------------------------------------------------------------------
 * Le fond : ce qui n'est pas un element
 *
 * Le point vise est-il sur un element, ou « a cote » ? On remonte depuis le
 * widget touche jusqu'a la pile. Une case de grille (« child »), une ligne
 * (« row ») ou un widget arme -- c'est un element. Les en-tetes de colonnes
 * et les barres de defilement ne sont pas le fond non plus : cliquer pour
 * trier ou pour defiler ne doit pas perdre la selection.
 * ------------------------------------------------------------------------- */
static gboolean
sur_un_element (double x, double y)
{
    GtkWidget *w = gtk_widget_pick (F.pile, x, y, GTK_PICK_DEFAULT);

    for (; w != NULL && w != F.pile; w = gtk_widget_get_parent (w)) {
        const char *n = gtk_widget_get_css_name (w);
        if (g_strcmp0 (n, "child") == 0 || g_strcmp0 (n, "row") == 0
            || g_strcmp0 (n, "header") == 0 || g_strcmp0 (n, "scrollbar") == 0
            || g_object_get_data (G_OBJECT (w), "list-item") != NULL)
            return TRUE;
    }
    return FALSE;
}

static void
menu_fond (GtkWidget *w, double x, double y)
{
    g_autoptr(GMenu) menu = g_menu_new ();
    g_autoptr(GMenu) s1 = g_menu_new ();
    g_menu_append (s1, "Nouveau dossier…", "fichiers.nouveau");
    if (F.presse->len > 0)
        g_menu_append (s1, "Coller", "fichiers.coller");
    g_menu_append (s1, "Ouvrir un terminal ici", "fichiers.terminal");
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

/* Clic droit sur le fond : comme dans Windows, la selection tombe d'abord.
 * Le menu du fond agit sur le dossier, et une selection restee allumee
 * laisserait croire que « Coller » ou « Nouveau dossier » la concerne. */
static void
on_clic_fond (GtkGestureClick *g, int n, double x, double y, gpointer data)
{
    (void) g; (void) n;
    if (!sur_un_element (x, y))
        gtk_selection_model_unselect_all (F.selection);
    menu_fond (data, x, y);
}

static void
on_appui_long_fond (GtkGestureLongPress *g, double x, double y, gpointer data)
{
    if (sur_un_element (x, y))
        return;
    gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_selection_model_unselect_all (F.selection);
    menu_fond (data, x, y);
}

/* LE CLIC A COTE DESELECTIONNE TOUT.
 *
 * Au RELACHEMENT, et non a l'appui : au doigt, poser le doigt a cote d'un
 * element pour faire defiler la vue ne doit pas perdre la selection. Le
 * defilement reclame la sequence, ce geste-ci est annule, et « released »
 * n'arrive jamais. Seul un appui bref -- un vrai clic -- deselectionne.
 *
 * Pose en CAPTURE sur la pile, pour voir passer le clic avant les vues. Il
 * ne reclame rien : un clic sur un element continue son chemin jusqu'a
 * l'element de liste, qui le selectionne comme avant.
 *
 * Ctrl et Maj gardent la selection : ce sont les touches qu'on tient pour
 * l'etendre, et les relacher un peu tard ne doit pas tout effacer. */
static void
on_relache_fond (GtkGestureClick *g, int n, double x, double y, gpointer data)
{
    (void) n; (void) data;
    GdkModifierType m =
        gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (g));
    if (m & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
        return;
    if (!sur_un_element (x, y))
        gtk_selection_model_unselect_all (F.selection);
}

/* -------------------------------------------------------------------------
 * Le doigt ouvre d'un seul appui
 *
 * A la souris, un clic selectionne et un double clic ouvre : c'est le
 * comportement de Windows, et il est garde. Au doigt, le double appui est
 * penible et imprecis ; un appui simple ouvre, comme sur une tablette.
 *
 * GTK sait faire l'un ou l'autre -- « single-click-activate » -- mais pour
 * toute la vue, et en mode simple clic une souris SELECTIONNE AU SURVOL.
 * On bascule donc selon le dernier appareil utilise : le premier contact du
 * doigt passe les vues en appui simple, le premier mouvement de souris ou de
 * pave les ramene au double clic.
 *
 * Controleur « legacy » en capture sur la fenetre : il voit chaque evenement
 * avant tout geste, donc AVANT que l'element de liste ne decide, au
 * relachement, s'il ouvre ou s'il selectionne. Il ne consomme rien.
 * ------------------------------------------------------------------------- */
static void
regler_tactile (gboolean tactile)
{
    if (F.tactile == tactile)
        return;
    F.tactile = tactile;

    gtk_grid_view_set_single_click_activate (GTK_GRID_VIEW (F.grille), tactile);
    gtk_grid_view_set_single_click_activate (GTK_GRID_VIEW (F.apercu), tactile);
    gtk_list_view_set_single_click_activate (GTK_LIST_VIEW (F.liste), tactile);
    gtk_column_view_set_single_click_activate (F.colonnes, tactile);
}

static gboolean
on_saisie (GtkEventControllerLegacy *c, GdkEvent *e, gpointer data)
{
    (void) c; (void) data;
    GdkEventType t = gdk_event_get_event_type (e);

    if (t == GDK_TOUCH_BEGIN) {
        regler_tactile (TRUE);
    } else if (t == GDK_BUTTON_PRESS || t == GDK_MOTION_NOTIFY) {
        GdkDevice *dev = gdk_event_get_device (e);
        if (dev != NULL && gdk_device_get_source (dev) != GDK_SOURCE_TOUCHSCREEN)
            regler_tactile (FALSE);
    }
    return GDK_EVENT_PROPAGATE;
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

    /* --- le modele, partage par les quatre vues --- */
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
        GTK_SORTER (gtk_custom_sorter_new (cmp_nom, NULL, NULL)), 320);
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
    colonne_bourrage ();

    /* Le trieur de la vue Details est LE trieur : cliquer un en-tete
     * reordonne aussi les vues Icones et Liste, et le menu « Trier par »
     * n'est qu'une autre facon d'agir sur les memes colonnes. Une seule
     * source de verite, aucun etat a synchroniser. */
    gtk_sort_list_model_set_sorter (F.modele_tri,
                                    gtk_column_view_get_sorter (F.colonnes));
    gtk_column_view_sort_by_column (F.colonnes, F.col_nom, GTK_SORT_ASCENDING);
    gtk_column_view_set_model (F.colonnes, F.selection);

    /* --- vues Icones, Apercu et Liste --- */
    /* Autant de colonnes que la fenetre en loge : les cases ont une largeur
     * fixe (voir case_nouvelle), et un plafond bas etalerait l'espace
     * restant ENTRE elles sur un grand ecran. */
    F.grille = gtk_grid_view_new (
        g_object_ref (F.selection),
        fabrique (G_CALLBACK (grille_setup), G_CALLBACK (case_bind), NULL));
    gtk_grid_view_set_max_columns (GTK_GRID_VIEW (F.grille), 32);
    gtk_grid_view_set_min_columns (GTK_GRID_VIEW (F.grille), 2);
    gtk_widget_add_css_class (F.grille, "fichiers-grille");

    GtkListItemFactory *f_apercu = gtk_signal_list_item_factory_new ();
    g_signal_connect (f_apercu, "setup",  G_CALLBACK (apercu_setup),  NULL);
    g_signal_connect (f_apercu, "bind",   G_CALLBACK (apercu_bind),   NULL);
    g_signal_connect (f_apercu, "unbind", G_CALLBACK (apercu_unbind), NULL);
    F.apercu = gtk_grid_view_new (g_object_ref (F.selection), f_apercu);
    gtk_grid_view_set_max_columns (GTK_GRID_VIEW (F.apercu), 32);
    gtk_grid_view_set_min_columns (GTK_GRID_VIEW (F.apercu), 1);
    gtk_widget_add_css_class (F.apercu, "fichiers-grille");
    gtk_widget_add_css_class (F.apercu, "fichiers-apercu");

    F.liste = gtk_list_view_new (
        g_object_ref (F.selection),
        fabrique (G_CALLBACK (liste_setup), G_CALLBACK (case_bind), NULL));
    gtk_widget_add_css_class (F.liste, "fichiers-liste");

    g_signal_connect (F.grille, "activate", G_CALLBACK (on_active), NULL);
    g_signal_connect (F.apercu, "activate", G_CALLBACK (on_active), NULL);
    g_signal_connect (F.liste,  "activate", G_CALLBACK (on_active), NULL);
    g_signal_connect (F.colonnes, "activate", G_CALLBACK (on_active), NULL);
    g_signal_connect_after (F.selection, "items-changed",
                            G_CALLBACK (garder_en_haut), NULL);
    /* Le modele survit aux vues. En se detruisant, la vue Details retire
     * son trieur : le modele se retrie, et garder_en_haut() visait des vues
     * deja liberees -- trois Gtk-CRITICAL a chaque fermeture, vus sous
     * AddressSanitizer. Se desabonner sur « destroy » ne suffit pas : une
     * GtkWindow detruit ses enfants AVANT d'emettre ce signal. Des pointeurs
     * faibles, remis a NULL a la liberation de chaque vue. */
    g_object_add_weak_pointer (G_OBJECT (F.grille),   (gpointer *) &F.grille);
    g_object_add_weak_pointer (G_OBJECT (F.apercu),   (gpointer *) &F.apercu);
    g_object_add_weak_pointer (G_OBJECT (F.liste),    (gpointer *) &F.liste);
    g_object_add_weak_pointer (G_OBJECT (F.colonnes), (gpointer *) &F.colonnes);

    GtkWidget *d_grille = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (d_grille), F.grille);
    GtkWidget *d_apercu = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (d_apercu), F.apercu);
    GtkWidget *d_liste = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (d_liste), F.liste);
    GtkWidget *d_col = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (d_col), GTK_WIDGET (F.colonnes));

    F.pile = gtk_stack_new ();
    gtk_stack_add_named (GTK_STACK (F.pile), d_grille, "icones");
    gtk_stack_add_named (GTK_STACK (F.pile), d_apercu, "apercu");
    gtk_stack_add_named (GTK_STACK (F.pile), d_liste,  "liste");
    gtk_stack_add_named (GTK_STACK (F.pile), d_col,    "details");
    gtk_widget_set_hexpand (F.pile, TRUE);
    gtk_widget_set_vexpand (F.pile, TRUE);

    /* Clic droit sur le fond, et depot dans le dossier courant : poses sur
     * la pile, ils valent pour toutes les vues d'un coup. */
    GtkGestureClick *fond = GTK_GESTURE_CLICK (gtk_gesture_click_new ());
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (fond), GDK_BUTTON_SECONDARY);
    g_signal_connect (fond, "pressed", G_CALLBACK (on_clic_fond), F.pile);
    gtk_widget_add_controller (F.pile, GTK_EVENT_CONTROLLER (fond));

    GtkGesture *fond_long = gtk_gesture_long_press_new ();
    gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (fond_long), TRUE);
    g_signal_connect (fond_long, "pressed", G_CALLBACK (on_appui_long_fond), F.pile);
    gtk_widget_add_controller (F.pile, GTK_EVENT_CONTROLLER (fond_long));

    GtkGestureClick *a_cote = GTK_GESTURE_CLICK (gtk_gesture_click_new ());
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (a_cote), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (a_cote),
                                                GTK_PHASE_CAPTURE);
    g_signal_connect (a_cote, "released", G_CALLBACK (on_relache_fond), NULL);
    gtk_widget_add_controller (F.pile, GTK_EVENT_CONTROLLER (a_cote));

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
    /* Du plus grand au plus dense, comme le menu Affichage de Windows. */
    const struct { const char *nom, *cible; } v[] = {
        { "Aperçu", "apercu" }, { "Icônes", "icones" },
        { "Liste", "liste" },   { "Détails", "details" }
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

    GtkEventController *saisie = gtk_event_controller_legacy_new ();
    gtk_event_controller_set_propagation_phase (saisie, GTK_PHASE_CAPTURE);
    g_signal_connect (saisie, "event", G_CALLBACK (on_saisie), NULL);
    gtk_widget_add_controller (F.fenetre, saisie);

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
