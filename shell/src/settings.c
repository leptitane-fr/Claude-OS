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
#include "reseau.h"

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
/* -------------------------------------------------------------------------
 * Lecteurs reseau
 *
 * Le volet qui les DECLARE. Les connecter au quotidien se fait dans le volet
 * lateral de « Fichiers », a un clic du dossier qu'on cherche ; venir ici
 * pour ouvrir un partage serait un detour. Ce panneau sert a decrire un
 * serveur une fois, et a ne plus jamais y penser.
 *
 * Le fichier ecrit est ~/.config/claude-os/lecteurs, et « Fichiers » le
 * relit a chaque reconstruction de son volet. Aucun protocole entre les
 * deux programmes : la configuration est la seule source de verite, comme
 * pour shell.conf.
 * ------------------------------------------------------------------------- */

static GtkWidget *reseau_liste_boite = NULL;   /* la boite a repeupler */
static GtkWidget *reseau_fenetre     = NULL;   /* pour les dialogues   */

static void reseau_rafraichir (void);

/* Etat affiche a cote du nom. */
static const char *
etat_texte (const Lecteur *l)
{
    return reseau_est_connecte (l) ? "Connecté" : "Déconnecté";
}

/* ---------------------------------------------------------------- editeur */
typedef struct {
    GtkWidget *fenetre;
    GtkWidget *nom, *serveur, *partage, *utilisateur, *domaine, *options;
    GtkWidget *mot_de_passe;
    GtkWidget *protocole, *schema;
    GtkWidget *automatique;
    GtkWidget *partages;        /* liste deroulante des partages trouves */
    GtkWidget *parcourir;
    char      *id;              /* NULL : c'est une creation             */
} Editeur;

static void
editeur_free (gpointer data, GClosure *c)
{
    (void) c;
    Editeur *e = data;
    g_free (e->id);
    g_free (e);
}

static const char *
texte (GtkWidget *w)
{
    return gtk_editable_get_text (GTK_EDITABLE (w));
}

/* Le schema WebDAV et le nom d'utilisateur n'ont pas de sens pour tous les
 * protocoles. Les griser plutot que de les cacher garde la fenetre stable :
 * une boite dont les champs sautent a chaque changement de liste est
 * desagreable a remplir. */
static void
on_protocole_change (GObject *dd, GParamSpec *ps, gpointer data)
{
    (void) ps;
    Editeur *e = data;
    ReseauProtocole p = (ReseauProtocole) gtk_drop_down_get_selected (GTK_DROP_DOWN (dd));

    gtk_widget_set_sensitive (e->schema,       p == RESEAU_DAV);
    gtk_widget_set_sensitive (e->domaine,      p == RESEAU_SMB);
    gtk_widget_set_sensitive (e->mot_de_passe, p != RESEAU_SFTP);
    gtk_widget_set_sensitive (e->parcourir,    p == RESEAU_SMB);

    const char *invite = "";
    switch (p) {
        case RESEAU_SMB:  invite = "Nom du partage (ex. Public)";        break;
        case RESEAU_NFS:  invite = "Export (ex. /mnt/HD/HD_a2/Public)";  break;
        case RESEAU_SFTP: invite = "Chemin distant (ex. /home/moi)";     break;
        case RESEAU_DAV:  invite = "Chemin (ex. /remote.php/webdav)";    break;
    }
    gtk_entry_set_placeholder_text (GTK_ENTRY (e->partage), invite);
}

