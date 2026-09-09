#include "fichiers-lieux.h"
#include "reseau.h"

#include <string.h>

typedef struct {
    GtkWidget       *boite;        /* la GtkBox racine, celle qu'on rend    */
    GtkWidget       *liste;        /* GtkListBox des emplacements           */
    LieuxNavFunc     nav;
    gpointer         data;
    GVolumeMonitor  *moniteur;
    GPtrArray       *favoris;      /* char* : chemins, dans l'ordre du fichier */
    GFile           *courant;

    /* Lecteurs reseau declares, relus a chaque reconstruction. */
    GPtrArray       *lecteurs;     /* Lecteur*                              */
    /* Identifiants des lecteurs dont la connexion est en cours. Une
     * connexion peut durer le delai TCP complet quand le serveur est
     * eteint : sans cette marque, la ligne resterait muette et l'on
     * cliquerait trois fois de suite. */
    GHashTable      *en_cours;     /* char* -> GINT_TO_POINTER (1)          */
} Lieux;

/* ------------------------------------------------------------------------- */
static char *
chemin_favoris (void)
{
    return g_build_filename (g_get_user_config_dir (), "claude-os", "favoris", NULL);
}

static void
favoris_lire (Lieux *L)
{
    g_ptr_array_set_size (L->favoris, 0);

    g_autofree char *chemin = chemin_favoris ();
    g_autofree char *contenu = NULL;
    if (!g_file_get_contents (chemin, &contenu, NULL, NULL))
        return;

    g_auto(GStrv) lignes = g_strsplit (contenu, "\n", -1);
    for (guint i = 0; lignes[i] != NULL; i++) {
        g_strstrip (lignes[i]);
        if (*lignes[i] != '\0' && *lignes[i] != '#')
            g_ptr_array_add (L->favoris, g_strdup (lignes[i]));
    }
}

static void
favoris_ecrire (Lieux *L)
{
    g_autofree char *chemin = chemin_favoris ();
    g_autofree char *dossier = g_path_get_dirname (chemin);
    g_mkdir_with_parents (dossier, 0700);

    GString *s = g_string_new ("# Emplacements favoris du gestionnaire de fichiers.\n"
                               "# Un chemin par ligne.\n");
    for (guint i = 0; i < L->favoris->len; i++)
        g_string_append_printf (s, "%s\n", (const char *) g_ptr_array_index (L->favoris, i));

    g_autoptr(GError) e = NULL;
    if (!g_file_set_contents (chemin, s->str, -1, &e))
        g_warning ("favoris non enregistrés : %s", e->message);
    g_string_free (s, TRUE);
}

/* ------------------------------------------------------------------------- */
static void reconstruire (Lieux *L);
static void on_lecteur_active (Lieux *L, const char *id);

static void
on_ligne_activee (GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    Lieux *L = data;
    (void) box;

    GFile *cible = g_object_get_data (G_OBJECT (row), "fichier");
    if (cible != NULL) {
        L->nav (cible, L->data);
        return;
    }

    /* Un volume pas encore monte : on le monte, la navigation suit dans le
     * rappel. Cliquer sur une cle USB doit la monter, pas ne rien faire. */
    GVolume *vol = g_object_get_data (G_OBJECT (row), "volume");
    if (vol != NULL) {
        g_volume_mount (vol, G_MOUNT_MOUNT_NONE, NULL, NULL, NULL, NULL);
        return;
    }

    /* Un lecteur reseau declare mais pas connecte : meme geste, meme
     * attente. Cliquer dessus le connecte, et la navigation suit. */
    const char *id = g_object_get_data (G_OBJECT (row), "lecteur-id");
    if (id != NULL)
        on_lecteur_active (L, id);
}

static void
on_ejecter (GtkButton *b, gpointer data)
{
    Lieux *L = data;
    GMount *mnt = g_object_get_data (G_OBJECT (b), "montage");
    (void) L;

    if (mnt == NULL)
        return;

    /* Ejecter si le materiel le sait, demonter sinon : une cle USB veut
     * l'un, une partition interne montee veut l'autre. */
    if (g_mount_can_eject (mnt))
        g_mount_eject_with_operation (mnt, G_MOUNT_UNMOUNT_NONE, NULL, NULL, NULL, NULL);
    else
        g_mount_unmount_with_operation (mnt, G_MOUNT_UNMOUNT_NONE, NULL, NULL, NULL, NULL);
}

