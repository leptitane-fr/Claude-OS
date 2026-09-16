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

/* ------------------------------------------------------------------------- */
gboolean
shell_auvent_ouvrir (ShellAuvent *a, const char *controle, const char *invite,
                     ShellAuventValeur f, gpointer data)
{
    g_return_val_if_fail (SHELL_IS_AUVENT (a), FALSE);

    if (controle == NULL)
        controle = SHELL_OUTILS_SAISIE;

    /* CE QU'ON NE SAIT PAS DESSINER SE REFUSE, ET SE DIT. Ouvrir un volet
     * vide laisserait croire à une panne du dock, là où c'est le contrat qui
     * n'est pas encore rempli. */
    if (!g_str_equal (controle, SHELL_OUTILS_SAISIE)) {
        g_message ("auvent : contrôle « %s » pas encore écrit — rien ouvert",
                   controle);
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
    a->contenu  = controle_saisie (a, invite);
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

    /* UNE DERNIÈRE VALEUR, VIDE. C'est ainsi qu'une application sait qu'il
     * faut rendre la liste complète : sans elle, une recherche refermée
     * laisserait le filtre en place, et l'utilisateur chercherait pourquoi
     * la moitié de ses fichiers a disparu. */
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
    a->panneau = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
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
