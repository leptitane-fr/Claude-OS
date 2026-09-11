/* =========================================================================
 * Claude OS — le clavier à l'écran de la session. Voir clavier-ecran.h.
 * ========================================================================= */
#include "clavier-ecran.h"

#include <gtk4-layer-shell.h>

#include "clavier.h"
#include "saisie.h"

/* La grille fait vingt colonnes, comme celle de l'écran de connexion : une
 * touche ordinaire en occupe deux, une touche de commande trois, et toutes
 * les rangées tombent juste. */
#define COLONNES 20
#define ORD 2
#define CMD 3

/* Au-delà, les touches s'élargissent sans rien gagner : 1920 px pour dix
 * touches font 31 mm par touche sur cette dalle de 310 mm, et le doigt
 * voyage pour rien. 1440 px, c'est 23 mm — la largeur d'une touche de
 * tablette de 13 pouces. Le fond, lui, couvre toute la largeur. */
#define LARGEUR_MAX 1440

/* Un champ qui perd le focus pour un autre en gagne un dans le même
 * instant, mais pas toujours dans le même lot d'événements. Sans ce délai,
 * le clavier clignoterait à chaque passage d'un champ au suivant. */
#define DELAI_MASQUER_MS 150

/* Deux appuis sur Maj dans cet intervalle la verrouillent. */
#define DOUBLE_APPUI_US (350 * 1000)

/* Répétition des touches tenues : ⌫ et les flèches. Le délai est celui de
 * l'appui long de GTK ; la cadence, celle d'un clavier physique. */
#define REPETITION_MS 55

typedef enum {
    T_TEXTE,
    T_MAJ,
    T_EFFACER,
    T_ENTREE,
    T_SYMBOLES,     /* vers la couche ?123       */
    T_LETTRES,      /* retour aux lettres        */
    T_GAUCHE,
    T_DROITE,
    T_TABULATION,
    T_MASQUER,
} Genre;

typedef struct {
    const char *etiquette;
    const char *texte;      /* NULL : l'étiquette fait office de texte */
    Genre       genre;
    int         largeur;
} Touche;

#define FIN {NULL, NULL, T_TEXTE, 0}
#define L(c) {c, NULL, T_TEXTE, ORD}

/* --- Les lettres ----------------------------------------------------------
 * L'apostrophe à la place du tiret de l'écran de connexion : « l'été »,
 * « aujourd'hui » — c'est le signe le plus fréquent du français après le
 * point et la virgule. */
static const Touche A0[] = { L("é"),L("è"),L("à"),L("ç"),L("ù"),
                             L("ê"),L("â"),L("î"),L("ô"),L("û"), FIN };
static const Touche A1[] = { L("a"),L("z"),L("e"),L("r"),L("t"),
                             L("y"),L("u"),L("i"),L("o"),L("p"), FIN };
static const Touche A2[] = { L("q"),L("s"),L("d"),L("f"),L("g"),
                             L("h"),L("j"),L("k"),L("l"),L("m"), FIN };
static const Touche A3[] = { {"⇧",NULL,T_MAJ,CMD},
                             L("w"),L("x"),L("c"),L("v"),L("b"),L("n"),L("'"),
                             {"⌫",NULL,T_EFFACER,CMD}, FIN };
static const Touche A4[] = { {"?123",NULL,T_SYMBOLES,CMD}, L(","),
                             {"espace"," ",T_TEXTE,10},
                             L("."), {"↵",NULL,T_ENTREE,CMD}, FIN };

/* --- Les symboles ---------------------------------------------------------
 * Les chiffres en tête, puisque la rangée d'accents a pris leur place chez
 * les lettres. Les guillemets français à côté de l'espace : on les ouvre et
 * les ferme en l'encadrant. */
static const Touche B0[] = { L("1"),L("2"),L("3"),L("4"),L("5"),
                             L("6"),L("7"),L("8"),L("9"),L("0"), FIN };
