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
#include <string.h>   /* strlen */

#include "config.h"
#include "energie.h"   /* la table des modes, definie une seule fois */

/* -------------------------------------------------------------------------
 * Enregistrement
 * ------------------------------------------------------------------------- */
typedef void (*Modif) (ShellConfig *cfg, gpointer data);

/* Rappelee apres chaque enregistrement, avec la configuration ecrite. */
static void (*apres_modif) (const ShellConfig *cfg) = NULL;

static void
modifier (Modif apply, gpointer data)
{
    ShellConfig *cfg = shell_config_load ();
    apply (cfg, data);

    g_autoptr(GError) error = NULL;
    if (!shell_config_save (cfg, &error))
        g_warning ("enregistrement impossible : %s", error->message);

    /* Le panneau se reapplique a lui-meme sans attendre. Il ne surveille pas
     * shell.conf -- il en est le seul redacteur pour ces cles -- mais il est
     * la fenetre que l'utilisateur regarde en choisissant : la voir rester
     * en arriere donnait l'impression que le reglage n'avait pas pris. */
    if (apres_modif != NULL)
        apres_modif (cfg);

    shell_config_free (cfg);
}

static void set_theme     (ShellConfig *c, gpointer d) { g_free (c->theme);      c->theme      = g_strdup (d); }
static void set_reserve   (ShellConfig *c, gpointer d) { c->reserve_space = GPOINTER_TO_INT (d); }
static void set_fill      (ShellConfig *c, gpointer d) { c->wallpaper_fill = GPOINTER_TO_INT (d); }
static void set_font      (ShellConfig *c, gpointer d) { g_free (c->font);       c->font       = g_strdup (d); }
static void set_icons     (ShellConfig *c, gpointer d) { g_free (c->icon_theme); c->icon_theme = g_strdup (d); }
static void set_wallpaper (ShellConfig *c, gpointer d) { g_free (c->wallpaper);  c->wallpaper  = g_strdup (d); }

/* Widgets que la reapplication doit rafraichir. Un seul panneau par
 * processus : une structure globale suffit et evite de promener un contexte
 * dans chaque rappel. */
static struct {
    GtkWidget *police_detail;
} P;

static void mettre_a_jour_police_detail (const ShellConfig *cfg);

/* LE THEME NE S'ARRETE PAS AU SHELL.
 *
 * Chromium, Claude Desktop, le terminal et les barres de titre dessinees par
 * labwc ne lisent pas notre feuille de style. Ils lisent tous la meme chose,
 * et une seule : « color-scheme » de org.freedesktop.appearance, publie par
 * le portail XDG. claude-os-theme alimente ce reglage, engendre le themerc
 * de labwc et lui demande de se reconfigurer.
 *
 * Lance en ASYNCHRONE : la chaine passe par gsettings, dconf et le portail,
 * ce qui prend quelques dizaines de millisecondes. Bloquer dessus figerait
 * la liste deroulante sous le doigt a chaque changement de theme.
 *
 * Sans attendre le resultat non plus : le script dit lui-meme ce qui a
 * echoue, sur sa sortie d'erreur, et la fenetre des Reglages n'est pas
 * l'endroit ou l'on diagnostique un portail absent. */
static void
propager_theme (const char *theme)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GSubprocess) proc = g_subprocess_new (
        G_SUBPROCESS_FLAGS_NONE, &err,
        "/usr/local/bin/claude-os-theme", theme, NULL);
    if (proc == NULL)
        g_message ("propagation du theme impossible : %s", err->message);
}

static void
reappliquer (const ShellConfig *cfg)
{
    shell_styles_load (cfg->theme);
    shell_config_apply (cfg);
    g_object_set (gtk_settings_get_default (),
                  "gtk-application-prefer-dark-theme",
                  shell_theme_actif (cfg)->sombre, NULL);
    mettre_a_jour_police_detail (cfg);
    propager_theme (cfg->theme);
}

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

/* Renvoie l'etiquette de detail, pour les rares lignes dont le texte
 * explicatif change a l'usage. NULL quand la ligne n'en a pas. */
