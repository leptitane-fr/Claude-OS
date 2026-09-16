#include "retourneur.h"

#include <math.h>

/* Durée d'un retournement complet.
 *
 * Plus long que la glissière (220 ms) : celle-ci fait disparaître, celui-ci
 * fait COMPRENDRE. L'œil doit voir que c'est le même objet qui tourne, et
 * une bascule trop vive se lit comme un remplacement brutal. Un demi-tour,
 * quand on inverse en route, prend la moitié du temps. */
#define DUREE_MS 260

/* La distance de l'œil, en pixels. C'est un réglage de GOÛT, pas une
 * mesure : très grande, la rotation devient un simple écrasement vertical
 * sans profondeur ; très petite, le bord qui s'approche enfle et le dock a
 * l'air de tomber sur l'utilisateur. 400 px donne à une pilule de 86 px de
 * haut une fuite d'environ un dixième -- assez pour qu'on voie un volume,
 * assez peu pour que rien ne se déforme. */
#define PROFONDEUR 400.f

/* LA DURÉE EST RÉGLABLE PAR L'ENVIRONNEMENT, et uniquement pour le banc.
 *
 * Un retournement dure moins d'un tiers de seconde, et « grim » met plus
 * longtemps que cela à produire une capture : sans ce réglage, aucune image
 * du mouvement ne peut être photographiée, et la justesse de la perspective
 * ne se vérifierait que de visu sur la machine. Le 16 septembre 2026, c'est
 * par là qu'on a établi que le rendu logiciel de GTK ne sait PAS dessiner
 * une transformation 3D -- il peint la face en rose vif.
 *
 * Hors banc, la variable n'existe pas et la constante s'applique. */
static gint64
duree_ms (void)
{
    const char *r = g_getenv ("CLAUDE_OS_RETOURNEUR_MS");
    if (r == NULL)
        return DUREE_MS;

    gint64 v = g_ascii_strtoll (r, NULL, 10);
    return (v >= 20 && v <= 10000) ? v : DUREE_MS;
}

struct _ShellRetourneur {
    GtkWidget  parent_instance;

    GtkWidget *avant;
    GtkWidget *arriere;

    double     position;     /* 0 : avant ; 1 : arrière                     */
    double     depart;
    double     arrivee;
    gint64     t0;           /* première image du mouvement, en µs          */
    gint64     duree_us;
    guint      tick;

    ShellRetourneurDepart     debut;
    gpointer                  debut_data;
    ShellRetourneurFin        fin;
    gpointer                  fin_data;
    ShellRetourneurAllocation alloc;
    gpointer                  alloc_data;

    /* 0 : pas encore demandé ; 1 : oui ; -1 : non. Voir sait_la_3d(). */
    int        trois_d;
};

G_DEFINE_FINAL_TYPE (ShellRetourneur, shell_retourneur, GTK_TYPE_WIDGET)

/* -------------------------------------------------------------------------
 * Mise en page
 *
 * TOUJOURS AU PLUS LARGE DES DEUX FACES, et c'est la parade principale à la
 * limite de labwc rappelée dans l'en-tête : la surface ne change pas de
 * taille pendant le retournement, puisqu'elle a déjà celle de la face la
 * plus encombrante.
 * ------------------------------------------------------------------------- */
static void
retourneur_measure (GtkWidget *w, GtkOrientation o, int pour,
                    int *min, int *nat, int *min_base, int *nat_base)
{
    ShellRetourneur *r = SHELL_RETOURNEUR (w);
    int amin = 0, anat = 0, bmin = 0, bnat = 0;

    if (r->avant != NULL)
        gtk_widget_measure (r->avant, o, pour, &amin, &anat, NULL, NULL);
    if (r->arriere != NULL)
        gtk_widget_measure (r->arriere, o, pour, &bmin, &bnat, NULL, NULL);

    *min = MAX (amin, bmin);
    *nat = MAX (anat, bnat);

    /* Aucune ligne de base : deux faces qui tournent l'une dans l'autre
     * n'ont pas de texte à aligner sur quoi que ce soit d'extérieur. */
    if (min_base != NULL) *min_base = -1;
    if (nat_base != NULL) *nat_base = -1;
}