static const Touche B1[] = { L("@"),L("#"),L("€"),L("_"),L("&"),
                             L("-"),L("+"),L("("),L(")"),L("/"), FIN };
static const Touche B2[] = { L("*"),L("\""),L("'"),L(":"),L(";"),
                             L("!"),L("?"),L("="),L("%"),L("$"), FIN };
static const Touche B3[] = { L("\\"),L("|"),L("<"),L(">"),L("{"),
                             L("}"),L("["),L("]"),L("~"),
                             {"⌫",NULL,T_EFFACER,ORD}, FIN };
static const Touche B4[] = { {"abc",NULL,T_LETTRES,CMD}, L("«"),
                             {"espace"," ",T_TEXTE,10},
                             L("»"), {"↵",NULL,T_ENTREE,CMD}, FIN };

static const Touche *const LETTRES[]  = { A0, A1, A2, A3, A4, NULL };
static const Touche *const SYMBOLES[] = { B0, B1, B2, B3, B4, NULL };

typedef enum { MAJ_NON, MAJ_UNE, MAJ_VERROU } EtatMaj;

static struct {
    GtkWidget *fenetre;
    GtkWidget *interieur;       /* la largeur bornée                     */
    GtkWidget *pile;            /* lettres / symboles / chiffres         */
    GPtrArray *lettres;         /* boutons dont l'étiquette suit Maj     */
    GPtrArray *boutons_maj;

    gboolean   ecrit;           /* le clavier virtuel est branché        */
    gboolean   tablette;
    gboolean   champ;           /* un champ de texte a le focus          */
    gboolean   renvoye;         /* ⌄ appuyé depuis le dernier champ      */
    gboolean   demande;         /* montré sur demande                    */
    gboolean   visible;
    guint      masquage;        /* délai avant de masquer                */

    EtatMaj    maj;
    gint64     dernier_maj;

    Genre      repete;          /* touche tenue                          */
    guint      repetition;
} K;

/* -------------------------------------------------------------------------
 * Maj
 * ------------------------------------------------------------------------- */
static void
maj_afficher (void)
{
    for (guint i = 0; i < K.lettres->len; i++) {
        GtkWidget *b = g_ptr_array_index (K.lettres, i);
        const char *base = g_object_get_data (G_OBJECT (b), "lettre");
        g_autofree char *haut = K.maj != MAJ_NON ? g_utf8_strup (base, -1) : NULL;
        gtk_button_set_label (GTK_BUTTON (b), haut != NULL ? haut : base);
    }
    for (guint i = 0; i < K.boutons_maj->len; i++) {
        GtkWidget *b = g_ptr_array_index (K.boutons_maj, i);
        if (K.maj != MAJ_NON) gtk_widget_add_css_class (b, "active");
        else                  gtk_widget_remove_css_class (b, "active");
        if (K.maj == MAJ_VERROU) gtk_widget_add_css_class (b, "verrouille");
        else                     gtk_widget_remove_css_class (b, "verrouille");
    }
}

static void
maj_poser (EtatMaj etat)
{
    if (K.maj == etat)
        return;
    K.maj = etat;
    maj_afficher ();
}

/* Une touche frappée : Maj « une lettre » retombe. */
static void
maj_consommer (void)
{
    if (K.maj == MAJ_UNE)
        maj_poser (MAJ_NON);
}

/* -------------------------------------------------------------------------
 * Les actions
 * ------------------------------------------------------------------------- */
static void appliquer (void);

static void
masquer_sur_demande (void)
{
    K.demande = FALSE;
    K.renvoye = TRUE;
    appliquer ();
}

