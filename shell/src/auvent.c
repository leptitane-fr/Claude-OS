#include "auvent.h"

#include "outils.h"

/* Durée du déploiement. Plus court que le retournement (260 ms) : l'auvent
 * ne demande rien à comprendre, il apporte un champ. Ce qui doit être rapide
 * l'est, ce qui doit être lu prend son temps. */
#define DUREE_MS 180

struct _ShellAuvent {
    GtkWidget  parent_instance;

    GtkWidget *revelateur;
    GtkWidget *panneau;     /* la surface de l'auvent, ce qui se voit    */
    GtkWidget *contenu;     /* le contrôle du moment                     */

    char      *controle;
    ShellAuventValeur    valeur;
    gpointer             valeur_data;
    ShellAuventOuverture ouverture;
    gpointer             ouverture_data;
};

G_DEFINE_FINAL_TYPE (ShellAuvent, shell_auvent, GTK_TYPE_WIDGET)

/* ------------------------------------------------------------------------- */
static void
dire (ShellAuvent *a, const char *valeur)
{
    if (a->valeur != NULL)
        a->valeur (valeur, a->valeur_data);
}

static void
on_saisie (GtkEditable *e, gpointer data)
{
    /* FRAPPE PAR FRAPPE, et c'est voulu : une recherche doit filtrer pendant
     * qu'on tape. L'application décide si cela lui coûte cher -- elle seule
     * sait ce qu'il y a derrière. */
    dire (SHELL_AUVENT (data), gtk_editable_get_text (e));
}

