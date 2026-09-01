/* =========================================================================
 * Claude-OS Shell — dock central
 *
 * Barre d'icones centree en bas de l'ecran, ancree hors du flux des fenetres
 * par le protocole layer-shell. Inspiration : dock macOS pour la position et
 * l'agrandissement au survol, surfaces ChromeOS pour les couleurs.
 *
 * Principe d'energie : aucune minuterie, aucune boucle. Le dock ne fait rien
 * tant que l'utilisateur ne le touche pas ; les transitions sont portees par
 * le moteur CSS de GTK et ne s'executent que pendant le survol.
 * ========================================================================= */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
/* GDesktopAppInfo vit dans gio-unix, pas dans gio tout court. */
#include <gio/gdesktopappinfo.h>

#define DOCK_ICON_SIZE 38          /* pictogramme dans un bouton de 52 px    */

/* -------------------------------------------------------------------------
 * Configuration
 *
 * ~/.config/claude-os/shell.conf, au format cle-valeur de GLib. Absent, les
 * valeurs par defaut ci-dessous s'appliquent : le shell doit fonctionner
 * sans qu'aucun fichier n'ait ete ecrit.
 * ------------------------------------------------------------------------- */

typedef struct {
    char    **pinned;      /* identifiants .desktop, dans l'ordre d'affichage */
    char     *font;        /* famille de police de l'interface                */
    char     *icon_theme;  /* theme d'icones                                  */
    gboolean  dark;        /* theme sombre                                    */
} ShellConfig;

/* Applications epinglees par defaut : celles pour lesquelles ce systeme
 * existe, plus de quoi ouvrir un dossier et un terminal. */
static const char *default_pinned[] = {
    "chromium", "claude-desktop", "thunar", "xfce4-terminal", NULL
};

static char *
config_path (void)
{
    return g_build_filename (g_get_user_config_dir (),
                             "claude-os", "shell.conf", NULL);
}

static ShellConfig *
config_load (void)
{
    ShellConfig *cfg = g_new0 (ShellConfig, 1);
    g_autoptr(GKeyFile) kf = g_key_file_new ();
    g_autofree char *path = config_path ();

    /* Valeurs par defaut, ecrasees ensuite si le fichier en fournit. */
    cfg->pinned = g_strdupv ((char **) default_pinned);
    cfg->font   = g_strdup ("Inter");
    /* Papirus plutot qu'Adwaita : Adwaita a abandonne les noms d'icones
     * herites (web-browser, utilities-terminal...) que la plupart des
     * fichiers .desktop declarent encore, et affiche donc un pictogramme
     * generique pour la moitie des applications. Papirus les conserve, et
     * son style plat et arrondi est plus proche de ChromeOS. */
    cfg->icon_theme = g_strdup ("Papirus");
    cfg->dark   = FALSE;

    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
        return cfg;   /* pas de fichier : les defauts suffisent */

    g_auto(GStrv) pinned = g_key_file_get_string_list (kf, "dock", "pinned",
                                                       NULL, NULL);
    if (pinned != NULL && pinned[0] != NULL) {
        g_strfreev (cfg->pinned);
        cfg->pinned = g_steal_pointer (&pinned);
    }

    g_autofree char *font = g_key_file_get_string (kf, "appearance", "font", NULL);
    if (font != NULL && *font != '\0') {
        g_free (cfg->font);
        cfg->font = g_steal_pointer (&font);
    }

    g_autofree char *icons = g_key_file_get_string (kf, "appearance", "icon_theme", NULL);
    if (icons != NULL && *icons != '\0') {
        g_free (cfg->icon_theme);
        cfg->icon_theme = g_steal_pointer (&icons);
    }

    g_autofree char *theme = g_key_file_get_string (kf, "appearance", "theme", NULL);
    if (g_strcmp0 (theme, "dark") == 0)
        cfg->dark = TRUE;

    return cfg;
}

/* -------------------------------------------------------------------------
 * Lancement d'une application
 * ------------------------------------------------------------------------- */
static GDesktopAppInfo *
app_info_for (const char *app_id)
{
    g_autofree char *desktop_id = g_strconcat (app_id, ".desktop", NULL);
    return g_desktop_app_info_new (desktop_id);
}

static void
on_item_clicked (GtkButton *button, gpointer user_data)
{
    const char *app_id = user_data;
    g_autoptr(GError) error = NULL;

    /* g_app_info_launch gere le .desktop, les variables d'environnement et
     * le rattachement au bon cgroup. Bien preferable a un fork/exec brut. */
    g_autoptr(GDesktopAppInfo) info = app_info_for (app_id);

    if (info == NULL) {
        g_warning ("aucun fichier .desktop pour « %s »", app_id);
        return;
    }
    if (!g_app_info_launch (G_APP_INFO (info), NULL, NULL, &error))
        g_warning ("lancement de « %s » impossible : %s", app_id, error->message);

    (void) button;
}

