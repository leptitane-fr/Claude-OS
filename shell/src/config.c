#include "config.h"

#include <errno.h>

#include <pango/pangocairo.h>

/* Applications epinglees par defaut : celles pour lesquelles ce systeme
 * existe, plus de quoi ouvrir un dossier et un terminal. */
static const char *default_pinned[] = {
    "chromium", "claude-desktop", "thunar", "xfce4-terminal",
    /* Tant qu'il n'y a pas de lanceur, le panneau de reglages ne serait
     * atteignable par aucun moyen s'il n'etait pas epingle. */
    "claude-os-reglages", NULL
};

/* -------------------------------------------------------------------------
 * Themes
 * ------------------------------------------------------------------------- */
/* La police est ICI et non dans le fichier CSS du theme, pour qu'elle ait une
 * source unique : le panneau de reglages doit pouvoir la nommer et verifier
 * qu'elle est installee, ce qu'il ne saurait pas faire en lisant une regle
 * CSS. */
static const ShellTheme themes[] = {
    { "clair",         "Clair",         FALSE, "Inter" },
    { "sombre",        "Sombre",        TRUE,  "Inter" },
    { "claude-clair",  "Claude clair",  FALSE, "Lato"  },
    { "claude-sombre", "Claude sombre", TRUE,  "Lato"  },
    { NULL, NULL, FALSE, NULL },
};

const ShellTheme *
shell_themes (void)
{
    return themes;
}

static const ShellTheme *
theme_par_id (const char *id)
{
    /* « light » et « dark » restent acceptes : ce sont les noms qu'utilisaient
     * les configurations ecrites avant l'arrivee des themes nommes. */
    if (g_strcmp0 (id, "light") == 0) id = "clair";
    if (g_strcmp0 (id, "dark")  == 0) id = "sombre";

    for (guint i = 0; themes[i].id != NULL; i++)
        if (g_strcmp0 (themes[i].id, id) == 0)
            return &themes[i];
    return NULL;
}

const ShellTheme *
shell_theme_actif (const ShellConfig *cfg)
{
    const ShellTheme *t = theme_par_id (cfg->theme);
    return t != NULL ? t : &themes[0];
}

