#include "fichiers-lieux.h"

typedef struct {
    GtkWidget       *boite;        /* la GtkBox racine, celle qu'on rend    */
    GtkWidget       *liste;        /* GtkListBox des emplacements           */
    LieuxNavFunc     nav;
    gpointer         data;
    GVolumeMonitor  *moniteur;
    GPtrArray       *favoris;      /* char* : chemins, dans l'ordre du fichier */
    GFile           *courant;
} Lieux;

/* ------------------------------------------------------------------------- */
static char *
chemin_favoris (void)
{
    return g_build_filename (g_get_user_config_dir (), "claude-os", "favoris", NULL);
}

static void
favoris_lire (Lieux *L)
{
    g_ptr_array_set_size (L->favoris, 0);

    g_autofree char *chemin = chemin_favoris ();
    g_autofree char *contenu = NULL;
    if (!g_file_get_contents (chemin, &contenu, NULL, NULL))
        return;

    g_auto(GStrv) lignes = g_strsplit (contenu, "\n", -1);
    for (guint i = 0; lignes[i] != NULL; i++) {
        g_strstrip (lignes[i]);
        if (*lignes[i] != '\0' && *lignes[i] != '#')
            g_ptr_array_add (L->favoris, g_strdup (lignes[i]));
    }
}

static void
favoris_ecrire (Lieux *L)
{
    g_autofree char *chemin = chemin_favoris ();
    g_autofree char *dossier = g_path_get_dirname (chemin);
    g_mkdir_with_parents (dossier, 0700);

    GString *s = g_string_new ("# Emplacements favoris du gestionnaire de fichiers.\n"
                               "# Un chemin par ligne.\n");
    for (guint i = 0; i < L->favoris->len; i++)
        g_string_append_printf (s, "%s\n", (const char *) g_ptr_array_index (L->favoris, i));

    g_autoptr(GError) e = NULL;
    if (!g_file_set_contents (chemin, s->str, -1, &e))
        g_warning ("favoris non enregistrés : %s", e->message);
    g_string_free (s, TRUE);
}

/* ------------------------------------------------------------------------- */
static void reconstruire (Lieux *L);

static void
on_ligne_activee (GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    Lieux *L = data;
    (void) box;

    GFile *cible = g_object_get_data (G_OBJECT (row), "fichier");
    if (cible != NULL) {
        L->nav (cible, L->data);
        return;
    }

    /* Un volume pas encore monte : on le monte, la navigation suit dans le
     * rappel. Cliquer sur une cle USB doit la monter, pas ne rien faire. */
    GVolume *vol = g_object_get_data (G_OBJECT (row), "volume");
    if (vol != NULL)
        g_volume_mount (vol, G_MOUNT_MOUNT_NONE, NULL, NULL, NULL, NULL);
}

static void
on_ejecter (GtkButton *b, gpointer data)
{
    Lieux *L = data;
    GMount *mnt = g_object_get_data (G_OBJECT (b), "montage");
    (void) L;

    if (mnt == NULL)
        return;

    /* Ejecter si le materiel le sait, demonter sinon : une cle USB veut
     * l'un, une partition interne montee veut l'autre. */
    if (g_mount_can_eject (mnt))
        g_mount_eject_with_operation (mnt, G_MOUNT_UNMOUNT_NONE, NULL, NULL, NULL, NULL);
    else
        g_mount_unmount_with_operation (mnt, G_MOUNT_UNMOUNT_NONE, NULL, NULL, NULL, NULL);
}

static void
on_retirer_favori (GSimpleAction *a, GVariant *param, gpointer data)
{
    Lieux *L = data;
    (void) a;

    g_autoptr(GFile) f = g_file_new_for_path (g_variant_get_string (param, NULL));
    fichiers_lieux_retirer (L->boite, f);
}

/* ------------------------------------------------------------------------- */
static GtkWidget *
entete (const char *titre)
{
    GtkWidget *l = gtk_label_new (titre);
    gtk_widget_add_css_class (l, "lieux-entete");
    gtk_widget_set_halign (l, GTK_ALIGN_START);

    GtkWidget *row = gtk_list_box_row_new ();
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), l);
    gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
    gtk_widget_add_css_class (row, "lieux-entete-row");
    return row;
}

/* Une entree. `cible` est le dossier a ouvrir, ou NULL pour un volume a
 * monter ; `mnt` non NULL ajoute le bouton d'ejection. */
