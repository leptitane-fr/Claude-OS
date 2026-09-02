/* =========================================================================
 * Claude-OS Shell — fond d'ecran
 *
 * Une surface layer-shell sur la couche la plus basse, ancree aux quatre
 * bords. Sans image configuree, elle dessine un degrade sombre concu ici :
 * la distribution doit etre presentable des le premier demarrage, sans
 * embarquer de photographie ni dependre de swaybg.
 *
 * Discipline d'energie : rien ne bouge. Le degrade est compose une fois par
 * le compositeur, l'image decodee une fois au chargement. Un fond anime
 * serait le plus sur moyen de reveiller le GPU en permanence sur une
 * machine dont l'autonomie est la raison d'etre.
 * ========================================================================= */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

#include "config.h"

typedef struct {
    GtkWidget *window;
    GtkWidget *pile;     /* degrade dessous, image par-dessus si presente   */
    GtkWidget *image;
} Wallpaper;

static void
wallpaper_apply (Wallpaper *w, const ShellConfig *cfg)
{
    gboolean has_image = (cfg->wallpaper != NULL && *cfg->wallpaper != '\0'
                          && g_file_test (cfg->wallpaper, G_FILE_TEST_IS_REGULAR));

    if (!has_image) {
        /* Le degrade reste dessous en permanence : il sert aussi de repli
         * si l'image disparait ou ne se decode pas. */
        gtk_widget_set_visible (w->image, FALSE);
        return;
    }

    g_autoptr(GError) error = NULL;
    g_autoptr(GdkTexture) texture = gdk_texture_new_from_filename (cfg->wallpaper, &error);
    if (texture == NULL) {
        g_message ("fond d'ecran illisible (%s) : %s", cfg->wallpaper, error->message);
        gtk_widget_set_visible (w->image, FALSE);
        return;
    }

    gtk_picture_set_paintable (GTK_PICTURE (w->image), GDK_PAINTABLE (texture));
    /* COVER rogne pour remplir, CONTAIN montre tout quitte a laisser du
     * degrade autour -- ce qui est preferable a des bandes noires. */
    gtk_picture_set_content_fit (GTK_PICTURE (w->image),
                                 cfg->wallpaper_fill ? GTK_CONTENT_FIT_COVER
                                                     : GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_visible (w->image, TRUE);
}

static void
on_config_reloaded (ShellConfig *cfg, gpointer data)
{
    Wallpaper *w = data;

    if (cfg->dark)
        gtk_widget_add_css_class (w->window, "dark");
    else
        gtk_widget_remove_css_class (w->window, "dark");

    wallpaper_apply (w, cfg);
    shell_config_free (cfg);
}

static void
on_activate (GtkApplication *app, gpointer user_data)
{
    ShellConfig *cfg = user_data;
    Wallpaper *w = g_new0 (Wallpaper, 1);

    w->window = gtk_application_window_new (app);
    gtk_widget_add_css_class (w->window, "shell");
    gtk_widget_add_css_class (w->window, "fond");
    if (cfg->dark)
        gtk_widget_add_css_class (w->window, "dark");

    gtk_layer_init_for_window (GTK_WINDOW (w->window));
    /* BACKGROUND : sous toutes les fenetres, y compris les autres surfaces
     * du shell. */
    gtk_layer_set_layer (GTK_WINDOW (w->window), GTK_LAYER_SHELL_LAYER_BACKGROUND);
    for (int edge = 0; edge < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; edge++)
        gtk_layer_set_anchor (GTK_WINDOW (w->window), edge, TRUE);
    gtk_layer_set_namespace (GTK_WINDOW (w->window), "claude-os-fond");
    /* -1 : le fond couvre l'ecran entier et ne reserve rien. Avec 0 il
     * s'arreterait aux zones reservees par les autres surfaces. */
    gtk_layer_set_exclusive_zone (GTK_WINDOW (w->window), -1);
    gtk_layer_set_keyboard_mode (GTK_WINDOW (w->window),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    w->image = gtk_picture_new ();
    gtk_widget_set_visible (w->image, FALSE);

    w->pile = gtk_overlay_new ();
    gtk_widget_add_css_class (w->pile, "fond-degrade");
    gtk_overlay_add_overlay (GTK_OVERLAY (w->pile), w->image);

    gtk_window_set_child (GTK_WINDOW (w->window), w->pile);
    wallpaper_apply (w, cfg);
    gtk_window_present (GTK_WINDOW (w->window));

    shell_config_watch (on_config_reloaded, w);
}

int
main (int argc, char **argv)
{
    (void) argc; (void) argv;

    ShellConfig *cfg = shell_config_load ();
    GtkApplication *app = gtk_application_new ("os.claude.shell.fond",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "startup",  G_CALLBACK (shell_styles_load), NULL);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), cfg);

    int status = g_application_run (G_APPLICATION (app), 0, NULL);
    g_object_unref (app);
    return status;
}
