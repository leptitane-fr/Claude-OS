#include "glissiere.h"

#include <math.h>

/* Duree d'un trajet complet. Assez court pour ne jamais faire attendre --
 * la touche Loupe doit sembler repondre tout de suite --, assez long pour
 * que l'oeil voie d'ou vient la barre et ou elle va. Un demi-trajet, quand
 * on inverse le mouvement en route, prend la moitie du temps. */
#define DUREE_MS 220

/* Au-dela du contenu : l'ombre portee de la pilule deborde vers le haut
 * (28 px de flou, 8 de decalage -- voir .dock dans shell.css). Sans cette
 * marge, une frange d'ombre resterait visible au ras de l'ecran pendant les
 * dernieres images. */
#define DEPASSEMENT_PX 32

struct _ShellGlissiere {
    GtkWidget  parent_instance;

    GtkWidget *enfant;

    double     position;     /* 0 : en place ; 1 : sorti par le bas          */
    double     depart;
    double     arrivee;
    gint64     t0;           /* premiere image du mouvement, en µs ; 0 = a poser */
    gint64     duree_us;
    guint      tick;

    ShellGlissiereFin        fin;
    gpointer                 fin_data;
    ShellGlissiereAllocation alloc;
    gpointer                 alloc_data;
};

G_DEFINE_FINAL_TYPE (ShellGlissiere, shell_glissiere, GTK_TYPE_WIDGET)

static GtkWidget *
fenetre_de (ShellGlissiere *g)
{
    GtkRoot *racine = gtk_widget_get_root (GTK_WIDGET (g));
    return GTK_IS_WINDOW (racine) ? GTK_WIDGET (racine) : NULL;
}

/* -------------------------------------------------------------------------
 * Mise en page : la taille de l'enfant, decalee vers le bas
 * ------------------------------------------------------------------------- */
static GtkSizeRequestMode
glissiere_request_mode (GtkWidget *w)
{
    ShellGlissiere *g = SHELL_GLISSIERE (w);
    return g->enfant ? gtk_widget_get_request_mode (g->enfant)
                     : GTK_SIZE_REQUEST_CONSTANT_SIZE;
}

static void
glissiere_measure (GtkWidget *w, GtkOrientation o, int pour,
                   int *min, int *nat, int *min_base, int *nat_base)
{
    ShellGlissiere *g = SHELL_GLISSIERE (w);
    if (g->enfant == NULL) {
        *min = *nat = 0;
        return;
    }
    gtk_widget_measure (g->enfant, o, pour, min, nat, min_base, nat_base);
}

static void
glissiere_size_allocate (GtkWidget *w, int largeur, int hauteur, int base)
{
    ShellGlissiere *g = SHELL_GLISSIERE (w);
    if (g->enfant == NULL)
        return;

    /* LA DISTANCE EST CELLE DU CONTENU, PAS CELLE DE LA FENETRE.
     *
     * Le dock tend sa fenetre a tout l'ecran quand on le rappelle par-dessus
     * une application (voir dock.c, « la nappe »). Descendre de la hauteur
     * de la fenetre ferait alors parcourir 1200 px en 220 ms a une pilule
     * qui n'en montre que 86 : elle tomberait comme une pierre. La hauteur
     * naturelle du contenu, marges comprises, donne le meme trajet dans les
     * deux cas. */
    GskTransform *t = NULL;
    if (g->position > 0.0) {
        int nat = 0;
        gtk_widget_measure (g->enfant, GTK_ORIENTATION_VERTICAL, largeur,
                            NULL, &nat, NULL, NULL);
        float dy = (float) (g->position * (MIN (nat, hauteur) + DEPASSEMENT_PX));
        t = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0.f, dy));
    }
    gtk_widget_allocate (g->enfant, largeur, hauteur, base, t);

    if (g->alloc != NULL)
        g->alloc (largeur, hauteur, g->alloc_data);
}

/* -------------------------------------------------------------------------
 * Le mouvement
 * ------------------------------------------------------------------------- */
static void
terminer (ShellGlissiere *g)
{
    gboolean visible = (g->arrivee < 1.0);

    /* Descendu : la fenetre s'en va pour de bon. Voir l'en-tete -- une
     * surface vide qui reste est une surface qui avale des clics. */
    if (!visible) {
        GtkWidget *fen = fenetre_de (g);
        if (fen != NULL)
            gtk_widget_set_visible (fen, FALSE);
    }
    if (g->fin != NULL)
        g->fin (visible, g->fin_data);
}

/* Cubique, ralentie a l'arrivee quand on monte, acceleree au depart quand on
 * descend : ce qui arrive se pose, ce qui s'en va s'eloigne. */
static double
adoucir (double t, gboolean monte)
{
    if (monte) {
        double u = 1.0 - t;
        return 1.0 - u * u * u;
    }
    return t * t * t;
}

static gboolean
on_tick (GtkWidget *w, GdkFrameClock *horloge, gpointer data)
{
    ShellGlissiere *g = SHELL_GLISSIERE (w);
    (void) data;

    gint64 maintenant = gdk_frame_clock_get_frame_time (horloge);
    if (g->t0 == 0)
        g->t0 = maintenant;

    double t = (double) (maintenant - g->t0) / (double) g->duree_us;
    if (t > 1.0)
        t = 1.0;

    g->position = g->depart
                + (g->arrivee - g->depart) * adoucir (t, g->arrivee < g->depart);
    gtk_widget_queue_allocate (w);

    if (t < 1.0)
        return G_SOURCE_CONTINUE;

    g->position = g->arrivee;
    g->tick = 0;              /* avant terminer() : il peut demasquer, relancer */
    terminer (g);
    return G_SOURCE_REMOVE;
}