static GtkWidget *
entree (Lieux *L, const char *nom, GIcon *icone, GFile *cible,
        GVolume *vol, GMount *mnt, const char *favori)
{
    GtkWidget *img = gtk_image_new_from_gicon (icone);
    gtk_image_set_pixel_size (GTK_IMAGE (img), 16);
    gtk_widget_add_css_class (img, "lieux-icone");

    GtkWidget *lbl = gtk_label_new (nom);
    gtk_widget_add_css_class (lbl, "lieux-nom");
    gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign (lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand (lbl, TRUE);

    GtkWidget *ligne = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append (GTK_BOX (ligne), img);
    gtk_box_append (GTK_BOX (ligne), lbl);

    if (mnt != NULL && (g_mount_can_eject (mnt) || g_mount_can_unmount (mnt))) {
        GtkWidget *ej = gtk_button_new_from_icon_name ("media-eject-symbolic");
        gtk_widget_add_css_class (ej, "lieux-ejecter");
        gtk_widget_set_valign (ej, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text (ej, "Éjecter");
        g_object_set_data_full (G_OBJECT (ej), "montage", g_object_ref (mnt), g_object_unref);
        g_signal_connect (ej, "clicked", G_CALLBACK (on_ejecter), L);
        gtk_box_append (GTK_BOX (ligne), ej);
    }

    GtkWidget *row = gtk_list_box_row_new ();
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), ligne);
    gtk_widget_add_css_class (row, "lieux-ligne");

    if (cible != NULL)
        g_object_set_data_full (G_OBJECT (row), "fichier", g_object_ref (cible), g_object_unref);
    if (vol != NULL)
        g_object_set_data_full (G_OBJECT (row), "volume", g_object_ref (vol), g_object_unref);

    /* Un favori ajoute a la main se retire au clic droit. Les dossiers
     * personnels et les peripheriques, eux, ne se retirent pas : ils ne
     * viennent pas d'un choix, ils constatent ce qui existe. */
    if (favori != NULL) {
        g_autoptr(GMenu) menu = g_menu_new ();
        g_autofree char *d = g_strdup_printf ("lieux.retirer::%s", favori);
        g_menu_append (menu, "Retirer des favoris", d);

        GtkWidget *pop = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
        gtk_widget_add_css_class (pop, "fichiers-menu");
        gtk_popover_set_has_arrow (GTK_POPOVER (pop), FALSE);
        gtk_widget_set_parent (pop, row);
        g_object_set_data (G_OBJECT (row), "menu", pop);

        GtkGestureClick *g = GTK_GESTURE_CLICK (gtk_gesture_click_new ());
        gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (g), GDK_BUTTON_SECONDARY);
        g_signal_connect_swapped (g, "pressed", G_CALLBACK (gtk_popover_popup), pop);
        gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (g));

        /* Un popover attache par set_parent doit etre detache avant que son
         * parent ne disparaisse, sinon GTK signale un widget detruit avec
         * des enfants encore attaches -- et le volet se reconstruit a chaque
         * branchement de cle USB. */
        g_signal_connect_swapped (row, "destroy", G_CALLBACK (gtk_widget_unparent), pop);
    }

    return row;
}

static void
ajouter_chemin (Lieux *L, const char *chemin, const char *nom,
                const char *icone, const char *favori)
{
    if (chemin == NULL)
        return;
    if (!g_file_test (chemin, G_FILE_TEST_IS_DIR))
        return;                    /* un dossier absent n'a rien a faire la */

    g_autoptr(GFile) f = g_file_new_for_path (chemin);
    g_autoptr(GIcon) ic = g_themed_icon_new (icone);
    g_autofree char *base = (nom != NULL) ? NULL : g_path_get_basename (chemin);

    gtk_list_box_append (GTK_LIST_BOX (L->liste),
                         entree (L, nom ? nom : base, ic, f, NULL, NULL, favori));
}

