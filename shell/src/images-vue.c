/* =========================================================================
 * Claude-OS — Images : la toile
 *
 * Voir images-vue.h pour ce qu'elle fait et pourquoi elle existe. Ici, le
 * comment : trois gestes qui se partagent le même doigt, et une géométrie
 * tenue en pixels de l'image d'origine.
 *
 * QUI DÉCIDE QUOI, SOUS LE DOIGT
 *
 *   image ajustée   un doigt fait GLISSER : l'image suit, sa voisine entre
 *                   par le bord, et c'est au lever qu'on décide.
 *   image agrandie  un doigt DÉPLACE l'image dans la fenêtre.
 *   deux doigts     ZOOMENT autour de leur milieu, et déplacent en même
 *                   temps — le point pincé reste sous les doigts.
 *
 * La souris fait exactement la même chose que le doigt : glisser à la souris
 * est moins courant, mais ne coûte rien et se découvre tout seul.
 * ========================================================================= */

#include "images-vue.h"

#include <math.h>
#include <string.h>

/* Espace entre deux images pendant le glissement. Sans lui, les bords se
 * touchent, et deux photos sombres n'en font plus qu'une. */
#define ECART             32.0

/* Un balayage aboutit s'il a parcouru ce cinquième de largeur, OU s'il a été
 * lancé assez vite : un geste bref et franc doit suffire, comme sur un
 * téléphone. Mais la vitesse seule ne décide pas en deçà de quelques
 * pixels — sans ce plancher, le tremblement d'un appui prolongé passerait
 * pour un geste. */
#define SEUIL_DISTANCE    0.20
#define SEUIL_VITESSE     650.0     /* pixels logiques par seconde */
#define DISTANCE_MINIMALE 24.0

/* Seul compte le mouvement des derniers instants : un doigt qui s'est arrêté
 * avant d'être levé n'a rien lancé. */
#define FENETRE_VITESSE   100000    /* µs */

#define ZOOM_MAX          16.0
#define PAS_MOLETTE       1.15

/* Tourner à deux doigts ne démarre qu'au-delà de ce seuil. Un pincement pur
 * tourne toujours un peu — les doigts ne s'écartent jamais en ligne droite —,
 * et une photo qui vacille pendant qu'on zoome paraît instable. L'angle est
 * compté À PARTIR du seuil : pas de saut quand la rotation s'enclenche. */
#define ROTATION_MORTE    12.0      /* degrés */

/* Sous l'ajustement, pendant le geste seulement : l'image rétrécit encore un
 * peu sous les doigts, puis revient. Un zoom qui bute net contre un mur ne
 * dit pas qu'il est au bout ; un zoom qui cède et revient, si. */
#define ZOOM_ELASTIQUE    0.6

#define DUREE_RETOUR      220000    /* µs */

/* Délai de la reconnaissance d'un double appui. Un appui simple n'est annoncé
 * qu'après lui : sinon le premier appui d'un double montrerait les
 * commandes, que le second ferait aussitôt disparaître sous un zoom. */
#define DELAI_DOUBLE      260       /* ms */

typedef struct {
    GdkTexture *texture;
    int         largeur, hauteur;   /* image d'origine, orientation appliquée */
    gboolean    existe;             /* voisines seulement                     */
} Diapo;

struct _ImagesVue {
    GtkWidget parent_instance;

    Diapo    courante;
    Diapo    voisines[2];           /* [0] précédente, [1] suivante           */
    int      quarts;                /* rotation d'affichage                   */

    gboolean ajuste;                /* le zoom suit la taille de la toile     */
    double   zoom;                  /* pixels d'écran par pixel d'image       */
    double   cx, cy;                /* centre de l'image / centre de la toile */

    /* --- glissement --- */
    double   glisse;                /* décalage horizontal affiché            */
    double   brut;                  /* celui du doigt, avant la résistance    */
    double   brut0;
    gboolean glisse_actif;          /* un doigt ou la souris tient l'image    */
    gboolean pave_actif;            /* deux doigts sur le pavé tactile        */
    guint    pave_fin_id;
    struct { gint64 t; double x; } ech[8];
    guint    n_ech;

    guint    anim_id;
    gint64   anim_debut;
    gint64   anim_duree;
    double   anim_de, anim_vers;
    int      anim_sens;

    /* --- déplacement d'une image agrandie --- */
    gboolean deplace;
    double   dep_cx, dep_cy;

    /* --- deux doigts : pincer et tourner, en un seul geste --- */
    GtkGesture *g_pince, *g_rotation;
    gboolean geste;                 /* deux doigts posés                      */
    double   geste_zoom0;
    double   geste_angle0;          /* rotation libre au début du geste       */
    double   geste_ux, geste_uy;    /* point saisi, en pixels d'image         */
    double   geste_echelle;         /* rapport d'écartement depuis le début   */
    double   geste_tour;            /* angle tourné depuis le début, radians  */
    gboolean rebase;                /* le doigt restant repart d'ici          */

    /* Rotation LIBRE, en degrés, ajoutée aux quarts de tour. Elle n'existe
     * que sous les doigts et pendant le retour qui suit : au repos, elle vaut
     * toujours zéro. */
    double   angle;

    /* --- retour animé : zoom, position et angle vers un état de repos --- */
    guint    retour_id;
    gint64   retour_debut;
    double   r_de[4], r_vers[4];    /* zoom, cx, cy, angle                    */
    gboolean r_ajuste;

    guint    touche_id;             /* appui simple, en attente d'un second   */

    double   cumul;                 /* crans de molette fractionnaires        */
    double   px, py;                /* dernier emplacement du pointeur        */
    gboolean curseur_masque;

    guint    annonce_id;
};

enum { NAVIGUER, ZOOM_CHANGE, TOUCHE, DOUBLE_CLIC, N_SIGNAUX };
static guint signaux[N_SIGNAUX];

G_DEFINE_FINAL_TYPE (ImagesVue, images_vue, GTK_TYPE_WIDGET)

/* -------------------------------------------------------------------------
 * Géométrie
 * ------------------------------------------------------------------------- */

static double
largeur_toile (ImagesVue *v)
{
    return gtk_widget_get_width (GTK_WIDGET (v));
}

static double
hauteur_toile (ImagesVue *v)
{
    return gtk_widget_get_height (GTK_WIDGET (v));
}

/* L'échelle RÉELLE de la sortie, fractionnaire comprise. Le facteur entier
 * de gtk_widget_get_scale_factor arrondit 1,25 à 2, et « taille réelle »
 * cesserait de l'être. */