/* LE RENDU LOGICIEL NE SAIT PAS DESSINER LA 3D, ET IL LE DIT EN ROSE.
 *
 * Mesuré au banc le 16 septembre 2026 : sous GSK_RENDERER=cairo, une face
 * portant gsk_transform_perspective() est peinte en ROSE VIF -- la couleur
 * dont GSK marque un nœud qu'il ne sait pas rendre. Avec « ngl », la même
 * face tourne correctement, verticales convergentes comprises.
 *
 * MADOO utilise ngl. Mais un repli logiciel est toujours possible -- pilote
 * en panne, machine virtuelle, banc d'essai -- et un dock qui vire au rose à
 * chaque bascule serait un désastre visible. La rotation est donc remplacée,
 * dans ce cas, par un ÉCRASEMENT VERTICAL : l'objet se referme puis se
 * rouvre, ce qui se lit de la même façon, et cairo sait le faire.
 *
 * La question est posée une seule fois par widget : le renderer d'une
 * surface ne change pas en cours de route. */
static gboolean
sait_la_3d (GtkWidget *w)
{
    ShellRetourneur *r = SHELL_RETOURNEUR (w);

    if (r->trois_d == 0) {
        GtkNative *natif = gtk_widget_get_native (w);
        GskRenderer *rendu = natif ? gtk_native_get_renderer (natif) : NULL;

        if (rendu == NULL)
            return TRUE;      /* pas encore réalisé : on ne décide rien */

        const char *nom = G_OBJECT_TYPE_NAME (rendu);
        r->trois_d = g_str_equal (nom, "GskCairoRenderer") ? -1 : 1;
        if (r->trois_d < 0)
            g_message ("retourneur : %s ne sait pas la 3D, bascule par "
                       "écrasement", nom);
    }
    return r->trois_d > 0;
}

/* La transformation d'une face : on l'amène à sa place, puis on la fait
 * tourner autour de son propre centre.
 *
 * L'ordre compte, et il se lit de l'extérieur vers l'intérieur : placer,
 * aller au centre, poser la perspective, tourner, revenir du centre. Une
 * perspective posée avant le centrage ferait fuir la face vers le coin haut
 * gauche de la fenêtre au lieu de tourner sur elle-même. */
static GskTransform *
transformation (int x, int y, int largeur, int hauteur, float angle,
                gboolean trois_d)
{
    GskTransform *t = gsk_transform_translate (
        NULL, &GRAPHENE_POINT_INIT ((float) x, (float) y));

    if (angle == 0.f)
        return t;

    t = gsk_transform_translate (
        t, &GRAPHENE_POINT_INIT (largeur / 2.f, hauteur / 2.f));

    if (trois_d) {
        t = gsk_transform_perspective (t, PROFONDEUR);
        t = gsk_transform_rotate_3d (t, angle, graphene_vec3_x_axis ());
    } else {
        /* Le repli : la hauteur suit le cosinus de l'angle, exactement comme
         * la projection d'un plan qui tourne. C'est la rotation dont on a
         * retiré la profondeur, pas une autre animation. */
        float k = fabsf (cosf (angle * (float) G_PI / 180.f));
        t = gsk_transform_scale (t, 1.f, MAX (k, 0.01f));
    }

    t = gsk_transform_translate (
        t, &GRAPHENE_POINT_INIT (-largeur / 2.f, -hauteur / 2.f));
    return t;
}

/* Une face, centrée dans la place disponible, à sa taille naturelle.
 *
 * À SA TAILLE NATURELLE, ET PAS À CELLE DE LA FENÊTRE : la fenêtre est
 * taillée au plus large des deux, et étirer la face étroite jusque-là
 * étalerait sa pilule sur toute la largeur -- ce qu'on veut précisément
 * éviter, puisque c'est la pilule qui doit s'élargir au retournement, pas la
 * surface. */