/* --- decouverte des partages ------------------------------------------- */
static void
on_partages_listes (GObject *src, GAsyncResult *res, gpointer data)
{
    Editeur *e = data;
    g_autoptr(GError) err = NULL;
    g_autofree char *sortie = NULL;

    gtk_widget_set_sensitive (e->parcourir, TRUE);
    gtk_button_set_label (GTK_BUTTON (e->parcourir), "Parcourir…");

    if (!g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res,
                                               &sortie, NULL, &err)) {
        GtkAlertDialog *d = gtk_alert_dialog_new ("Impossible d'interroger le serveur");
        gtk_alert_dialog_set_detail (d, err->message);
        gtk_alert_dialog_show (d, GTK_WINDOW (e->fenetre));
        g_object_unref (d);
        return;
    }

    /* smbclient a ete lance avec « -g » : une ligne par partage, au format
     * « Type|Nom|Commentaire ». C'est la sortie destinee aux programmes, et
     * la seule utilisable ici -- le tableau en colonnes du mode normal se
     * lit a l'oeil mais separe mal un nom de partage contenant une espace.
     *
     * Mesure du 9 septembre 2026 sur le NAS WDMyCloudMirror :
     *     Disk|Vidéos|
     *     IPC|IPC$|IPC Service (WDMyCloudMirror)
     * On ne garde donc que « Disk », ce qui ecarte IPC$ et les imprimantes. */
    GtkStringList *trouves = gtk_string_list_new (NULL);
    guint n = 0;
    g_auto(GStrv) lignes = g_strsplit (sortie != NULL ? sortie : "", "\n", -1);

    for (guint i = 0; lignes[i] != NULL; i++) {
        g_auto(GStrv) champs = g_strsplit (g_strstrip (lignes[i]), "|", 3);
        if (champs[0] == NULL || champs[1] == NULL)
            continue;
        if (g_strcmp0 (champs[0], "Disk") != 0 || *champs[1] == '\0')
            continue;
        gtk_string_list_append (trouves, champs[1]);
        n++;
    }

    if (n == 0) {
        g_object_unref (trouves);
        GtkAlertDialog *d = gtk_alert_dialog_new ("Aucun partage visible");
        gtk_alert_dialog_set_detail (d,
            "Le serveur n'a annoncé aucun partage accessible sans mot de passe. "
            "Saisissez le nom du partage à la main.");
        gtk_alert_dialog_show (d, GTK_WINDOW (e->fenetre));
        g_object_unref (d);
        return;
    }

    gtk_drop_down_set_model (GTK_DROP_DOWN (e->partages), G_LIST_MODEL (trouves));
    g_object_unref (trouves);
    gtk_widget_set_visible (e->partages, TRUE);
}

static void
on_partage_choisi (GObject *dd, GParamSpec *ps, gpointer data)
{
    (void) ps;
    Editeur *e = data;
    GListModel *m = gtk_drop_down_get_model (GTK_DROP_DOWN (dd));
    guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (dd));

    if (m == NULL || i == GTK_INVALID_LIST_POSITION)
        return;
    g_autoptr(GtkStringObject) o = g_list_model_get_item (m, i);
    if (o != NULL)
        gtk_editable_set_text (GTK_EDITABLE (e->partage), gtk_string_object_get_string (o));
}

static void
on_parcourir (GtkButton *b, gpointer data)
{
    Editeur *e = data;
    const char *serveur = texte (e->serveur);

    if (*serveur == '\0') {
        GtkAlertDialog *d = gtk_alert_dialog_new ("Renseignez d'abord le serveur");
        gtk_alert_dialog_show (d, GTK_WINDOW (e->fenetre));
        g_object_unref (d);
        return;
    }

    g_autofree char *cible = g_strdup_printf ("//%s", serveur);
    /* En invite, et sans plus : lister les partages d'un serveur ne demande
     * pas de compte sur la plupart des NAS, et demander un mot de passe pour
     * une simple liste decouragerait d'utiliser le bouton. */
    const char *argv[] = { "smbclient", "-L", cible, "-N", "-g", NULL };

    g_autoptr(GError) err = NULL;
    g_autoptr(GSubprocess) proc = g_subprocess_newv (
        argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &err);

    if (proc == NULL) {
        GtkAlertDialog *d = gtk_alert_dialog_new ("smbclient est introuvable");
        gtk_alert_dialog_set_detail (d, err->message);
        gtk_alert_dialog_show (d, GTK_WINDOW (e->fenetre));
        g_object_unref (d);
        return;
    }

    gtk_widget_set_sensitive (GTK_WIDGET (b), FALSE);
    gtk_button_set_label (b, "Recherche…");
    g_subprocess_communicate_utf8_async (proc, NULL, NULL, on_partages_listes, e);
}