static double
echelle (ImagesVue *v)
{
    GtkNative *n = gtk_widget_get_native (GTK_WIDGET (v));
    GdkSurface *s = n != NULL ? gtk_native_get_surface (n) : NULL;
    double e = s != NULL ? gdk_surface_get_scale (s) : 1.0;
    return e > 0.0 ? e : 1.0;
}

static gboolean
a_une_image (ImagesVue *v)
{
    return v->courante.largeur > 0 && v->courante.hauteur > 0;
}

/* Dimensions telles qu'affichées : un quart de tour échange les côtés. */
static void
dims (ImagesVue *v, double *w, double *h)
{
    gboolean couchee = (v->quarts & 1) != 0;
    *w = couchee ? v->courante.hauteur : v->courante.largeur;
    *h = couchee ? v->courante.largeur : v->courante.hauteur;
}

static double
ajuste_pour (ImagesVue *v, double iw, double ih)
{
    double W = largeur_toile (v), H = hauteur_toile (v);
    if (iw <= 0 || ih <= 0 || W <= 0 || H <= 0)
        return 1.0;

    /* Jamais au-delà de la taille réelle : une icône de 48 pixels étirée
     * sur tout l'écran serait floue, et l'on croirait le fichier abîmé. */
    double e = echelle (v);
    return MIN (1.0, MIN (W * e / iw, H * e / ih));
}

static double
zoom_ajuste (ImagesVue *v)
{
    double iw, ih;
    dims (v, &iw, &ih);
    return ajuste_pour (v, iw, ih);
}

/* Pixels LOGIQUES de la toile par pixel d'image : ceux de la géométrie. */
static double
zoom_logique (ImagesVue *v)
{
    return v->zoom / echelle (v);
}

/* L'image déborde-t-elle de la toile ? C'est ce qui fait qu'un doigt
 * déplace au lieu de faire glisser. */
static gboolean
zoomee (ImagesVue *v)
{
    if (!a_une_image (v))
        return FALSE;
    double iw, ih, zl = zoom_logique (v);
    dims (v, &iw, &ih);
    return iw * zl > largeur_toile (v) + 0.5 || ih * zl > hauteur_toile (v) + 0.5;
}

/* Une image plus grande que la toile ne s'en écarte pas : ses bords
 * s'arrêtent aux bords. Plus petite, elle reste centrée. */
static void
bornes_pour (ImagesVue *v, double zoom, double *cx, double *cy)
{
    double iw, ih, zl = zoom / echelle (v);
    dims (v, &iw, &ih);
    double mx = MAX (0.0, (iw * zl - largeur_toile (v)) / 2.0);
    double my = MAX (0.0, (ih * zl - hauteur_toile (v)) / 2.0);
    *cx = CLAMP (*cx, -mx, mx);
    *cy = CLAMP (*cy, -my, my);
}

static void
borner (ImagesVue *v)
{
    bornes_pour (v, v->zoom, &v->cx, &v->cy);
}

/* -------------------------------------------------------------------------
 * Annonces
 * ------------------------------------------------------------------------- */

static void
maj_curseur (ImagesVue *v)
{
    const char *nom = NULL;
    if (v->curseur_masque)
        nom = "none";
    else if (v->deplace)
        nom = "grabbing";
    else if (zoomee (v))
        nom = "grab";
    gtk_widget_set_cursor_from_name (GTK_WIDGET (v), nom);
}

static gboolean
emettre_zoom (gpointer data)
{
    ImagesVue *v = data;
    v->annonce_id = 0;
    maj_curseur (v);
    g_signal_emit (v, signaux[ZOOM_CHANGE], 0);
    return G_SOURCE_REMOVE;
}

/* DIFFÉRÉE, et ce n'est pas un raffinement. Le zoom change aussi pendant
 * l'allocation, quand la fenêtre change de taille ; l'application y répond
 * en réécrivant le pourcentage de la barre — donc en redemandant une mise
 * en page, au milieu de celle qui est en cours. GTK le signale, et selon
 * l'ordre des widgets la barre peut garder l'ancienne valeur. */
static void
annoncer_zoom (ImagesVue *v)
{
    if (v->annonce_id == 0)
        v->annonce_id = g_idle_add (emettre_zoom, v);
}

/* -------------------------------------------------------------------------
 * Retour animé
 *
 * Ce qui suit un geste : l'image lâchée de travers se cale sur le quart de
 * tour le plus proche, l'image trop rétrécie revient à l'ajustement, et le
 * double appui zoome en glissant plutôt qu'en sautant. Zoom, position et
 * angle bougent ensemble, sur la même courbe que le balayage.
 * ------------------------------------------------------------------------- */

static void
retour_poser (ImagesVue *v, double p)
{
    v->zoom  = v->r_de[0] + (v->r_vers[0] - v->r_de[0]) * p;
    v->cx    = v->r_de[1] + (v->r_vers[1] - v->r_de[1]) * p;
    v->cy    = v->r_de[2] + (v->r_vers[2] - v->r_de[2]) * p;
    v->angle = v->r_de[3] + (v->r_vers[3] - v->r_de[3]) * p;
}

static void
retour_fini (ImagesVue *v)
{
    retour_poser (v, 1.0);
    v->angle  = 0.0;
    v->ajuste = v->r_ajuste;
    /* La toile a pu changer de taille pendant les 220 ms : l'ajustement se
     * recalcule sur celle d'aujourd'hui, pas sur celle du départ. */
    if (v->ajuste) {
        v->zoom = zoom_ajuste (v);
        v->cx = v->cy = 0.0;
    }
    borner (v);
    gtk_widget_queue_draw (GTK_WIDGET (v));
    annoncer_zoom (v);
}

/* Un nouveau geste, un bouton, une touche pendant le retour : on l'achève
 * d'un coup. L'interrompre en chemin laisserait l'image de biais — la
 * rotation libre n'a pas le droit de survivre au geste qui l'a créée. */
static void
finir_retour (ImagesVue *v)
{
    if (v->retour_id == 0)
        return;
    gtk_widget_remove_tick_callback (GTK_WIDGET (v), v->retour_id);
    v->retour_id = 0;
    retour_fini (v);
}

