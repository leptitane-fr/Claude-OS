#include "tiroir.h"

#include <gtk4-layer-shell.h>

/* Largeur de la bande sensible, au ras du cadre. 10 px : la meme que celle
 * du bord bas qui rappelle le dock, et pour la meme raison -- assez large
 * pour qu'un doigt venu du cadre la trouve, assez etroite pour ne jamais
 * gener ce qui se trouve dessous. */
#define BANDE_PX 10

/* Ce qu'il faut parcourir vers la gauche pour que le glisser compte. 32 px :
 * la valeur retenue pour le bord bas. En dessous, un simple appui au bord
 * ouvrirait le tiroir. */
#define SEUIL_PX 32

/* Le temps que le pointeur doit rester POSE contre le bord pour que le
 * tiroir s'ouvre. Une seconde, demandee telle quelle : le bord droit est
 * l'endroit ou finit tout mouvement un peu vif, et un declenchement au
 * contact ouvrirait surtout par accident. */
#define ATTENTE_MS 1000

/* Duree du deploiement. 200 ms : assez pour qu'on voie d'ou vient le
 * tiroir, assez court pour ne pas attendre. */
#define GLISSE_MS 200

/* Ecart au bord et entre les volets -- la trame du bureau. */
#define MARGE 12

static struct {
    GtkWidget *bande;        /* la lisiere sensible, toujours presente      */
    GtkWidget *fenetre;      /* le tiroir : plein ecran, masque au repos    */
    GtkWidget *reveleur;
    guint      attente;      /* la minuterie du pointeur pose               */
    guint      retrait;      /* masquer la fenetre apres l'animation        */
    gboolean   ouvert;
} T;

/* ------------------------------------------------------------------------- */
gboolean
shell_tiroir_ouvert (void)
{
    return T.ouvert;
}

static gboolean
retrait_fin (gpointer data)
{
    (void) data;
    T.retrait = 0;
    /* On ne masque la fenetre qu'une fois les volets sortis de l'ecran :
     * la masquer tout de suite escamoterait l'animation. */
    if (!T.ouvert && T.fenetre != NULL)
        gtk_widget_set_visible (T.fenetre, FALSE);
    return G_SOURCE_REMOVE;
}

void
shell_tiroir_ouvrir (void)
{
    if (T.fenetre == NULL || T.ouvert)
        return;

    if (T.retrait != 0) {
        g_source_remove (T.retrait);
        T.retrait = 0;
    }
    T.ouvert = TRUE;

    /* LA FENETRE NE CHANGE JAMAIS DE TAILLE : elle est plein ecran des sa
     * creation, et c'est le reveleur qui bouge. Redimensionner une surface
     * layer-shell qui porte un popover ouvert fait partir ce popover hors de
     * l'ecran sous labwc 0.8.3 -- regle payee le 11 septembre 2026, et la
     * Console du volet bas ouvre des popovers. */
    gtk_widget_set_visible (T.fenetre, TRUE);
    gtk_revealer_set_reveal_child (GTK_REVEALER (T.reveleur), TRUE);
}

void
shell_tiroir_fermer (gpointer inutilise)
{
    (void) inutilise;
    if (T.fenetre == NULL || !T.ouvert)
        return;

    T.ouvert = FALSE;
    gtk_revealer_set_reveal_child (GTK_REVEALER (T.reveleur), FALSE);

    if (T.retrait != 0)
        g_source_remove (T.retrait);
    T.retrait = g_timeout_add (GLISSE_MS + 40, retrait_fin, NULL);
}

void
shell_tiroir_basculer (void)
{
    if (T.ouvert)
        shell_tiroir_fermer (NULL);
    else
        shell_tiroir_ouvrir ();
}

/* -------------------------------------------------------------------------
 * La bande du bord
 * ------------------------------------------------------------------------- */
static void
attente_annuler (void)
{
    if (T.attente != 0) {
        g_source_remove (T.attente);
        T.attente = 0;
    }
}

static gboolean
attente_echue (gpointer data)
{
    (void) data;
    T.attente = 0;
    shell_tiroir_ouvrir ();
    return G_SOURCE_REMOVE;
}

/* LE POINTEUR POSE, ET NON LE POINTEUR QUI PASSE.
 *
 * « enter » suffirait a armer la minuterie, mais un curseur qui traverse la
 * bande en diagonale la declencherait aussi. On rearme donc a chaque
 * mouvement DANS la bande : tant que le curseur bouge, le compte repart de
 * zero, et il ne s'achève que s'il s'immobilise contre le bord. */
static void
on_bande_entree (GtkEventControllerMotion *c, double x, double y, gpointer d)
{
    (void) c; (void) x; (void) y; (void) d;
    if (T.ouvert)
        return;
    attente_annuler ();
    T.attente = g_timeout_add (ATTENTE_MS, attente_echue, NULL);
}