/* --- enregistrement ----------------------------------------------------- */
static void
on_editeur_valide (GtkWidget *w, gpointer data)
{
    (void) w;
    Editeur *ed = data;

    if (*texte (ed->nom) == '\0' || *texte (ed->serveur) == '\0') {
        GtkAlertDialog *d = gtk_alert_dialog_new ("Un nom et un serveur sont nécessaires");
        gtk_alert_dialog_show (d, GTK_WINDOW (ed->fenetre));
        g_object_unref (d);
        return;
    }

    g_autoptr(GPtrArray) lecteurs = reseau_charger ();

    Lecteur *l = NULL;
    if (ed->id != NULL) {
        for (guint i = 0; i < lecteurs->len; i++)
            if (g_strcmp0 (((Lecteur *) g_ptr_array_index (lecteurs, i))->id, ed->id) == 0)
                l = g_ptr_array_index (lecteurs, i);
    }
    if (l == NULL) {
        l = g_new0 (Lecteur, 1);
        l->id = reseau_id_depuis_nom (texte (ed->nom), lecteurs);
        l->nom = l->serveur = l->partage = NULL;
        l->utilisateur = l->domaine = l->schema = l->options = NULL;
        g_ptr_array_add (lecteurs, l);
    }

    g_free (l->nom);         l->nom         = g_strdup (texte (ed->nom));
    g_free (l->serveur);     l->serveur     = g_strdup (texte (ed->serveur));
    g_free (l->partage);     l->partage     = g_strdup (texte (ed->partage));
    g_free (l->utilisateur); l->utilisateur = g_strdup (texte (ed->utilisateur));
    g_free (l->domaine);     l->domaine     = g_strdup (texte (ed->domaine));
    g_free (l->options);     l->options     = g_strdup (texte (ed->options));
    g_free (l->schema);
    l->schema = g_strdup (gtk_drop_down_get_selected (GTK_DROP_DOWN (ed->schema)) == 0
                          ? "https" : "http");
    l->protocole   = (ReseauProtocole) gtk_drop_down_get_selected (GTK_DROP_DOWN (ed->protocole));
    l->automatique = gtk_check_button_get_active (GTK_CHECK_BUTTON (ed->automatique));

    g_autoptr(GError) err = NULL;
    if (!reseau_enregistrer (lecteurs, &err)) {
        GtkAlertDialog *d = gtk_alert_dialog_new ("Enregistrement impossible");
        gtk_alert_dialog_set_detail (d, err->message);
        gtk_alert_dialog_show (d, GTK_WINDOW (ed->fenetre));
        g_object_unref (d);
        return;
    }

    /* Le mot de passe part au trousseau, jamais dans le fichier. Le laisser
     * vide ne l'efface pas : on ne devine pas qu'un champ vide veut dire
     * « oublie-le » -- pour cela il y a le bouton Oublier. */
    const char *mdp = gtk_editable_get_text (GTK_EDITABLE (ed->mot_de_passe));
    if (*mdp != '\0')
        reseau_connecter (l, mdp, TRUE, NULL, NULL);

    gtk_window_destroy (GTK_WINDOW (ed->fenetre));
    reseau_rafraichir ();
}

static void
on_editeur_annule (GtkWidget *w, gpointer data)
{
    (void) w;
    gtk_window_destroy (GTK_WINDOW (((Editeur *) data)->fenetre));
}

static GtkWidget *
champ (GtkWidget *grille, int rang, const char *libelle, const char *valeur,
       const char *invite)
{
    GtkWidget *l = gtk_label_new (libelle);
    gtk_widget_add_css_class (l, "reglages-libelle");
    gtk_widget_set_halign (l, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grille), l, 0, rang, 1, 1);

    GtkWidget *e = gtk_entry_new ();
    if (valeur != NULL)
        gtk_editable_set_text (GTK_EDITABLE (e), valeur);
    if (invite != NULL)
        gtk_entry_set_placeholder_text (GTK_ENTRY (e), invite);
    gtk_widget_set_hexpand (e, TRUE);
    gtk_grid_attach (GTK_GRID (grille), e, 1, rang, 1, 1);
    return e;
}