static gboolean
on_retour (GtkWidget *w, GdkFrameClock *horloge, gpointer data)
{
    (void) data;
    ImagesVue *v = IMAGES_VUE (w);

    gint64 t = gdk_frame_clock_get_frame_time (horloge);
    if (v->retour_debut == 0)
        v->retour_debut = t;
    double p = MIN (1.0, (double) (t - v->retour_debut) / DUREE_RETOUR);

    if (p < 1.0) {
        retour_poser (v, 1.0 - pow (1.0 - p, 3.0));
        gtk_widget_queue_draw (w);
        annoncer_zoom (v);
        return G_SOURCE_CONTINUE;
    }
    v->retour_id = 0;
    retour_fini (v);
    return G_SOURCE_REMOVE;
}

static void
animer_retour (ImagesVue *v, double zoom, double cx, double cy, double angle,
               gboolean ajuste)
{
    if (v->retour_id != 0) {
        gtk_widget_remove_tick_callback (GTK_WIDGET (v), v->retour_id);
        v->retour_id = 0;
    }
    v->r_de[0] = v->zoom; v->r_de[1] = v->cx; v->r_de[2] = v->cy; v->r_de[3] = v->angle;
    v->r_vers[0] = zoom;  v->r_vers[1] = cx;  v->r_vers[2] = cy;  v->r_vers[3] = angle;
    v->r_ajuste = ajuste;

    gboolean animer = TRUE;
    g_object_get (gtk_widget_get_settings (GTK_WIDGET (v)),
                  "gtk-enable-animations", &animer, NULL);
    if (!animer) {
        retour_fini (v);
        return;
    }
    /* Pas « ajustée » pendant le trajet : une allocation qui tomberait au
     * milieu recalerait l'image d'un coup sur l'ajustement. */
    v->ajuste = FALSE;
    v->retour_debut = 0;
    v->retour_id = gtk_widget_add_tick_callback (GTK_WIDGET (v), on_retour, NULL, NULL);
}

/* -------------------------------------------------------------------------
 * Zoom
 * ------------------------------------------------------------------------- */

/* Le point (px, py) de la toile reste sous le pointeur, ou sous les doigts :
 * c'est lui qu'on regardait, c'est lui qu'on veut voir grandir. */
static void
zoomer_en (ImagesVue *v, double nz, double px, double py)
{
    if (!a_une_image (v))
        return;
    finir_retour (v);

    double fit = zoom_ajuste (v);
    nz = CLAMP (nz, fit, MAX (fit, ZOOM_MAX));

    double W = largeur_toile (v), H = hauteur_toile (v);
    double zl = zoom_logique (v);
    double ux = (px - (W / 2.0 + v->cx)) / zl;
    double uy = (py - (H / 2.0 + v->cy)) / zl;

    v->zoom = nz;
    zl = zoom_logique (v);
    v->cx = px - ux * zl - W / 2.0;
    v->cy = py - uy * zl - H / 2.0;

    /* Revenu à l'ajustement, on s'y tient : la fenêtre qui change de taille
     * ensuite doit ré-ajuster l'image, pas la figer à ce pourcentage. */
    v->ajuste = (nz <= fit * 1.0001);
    if (v->ajuste) {
        v->zoom = fit;
        v->cx = v->cy = 0.0;
    }
    borner (v);
    gtk_widget_queue_draw (GTK_WIDGET (v));
    annoncer_zoom (v);
}

void
images_vue_zoom_ajuste (ImagesVue *v)
{
    g_return_if_fail (IMAGES_IS_VUE (v));
    finir_retour (v);
    v->ajuste = TRUE;
    v->zoom = zoom_ajuste (v);
    v->cx = v->cy = 0.0;
    gtk_widget_queue_draw (GTK_WIDGET (v));
    annoncer_zoom (v);
}

void
images_vue_zoom_reel (ImagesVue *v)
{
    g_return_if_fail (IMAGES_IS_VUE (v));
    zoomer_en (v, 1.0, largeur_toile (v) / 2.0, hauteur_toile (v) / 2.0);
}

void
images_vue_zoomer (ImagesVue *v, double facteur)
{
    g_return_if_fail (IMAGES_IS_VUE (v));
    finir_retour (v);
    double nz = v->zoom * facteur;

    /* On s'arrête sur 100 % en le franchissant : sans ce cran, les pas de
     * 25 % passeraient de 80 à 125 sans jamais montrer la taille réelle. */
    if ((v->zoom < 0.999 && nz > 1.001) || (v->zoom > 1.001 && nz < 0.999))
        nz = 1.0;

    zoomer_en (v, nz, largeur_toile (v) / 2.0, hauteur_toile (v) / 2.0);
}

double
images_vue_get_zoom (ImagesVue *v)
{
    g_return_val_if_fail (IMAGES_IS_VUE (v), 1.0);
    return v->zoom;
}

gboolean
images_vue_est_ajustee (ImagesVue *v)
{
    g_return_val_if_fail (IMAGES_IS_VUE (v), TRUE);
    return v->ajuste;
}

void
images_vue_pivoter (ImagesVue *v, int sens)
{
    g_return_if_fail (IMAGES_IS_VUE (v));
    if (!a_une_image (v))
        return;
    finir_retour (v);
    v->quarts = (v->quarts + (sens > 0 ? 1 : 3)) % 4;
    images_vue_zoom_ajuste (v);
}

gboolean
images_vue_manque_de_details (ImagesVue *v)
{
    g_return_val_if_fail (IMAGES_IS_VUE (v), FALSE);
    GdkTexture *t = v->courante.texture;
    if (t == NULL || v->courante.largeur <= 0)
        return FALSE;

    int tw = gdk_texture_get_width (t);
    if (tw >= v->courante.largeur)
        return FALSE;      /* le fichier n'a rien de plus fin à donner */

    /* 5 % de tolérance : un pixel de texture étalé sur 1,02 pixel d'écran
     * ne se voit pas, et décoder 35 Mo pour cela serait du gaspillage. */
    return v->zoom * v->courante.largeur / tw > 1.05;
}

void
images_vue_masquer_curseur (ImagesVue *v, gboolean masquer)
{
    g_return_if_fail (IMAGES_IS_VUE (v));
    v->curseur_masque = masquer;
    maj_curseur (v);
}

/* -------------------------------------------------------------------------
 * Glissement et animation
 * ------------------------------------------------------------------------- */

static void
arreter_animation (ImagesVue *v)
{
    if (v->anim_id != 0) {
        gtk_widget_remove_tick_callback (GTK_WIDGET (v), v->anim_id);
        v->anim_id = 0;
    }
    v->anim_sens = 0;
}

/* Un doigt reprend l'image pendant qu'elle file vers la suivante : on la
 * laisse arriver d'un coup, puis le nouveau geste part de là. Le balayage
 * a déjà été décidé ; le reprendre en vol serait le désavouer. */