/* -------------------------------------------------------------------------
 * Construction d'une icone du dock
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_dock_item (const char *app_id, gboolean running)
{
    g_autoptr(GDesktopAppInfo) info = app_info_for (app_id);

    /* Icone et libelle proviennent du .desktop : c'est la source d'autorite,
     * et cela evite de maintenir une table parallele qui se desynchronise. */
    g_autofree char *icon_from_desktop = NULL;
    const char *label = app_id;
    if (info != NULL) {
        GIcon *gicon = g_app_info_get_icon (G_APP_INFO (info));
        if (gicon != NULL)
            icon_from_desktop = g_icon_to_string (gicon);
        const char *name = g_app_info_get_display_name (G_APP_INFO (info));
        if (name != NULL)
            label = name;
    }
    const char *wanted_icon = icon_from_desktop ? icon_from_desktop : app_id;

    GtkWidget *box    = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *button = gtk_button_new ();

    /* Repli explicite : sans lui, une icone absente du theme affiche un carre
     * barre, ce qui est bien plus laid qu'un pictogramme generique. */
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
    const char *icon_name = gtk_icon_theme_has_icon (theme, wanted_icon)
                          ? wanted_icon
                          : "application-x-executable";
    GtkWidget *image = gtk_image_new_from_icon_name (icon_name);

    gtk_image_set_pixel_size (GTK_IMAGE (image), DOCK_ICON_SIZE);
    gtk_button_set_child (GTK_BUTTON (button), image);
    gtk_widget_add_css_class (button, "dock-item");
    gtk_widget_set_tooltip_text (button, label);

    /* La chaine appartient au tableau de configuration, qui vit aussi
     * longtemps que l'application : la passer telle quelle est sur. */
    g_signal_connect (button, "clicked",
                      G_CALLBACK (on_item_clicked), (gpointer) app_id);

    /* Point d'etat sous l'icone : present pour toutes les entrees afin que
     * la hauteur du dock ne change pas selon les applications ouvertes --
     * un dock qui grandit et retrecit est desagreable a l'usage. */
    GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (dot, "dock-indicator");
    if (running)
        gtk_widget_add_css_class (dot, "active");
    else
        gtk_widget_set_opacity (dot, 0.0);
    gtk_widget_set_halign (dot, GTK_ALIGN_CENTER);

    gtk_box_append (GTK_BOX (box), button);
    gtk_box_append (GTK_BOX (box), dot);
    return box;
}

/* -------------------------------------------------------------------------
 * Feuilles de style
 * ------------------------------------------------------------------------- */
static void
load_styles (void)
{
    const char *files[] = { "style/tokens.css", "style/shell.css" };

    for (guint i = 0; i < G_N_ELEMENTS (files); i++) {
        GtkCssProvider *provider = gtk_css_provider_new ();
        g_autofree char *path = g_build_filename (SHELL_DATA_DIR, files[i], NULL);

        gtk_css_provider_load_from_path (provider, path);
        gtk_style_context_add_provider_for_display (
            gdk_display_get_default (),
            GTK_STYLE_PROVIDER (provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref (provider);
    }
}

/* -------------------------------------------------------------------------
 * Fenetre du dock
 * ------------------------------------------------------------------------- */
static void
on_activate (GtkApplication *app, gpointer user_data)
{
    ShellConfig *cfg = user_data;

    /* Le theme d'icones doit etre pose AVANT de construire les boutons :
     * gtk_icon_theme_has_icon interroge le theme actif. */
    if (cfg->icon_theme != NULL && *cfg->icon_theme != '\0')
        g_object_set (gtk_settings_get_default (),
                      "gtk-icon-theme-name", cfg->icon_theme, NULL);

    GtkWidget *window = gtk_application_window_new (app);
    gtk_widget_add_css_class (window, "shell");
    if (cfg->dark)
        gtk_widget_add_css_class (window, "dark");

    /* Police de l'interface : appliquee par une regle CSS injectee, ce qui
     * evite de dependre des reglages GTK globaux du systeme. */
    if (cfg->font != NULL && *cfg->font != '\0') {
        g_autofree char *rule = g_strdup_printf ("window.shell { font-family: \"%s\"; }",
                                                 cfg->font);
        GtkCssProvider *fp = gtk_css_provider_new ();
        gtk_css_provider_load_from_string (fp, rule);
        gtk_style_context_add_provider_for_display (
            gdk_display_get_default (), GTK_STYLE_PROVIDER (fp),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        g_object_unref (fp);
    }

    /* --- Ancrage layer-shell ---------------------------------------------
     * Sans cela, le dock serait une fenetre ordinaire : elle passerait
     * derriere les autres et apparaitrait dans la liste des fenetres. */
    gtk_layer_init_for_window (GTK_WINDOW (window));
    gtk_layer_set_layer (GTK_WINDOW (window), GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor (GTK_WINDOW (window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_namespace (GTK_WINDOW (window), "claude-os-dock");

    /* Zone reservee : les fenetres maximisees s'arretent au-dessus du dock.
     * 68 px de dock + 10 px de marge + 2 px de respiration. */
    gtk_layer_set_exclusive_zone (GTK_WINDOW (window), 86);

    /* Le dock ne prend le clavier a aucun moment : la saisie continue d'aller
     * a la fenetre active meme quand la souris le survole. */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (window),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GtkWidget *dock = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (dock, "dock");
    gtk_widget_set_halign (dock, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (dock, GTK_ALIGN_END);

    for (guint i = 0; cfg->pinned[i] != NULL; i++)
        gtk_box_append (GTK_BOX (dock), build_dock_item (cfg->pinned[i], FALSE));

    gtk_window_set_child (GTK_WINDOW (window), dock);
    gtk_window_present (GTK_WINDOW (window));
}

int
main (int argc, char **argv)
{
    ShellConfig *cfg = config_load ();

    /* Les options de ligne de commande priment sur le fichier : pratique
     * pour essayer un theme sans toucher a sa configuration. */
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0 (argv[i], "--dark") == 0)  cfg->dark = TRUE;
        if (g_strcmp0 (argv[i], "--light") == 0) cfg->dark = FALSE;
    }

    GtkApplication *app = gtk_application_new ("os.claude.shell.dock",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "startup",  G_CALLBACK (load_styles), NULL);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), cfg);

    /* Les arguments sont deja traites ci-dessus ; on n'en passe aucun a GTK
     * pour eviter qu'il ne rejette --dark comme option inconnue. */
    int status = g_application_run (G_APPLICATION (app), 0, NULL);
    g_object_unref (app);
    return status;
}
