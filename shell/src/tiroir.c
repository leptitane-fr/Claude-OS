#include "tiroir.h"

#include <gtk4-layer-shell.h>

/* LARGEUR DE LA LISIERE, ET POURQUOI ELLE N'EST PLUS CELLE DU BORD BAS.
 *
 * Elle a fait 10 px, comme la bande qui rappelle le dock. Mesure au banc le
 * 15 septembre 2026, doigt virtuel par uinput -- donc par libinput et le
 * touch.c de labwc, le vrai chemin du contact : le glissé n'ouvrait QUE si
 * le premier contact tombait entre 0 et 9 px du bord. A 11 px, plus rien,
 * des deux cotes.
 *
 * C'est assez pour le pointeur, qu'on vise. Ce ne l'est pas pour le doigt :
 * la dalle rapporte la pose une trame apres le contact, et un doigt qui
 * entre vite depuis le cadre a deja parcouru dix a vingt pixels quand sa
 * position est rapportee. Le bord BAS s'en tire a 10 px parce qu'on l'aborde
 * perpendiculairement, en butant contre le chassis ; les bords lateraux se
 * prennent en biais.
 *
 * CE QUE CES 24 PX COUTENT : les applications ne recoivent plus ni contact
 * ni clic dans les 24 premiers pixels de gauche et de droite -- 3,9 mm sur
 * cette dalle, qui fait 310 mm pour 1920 px. C'est le prix du geste, et il
 * se rend en changeant cette seule ligne. */
#define BANDE_PX 24

/* CE QUE LE POINTEUR, LUI, GARDE : dix pixels.
 *
 * La lisiere elargie sert le doigt ; l'ouverture au pointeur pose reste
 * bornee aux 10 px du bord. Une souris immobilisee a 20 px du cadre -- sur
 * la bordure d'une fenetre, par exemple -- ne doit pas faire sortir un volet
 * au bout d'une seconde. Le doigt est imprecis, le pointeur ne l'est pas :
 * il n'y a aucune raison de leur donner la meme tolerance. */
#define POSE_PX 10

/* Ce qu'il faut parcourir vers l'interieur pour que le glisser compte.
 * 32 px : la valeur retenue pour le bord bas. En dessous, un simple appui au
 * bord ouvrirait le tiroir. */
#define SEUIL_PX 32

/* Le temps que le pointeur doit rester POSE contre le bord pour que le
 * tiroir s'ouvre. Une seconde, demandee telle quelle : les bords lateraux
 * sont l'endroit ou finit tout mouvement un peu vif, et un declenchement au
 * contact ouvrirait surtout par accident. */
#define ATTENTE_MS 1000

/* Duree du deploiement. 200 ms : assez pour qu'on voie d'ou vient le
 * tiroir, assez court pour ne pas attendre. */
#define GLISSE_MS 200

/* -------------------------------------------------------------------------
 * UN TIROIR PAR BORD, ET UNE SEULE FENETRE POUR LES DEUX.
 *
 * Les deux cotes ont chacun leur lisiere, leur minuterie et leur reveleur ;
 * ils s'ouvrent et se ferment sans se consulter. Ils partagent en revanche
 * LA fenetre plein ecran et sa nappe : deux nappes superposees se seraient
 * disputees le clic exterieur, et l'une des deux aurait ferme le mauvais
 * tiroir. La fenetre est montree des qu'un cote s'ouvre, et masquee quand le
 * dernier est rentre.
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkWidget        *bande;      /* la lisiere sensible, toujours presente  */
    GtkWidget        *reveleur;
    guint             attente;    /* la minuterie du pointeur pose           */
    gboolean          ouvert;
    /* Le sens du glisser qui ouvre : +1 vers la droite depuis le bord
     * gauche, -1 vers la gauche depuis le bord droit. Le signe compte -- un
     * glisser qui s'eloigne de l'ecran n'a aucun sens et ne doit rien
     * ouvrir. */
    int               sens;
    GtkLayerShellEdge bord;
    const char       *espace;     /* le namespace de la lisiere              */
    const char       *nom;        /* pour le journal, et pour le banc        */
} Cote;

static Cote G = { .sens = +1, .bord = GTK_LAYER_SHELL_EDGE_LEFT,
                  .espace = "claude-os-lisiere-gauche", .nom = "gauche" };
static Cote D = { .sens = -1, .bord = GTK_LAYER_SHELL_EDGE_RIGHT,
                  .espace = "claude-os-lisiere-droite", .nom = "droite" };

static struct {
    GtkWidget *fenetre;      /* le tiroir : plein ecran, masque au repos    */
    guint      retrait;      /* masquer la fenetre apres l'animation        */
} T;

/* ------------------------------------------------------------------------- */
gboolean
shell_tiroir_ouvert (void)
{
    return G.ouvert || D.ouvert;
}

