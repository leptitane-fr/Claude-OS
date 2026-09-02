/* =========================================================================
 * Claude-OS Shell — panneau de reglages
 *
 * Une fenetre ordinaire, pas une surface layer-shell : elle se lance a la
 * demande, se ferme, et ne reside pas en memoire.
 *
 * ELLE NE DETIENT AUCUN ETAT
 *
 * Chaque reglage relit shell.conf, modifie la seule cle concernee, et
 * reecrit. Garder une configuration en memoire pendant que le panneau est
 * ouvert paraitrait plus simple, mais le dock ecrit lui aussi dans ce
 * fichier -- l'ordre des icones change au glisser-deposer. Le panneau
 * ecraserait alors une reorganisation faite pendant qu'il etait ouvert.
 *
 * Il n'y a pas de bouton « Appliquer » : les composants surveillent
 * shell.conf et se reappliquent aussitot. On voit le resultat en le
 * choisissant, ce qui est le seul moyen de juger une apparence.
 * ========================================================================= */

#include <gtk/gtk.h>

#include "config.h"

/* -------------------------------------------------------------------------
 * Enregistrement
 * ------------------------------------------------------------------------- */
typedef void (*Modif) (ShellConfig *cfg, gpointer data);

static void
modifier (Modif apply, gpointer data)
{
    ShellConfig *cfg = shell_config_load ();
    apply (cfg, data);

    g_autoptr(GError) error = NULL;
    if (!shell_config_save (cfg, &error))
        g_warning ("enregistrement impossible : %s", error->message);

    shell_config_free (cfg);
}

static void set_theme     (ShellConfig *c, gpointer d) { g_free (c->theme);      c->theme      = g_strdup (d); }
static void set_reserve   (ShellConfig *c, gpointer d) { c->reserve_space = GPOINTER_TO_INT (d); }
static void set_fill      (ShellConfig *c, gpointer d) { c->wallpaper_fill = GPOINTER_TO_INT (d); }
static void set_font      (ShellConfig *c, gpointer d) { g_free (c->font);       c->font       = g_strdup (d); }
static void set_icons     (ShellConfig *c, gpointer d) { g_free (c->icon_theme); c->icon_theme = g_strdup (d); }
static void set_wallpaper (ShellConfig *c, gpointer d) { g_free (c->wallpaper);  c->wallpaper  = g_strdup (d); }

/* -------------------------------------------------------------------------
 * Fabrique de lignes : libelle a gauche, controle a droite
 * ------------------------------------------------------------------------- */
static GtkWidget *
carte (const char *titre)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class (box, "reglages-carte");

    GtkWidget *t = gtk_label_new (titre);
    gtk_widget_add_css_class (t, "reglages-titre");
    gtk_widget_set_halign (t, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (box), t);

    return box;
}

static GtkWidget *
ligne (GtkWidget *carte, const char *libelle, const char *detail, GtkWidget *controle)
{
    GtkWidget *textes = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *l = gtk_label_new (libelle);
    gtk_widget_add_css_class (l, "reglages-libelle");
    gtk_widget_set_halign (l, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (textes), l);

    if (detail != NULL) {
        GtkWidget *d = gtk_label_new (detail);
        gtk_widget_add_css_class (d, "reglages-detail");
        gtk_widget_set_halign (d, GTK_ALIGN_START);
        gtk_label_set_wrap (GTK_LABEL (d), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (d), 42);
        gtk_box_append (GTK_BOX (textes), d);
    }

    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class (row, "reglages-ligne");
    gtk_widget_set_hexpand (textes, TRUE);
    gtk_box_append (GTK_BOX (row), textes);
    gtk_widget_set_valign (controle, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (row), controle);

    gtk_box_append (GTK_BOX (carte), row);
    return row;
}

/* -------------------------------------------------------------------------
 * Apparence
 * ------------------------------------------------------------------------- */

/* Positionne une liste deroulante sur une valeur, si elle s'y trouve.
 * A appeler AVANT de brancher notify::selected : sinon la selection initiale
 * declencherait un enregistrement a la simple ouverture du panneau. */
