#include "config.h"

#include <errno.h>

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
shell_config_free (ShellConfig *cfg)
{
    if (cfg == NULL)
        return;
    g_strfreev (cfg->pinned);
    g_free (cfg->font);
    g_free (cfg->icon_theme);
    g_free (cfg);
}

gboolean
shell_config_save (const ShellConfig *cfg, GError **error)
{
    g_autofree char *path = config_path ();
    g_autofree char *dir  = g_path_get_dirname (path);

    if (g_mkdir_with_parents (dir, 0700) != 0) {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "impossible de creer %s : %s", dir, g_strerror (errno));
        return FALSE;
    }

    /* Relire avant d'ecrire : le fichier peut contenir des commentaires et
     * des reglages qu'une version ulterieure aura ajoutes. */
    g_autoptr(GKeyFile) kf = g_key_file_new ();
    g_key_file_load_from_file (kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);

    g_key_file_set_string_list (kf, "dock", "pinned",
                                (const char * const *) cfg->pinned,
                                g_strv_length (cfg->pinned));
    g_key_file_set_boolean (kf, "dock", "reserve_space", cfg->reserve_space);
    g_key_file_set_string  (kf, "appearance", "font", cfg->font);
    g_key_file_set_string  (kf, "appearance", "icon_theme", cfg->icon_theme);
    g_key_file_set_string  (kf, "appearance", "theme", cfg->dark ? "dark" : "light");

    return g_key_file_save_to_file (kf, path, error);
}

/* -------------------------------------------------------------------------
 * Relecture a chaud
 * ------------------------------------------------------------------------- */
typedef struct {
    ShellConfigChangedFunc cb;
    gpointer               data;
    GFileMonitor          *monitor;
    guint                  pending;
} Watch;

/* Une sauvegarde produit plusieurs evenements -- creation du fichier
 * temporaire, deplacement, changement d'attributs. Sans ce delai, chaque
 * enregistrement reconstruirait le dock trois fois de suite. */
static gboolean
watch_fire (gpointer data)
{
    Watch *w = data;
    w->pending = 0;
    w->cb (shell_config_load (), w->data);
    return G_SOURCE_REMOVE;
}

static void
on_config_changed (GFileMonitor *m, GFile *f, GFile *other,
                   GFileMonitorEvent event, gpointer data)
{
    Watch *w = data;
    (void) m; (void) f; (void) other; (void) event;

    if (w->pending != 0)
        g_source_remove (w->pending);
    w->pending = g_timeout_add (120, watch_fire, w);
}

void
shell_config_watch (ShellConfigChangedFunc cb, gpointer user_data)
{
    g_autofree char *path = config_path ();
    g_autoptr(GFile) file = g_file_new_for_path (path);
    g_autoptr(GError) error = NULL;

    Watch *w = g_new0 (Watch, 1);
    w->cb   = cb;
    w->data = user_data;

    /* WATCH_MOVES : une sauvegarde atomique remplace le fichier par un
     * autre. Sans cet indicateur, la surveillance suivrait l'ancien inode et
     * ne verrait plus jamais rien apres le premier enregistrement. */
    w->monitor = g_file_monitor_file (file, G_FILE_MONITOR_WATCH_MOVES,
                                      NULL, &error);
    if (w->monitor == NULL) {
        g_message ("relecture a chaud indisponible : %s", error->message);
        g_free (w);
        return;
    }
    g_signal_connect (w->monitor, "changed", G_CALLBACK (on_config_changed), w);
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
     * autrement, et une seule source decide de son style.
     *
     * Un SEUL fournisseur, cree une fois puis mis a jour : en empiler un
     * nouveau a chaque relecture laisserait l'ancienne police dans la
     * cascade. */
    static GtkCssProvider *font_provider = NULL;

    if (cfg->font == NULL || *cfg->font == '\0')
        return;

    if (font_provider == NULL) {
        font_provider = gtk_css_provider_new ();
        gtk_style_context_add_provider_for_display (
            gdk_display_get_default (), GTK_STYLE_PROVIDER (font_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }

    g_autofree char *rule = g_strdup_printf ("window.shell { font-family: \"%s\"; }",
                                             cfg->font);
    gtk_css_provider_load_from_string (font_provider, rule);
}