gboolean
shell_police_installee (const char *famille)
{
    if (famille == NULL || *famille == '\0')
        return TRUE;

    PangoFontMap *carte = pango_cairo_font_map_get_default ();
    PangoFontFamily **familles = NULL;
    int n = 0;
    pango_font_map_list_families (carte, &familles, &n);

    gboolean trouvee = FALSE;
    for (int i = 0; i < n && !trouvee; i++)
        trouvee = (g_ascii_strcasecmp (pango_font_family_get_name (familles[i]),
                                       famille) == 0);
    g_free (familles);
    return trouvee;
}

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
    /* Vide : c'est le theme qui fixe la police. Un nom ici prendrait le pas
     * sur les quatre themes a la fois, ce que personne ne demande par
     * defaut. */
    cfg->font   = g_strdup ("");
    /* Papirus plutot qu'Adwaita : Adwaita a abandonne les noms d'icones
     * herites (web-browser, utilities-terminal...) que la plupart des
     * fichiers .desktop declarent encore, et affiche donc un pictogramme
     * generique pour la moitie des applications. Papirus les conserve, et
     * son style plat et arrondi est plus proche de ChromeOS. */
    cfg->icon_theme = g_strdup ("Papirus");
    cfg->theme  = g_strdup ("clair");
    cfg->dark   = FALSE;
    /* Par defaut le dock ne repousse rien : afficher ou masquer le dock ne
     * doit pas redimensionner la fenetre en dessous, il doit passer par
     * dessus. Voir le commentaire de la zone exclusive dans dock.c. */
    cfg->reserve_space = FALSE;
    cfg->wallpaper      = g_strdup ("");
    cfg->wallpaper_fill = TRUE;

    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
        return cfg;   /* pas de fichier : les defauts suffisent */

    g_auto(GStrv) pinned = g_key_file_get_string_list (kf, "dock", "pinned",
                                                       NULL, NULL);
    if (pinned != NULL && pinned[0] != NULL) {
        g_strfreev (cfg->pinned);
        cfg->pinned = g_steal_pointer (&pinned);
    }

    g_autofree char *font = g_key_file_get_string (kf, "appearance", "font", NULL);
    if (font != NULL) {
        g_free (cfg->font);
        cfg->font = g_steal_pointer (&font);
    }

    g_autofree char *icons = g_key_file_get_string (kf, "appearance", "icon_theme", NULL);
    if (icons != NULL && *icons != '\0') {
        g_free (cfg->icon_theme);
        cfg->icon_theme = g_steal_pointer (&icons);
    }

    g_autofree char *theme = g_key_file_get_string (kf, "appearance", "theme", NULL);
    const ShellTheme *t = theme_par_id (theme);
    if (t != NULL) {
        g_free (cfg->theme);
        cfg->theme = g_strdup (t->id);
        cfg->dark  = t->sombre;
    } else if (theme != NULL && *theme != '\0') {
        /* Un thème inconnu -- faute de frappe, ou fichier écrit par une
         * version ultérieure. On garde le défaut plutôt que de refuser de
         * démarrer, et on le dit. */
        g_message ("thème « %s » inconnu, « %s » utilisé", theme, cfg->theme);
    }

    g_autoptr(GError) e = NULL;
    gboolean reserve = g_key_file_get_boolean (kf, "dock", "reserve_space", &e);
    if (e == NULL)
        cfg->reserve_space = reserve;

    g_autofree char *wp = g_key_file_get_string (kf, "wallpaper", "image", NULL);
    if (wp != NULL) {
        g_free (cfg->wallpaper);
        cfg->wallpaper = g_steal_pointer (&wp);
    }

    g_autoptr(GError) e2 = NULL;
    gboolean fill = g_key_file_get_boolean (kf, "wallpaper", "fill", &e2);
    if (e2 == NULL)
        cfg->wallpaper_fill = fill;

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
    g_free (cfg->theme);
    g_free (cfg->wallpaper);
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
    g_key_file_set_string  (kf, "appearance", "theme", cfg->theme);
    g_key_file_set_string  (kf, "wallpaper", "image", cfg->wallpaper);
    g_key_file_set_boolean (kf, "wallpaper", "fill", cfg->wallpaper_fill);

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
shell_styles_load (const char *theme)
{
    /* Deux fournisseurs, gardes entre les appels. Celui du theme est
     * RECHARGE a chaque changement ; celui des regles n'est charge qu'une
     * fois. En creer de nouveaux a chaque appel empilerait les anciennes
     * couleurs dans la cascade.
     *
     * Recharger le seul fichier de jetons suffit : GTK re-resout les
     * couleurs nommees des regles quand le fournisseur qui les definit
     * change. Verifie dans les deux sens plutot que suppose -- le dock suit
     * bien le theme sans que shell.css soit relu. */
    static GtkCssProvider *theme_provider = NULL;
    static GtkCssProvider *rules_provider = NULL;

    if (theme_par_id (theme) == NULL)
        theme = "clair";

    if (theme_provider == NULL) {
        theme_provider = gtk_css_provider_new ();
        gtk_style_context_add_provider_for_display (
            gdk_display_get_default (), GTK_STYLE_PROVIDER (theme_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    g_autofree char *nom  = g_strdup_printf ("theme-%s.css", theme);
    g_autofree char *path = g_build_filename (SHELL_DATA_DIR, "style", nom, NULL);
    gtk_css_provider_load_from_path (theme_provider, path);

    if (rules_provider != NULL)
        return;

    /* Les regles apres le theme, a priorite egale : a egalite, le dernier
     * fournisseur ajoute l'emporte. */
    rules_provider = gtk_css_provider_new ();
    g_autofree char *rules = g_build_filename (SHELL_DATA_DIR, "style", "shell.css", NULL);
    gtk_css_provider_load_from_path (rules_provider, rules);
    gtk_style_context_add_provider_for_display (
        gdk_display_get_default (), GTK_STYLE_PROVIDER (rules_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

void
shell_styles_startup (GtkApplication *app, gpointer cfg)
{
    (void) app;
    shell_styles_load (((const ShellConfig *) cfg)->theme);
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

    if (font_provider == NULL) {
        font_provider = gtk_css_provider_new ();
        gtk_style_context_add_provider_for_display (
            gdk_display_get_default (), GTK_STYLE_PROVIDER (font_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }

    /* Police vide = celle du theme. La regle est TOUJOURS ecrite, jamais
     * omise : revenir a l'automatique en ne faisant rien laisserait
     * l'ancienne famille dans la cascade. */
    const char *famille = (cfg->font != NULL && *cfg->font != '\0')
                        ? cfg->font
                        : shell_theme_actif (cfg)->police;

    g_autofree char *rule = g_strdup_printf (
        "window.shell { font-family: \"%s\"; }", famille);
    gtk_css_provider_load_from_string (font_provider, rule);
}