static void
selectionner (GtkDropDown *dd, const char *valeur)
{
    GListModel *m = gtk_drop_down_get_model (dd);

    for (guint i = 0; i < g_list_model_get_n_items (m); i++) {
        g_autoptr(GtkStringObject) o = g_list_model_get_item (m, i);
        if (g_strcmp0 (gtk_string_object_get_string (o), valeur) == 0) {
            gtk_drop_down_set_selected (dd, i);
            return;
        }
    }

    /* Valeur configuree absente de la liste -- une police desinstallee, par
     * exemple. On l'ajoute en tete plutot que de laisser la liste afficher
     * son premier element : le panneau doit montrer ce qui est reglé, pas
     * ce qui se trouve etre disponible. */
    if (valeur != NULL && *valeur != '\0') {
        gtk_string_list_splice (GTK_STRING_LIST (m), 0, 0,
                                (const char *[]) { valeur, NULL });
        gtk_drop_down_set_selected (dd, 0);
    }
}
static void
on_theme (GObject *dd, GParamSpec *ps, gpointer data)
{
    (void) ps; (void) data;

    guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (dd));
    if (i == GTK_INVALID_LIST_POSITION)
        return;

    const ShellTheme *t = &shell_themes ()[i];
    modifier (set_theme, (gpointer) t->id);

    /* Le panneau se repeint lui-meme sans attendre : il est la fenetre que
     * l'utilisateur regarde en choisissant, ce serait etrange qu'elle soit
     * la derniere a changer. */
    shell_styles_load (t->id);
    g_object_set (gtk_settings_get_default (),
                  "gtk-application-prefer-dark-theme", t->sombre, NULL);
}

/* Une liste de familles plutot qu'un GtkFontDialogButton, pour trois
 * raisons. Seule la FAMILLE nous interesse -- la taille est fixee par la
 * feuille de style, composant par composant, et laisser choisir une taille
 * ici ne ferait que promettre un reglage sans effet. La liste a la meme
 * allure que celle des themes d'icones, juste en dessous. Et le bouton de
 * GTK affichait « None » tant qu'on ne lui donnait pas une taille, en
 * signalant au passage un g_list_model_get_n_items sur un modele pas encore
 * pret -- deux symptomes pour une commodite dont on n'a pas l'usage. */
#define AUTO_POLICE "Automatique (selon le thème)"

static int
comparer_noms (gconstpointer a, gconstpointer b)
{
    return g_utf8_collate (*(const char * const *) a, *(const char * const *) b);
}

static GtkStringList *
familles_polices (GtkWidget *widget)
{
    PangoFontFamily **familles = NULL;
    int n = 0;
    pango_context_list_families (gtk_widget_get_pango_context (widget),
                                 &familles, &n);

    g_autoptr(GPtrArray) noms = g_ptr_array_new ();
    for (int i = 0; i < n; i++)
        g_ptr_array_add (noms, (gpointer) pango_font_family_get_name (familles[i]));
    g_free (familles);

    g_ptr_array_sort (noms, comparer_noms);
    /* En tete, le choix « laisser le theme decider » : c'est le defaut, et
     * c'est ce qui donne sa police propre a chaque theme. */
    g_ptr_array_insert (noms, 0, (gpointer) AUTO_POLICE);
    g_ptr_array_add (noms, NULL);

    return gtk_string_list_new ((const char * const *) noms->pdata);
}

static void
on_police (GObject *dd, GParamSpec *ps, gpointer data)
{
    (void) ps; (void) data;

    GtkStringObject *sel = gtk_drop_down_get_selected_item (GTK_DROP_DOWN (dd));
    if (sel == NULL)
        return;

    const char *choix = gtk_string_object_get_string (sel);
    modifier (set_font,
              (gpointer) (g_strcmp0 (choix, AUTO_POLICE) == 0 ? "" : choix));
}

/* Themes d'icones installes : un repertoire avec un index.theme.
 * hicolor est exclu -- c'est le repli commun, pas un theme utilisable seul. */
static GtkStringList *
themes_icones (void)
{
    GtkStringList *liste = gtk_string_list_new (NULL);
    const char *bases[] = { "/usr/share/icons", "/usr/local/share/icons", NULL };
    /* Les cles sont dupliquees : g_dir_read_name reutilise son tampon, la
     * chaine rendue est invalidee au tour suivant. La stocker telle quelle
     * laisserait des pointeurs morts dans la table. */
    g_autoptr(GHashTable) vus =
        g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    for (guint b = 0; bases[b] != NULL; b++) {
        g_autoptr(GDir) dir = g_dir_open (bases[b], 0, NULL);
        if (dir == NULL)
            continue;

        const char *nom;
        while ((nom = g_dir_read_name (dir)) != NULL) {
            if (g_strcmp0 (nom, "hicolor") == 0 || g_strcmp0 (nom, "default") == 0)
                continue;
            if (g_hash_table_contains (vus, nom))
                continue;

            g_autofree char *index = g_build_filename (bases[b], nom, "index.theme", NULL);
            if (!g_file_test (index, G_FILE_TEST_EXISTS))
                continue;

            g_hash_table_add (vus, g_strdup (nom));
            gtk_string_list_append (liste, nom);
        }
    }
    return liste;
}