static gboolean
animations_permises (ShellGlissiere *g)
{
    gboolean oui = TRUE;
    g_object_get (gtk_widget_get_settings (GTK_WIDGET (g)),
                  "gtk-enable-animations", &oui, NULL);
    return oui;
}

/* L'horloge n'est armee que sur un widget AFFICHE.
 *
 * gtk_widget_add_tick_callback() sur un widget qui ne l'est pas encore rend
 * un identifiant valide et n'arme rien : le rappel passe une fois, puis plus
 * jamais, sans erreur. Paye sur le lecteur video le 10 septembre 2026. Si le
 * widget n'est pas encore affiche, c'est map() qui armera. */
static void
armer (ShellGlissiere *g)
{
    if (g->tick == 0 && gtk_widget_get_mapped (GTK_WIDGET (g)))
        g->tick = gtk_widget_add_tick_callback (GTK_WIDGET (g), on_tick, NULL, NULL);
}

static void
lancer (ShellGlissiere *g, double arrivee)
{
    g->depart   = g->position;
    g->arrivee  = arrivee;
    g->t0       = 0;
    g->duree_us = (gint64) (fabs (arrivee - g->depart) * DUREE_MS * 1000);

    if (g->duree_us <= 0 || !animations_permises (g)) {
        if (g->tick != 0) {
            gtk_widget_remove_tick_callback (GTK_WIDGET (g), g->tick);
            g->tick = 0;
        }
        g->position = arrivee;
        gtk_widget_queue_allocate (GTK_WIDGET (g));
        terminer (g);
        return;
    }
    armer (g);
}

void
shell_glissiere_montrer (ShellGlissiere *g)
{
    GtkWidget *fen = fenetre_de (g);
    if (fen != NULL && !gtk_widget_get_visible (fen)) {
        /* Remise hors champ : la premiere image composee montre la surface
         * vide, et le contenu entre par le bas a partir de la suivante. */
        g->position = 1.0;
        gtk_widget_set_visible (fen, TRUE);
    }
    lancer (g, 0.0);
}

void
shell_glissiere_cacher (ShellGlissiere *g)
{
    GtkWidget *fen = fenetre_de (g);
    if (fen == NULL || !gtk_widget_get_visible (fen)) {
        g->position = 1.0;
        return;
    }
    lancer (g, 1.0);
}

gboolean
shell_glissiere_en_place (ShellGlissiere *g)
{
    GtkWidget *fen = fenetre_de (g);
    return fen != NULL && gtk_widget_get_visible (fen)
        && g->tick == 0 && g->position == 0.0;
}

void
shell_glissiere_sur_fin (ShellGlissiere *g, ShellGlissiereFin f, gpointer data)
{
    g->fin      = f;
    g->fin_data = data;
}

void
shell_glissiere_sur_allocation (ShellGlissiere *g, ShellGlissiereAllocation f,
                                gpointer data)
{
    g->alloc      = f;
    g->alloc_data = data;
}

/* -------------------------------------------------------------------------
 * Cycle de vie
 * ------------------------------------------------------------------------- */
static void
glissiere_map (GtkWidget *w)
{
    ShellGlissiere *g = SHELL_GLISSIERE (w);
    GTK_WIDGET_CLASS (shell_glissiere_parent_class)->map (w);

    /* Un mouvement demande avant que la fenetre ne soit affichee. */
    if (g->position != g->arrivee)
        armer (g);
}

static void
glissiere_unmap (GtkWidget *w)
{
    ShellGlissiere *g = SHELL_GLISSIERE (w);
    if (g->tick != 0) {
        gtk_widget_remove_tick_callback (w, g->tick);
        g->tick = 0;
    }
    GTK_WIDGET_CLASS (shell_glissiere_parent_class)->unmap (w);
}

static void
glissiere_dispose (GObject *o)
{
    ShellGlissiere *g = SHELL_GLISSIERE (o);
    g_clear_pointer (&g->enfant, gtk_widget_unparent);
    G_OBJECT_CLASS (shell_glissiere_parent_class)->dispose (o);
}

static void
shell_glissiere_class_init (ShellGlissiereClass *klass)
{
    GObjectClass   *oc = G_OBJECT_CLASS (klass);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS (klass);

    oc->dispose          = glissiere_dispose;
    wc->get_request_mode = glissiere_request_mode;
    wc->measure          = glissiere_measure;
    wc->size_allocate    = glissiere_size_allocate;
    wc->map              = glissiere_map;
    wc->unmap            = glissiere_unmap;
}

static void
shell_glissiere_init (ShellGlissiere *g)
{
    g->position = 0.0;
    g->arrivee  = 0.0;
}

GtkWidget *
shell_glissiere_new (GtkWidget *enfant)
{
    ShellGlissiere *g = g_object_new (SHELL_TYPE_GLISSIERE, NULL);
    g->enfant = enfant;
    gtk_widget_set_parent (enfant, GTK_WIDGET (g));
    return GTK_WIDGET (g);
}