/* `modele` NULL : creation. Sinon modification. */
static void
ouvrir_editeur (const Lecteur *modele)
{
    Editeur *ed = g_new0 (Editeur, 1);
    ed->id = (modele != NULL) ? g_strdup (modele->id) : NULL;

    ed->fenetre = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (ed->fenetre),
                          modele != NULL ? "Modifier le lecteur réseau"
                                         : "Nouveau lecteur réseau");
    gtk_window_set_modal (GTK_WINDOW (ed->fenetre), TRUE);
    gtk_window_set_resizable (GTK_WINDOW (ed->fenetre), FALSE);
    if (reseau_fenetre != NULL)
        gtk_window_set_transient_for (GTK_WINDOW (ed->fenetre), GTK_WINDOW (reseau_fenetre));

    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top (boite, 18);
    gtk_widget_set_margin_bottom (boite, 18);
    gtk_widget_set_margin_start (boite, 18);
    gtk_widget_set_margin_end (boite, 18);

    GtkWidget *grille = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grille), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grille), 12);
    int r = 0;

    ed->nom = champ (grille, r++, "Nom", modele ? modele->nom : NULL,
                     "Comme il apparaîtra dans Fichiers");

    /* Protocole */
    GtkStringList *protos = gtk_string_list_new (NULL);
    for (int i = 0; i < 4; i++)
        gtk_string_list_append (protos, reseau_protocole_nom ((ReseauProtocole) i));
    ed->protocole = gtk_drop_down_new (G_LIST_MODEL (protos), NULL);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (ed->protocole),
                                modele ? (guint) modele->protocole : RESEAU_SMB);
    GtkWidget *lp = gtk_label_new ("Protocole");
    gtk_widget_add_css_class (lp, "reglages-libelle");
    gtk_widget_set_halign (lp, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grille), lp, 0, r, 1, 1);
    gtk_grid_attach (GTK_GRID (grille), ed->protocole, 1, r++, 1, 1);

    ed->serveur = champ (grille, r++, "Serveur", modele ? modele->serveur : NULL,
                         "Adresse ou nom (ex. 192.168.1.29)");

    /* Partage, avec le bouton qui interroge le serveur. Taper un nom de
     * partage de memoire, avec ses accents et sa casse exacte, est la
     * premiere source d'echec d'un montage SMB. */
    GtkWidget *lpart = gtk_label_new ("Partage");
    gtk_widget_add_css_class (lpart, "reglages-libelle");
    gtk_widget_set_halign (lpart, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grille), lpart, 0, r, 1, 1);

    GtkWidget *bpart = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    ed->partage = gtk_entry_new ();
    if (modele != NULL)
        gtk_editable_set_text (GTK_EDITABLE (ed->partage), modele->partage);
    gtk_widget_set_hexpand (ed->partage, TRUE);
    ed->parcourir = gtk_button_new_with_label ("Parcourir…");
    gtk_box_append (GTK_BOX (bpart), ed->partage);
    gtk_box_append (GTK_BOX (bpart), ed->parcourir);
    gtk_grid_attach (GTK_GRID (grille), bpart, 1, r++, 1, 1);

    ed->partages = gtk_drop_down_new (NULL, NULL);
    gtk_widget_set_visible (ed->partages, FALSE);
    gtk_grid_attach (GTK_GRID (grille), ed->partages, 1, r++, 1, 1);

    ed->utilisateur = champ (grille, r++, "Utilisateur",
                             modele ? modele->utilisateur : NULL,
                             "Vide : connexion en invité");

    GtkWidget *lmdp = gtk_label_new ("Mot de passe");
    gtk_widget_add_css_class (lmdp, "reglages-libelle");
    gtk_widget_set_halign (lmdp, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grille), lmdp, 0, r, 1, 1);
    ed->mot_de_passe = gtk_password_entry_new ();
    gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (ed->mot_de_passe), TRUE);
    gtk_widget_set_hexpand (ed->mot_de_passe, TRUE);
    gtk_grid_attach (GTK_GRID (grille), ed->mot_de_passe, 1, r++, 1, 1);

    ed->domaine = champ (grille, r++, "Domaine", modele ? modele->domaine : NULL,
                         "Facultatif (SMB en entreprise)");

    GtkStringList *schemas = gtk_string_list_new (NULL);
    gtk_string_list_append (schemas, "https");
    gtk_string_list_append (schemas, "http");
    ed->schema = gtk_drop_down_new (G_LIST_MODEL (schemas), NULL);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (ed->schema),
                                (modele != NULL && g_strcmp0 (modele->schema, "http") == 0) ? 1 : 0);
    GtkWidget *ls = gtk_label_new ("Schéma WebDAV");
    gtk_widget_add_css_class (ls, "reglages-libelle");
    gtk_widget_set_halign (ls, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grille), ls, 0, r, 1, 1);
    gtk_grid_attach (GTK_GRID (grille), ed->schema, 1, r++, 1, 1);

    ed->options = champ (grille, r++, "Options", modele ? modele->options : NULL,
                         "Facultatif (ex. vers=2.1,ro)");

    gtk_box_append (GTK_BOX (boite), grille);

    ed->automatique = gtk_check_button_new_with_label (
        "Se connecter à l'ouverture de session");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (ed->automatique),
                                 modele != NULL && modele->automatique);
    gtk_box_append (GTK_BOX (boite), ed->automatique);

    GtkWidget *aide = gtk_label_new (
        "Le mot de passe est rangé dans le trousseau du bureau, jamais dans "
        "un fichier. Laissé vide, celui déjà retenu est conservé.");
    gtk_widget_add_css_class (aide, "reglages-detail");
    gtk_label_set_wrap (GTK_LABEL (aide), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (aide), 52);
    gtk_widget_set_halign (aide, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (boite), aide);

    GtkWidget *barre = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign (barre, GTK_ALIGN_END);
    GtkWidget *annuler = gtk_button_new_with_label ("Annuler");
    GtkWidget *ok = gtk_button_new_with_label (modele != NULL ? "Enregistrer" : "Ajouter");
    gtk_widget_add_css_class (ok, "suggested-action");
    gtk_box_append (GTK_BOX (barre), annuler);
    gtk_box_append (GTK_BOX (barre), ok);
    gtk_box_append (GTK_BOX (boite), barre);

    g_signal_connect (ed->protocole, "notify::selected",
                      G_CALLBACK (on_protocole_change), ed);
    g_signal_connect (ed->parcourir, "clicked", G_CALLBACK (on_parcourir), ed);
    g_signal_connect (ed->partages, "notify::selected",
                      G_CALLBACK (on_partage_choisi), ed);
    g_signal_connect (annuler, "clicked", G_CALLBACK (on_editeur_annule), ed);
    g_signal_connect_data (ok, "clicked", G_CALLBACK (on_editeur_valide),
                           ed, editeur_free, 0);

    /* Pose l'etat des champs conditionnels avant le premier affichage. */
    on_protocole_change (G_OBJECT (ed->protocole), NULL, ed);

    gtk_window_set_child (GTK_WINDOW (ed->fenetre), boite);
    gtk_window_present (GTK_WINDOW (ed->fenetre));
}