static void
on_retirer_favori (GSimpleAction *a, GVariant *param, gpointer data)
{
    Lieux *L = data;
    (void) a;

    g_autoptr(GFile) f = g_file_new_for_path (g_variant_get_string (param, NULL));
    fichiers_lieux_retirer (L->boite, f);
}

/* -------------------------------------------------------------------------
 * Lecteurs reseau
 *
 * Une ligne par lecteur declare, connecte ou non -- et c'est le point : un
 * partage qui n'apparait QUE lorsqu'il est monte oblige a se souvenir qu'il
 * existe pour penser a le monter. La ligne grisee est l'invitation.
 *
 * Le montage est un repertoire ordinaire, sous /run/claude-os/reseau. Une
 * fois connecte, le lecteur se parcourt, se copie et se cherche comme
 * n'importe quel dossier : aucune des vues, aucune des operations de
 * fichiers n'a eu une ligne a changer pour lui.
 * ------------------------------------------------------------------------- */

static void
lecteurs_relire (Lieux *L)
{
    g_clear_pointer (&L->lecteurs, g_ptr_array_unref);
    L->lecteurs = reseau_charger ();
}

static Lecteur *
lecteur_par_id (Lieux *L, const char *id)
{
    for (guint i = 0; L->lecteurs != NULL && i < L->lecteurs->len; i++) {
        Lecteur *l = g_ptr_array_index (L->lecteurs, i);
        if (g_strcmp0 (l->id, id) == 0)
            return l;
    }
    return NULL;
}

static GtkWindow *
fenetre_de (Lieux *L)
{
    GtkRoot *r = gtk_widget_get_root (L->boite);
    return GTK_IS_WINDOW (r) ? GTK_WINDOW (r) : NULL;
}

static void connecter_lecteur   (Lieux *L, const Lecteur *l,
                                 const char *mot_de_passe, gboolean retenir);
static void demander_mot_de_passe (Lieux *L, const Lecteur *l, const char *pourquoi);

/* Le message de mount, tel quel, est juste mais aride : « mount error(13) »
 * ne dit rien a qui n'a pas lu la page de manuel. On traduit les deux cas
 * qui comptent, et on garde le texte d'origine pour les autres -- inventer
 * un message pour une erreur qu'on n'a pas prevue serait pire que la citer. */
static gboolean
erreur_d_identifiants (const GError *e)
{
    return e != NULL && e->message != NULL
        && (strstr (e->message, "error(13)") != NULL
            || strstr (e->message, "Permission denied") != NULL
            || strstr (e->message, "permission non accordée") != NULL);
}

typedef struct {
    Lieux *L;
    char  *id;
    gboolean naviguer;   /* aller dans le dossier une fois connecte */
} Attente;

static void
attente_free (Attente *a)
{
    g_free (a->id);
    g_free (a);
}

static void
on_lecteur_fini (const Lecteur *l, GError *erreur, gpointer data)
{
    Attente *a = data;
    Lieux   *L = a->L;

    g_hash_table_remove (L->en_cours, a->id);

    if (erreur != NULL) {
        /* Mot de passe refuse : on redemande, plutot que d'afficher une
         * erreur que l'utilisateur ne peut pas corriger depuis la ou il
         * est. C'est aussi le chemin normal du tout premier acces a un
         * partage protege. */
        if (erreur_d_identifiants (erreur)) {
            /* Reconstruire D'ABORD, chercher ENSUITE : dans l'autre ordre,
             * `frais` designerait une entree que reconstruire() vient de
             * liberer. Meme piege que dans connecter_lecteur. */
            reconstruire (L);
            const Lecteur *frais = lecteur_par_id (L, a->id);
            if (frais != NULL) {
                demander_mot_de_passe (L, frais,
                    "Le serveur a refusé ces identifiants.");
                attente_free (a);
                return;
            }
        }

        GtkAlertDialog *d = gtk_alert_dialog_new ("Connexion impossible à « %s »", l->nom);
        gtk_alert_dialog_set_detail (d, erreur->message);
        gtk_alert_dialog_show (d, fenetre_de (L));
        g_object_unref (d);
        reconstruire (L);
        attente_free (a);
        return;
    }

    reconstruire (L);

    if (a->naviguer) {
        g_autofree char *point = reseau_point_montage (l);
        g_autoptr(GFile) f = g_file_new_for_path (point);
        L->nav (f, L->data);
    }
    attente_free (a);
}

/* ------------------------------------------------------------------------- */
typedef struct {
    Lieux     *L;
    Lecteur   *lecteur;
    GtkWidget *fenetre;
    GtkWidget *champ;
    GtkWidget *retenir;
} Demande;

