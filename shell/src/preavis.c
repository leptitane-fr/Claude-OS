#include "preavis.h"

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

/* Hauteur laissee libre en bas pour le dock et la barre d'etat. Meme
 * raisonnement que BANDE_DOCK dans launcher.c : le dock mesure 88 px, on
 * arrondit pour que le cadran ne flotte pas au ras des icones. */
#define BANDE_BASSE 108
/* Diametre de repli, si aucune reference n'a ete posee. La pilule mesurait
 * 172 px le 9 septembre 2026 ; on s'en approche sans en dependre. */
#define COTE_REPLI  160
#define COTE_MINI   80     /* garde-fous : une reference non encore allouee */
#define COTE_MAXI   320    /* rend 0, un theme geant rendrait un absurde    */

/* 100 ms : le cadran perd 3,6 degres par image sur un decompte de dix
 * secondes, ce qui suffit largement a paraitre continu. Plus rapide ne se
 * verrait pas et reveillerait le compositeur pour rien -- un module dont
 * l'objet est d'economiser n'a pas le droit d'etre desinvolte la-dessus.
 * Cent images par mise en veille, et rien entre deux. */
#define PAS_MS      100

static struct {
    GtkWidget *fenetre;
    GtkWidget *cadran;
    GtkWidget *reference;   /* la pilule de la barre : donne la largeur     */
    guint      minuterie;
    gint64     debut;      /* horloge monotone, en microsecondes            */
    double     total;      /* duree demandee, en secondes                   */
    double     fraction;   /* 1,0 au depart, 0,0 a l'echeance               */
} P;

static void
dessiner (GtkDrawingArea *aire, cairo_t *cr, int largeur, int hauteur,
          gpointer data)
{
    (void) data;
    double cx = largeur / 2.0, cy = hauteur / 2.0;
    double r  = MIN (largeur, hauteur) / 2.0 - 3.0;

    /* La couleur vient de la feuille de style, propriete « color » de
     * .preavis-cadran. Le theme clair et le theme sombre la posent chacun,
     * et le cadran suit sans qu'une seule teinte soit ecrite ici. */
    GdkRGBA c;
    gtk_widget_get_color (GTK_WIDGET (aire), &c);

    /* La piste : ce que le disque etait au depart. Sans elle, un disque aux
     * trois quarts vide ne dirait pas s'il se vide ou s'il se remplit. */
    /* 0,3 de l'opacite du disque, et non une valeur absolue : le disque
     * etant lui-meme translucide, une piste fixe deviendrait plus marquee
     * que ce qu'elle accompagne. */
    cairo_set_source_rgba (cr, c.red, c.green, c.blue, c.alpha * 0.30);
    cairo_arc (cr, cx, cy, r, 0, 2 * G_PI);
    cairo_fill (cr);

    if (P.fraction <= 0.0)
        return;

    /* Le fromage. Depart en haut et sens horaire : c'est le sens d'une
     * aiguille, donc celui qu'on lit sans y penser. */
    cairo_set_source_rgba (cr, c.red, c.green, c.blue, c.alpha);
    cairo_move_to (cr, cx, cy);
    cairo_arc (cr, cx, cy, r, -G_PI_2, -G_PI_2 + 2 * G_PI * P.fraction);
    cairo_close_path (cr);
    cairo_fill (cr);
}

static gboolean
on_tic (gpointer data)
{
    (void) data;

    /* Le reste se calcule sur l'horloge monotone, pas en comptant les
     * images : une minuterie GLib n'est pas exacte, et dix secondes
     * comptees a 100 ms pres deriveraient visiblement. */
    double ecoule = (g_get_monotonic_time () - P.debut) / 1000000.0;
    P.fraction = (P.total > 0.0) ? 1.0 - ecoule / P.total : 0.0;

    if (P.fraction <= 0.0) {
        P.fraction = 0.0;
        gtk_widget_queue_draw (P.cadran);
        /* On ne masque pas ici : l'etage « attenuer » suit immediatement et
         * appellera shell_preavis_cacher(). Masquer maintenant ferait
         * clignoter l'ecran juste avant qu'il ne baisse. */
        P.minuterie = 0;
        return G_SOURCE_REMOVE;
    }
    gtk_widget_queue_draw (P.cadran);
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
    /* Aucun clavier : le cadran ne doit pas interrompre une frappe. */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (P.fenetre),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_anchor (GTK_WINDOW (P.fenetre), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (P.fenetre), GTK_LAYER_SHELL_EDGE_RIGHT,  TRUE);
    gtk_layer_set_margin (GTK_WINDOW (P.fenetre), GTK_LAYER_SHELL_EDGE_BOTTOM,
                          BANDE_BASSE);
    /* 12 px : la trame du bureau, celle que le dock et la barre gardent
     * deja avec le bord de l'ecran (« margin: 0 12px 12px 0 »). Le cadran
     * s'aligne donc a droite exactement sur la pilule dont il prend la
     * largeur. */
    gtk_layer_set_margin (GTK_WINDOW (P.fenetre), GTK_LAYER_SHELL_EDGE_RIGHT, 12);

    P.cadran = gtk_drawing_area_new ();
    gtk_widget_add_css_class (P.cadran, "preavis-cadran");
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (P.cadran),
                                    dessiner, NULL, NULL);

    gtk_window_set_child (GTK_WINDOW (P.fenetre), P.cadran);
    g_signal_connect (P.fenetre, "realize", G_CALLBACK (on_realise), NULL);
}

void
shell_preavis_reference (GtkWidget *pilule)
{
    P.reference = pilule;
}

/* La largeur est relue A CHAQUE AFFICHAGE, pas retenue : la pilule change
 * de largeur quand l'heure passe de « 9:05 » a « 11:44 », et un cadran
 * fige finirait par ne plus s'aligner sur rien. */
static int
cote (void)
{
    int c = COTE_REPLI;
    if (P.reference != NULL) {
        int l = gtk_widget_get_width (P.reference);
        if (l > 0)
            c = l;
    }
    return CLAMP (c, COTE_MINI, COTE_MAXI);
}

void
shell_preavis_montrer (int secondes)
{
    if (secondes <= 0)
        return;

    construire ();
    int d = cote ();
    gtk_widget_set_size_request (P.cadran, d, d);
    P.total    = secondes;
    P.debut    = g_get_monotonic_time ();
    P.fraction = 1.0;
    gtk_widget_queue_draw (P.cadran);

    if (P.minuterie != 0)
        g_source_remove (P.minuterie);
    P.minuterie = g_timeout_add (PAS_MS, on_tic, NULL);

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