static void
finir_animation (ImagesVue *v)
{
    if (v->anim_id == 0)
        return;
    int sens = v->anim_sens;
    arreter_animation (v);
    if (sens != 0) {
        v->glisse = 0.0;
        g_signal_emit (v, signaux[NAVIGUER], 0, sens);
    }
}

static gboolean
on_tick (GtkWidget *w, GdkFrameClock *horloge, gpointer data)
{
    (void) data;
    ImagesVue *v = IMAGES_VUE (w);

    gint64 t = gdk_frame_clock_get_frame_time (horloge);
    if (v->anim_debut == 0)
        v->anim_debut = t;

    double p = (double) (t - v->anim_debut) / (double) v->anim_duree;
    if (p > 1.0)
        p = 1.0;
    /* Part vite, se pose doucement : c'est la courbe qui prolonge le mieux
     * un geste de la main. Une courbe symétrique donnerait l'impression que
     * l'image hésite au moment où le doigt la lâche. */
    double e = 1.0 - pow (1.0 - p, 3.0);
    v->glisse = v->anim_de + (v->anim_vers - v->anim_de) * e;
    gtk_widget_queue_draw (w);

    if (p < 1.0)
        return G_SOURCE_CONTINUE;

    int sens = v->anim_sens;
    v->anim_id = 0;
    v->anim_sens = 0;

    /* Remis à zéro AVANT d'annoncer : l'application change d'image dans le
     * rappel, et la voisine qui vient d'arriver au centre devient la
     * courante au même endroit. Si elle ne le fait pas — liste modifiée
     * entre-temps —, on retombe au moins sur l'image de départ. */
    v->glisse = 0.0;
    if (sens != 0)
        g_signal_emit (v, signaux[NAVIGUER], 0, sens);
    return G_SOURCE_REMOVE;
}

static void
lancer_glisse (ImagesVue *v, double vers, int sens)
{
    arreter_animation (v);

    double W = MAX (1.0, largeur_toile (v));
    double d = fabs (vers - v->glisse);

    gboolean animer = TRUE;
    g_object_get (gtk_widget_get_settings (GTK_WIDGET (v)),
                  "gtk-enable-animations", &animer, NULL);

    if (d < 0.5 || !animer) {
        v->glisse = 0.0;
        gtk_widget_queue_draw (GTK_WIDGET (v));
        if (sens != 0)
            g_signal_emit (v, signaux[NAVIGUER], 0, sens);
        return;
    }

    v->anim_de    = v->glisse;
    v->anim_vers  = vers;
    v->anim_sens  = sens;
    v->anim_debut = 0;
    /* Proportionnelle à ce qui reste à parcourir : une image lâchée aux
     * trois quarts n'a pas à mettre autant de temps qu'une image lancée
     * depuis le bord. */
    v->anim_duree = (gint64) CLAMP (d / W * 320000.0, 110000.0, 300000.0);
    v->anim_id = gtk_widget_add_tick_callback (GTK_WIDGET (v), on_tick, NULL, NULL);
}

static void
echantillon (ImagesVue *v, double x)
{
    if (v->n_ech == G_N_ELEMENTS (v->ech)) {
        memmove (v->ech, v->ech + 1, sizeof v->ech[0] * (G_N_ELEMENTS (v->ech) - 1));
        v->n_ech--;
    }
    v->ech[v->n_ech].t = g_get_monotonic_time ();
    v->ech[v->n_ech].x = x;
    v->n_ech++;
}

static double
vitesse (ImagesVue *v)
{
    if (v->n_ech < 2)
        return 0.0;

    gint64 fin = v->ech[v->n_ech - 1].t;
    if (g_get_monotonic_time () - fin > FENETRE_VITESSE)
        return 0.0;

    guint i = v->n_ech - 1;
    while (i > 0 && fin - v->ech[i - 1].t <= FENETRE_VITESSE)
        i--;

    gint64 dt = fin - v->ech[i].t;
    if (dt <= 0)
        return 0.0;
    return (v->ech[v->n_ech - 1].x - v->ech[i].x) * 1e6 / (double) dt;
}

/* Au bout de la liste, l'image suit le doigt de moins en moins, sans jamais
 * dépasser un huitième de la largeur. On sent le bord ; une image qui ne
 * bougerait pas du tout ferait croire que le geste n'est pas reconnu. */
static double
resister (ImagesVue *v, double brut)
{
    gboolean possible = brut > 0 ? v->voisines[0].existe : v->voisines[1].existe;
    if (possible || brut == 0.0)
        return brut;

    double W = MAX (1.0, largeur_toile (v));
    return copysign (W * 0.125 * (1.0 - exp (-fabs (brut) / (W * 0.4))), brut);
}

/* Le doigt est levé : on va au bout, ou l'on revient. */
static void
conclure (ImagesVue *v)
{
    double W  = MAX (1.0, largeur_toile (v));
    double vx = vitesse (v);
    int sens = 0;

    /* Un revers franc dans l'autre sens annule, même au-delà du seuil : on
     * a changé d'avis, et le geste le dit. */
    if (v->glisse < -DISTANCE_MINIMALE && v->voisines[1].existe
        && (v->glisse < -W * SEUIL_DISTANCE || vx < -SEUIL_VITESSE)
        && vx < SEUIL_VITESSE / 2.0)
        sens = +1;
    else if (v->glisse > DISTANCE_MINIMALE && v->voisines[0].existe
             && (v->glisse > W * SEUIL_DISTANCE || vx > SEUIL_VITESSE)
             && vx > -SEUIL_VITESSE / 2.0)
        sens = -1;

    double vers = sens > 0 ? -(W + ECART) : sens < 0 ? (W + ECART) : 0.0;
    lancer_glisse (v, vers, sens);
}

/* -------------------------------------------------------------------------
 * Tenue : un doigt, ou la souris bouton enfoncé
 * ------------------------------------------------------------------------- */

static void
on_tenue_debut (GtkGestureDrag *g, double x, double y, ImagesVue *v)
{
    (void) g; (void) x; (void) y;

    finir_animation (v);
    finir_retour (v);
    if (v->geste)
        return;

    v->rebase = FALSE;
    v->n_ech  = 0;
    if (zoomee (v)) {
        v->deplace = TRUE;
        v->dep_cx  = v->cx;
        v->dep_cy  = v->cy;
        maj_curseur (v);
    } else {
        v->glisse_actif = TRUE;
        v->brut0 = v->brut = v->glisse;
    }
}