static void
demande_free (gpointer data, GClosure *c)
{
    (void) c;
    Demande *d = data;
    lecteur_free (d->lecteur);
    g_free (d);
}

static void
on_mdp_valide (GtkWidget *w, gpointer data)
{
    (void) w;
    Demande *d = data;

    /* TOUT copier avant de fermer. Detruire la fenetre detruit le bouton,
     * donc sa fermeture, donc la Demande elle-meme -- c'est demande_free
     * qui est attache comme destructeur de closure. Lire d->lecteur ou
     * d->champ apres gtk_window_destroy, c'est lire de la memoire rendue. */
    Lieux *L = d->L;
    g_autoptr(Lecteur) cible = lecteur_copie (d->lecteur);
    g_autofree char *mdp = g_strdup (gtk_editable_get_text (GTK_EDITABLE (d->champ)));
    gboolean retenir = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->retenir));

    gtk_window_destroy (GTK_WINDOW (d->fenetre));
    connecter_lecteur (L, cible, mdp, retenir);
}

static void
on_mdp_annule (GtkWidget *w, gpointer data)
{
    (void) w;
    Demande *d = data;
    gtk_window_destroy (GTK_WINDOW (d->fenetre));
}

static void
demander_mot_de_passe (Lieux *L, const Lecteur *l, const char *pourquoi)
{
    Demande *d = g_new0 (Demande, 1);
    d->L       = L;
    d->lecteur = lecteur_copie (l);

    d->fenetre = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (d->fenetre), "Connexion au lecteur réseau");
    gtk_window_set_modal (GTK_WINDOW (d->fenetre), TRUE);
    gtk_window_set_resizable (GTK_WINDOW (d->fenetre), FALSE);
    gtk_window_set_transient_for (GTK_WINDOW (d->fenetre), fenetre_de (L));
    gtk_widget_add_css_class (d->fenetre, "fichiers-saisie");

    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top (boite, 18);
    gtk_widget_set_margin_bottom (boite, 18);
    gtk_widget_set_margin_start (boite, 18);
    gtk_widget_set_margin_end (boite, 18);

    g_autofree char *titre = g_strdup_printf ("Mot de passe pour « %s »", l->nom);
    GtkWidget *t = gtk_label_new (titre);
    gtk_widget_add_css_class (t, "fichiers-saisie-titre");
    gtk_widget_set_halign (t, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (boite), t);

    g_autofree char *ou = g_strdup_printf ("%s sur %s%s%s",
                                           *l->partage ? l->partage : "dossier distant",
                                           l->serveur,
                                           *l->utilisateur ? ", compte " : "",
                                           *l->utilisateur ? l->utilisateur : "");
    GtkWidget *s = gtk_label_new (pourquoi != NULL ? pourquoi : ou);
    gtk_widget_add_css_class (s, "reglages-detail");
    gtk_widget_set_halign (s, GTK_ALIGN_START);
    gtk_label_set_wrap (GTK_LABEL (s), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (s), 40);
    gtk_box_append (GTK_BOX (boite), s);

    /* GtkPasswordEntry, et non une GtkEntry a visibilite fermee : il gere
     * l'avertissement de verrouillage majuscule et refuse que le contenu
     * parte dans le presse-papier. */
    d->champ = gtk_password_entry_new ();
    gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (d->champ), TRUE);
    gtk_widget_set_size_request (d->champ, 300, -1);
    gtk_box_append (GTK_BOX (boite), d->champ);

    d->retenir = gtk_check_button_new_with_label ("Retenir dans le trousseau");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (d->retenir), TRUE);
    gtk_box_append (GTK_BOX (boite), d->retenir);

    GtkWidget *barre = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign (barre, GTK_ALIGN_END);
    GtkWidget *annuler = gtk_button_new_with_label ("Annuler");
    GtkWidget *ok      = gtk_button_new_with_label ("Se connecter");
    gtk_widget_add_css_class (ok, "suggested-action");
    gtk_box_append (GTK_BOX (barre), annuler);
    gtk_box_append (GTK_BOX (barre), ok);
    gtk_box_append (GTK_BOX (boite), barre);

    g_signal_connect_data (ok, "clicked", G_CALLBACK (on_mdp_valide), d, demande_free, 0);
    g_signal_connect (annuler, "clicked", G_CALLBACK (on_mdp_annule), d);
    g_signal_connect (d->champ, "activate", G_CALLBACK (on_mdp_valide), d);

    gtk_window_set_child (GTK_WINDOW (d->fenetre), boite);
    gtk_window_present (GTK_WINDOW (d->fenetre));
    gtk_widget_grab_focus (d->champ);
}