static void
reconstruire (Lieux *L)
{
    GtkWidget *enfant;
    while ((enfant = gtk_widget_get_first_child (L->liste)) != NULL)
        gtk_list_box_remove (GTK_LIST_BOX (L->liste), enfant);

    /* --- dossiers personnels --- */
    gtk_list_box_append (GTK_LIST_BOX (L->liste), entete ("Emplacements"));

    ajouter_chemin (L, g_get_home_dir (), "Dossier personnel", "user-home", NULL);
    const struct { GUserDirectory d; const char *nom; const char *ic; } perso[] = {
        { G_USER_DIRECTORY_DESKTOP,   "Bureau",        "user-desktop"   },
        { G_USER_DIRECTORY_DOWNLOAD,  "Téléchargements","folder-download" },
        { G_USER_DIRECTORY_DOCUMENTS, "Documents",     "folder-documents" },
        { G_USER_DIRECTORY_PICTURES,  "Images",        "folder-pictures" },
        { G_USER_DIRECTORY_MUSIC,     "Musique",       "folder-music"   },
        { G_USER_DIRECTORY_VIDEOS,    "Vidéos",        "folder-videos"  },
    };
    for (guint i = 0; i < G_N_ELEMENTS (perso); i++)
        ajouter_chemin (L, g_get_user_special_dir (perso[i].d), perso[i].nom,
                        perso[i].ic, NULL);

    g_autofree char *corbeille = g_build_filename (g_get_user_data_dir (),
                                                   "Trash", "files", NULL);
    /* Creee au besoin : sans elle, la Corbeille n'apparaitrait pas tant que
     * rien n'a jamais ete jete, ce qui est deroutant. */
    g_mkdir_with_parents (corbeille, 0700);
    ajouter_chemin (L, corbeille, "Corbeille", "user-trash", NULL);

    ajouter_chemin (L, "/", "Ordinateur", "drive-harddisk", NULL);

    /* --- favoris --- */
    if (L->favoris->len > 0) {
        gtk_list_box_append (GTK_LIST_BOX (L->liste), entete ("Favoris"));
        for (guint i = 0; i < L->favoris->len; i++) {
            const char *chemin = g_ptr_array_index (L->favoris, i);
            g_autofree char *nom = g_path_get_basename (chemin);
            ajouter_chemin (L, chemin, nom, "folder", chemin);
        }
    }

    /* --- peripheriques --- */
    GList *montages = g_volume_monitor_get_mounts (L->moniteur);
    GList *volumes  = g_volume_monitor_get_volumes (L->moniteur);
    gboolean titre = FALSE;

    for (GList *m = montages; m != NULL; m = m->next) {
        GMount *mnt = m->data;
        if (g_mount_is_shadowed (mnt))
            continue;

        if (!titre) {
            gtk_list_box_append (GTK_LIST_BOX (L->liste), entete ("Périphériques"));
            titre = TRUE;
        }
        g_autofree char *nom = g_mount_get_name (mnt);
        g_autoptr(GIcon) ic = g_mount_get_icon (mnt);
        g_autoptr(GFile) racine = g_mount_get_root (mnt);
        gtk_list_box_append (GTK_LIST_BOX (L->liste),
                             entree (L, nom, ic, racine, NULL, mnt, NULL));
    }

    /* Volumes connus mais pas montes : une cle branchee que personne n'a
     * encore ouverte. Les afficher permet de la monter d'un clic. */
    for (GList *v = volumes; v != NULL; v = v->next) {
        GVolume *vol = v->data;
        g_autoptr(GMount) deja = g_volume_get_mount (vol);
        if (deja != NULL || !g_volume_can_mount (vol))
            continue;

        if (!titre) {
            gtk_list_box_append (GTK_LIST_BOX (L->liste), entete ("Périphériques"));
            titre = TRUE;
        }
        g_autofree char *nom = g_volume_get_name (vol);
        g_autoptr(GIcon) ic = g_volume_get_icon (vol);
        gtk_list_box_append (GTK_LIST_BOX (L->liste),
                             entree (L, nom, ic, NULL, vol, NULL, NULL));
    }

    g_list_free_full (montages, g_object_unref);
    g_list_free_full (volumes, g_object_unref);

    /* --- le nuage --- */
    /* Google Drive et OneDrive viendront ici, lus depuis la configuration de
     * rclone : une section de plus, construite comme les autres. Rien n'est
     * affiche tant que rien n'est configure -- une entree qui ne mene nulle
     * part serait pire que son absence. */

    fichiers_lieux_suivre (L->boite, L->courant);
}

static void
on_volumes_changes (GVolumeMonitor *m, gpointer objet, gpointer data)
{
    (void) m; (void) objet;
    reconstruire (data);
}

