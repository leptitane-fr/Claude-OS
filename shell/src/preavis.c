#include "preavis.h"

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <math.h>            /* cos, sin, ceil */

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

/* SOIXANTE GRADUATIONS, comme un vrai cadran de chronometre.
 *
 * Douze faisaient une etoile, pas un chronometre : l'ecart entre deux
 * traits etait trop grand pour qu'on y lise une echelle. Soixante donnent
 * la trame qu'on reconnait sans la compter.
 *
 * TROIS LONGUEURS, celles d'un cadran horloger, et rien d'ecrit :
 *   les quarts   -- 12, 3, 6, 9        -- les plus longs et les plus epais
 *   les cinq     -- 5, 10, 20, 25...   -- intermediaires
 *   les minutes  -- tout le reste      -- courts et fins
 *
 * Avec dix secondes de preavis, un trait s'eteint toutes les 167 ms : ce
 * n'est plus une disparition, c'est un balayage -- exactement le geste
 * d'une trotteuse. */
#define RAYONS      60

/* Proportions du soleil, en fraction du rayon total. Le disque central ne
 * bouge JAMAIS : c'est lui qui dit « lumiere », et une lumiere qui se
 * retracte donnerait le message inverse de celle qui s'eteint d'un coup. */
#define DISQUE      0.42
#define RAYON_FIN   0.96      /* les traits finissent tous au meme rayon   */
#define DEB_QUART   0.70      /* 12, 3, 6, 9                               */
#define DEB_CINQ    0.79
#define DEB_MINUTE  0.87
#define TRAIT_QUART  0.048
#define TRAIT_CINQ   0.036
#define TRAIT_MINUTE 0.022

static struct {
    GtkWidget *fenetre;
    GtkWidget *cadran;
    GtkWidget *reference;   /* la pilule de la barre : donne la largeur     */
    GtkCssProvider *style;  /* opacite engendree -- voir opacite_appliquer  */
    int        opacite;     /* pourcent ; 0 = pas encore regle              */
    guint      minuterie;
    gint64     debut;      /* horloge monotone, en microsecondes            */
    double     total;      /* duree demandee, en secondes                   */
    double     fraction;   /* 1,0 au depart, 0,0 a l'echeance               */
} P;

static void opacite_appliquer (void);

static void
dessiner (GtkDrawingArea *aire, cairo_t *cr, int largeur, int hauteur,
          gpointer data)
{
    (void) data;
    double cx = largeur / 2.0, cy = hauteur / 2.0;

    /* AUCUNE MARGE INTERIEURE, ET C'EST LE POINT.
     *
     * Le cadran partage son bord droit avec la pilule de la barre d'etat et
     * doit faire exactement sa largeur. Or GTK donne ici la zone de CONTENU,
     * bordure CSS deja deduite : une encoche de 3 px dans le trace rendait
     * un disque de 164 px pour une allocation de 172, ceint d'un anneau
     * clair -- visiblement plus etroit que la barre. */
    double R = MIN (largeur, hauteur) / 2.0;

    /* La couleur ET son alpha viennent de la feuille de style, propriete
     * « color » de .preavis-cadran, que preavis_opacite() reecrit. */
    GdkRGBA c;
    gtk_widget_get_color (GTK_WIDGET (aire), &c);

    /* Le soleil : un disque plein, immobile. */
    cairo_set_source_rgba (cr, c.red, c.green, c.blue, c.alpha);
    cairo_arc (cr, cx, cy, R * DISQUE, 0, 2 * G_PI);
    cairo_fill (cr);

    /* Les graduations : rayons de soleil et cadran de chronometre a la
     * fois. Elles s'eteignent une a une, dans le sens horaire depuis midi --
     * celui d'une aiguille, donc celui qu'on lit sans y penser.
     *
     * Une graduation eteinte n'est pas effacee : il en reste une trace tres
     * faible. Sans elle, un cadran a deux traits ne dirait pas s'il en a
     * perdu cinquante-huit ou s'il n'en a jamais eu que deux. */
    int restants = (int) ceil (P.fraction * RAYONS);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);

    for (int i = 0; i < RAYONS; i++) {
        double deb, epaisseur;
        if (i % 15 == 0)      { deb = DEB_QUART;  epaisseur = TRAIT_QUART;  }
        else if (i % 5 == 0)  { deb = DEB_CINQ;   epaisseur = TRAIT_CINQ;   }
        else                  { deb = DEB_MINUTE; epaisseur = TRAIT_MINUTE; }

        double a  = -G_PI_2 + (2 * G_PI * i) / RAYONS;
        double ca = cos (a), sa = sin (a);

        cairo_set_line_width (cr, R * epaisseur);
        cairo_set_source_rgba (cr, c.red, c.green, c.blue,
                               (i < restants) ? c.alpha : c.alpha * 0.16);
        cairo_move_to (cr, cx + ca * R * deb,       cy + sa * R * deb);
        cairo_line_to (cr, cx + ca * R * RAYON_FIN, cy + sa * R * RAYON_FIN);
        cairo_stroke (cr);
    }
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
    opacite_appliquer ();
}

void
shell_preavis_reference (GtkWidget *pilule)
{
    P.reference = pilule;
}

/* L'OPACITE EST ENGENDREE, LES TEINTES NE LE SONT PAS.
 *
 * On reecrit « alpha(...) » autour des couleurs du theme plutot que des
 * valeurs RVB : le cadran suit donc les themes clair et sombre comme
 * avant, et seul son degre de presence change.
 *
 * PLUS AUCUN FOND. Le disque translucide qui portait le cadran en faisait
 * une pastille posee sur le bureau ; on ne veut que le soleil et ses
 * graduations, flottant sur ce qui se trouve dessous. La fenetre est donc
 * entierement transparente, et seul le trace se voit. */
static void
opacite_appliquer (void)
{
    if (P.opacite <= 0)
        return;

    double a = CLAMP (P.opacite, 5, 100) / 100.0;

    /* g_ascii_formatd ET NON %.3f : en français, printf écrit « 0,850 », et
     * la virgule coupe alpha() en deux arguments. GTK rejette alors la
     * règle — « Expected ')' at end of alpha() » dans shell.log — et le
     * cadran perd sa couleur sans que rien ne s'arrête. Vu le 13 septembre
     * 2026 ; c'est le piège du commit 2d7f52f, qui ne force le point
     * décimal que pour les nombres lus, pas pour ceux qu'on écrit. */
    char nombre[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd (nombre, sizeof nombre, "%.3f", a);
    g_autofree char *css = g_strdup_printf (
        ".preavis-cadran { color: alpha(@accent, %s); }", nombre);

    if (P.style == NULL) {
        P.style = gtk_css_provider_new ();
        gtk_style_context_add_provider_for_display (
            gdk_display_get_default (), GTK_STYLE_PROVIDER (P.style),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    gtk_css_provider_load_from_string (P.style, css);
}

void
shell_preavis_opacite (int pourcent)
{
    if (pourcent == P.opacite)
        return;
    P.opacite = pourcent;
    opacite_appliquer ();
    if (P.cadran != NULL)
        gtk_widget_queue_draw (P.cadran);
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
    /* Le cadran partage son bord droit avec la pilule de la barre : un
     * ecart de quelques pixels se voit immediatement. Tracer le diametre
     * demande evite d'avoir a le deduire d'une capture d'ecran. */
    g_message ("preavis : cadran de %d px (reference %d px)", d,
               P.reference != NULL ? gtk_widget_get_width (P.reference) : -1);
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