static void
frapper (const Touche *t)
{
    switch (t->genre) {
    case T_TEXTE: {
        const char *texte = t->texte != NULL ? t->texte : t->etiquette;
        g_autofree char *haut = K.maj != MAJ_NON ? g_utf8_strup (texte, -1) : NULL;
        shell_saisie_texte (haut != NULL ? haut : texte);
        maj_consommer ();
        break;
    }
    case T_MAJ: {
        /* Un appui : une lettre. Deux appuis rapprochés : verrou. Un appui
         * sur Maj verrouillée, ou enclenchée depuis longtemps : relâche. */
        gint64 maintenant = g_get_monotonic_time ();
        if (K.maj == MAJ_NON)
            maj_poser (MAJ_UNE);
        else if (K.maj == MAJ_UNE && maintenant - K.dernier_maj < DOUBLE_APPUI_US)
            maj_poser (MAJ_VERROU);
        else
            maj_poser (MAJ_NON);
        K.dernier_maj = maintenant;
        break;
    }
    case T_EFFACER:
        shell_saisie_touche (SHELL_TOUCHE_EFFACER, FALSE);
        break;
    case T_ENTREE:
        /* Maj+Entrée est le retour à la ligne de Claude Desktop, et de bien
         * des messageries : Maj enclenchée l'envoie comme tel. */
        shell_saisie_touche (SHELL_TOUCHE_ENTREE, K.maj != MAJ_NON);
        maj_consommer ();
        break;
    case T_SYMBOLES:
        gtk_stack_set_visible_child_name (GTK_STACK (K.pile), "symboles");
        break;
    case T_LETTRES:
        gtk_stack_set_visible_child_name (GTK_STACK (K.pile), "lettres");
        break;
    case T_GAUCHE:
        shell_saisie_touche (SHELL_TOUCHE_GAUCHE, FALSE);
        break;
    case T_DROITE:
        shell_saisie_touche (SHELL_TOUCHE_DROITE, FALSE);
        break;
    case T_TABULATION:
        shell_saisie_touche (SHELL_TOUCHE_TABULATION, FALSE);
        break;
    case T_MASQUER:
        masquer_sur_demande ();
        break;
    }
}

static void
on_clic (GtkButton *b, gpointer data)
{
    (void) b;
    frapper (data);
}

/* --- La répétition des touches tenues ----------------------------------- */
static void
arreter_repetition (void)
{
    if (K.repetition != 0) {
        g_source_remove (K.repetition);
        K.repetition = 0;
    }
}

static gboolean
on_repetition (gpointer data)
{
    frapper (data);
    return G_SOURCE_CONTINUE;
}

static void
on_tenue (GtkGestureLongPress *g, double x, double y, gpointer data)
{
    (void) x; (void) y;
    /* Réclamée : le clic du bouton est annulé, et le relâcher ne frappera
     * pas une fois de plus — ce que la répétition a déjà fait. */
    gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
    arreter_repetition ();
    frapper (data);
    K.repetition = g_timeout_add (REPETITION_MS, on_repetition, data);
}

static void
on_tenue_finie (GtkGesture *g, GdkEventSequence *s, gpointer data)
{
    (void) g; (void) s; (void) data;
    arreter_repetition ();
}

/* -------------------------------------------------------------------------
 * Le dessin
 * ------------------------------------------------------------------------- */
static gboolean
est_lettre (const Touche *t)
{
    if (t->genre != T_TEXTE || t->texte != NULL)
        return FALSE;
    gunichar c = g_utf8_get_char (t->etiquette);
    return g_unichar_islower (c) && *g_utf8_next_char (t->etiquette) == '\0';
}