static void
on_icones (GObject *dd, GParamSpec *ps, gpointer data)
{
    (void) ps; (void) data;

    GtkStringObject *sel = gtk_drop_down_get_selected_item (GTK_DROP_DOWN (dd));
    if (sel != NULL)
        modifier (set_icons, (gpointer) gtk_string_object_get_string (sel));
}

/* -------------------------------------------------------------------------
 * Fond d'ecran
 * ------------------------------------------------------------------------- */
static void
on_image_choisie (GObject *dialog, GAsyncResult *res, gpointer data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file =
        gtk_file_dialog_open_finish (GTK_FILE_DIALOG (dialog), res, &error);

    /* L'annulation par l'utilisateur remonte comme une erreur : ce n'est pas
     * un incident, on ne l'annonce pas. */
    if (file == NULL)
        return;

    g_autofree char *path = g_file_get_path (file);
    if (path == NULL)
        return;

    modifier (set_wallpaper, path);
    gtk_label_set_text (GTK_LABEL (data), path);
}

static void
on_choisir_image (GtkButton *b, gpointer data)
{
    GtkFileDialog *d = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (d, "Choisir un fond d'écran");

    GtkFileFilter *f = gtk_file_filter_new ();
    gtk_file_filter_set_name (f, "Images");
    gtk_file_filter_add_pixbuf_formats (f);
    g_autoptr(GListStore) filtres = g_list_store_new (GTK_TYPE_FILE_FILTER);
    g_list_store_append (filtres, f);
    gtk_file_dialog_set_filters (d, G_LIST_MODEL (filtres));

    GtkWindow *parent = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (b)));
    gtk_file_dialog_open (d, parent, NULL, on_image_choisie, data);
    g_object_unref (d);
}

static void
on_degrade (GtkButton *b, gpointer data)
{
    (void) b;
    modifier (set_wallpaper, (gpointer) "");
    gtk_label_set_text (GTK_LABEL (data), "Dégradé dessiné par le shell");
}

/* -------------------------------------------------------------------------
 * Commutateurs
 * ------------------------------------------------------------------------- */
static void
on_switch (GObject *sw, GParamSpec *ps, gpointer data)
{
    (void) ps;
    Modif apply = g_object_get_data (sw, "apply");
    gboolean on = gtk_switch_get_active (GTK_SWITCH (sw));
    (void) data;
    modifier (apply, GINT_TO_POINTER (on));
}

static GtkWidget *
commutateur (gboolean actif, Modif apply)
{
    GtkWidget *sw = gtk_switch_new ();
    gtk_switch_set_active (GTK_SWITCH (sw), actif);
    g_object_set_data (G_OBJECT (sw), "apply", apply);
    g_signal_connect (sw, "notify::active", G_CALLBACK (on_switch), NULL);
    return sw;
}

