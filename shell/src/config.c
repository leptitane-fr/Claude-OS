#include "config.h"

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

ShellConfig *
shell_config_load (void)
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
    /* Par defaut le dock ne repousse rien : afficher ou masquer le dock ne
     * doit pas redimensionner la fenetre en dessous, il doit passer par
     * dessus. Voir le commentaire de la zone exclusive dans dock.c. */
    cfg->reserve_space = FALSE;

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

    g_autoptr(GError) e = NULL;
    gboolean reserve = g_key_file_get_boolean (kf, "dock", "reserve_space", &e);
    if (e == NULL)
        cfg->reserve_space = reserve;

    return cfg;
}

void
shell_styles_load (void)
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

void
shell_config_apply (const ShellConfig *cfg)
{
    if (cfg->icon_theme != NULL && *cfg->icon_theme != '\0')
        g_object_set (gtk_settings_get_default (),
                      "gtk-icon-theme-name", cfg->icon_theme, NULL);

    /* Police injectee par une regle CSS plutot que par les reglages GTK
     * globaux : le shell garde son apparence meme si le systeme est configure
     * autrement, et une seule source decide de son style. */
    if (cfg->font == NULL || *cfg->font == '\0')
        return;

    g_autofree char *rule = g_strdup_printf ("window.shell { font-family: \"%s\"; }",
                                             cfg->font);
    GtkCssProvider *fp = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (fp, rule);
    gtk_style_context_add_provider_for_display (
        gdk_display_get_default (), GTK_STYLE_PROVIDER (fp),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_unref (fp);
}