/* --- actions sur une ligne --------------------------------------------- */
static void
on_fini_rafraichir (const Lecteur *l, GError *erreur, gpointer data)
{
    (void) data;
    if (erreur != NULL) {
        GtkAlertDialog *d = gtk_alert_dialog_new ("« %s » : opération impossible", l->nom);
        gtk_alert_dialog_set_detail (d, erreur->message);
        gtk_alert_dialog_show (d, reseau_fenetre ? GTK_WINDOW (reseau_fenetre) : NULL);
        g_object_unref (d);
    }
    reseau_rafraichir ();
}

static void
on_basculer (GtkButton *b, gpointer data)
{
    (void) b;
    const Lecteur *l = data;
    if (reseau_est_connecte (l))
        reseau_deconnecter (l, on_fini_rafraichir, NULL);
    else
        reseau_connecter (l, NULL, FALSE, on_fini_rafraichir, NULL);
}

static void
on_modifier (GtkButton *b, gpointer data)
{
    (void) b;
    ouvrir_editeur ((const Lecteur *) data);
}

static void
on_supprime_confirme (GObject *src, GAsyncResult *res, gpointer data)
{
    Lecteur *l = data;
    int choix = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (src), res, NULL);

    if (choix != 1) {          /* 0 = Annuler, 1 = Supprimer */
        lecteur_free (l);
        return;
    }

    /* Deconnecter d'abord : un montage dont plus rien ne porte la
     * declaration ne se retrouve plus dans l'interface, et il faudrait un
     * terminal pour s'en defaire. */
    if (reseau_est_connecte (l))
        reseau_deconnecter (l, NULL, NULL);

    /* Le secret part avec le lecteur. Un mot de passe orphelin dans le
     * trousseau ne se retrouve jamais, et personne ne pense a l'y chercher. */
    reseau_secret_effacer (l);

    g_autoptr(GPtrArray) lecteurs = reseau_charger ();
    for (guint i = 0; i < lecteurs->len; i++) {
        if (g_strcmp0 (((Lecteur *) g_ptr_array_index (lecteurs, i))->id, l->id) == 0) {
            g_ptr_array_remove_index (lecteurs, i);
            break;
        }
    }

    g_autoptr(GError) err = NULL;
    if (!reseau_enregistrer (lecteurs, &err))
        g_warning ("suppression non enregistrée : %s", err->message);

    lecteur_free (l);
    reseau_rafraichir ();
}