/* ------------------------------------------------------------------------- */
static void
connecter_lecteur (Lieux *L, const Lecteur *l, const char *mot_de_passe,
                   gboolean retenir)
{
    if (g_hash_table_contains (L->en_cours, l->id))
        return;                       /* deja en route : ne pas doubler */

    /* COPIE D'ABORD, ET C'EST OBLIGATOIRE.
     *
     * `l` pointe DANS L->lecteurs. Or reconstruire() relit le fichier :
     * il libere ce tableau et le remplace. Le `l` recu ici designe alors
     * de la memoire rendue, et le premier g_strdup de lecteur_copie la
     * lit. SIGSEGV a chaque clic sur un lecteur non connecte -- mesure
     * deux fois le 9 septembre 2026, pile de coredumpctl a l'appui.
     *
     * La lecon generale : dans ce fichier, tout ce qui vient de
     * L->lecteurs meurt au prochain reconstruire(). */
    g_autoptr(Lecteur) cible = lecteur_copie (l);

    Attente *a = g_new0 (Attente, 1);
    a->L        = L;
    a->id       = g_strdup (cible->id);
    a->naviguer = TRUE;

    g_hash_table_add (L->en_cours, g_strdup (cible->id));
    reconstruire (L);                 /* la ligne prend son sablier */

    reseau_connecter (cible, mot_de_passe, retenir, on_lecteur_fini, a);
}

static void
on_lecteur_active (Lieux *L, const char *id)
{
    const Lecteur *l = lecteur_par_id (L, id);
    if (l == NULL)
        return;

    if (reseau_est_connecte (l)) {
        g_autofree char *point = reseau_point_montage (l);
        g_autoptr(GFile) f = g_file_new_for_path (point);
        L->nav (f, L->data);
        return;
    }

    /* Sans nom d'utilisateur, le partage est declare ouvert : on tente en
     * invite. Avec un nom, on laisse le trousseau repondre -- et c'est
     * seulement s'il ne repond pas que la fenetre s'ouvrira. */
    connecter_lecteur (L, l, NULL, FALSE);
}

static void
on_lecteur_deconnecte (const Lecteur *l, GError *erreur, gpointer data)
{
    Attente *a = data;

    g_hash_table_remove (a->L->en_cours, a->id);

    if (erreur != NULL) {
        GtkAlertDialog *d = gtk_alert_dialog_new ("Déconnexion impossible de « %s »", l->nom);
        gtk_alert_dialog_set_detail (d, erreur->message);
        gtk_alert_dialog_show (d, fenetre_de (a->L));
        g_object_unref (d);
    }
    reconstruire (a->L);
    attente_free (a);
}

static void
on_deconnecter (GtkButton *b, gpointer data)
{
    Lieux *L = data;
    const char *id = g_object_get_data (G_OBJECT (b), "lecteur-id");
    const Lecteur *l = lecteur_par_id (L, id);

    if (l == NULL || g_hash_table_contains (L->en_cours, id))
        return;

    Attente *a = g_new0 (Attente, 1);
    a->L  = L;
    a->id = g_strdup (id);

    g_hash_table_add (L->en_cours, g_strdup (id));
    reseau_deconnecter (l, on_lecteur_deconnecte, a);
}