static GtkWidget *
touche_neuve (const Touche *t)
{
    GtkWidget *b = gtk_button_new_with_label (t->etiquette);

    /* Ceinture et bretelles. La surface ne prend jamais le clavier
     * (KEYBOARD_MODE_NONE), mais un bouton qui voudrait le focus ne doit
     * pas pouvoir le demander — voir clavier.c, qui l'a payé. */
    gtk_widget_set_can_focus (b, FALSE);
    gtk_widget_set_focus_on_click (b, FALSE);
    gtk_widget_add_css_class (b, "clavier-touche");
    if (t->genre != T_TEXTE)
        gtk_widget_add_css_class (b, "speciale");
    if (t->genre == T_ENTREE)
        gtk_widget_add_css_class (b, "valider");
    if (t->genre == T_TEXTE && t->texte != NULL && g_str_equal (t->texte, " "))
        gtk_widget_add_css_class (b, "espace");
    gtk_widget_set_hexpand (b, TRUE);
    gtk_widget_set_vexpand (b, TRUE);

    /* Les tables sont statiques : elles survivent aux boutons. */
    g_signal_connect (b, "clicked", G_CALLBACK (on_clic), (gpointer) t);

    if (t->genre == T_EFFACER || t->genre == T_GAUCHE || t->genre == T_DROITE) {
        GtkGesture *tenue = gtk_gesture_long_press_new ();
        g_signal_connect (tenue, "pressed", G_CALLBACK (on_tenue), (gpointer) t);
        g_signal_connect (tenue, "end", G_CALLBACK (on_tenue_finie), NULL);
        g_signal_connect (tenue, "cancel", G_CALLBACK (on_tenue_finie), NULL);
        gtk_widget_add_controller (b, GTK_EVENT_CONTROLLER (tenue));
    }

    if (est_lettre (t)) {
        g_object_set_data (G_OBJECT (b), "lettre", (gpointer) t->etiquette);
        g_ptr_array_add (K.lettres, b);
    }
    if (t->genre == T_MAJ)
        g_ptr_array_add (K.boutons_maj, b);
    return b;
}

static GtkWidget *
couche (const Touche *const rangees[])
{
    GtkWidget *grille = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grille), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grille), 8);
    gtk_grid_set_row_homogeneous (GTK_GRID (grille), TRUE);
    gtk_grid_set_column_homogeneous (GTK_GRID (grille), TRUE);

    for (int y = 0; rangees[y] != NULL; y++) {
        int x = 0;
        for (const Touche *t = rangees[y]; t->etiquette != NULL; t++) {
            gtk_grid_attach (GTK_GRID (grille), touche_neuve (t), x, y, t->largeur, 1);
            x += t->largeur;
        }
        g_warn_if_fail (x == COLONNES);   /* une rangée bancale se voit tout de suite */
    }
    return grille;
}

/* La barre du haut : les flèches et la tabulation — corriger une faute sans
 * viser au doigt entre deux lettres — et ⌄ pour renvoyer le clavier. */
static const Touche BARRE[] = {
    {"←", NULL, T_GAUCHE, 0}, {"→", NULL, T_DROITE, 0},
    {"⇥", NULL, T_TABULATION, 0}, {"⌄", NULL, T_MASQUER, 0},
};

static GtkWidget *
barre (void)
{
    GtkWidget *b = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class (b, "clavier-barre");
    for (guint i = 0; i < G_N_ELEMENTS (BARRE); i++) {
        GtkWidget *t = touche_neuve (&BARRE[i]);
        gtk_widget_set_hexpand (t, FALSE);
        gtk_widget_set_size_request (t, 96, -1);
        if (BARRE[i].genre == T_MASQUER) {
            GtkWidget *vide = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_set_hexpand (vide, TRUE);
            gtk_box_append (GTK_BOX (b), vide);
            gtk_widget_set_tooltip_text (t, "Masquer le clavier");
        }
        gtk_box_append (GTK_BOX (b), t);
    }
    return b;
}

/* --- Les chiffres : le pavé de l'écran de connexion ----------------------- */
static const Touche ENTREE_PAVE  = {"↵",   NULL, T_ENTREE,  0};
static const Touche LETTRES_PAVE = {"abc", NULL, T_LETTRES, 0};

static void
on_pave (char c, gpointer data)
{
    (void) data;
    if (c == '\b') {
        shell_saisie_touche (SHELL_TOUCHE_EFFACER, FALSE);
    } else {
        char texte[2] = { c, '\0' };
        shell_saisie_texte (texte);
    }
}