static GtkWidget *
ligne (GtkWidget *carte, const char *libelle, const char *detail, GtkWidget *controle)
{
    GtkWidget *textes = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *l = gtk_label_new (libelle);
    gtk_widget_add_css_class (l, "reglages-libelle");
    gtk_widget_set_halign (l, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (textes), l);

    GtkWidget *d = NULL;
    if (detail != NULL) {
        d = gtk_label_new (detail);
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
    return d;
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
/* Ce que l'utilisateur voit vraiment a l'ecran, et pourquoi. */
static void
mettre_a_jour_police_detail (const ShellConfig *cfg)
{
    if (P.police_detail == NULL)
        return;

    const ShellTheme *t = shell_theme_actif (cfg);
    gboolean auto_police = (cfg->font == NULL || *cfg->font == '\0');
    const char *famille  = auto_police ? t->police : cfg->font;

    g_autofree char *texte = NULL;
    /* On nomme la police manquante sans deviner son paquet : « Lato » donne
     * bien fonts-lato, mais « DejaVu Sans » ne donne pas fonts-dejavu-sans.
     * Une commande fausse serait pire que pas de commande. */
    if (!shell_police_installee (famille))
        texte = g_strdup_printf (
            "« %s » n'est pas installée : le système en substitue une autre.",
            famille);
    else if (auto_police)
        texte = g_strdup_printf ("Le thème « %s » utilise %s.", t->nom, famille);
    else
        texte = g_strdup_printf ("%s, choisie explicitement.", famille);

    gtk_label_set_text (GTK_LABEL (P.police_detail), texte);
}

static void
on_theme (GObject *dd, GParamSpec *ps, gpointer data)
{
    (void) ps; (void) data;

    guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (dd));
    if (i == GTK_INVALID_LIST_POSITION)
        return;

    modifier (set_theme, (gpointer) shell_themes ()[i].id);
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
/* -------------------------------------------------------------------------
 * Les volets
 *
 * AJOUTER UNE SECTION, C'EST AJOUTER UNE FONCTION ET UNE LIGNE DE TABLE.
 * Rien d'autre : ni bouton a cabler, ni page a nommer deux fois. Le panneau
 * n'etait dessine que pour l'interface ; il doit accueillir l'energie, puis
 * ce qui viendra, sans qu'on reecrive sa charpente a chaque fois.
 *
 * Chaque fabrique renvoie le CONTENU du volet, pas son defilement : c'est
 * on_activate qui enveloppe, pour que toutes les sections defilent pareil.
 * ------------------------------------------------------------------------- */
typedef GtkWidget *(*ConstructeurVolet) (ShellConfig *cfg, GtkWidget *window);

typedef struct {
    const char        *id;      /* nom de page dans la pile                 */
    const char        *titre;   /* libelle du bouton                        */
    const char        *icone;
    ConstructeurVolet  construire;
} Volet;

static GtkWidget *construire_interface (ShellConfig *cfg, GtkWidget *window);
static GtkWidget *construire_energie   (ShellConfig *cfg, GtkWidget *window);

static const Volet VOLETS[] = {
    { "interface", "Interface", "preferences-desktop-symbolic", construire_interface },
    { "energie",   "Énergie",   "battery-good-symbolic",        construire_energie   },
    { NULL, NULL, NULL, NULL },
};

/* Volet a ouvrir au demarrage, pose par la ligne de commande. NULL : le
 * premier de la table. */
static const char *volet_demande = NULL;

static void
on_volet (GtkToggleButton *b, gpointer pile)
{
    if (!gtk_toggle_button_get_active (b))
        return;   /* le groupe emet aussi pour celui qu'on vient de quitter */
    gtk_stack_set_visible_child_name (
        GTK_STACK (pile), g_object_get_data (G_OBJECT (b), "volet"));
}

static GtkWidget *
construire_interface (ShellConfig *cfg, GtkWidget *window)
{
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
    P.police_detail = ligne (apparence, "Police de l'interface", " ", police);
    mettre_a_jour_police_detail (cfg);

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

    return pile;
}

/* -------------------------------------------------------------------------
 * Volet Energie
 *
 * ON REGLE DES DUREES, JAMAIS CE QU'UN MODE EST.
 *
 * Les trois modes viennent de shell_energie_modes() et n'existent nulle
 * part ailleurs. Ce volet ne peut donc ni en ajouter, ni en retirer, ni
 * changer l'ordre attenuer -> eteindre -> suspendre : il n'expose que les
 * durees de chacun. Exposer davantage inviterait a fabriquer des
 * combinaisons sans signification -- eteindre avant d'attenuer, ou faire
 * dormir un mode « Travail » dont c'est precisement le contraire.
 *
 * Des listes de durees plutot que des champs libres : « 90 » saisi dans une
 * case ne dit pas s'il s'agit de secondes ou de minutes, et un delai de
 * trois secondes tape par erreur rendrait la machine inutilisable.
 * ------------------------------------------------------------------------- */
static void set_e_active   (ShellConfig *c, gpointer d) { c->energie_active = GPOINTER_TO_INT (d); }
static void set_e_niveau   (ShellConfig *c, gpointer d) { c->energie_niveau = GPOINTER_TO_INT (d); }

static void set_trav_pre   (ShellConfig *c, gpointer d) { c->energie_travail_preavis  = GPOINTER_TO_INT (d); }
static void set_trav_att   (ShellConfig *c, gpointer d) { c->energie_travail_attenuer = GPOINTER_TO_INT (d); }
static void set_trav_ete   (ShellConfig *c, gpointer d) { c->energie_travail_eteindre = GPOINTER_TO_INT (d); }

static void set_auto_att   (ShellConfig *c, gpointer d) { c->energie_auto_attenuer  = GPOINTER_TO_INT (d); }
static void set_auto_ete   (ShellConfig *c, gpointer d) { c->energie_auto_eteindre  = GPOINTER_TO_INT (d); }
static void set_auto_sus   (ShellConfig *c, gpointer d) { c->energie_auto_suspendre = GPOINTER_TO_INT (d); }

static void set_nom_att    (ShellConfig *c, gpointer d) { c->energie_nomade_attenuer  = GPOINTER_TO_INT (d); }
static void set_nom_ete    (ShellConfig *c, gpointer d) { c->energie_nomade_eteindre  = GPOINTER_TO_INT (d); }
static void set_nom_sus    (ShellConfig *c, gpointer d) { c->energie_nomade_suspendre = GPOINTER_TO_INT (d); }

/* Adresses stables, pour les passer en donnee utilisateur d'un signal sans
 * transformer un pointeur de fonction en gpointer -- ce que le C ne
 * garantit pas. */
static const Modif M_NIVEAU   = set_e_niveau;
static const Modif M_TRAV_PRE = set_trav_pre;
static const Modif M_TRAV_ATT = set_trav_att;
static const Modif M_TRAV_ETE = set_trav_ete;
static const Modif M_AUTO_ATT = set_auto_att;
static const Modif M_AUTO_ETE = set_auto_ete;
static const Modif M_AUTO_SUS = set_auto_sus;
static const Modif M_NOM_ATT  = set_nom_att;
static const Modif M_NOM_ETE  = set_nom_ete;
static const Modif M_NOM_SUS  = set_nom_sus;

static const int   DUREES[]     = { 0, 30, 45, 60, 120, 180, 300, 600, 900, 1200, 1800 };
static const char *DUREES_NOM[] = { "Jamais", "30 s", "45 s", "1 min", "2 min",
                                    "3 min", "5 min", "10 min", "15 min",
                                    "20 min", "30 min" };
#define DUREES_N ((int) G_N_ELEMENTS (DUREES))

/* Le preavis se compte en secondes : au-dela d'une demi-minute il cesse
 * d'etre un coup d'oeil et devient un element de decor. */
static const int   PREAVIS[]     = { 0, 5, 10, 20, 30 };
static const char *PREAVIS_NOM[] = { "Aucun", "5 s", "10 s", "20 s", "30 s" };
#define PREAVIS_N ((int) G_N_ELEMENTS (PREAVIS))

static const int   NIVEAUX[]     = { 10, 20, 30, 40, 50 };
static const char *NIVEAUX_NOM[] = { "10 %", "20 %", "30 %", "40 %", "50 %" };
#define NIVEAUX_N ((int) G_N_ELEMENTS (NIVEAUX))

/* Une liste de valeurs positionnee sur la valeur courante. Une valeur
 * absente de la table -- shell.conf s'edite a la main -- retient la valeur
 * connue immediatement inferieure plutot que de retomber sur la premiere,
 * qui fermerait l'etage a l'insu de celui qui a ecrit le fichier. */
typedef struct {
    const int   *valeurs;
    int          n;
    const Modif *modif;
} Choix;

static void
on_choix (GObject *dd, GParamSpec *ps, gpointer data)
{
    (void) ps;
    Choix *c = data;
    guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (dd));
    if (i == GTK_INVALID_LIST_POSITION || (int) i >= c->n)
        return;
    modifier (*c->modif, GINT_TO_POINTER (c->valeurs[i]));
}

static GtkWidget *
liste (const int *valeurs, const char **noms, int n, int courant,
       const Modif *modif)
{
    GtkStringList *l = gtk_string_list_new (NULL);
    for (int i = 0; i < n; i++)
        gtk_string_list_append (l, noms[i]);

    int choisi = 0;
    for (int i = 0; i < n; i++)
        if (valeurs[i] <= courant)
            choisi = i;

    GtkWidget *dd = gtk_drop_down_new (G_LIST_MODEL (l), NULL);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (dd), choisi);

    Choix *c = g_new0 (Choix, 1);
    c->valeurs = valeurs; c->n = n; c->modif = modif;
    g_object_set_data_full (G_OBJECT (dd), "choix", c, g_free);
    g_signal_connect (dd, "notify::selected", G_CALLBACK (on_choix), c);
    return dd;
}