static void
on_supprimer (GtkButton *b, gpointer data)
{
    (void) b;
    const Lecteur *l = data;

    GtkAlertDialog *d = gtk_alert_dialog_new ("Supprimer « %s » ?", l->nom);
    gtk_alert_dialog_set_detail (d,
        "La déclaration et le mot de passe retenu seront effacés. "
        "Rien n'est supprimé sur le serveur.");
    const char *boutons[] = { "Annuler", "Supprimer", NULL };
    gtk_alert_dialog_set_buttons (d, boutons);
    gtk_alert_dialog_set_cancel_button (d, 0);
    gtk_alert_dialog_set_default_button (d, 0);
    gtk_alert_dialog_choose (d, reseau_fenetre ? GTK_WINDOW (reseau_fenetre) : NULL,
                             NULL, on_supprime_confirme, lecteur_copie (l));
    g_object_unref (d);
}

static void
on_ajouter (GtkButton *b, gpointer data)
{
    (void) b; (void) data;
    ouvrir_editeur (NULL);
}

/* --- la liste ----------------------------------------------------------- */
static void
reseau_rafraichir (void)
{
    if (reseau_liste_boite == NULL)
        return;

    GtkWidget *enfant;
    while ((enfant = gtk_widget_get_first_child (reseau_liste_boite)) != NULL)
        gtk_box_remove (GTK_BOX (reseau_liste_boite), enfant);

    GPtrArray *lecteurs = reseau_charger ();

    if (lecteurs->len == 0) {
        GtkWidget *vide = gtk_label_new (
            "Aucun lecteur déclaré. « Ajouter un lecteur » pour commencer.");
        gtk_widget_add_css_class (vide, "reglages-detail");
        gtk_widget_set_halign (vide, GTK_ALIGN_START);
        gtk_box_append (GTK_BOX (reseau_liste_boite), vide);
        g_ptr_array_unref (lecteurs);
        return;
    }

    for (guint i = 0; i < lecteurs->len; i++) {
        Lecteur *l = g_ptr_array_index (lecteurs, i);
        gboolean connecte = reseau_est_connecte (l);

        GtkWidget *rangee = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_add_css_class (rangee, "reglages-ligne");

        GtkWidget *textes = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_hexpand (textes, TRUE);

        GtkWidget *nom = gtk_label_new (l->nom);
        gtk_widget_add_css_class (nom, "reglages-libelle");
        gtk_widget_set_halign (nom, GTK_ALIGN_START);
        gtk_box_append (GTK_BOX (textes), nom);

        g_autofree char *detail = g_strdup_printf (
            "%s · %s%s%s · %s%s",
            reseau_protocole_nom (l->protocole),
            l->serveur, *l->partage ? "/" : "", l->partage,
            etat_texte (l),
            l->automatique ? " · à l'ouverture de session" : "");
        GtkWidget *d = gtk_label_new (detail);
        gtk_widget_add_css_class (d, "reglages-detail");
        gtk_widget_set_halign (d, GTK_ALIGN_START);
        gtk_label_set_wrap (GTK_LABEL (d), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (d), 46);
        gtk_box_append (GTK_BOX (textes), d);
        gtk_box_append (GTK_BOX (rangee), textes);

        GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_valign (actions, GTK_ALIGN_CENTER);

        GtkWidget *bascule = gtk_button_new_with_label (connecte ? "Déconnecter"
                                                                 : "Connecter");
        GtkWidget *modif = gtk_button_new_from_icon_name ("document-edit-symbolic");
        gtk_widget_set_tooltip_text (modif, "Modifier");
        GtkWidget *supp = gtk_button_new_from_icon_name ("user-trash-symbolic");
        gtk_widget_set_tooltip_text (supp, "Supprimer");

        /* Chaque bouton porte SA copie du lecteur : la liste sera liberee au
         * retour de cette fonction, et un pointeur dans le tableau
         * deviendrait pendouillant des le premier rafraichissement. */
        g_object_set_data_full (G_OBJECT (bascule), "lecteur", lecteur_copie (l),
                                (GDestroyNotify) lecteur_free);
        g_object_set_data_full (G_OBJECT (modif), "lecteur", lecteur_copie (l),
                                (GDestroyNotify) lecteur_free);
        g_object_set_data_full (G_OBJECT (supp), "lecteur", lecteur_copie (l),
                                (GDestroyNotify) lecteur_free);

        g_signal_connect (bascule, "clicked", G_CALLBACK (on_basculer),
                          g_object_get_data (G_OBJECT (bascule), "lecteur"));
        g_signal_connect (modif, "clicked", G_CALLBACK (on_modifier),
                          g_object_get_data (G_OBJECT (modif), "lecteur"));
        g_signal_connect (supp, "clicked", G_CALLBACK (on_supprimer),
                          g_object_get_data (G_OBJECT (supp), "lecteur"));

        gtk_box_append (GTK_BOX (actions), bascule);
        gtk_box_append (GTK_BOX (actions), modif);
        gtk_box_append (GTK_BOX (actions), supp);
        gtk_box_append (GTK_BOX (rangee), actions);

        gtk_box_append (GTK_BOX (reseau_liste_boite), rangee);
    }
    g_ptr_array_unref (lecteurs);
}