static void
on_bande_mouvement (GtkEventControllerMotion *c, double x, double y, gpointer d)
{
    on_bande_entree (c, x, y, d);
}

static void
on_bande_sortie (GtkEventControllerMotion *c, gpointer d)
{
    (void) c; (void) d;
    attente_annuler ();
}

/* Le glisser du doigt : vers la GAUCHE, depuis le bord droit. Le signe
 * compte -- un glisser vers la droite depuis la bande n'a aucun sens et ne
 * doit rien ouvrir. */
static void
on_bande_glisse (GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    (void) g; (void) dy; (void) d;
    if (!T.ouvert && dx <= -SEUIL_PX) {
        attente_annuler ();
        shell_tiroir_ouvrir ();
    }
}

static void
bande_creer (GtkApplication *app)
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
    gtk_layer_set_namespace (GTK_WINDOW (bande), "claude-os-lisiere");
    gtk_layer_set_anchor (GTK_WINDOW (bande), GTK_LAYER_SHELL_EDGE_RIGHT,  TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (bande), GTK_LAYER_SHELL_EDGE_TOP,    TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (bande), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_exclusive_zone (GTK_WINDOW (bande), -1);
    gtk_layer_set_keyboard_mode (GTK_WINDOW (bande),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GtkWidget *plage = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request (plage, BANDE_PX, -1);
    gtk_window_set_child (GTK_WINDOW (bande), plage);

    GtkGesture *g = gtk_gesture_drag_new ();
    g_signal_connect (g, "drag-update", G_CALLBACK (on_bande_glisse), NULL);
    gtk_widget_add_controller (bande, GTK_EVENT_CONTROLLER (g));

    GtkEventController *m = gtk_event_controller_motion_new ();
    g_signal_connect (m, "enter",  G_CALLBACK (on_bande_entree),    NULL);
    g_signal_connect (m, "motion", G_CALLBACK (on_bande_mouvement), NULL);
    g_signal_connect (m, "leave",  G_CALLBACK (on_bande_sortie),    NULL);
    gtk_widget_add_controller (bande, m);

    gtk_window_present (GTK_WINDOW (bande));
    T.bande = bande;
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

void
shell_tiroir_init (GtkApplication *app, GtkWidget *console, gboolean apercu)
{
    (void) apercu;
    if (T.fenetre != NULL)
        return;

    bande_creer (app);

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
     * surface. Voir shell_tiroir_ouvrir(). */
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

    /* --- les deux volets --- */
    GtkWidget *haut = volet ("tiroir-widgets");
    GtkWidget *mot = gtk_label_new ("Widget\nà venir");
    gtk_label_set_justify (GTK_LABEL (mot), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class (mot, "tiroir-attente");
    gtk_widget_set_vexpand (mot, TRUE);
    gtk_widget_set_valign (mot, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (mot, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (haut), mot);

    GtkWidget *bas = volet ("tiroir-console");
    /* La Console se pose en haut de son volet : etiree, elle laisserait
     * l'alimentation flotter au bas d'un grand vide. */
    gtk_widget_set_valign (console, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (bas), console);

    GtkWidget *colonne = gtk_box_new (GTK_ORIENTATION_VERTICAL, MARGE);
    gtk_widget_add_css_class (colonne, "tiroir-colonne");
    gtk_widget_set_valign (colonne, GTK_ALIGN_FILL);
    gtk_widget_set_halign (colonne, GTK_ALIGN_END);
    gtk_widget_set_vexpand (haut, TRUE);
    gtk_box_append (GTK_BOX (colonne), haut);
    gtk_box_append (GTK_BOX (colonne), bas);

    T.reveleur = gtk_revealer_new ();
    gtk_revealer_set_child (GTK_REVEALER (T.reveleur), colonne);
    /* SLIDE_LEFT : les volets entrent par la droite. C'est le geste qu'on
     * vient de faire, joue a l'envers -- rien d'autre ne se lit aussi vite
     * comme « ce que tu as tire vient de la ». */
    gtk_revealer_set_transition_type (GTK_REVEALER (T.reveleur),
                                      GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_transition_duration (GTK_REVEALER (T.reveleur), GLISSE_MS);
    gtk_revealer_set_reveal_child (GTK_REVEALER (T.reveleur), FALSE);
    gtk_widget_set_halign (T.reveleur, GTK_ALIGN_END);

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
    gtk_overlay_add_overlay (GTK_OVERLAY (superpose), T.reveleur);

    gtk_window_set_child (GTK_WINDOW (fenetre), superpose);
    /* Cree masque : le tiroir est ferme a l'ouverture de session. */
    gtk_widget_set_visible (fenetre, FALSE);
}
