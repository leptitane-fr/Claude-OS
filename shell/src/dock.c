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

#define DOCK_ICON_SIZE 36          /* pictogramme dans un bouton de 48 px    */

/* Applications epinglees. Volontairement en dur pour cette premiere version :
 * la lecture d'un fichier de configuration viendra quand la forme visuelle
 * sera arretee, pour ne pas figer un format trop tot. */
typedef struct {
    const char *id;                /* identifiant .desktop                   */
    const char *icon;              /* nom dans le theme d'icones             */
    const char *label;             /* infobulle                              */
} PinnedApp;

static const PinnedApp pinned[] = {
    { "chromium",       "chromium",              "Chromium"       },
    { "claude-desktop", "claude-desktop",        "Claude"         },
    { "thunar",         "system-file-manager",   "Fichiers"       },
    { "xfce4-terminal", "utilities-terminal",    "Terminal"       },
};

/* -------------------------------------------------------------------------
 * Lancement d'une application
 * ------------------------------------------------------------------------- */
static void
on_item_clicked (GtkButton *button, gpointer user_data)
{
    const char *app_id = user_data;
    g_autoptr(GError) error = NULL;

    /* g_app_info_launch gere le .desktop, les variables d'environnement et
     * le rattachement au bon cgroup. Bien preferable a un fork/exec brut. */
    g_autofree char *desktop_id = g_strconcat (app_id, ".desktop", NULL);
    g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new (desktop_id);

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
build_dock_item (const PinnedApp *app, gboolean running)
{
    GtkWidget *box    = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *button = gtk_button_new ();
    GtkWidget *image  = gtk_image_new_from_icon_name (app->icon);

    gtk_image_set_pixel_size (GTK_IMAGE (image), DOCK_ICON_SIZE);
    gtk_button_set_child (GTK_BUTTON (button), image);
    gtk_widget_add_css_class (button, "dock-item");
    gtk_widget_set_tooltip_text (button, app->label);

    g_signal_connect (button, "clicked",
                      G_CALLBACK (on_item_clicked), (gpointer) app->id);

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
    gboolean dark = GPOINTER_TO_INT (user_data);

    GtkWidget *window = gtk_application_window_new (app);
    gtk_widget_add_css_class (window, "shell");
    if (dark)
        gtk_widget_add_css_class (window, "dark");

    /* --- Ancrage layer-shell ---------------------------------------------
     * Sans cela, le dock serait une fenetre ordinaire : elle passerait
     * derriere les autres et apparaitrait dans la liste des fenetres. */
    gtk_layer_init_for_window (GTK_WINDOW (window));
    gtk_layer_set_layer (GTK_WINDOW (window), GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor (GTK_WINDOW (window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_namespace (GTK_WINDOW (window), "claude-os-dock");

    /* Zone reservee : les fenetres maximisees s'arretent au-dessus du dock.
     * 68 px de dock + 10 px de marge + 2 px de respiration. */
    gtk_layer_set_exclusive_zone (GTK_WINDOW (window), 80);

    /* Le dock ne prend le clavier a aucun moment : la saisie continue d'aller
     * a la fenetre active meme quand la souris le survole. */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (window),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GtkWidget *dock = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (dock, "dock");
    gtk_widget_set_halign (dock, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (dock, GTK_ALIGN_END);

    for (guint i = 0; i < G_N_ELEMENTS (pinned); i++)
        gtk_box_append (GTK_BOX (dock), build_dock_item (&pinned[i], FALSE));

    gtk_window_set_child (GTK_WINDOW (window), dock);
    gtk_window_present (GTK_WINDOW (window));
}

int
main (int argc, char **argv)
{
    gboolean dark = FALSE;
    for (int i = 1; i < argc; i++)
        if (g_strcmp0 (argv[i], "--dark") == 0)
            dark = TRUE;

    GtkApplication *app = gtk_application_new ("os.claude.shell.dock",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "startup",  G_CALLBACK (load_styles), NULL);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate),
                      GINT_TO_POINTER (dark));

    /* Les arguments sont deja traites ci-dessus ; on n'en passe aucun a GTK
     * pour eviter qu'il ne rejette --dark comme option inconnue. */
    int status = g_application_run (G_APPLICATION (app), 0, NULL);
    g_object_unref (app);
    return status;
}
