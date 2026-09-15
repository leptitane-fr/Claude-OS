#include "avis.h"

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <math.h>            /* cos, sin, ceil */

/* Hauteur laissee libre en bas pour le dock. Meme raisonnement que
 * BANDE_DOCK dans launcher.c : le dock mesure 88 px et garde 12 px de marge,
 * on arrondit pour que l'avis ne flotte pas au ras des icones.
 *
 * ELLE NE SUIT PAS LE DOCK QUAND IL SORT DE L'ECRAN, et c'est voulu : la
 * position est FIXE. Un avis qui monterait et descendrait selon qu'une
 * application est au premier plan demanderait a l'oeil de le chercher, ce
 * qui est precisement ce qu'on veut lui epargner. */
#define BANDE_BASSE 108

/* Diametre du cadran.
 *
 * FIXE, DESORMAIS. Il valait la largeur de la pilule de la barre d'etat --
 * 172 px mesures le 9 septembre 2026 -- parce qu'il etait cale sur son bord
 * droit et qu'un ecart de quelques pixels s'y serait vu. Au centre de
 * l'ecran il n'y a plus de bord a partager : la mesure n'avait plus d'objet,
 * et faire dependre un diametre de la largeur de l'heure affichee etait
 * devenu une coincidence entretenue pour rien. 168 est ce que valait la
 * pilule, arrondi. */
#define COTE        168

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
    GtkWidget *boite;       /* cadran et message, un seul visible a la fois  */
    GtkWidget *cadran;
    GtkWidget *message;     /* la ligne entiere : icone + texte              */
    GtkWidget *message_icone;
    GtkWidget *message_texte;
    GtkCssProvider *style;  /* opacite engendree -- voir opacite_appliquer   */
    int        opacite;     /* pourcent ; 0 = pas encore regle               */
    guint      minuterie;   /* le decompte du cadran                         */
    guint      expiration;  /* le retrait automatique d'un message           */
    gint64     debut;      /* horloge monotone, en microsecondes             */
    double     total;      /* duree demandee, en secondes                    */
    double     fraction;   /* 1,0 au depart, 0,0 a l'echeance                */
} P;

static void opacite_appliquer (void);

static void
dessiner (GtkDrawingArea *aire, cairo_t *cr, int largeur, int hauteur,
          gpointer data)
{
    (void) data;
    double cx = largeur / 2.0, cy = hauteur / 2.0;

    /* AUCUNE MARGE INTERIEURE, ET C'EST LE POINT. GTK donne ici la zone de
     * CONTENU, bordure CSS deja deduite : une encoche de 3 px dans le trace
     * rendait un disque de 164 px pour une allocation de 172, ceint d'un
     * anneau clair. */
    double R = MIN (largeur, hauteur) / 2.0;

    /* La couleur ET son alpha viennent de la feuille de style, propriete
     * « color » de .avis-cadran, que shell_avis_opacite() reecrit. */
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
         * appellera shell_avis_cacher(). Masquer maintenant ferait
         * clignoter l'ecran juste avant qu'il ne baisse. */
        P.minuterie = 0;
        return G_SOURCE_REMOVE;
    }
    gtk_widget_queue_draw (P.cadran);
    return G_SOURCE_CONTINUE;
}

/* REGION D'ENTREE VIDE : la surface se voit et ne s'attrape pas. Sans cela
 * elle poserait un rectangle mort par-dessus le bureau, et un clic destine a
 * ce qui se trouve dessous serait avale.
 *
 * REPOSEE A CHAQUE AFFICHAGE, et pas seulement au « realize ». La fenetre ne
 * changeait jamais de taille tant qu'elle ne montrait qu'un cadran ; elle
 * passe maintenant d'un disque de 168 px a une ligne de texte et retour. Le
 * protocole veut qu'une region d'entree survive au redimensionnement, mais
 * cela ne coute qu'un appel de le garantir -- et le symptome, un rectangle
 * invisible qui avale les clics au milieu du bureau, serait de ceux qu'on
 * met une soiree a rapporter a leur cause. */