static void
on_volet_reseau_detruit (GtkWidget *w, gpointer data)
{
    (void) w; (void) data;
    reseau_ne_plus_surveiller (NULL);
    reseau_liste_boite = NULL;
    reseau_fenetre     = NULL;
}

static GtkWidget *
construire_reseau (ShellConfig *cfg, GtkWidget *window)
{
    (void) cfg;
    reseau_fenetre = window;

    GtkWidget *pile = gtk_box_new (GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class (pile, "reglages-pile");

    GtkWidget *c = carte ("Lecteurs réseau");

    reseau_liste_boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append (GTK_BOX (c), reseau_liste_boite);

    GtkWidget *ajouter = gtk_button_new_with_label ("Ajouter un lecteur…");
    gtk_widget_set_halign (ajouter, GTK_ALIGN_START);
    gtk_widget_set_margin_top (ajouter, 6);
    g_signal_connect (ajouter, "clicked", G_CALLBACK (on_ajouter), NULL);
    gtk_box_append (GTK_BOX (c), ajouter);
    gtk_box_append (GTK_BOX (pile), c);

    GtkWidget *info = carte ("Comment cela fonctionne");
    GtkWidget *t = gtk_label_new (
        "Un lecteur connecté devient un dossier ordinaire, sous "
        "/run/claude-os/reseau. Il apparaît dans le volet latéral de Fichiers, "
        "et toutes les applications le voient — le terminal, Chromium et ses "
        "boîtes « Enregistrer sous » comprises.\n\n"
        "Les mots de passe sont rangés dans le trousseau du bureau. Le fichier "
        "~/.config/claude-os/lecteurs ne contient rien de secret.\n\n"
        "Les montages vivent dans /run : un redémarrage les efface, et aucun "
        "point de montage mort ne s'accumule.");
    gtk_widget_add_css_class (t, "reglages-detail");
    gtk_label_set_wrap (GTK_LABEL (t), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (t), 56);
    gtk_widget_set_halign (t, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (info), t);
    gtk_box_append (GTK_BOX (pile), info);

    reseau_rafraichir ();

    /* Le panneau suit l'etat reel : un lecteur connecte depuis « Fichiers »
     * pendant que cette fenetre est ouverte doit s'y voir. */
    reseau_surveiller ((ReseauChangeFunc) reseau_rafraichir, NULL);

    /* Et il se debranche en partant. Le moniteur des montages est un
     * singleton qui survit au volet : sans cela, un partage demonte apres la
     * fermeture de la fenetre appellerait reseau_rafraichir() sur une boite
     * detruite. Le meme piege que dans fichiers-lieux.c, et la meme
     * reponse. */
    g_signal_connect (pile, "destroy", G_CALLBACK (on_volet_reseau_detruit), NULL);

    return pile;
}

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
    { "reseau",    "Lecteurs réseau", "network-server-symbolic",  construire_reseau    },
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
static void set_e_opacite  (ShellConfig *c, gpointer d) { c->energie_opacite = GPOINTER_TO_INT (d); }
static void set_verrou     (ShellConfig *c, gpointer d) { c->energie_verrou = GPOINTER_TO_INT (d); }
static void set_verrou_del (ShellConfig *c, gpointer d) { c->energie_verrou_delai = GPOINTER_TO_INT (d); }

static void set_preavis    (ShellConfig *c, gpointer d) { c->energie_preavis = GPOINTER_TO_INT (d); }
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
static const Modif M_OPACITE  = set_e_opacite;
static const Modif M_VERROU_D = set_verrou_del;
static const Modif M_PREAVIS  = set_preavis;
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

static const int   OPACITES[]     = { 25, 40, 55, 70, 85, 100 };
static const char *OPACITES_NOM[] = { "25 %", "40 %", "55 %", "70 %",
                                      "85 %", "Opaque" };
#define OPACITES_N ((int) G_N_ELEMENTS (OPACITES))

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

    /* Le bon équilibre dépend du fond d'écran et de la vue de chacun :
     * trop discret le cadran ne prévient pas, trop marqué il occupe le
     * coin de l'écran. Un curseur coûte moins cher qu'un arbitrage. */
    /* Le compte a rebours a quitte la carte « Travail » : il vaut pour les
     * trois modes. La gene qu'il corrige -- l'ecran qui baisse au milieu
     * d'un paragraphe -- depend de ce qu'on fait, pas du profil choisi. */
    ligne (ecran, "Compte à rebours",
           "Un cadran s'affiche en bas à droite avant chaque baisse d'écran, "
           "atténuation et extinction. Il ne prend ni le clavier ni le clic : "
           "un geste suffit à l'annuler.",
           LISTE_PREAVIS (cfg->energie_preavis, &M_PREAVIS));

    ligne (ecran, "Opacité du compte à rebours",
           "Le cadran prend la largeur de la barre d'état.",
           liste (OPACITES, OPACITES_NOM, OPACITES_N,
                  cfg->energie_opacite, &M_OPACITE));
    gtk_box_append (GTK_BOX (pile), ecran);

    /* --- Reprise de la session ---
     *
     * Le délai se compte DEPUIS L'EXTINCTION et non depuis le début de
     * l'inactivité : c'est un sursis. On revient dans la minute, un geste
     * rend la main ; au-delà, le code PIN. « Immédiat » verrouille au
     * moment même où l'écran s'éteint. */
    GtkWidget *rep = carte ("Reprise de la session");
    ligne (rep, "Demander le code PIN au réveil",
           "L'écran se verrouille sans fermer la session : les applications "
           "continuent, les téléchargements aussi. Le mot de passe reste "
           "accepté si le code PIN n'est pas disponible.",
           commutateur (cfg->energie_verrou, set_verrou));
    ligne (rep, "Après l'extinction de l'écran",
           "Sursis avant le verrouillage. Sans étage « éteindre », le "
           "verrouillage ne se déclenche pas : il s'ancre sur lui.",
           LISTE_DUREE (cfg->energie_verrou_delai, &M_VERROU_D));
    gtk_box_append (GTK_BOX (pile), rep);

    /* --- Un volet par mode, dans l'ordre de la table --- */
    const ShellModeEnergie *modes = shell_energie_modes ();

    GtkWidget *trav = carte (modes[0].nom);
    GtkWidget *d_trav = gtk_label_new (modes[0].resume);
    gtk_widget_add_css_class (d_trav, "reglages-detail");
    gtk_label_set_wrap (GTK_LABEL (d_trav), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (d_trav), 46);
    gtk_widget_set_halign (d_trav, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (trav), d_trav);
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
