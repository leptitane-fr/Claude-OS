#include "preavis.h"
#include "config.h"

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

/* Hauteur laissee libre en bas pour le dock et la barre d'etat. Meme
 * raisonnement que BANDE_DOCK dans launcher.c : le dock mesure 88 px, on
 * arrondit pour que le decompte ne flotte pas au ras des icones. */
#define BANDE_BASSE 108

static struct {
    GtkWidget *fenetre;
    GtkWidget *texte;
    guint      minuterie;
    int        reste;
} P;

static void
peindre (void)
{
    g_autofree char *t = g_strdup_printf ("Veille dans %d s", P.reste);
    gtk_label_set_text (GTK_LABEL (P.texte), t);
}

static gboolean
on_tic (gpointer data)
{
    (void) data;
    if (--P.reste <= 0) {
        /* On ne masque pas ici : c'est l'etage « attenuer » qui suit
         * immediatement et qui appellera shell_preavis_cacher(). Masquer
         * maintenant ferait clignoter l'ecran juste avant qu'il ne baisse. */
        P.reste = 0;
        peindre ();
        P.minuterie = 0;
        return G_SOURCE_REMOVE;
    }
    peindre ();
    return G_SOURCE_CONTINUE;
}

static void
on_realise (GtkWidget *w, gpointer data)
{
    (void) data;
    GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (w));
    if (surface == NULL)
        return;

    /* REGION D'ENTREE VIDE : la surface se voit et ne s'attrape pas. Sans
     * cela elle poserait un rectangle mort par-dessus le bureau, et un clic
     * destine a ce qui se trouve dessous serait avale. */
    cairo_region_t *vide = cairo_region_create ();
    gdk_surface_set_input_region (surface, vide);
    cairo_region_destroy (vide);
}

static void
construire (void)
{
    if (P.fenetre != NULL)
        return;

    P.fenetre = gtk_window_new ();
    gtk_widget_add_css_class (P.fenetre, "shell");
    gtk_widget_add_css_class (P.fenetre, "preavis");

    gtk_layer_init_for_window (GTK_WINDOW (P.fenetre));
    gtk_layer_set_layer (GTK_WINDOW (P.fenetre), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace (GTK_WINDOW (P.fenetre), "claude-os-preavis");
    /* Aucun clavier : le decompte ne doit pas interrompre une frappe. */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (P.fenetre),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_anchor (GTK_WINDOW (P.fenetre), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (P.fenetre), GTK_LAYER_SHELL_EDGE_RIGHT,  TRUE);
    gtk_layer_set_margin (GTK_WINDOW (P.fenetre), GTK_LAYER_SHELL_EDGE_BOTTOM,
                          BANDE_BASSE);
    gtk_layer_set_margin (GTK_WINDOW (P.fenetre), GTK_LAYER_SHELL_EDGE_RIGHT, 12);

    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class (boite, "preavis-pastille");

    P.texte = gtk_label_new ("");
    gtk_widget_add_css_class (P.texte, "preavis-compte");
    gtk_box_append (GTK_BOX (boite), P.texte);

    GtkWidget *aide = gtk_label_new ("Un geste suffit à l'annuler");
    gtk_widget_add_css_class (aide, "preavis-aide");
    gtk_box_append (GTK_BOX (boite), aide);

    gtk_window_set_child (GTK_WINDOW (P.fenetre), boite);
    g_signal_connect (P.fenetre, "realize", G_CALLBACK (on_realise), NULL);
}

void
shell_preavis_montrer (int secondes)
{
    if (secondes <= 0)
        return;

    construire ();
    P.reste = secondes;
    peindre ();

    if (P.minuterie != 0)
        g_source_remove (P.minuterie);
    P.minuterie = g_timeout_add_seconds (1, on_tic, NULL);

    gtk_widget_set_visible (P.fenetre, TRUE);
}

void
shell_preavis_cacher (void)
{
    if (P.minuterie != 0) {
        g_source_remove (P.minuterie);
        P.minuterie = 0;
    }
    if (P.fenetre != NULL)
        gtk_widget_set_visible (P.fenetre, FALSE);
}