static void
verrouiller_entrees (void)
{
    if (P.fenetre == NULL)
        return;
    GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (P.fenetre));
    if (surface == NULL)
        return;

    cairo_region_t *vide = cairo_region_create ();
    gdk_surface_set_input_region (surface, vide);
    cairo_region_destroy (vide);
}

static void
on_realise (GtkWidget *w, gpointer data)
{
    (void) w; (void) data;
    verrouiller_entrees ();
}

static void
construire (void)
{
    if (P.fenetre != NULL)
        return;

    P.fenetre = gtk_window_new ();
    gtk_widget_add_css_class (P.fenetre, "shell");
    gtk_widget_add_css_class (P.fenetre, "avis");

    gtk_layer_init_for_window (GTK_WINDOW (P.fenetre));
    gtk_layer_set_layer (GTK_WINDOW (P.fenetre), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace (GTK_WINDOW (P.fenetre), "claude-os-avis");
    /* Aucun clavier : l'avis ne doit pas interrompre une frappe. */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (P.fenetre),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    /* UN SEUL BORD ANCRE, ET C'EST CE QUI CENTRE. Sans ancrage a gauche ni a
     * droite, le compositeur centre la surface sur cet axe : il n'y a donc
     * aucune largeur d'ecran a lire, aucune marge a calculer, et rien a
     * reprendre quand l'ecran tourne en mode tablette. */
    gtk_layer_set_anchor (GTK_WINDOW (P.fenetre), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin (GTK_WINDOW (P.fenetre), GTK_LAYER_SHELL_EDGE_BOTTOM,
                          BANDE_BASSE);

    /* Les deux contenus vivent dans la meme boite, un seul visible a la
     * fois. GTK ne compte pas un enfant masque dans la taille demandee : la
     * fenetre fait donc 168 px pour le cadran et la largeur du texte pour un
     * message, sans qu'on ait a la redimensionner a la main. */
    P.boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    P.cadran = gtk_drawing_area_new ();
    gtk_widget_add_css_class (P.cadran, "avis-cadran");
    gtk_widget_set_size_request (P.cadran, COTE, COTE);
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (P.cadran),
                                    dessiner, NULL, NULL);
    gtk_widget_set_visible (P.cadran, FALSE);
    gtk_box_append (GTK_BOX (P.boite), P.cadran);

    /* UNE LIGNE, ET LA COULEUR EST PORTEE PAR ELLE. La regle engendree pose
     * « color » sur .avis-message, c'est-a-dire sur la boite : l'etiquette
     * en herite, et l'icone symbolique aussi -- GTK recolore une icone
     * « -symbolic » avec la couleur du widget, alpha compris. Poser la
     * couleur sur l'etiquette seule aurait laisse l'icone a la teinte du
     * theme, et les deux auraient diverge au premier reglage d'opacite. */
    P.message = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (P.message, "avis-message");
    gtk_widget_set_halign (P.message, GTK_ALIGN_CENTER);

    P.message_icone = gtk_image_new ();
    gtk_widget_add_css_class (P.message_icone, "avis-message-icone");
    /* 30 px : la hauteur des majuscules du texte a cote, a peu pres. Une
     * icone plus petite se lirait comme une puce, plus grande comme le
     * sujet -- or c'est le couple qui porte le sens. */
    gtk_image_set_pixel_size (GTK_IMAGE (P.message_icone), 30);
    gtk_box_append (GTK_BOX (P.message), P.message_icone);

    P.message_texte = gtk_label_new (NULL);
    gtk_box_append (GTK_BOX (P.message), P.message_texte);

    gtk_widget_set_visible (P.message, FALSE);
    gtk_box_append (GTK_BOX (P.boite), P.message);

    gtk_window_set_child (GTK_WINDOW (P.fenetre), P.boite);
    g_signal_connect (P.fenetre, "realize", G_CALLBACK (on_realise), NULL);
    opacite_appliquer ();
}

/* L'OPACITE EST ENGENDREE, LA TEINTE NE L'EST PAS.
 *
 * On reecrit « alpha(...) » autour d'un jeton du theme plutot qu'une valeur
 * RVB : pas une couleur n'est ecrite dans ce fichier, et @avis reste
 * modifiable la ou vivent toutes les autres.
 *
 * LE CADRAN ET LE MESSAGE PARTAGENT LA REGLE, et ce n'est pas une economie
 * de frappe : c'est la garantie qu'ils ne divergeront pas. Deux regles, et
 * un reglage d'opacite finit tot ou tard par n'en toucher qu'une.
 *
 * PLUS AUCUN FOND. Le disque translucide qui portait le cadran en faisait
 * une pastille posee sur le bureau ; on ne veut que le trace, flottant sur
 * ce qui se trouve dessous. La fenetre est donc entierement transparente. */
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
        ".avis-cadran, .avis-message { color: alpha(@avis, %s); }", nombre);

    if (P.style == NULL) {
        P.style = gtk_css_provider_new ();
        gtk_style_context_add_provider_for_display (
            gdk_display_get_default (), GTK_STYLE_PROVIDER (P.style),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    gtk_css_provider_load_from_string (P.style, css);
}

void
shell_avis_opacite (int pourcent)
{
    if (pourcent == P.opacite)
        return;
    P.opacite = pourcent;
    opacite_appliquer ();
    if (P.cadran != NULL)
        gtk_widget_queue_draw (P.cadran);
}

/* Montre l'un des deux contenus, et lui seul. */
static void
montrer (GtkWidget *quoi)
{
    gtk_widget_set_visible (P.cadran,  quoi == P.cadran);
    gtk_widget_set_visible (P.message, quoi == P.message);
    gtk_widget_set_visible (P.fenetre, TRUE);
    verrouiller_entrees ();
}

void
shell_avis_cadran (int secondes)
{
    if (secondes <= 0)
        return;

    construire ();

    /* Un message en cours cede la place : le decompte a une echeance, lui. */
    if (P.expiration != 0) {
        g_source_remove (P.expiration);
        P.expiration = 0;
    }

    P.total    = secondes;
    P.debut    = g_get_monotonic_time ();
    P.fraction = 1.0;
    gtk_widget_queue_draw (P.cadran);

    if (P.minuterie != 0)
        g_source_remove (P.minuterie);
    P.minuterie = g_timeout_add (PAS_MS, on_tic, NULL);

    montrer (P.cadran);
}

static gboolean
on_expire (gpointer data)
{
    (void) data;
    P.expiration = 0;
    if (P.fenetre != NULL)
        gtk_widget_set_visible (P.fenetre, FALSE);
    return G_SOURCE_REMOVE;
}

void
shell_avis_message (const char *icone, const char *texte, int secondes)
{
    if (texte == NULL || *texte == '\0' || secondes <= 0)
        return;

    /* Le compte a rebours l'emporte : voir avis.h. Le message part de toute
     * facon en notification chez celui qui l'emet, rien n'est perdu. */
    if (P.minuterie != 0) {
        g_message ("avis : « %s » ecarte, un compte a rebours est en cours",
                   texte);
        return;
    }

    construire ();
    gtk_label_set_text (GTK_LABEL (P.message_texte), texte);

    /* Icone absente : on la masque au lieu de la vider. Une GtkImage sans
     * nom garde sa place dans la boite -- le texte serait decale a droite
     * du centre, et un message sur deux ne serait pas aligne avec l'autre. */
    if (icone != NULL && *icone != '\0') {
        gtk_image_set_from_icon_name (GTK_IMAGE (P.message_icone), icone);
        gtk_widget_set_visible (P.message_icone, TRUE);
    } else {
        gtk_widget_set_visible (P.message_icone, FALSE);
    }

    /* Rappelable : un second message remplace le premier et repart pour sa
     * propre duree, au lieu d'heriter du reste de celle du precedent. */
    if (P.expiration != 0)
        g_source_remove (P.expiration);
    P.expiration = g_timeout_add_seconds (secondes, on_expire, NULL);

    montrer (P.message);
}

void
shell_avis_cacher (void)
{
    if (P.minuterie != 0) {
        g_source_remove (P.minuterie);
        P.minuterie = 0;
    }
    if (P.expiration != 0) {
        g_source_remove (P.expiration);
        P.expiration = 0;
    }
    if (P.fenetre != NULL)
        gtk_widget_set_visible (P.fenetre, FALSE);
}