/* ------------------------------------------------------------------------- */
static void
on_activate (GtkApplication *app, gpointer user_data)
{
    ShellConfig *cfg = user_data;

    shell_config_apply (cfg);

    /* Le panneau utilise des widgets GTK ordinaires -- listes deroulantes,
     * commutateurs, selecteur de fichier -- que notre feuille de style ne
     * redessine pas. Sans cela, la barre de titre et les boutons resteraient
     * clairs sur un panneau sombre. */
    g_object_set (gtk_settings_get_default (),
                  "gtk-application-prefer-dark-theme", cfg->dark, NULL);

    GtkWidget *window = gtk_application_window_new (app);
    gtk_widget_add_css_class (window, "shell");
    gtk_widget_add_css_class (window, "reglages");
    gtk_window_set_title (GTK_WINDOW (window), "Réglages");
    gtk_window_set_default_size (GTK_WINDOW (window), 520, 600);

    GtkWidget *pile = gtk_box_new (GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class (pile, "reglages-pile");

    /* --- Apparence --- */
    GtkWidget *apparence = carte ("Apparence");

    /* La liste suit l'ordre de la table des themes : l'indice choisi y
     * renvoie directement, sans table de correspondance a maintenir. */
    GtkStringList *noms = gtk_string_list_new (NULL);
    guint choisi = 0;
    for (guint i = 0; shell_themes ()[i].id != NULL; i++) {
        gtk_string_list_append (noms, shell_themes ()[i].nom);
        if (g_strcmp0 (shell_themes ()[i].id, cfg->theme) == 0)
            choisi = i;
    }
    GtkWidget *theme = gtk_drop_down_new (G_LIST_MODEL (noms), NULL);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (theme), choisi);
    g_signal_connect (theme, "notify::selected", G_CALLBACK (on_theme), NULL);
    ligne (apparence, "Thème",
           "Les thèmes Claude reprennent les couleurs de la charte "
           "d'Anthropic. Hommage, pas habillage officiel.",
           theme);

    GtkStringList *polices = familles_polices (window);
    GtkWidget *police = gtk_drop_down_new (G_LIST_MODEL (polices), NULL);
    /* Recherche au clavier : une machine porte facilement deux cents
     * familles, dérouler la liste entière serait pénible. */
    gtk_drop_down_set_expression (GTK_DROP_DOWN (police),
        gtk_property_expression_new (GTK_TYPE_STRING_OBJECT, NULL, "string"));
    gtk_drop_down_set_enable_search (GTK_DROP_DOWN (police), TRUE);
    selectionner (GTK_DROP_DOWN (police),
                  (cfg->font != NULL && *cfg->font != '\0') ? cfg->font : AUTO_POLICE);
    g_signal_connect (police, "notify::selected", G_CALLBACK (on_police), NULL);
    ligne (apparence, "Police de l'interface",
           "Les thèmes Claude utilisent Lato : les fontes d'Anthropic sont "
           "propriétaires et ne peuvent pas être embarquées.",
           police);

    GtkWidget *icones = gtk_drop_down_new (G_LIST_MODEL (themes_icones ()), NULL);
    selectionner (GTK_DROP_DOWN (icones), cfg->icon_theme);
    g_signal_connect (icones, "notify::selected", G_CALLBACK (on_icones), NULL);
    ligne (apparence, "Thème d'icônes",
           "Papirus conserve les noms d'icônes hérités qu'Adwaita a abandonnés.",
           icones);

    gtk_box_append (GTK_BOX (pile), apparence);

    /* --- Fond d'ecran --- */
    GtkWidget *fond = carte ("Fond d'écran");

    GtkWidget *chemin = gtk_label_new (
        (cfg->wallpaper != NULL && *cfg->wallpaper != '\0')
        ? cfg->wallpaper : "Dégradé dessiné par le shell");
    gtk_widget_add_css_class (chemin, "reglages-detail");
    gtk_label_set_ellipsize (GTK_LABEL (chemin), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars (GTK_LABEL (chemin), 44);
    gtk_widget_set_halign (chemin, GTK_ALIGN_START);

    GtkWidget *choisir = gtk_button_new_with_label ("Choisir une image…");
    g_signal_connect (choisir, "clicked", G_CALLBACK (on_choisir_image), chemin);
    ligne (fond, "Image", NULL, choisir);

    GtkWidget *revenir = gtk_button_new_with_label ("Revenir au dégradé");
    g_signal_connect (revenir, "clicked", G_CALLBACK (on_degrade), chemin);
    ligne (fond, "Par défaut", NULL, revenir);

    ligne (fond, "Couvrir l'écran",
           "Rogne l'image pour remplir. Désactivé, elle est montrée en "
           "entier et le dégradé comble les côtés.",
           commutateur (cfg->wallpaper_fill, set_fill));

    gtk_box_append (GTK_BOX (fond), chemin);
    gtk_box_append (GTK_BOX (pile), fond);

    /* --- Dock --- */
    GtkWidget *dock = carte ("Dock");
    ligne (dock, "Réserver la place du dock",
           "Les fenêtres maximisées s'arrêtent au-dessus. Elles seront "
           "redimensionnées chaque fois que le dock apparaît ou disparaît.",
           commutateur (cfg->reserve_space, set_reserve));
    ligne (dock, "Ordre des icônes",
           "Se règle directement dans le dock, en faisant glisser une icône.",
           gtk_label_new (""));
    gtk_box_append (GTK_BOX (pile), dock);

    GtkWidget *defil = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (defil),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (defil), pile);
    gtk_window_set_child (GTK_WINDOW (window), defil);

    gtk_window_present (GTK_WINDOW (window));
}

int
main (int argc, char **argv)
{
    (void) argc; (void) argv;

    ShellConfig *cfg = shell_config_load ();
    GtkApplication *app = gtk_application_new ("os.claude.shell.reglages",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "startup",  G_CALLBACK (shell_styles_startup), cfg);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), cfg);

    int status = g_application_run (G_APPLICATION (app), 0, NULL);
    g_object_unref (app);
    return status;
}