#define LISTE_DUREE(v, m)   liste (DUREES,  DUREES_NOM,  DUREES_N,  (v), (m))
#define LISTE_PREAVIS(v, m) liste (PREAVIS, PREAVIS_NOM, PREAVIS_N, (v), (m))

static GtkWidget *
construire_energie (ShellConfig *cfg, GtkWidget *window)
{
    (void) window;

    GtkWidget *pile = gtk_box_new (GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class (pile, "reglages-pile");

    /* --- Commun aux trois modes --- */
    GtkWidget *ecran = carte ("Veille de l'écran");
    ligne (ecran, "Activer la veille progressive",
           "Le rétroéclairage est le premier poste de consommation de cette "
           "machine : environ 1 à 2 W sur 6,8 W mesurés.",
           commutateur (cfg->energie_active, set_e_active));

    GtkStringList *niv = gtk_string_list_new (NULL);
    int niv_choisi = 2;
    for (int i = 0; i < NIVEAUX_N; i++) {
        gtk_string_list_append (niv, NIVEAUX_NOM[i]);
        if (NIVEAUX[i] == cfg->energie_niveau)
            niv_choisi = i;
    }
    GtkWidget *dd_niv = gtk_drop_down_new (G_LIST_MODEL (niv), NULL);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (dd_niv), niv_choisi);
    Choix *cn = g_new0 (Choix, 1);
    cn->valeurs = NIVEAUX; cn->n = NIVEAUX_N; cn->modif = &M_NIVEAU;
    g_object_set_data_full (G_OBJECT (dd_niv), "choix", cn, g_free);
    g_signal_connect (dd_niv, "notify::selected", G_CALLBACK (on_choix), cn);
    ligne (ecran, "Luminosité atténuée",
           "Assez bas pour que le gain soit réel, assez haut pour qu'on "
           "comprenne que la machine s'assoupit plutôt qu'elle ne s'éteint.",
           dd_niv);
    gtk_box_append (GTK_BOX (pile), ecran);

    /* --- Un volet par mode, dans l'ordre de la table --- */
    const ShellModeEnergie *modes = shell_energie_modes ();

    GtkWidget *trav = carte (modes[0].nom);
    GtkWidget *d_trav = gtk_label_new (modes[0].resume);
    gtk_widget_add_css_class (d_trav, "reglages-detail");
    gtk_label_set_wrap (GTK_LABEL (d_trav), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (d_trav), 46);
    gtk_widget_set_halign (d_trav, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (trav), d_trav);
    ligne (trav, "Compte à rebours",
           "Affiché avant l'atténuation, en bas à droite. Il ne prend ni le "
           "clavier ni le clic : un geste suffit à l'annuler.",
           LISTE_PREAVIS (cfg->energie_travail_preavis, &M_TRAV_PRE));
    ligne (trav, "Atténuer après", NULL,
           LISTE_DUREE (cfg->energie_travail_attenuer, &M_TRAV_ATT));
    ligne (trav, "Éteindre l'écran après", NULL,
           LISTE_DUREE (cfg->energie_travail_eteindre, &M_TRAV_ETE));
    gtk_box_append (GTK_BOX (pile), trav);

    GtkWidget *au = carte (modes[1].nom);
    GtkWidget *d_au = gtk_label_new (modes[1].resume);
    gtk_widget_add_css_class (d_au, "reglages-detail");
    gtk_label_set_wrap (GTK_LABEL (d_au), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (d_au), 46);
    gtk_widget_set_halign (d_au, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (au), d_au);
    ligne (au, "Atténuer après", NULL,
           LISTE_DUREE (cfg->energie_auto_attenuer, &M_AUTO_ATT));
    ligne (au, "Éteindre l'écran après", NULL,
           LISTE_DUREE (cfg->energie_auto_eteindre, &M_AUTO_ETE));
    ligne (au, "Veille de l'ordinateur après", NULL,
           LISTE_DUREE (cfg->energie_auto_suspendre, &M_AUTO_SUS));
    gtk_box_append (GTK_BOX (pile), au);

    GtkWidget *nom = carte (modes[2].nom);
    GtkWidget *d_nom = gtk_label_new (modes[2].resume);
    gtk_widget_add_css_class (d_nom, "reglages-detail");
    gtk_label_set_wrap (GTK_LABEL (d_nom), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (d_nom), 46);
    gtk_widget_set_halign (d_nom, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (nom), d_nom);
    ligne (nom, "Atténuer après", NULL,
           LISTE_DUREE (cfg->energie_nomade_attenuer, &M_NOM_ATT));
    ligne (nom, "Éteindre l'écran après", NULL,
           LISTE_DUREE (cfg->energie_nomade_eteindre, &M_NOM_ETE));
    ligne (nom, "Veille de l'ordinateur après", NULL,
           LISTE_DUREE (cfg->energie_nomade_suspendre, &M_NOM_SUS));
    gtk_box_append (GTK_BOX (pile), nom);

    /* --- Ce qui n'est pas reglable, et pourquoi ---
     *
     * Une case grisee sans explication passe pour une panne. Celle-ci dit
     * ce qui manque et ce qu'il faudrait pour l'ouvrir. */
    GtkWidget *ordi = carte ("Mise en veille de l'ordinateur");
    GtkWidget *etat = gtk_label_new (
        cfg->energie_suspendre_permis
        ? "Autorisée."
        : "Verrouillée. Le 9 septembre 2026, onze suspensions consécutives "
          "n'ont pas repris : la machine redémarrait au lieu de se réveiller. "
          "Tant que la reprise n'est pas fiable, les durées ci-dessus sont "
          "enregistrées mais sans effet.");
    gtk_widget_add_css_class (etat, "reglages-detail");
    gtk_label_set_wrap (GTK_LABEL (etat), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (etat), 46);
    gtk_widget_set_halign (etat, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (ordi), etat);
    gtk_box_append (GTK_BOX (pile), ordi);

    return pile;
}