static void
placer (ShellRetourneur *r, GtkWidget *face, int largeur, int hauteur,
        float angle, gboolean visible)
{
    if (face == NULL)
        return;

    gtk_widget_set_child_visible (face, visible);

    int fw = 0, fh = 0;
    gtk_widget_measure (face, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &fw, NULL, NULL);
    gtk_widget_measure (face, GTK_ORIENTATION_VERTICAL, fw, NULL, &fh, NULL, NULL);
    fw = MIN (fw, largeur);
    fh = MIN (fh, hauteur);

    int x = (largeur - fw) / 2;

    /* LA FACE SUIT SON PROPRE ALIGNEMENT VERTICAL, ET C'EST UN BUG PAYE.
     *
     * Centrer valait tant que la fenetre avait la taille de son contenu.
     * Mais le dock TEND SA FENETRE A TOUT L'ECRAN quand il est convoque
     * par-dessus une application, ou quand l'auvent est ouvert -- et la
     * pilule, centree dans 1080 px, partait au milieu de l'ecran. Vu au banc
     * le 16 septembre 2026, sur deux captures d'auvent ou le dock avait tout
     * simplement disparu du bas.
     *
     * Le dock et l'etabli s'alignent en bas ; un autre porteur de faces
     * pourrait vouloir autre chose, et c'est lui qui le dit. */
    int y;
    switch (gtk_widget_get_valign (face)) {
    case GTK_ALIGN_START:  y = 0; break;
    case GTK_ALIGN_CENTER: y = (hauteur - fh) / 2; break;
    default:               y = hauteur - fh; break;   /* END, FILL, BASELINE */
    }

    gtk_widget_allocate (face, fw, fh, -1,
                         transformation (x, y, fw, fh, angle,
                                         sait_la_3d (GTK_WIDGET (r))));

    if (visible && r->alloc != NULL)
        r->alloc (x, y, fw, fh, r->alloc_data);
}

static void
retourneur_size_allocate (GtkWidget *w, int largeur, int hauteur, int base)
{
    ShellRetourneur *r = SHELL_RETOURNEUR (w);
    (void) base;

    float angle = (float) (r->position * 180.0);

    /* Passé le quart de tour, c'est l'arrière qu'on voit -- et il faut le
     * remettre à l'endroit : sans le retrait de 180°, il apparaîtrait
     * retourné, lisible dans un miroir. */
    gboolean cote_avant = (angle <= 90.f);

    placer (r, r->avant,   largeur, hauteur, angle,          cote_avant);
    placer (r, r->arriere, largeur, hauteur, angle - 180.f, !cote_avant);
}

/* -------------------------------------------------------------------------
 * Le mouvement
 * ------------------------------------------------------------------------- */

/* Cubique des deux côtés : un retournement est un aller symétrique, il part
 * doucement et se pose doucement. La glissière, elle, est dissymétrique --
 * ce qui arrive se pose, ce qui s'en va s'éloigne -- parce qu'elle n'a pas
 * le même sens dans les deux sens. */
static double
adoucir (double t)
{
    if (t < 0.5)
        return 4.0 * t * t * t;
    double u = -2.0 * t + 2.0;
    return 1.0 - u * u * u / 2.0;
}

static void
terminer (ShellRetourneur *r)
{
    if (r->fin != NULL)
        r->fin (r->arrivee >= 1.0, r->fin_data);
}

static gboolean
on_tick (GtkWidget *w, GdkFrameClock *horloge, gpointer data)
{
    ShellRetourneur *r = SHELL_RETOURNEUR (w);
    (void) data;

    gint64 maintenant = gdk_frame_clock_get_frame_time (horloge);
    if (r->t0 == 0)
        r->t0 = maintenant;

    double t = (double) (maintenant - r->t0) / (double) r->duree_us;
    if (t > 1.0)
        t = 1.0;

    r->position = r->depart + (r->arrivee - r->depart) * adoucir (t);
    gtk_widget_queue_allocate (w);

    if (t < 1.0)
        return G_SOURCE_CONTINUE;

    r->position = r->arrivee;
    r->tick = 0;              /* avant terminer() : il peut relancer */
    terminer (r);
    return G_SOURCE_REMOVE;
}

static gboolean
animations_permises (ShellRetourneur *r)
{
    gboolean oui = TRUE;
    g_object_get (gtk_widget_get_settings (GTK_WIDGET (r)),
                  "gtk-enable-animations", &oui, NULL);
    return oui;
}

/* L'horloge n'est armée que sur un widget AFFICHÉ.
 *
 * gtk_widget_add_tick_callback() sur un widget qui ne l'est pas encore rend
 * un identifiant valide et n'arme rien : le rappel passe une fois, puis plus
 * jamais, sans erreur. Payé sur le lecteur vidéo le 10 septembre 2026. Si le
 * widget n'est pas encore affiché, c'est map() qui armera. */
static void
armer (ShellRetourneur *r)
{
    if (r->tick == 0 && gtk_widget_get_mapped (GTK_WIDGET (r)))
        r->tick = gtk_widget_add_tick_callback (GTK_WIDGET (r), on_tick, NULL, NULL);
}