static GtkWidget *
chiffres (void)
{
    GtkWidget *rangee = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign (rangee, GTK_ALIGN_CENTER);

    GtkWidget *pave = shell_clavier_pave (on_pave, NULL);
    gtk_widget_set_size_request (pave, 480, -1);
    gtk_box_append (GTK_BOX (rangee), pave);

    GtkWidget *cote = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class (cote, "clavier-pave");   /* même marge que le pavé */
    GtkWidget *abc = touche_neuve (&LETTRES_PAVE);
    GtkWidget *ok  = touche_neuve (&ENTREE_PAVE);
    gtk_widget_set_size_request (abc, 140, -1);
    gtk_box_append (GTK_BOX (cote), abc);
    gtk_box_append (GTK_BOX (cote), ok);
    gtk_box_append (GTK_BOX (rangee), cote);
    return rangee;
}

/* -------------------------------------------------------------------------
 * À l'écran ou non
 * ------------------------------------------------------------------------- */
static void
ajuster_largeur (void)
{
    int largeur = LARGEUR_MAX;
    GListModel *moniteurs = gdk_display_get_monitors (gdk_display_get_default ());
    for (guint i = 0; i < g_list_model_get_n_items (moniteurs); i++) {
        g_autoptr(GdkMonitor) m = g_list_model_get_item (moniteurs, i);
        const char *c = gdk_monitor_get_connector (m);
        if (c != NULL && g_str_has_prefix (c, "eDP")) {
            GdkRectangle g;
            gdk_monitor_get_geometry (m, &g);
            largeur = MIN (largeur, g.width - 16);
            break;
        }
    }
    gtk_widget_set_size_request (K.interieur, largeur, -1);
}

static void
appliquer (void)
{
    gboolean voir = K.ecrit && K.tablette && ((K.champ && !K.renvoye) || K.demande);
    if (voir == K.visible)
        return;
    K.visible = voir;

    if (voir) {
        ajuster_largeur ();
        gtk_window_present (GTK_WINDOW (K.fenetre));
    } else {
        arreter_repetition ();
        gtk_widget_set_visible (K.fenetre, FALSE);
        /* On revient aux lettres, Maj relâchée : le prochain champ ne doit
         * pas hériter de la couche ni de la majuscule du précédent. */
        gtk_stack_set_visible_child_name (GTK_STACK (K.pile), "lettres");
        maj_poser (MAJ_NON);
    }
    g_debug ("clavier à l'écran : %s", voir ? "montré" : "masqué");
}

static gboolean
on_masquage (gpointer data)
{
    (void) data;
    K.masquage = 0;
    K.champ = FALSE;
    /* Montré sur demande puis le champ perdu : on range aussi. Pour une
     * application qui ne signale pas ses champs, ce délai ne vient jamais,
     * et la demande tient jusqu'à ⌄. */
    K.demande = FALSE;
    appliquer ();
    return G_SOURCE_REMOVE;
}

static void
on_saisie (gboolean actif, ShellSaisieBut but, gpointer data)
{
    (void) data;
    if (!actif) {
        if (K.masquage == 0)
            K.masquage = g_timeout_add (DELAI_MASQUER_MS, on_masquage, NULL);
        return;
    }

    if (K.masquage != 0) {
        g_source_remove (K.masquage);
        K.masquage = 0;
    }
    K.champ = TRUE;
    K.renvoye = FALSE;

    /* Un champ qui attend des chiffres ouvre le pavé ; tout autre revient
     * aux lettres s'il héritait du pavé. */
    const char *voulue = but == SHELL_SAISIE_CHIFFRES ? "chiffres" : NULL;
    const char *courante = gtk_stack_get_visible_child_name (GTK_STACK (K.pile));
    if (voulue != NULL)
        gtk_stack_set_visible_child_name (GTK_STACK (K.pile), voulue);
    else if (g_strcmp0 (courante, "chiffres") == 0)
        gtk_stack_set_visible_child_name (GTK_STACK (K.pile), "lettres");

    appliquer ();
}