static gboolean
retrait_fin (gpointer data)
{
    (void) data;
    T.retrait = 0;
    /* On ne masque la fenetre qu'une fois les volets sortis de l'ecran :
     * la masquer tout de suite escamoterait l'animation. Et seulement si
     * l'autre cote n'a pas ete ouvert entre-temps. */
    if (!shell_tiroir_ouvert () && T.fenetre != NULL)
        gtk_widget_set_visible (T.fenetre, FALSE);
    return G_SOURCE_REMOVE;
}

static void
cote_ouvrir (Cote *c)
{
    if (T.fenetre == NULL || c->ouvert)
        return;

    if (T.retrait != 0) {
        g_source_remove (T.retrait);
        T.retrait = 0;
    }
    c->ouvert = TRUE;

    /* LA FENETRE NE CHANGE JAMAIS DE TAILLE : elle est plein ecran des sa
     * creation, et ce sont les reveleurs qui bougent. Redimensionner une
     * surface layer-shell qui porte un popover ouvert fait partir ce popover
     * hors de l'ecran sous labwc 0.8.3 -- regle payee le 11 septembre 2026,
     * et la Console du bord droit ouvre des popovers. */
    gtk_widget_set_visible (T.fenetre, TRUE);
    gtk_revealer_set_reveal_child (GTK_REVEALER (c->reveleur), TRUE);
    /* EN DEBOGAGE SEULEMENT, et c'est ce qui rend le banc possible : une
     * capture montre un volet sorti, elle ne dit pas lequel des deux cotes
     * l'a decide. Meme usage que « visibilite : » dans le dock. */
    g_debug ("tiroir %s : ouvert", c->nom);
}

static void
cote_fermer (Cote *c)
{
    if (T.fenetre == NULL || !c->ouvert)
        return;

    c->ouvert = FALSE;
    gtk_revealer_set_reveal_child (GTK_REVEALER (c->reveleur), FALSE);
    g_debug ("tiroir %s : ferme", c->nom);

    if (T.retrait != 0)
        g_source_remove (T.retrait);
    T.retrait = g_timeout_add (GLISSE_MS + 40, retrait_fin, NULL);
}

static void
cote_basculer (Cote *c)
{
    if (c->ouvert)
        cote_fermer (c);
    else
        cote_ouvrir (c);
}

void shell_tiroir_console_ouvrir   (void) { cote_ouvrir   (&D); }
void shell_tiroir_console_basculer (void) { cote_basculer (&D); }
void shell_tiroir_widgets_ouvrir   (void) { cote_ouvrir   (&G); }
void shell_tiroir_widgets_basculer (void) { cote_basculer (&G); }

/* LES DEUX A LA FOIS, et c'est ce que veut dire « referme le tiroir ».
 * La rangee d'alimentation s'en sert avant d'eteindre ou d'ouvrir les
 * Reglages : ce qu'elle demande, c'est que l'ecran soit rendu, pas qu'un
 * volet precis rentre. */
void
shell_tiroir_fermer (gpointer inutilise)
{
    (void) inutilise;
    cote_fermer (&G);
    cote_fermer (&D);
}

/* -------------------------------------------------------------------------
 * La bande du bord
 * ------------------------------------------------------------------------- */
static void
attente_annuler (Cote *c)
{
    if (c->attente != 0) {
        g_source_remove (c->attente);
        c->attente = 0;
    }
}

static gboolean
attente_echue (gpointer data)
{
    Cote *c = data;
    c->attente = 0;
    cote_ouvrir (c);
    return G_SOURCE_REMOVE;
}

/* LE POINTEUR POSE, ET NON LE POINTEUR QUI PASSE.
 *
 * « enter » suffirait a armer la minuterie, mais un curseur qui traverse la
 * bande en diagonale la declencherait aussi. On rearme donc a chaque
 * mouvement DANS la bande : tant que le curseur bouge, le compte repart de
 * zero, et il ne s'acheve que s'il s'immobilise contre le bord. */
/* La distance au bord de l'ecran, depuis les coordonnees de la lisiere :
 * celle de gauche a son bord en x = 0, celle de droite a l'autre bout. */
static double
au_bord (const Cote *c, double x)
{
    return c->sens > 0 ? x : BANDE_PX - x;
}

static void
on_bande_entree (GtkEventControllerMotion *ctrl, double x, double y, gpointer d)
{
    (void) ctrl; (void) y;
    Cote *c = d;
    if (c->ouvert)
        return;
    attente_annuler (c);
    /* Hors des dix premiers pixels, on n'arme meme pas : voir POSE_PX. */
    if (au_bord (c, x) > POSE_PX)
        return;
    c->attente = g_timeout_add (ATTENTE_MS, attente_echue, c);
}