static void
on_tenue (GtkGestureDrag *g, double ox, double oy, ImagesVue *v)
{
    (void) g;

    if (v->geste) {
        v->rebase = TRUE;
        return;
    }

    /* Le second doigt vient de se lever. Le décalage que donne GTK part
     * toujours du PREMIER appui : sans nouvelle base, l'image sauterait à
     * l'endroit où elle aurait été si l'on n'avait jamais pincé. */
    if (v->rebase) {
        v->rebase = FALSE;
        v->n_ech  = 0;
        if (zoomee (v)) {
            v->deplace = TRUE;
            v->glisse_actif = FALSE;
            v->dep_cx = v->cx - ox;
            v->dep_cy = v->cy - oy;
        } else {
            v->deplace = FALSE;
            v->glisse_actif = TRUE;
            v->brut0 = v->glisse - ox;
        }
    }

    if (v->deplace) {
        v->cx = v->dep_cx + ox;
        v->cy = v->dep_cy + oy;
        borner (v);
        gtk_widget_queue_draw (GTK_WIDGET (v));
    } else if (v->glisse_actif) {
        v->brut   = v->brut0 + ox;
        v->glisse = resister (v, v->brut);
        echantillon (v, v->brut);
        gtk_widget_queue_draw (GTK_WIDGET (v));
    }
}

static void
on_tenue_fin (GtkGestureDrag *g, double ox, double oy, ImagesVue *v)
{
    (void) g; (void) ox; (void) oy;

    if (v->deplace) {
        v->deplace = FALSE;
        maj_curseur (v);
        return;
    }
    if (v->glisse_actif) {
        v->glisse_actif = FALSE;
        conclure (v);
    }
}

static void
on_tenue_annulee (GtkGesture *g, GdkEventSequence *s, ImagesVue *v)
{
    (void) g; (void) s;

    if (v->glisse_actif) {
        v->glisse_actif = FALSE;
        lancer_glisse (v, 0.0, 0);
    }
    if (v->deplace) {
        v->deplace = FALSE;
        maj_curseur (v);
    }
}

/* -------------------------------------------------------------------------
 * Deux doigts : pincer pour zoomer, tourner pour pivoter
 *
 * UN SEUL GESTE, DEUX RECONNAISSEURS. GTK sépare le pincement
 * (GtkGestureZoom) de la rotation (GtkGestureRotate) ; la main, non : on
 * écarte et on tourne dans le même mouvement. Les deux alimentent donc le
 * même état, et l'image est recalculée d'un bloc à chaque événement — le
 * point saisi entre les doigts y reste, quelle que soit la combinaison.
 *
 * Au lever, la rotation se cale sur le quart de tour le plus proche : une
 * photo s'affiche droite ou couchée, jamais de biais. Comme avec les boutons
 * de la barre, le fichier n'est jamais réécrit.
 *
 * Le pavé tactile passe par les mêmes reconnaisseurs : pincer et tourner à
 * deux doigts y fait la même chose.
 * ------------------------------------------------------------------------- */

/* Le milieu des doigts est passé en argument plutôt que demandé au
 * reconnaisseur : la géométrie du geste s'éprouve alors sans écran tactile,
 * en l'appelant directement depuis le banc d'essai. */
static void
geste_commencer (ImagesVue *v, double x, double y)
{
    if (v->geste)
        return;

    /* Un glissement en cours cède la place : on ne peut pas à la fois
     * changer d'image et zoomer dans celle qu'on quitte. */
    finir_animation (v);
    finir_retour (v);
    if (v->glisse_actif || v->glisse != 0.0) {
        v->glisse_actif = FALSE;
        arreter_animation (v);
        v->glisse = 0.0;
    }
    v->deplace       = FALSE;
    v->geste         = TRUE;
    v->geste_zoom0   = v->zoom;
    v->geste_angle0  = v->angle;
    v->geste_echelle = 1.0;
    v->geste_tour    = 0.0;

    double W = largeur_toile (v), H = hauteur_toile (v);
    double zl = zoom_logique (v);
    v->geste_ux = (x - (W / 2.0 + v->cx)) / zl;
    v->geste_uy = (y - (H / 2.0 + v->cy)) / zl;
}

static void
geste_appliquer (ImagesVue *v, double x, double y)
{
    double W = largeur_toile (v), H = hauteur_toile (v);
    double fit = zoom_ajuste (v);
    v->zoom = CLAMP (v->geste_zoom0 * v->geste_echelle,
                     fit * ZOOM_ELASTIQUE, MAX (fit, ZOOM_MAX));

    double deg   = v->geste_tour * 180.0 / G_PI;
    double utile = copysign (MAX (0.0, fabs (deg) - ROTATION_MORTE), deg);
    v->angle = v->geste_angle0 + utile;

    /* Le point saisi tourne avec l'image autour de son centre ; on place ce
     * centre pour qu'il retombe sous le milieu des doigts. Zoomer, tourner
     * et déplacer ne font qu'un geste, comme sur un téléphone. */
    double t  = utile * G_PI / 180.0, c = cos (t), s = sin (t);
    double rx = v->geste_ux * c - v->geste_uy * s;
    double ry = v->geste_ux * s + v->geste_uy * c;
    double zl = zoom_logique (v);
    v->cx = x - rx * zl - W / 2.0;
    v->cy = y - ry * zl - H / 2.0;
    v->ajuste = FALSE;

    gtk_widget_queue_draw (GTK_WIDGET (v));
    annoncer_zoom (v);
}

/* Les doigts sont levés : quart de tour le plus proche, retour à
 * l'ajustement si l'image a été rétrécie en deçà, image ramenée dans la
 * toile — le tout en une seule animation. */
static void
geste_conclure (ImagesVue *v)
{
    int q = (int) lround (v->angle / 90.0);
    v->quarts = ((v->quarts + q) % 4 + 4) % 4;
    /* Même image au même endroit : on ne fait que changer d'écriture, les
     * quarts entiers d'un côté, le reste — au plus 45° — de l'autre. */
    v->angle -= 90.0 * q;

    double fit = zoom_ajuste (v);          /* celui des quarts neufs */
    /* 2 % de tolérance : relâché à 101 % de l'ajustement, on veut l'image
     * entière, pas un zoom que personne n'a demandé. */
    gboolean ajuste = v->zoom <= fit * 1.02;
    double zoom = ajuste ? fit : v->zoom;
    double cx = v->cx, cy = v->cy;
    if (ajuste)
        cx = cy = 0.0;
    else
        bornes_pour (v, zoom, &cx, &cy);
    animer_retour (v, zoom, cx, cy, 0.0, ajuste);
}