/* ------------------------------------------------------------------------- */
void
fichiers_lieux_suivre (GtkWidget *widget, GFile *dossier)
{
    Lieux *L = g_object_get_data (G_OBJECT (widget), "lieux");
    if (L == NULL)
        return;

    if (dossier != L->courant) {
        g_clear_object (&L->courant);
        L->courant = dossier ? g_object_ref (dossier) : NULL;
    }

    gtk_list_box_unselect_all (GTK_LIST_BOX (L->liste));
    if (L->courant == NULL)
        return;

    for (GtkWidget *r = gtk_widget_get_first_child (L->liste);
         r != NULL; r = gtk_widget_get_next_sibling (r)) {
        GFile *f = g_object_get_data (G_OBJECT (r), "fichier");
        if (f != NULL && g_file_equal (f, L->courant)) {
            gtk_list_box_select_row (GTK_LIST_BOX (L->liste), GTK_LIST_BOX_ROW (r));
            return;
        }
    }
}

gboolean
fichiers_lieux_est_favori (GtkWidget *widget, GFile *dossier)
{
    Lieux *L = g_object_get_data (G_OBJECT (widget), "lieux");
    g_autofree char *chemin = g_file_get_path (dossier);
    if (L == NULL || chemin == NULL)
        return FALSE;

    for (guint i = 0; i < L->favoris->len; i++)
        if (g_strcmp0 (g_ptr_array_index (L->favoris, i), chemin) == 0)
            return TRUE;
    return FALSE;
}

void
fichiers_lieux_ajouter (GtkWidget *widget, GFile *dossier)
{
    Lieux *L = g_object_get_data (G_OBJECT (widget), "lieux");
    g_autofree char *chemin = g_file_get_path (dossier);

    if (L == NULL || chemin == NULL || fichiers_lieux_est_favori (widget, dossier))
        return;

    g_ptr_array_add (L->favoris, g_strdup (chemin));
    favoris_ecrire (L);
    reconstruire (L);
}

void
fichiers_lieux_retirer (GtkWidget *widget, GFile *dossier)
{
    Lieux *L = g_object_get_data (G_OBJECT (widget), "lieux");
    g_autofree char *chemin = g_file_get_path (dossier);
    if (L == NULL || chemin == NULL)
        return;

    for (guint i = 0; i < L->favoris->len; i++) {
        if (g_strcmp0 (g_ptr_array_index (L->favoris, i), chemin) == 0) {
            g_ptr_array_remove_index (L->favoris, i);
            favoris_ecrire (L);
            reconstruire (L);
            return;
        }
    }
}

static void
lieux_free (gpointer data)
{
    Lieux *L = data;
    g_clear_object (&L->moniteur);
    g_clear_object (&L->courant);
    g_ptr_array_unref (L->favoris);
    g_free (L);
}

GtkWidget *
fichiers_lieux_new (LieuxNavFunc nav, gpointer data)
{
    Lieux *L = g_new0 (Lieux, 1);
    L->nav     = nav;
    L->data    = data;
    L->favoris = g_ptr_array_new_with_free_func (g_free);
    favoris_lire (L);

    L->liste = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (L->liste), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class (L->liste, "lieux");
    g_signal_connect (L->liste, "row-activated", G_CALLBACK (on_ligne_activee), L);

    GtkWidget *defil = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (defil),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (defil), L->liste);
    gtk_widget_set_vexpand (defil, TRUE);

    L->boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (L->boite, "lieux-volet");
    gtk_box_append (GTK_BOX (L->boite), defil);
    gtk_widget_set_size_request (L->boite, 190, -1);

    g_object_set_data_full (G_OBJECT (L->boite), "lieux", L, lieux_free);

    /* Le moniteur previent quand une cle est branchee ou retiree. Aucune
     * consultation periodique : on ne se reveille que sur evenement. */
    L->moniteur = g_volume_monitor_get ();
    const char *signaux[] = { "mount-added", "mount-removed", "mount-changed",
                              "volume-added", "volume-removed", NULL };
    for (guint i = 0; signaux[i] != NULL; i++)
        g_signal_connect (L->moniteur, signaux[i],
                          G_CALLBACK (on_volumes_changes), L);

    GSimpleActionGroup *groupe = g_simple_action_group_new ();
    const GActionEntry actions[] = {
        { "retirer", on_retirer_favori, "s", NULL, NULL, { 0 } },
    };
    g_action_map_add_action_entries (G_ACTION_MAP (groupe), actions,
                                     G_N_ELEMENTS (actions), L);
    gtk_widget_insert_action_group (L->boite, "lieux", G_ACTION_GROUP (groupe));
    g_object_unref (groupe);

    reconstruire (L);
    return L->boite;
}