/* Une ligne de lecteur reseau. */
static GtkWidget *
entree_lecteur (Lieux *L, const Lecteur *l)
{
    gboolean connecte = reseau_est_connecte (l);
    gboolean en_route = g_hash_table_contains (L->en_cours, l->id);

    GtkWidget *ligne = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

    /* Trois etats, trois images. Le sablier n'est pas un ornement : une
     * connexion vers un serveur eteint dure le delai TCP complet, et sans
     * lui la ligne semblerait inerte. */
    GtkWidget *img;
    if (en_route) {
        img = gtk_spinner_new ();
        gtk_spinner_set_spinning (GTK_SPINNER (img), TRUE);
        gtk_widget_set_size_request (img, 16, 16);
    } else {
        /* Deux noms par etat, et non un seul : « folder-remote » n'est,
         * dans Papirus, qu'une paire de fleches -- illisible a 16 px et
         * sans parente visuelle avec les dossiers de la meme colonne.
         * Le second nom sert aussi de repli pour les themes d'icones qui
         * ne connaitraient pas le premier. */
        g_autoptr(GIcon) ic = g_themed_icon_new (connecte ? "folder-network"
                                                          : "network-server");
        g_themed_icon_append_name (G_THEMED_ICON (ic),
                                   connecte ? "folder-remote" : "network-workgroup");
        img = gtk_image_new_from_gicon (ic);
        gtk_image_set_pixel_size (GTK_IMAGE (img), 16);
    }
    gtk_widget_add_css_class (img, "lieux-icone");

    GtkWidget *lbl = gtk_label_new (l->nom);
    gtk_widget_add_css_class (lbl, "lieux-nom");
    gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign (lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand (lbl, TRUE);

    gtk_box_append (GTK_BOX (ligne), img);
    gtk_box_append (GTK_BOX (ligne), lbl);

    if (connecte && !en_route) {
        GtkWidget *dc = gtk_button_new_from_icon_name ("media-eject-symbolic");
        gtk_widget_add_css_class (dc, "lieux-ejecter");
        gtk_widget_set_valign (dc, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text (dc, "Se déconnecter");
        g_object_set_data_full (G_OBJECT (dc), "lecteur-id", g_strdup (l->id), g_free);
        g_signal_connect (dc, "clicked", G_CALLBACK (on_deconnecter), L);
        gtk_box_append (GTK_BOX (ligne), dc);
    }

    GtkWidget *row = gtk_list_box_row_new ();
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), ligne);
    gtk_widget_add_css_class (row, "lieux-ligne");
    if (!connecte)
        gtk_widget_add_css_class (row, "lieux-hors-ligne");

    /* Connecte, la ligne porte son dossier : le surlignage du volet et la
     * navigation marchent alors exactement comme pour un dossier local,
     * sans un cas particulier de plus dans fichiers_lieux_suivre. */
    if (connecte) {
        g_autofree char *point = reseau_point_montage (l);
        g_autoptr(GFile) f = g_file_new_for_path (point);
        g_object_set_data_full (G_OBJECT (row), "fichier", g_object_ref (f), g_object_unref);
    }
    g_object_set_data_full (G_OBJECT (row), "lecteur-id", g_strdup (l->id), g_free);

    g_autofree char *info = g_strdup_printf (
        "%s\n%s%s%s\n%s",
        reseau_protocole_nom (l->protocole),
        l->serveur, *l->partage ? " / " : "", l->partage,
        connecte ? "Connecté" : "Cliquer pour se connecter");
    gtk_widget_set_tooltip_text (row, info);

    return row;
}

/* ------------------------------------------------------------------------- */
static GtkWidget *
entete (const char *titre)
{
    GtkWidget *l = gtk_label_new (titre);
    gtk_widget_add_css_class (l, "lieux-entete");
    gtk_widget_set_halign (l, GTK_ALIGN_START);

    GtkWidget *row = gtk_list_box_row_new ();
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), l);
    gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
    gtk_widget_add_css_class (row, "lieux-entete-row");
    return row;
}

/* Une entree. `cible` est le dossier a ouvrir, ou NULL pour un volume a
 * monter ; `mnt` non NULL ajoute le bouton d'ejection. */