static void
on_deux_debut (GtkGesture *g, GdkEventSequence *s, ImagesVue *v)
{
    (void) s;
    double x, y;
    if (!a_une_image (v))
        return;
    if (!gtk_gesture_get_bounding_box_center (g, &x, &y)) {
        x = largeur_toile (v) / 2.0;
        y = hauteur_toile (v) / 2.0;
    }
    geste_commencer (v, x, y);
}

static void
on_pince (GtkGestureZoom *g, double facteur, ImagesVue *v)
{
    if (!v->geste)
        return;
    double x, y;
    if (!gtk_gesture_get_bounding_box_center (GTK_GESTURE (g), &x, &y))
        return;
    v->geste_echelle = facteur;
    geste_appliquer (v, x, y);
}

static void
on_rotation (GtkGestureRotate *g, double angle, double delta, ImagesVue *v)
{
    (void) angle; (void) delta;
    if (!v->geste)
        return;
    /* L'écart depuis le DÉBUT du geste, pas depuis l'événement précédent :
     * additionner des écarts successifs accumulerait les erreurs d'arrondi,
     * et l'image dériverait sous des doigts immobiles. */
    double x, y;
    if (!gtk_gesture_get_bounding_box_center (GTK_GESTURE (g), &x, &y))
        return;
    v->geste_tour = gtk_gesture_rotate_get_angle_delta (g);
    geste_appliquer (v, x, y);
}

/* Les deux reconnaisseurs finissent chacun de leur côté, dans un ordre que
 * rien ne garantit. Le geste ne se conclut qu'au départ du dernier. */
static void
on_deux_fin (GtkGesture *g, GdkEventSequence *s, ImagesVue *v)
{
    (void) s;
    if (!v->geste)
        return;
    GtkGesture *autre = (g == v->g_pince) ? v->g_rotation : v->g_pince;
    if (gtk_gesture_is_recognized (autre))
        return;
    v->geste  = FALSE;
    v->rebase = TRUE;
    geste_conclure (v);
}

/* -------------------------------------------------------------------------
 * Appuis
 *
 *   un appui du doigt     montre ou masque les commandes
 *   double appui, doigt   plein écran, et retour — au doigt comme à la
 *   ou souris             souris : essayé sur MADOO le 10 septembre 2026,
 *                         le zoom au double appui s'est révélé moins
 *                         naturel que le plein écran sur cette machine.
 * ------------------------------------------------------------------------- */

static gboolean
est_doigt (GtkGesture *g)
{
    GdkDevice *d = gtk_gesture_get_device (g);
    GdkInputSource src = d != NULL ? gdk_device_get_source (d) : GDK_SOURCE_MOUSE;
    return src == GDK_SOURCE_TOUCHSCREEN || src == GDK_SOURCE_PEN;
}

static gboolean
emettre_touche (gpointer data)
{
    ImagesVue *v = data;
    v->touche_id = 0;
    g_signal_emit (v, signaux[TOUCHE], 0);
    return G_SOURCE_REMOVE;
}

static void
on_appui (GtkGestureClick *g, int n, double x, double y, ImagesVue *v)
{
    if (n != 2)
        return;
    (void) g; (void) x; (void) y;
    g_clear_handle_id (&v->touche_id, g_source_remove);
    g_signal_emit (v, signaux[DOUBLE_CLIC], 0);
}

static void
on_relache (GtkGestureClick *g, int n, double x, double y, ImagesVue *v)
{
    (void) x; (void) y;
    if (n != 1 || !est_doigt (GTK_GESTURE (g)))
        return;
    g_clear_handle_id (&v->touche_id, g_source_remove);
    v->touche_id = g_timeout_add (DELAI_DOUBLE, emettre_touche, v);
}

/* -------------------------------------------------------------------------
 * Molette et pavé tactile
 *
 *   Ctrl + molette      zoom autour du pointeur, partout
 *   image agrandie      la molette la déplace
 *   image ajustée
 *     souris            un cran = une image
 *     pavé tactile      deux doigts à l'horizontale font glisser l'image,
 *                       exactement comme un doigt sur l'écran
 *
 * Le pavé ne change PAS d'image à la verticale : il émet des dizaines de
 * petits événements par geste, et chaque frôlement ferait défiler le dossier.
 * ------------------------------------------------------------------------- */

static void
pave_conclure (ImagesVue *v)
{
    g_clear_handle_id (&v->pave_fin_id, g_source_remove);
    if (v->pave_actif) {
        v->pave_actif = FALSE;
        conclure (v);
    }
}

/* Filet si la fin du geste n'arrive pas. Le pavé la signale normalement
 * quand les doigts se lèvent ; mais un geste mal reconnu laisserait sinon
 * l'image à mi-course, sans que rien ne la ramène. */
static gboolean
pave_delai (gpointer data)
{
    ImagesVue *v = data;
    v->pave_fin_id = 0;
    pave_conclure (v);
    return G_SOURCE_REMOVE;
}

static void
on_defile_fin (GtkEventControllerScroll *c, ImagesVue *v)
{
    (void) c;
    pave_conclure (v);
}

static gboolean
on_defile (GtkEventControllerScroll *c, double dx, double dy, ImagesVue *v)
{
    GdkModifierType mods = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (c));
    gboolean cran = gtk_event_controller_scroll_get_unit (c) == GDK_SCROLL_UNIT_WHEEL;

    if (mods & GDK_CONTROL_MASK) {
        double f = cran ? pow (PAS_MOLETTE, -dy) : exp (-dy * 0.01);
        zoomer_en (v, v->zoom * f, v->px, v->py);
        return TRUE;
    }

    if (!v->pave_actif && zoomee (v)) {
        double k = cran ? 64.0 : 1.0;
        v->cx -= dx * k;
        v->cy -= dy * k;
        borner (v);
        gtk_widget_queue_draw (GTK_WIDGET (v));
        return TRUE;
    }

    if (cran) {
        double d = fabs (dy) >= fabs (dx) ? dy : dx;
        /* Une molette fine émet des quarts de cran : on les additionne. Un
         * changement de sens repart de zéro, sinon trois quarts vers le bas
         * suivis d'un quart vers le haut feraient avancer. */
        if ((d > 0) != (v->cumul > 0))
            v->cumul = 0.0;
        v->cumul += d;
        if (fabs (v->cumul) >= 1.0) {
            int sens = v->cumul > 0 ? +1 : -1;
            v->cumul = 0.0;
            g_signal_emit (v, signaux[NAVIGUER], 0, sens);
        }
        return TRUE;
    }

    if (!v->pave_actif) {
        if (fabs (dx) <= fabs (dy))
            return FALSE;
        finir_animation (v);
        v->pave_actif = TRUE;
        v->brut = v->glisse;
        v->n_ech = 0;
    }
    /* Même sens que dans une fenêtre qui défile : le contenu suit les
     * doigts selon le réglage du pavé, défilement naturel ou non. */
    v->brut  -= dx;
    v->glisse = resister (v, v->brut);
    echantillon (v, v->brut);
    gtk_widget_queue_draw (GTK_WIDGET (v));

    g_clear_handle_id (&v->pave_fin_id, g_source_remove);
    v->pave_fin_id = g_timeout_add (150, pave_delai, v);
    return TRUE;
}