/* -------------------------------------------------------------------------
 * L'interface publique
 * ------------------------------------------------------------------------- */

/* Tout ce que le clavier peut écrire, pour la disposition fabriquée : les
 * textes des tables, leurs majuscules, et les chiffres du pavé. */
static GPtrArray *
tous_les_textes (void)
{
    GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
    const Touche *const *couches[] = { LETTRES, SYMBOLES };
    for (guint c = 0; c < G_N_ELEMENTS (couches); c++)
        for (int y = 0; couches[c][y] != NULL; y++)
            for (const Touche *t = couches[c][y]; t->etiquette != NULL; t++) {
                if (t->genre != T_TEXTE)
                    continue;
                g_ptr_array_add (a, g_strdup (t->texte != NULL ? t->texte : t->etiquette));
                if (est_lettre (t))
                    g_ptr_array_add (a, g_utf8_strup (t->etiquette, -1));
            }
    g_ptr_array_add (a, NULL);
    return a;
}

void
shell_clavier_ecran_init (GtkApplication *app)
{
    K.lettres = g_ptr_array_new ();
    K.boutons_maj = g_ptr_array_new ();

    K.fenetre = gtk_window_new ();
    gtk_window_set_application (GTK_WINDOW (K.fenetre), app);
    gtk_widget_add_css_class (K.fenetre, "shell");

    gtk_layer_init_for_window (GTK_WINDOW (K.fenetre));
    /* OVERLAY, comme le dock : labwc éteint la couche TOP sous une fenêtre
     * plein écran (vu au banc, voir CLAUDE.md), et un champ peut vivre dans
     * une fenêtre plein écran. */
    gtk_layer_set_layer (GTK_WINDOW (K.fenetre), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor (GTK_WINDOW (K.fenetre), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (K.fenetre), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (K.fenetre), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_namespace (GTK_WINDOW (K.fenetre), "claude-os-clavier");
    /* Zone réservée à la hauteur du clavier : les fenêtres agrandies
     * raccourcissent, et le champ où l'on écrit reste au-dessus. */
    gtk_layer_auto_exclusive_zone_enable (GTK_WINDOW (K.fenetre));
    /* LA ligne qui compte : le clavier ne prend JAMAIS le focus. Sans elle,
     * le premier appui enlèverait le focus au champ où l'on écrit. */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (K.fenetre),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GtkWidget *fond = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (fond, "clavier-ecran");

    K.interieur = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_halign (K.interieur, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (K.interieur), barre ());

    K.pile = gtk_stack_new ();
    gtk_widget_add_css_class (K.pile, "clavier");
    gtk_stack_add_named (GTK_STACK (K.pile), couche (LETTRES), "lettres");
    gtk_stack_add_named (GTK_STACK (K.pile), couche (SYMBOLES), "symboles");
    gtk_stack_add_named (GTK_STACK (K.pile), chiffres (), "chiffres");
    gtk_stack_set_visible_child_name (GTK_STACK (K.pile), "lettres");
    gtk_box_append (GTK_BOX (K.interieur), K.pile);

    gtk_box_append (GTK_BOX (fond), K.interieur);
    gtk_window_set_child (GTK_WINDOW (K.fenetre), fond);

    g_autoptr(GPtrArray) textes = tous_les_textes ();
    K.ecrit = shell_saisie_init ((const char *const *) textes->pdata, on_saisie, NULL);
}

void
shell_clavier_ecran_tablette (gboolean tablette)
{
    K.tablette = tablette;
    if (!tablette) {
        K.demande = FALSE;
        K.renvoye = FALSE;
    }
    appliquer ();
}

void
shell_clavier_ecran_basculer (void)
{
    if (K.visible) {
        masquer_sur_demande ();
    } else {
        K.demande = TRUE;
        K.renvoye = FALSE;
        appliquer ();
    }
}

gboolean
shell_clavier_ecran_visible (void)
{
    return K.visible;
}