void
shell_retourneur_montrer (ShellRetourneur *r, gboolean arriere)
{
    double but = arriere ? 1.0 : 0.0;

    if (r->arrivee == but && r->tick == 0 && r->position == but)
        return;                    /* déjà là, et immobile */
    if (r->arrivee == but && r->tick != 0)
        return;                    /* déjà en route vers cette face */

    /* AVANT TOUT MOUVEMENT, ET UNE SEULE FOIS. C'est ici que le dock ferme
     * ses popovers : une liste ouverte au survol, laissée en place pendant
     * que sa face tourne, resterait plantée au milieu de l'écran. */
    if (r->debut != NULL)
        r->debut (r->debut_data);

    r->depart   = r->position;
    r->arrivee  = but;
    r->t0       = 0;
    r->duree_us = (gint64) (fabs (but - r->depart) * (double) duree_ms () * 1000.0);

    if (r->duree_us <= 0 || !animations_permises (r)) {
        if (r->tick != 0) {
            gtk_widget_remove_tick_callback (GTK_WIDGET (r), r->tick);
            r->tick = 0;
        }
        r->position = but;
        gtk_widget_queue_allocate (GTK_WIDGET (r));
        terminer (r);
        return;
    }
    armer (r);
}

gboolean
shell_retourneur_face (ShellRetourneur *r)
{
    return r->arrivee >= 1.0;
}

gboolean
shell_retourneur_en_place (ShellRetourneur *r)
{
    return r->tick == 0 && (r->position == 0.0 || r->position == 1.0);
}

void
shell_retourneur_sur_depart (ShellRetourneur *r, ShellRetourneurDepart f,
                             gpointer data)
{
    r->debut      = f;
    r->debut_data = data;
}

void
shell_retourneur_sur_fin (ShellRetourneur *r, ShellRetourneurFin f, gpointer data)
{
    r->fin      = f;
    r->fin_data = data;
}

void
shell_retourneur_sur_allocation (ShellRetourneur *r, ShellRetourneurAllocation f,
                                 gpointer data)
{
    r->alloc      = f;
    r->alloc_data = data;
}

/* -------------------------------------------------------------------------
 * Cycle de vie
 * ------------------------------------------------------------------------- */
static void
retourneur_map (GtkWidget *w)
{
    ShellRetourneur *r = SHELL_RETOURNEUR (w);
    GTK_WIDGET_CLASS (shell_retourneur_parent_class)->map (w);

    /* Un retournement demandé avant que la fenêtre ne soit affichée. */
    if (r->position != r->arrivee)
        armer (r);
}

static void
retourneur_unmap (GtkWidget *w)
{
    ShellRetourneur *r = SHELL_RETOURNEUR (w);
    if (r->tick != 0) {
        gtk_widget_remove_tick_callback (w, r->tick);
        r->tick = 0;
    }
    GTK_WIDGET_CLASS (shell_retourneur_parent_class)->unmap (w);
}

static void
retourneur_dispose (GObject *o)
{
    ShellRetourneur *r = SHELL_RETOURNEUR (o);
    g_clear_pointer (&r->avant, gtk_widget_unparent);
    g_clear_pointer (&r->arriere, gtk_widget_unparent);
    G_OBJECT_CLASS (shell_retourneur_parent_class)->dispose (o);
}

static void
shell_retourneur_class_init (ShellRetourneurClass *klass)
{
    GObjectClass   *oc = G_OBJECT_CLASS (klass);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS (klass);

    oc->dispose       = retourneur_dispose;
    wc->measure       = retourneur_measure;
    wc->size_allocate = retourneur_size_allocate;
    wc->map           = retourneur_map;
    wc->unmap         = retourneur_unmap;
}

static void
shell_retourneur_init (ShellRetourneur *r)
{
    r->position = 0.0;
    r->arrivee  = 0.0;
}

GtkWidget *
shell_retourneur_new (GtkWidget *avant, GtkWidget *arriere)
{
    ShellRetourneur *r = g_object_new (SHELL_TYPE_RETOURNEUR, NULL);

    r->avant   = avant;
    r->arriere = arriere;
    gtk_widget_set_parent (avant, GTK_WIDGET (r));
    gtk_widget_set_parent (arriere, GTK_WIDGET (r));

    /* L'arrière est caché dès le départ : sans cela, il serait dessiné
     * par-dessus l'avant jusqu'à la première allocation. */
    gtk_widget_set_child_visible (arriere, FALSE);
    return GTK_WIDGET (r);
}