static void
on_mouvement (GtkEventControllerMotion *m, double x, double y, ImagesVue *v)
{
    (void) m;
    v->px = x;
    v->py = y;
}

/* -------------------------------------------------------------------------
 * Dessin
 * ------------------------------------------------------------------------- */

static void
peindre (ImagesVue *v, GtkSnapshot *s, const Diapo *d,
         double zoom, double degres, double x, double y)
{
    if (d->texture == NULL || d->largeur <= 0 || d->hauteur <= 0)
        return;

    double e  = echelle (v);
    double zl = zoom / e;
    double w  = d->largeur * zl, h = d->hauteur * zl;

    /* Le filtre dépend de ce que devient un pixel de texture à l'écran.
     * Réduire sans mipmaps crénelle les photos fines — les feuillages, les
     * textes. Agrandir fortement en lissant rend flous les pixels qu'on
     * zoome précisément pour les voir. */
    double r = (w * e) / gdk_texture_get_width (d->texture);
    GskScalingFilter f = r < 0.999 ? GSK_SCALING_FILTER_TRILINEAR
                       : r >= 3.0  ? GSK_SCALING_FILTER_NEAREST
                       :             GSK_SCALING_FILTER_LINEAR;

    gtk_snapshot_save (s);
    if (degres == 0.0) {
        /* Calée sur le pixel de l'écran : ajustée à une demi-position
         * près, toute la photo serait rééchantillonnée et paraîtrait
         * floue. */
        graphene_rect_t rect = GRAPHENE_RECT_INIT (
            (float) (round ((x - w / 2.0) * e) / e),
            (float) (round ((y - h / 2.0) * e) / e),
            (float) w, (float) h);
        gtk_snapshot_append_scaled_texture (s, d->texture, f, &rect);
    } else {
        gtk_snapshot_translate (s, &GRAPHENE_POINT_INIT ((float) x, (float) y));
        gtk_snapshot_rotate (s, (float) degres);
        gtk_snapshot_append_scaled_texture (s, d->texture, f,
            &GRAPHENE_RECT_INIT ((float) (-w / 2.0), (float) (-h / 2.0),
                                 (float) w, (float) h));
    }
    gtk_snapshot_restore (s);
}

static void
images_vue_snapshot (GtkWidget *w, GtkSnapshot *s)
{
    ImagesVue *v = IMAGES_VUE (w);
    double W = largeur_toile (v), H = hauteur_toile (v);
    double pas = W + ECART;

    peindre (v, s, &v->courante, v->zoom, v->quarts * 90.0 + v->angle,
             W / 2.0 + v->cx + v->glisse, H / 2.0 + v->cy);

    /* Les voisines toujours ajustées et droites : c'est ainsi qu'elles
     * s'afficheront en devenant courantes, et le relais se fait sans saut. */
    if (v->glisse > 0.5) {
        const Diapo *d = &v->voisines[0];
        peindre (v, s, d, ajuste_pour (v, d->largeur, d->hauteur), 0,
                 W / 2.0 + v->glisse - pas, H / 2.0);
    } else if (v->glisse < -0.5) {
        const Diapo *d = &v->voisines[1];
        peindre (v, s, d, ajuste_pour (v, d->largeur, d->hauteur), 0,
                 W / 2.0 + v->glisse + pas, H / 2.0);
    }
}

static void
images_vue_measure (GtkWidget *w, GtkOrientation o, int pour,
                    int *min, int *nat, int *min_base, int *nat_base)
{
    (void) w; (void) pour;
    /* Aucune exigence : c'est la fenêtre qui décide de la taille de la
     * toile, et l'image s'y ajuste — jamais l'inverse. */
    *min = 1;
    *nat = (o == GTK_ORIENTATION_HORIZONTAL) ? 640 : 420;
    *min_base = *nat_base = -1;
}

static void
images_vue_size_allocate (GtkWidget *w, int largeur, int hauteur, int ligne)
{
    (void) largeur; (void) hauteur; (void) ligne;
    ImagesVue *v = IMAGES_VUE (w);

    /* Sous les doigts ou pendant le retour, la géométrie appartient au
     * geste : l'allocation déclenchée par la mise à jour du pourcentage
     * dans la barre ne doit pas recaler l'image en plein mouvement. */
    if (v->geste || v->retour_id != 0)
        return;

    if (v->ajuste) {
        v->zoom = zoom_ajuste (v);
    } else if (v->zoom <= zoom_ajuste (v)) {
        /* La fenêtre a grandi jusqu'à contenir l'image agrandie : elle
         * redevient ajustée, et le restera. */
        images_vue_zoom_ajuste (v);
    }
    borner (v);
    annoncer_zoom (v);
}

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

static void
poser (Diapo *d, GdkTexture *t, int largeur, int hauteur)
{
    g_set_object (&d->texture, t);
    d->largeur = largeur;
    d->hauteur = hauteur;
}

void
images_vue_set_image (ImagesVue *v, GdkTexture *texture, int largeur, int hauteur)
{
    g_return_if_fail (IMAGES_IS_VUE (v));

    poser (&v->courante, texture, largeur, hauteur);
    v->quarts  = 0;
    v->deplace = FALSE;
    v->geste   = FALSE;
    v->angle   = 0.0;
    g_clear_handle_id (&v->touche_id, g_source_remove);
    if (v->retour_id != 0) {
        gtk_widget_remove_tick_callback (GTK_WIDGET (v), v->retour_id);
        v->retour_id = 0;
    }

    /* Un doigt encore posé garde la main : l'image courante vient d'arriver
     * du décodeur pendant qu'on balayait, le geste n'a pas à sauter. */
    if (!v->glisse_actif && !v->pave_actif) {
        arreter_animation (v);
        v->glisse = 0.0;
    }
    images_vue_zoom_ajuste (v);
}