static GtkWidget *
entree (Lieux *L, const char *nom, GIcon *icone, GFile *cible,
        GVolume *vol, GMount *mnt, const char *favori)
{
    GtkWidget *img = gtk_image_new_from_gicon (icone);
    gtk_image_set_pixel_size (GTK_IMAGE (img), 16);
    gtk_widget_add_css_class (img, "lieux-icone");

    GtkWidget *lbl = gtk_label_new (nom);
    gtk_widget_add_css_class (lbl, "lieux-nom");
    gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign (lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand (lbl, TRUE);

    GtkWidget *ligne = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append (GTK_BOX (ligne), img);
    gtk_box_append (GTK_BOX (ligne), lbl);

    if (mnt != NULL && (g_mount_can_eject (mnt) || g_mount_can_unmount (mnt))) {
        GtkWidget *ej = gtk_button_new_from_icon_name ("media-eject-symbolic");
        gtk_widget_add_css_class (ej, "lieux-ejecter");
        gtk_widget_set_valign (ej, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text (ej, "Éjecter");
        g_object_set_data_full (G_OBJECT (ej), "montage", g_object_ref (mnt), g_object_unref);
        g_signal_connect (ej, "clicked", G_CALLBACK (on_ejecter), L);
        gtk_box_append (GTK_BOX (ligne), ej);
    }

    GtkWidget *row = gtk_list_box_row_new ();
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), ligne);
    gtk_widget_add_css_class (row, "lieux-ligne");

    if (cible != NULL)
        g_object_set_data_full (G_OBJECT (row), "fichier", g_object_ref (cible), g_object_unref);
    if (vol != NULL)
        g_object_set_data_full (G_OBJECT (row), "volume", g_object_ref (vol), g_object_unref);

    /* Un favori ajoute a la main se retire au clic droit. Les dossiers
     * personnels et les peripheriques, eux, ne se retirent pas : ils ne
     * viennent pas d'un choix, ils constatent ce qui existe. */
    if (favori != NULL) {
        g_autoptr(GMenu) menu = g_menu_new ();
        g_autofree char *d = g_strdup_printf ("lieux.retirer::%s", favori);
        g_menu_append (menu, "Retirer des favoris", d);

        GtkWidget *pop = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
        gtk_widget_add_css_class (pop, "fichiers-menu");
        gtk_popover_set_has_arrow (GTK_POPOVER (pop), FALSE);
        gtk_widget_set_parent (pop, row);
        g_object_set_data (G_OBJECT (row), "menu", pop);

        GtkGestureClick *g = GTK_GESTURE_CLICK (gtk_gesture_click_new ());
        gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (g), GDK_BUTTON_SECONDARY);
        g_signal_connect_swapped (g, "pressed", G_CALLBACK (gtk_popover_popup), pop);
        gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (g));

        /* Un popover attache par set_parent doit etre detache avant que son
         * parent ne disparaisse, sinon GTK signale un widget detruit avec
         * des enfants encore attaches -- et le volet se reconstruit a chaque
         * branchement de cle USB. */
        g_signal_connect_swapped (row, "destroy", G_CALLBACK (gtk_widget_unparent), pop);
    }

    return row;
}

static void
ajouter_chemin (Lieux *L, const char *chemin, const char *nom,
                const char *icone, const char *favori)
{
    if (chemin == NULL)
        return;
    if (!g_file_test (chemin, G_FILE_TEST_IS_DIR))
        return;                    /* un dossier absent n'a rien a faire la */

    g_autoptr(GFile) f = g_file_new_for_path (chemin);
    g_autoptr(GIcon) ic = g_themed_icon_new (icone);
    g_autofree char *base = (nom != NULL) ? NULL : g_path_get_basename (chemin);

    gtk_list_box_append (GTK_LIST_BOX (L->liste),
                         entree (L, nom ? nom : base, ic, f, NULL, NULL, favori));
}