static gboolean
on_echap (GtkEventControllerKey *c, guint touche, guint code,
          GdkModifierType mods, gpointer data)
{
    (void) c; (void) code; (void) mods;

    /* Échap referme. C'est le geste qu'on essaie sans qu'on vous l'explique,
     * et il évite d'avoir à retrouver le bouton qui a ouvert le volet. */
    if (touche == GDK_KEY_Escape) {
        shell_auvent_fermer (SHELL_AUVENT (data));
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

/* -------------------------------------------------------------------------
 * Les contrôles
 * ------------------------------------------------------------------------- */
static GtkWidget *
controle_saisie (ShellAuvent *a, const char *invite)
{
    GtkWidget *champ = gtk_entry_new ();
    gtk_widget_add_css_class (champ, "auvent-saisie");
    gtk_entry_set_placeholder_text (GTK_ENTRY (champ), invite);

    /* Assez large pour qu'on voie ce qu'on écrit, assez étroit pour que la
     * pilule ne double pas de largeur. */
    gtk_editable_set_width_chars (GTK_EDITABLE (champ), 28);

    g_signal_connect (champ, "changed", G_CALLBACK (on_saisie), a);

    GtkEventController *touches = gtk_event_controller_key_new ();
    g_signal_connect (touches, "key-pressed", G_CALLBACK (on_echap), a);
    gtk_widget_add_controller (champ, touches);

    return champ;
}

/* Une liste : une ligne par entrée du sous-menu, chacune avec SON action et
 * SA cible.
 *
 * Le groupe d'actions n'est pas cherché ici : l'auvent descend de l'établi,
 * qui l'a inséré sous « outils » (etabli.c). GTK remonte l'arbre pour
 * résoudre « outils.aller », et les lignes agissent comme les boutons de la
 * rangée -- même chemin, même contrat.
 *
 * VERTICALE, ET DANS L'ORDRE DU MODÈLE. Pour un chemin, cela le fait lire de
 * la racine vers le dossier courant, de haut en bas : c'est le sens d'un
 * fil d'Ariane qu'on aurait redressé. */
static GtkWidget *
controle_liste (ShellAuvent *a, GMenuModel *lignes)
{
    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class (boite, "auvent-liste");

    int n = lignes ? g_menu_model_get_n_items (lignes) : 0;
    for (int i = 0; i < n; i++) {
        g_autoptr(GVariant) v_label = g_menu_model_get_item_attribute_value (
            lignes, i, G_MENU_ATTRIBUTE_LABEL, G_VARIANT_TYPE_STRING);
        g_autoptr(GVariant) v_action = g_menu_model_get_item_attribute_value (
            lignes, i, G_MENU_ATTRIBUTE_ACTION, G_VARIANT_TYPE_STRING);
        g_autoptr(GVariant) but = g_menu_model_get_item_attribute_value (
            lignes, i, G_MENU_ATTRIBUTE_TARGET, NULL);

        GtkWidget *b = gtk_button_new_with_label (
            v_label ? g_variant_get_string (v_label, NULL) : "…");
        gtk_widget_add_css_class (b, "auvent-ligne");
        gtk_button_set_has_frame (GTK_BUTTON (b), FALSE);
        gtk_widget_set_halign (b, GTK_ALIGN_FILL);

        /* Le libellé cale à gauche : une liste de chemins centrée est
         * illisible, les noms n'ont pas la même longueur. */
        GtkWidget *l = gtk_button_get_child (GTK_BUTTON (b));
        if (GTK_IS_LABEL (l)) {
            gtk_label_set_xalign (GTK_LABEL (l), 0.0);
            gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_MIDDLE);
        }

        if (v_action != NULL) {
            gtk_actionable_set_action_name (GTK_ACTIONABLE (b),
                                            g_variant_get_string (v_action, NULL));
            if (but != NULL)
                gtk_actionable_set_action_target_value (GTK_ACTIONABLE (b), but);
        }

        /* Choisir une ligne referme le volet : on a obtenu ce pour quoi on
         * l'avait ouvert. Sans cela il resterait en travers, et il tient le
         * clavier. */
        g_signal_connect_swapped (b, "clicked",
                                  G_CALLBACK (shell_auvent_fermer), a);
        gtk_box_append (GTK_BOX (boite), b);
    }
    return boite;
}

/* ------------------------------------------------------------------------- */
gboolean
shell_auvent_ouvrir (ShellAuvent *a, const char *controle, const char *invite,
                     GMenuModel *lignes, ShellAuventValeur f, gpointer data)
{
    g_return_val_if_fail (SHELL_IS_AUVENT (a), FALSE);

    if (controle == NULL)
        controle = SHELL_OUTILS_SAISIE;

    gboolean saisie = g_str_equal (controle, SHELL_OUTILS_SAISIE);
    gboolean liste  = g_str_equal (controle, SHELL_OUTILS_LISTE);

    /* CE QU'ON NE SAIT PAS DESSINER SE REFUSE, ET SE DIT. Ouvrir un volet
     * vide laisserait croire à une panne du dock, là où c'est le contrat qui
     * n'est pas encore rempli. */
    if (!saisie && !liste) {
        g_message ("auvent : contrôle « %s » pas encore écrit — rien ouvert",
                   controle);
        return FALSE;
    }

    /* Une liste sans lignes n'est pas une liste. Le dire plutôt que de faire
     * monter un volet vide, que l'utilisateur prendrait pour une panne. */
    if (liste && (lignes == NULL || g_menu_model_get_n_items (lignes) == 0)) {
        g_message ("auvent : liste vide — rien ouvert");
        return FALSE;
    }

    a->valeur      = f;
    a->valeur_data = data;

    /* Le même contrôle qu'on redemande : on ne referme pas, on rend le
     * clavier au champ. Une bascule ferait disparaître ce qu'on était en
     * train d'écrire, et c'est exactement ce qu'on fait sans y penser en
     * recliquant la loupe. */
    if (shell_auvent_ouvert (a) && g_strcmp0 (a->controle, controle) == 0) {
        gtk_widget_grab_focus (a->contenu);
        return TRUE;
    }

    if (a->contenu != NULL) {
        gtk_box_remove (GTK_BOX (a->panneau), a->contenu);
        a->contenu = NULL;
    }

    g_free (a->controle);
    a->controle = g_strdup (controle);
    a->contenu  = saisie ? controle_saisie (a, invite)
                         : controle_liste (a, lignes);
    gtk_box_append (GTK_BOX (a->panneau), a->contenu);

    /* PRÉVENIR AVANT D'OUVRIR : le dock pose le mode clavier de sa surface
     * et ferme ses popovers, et la hauteur va changer. */
    if (a->ouverture != NULL)
        a->ouverture (TRUE, a->ouverture_data);

    gtk_revealer_set_reveal_child (GTK_REVEALER (a->revelateur), TRUE);
    gtk_widget_grab_focus (a->contenu);
    return TRUE;
}

void
shell_auvent_fermer (ShellAuvent *a)
{
    if (!shell_auvent_ouvert (a))
        return;

    gtk_revealer_set_reveal_child (GTK_REVEALER (a->revelateur), FALSE);

    /* UNE DERNIÈRE VALEUR, VIDE — mais pour une SAISIE seulement. C'est ainsi
     * qu'une application sait qu'il faut rendre la liste complète : sans
     * elle, une recherche refermée laisserait le filtre en place, et l'on
     * chercherait pourquoi la moitié de ses fichiers a disparu.
     *
     * Une liste, elle, n'a jamais rien produit : lui faire envoyer une chaîne
     * vide reviendrait à déclencher une action qu'on n'a pas demandée. */
    if (g_strcmp0 (a->controle, SHELL_OUTILS_SAISIE) == 0)
        dire (a, "");

    if (a->ouverture != NULL)
        a->ouverture (FALSE, a->ouverture_data);
}

gboolean
shell_auvent_ouvert (ShellAuvent *a)
{
    return a != NULL
        && gtk_revealer_get_reveal_child (GTK_REVEALER (a->revelateur));
}

const char *
shell_auvent_controle (ShellAuvent *a)
{
    return a ? a->controle : NULL;
}

void
shell_auvent_sur_ouverture (ShellAuvent *a, ShellAuventOuverture f, gpointer data)
{
    a->ouverture      = f;
    a->ouverture_data = data;
}

/* -------------------------------------------------------------------------
 * Cycle de vie
 * ------------------------------------------------------------------------- */
static void
auvent_measure (GtkWidget *w, GtkOrientation o, int pour,
                int *min, int *nat, int *min_base, int *nat_base)
{
    ShellAuvent *a = SHELL_AUVENT (w);
    gtk_widget_measure (a->revelateur, o, pour, min, nat, min_base, nat_base);
}

static void
auvent_size_allocate (GtkWidget *w, int largeur, int hauteur, int base)
{
    ShellAuvent *a = SHELL_AUVENT (w);
    gtk_widget_allocate (a->revelateur, largeur, hauteur, base, NULL);
}

static void
auvent_dispose (GObject *o)
{
    ShellAuvent *a = SHELL_AUVENT (o);
    g_clear_pointer (&a->controle, g_free);
    g_clear_pointer (&a->revelateur, gtk_widget_unparent);
    G_OBJECT_CLASS (shell_auvent_parent_class)->dispose (o);
}

static void
shell_auvent_class_init (ShellAuventClass *klass)
{
    GObjectClass   *oc = G_OBJECT_CLASS (klass);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS (klass);

    oc->dispose       = auvent_dispose;
    wc->measure       = auvent_measure;
    wc->size_allocate = auvent_size_allocate;
}

static void
shell_auvent_init (ShellAuvent *a)
{
    a->panneau = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class (a->panneau, "auvent");
    gtk_widget_set_halign (a->panneau, GTK_ALIGN_CENTER);

    a->revelateur = gtk_revealer_new ();
    /* SLIDE_UP : le volet SORT du dock vers le haut. Un fondu le ferait
     * apparaître de nulle part ; c'est le mouvement qui dit d'où il vient. */
    gtk_revealer_set_transition_type (GTK_REVEALER (a->revelateur),
                                      GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration (GTK_REVEALER (a->revelateur), DUREE_MS);
    gtk_revealer_set_child (GTK_REVEALER (a->revelateur), a->panneau);
    gtk_widget_set_parent (a->revelateur, GTK_WIDGET (a));
}

GtkWidget *
shell_auvent_new (void)
{
    return g_object_new (SHELL_TYPE_AUVENT, NULL);
}
