#include "config.h"

#include <errno.h>

#include <pango/pangocairo.h>

/* Applications epinglees par defaut : celles pour lesquelles ce systeme
 * existe, plus de quoi ouvrir un dossier et un terminal. */
static const char *default_pinned[] = {
    "chromium", "claude-desktop", "claude-os-fichiers", "xfce4-terminal", NULL
};
/* Le panneau de reglages n'est plus epingle : il vit desormais dans le
 * panneau des reglages rapides, ouvert au clic sur la barre d'etat. Sa place
 * naturelle est la, avec le Wi-Fi et la batterie -- et le dock est reserve a
 * ce qu'on ouvre souvent. Le bouton du lanceur, lui, garantit qu'aucune
 * application n'est inatteignable. */

/* -------------------------------------------------------------------------
 * Themes
 * ------------------------------------------------------------------------- */
/* La police est ICI et non dans le fichier CSS du theme, pour qu'elle ait une
 * source unique : le panneau de reglages doit pouvoir la nommer et verifier
 * qu'elle est installee, ce qu'il ne saurait pas faire en lisant une regle
 * CSS.
 *
 * MIROIR : /usr/local/bin/claude-os-theme refait la meme resolution en shell,
 * pour ecrire « gtk-font-name » a l'usage des applications qui ne sont pas le
 * shell. Il ne sait pas lire du C ; sa table est donc une copie de celle-ci,
 * et un theme ajoute ici doit l'etre la-bas aussi -- sans quoi les fenetres
 * exterieures n'auront pas la police du bureau. */
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
shell_config_set_theme (ShellConfig *cfg, const char *id)
{
    const ShellTheme *t = theme_par_id (id);
    if (t == NULL)
        return FALSE;

    g_free (cfg->theme);
    cfg->theme = g_strdup (t->id);
    cfg->dark  = t->sombre;
    return TRUE;
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

/* Deux aides pour la section « energie », qui compte a elle seule neuf
 * cles. Une valeur absente ou mal ecrite laisse le defaut en place plutot
 * que de rendre zero -- un delai de zero seconde eteindrait l'ecran
 * immediatement, ce qui est la pire facon de traiter une faute de frappe. */
static void
lire_entier (GKeyFile *kf, const char *cle, int *cible)
{
    g_autoptr(GError) e = NULL;
    int v = g_key_file_get_integer (kf, "energie", cle, &e);
    if (e == NULL && v >= 0)
        *cible = v;
}

static void
lire_bool (GKeyFile *kf, const char *cle, gboolean *cible)
{
    g_autoptr(GError) e = NULL;
    gboolean v = g_key_file_get_boolean (kf, "energie", cle, &e);
    if (e == NULL)
        *cible = v;
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

    /* Defauts de la veille progressive.
     *
     * TRAVAIL : l'ordinateur ne dort jamais, mais l'ecran a quand meme une
     * duree de vie -- un ecran allume toute la nuit sur un article qu'on ne
     * lit plus n'aide personne. Les delais sont longs, et l'attenuation est
     * annoncee par un compte a rebours : voir energie_travail_preavis.
     *
     * AUTOMATIQUE : l'equilibre, et le defaut.
     *
     * NOMADE : le plus econome. C'est la que le watt compte.
     *
     * 30 % pour l'attenuation : assez bas pour que le gain soit reel, assez
     * haut pour qu'on lise encore l'ecran et qu'on comprenne que la machine
     * s'assoupit au lieu de s'eteindre. */
    cfg->energie_active   = TRUE;
    cfg->energie_mode     = g_strdup ("automatique");
    cfg->energie_niveau   = 30;
    cfg->energie_opacite  = 55;
    cfg->energie_preavis  = 10;    /* 10 s, commun aux trois modes */
    cfg->energie_verrou       = FALSE;
    cfg->energie_verrou_delai = 60;   /* une minute de sursis */
    cfg->energie_suspendre_permis = FALSE;

    cfg->energie_travail_attenuer = 600;   /* 10 min */
    cfg->energie_travail_eteindre = 1200;  /* 20 min */

    cfg->energie_auto_attenuer    = 120;   /*  2 min */
    cfg->energie_auto_eteindre    = 300;   /*  5 min */
    cfg->energie_auto_suspendre   = 900;   /* 15 min, sous condition */

    cfg->energie_nomade_attenuer  = 45;
    cfg->energie_nomade_eteindre  = 120;   /*  2 min */
    cfg->energie_nomade_suspendre = 300;   /*  5 min, sous condition */

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
    if (shell_config_set_theme (cfg, theme)) {
        /* rien de plus : le setter a pose « theme » et « dark » ensemble */
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

    lire_bool   (kf, "active",           &cfg->energie_active);
    lire_bool   (kf, "suspendre_permis", &cfg->energie_suspendre_permis);
    lire_entier (kf, "niveau",           &cfg->energie_niveau);
    lire_entier (kf, "opacite",          &cfg->energie_opacite);
    /* « travail_preavis » d'abord : c'est l'ancien nom, du temps ou le
     * compte a rebours n'existait que pour le mode Travail. Un fichier
     * ecrit avant le 9 septembre 2026 garde donc son reglage. « preavis »
     * ensuite, qui prime. */
    lire_entier (kf, "travail_preavis",  &cfg->energie_preavis);
    lire_entier (kf, "preavis",          &cfg->energie_preavis);
    lire_bool   (kf, "verrou",           &cfg->energie_verrou);
    lire_entier (kf, "verrou_delai",     &cfg->energie_verrou_delai);

    lire_entier (kf, "travail_attenuer", &cfg->energie_travail_attenuer);
    lire_entier (kf, "travail_eteindre", &cfg->energie_travail_eteindre);

    lire_entier (kf, "automatique_attenuer",  &cfg->energie_auto_attenuer);
    lire_entier (kf, "automatique_eteindre",  &cfg->energie_auto_eteindre);
    lire_entier (kf, "automatique_suspendre", &cfg->energie_auto_suspendre);

    lire_entier (kf, "nomade_attenuer",  &cfg->energie_nomade_attenuer);
    lire_entier (kf, "nomade_eteindre",  &cfg->energie_nomade_eteindre);
    lire_entier (kf, "nomade_suspendre", &cfg->energie_nomade_suspendre);

    /* Un mode inconnu -- fichier d'une version anterieure, ou faute de
     * frappe -- laisse le defaut en place plutot que d'inventer un
     * comportement. Les anciens noms « auto », « normal » et « econome »
     * tombent donc naturellement sur « automatique ». */
    g_autofree char *mode = g_key_file_get_string (kf, "energie", "mode", NULL);
    if (mode != NULL && (g_strcmp0 (mode, "travail")     == 0 ||
                         g_strcmp0 (mode, "automatique") == 0 ||
                         g_strcmp0 (mode, "nomade")      == 0)) {
        g_free (cfg->energie_mode);
        cfg->energie_mode = g_steal_pointer (&mode);
    }

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
    g_free (cfg->energie_mode);
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

    g_key_file_set_boolean (kf, "energie", "active", cfg->energie_active);
    g_key_file_set_string  (kf, "energie", "mode", cfg->energie_mode);
    g_key_file_set_integer (kf, "energie", "niveau", cfg->energie_niveau);
    g_key_file_set_integer (kf, "energie", "opacite", cfg->energie_opacite);
    g_key_file_set_integer (kf, "energie", "preavis", cfg->energie_preavis);
    g_key_file_set_boolean (kf, "energie", "verrou", cfg->energie_verrou);
    g_key_file_set_integer (kf, "energie", "verrou_delai", cfg->energie_verrou_delai);
    g_key_file_set_boolean (kf, "energie", "suspendre_permis",
                            cfg->energie_suspendre_permis);
    g_key_file_set_integer (kf, "energie", "travail_attenuer", cfg->energie_travail_attenuer);
    g_key_file_set_integer (kf, "energie", "travail_eteindre", cfg->energie_travail_eteindre);
    g_key_file_set_integer (kf, "energie", "automatique_attenuer",  cfg->energie_auto_attenuer);
    g_key_file_set_integer (kf, "energie", "automatique_eteindre",  cfg->energie_auto_eteindre);
    g_key_file_set_integer (kf, "energie", "automatique_suspendre", cfg->energie_auto_suspendre);
    g_key_file_set_integer (kf, "energie", "nomade_attenuer",  cfg->energie_nomade_attenuer);
    g_key_file_set_integer (kf, "energie", "nomade_eteindre",  cfg->energie_nomade_eteindre);
    g_key_file_set_integer (kf, "energie", "nomade_suspendre", cfg->energie_nomade_suspendre);

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

/* LES ICONES LIVREES AVEC LE SHELL.
 *
 * Adwaita n'a pas de cloche, et Papirus n'est pas garanti d'etre le theme
 * actif : le shell fournit donc la sienne. Elle vit dans son propre
 * repertoire plutot que dans /usr/share/icons/hicolor, ou le cache du theme
 * — deja present — l'emportait sur le repertoire et la rendait introuvable
 * meme apres regeneration.
 *
 * Ajoute en tete : GTK cherche d'abord ici, puis dans le theme choisi. Nos
 * icones ne masquent rien puisqu'elles portent toutes le prefixe
 * « claude-os- ». */
static void
shell_icones_load (void)
{
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
    if (theme == NULL)
        return;
    g_autofree char *dir = g_build_filename (SHELL_DATA_DIR, "icons", NULL);
    gtk_icon_theme_add_search_path (theme, dir);
}

void
shell_styles_startup (GtkApplication *app, gpointer cfg)
{
    (void) app;
    shell_styles_load (((const ShellConfig *) cfg)->theme);
    shell_icones_load ();
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