static void
on_bande_mouvement (GtkEventControllerMotion *ctrl, double x, double y, gpointer d)
{
    on_bande_entree (ctrl, x, y, d);
}

static void
on_bande_sortie (GtkEventControllerMotion *ctrl, gpointer d)
{
    (void) ctrl;
    attente_annuler (d);
}

/* Le glisser du doigt : vers l'interieur de l'ecran, depuis le bord. Il part
 * d'OU QU'IL SOIT dans la lisiere -- c'est tout l'objet de ses 24 px. */
static void
on_bande_glisse (GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    (void) g; (void) dy;
    Cote *c = d;
    if (!c->ouvert && dx * c->sens >= SEUIL_PX) {
        attente_annuler (c);
        cote_ouvrir (c);
    }
}

static void
bande_creer (GtkApplication *app, Cote *c)
{
    GtkWidget *bande = gtk_application_window_new (app);

    /* PRESQUE TRANSPARENTE, ET SURTOUT PAS TOUT A FAIT.
     *
     * Une fenetre GTK entierement transparente et vide ne recoit AUCUN
     * appui : le compositeur ne lui envoie rien, sans une erreur nulle part.
     * Mesure au banc le 11 septembre 2026 sur la bande du bord bas ; fond
     * « transparent », pas un evenement ; fond a 1 %, l'appui et tout le
     * glisser arrivent. La regle est ecrite ici et non dans shell.css : ce
     * n'est pas une couleur, c'est une condition de fonctionnement. */
    static GtkCssProvider *presque = NULL;
    if (presque == NULL) {
        presque = gtk_css_provider_new ();
        gtk_css_provider_load_from_string (presque,
            "window.claude-os-lisiere { background-color: rgba(0, 0, 0, 0.01); }");
        gtk_style_context_add_provider_for_display (gdk_display_get_default (),
            GTK_STYLE_PROVIDER (presque), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_widget_add_css_class (bande, "claude-os-lisiere");

    gtk_layer_init_for_window (GTK_WINDOW (bande));
    gtk_layer_set_layer (GTK_WINDOW (bande), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace (GTK_WINDOW (bande), c->espace);
    gtk_layer_set_anchor (GTK_WINDOW (bande), c->bord,                     TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (bande), GTK_LAYER_SHELL_EDGE_TOP,    TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (bande), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_exclusive_zone (GTK_WINDOW (bande), -1);
    gtk_layer_set_keyboard_mode (GTK_WINDOW (bande),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GtkWidget *plage = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request (plage, BANDE_PX, -1);
    gtk_window_set_child (GTK_WINDOW (bande), plage);

    GtkGesture *g = gtk_gesture_drag_new ();
    g_signal_connect (g, "drag-update", G_CALLBACK (on_bande_glisse), c);
    gtk_widget_add_controller (bande, GTK_EVENT_CONTROLLER (g));

    GtkEventController *m = gtk_event_controller_motion_new ();
    g_signal_connect (m, "enter",  G_CALLBACK (on_bande_entree),    c);
    g_signal_connect (m, "motion", G_CALLBACK (on_bande_mouvement), c);
    g_signal_connect (m, "leave",  G_CALLBACK (on_bande_sortie),    c);
    gtk_widget_add_controller (bande, m);

    gtk_window_present (GTK_WINDOW (bande));
    c->bande = bande;
}

/* -------------------------------------------------------------------------
 * Le tiroir
 * ------------------------------------------------------------------------- */
static void
on_nappe_clic (GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    (void) g; (void) n; (void) x; (void) y; (void) d;
    shell_tiroir_fermer (NULL);
}

static GtkWidget *
volet (const char *classe)
{
    GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (v, "tiroir-volet");
    if (classe != NULL)
        gtk_widget_add_css_class (v, classe);
    return v;
}

/* Le reveleur d'un cote : il entre depuis SON bord, et s'y tient. */
static GtkWidget *
reveleur_creer (Cote *c, GtkWidget *contenu, GtkAlign valign)
{
    GtkWidget *r = gtk_revealer_new ();
    gtk_revealer_set_child (GTK_REVEALER (r), contenu);
    /* La glisse joue a l'envers le geste qu'on vient de faire -- rien
     * d'autre ne se lit aussi vite comme « ce que tu as tire vient de la ».
     * SLIDE_RIGHT pour le volet de gauche, SLIDE_LEFT pour celui de
     * droite. */
    gtk_revealer_set_transition_type (GTK_REVEALER (r),
        c->sens > 0 ? GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT
                    : GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_transition_duration (GTK_REVEALER (r), GLISSE_MS);
    gtk_revealer_set_reveal_child (GTK_REVEALER (r), FALSE);
    gtk_widget_set_halign (r, c->sens > 0 ? GTK_ALIGN_START : GTK_ALIGN_END);
    gtk_widget_set_valign (r, valign);
    c->reveleur = r;
    return r;
}

void
shell_tiroir_init (GtkApplication *app, GtkWidget *console, gboolean apercu)
{
    (void) apercu;
    if (T.fenetre != NULL)
        return;

    bande_creer (app, &G);
    bande_creer (app, &D);

    GtkWidget *fenetre = gtk_application_window_new (app);
    T.fenetre = fenetre;
    gtk_widget_add_css_class (fenetre, "shell");
    gtk_widget_add_css_class (fenetre, "tiroir");

    gtk_layer_init_for_window (GTK_WINDOW (fenetre));
    gtk_layer_set_layer (GTK_WINDOW (fenetre), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace (GTK_WINDOW (fenetre), "claude-os-tiroir");
    /* LES QUATRE BORDS : la fenetre fait tout l'ecran des sa creation. Ce
     * n'est pas une coquetterie -- c'est ce qui permet a la nappe de
     * recueillir le clic exterieur sans qu'on redimensionne jamais la
     * surface. Voir cote_ouvrir(). */
    gtk_layer_set_anchor (GTK_WINDOW (fenetre), GTK_LAYER_SHELL_EDGE_TOP,    TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (fenetre), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (fenetre), GTK_LAYER_SHELL_EDGE_LEFT,   TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (fenetre), GTK_LAYER_SHELL_EDGE_RIGHT,  TRUE);
    gtk_layer_set_exclusive_zone (GTK_WINDOW (fenetre), -1);
    /* ON_DEMAND : la Console doit pouvoir prendre le clavier -- le mot de
     * passe d'un reseau Wi-Fi se tape. Hors saisie, le tiroir ne reclame
     * rien. */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (fenetre),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    /* --- le volet des widgets, au bord gauche ---
     *
     * IL PREND TOUTE LA HAUTEUR, et c'est une place reservee autant qu'une
     * mise en page : les widgets a venir s'empileront dedans, et le volet
     * n'aura qu'a se remplir. Sa largeur est celle de la Console (shell.css)
     * pour que les deux bords se repondent. */
    GtkWidget *gauche = volet ("tiroir-widgets");
    gtk_widget_set_vexpand (gauche, TRUE);
    GtkWidget *mot = gtk_label_new ("Widget\nà venir");
    gtk_label_set_justify (GTK_LABEL (mot), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class (mot, "tiroir-attente");
    gtk_widget_set_vexpand (mot, TRUE);
    gtk_widget_set_valign (mot, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (mot, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (gauche), mot);

    /* --- la Console, au bord droit ---
     *
     * CENTREE VERTICALEMENT, et le volet ne fait que sa hauteur : etire, il
     * laisserait l'alimentation flotter au bas d'un grand vide. C'est le
     * reveleur qui porte le centrage -- le volet, lui, se contente d'etre a
     * sa taille. */
    GtkWidget *droite = volet ("tiroir-console");
    gtk_widget_set_valign (console, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (droite), console);

    GtkWidget *rev_g = reveleur_creer (&G, gauche, GTK_ALIGN_FILL);
    GtkWidget *rev_d = reveleur_creer (&D, droite, GTK_ALIGN_CENTER);

    /* LA NAPPE, ET POURQUOI UN GtkOverlay.
     *
     * Le clic « a cote » doit fermer, le clic « dedans » ne doit rien
     * fermer du tout. Poser un geste de clic sur le conteneur des volets
     * aurait attrape les deux -- un geste pose en amont voit passer ce que
     * ses enfants recoivent. Avec une superposition, GTK designe le widget
     * le plus haut sous le pointeur : le volet s'il y en a un, la nappe
     * sinon. La distinction n'est pas codee, elle est structurelle. */
    GtkWidget *nappe = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (nappe, "tiroir-nappe");
    gtk_widget_set_hexpand (nappe, TRUE);
    gtk_widget_set_vexpand (nappe, TRUE);

    GtkGesture *clic = gtk_gesture_click_new ();
    g_signal_connect (clic, "released", G_CALLBACK (on_nappe_clic), NULL);
    gtk_widget_add_controller (nappe, GTK_EVENT_CONTROLLER (clic));

    GtkWidget *superpose = gtk_overlay_new ();
    gtk_overlay_set_child (GTK_OVERLAY (superpose), nappe);
    gtk_overlay_add_overlay (GTK_OVERLAY (superpose), rev_g);
    gtk_overlay_add_overlay (GTK_OVERLAY (superpose), rev_d);

    gtk_window_set_child (GTK_WINDOW (fenetre), superpose);
    /* Cree masque : les deux volets sont rentres a l'ouverture de session. */
    gtk_widget_set_visible (fenetre, FALSE);
}