void
images_vue_set_texture (ImagesVue *v, GdkTexture *texture)
{
    g_return_if_fail (IMAGES_IS_VUE (v));
    g_set_object (&v->courante.texture, texture);
    gtk_widget_queue_draw (GTK_WIDGET (v));
}

void
images_vue_set_voisine (ImagesVue *v, int sens, gboolean existe,
                        GdkTexture *texture, int largeur, int hauteur)
{
    g_return_if_fail (IMAGES_IS_VUE (v));
    Diapo *d = &v->voisines[sens > 0 ? 1 : 0];
    d->existe = existe;
    poser (d, texture, largeur, hauteur);
    if (v->glisse != 0.0)
        gtk_widget_queue_draw (GTK_WIDGET (v));
}

GtkWidget *
images_vue_new (void)
{
    return g_object_new (IMAGES_TYPE_VUE, NULL);
}

/* -------------------------------------------------------------------------
 * Cycle de vie
 * ------------------------------------------------------------------------- */

static void
images_vue_dispose (GObject *o)
{
    ImagesVue *v = IMAGES_VUE (o);

    g_clear_handle_id (&v->annonce_id, g_source_remove);
    g_clear_handle_id (&v->pave_fin_id, g_source_remove);
    g_clear_handle_id (&v->touche_id, g_source_remove);
    g_clear_object (&v->courante.texture);
    g_clear_object (&v->voisines[0].texture);
    g_clear_object (&v->voisines[1].texture);

    G_OBJECT_CLASS (images_vue_parent_class)->dispose (o);
}

static void
images_vue_class_init (ImagesVueClass *klass)
{
    GObjectClass   *ok = G_OBJECT_CLASS (klass);
    GtkWidgetClass *wk = GTK_WIDGET_CLASS (klass);

    ok->dispose       = images_vue_dispose;
    wk->snapshot      = images_vue_snapshot;
    wk->measure       = images_vue_measure;
    wk->size_allocate = images_vue_size_allocate;

    gtk_widget_class_set_css_name (wk, "toile");

    signaux[NAVIGUER] = g_signal_new ("naviguer", G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
    signaux[ZOOM_CHANGE] = g_signal_new ("zoom-change", G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signaux[TOUCHE] = g_signal_new ("touche", G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signaux[DOUBLE_CLIC] = g_signal_new ("double-clic", G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
images_vue_init (ImagesVue *v)
{
    GtkWidget *w = GTK_WIDGET (v);

    v->ajuste = TRUE;
    v->zoom   = 1.0;

    /* Les voisines sont dessinées hors de la toile pendant le glissement :
     * sans découpe, elles passeraient par-dessus les barres. */
    gtk_widget_set_overflow (w, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_hexpand (w, TRUE);
    gtk_widget_set_vexpand (w, TRUE);
    gtk_widget_add_css_class (w, "images-toile");

    GtkGesture *tenue = gtk_gesture_drag_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (tenue), GDK_BUTTON_PRIMARY);
    g_signal_connect (tenue, "drag-begin",  G_CALLBACK (on_tenue_debut),   v);
    g_signal_connect (tenue, "drag-update", G_CALLBACK (on_tenue),         v);
    g_signal_connect (tenue, "drag-end",    G_CALLBACK (on_tenue_fin),     v);
    g_signal_connect (tenue, "cancel",      G_CALLBACK (on_tenue_annulee), v);
    gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (tenue));

    GtkGesture *pince = gtk_gesture_zoom_new ();
    g_signal_connect (pince, "begin",         G_CALLBACK (on_deux_debut), v);
    g_signal_connect (pince, "scale-changed", G_CALLBACK (on_pince),      v);
    g_signal_connect (pince, "end",           G_CALLBACK (on_deux_fin),   v);
    gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (pince));

    GtkGesture *rotation = gtk_gesture_rotate_new ();
    g_signal_connect (rotation, "begin",         G_CALLBACK (on_deux_debut), v);
    g_signal_connect (rotation, "angle-changed", G_CALLBACK (on_rotation),   v);
    g_signal_connect (rotation, "end",           G_CALLBACK (on_deux_fin),   v);
    gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (rotation));

    /* Empruntés, pas possédés : le widget les garde, et ils meurent avec
     * lui. On ne s'en sert que pour savoir lequel des deux vient de finir. */
    v->g_pince    = pince;
    v->g_rotation = rotation;

    GtkGesture *clic = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (clic), GDK_BUTTON_PRIMARY);
    g_signal_connect (clic, "pressed",  G_CALLBACK (on_appui),   v);
    g_signal_connect (clic, "released", G_CALLBACK (on_relache), v);
    gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (clic));

    /* DEUX GROUPES, ET SURTOUT PAS UN SEUL.
     *
     * Un groupe GTK partage l'état de chaque doigt entre ses gestes : ce que
     * l'un refuse, tous le refusent. La tenue et l'appui ne suivent qu'UN
     * doigt, et refusent le second. Réunis avec eux, pincer et tourner
     * perdaient ce second doigt à l'instant où il se posait : ils
     * démarraient, puis s'arrêtaient dans la même image, sans avoir rien
     * transmis. Constaté sur MADOO le 10 septembre 2026, au journal —
     * « begin » puis « end » sur le même wl_touch.down — alors que le banc
     * d'essai n'y voyait rien : sa souris virtuelle n'a qu'un seul contact.
     *
     * Pincer et tourner partagent les deux mêmes doigts : ensemble. La
     * tenue et l'appui partagent le premier : ensemble. Entre les deux
     * groupes, rien ne se transmet tant que personne ne réclame un doigt
     * — et aucun de ces gestes ne le fait. */
    gtk_gesture_group (rotation, pince);
    gtk_gesture_group (clic,     tenue);

    GtkEventController *defil = gtk_event_controller_scroll_new (
        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect (defil, "scroll",     G_CALLBACK (on_defile),     v);
    g_signal_connect (defil, "scroll-end", G_CALLBACK (on_defile_fin), v);
    gtk_widget_add_controller (w, defil);

    GtkEventController *mvt = gtk_event_controller_motion_new ();
    g_signal_connect (mvt, "motion", G_CALLBACK (on_mouvement), v);
    gtk_widget_add_controller (w, mvt);
}