static void
on_activate (GtkApplication *app, gpointer user_data)
{
    ShellConfig *cfg = user_data;

    shell_config_apply (cfg);

    apres_modif = reappliquer;

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
    /* 760 et non 520 : la colonne de navigation prend sa place a gauche, et
     * les textes explicatifs des lignes ont besoin de la leur a droite. En
     * dessous, les listes deroulantes se collaient aux libelles. */
    gtk_window_set_default_size (GTK_WINDOW (window), 760, 620);

    GtkWidget *contenu = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

    GtkWidget *volets = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class (volets, "reglages-volets");

    GtkWidget *pile_volets = gtk_stack_new ();
    gtk_widget_set_hexpand (pile_volets, TRUE);
    gtk_stack_set_transition_type (GTK_STACK (pile_volets),
                                   GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    /* 90 ms : assez pour que l'oeil suive le changement de volet, assez peu
     * pour qu'un aller-retour entre deux sections ne donne pas l'impression
     * d'attendre. */
    gtk_stack_set_transition_duration (GTK_STACK (pile_volets), 90);

    /* « --volet=energie » ouvre directement la bonne section. La Console s'en
     * sert : proposer un reglage puis obliger a le chercher dans une liste
     * est une facon sure de le rendre introuvable. */
    GtkWidget *premier = NULL, *demande = NULL;
    for (int v = 0; VOLETS[v].id != NULL; v++) {
        GtkWidget *page = VOLETS[v].construire (cfg, window);

        GtkWidget *defil = gtk_scrolled_window_new ();
        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (defil),
                                        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (defil), page);
        gtk_stack_add_named (GTK_STACK (pile_volets), defil, VOLETS[v].id);

        GtkWidget *b = gtk_toggle_button_new ();
        gtk_widget_add_css_class (b, "reglages-volet");

        GtkWidget *rangee = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_append (GTK_BOX (rangee),
                        gtk_image_new_from_icon_name (VOLETS[v].icone));
        GtkWidget *nom = gtk_label_new (VOLETS[v].titre);
        gtk_widget_add_css_class (nom, "reglages-volet-nom");
        gtk_widget_set_halign (nom, GTK_ALIGN_START);
        gtk_box_append (GTK_BOX (rangee), nom);
        gtk_button_set_child (GTK_BUTTON (b), rangee);

        if (premier == NULL)
            premier = b;
        else
            gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (b),
                                         GTK_TOGGLE_BUTTON (premier));
        if (volet_demande != NULL && g_strcmp0 (volet_demande, VOLETS[v].id) == 0)
            demande = b;

        /* L'identifiant de page voyage avec le bouton : le rappel n'a besoin
         * de rien d'autre, et ajouter un volet ne demande pas d'y toucher. */
        g_object_set_data (G_OBJECT (b), "volet", (gpointer) VOLETS[v].id);
        g_signal_connect (b, "toggled", G_CALLBACK (on_volet), pile_volets);
        gtk_box_append (GTK_BOX (volets), b);
    }
    GtkWidget *actif = (demande != NULL) ? demande : premier;
    if (actif != NULL)
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (actif), TRUE);

    gtk_box_append (GTK_BOX (contenu), volets);
    gtk_box_append (GTK_BOX (contenu), pile_volets);
    gtk_window_set_child (GTK_WINDOW (window), contenu);

    gtk_window_present (GTK_WINDOW (window));
}

int
main (int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix (argv[i], "--volet="))
            volet_demande = argv[i] + strlen ("--volet=");
        else
            g_message ("reglages : argument ignore — %s", argv[i]);
    }

    ShellConfig *cfg = shell_config_load ();
    GtkApplication *app = gtk_application_new ("os.claude.shell.reglages",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "startup",  G_CALLBACK (shell_styles_startup), cfg);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), cfg);

    int status = g_application_run (G_APPLICATION (app), 0, NULL);
    g_object_unref (app);
    return status;
}