static void
reconstruire (Lieux *L)
{
    GtkWidget *enfant;
    while ((enfant = gtk_widget_get_first_child (L->liste)) != NULL)
        gtk_list_box_remove (GTK_LIST_BOX (L->liste), enfant);

    /* --- dossiers personnels --- */
    gtk_list_box_append (GTK_LIST_BOX (L->liste), entete ("Emplacements"));

    ajouter_chemin (L, g_get_home_dir (), "Dossier personnel", "user-home", NULL);
    const struct { GUserDirectory d; const char *nom; const char *ic; } perso[] = {
        { G_USER_DIRECTORY_DESKTOP,   "Bureau",        "user-desktop"   },
        { G_USER_DIRECTORY_DOWNLOAD,  "Téléchargements","folder-download" },
        { G_USER_DIRECTORY_DOCUMENTS, "Documents",     "folder-documents" },
        { G_USER_DIRECTORY_PICTURES,  "Images",        "folder-pictures" },
        { G_USER_DIRECTORY_MUSIC,     "Musique",       "folder-music"   },
        { G_USER_DIRECTORY_VIDEOS,    "Vidéos",        "folder-videos"  },
    };
    for (guint i = 0; i < G_N_ELEMENTS (perso); i++)
        ajouter_chemin (L, g_get_user_special_dir (perso[i].d), perso[i].nom,
                        perso[i].ic, NULL);

    g_autofree char *corbeille = g_build_filename (g_get_user_data_dir (),
                                                   "Trash", "files", NULL);
    /* Creee au besoin : sans elle, la Corbeille n'apparaitrait pas tant que
     * rien n'a jamais ete jete, ce qui est deroutant. */
    g_mkdir_with_parents (corbeille, 0700);
    ajouter_chemin (L, corbeille, "Corbeille", "user-trash", NULL);

    ajouter_chemin (L, "/", "Ordinateur", "drive-harddisk", NULL);

    /* --- favoris --- */
    if (L->favoris->len > 0) {
        gtk_list_box_append (GTK_LIST_BOX (L->liste), entete ("Favoris"));
        for (guint i = 0; i < L->favoris->len; i++) {
            const char *chemin = g_ptr_array_index (L->favoris, i);
            g_autofree char *nom = g_path_get_basename (chemin);
            ajouter_chemin (L, chemin, nom, "folder", chemin);
        }
    }

    /* --- peripheriques --- */
    GList *montages = g_volume_monitor_get_mounts (L->moniteur);
    GList *volumes  = g_volume_monitor_get_volumes (L->moniteur);
    gboolean titre = FALSE;

    for (GList *m = montages; m != NULL; m = m->next) {
        GMount *mnt = m->data;
        if (g_mount_is_shadowed (mnt))
            continue;

        if (!titre) {
            gtk_list_box_append (GTK_LIST_BOX (L->liste), entete ("Périphériques"));
            titre = TRUE;
        }
        g_autofree char *nom = g_mount_get_name (mnt);
        g_autoptr(GIcon) ic = g_mount_get_icon (mnt);
        g_autoptr(GFile) racine = g_mount_get_root (mnt);
        gtk_list_box_append (GTK_LIST_BOX (L->liste),
                             entree (L, nom, ic, racine, NULL, mnt, NULL));
    }

    /* Volumes connus mais pas montes : une cle branchee que personne n'a
     * encore ouverte. Les afficher permet de la monter d'un clic. */
    for (GList *v = volumes; v != NULL; v = v->next) {
        GVolume *vol = v->data;
        g_autoptr(GMount) deja = g_volume_get_mount (vol);
        if (deja != NULL || !g_volume_can_mount (vol))
            continue;

        if (!titre) {
            gtk_list_box_append (GTK_LIST_BOX (L->liste), entete ("Périphériques"));
            titre = TRUE;
        }
        g_autofree char *nom = g_volume_get_name (vol);
        g_autoptr(GIcon) ic = g_volume_get_icon (vol);
        gtk_list_box_append (GTK_LIST_BOX (L->liste),
                             entree (L, nom, ic, NULL, vol, NULL, NULL));
    }

    g_list_free_full (montages, g_object_unref);
    g_list_free_full (volumes, g_object_unref);

    /* --- lecteurs reseau --- */
    /* Relus a chaque reconstruction : le panneau de reglages ecrit le
     * fichier, et le volet s'aligne sans qu'aucun protocole n'ait ete
     * invente entre les deux -- exactement ce que fait shell.conf pour le
     * reste du bureau. */
    lecteurs_relire (L);

    if (L->lecteurs->len > 0) {
        gtk_list_box_append (GTK_LIST_BOX (L->liste), entete ("Lecteurs réseau"));
        for (guint i = 0; i < L->lecteurs->len; i++)
            gtk_list_box_append (GTK_LIST_BOX (L->liste),
                                 entree_lecteur (L, g_ptr_array_index (L->lecteurs, i)));
    }

    /* --- le nuage --- */
    /* Google Drive et OneDrive viendront ici, lus depuis la configuration de
     * rclone : une section de plus, construite comme les autres. Rien n'est
     * affiche tant que rien n'est configure -- une entree qui ne mene nulle
     * part serait pire que son absence. */

    fichiers_lieux_suivre (L->boite, L->courant);
}

static void
on_volumes_changes (GVolumeMonitor *m, gpointer objet, gpointer data)
{
    (void) m; (void) objet;
    reconstruire (data);
}

/* ------------------------------------------------------------------------- */
void
fichiers_lieux_suivre (GtkWidget *widget, GFile *dossier)
{
    Lieux *L = g_object_get_data (G_OBJECT (widget), "lieux");
    if (L == NULL)
        return;

    if (dossier != L->courant) {
        g_clear_object (&L->courant);
        L->courant = dossier ? g_object_ref (dossier) : NULL;
    }

    gtk_list_box_unselect_all (GTK_LIST_BOX (L->liste));
    if (L->courant == NULL)
        return;

    for (GtkWidget *r = gtk_widget_get_first_child (L->liste);
         r != NULL; r = gtk_widget_get_next_sibling (r)) {
        GFile *f = g_object_get_data (G_OBJECT (r), "fichier");
        if (f != NULL && g_file_equal (f, L->courant)) {
            gtk_list_box_select_row (GTK_LIST_BOX (L->liste), GTK_LIST_BOX_ROW (r));
            return;
        }
    }
}

gboolean
fichiers_lieux_est_favori (GtkWidget *widget, GFile *dossier)
{
    Lieux *L = g_object_get_data (G_OBJECT (widget), "lieux");
    g_autofree char *chemin = g_file_get_path (dossier);
    if (L == NULL || chemin == NULL)
        return FALSE;

    for (guint i = 0; i < L->favoris->len; i++)
        if (g_strcmp0 (g_ptr_array_index (L->favoris, i), chemin) == 0)
            return TRUE;
    return FALSE;
}

void
fichiers_lieux_ajouter (GtkWidget *widget, GFile *dossier)
{
    Lieux *L = g_object_get_data (G_OBJECT (widget), "lieux");
    g_autofree char *chemin = g_file_get_path (dossier);

    if (L == NULL || chemin == NULL || fichiers_lieux_est_favori (widget, dossier))
        return;

    g_ptr_array_add (L->favoris, g_strdup (chemin));
    favoris_ecrire (L);
    reconstruire (L);
}

void
fichiers_lieux_retirer (GtkWidget *widget, GFile *dossier)
{
    Lieux *L = g_object_get_data (G_OBJECT (widget), "lieux");
    g_autofree char *chemin = g_file_get_path (dossier);
    if (L == NULL || chemin == NULL)
        return;

    for (guint i = 0; i < L->favoris->len; i++) {
        if (g_strcmp0 (g_ptr_array_index (L->favoris, i), chemin) == 0) {
            g_ptr_array_remove_index (L->favoris, i);
            favoris_ecrire (L);
            reconstruire (L);
            return;
        }
    }
}

static void
lieux_free (gpointer data)
{
    Lieux *L = data;
    g_clear_object (&L->moniteur);
    g_clear_object (&L->courant);
    g_ptr_array_unref (L->favoris);
    g_clear_pointer (&L->lecteurs, g_ptr_array_unref);
    g_clear_pointer (&L->en_cours, g_hash_table_unref);

    /* Le moniteur des montages est un singleton qui survit au volet. Sans
     * ce debranchement, un partage demonte apres la fermeture de la fenetre
     * appellerait reconstruire() sur un Lieux libere. */
    reseau_ne_plus_surveiller (L);
    g_free (L);
}

GtkWidget *
fichiers_lieux_new (LieuxNavFunc nav, gpointer data)
{
    Lieux *L = g_new0 (Lieux, 1);
    L->nav     = nav;
    L->data    = data;
    L->favoris = g_ptr_array_new_with_free_func (g_free);
    L->en_cours = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    L->lecteurs = reseau_charger ();
    favoris_lire (L);

    L->liste = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (L->liste), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class (L->liste, "lieux");
    g_signal_connect (L->liste, "row-activated", G_CALLBACK (on_ligne_activee), L);

    GtkWidget *defil = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (defil),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (defil), L->liste);
    gtk_widget_set_vexpand (defil, TRUE);

    L->boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (L->boite, "lieux-volet");
    gtk_box_append (GTK_BOX (L->boite), defil);
    gtk_widget_set_size_request (L->boite, 190, -1);

    g_object_set_data_full (G_OBJECT (L->boite), "lieux", L, lieux_free);

    /* Le moniteur previent quand une cle est branchee ou retiree. Aucune
     * consultation periodique : on ne se reveille que sur evenement. */
    L->moniteur = g_volume_monitor_get ();
    const char *signaux[] = { "mount-added", "mount-removed", "mount-changed",
                              "volume-added", "volume-removed", NULL };
    for (guint i = 0; signaux[i] != NULL; i++)
        g_signal_connect (L->moniteur, signaux[i],
                          G_CALLBACK (on_volumes_changes), L);

    /* GVolumeMonitor ne voit PAS les montages du noyau poses a la main : il
     * ne rapporte que ce qu'udisks2 lui annonce, c'est-a-dire les disques.
     * Un partage monte depuis un terminal, ou demonte parce que le Wi-Fi est
     * tombe, passerait donc inapercu. D'ou ce second moniteur, qui lit la
     * notification du noyau sur la table des montages. */
    reseau_surveiller ((ReseauChangeFunc) reconstruire, L);

    GSimpleActionGroup *groupe = g_simple_action_group_new ();
    const GActionEntry actions[] = {
        { "retirer", on_retirer_favori, "s", NULL, NULL, { 0 } },
    };
    g_action_map_add_action_entries (G_ACTION_MAP (groupe), actions,
                                     G_N_ELEMENTS (actions), L);
    gtk_widget_insert_action_group (L->boite, "lieux", G_ACTION_GROUP (groupe));
    g_object_unref (groupe);

    reconstruire (L);
    return L->boite;
}
