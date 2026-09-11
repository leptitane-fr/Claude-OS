/* =========================================================================
 * Claude OS — le clavier à l'écran de la session. Voir clavier-ecran.h.
 * ========================================================================= */
#include "clavier-ecran.h"

#include <errno.h>
#include <gtk4-layer-shell.h>

#include "clavier.h"
#include "saisie.h"

/* Un champ qui perd le focus pour un autre en gagne un dans le même
 * instant, mais pas toujours dans le même lot d'événements. Sans ce délai,
 * le clavier clignoterait à chaque passage d'un champ au suivant. Mesuré
 * au journal le 11 septembre 2026 : masqué 150 ms après la perte. */
#define DELAI_MASQUER_MS 150

/* Deux appuis sur Maj dans cet intervalle la verrouillent. */
#define DOUBLE_APPUI_US (350 * 1000)

/* Répétition des touches tenues : ⌫ et les flèches. Le délai est celui de
 * l'appui long de GTK ; la cadence, celle d'un clavier physique. */
#define REPETITION_MS 55

/* LE PLEIN FORMAT A L'ÉCARTEMENT D'UN VRAI CLAVIER. Une rangée AZERTY fait
 * quinze unités (douze touches et ⌫ double) ; à 120 px l'unité, 1800 px :
 * sur cette dalle de 310 mm pour 1920 px, 19,4 mm par touche — l'écartement
 * normalisé d'un clavier physique, que l'utilisateur voulait retrouver. */
#define LARGEUR_PLEIN 1800

/* Le mode à deux mains : les lettres sous le pouce gauche, les chiffres
 * sous le droit, et le milieu de l'écran libre. Dix touches en 760 px, 12 mm
 * par touche : un clavier de téléphone un peu large, pour un pouce. */
#define LARGEUR_GAUCHE 760
#define LARGEUR_DROITE 400

/* =========================================================================
 * Les touches
 * ========================================================================= */
typedef enum {
    T_TEXTE,        /* écrit base, ou maj avec Maj                          */
    T_MORTE,        /* ^ ou ¨ : attend la lettre suivante                   */
    T_ESPACE,
    T_MAJ,          /* ⇧ : une lettre ; double appui : verrou               */
    T_VERR_MAJ,     /* ⇪ : les lettres seulement, comme sur le physique     */
    T_EFFACER,
    T_ENTREE,
    T_TABULATION,
    T_GAUCHE,
    T_DROITE,
    T_COUCHE,       /* vers la couche nommée par « maj »                    */
    T_MODE,         /* plein format <-> deux mains                          */
    T_MASQUER,
    T_VIDE,         /* une place sans touche : sous ↵, qui tient 2 rangées  */
} Genre;

typedef struct {
    const char *base;      /* ce qu'écrit la touche, et son étiquette        */
    const char *maj;       /* avec Maj ; NULL : la majuscule de base, s'il y
                              en a une. Pour T_COUCHE : la couche visée.     */
    Genre       genre;
    int         largeur;   /* en colonnes de la grille                       */
    int         hauteur;   /* en rangées ; 0 vaut 1                          */
} Touche;

#define FIN          {NULL, NULL, T_TEXTE, 0, 0}
#define L(c)         {c, NULL, T_TEXTE, 2, 0}
#define D(c, m)      {c, m, T_TEXTE, 2, 0}

/* --- Plein format : le clavier physique ----------------------------------
 *
 * Choisi par l'utilisateur le 11 septembre 2026 : « comme sur un clavier
 * physique », la rangée &é"'(-è_çà)= avec les chiffres sous Maj, et ^ ¨ en
 * touches mortes — c'est ainsi qu'il écrit ê et ë. Grille de trente
 * colonnes : une touche vaut deux colonnes, une unité de clavier. Seul le ²
 * du coin a été laissé : il n'a pas sa place sur un écran qu'on tape. */
static const Touche P0[] = {
    D("&","1"), D("é","2"), D("\"","3"), D("'","4"), D("(","5"), D("-","6"),
    D("è","7"), D("_","8"), D("ç","9"), D("à","0"), D(")","°"), D("=","+"),
    {"⌫", NULL, T_EFFACER, 6, 0}, FIN };
static const Touche P1[] = {
    {"⇥", NULL, T_TABULATION, 3, 0},
    L("a"), L("z"), L("e"), L("r"), L("t"), L("y"), L("u"), L("i"), L("o"), L("p"),
    {"^", "¨", T_MORTE, 2, 0}, D("$","£"),
    {"↵", NULL, T_ENTREE, 3, 2}, FIN };
static const Touche P2[] = {
    {"⇪", NULL, T_VERR_MAJ, 3, 0},
    L("q"), L("s"), L("d"), L("f"), L("g"), L("h"), L("j"), L("k"), L("l"), L("m"),
    D("ù","%"), D("*","µ"),
    {"", NULL, T_VIDE, 3, 0}, FIN };
static const Touche P3[] = {
    {"⇧", NULL, T_MAJ, 3, 0}, D("<",">"),
    L("w"), L("x"), L("c"), L("v"), L("b"), L("n"),
    D(",","?"), D(";","."), D(":","/"), D("!","§"),
    {"⇧", NULL, T_MAJ, 5, 0}, FIN };
static const Touche P4[] = {
    {"&@#", "altgr", T_COUCHE, 4, 0}, {"←", NULL, T_GAUCHE, 3, 0},
    {"espace", NULL, T_ESPACE, 13, 0},
    {"→", NULL, T_DROITE, 3, 0}, {"⇆", NULL, T_MODE, 3, 0},
    {"⌄", NULL, T_MASQUER, 4, 0}, FIN };

/* --- Plein format : ce que donne AltGr sur le physique, et au-delà ------- */
static const Touche Q0[] = {
    L("~"), L("#"), L("{"), L("["), L("|"), L("`"), L("\\"), L("^"), L("@"),
    L("]"), L("}"), L("€"), {"⌫", NULL, T_EFFACER, 6, 0}, FIN };
static const Touche Q1[] = {
    {"⇥", NULL, T_TABULATION, 3, 0},
    L("«"), L("»"), L("“"), L("”"), L("‘"), L("’"), L("–"), L("—"), L("…"),
    L("•"), L("°"), L("²"),
    {"↵", NULL, T_ENTREE, 3, 2}, FIN };
static const Touche Q2[] = {
    {"abc", "lettres", T_COUCHE, 3, 0},
    L("œ"), L("Œ"), L("æ"), L("Æ"), L("ß"), L("ñ"), L("Ñ"), L("¿"), L("¡"),
    L("±"), L("×"), L("÷"),
    {"", NULL, T_VIDE, 3, 0}, FIN };
static const Touche Q3[] = {
    L("¤"), L("≠"), L("≤"), L("≥"), L("¼"), L("½"), L("¾"), L("©"), L("®"),
    L("™"), L("¶"), L("¥"), L("¢"), L("‰"), L("³"), FIN };
static const Touche Q4[] = {
    {"abc", "lettres", T_COUCHE, 4, 0}, {"←", NULL, T_GAUCHE, 3, 0},
    {"espace", NULL, T_ESPACE, 13, 0},
    {"→", NULL, T_DROITE, 3, 0}, {"⇆", NULL, T_MODE, 3, 0},
    {"⌄", NULL, T_MASQUER, 4, 0}, FIN };

/* --- Deux mains, à gauche : un clavier de téléphone ----------------------
 *
 * La première disposition, celle qui a été vue fonctionner : rangée
 * d'accents fixe, apostrophe à la place du tiret — « l'été ». Vingt
 * colonnes. */
static const Touche G0[] = { L("é"),L("è"),L("à"),L("ç"),L("ù"),
                             L("ê"),L("â"),L("î"),L("ô"),L("û"), FIN };
static const Touche G1[] = { L("a"),L("z"),L("e"),L("r"),L("t"),
                             L("y"),L("u"),L("i"),L("o"),L("p"), FIN };
static const Touche G2[] = { L("q"),L("s"),L("d"),L("f"),L("g"),
                             L("h"),L("j"),L("k"),L("l"),L("m"), FIN };
static const Touche G3[] = { {"⇧", NULL, T_MAJ, 3, 0},
                             L("w"),L("x"),L("c"),L("v"),L("b"),L("n"),L("'"),
                             {"⌫", NULL, T_EFFACER, 3, 0}, FIN };
static const Touche G4[] = { {"?123", "symboles", T_COUCHE, 3, 0}, L(","),
                             {"espace", NULL, T_ESPACE, 10, 0},
                             L("."), {"↵", NULL, T_ENTREE, 3, 0}, FIN };

static const Touche H0[] = { L("@"),L("#"),L("€"),L("_"),L("&"),
                             L("-"),L("+"),L("("),L(")"),L("/"), FIN };
static const Touche H1[] = { L("*"),L("\""),L("'"),L(":"),L(";"),
                             L("!"),L("?"),L("="),L("%"),L("$"), FIN };
static const Touche H2[] = { L("\\"),L("|"),L("<"),L(">"),L("{"),
                             L("}"),L("["),L("]"),L("~"),L("°"), FIN };
static const Touche H3[] = { L("«"),L("»"),L("^"),L("`"),L("£"),
                             L("§"),L("µ"),L("œ"),L("…"),
                             {"⌫", NULL, T_EFFACER, 2, 0}, FIN };
static const Touche H4[] = { {"abc", "lettres", T_COUCHE, 3, 0}, L(","),
                             {"espace", NULL, T_ESPACE, 10, 0},
                             L("."), {"↵", NULL, T_ENTREE, 3, 0}, FIN };

/* --- Deux mains, à droite : le pavé numérique ----------------------------
 *
 * Disposition du pavé d'un clavier — 7 en haut — et non celle d'un
 * téléphone : c'est un pavé de calcul. Il porte aussi les commandes du
 * clavier, pour que la main gauche n'ait que des lettres. Huit colonnes. */
static const Touche N0[] = { {"←", NULL, T_GAUCHE, 2, 0}, {"→", NULL, T_DROITE, 2, 0},
                             {"⇆", NULL, T_MODE, 2, 0}, {"⌄", NULL, T_MASQUER, 2, 0}, FIN };
static const Touche N1[] = { L("7"), L("8"), L("9"), {"⌫", NULL, T_EFFACER, 2, 0}, FIN };
static const Touche N2[] = { L("4"), L("5"), L("6"), {"↵", NULL, T_ENTREE, 2, 2}, FIN };
static const Touche N3[] = { L("1"), L("2"), L("3"), {"", NULL, T_VIDE, 2, 0}, FIN };
static const Touche N4[] = { {"0", NULL, T_TEXTE, 4, 0}, L(","), L("."), FIN };

typedef struct {
    const char           *nom;
    const Touche *const  *rangees;
    int                   colonnes;
} Couche;

static const Touche *const PLEIN_L[]  = { P0, P1, P2, P3, P4, NULL };
static const Touche *const PLEIN_S[]  = { Q0, Q1, Q2, Q3, Q4, NULL };
static const Touche *const GAUCHE_L[] = { G0, G1, G2, G3, G4, NULL };
static const Touche *const GAUCHE_S[] = { H0, H1, H2, H3, H4, NULL };
static const Touche *const PAVE[]     = { N0, N1, N2, N3, N4, NULL };

static const Couche COUCHES_PLEIN[]  = { {"lettres", PLEIN_L, 30},
                                         {"altgr", PLEIN_S, 30}, {NULL, NULL, 0} };
static const Couche COUCHES_GAUCHE[] = { {"lettres", GAUCHE_L, 20},
                                         {"symboles", GAUCHE_S, 20}, {NULL, NULL, 0} };
static const Couche COUCHES_DROITE[] = { {"pave", PAVE, 8}, {NULL, NULL, 0} };

/* --- Les touches mortes --------------------------------------------------
 * Celles du clavier physique français, et elles seules. */
typedef struct { const char *accent, *lettre, *resultat; } Composition;

static const Composition COMPOSITIONS[] = {
    {"^","a","â"},{"^","e","ê"},{"^","i","î"},{"^","o","ô"},{"^","u","û"},
    {"^","A","Â"},{"^","E","Ê"},{"^","I","Î"},{"^","O","Ô"},{"^","U","Û"},
    {"¨","a","ä"},{"¨","e","ë"},{"¨","i","ï"},{"¨","o","ö"},{"¨","u","ü"},
    {"¨","y","ÿ"},{"¨","A","Ä"},{"¨","E","Ë"},{"¨","I","Ï"},{"¨","O","Ö"},
    {"¨","U","Ü"},{"¨","Y","Ÿ"},
};

/* =========================================================================
 * L'état
 * ========================================================================= */
typedef enum { MAJ_NON, MAJ_UNE, MAJ_VERROU } EtatMaj;
typedef enum { MODE_PLEIN, MODE_DEUX_MAINS } Mode;

static struct {
    GtkWidget *plein;           /* fenêtre du plein format                */
    GtkWidget *plein_interieur; /* sa largeur bornée                      */
    GtkWidget *plein_pile;
    GtkWidget *gauche;          /* fenêtres du mode à deux mains          */
    GtkWidget *gauche_pile;
    GtkWidget *droite;

    GPtrArray *textes;          /* boutons T_TEXTE : étiquette selon Maj  */
    GPtrArray *boutons_maj;
    GPtrArray *boutons_verr;
    GPtrArray *boutons_morte;

    Mode       mode;
    gboolean   ecrit;           /* le clavier virtuel est branché         */
    gboolean   tablette;
    gboolean   champ;           /* un champ de texte a le focus           */
    ShellSaisieBut but;
    gboolean   renvoye;         /* ⌄ appuyé depuis le dernier champ       */
    gboolean   demande;         /* montré sur demande                     */
    gboolean   visible;
    guint      masquage;

    EtatMaj    maj;
    gint64     dernier_maj;
    gboolean   verr_maj;        /* ⇪                                      */
    const char *morte;          /* accent en attente, ou NULL             */

    guint      repetition;
    GtkWidget *tenue;           /* la touche qui se répète                */
} K;

/* =========================================================================
 * Ce qu'écrit une touche, et ce qu'elle montre
 * ========================================================================= */
static gboolean
est_lettre (const char *s)
{
    return s != NULL && g_unichar_isalpha (g_utf8_get_char (s))
        && *g_utf8_next_char (s) == '\0';
}

/* Le texte que la touche écrirait maintenant. `libre` reçoit ce qui a été
 * alloué, s'il y a lieu. */
static const char *
sortie (const Touche *t, char **libre)
{
    *libre = NULL;
    if (K.maj != MAJ_NON) {
        if (t->maj != NULL)
            return t->maj;
        if (est_lettre (t->base))
            return *libre = g_utf8_strup (t->base, -1);
        return t->base;
    }
    /* ⇪ ne touche que les lettres, comme sur le physique sous Linux : é
     * devient É, et non 2. */
    if (K.verr_maj && est_lettre (t->base))
        return *libre = g_utf8_strup (t->base, -1);
    return t->base;
}

/* L'étiquette : ce qui sortira, en grand ; et pour une touche double, en
 * petit au-dessus, l'autre caractère — comme sur le cabochon d'un clavier,
 * mais le caractère actif toujours en grand. */
static void
etiqueter (GtkWidget *b)
{
    const Touche *t = g_object_get_data (G_OBJECT (b), "touche");
    g_autofree char *libre = NULL;
    const char *maintenant = sortie (t, &libre);

    if (t->maj == NULL) {
        gtk_label_set_text (GTK_LABEL (gtk_button_get_child (GTK_BUTTON (b))), maintenant);
        return;
    }
    const char *autre = g_strcmp0 (maintenant, t->maj) == 0 ? t->base : t->maj;
    /* 72 % et 85 % : à 62 % et 55 %, « trop peu visible » au premier essai
     * (11 septembre 2026). Il doit rester en retrait sans devenir illisible
     * — c'est lui qu'on cherche pour les chiffres. */
    g_autofree char *m = g_markup_printf_escaped (
        "<span size=\"72%%\" alpha=\"85%%\">%s</span>\n%s", autre, maintenant);
    gtk_label_set_markup (GTK_LABEL (gtk_button_get_child (GTK_BUTTON (b))), m);
}

static void
marquer (GPtrArray *boutons, const char *classe, gboolean oui)
{
    for (guint i = 0; i < boutons->len; i++) {
        GtkWidget *b = g_ptr_array_index (boutons, i);
        if (oui) gtk_widget_add_css_class (b, classe);
        else     gtk_widget_remove_css_class (b, classe);
    }
}

static void
afficher_etat (void)
{
    for (guint i = 0; i < K.textes->len; i++)
        etiqueter (g_ptr_array_index (K.textes, i));
    marquer (K.boutons_maj, "active", K.maj != MAJ_NON);
    marquer (K.boutons_maj, "verrouille", K.maj == MAJ_VERROU);
    marquer (K.boutons_verr, "active", K.verr_maj);
    marquer (K.boutons_morte, "active", K.morte != NULL);
}

static void
maj_poser (EtatMaj etat)
{
    if (K.maj == etat)
        return;
    K.maj = etat;
    afficher_etat ();
}

/* Une touche frappée : Maj « une lettre » retombe. */
static void
maj_consommer (void)
{
    if (K.maj == MAJ_UNE)
        maj_poser (MAJ_NON);
}

/* =========================================================================
 * Frapper
 * ========================================================================= */
static void appliquer (void);
static void changer_mode (void);

static void
morte_poser (const char *accent)
{
    K.morte = accent;
    marquer (K.boutons_morte, "active", accent != NULL);
}

/* Une touche morte en attente, puis autre chose qu'une lettre : le
 * physique écrit l'accent seul, puis ce qui suit. */
static void
morte_liberer (void)
{
    if (K.morte != NULL) {
        shell_saisie_texte (K.morte);
        morte_poser (NULL);
    }
}

static void
ecrire (const char *texte)
{
    if (K.morte != NULL) {
        for (guint i = 0; i < G_N_ELEMENTS (COMPOSITIONS); i++)
            if (g_str_equal (COMPOSITIONS[i].accent, K.morte)
                && g_str_equal (COMPOSITIONS[i].lettre, texte)) {
                shell_saisie_texte (COMPOSITIONS[i].resultat);
                morte_poser (NULL);
                return;
            }
        morte_liberer ();
    }
    shell_saisie_texte (texte);
}

static void
masquer_sur_demande (void)
{
    K.demande = FALSE;
    K.renvoye = TRUE;
    appliquer ();
}

static void
frapper (const Touche *t, GtkWidget *bouton)
{
    switch (t->genre) {
    case T_TEXTE: {
        g_autofree char *libre = NULL;
        ecrire (sortie (t, &libre));
        maj_consommer ();
        break;
    }
    case T_MORTE: {
        g_autofree char *libre = NULL;
        const char *accent = sortie (t, &libre);
        /* Deux fois la même touche morte : l'accent seul, comme ^^ sur le
         * physique. Une autre : la première part seule. */
        gboolean meme = K.morte != NULL && g_str_equal (K.morte, accent);
        morte_liberer ();
        if (!meme)
            morte_poser (accent);   /* base ou maj : statiques, elles durent */
        maj_consommer ();
        break;
    }
    case T_ESPACE:
        /* Accent en attente + espace : l'accent seul, sans espace. */
        if (K.morte != NULL)
            morte_liberer ();
        else
            shell_saisie_texte (" ");
        maj_consommer ();
        break;
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
    case T_VERR_MAJ:
        K.verr_maj = !K.verr_maj;
        afficher_etat ();
        break;
    case T_EFFACER:
        /* ⌫ sur un accent en attente l'annule, comme sur le physique. */
        if (K.morte != NULL)
            morte_poser (NULL);
        else
            shell_saisie_touche (SHELL_TOUCHE_EFFACER, FALSE);
        break;
    case T_ENTREE:
        morte_liberer ();
        /* Maj+Entrée est le retour à la ligne de Claude Desktop, et de bien
         * des messageries : Maj enclenchée l'envoie comme tel. */
        shell_saisie_touche (SHELL_TOUCHE_ENTREE, K.maj != MAJ_NON);
        maj_consommer ();
        break;
    case T_TABULATION:
        morte_liberer ();
        shell_saisie_touche (SHELL_TOUCHE_TABULATION, FALSE);
        break;
    case T_GAUCHE:
        morte_poser (NULL);
        shell_saisie_touche (SHELL_TOUCHE_GAUCHE, FALSE);
        break;
    case T_DROITE:
        morte_poser (NULL);
        shell_saisie_touche (SHELL_TOUCHE_DROITE, FALSE);
        break;
    case T_COUCHE: {
        GtkWidget *pile = gtk_widget_get_ancestor (bouton, GTK_TYPE_STACK);
        if (pile != NULL)
            gtk_stack_set_visible_child_name (GTK_STACK (pile), t->maj);
        break;
    }
    case T_MODE:
        changer_mode ();
        break;
    case T_MASQUER:
        masquer_sur_demande ();
        break;
    case T_VIDE:
        break;
    }
}

static void
on_clic (GtkButton *b, gpointer data)
{
    frapper (data, GTK_WIDGET (b));
}

/* --- La répétition des touches tenues ----------------------------------- */
static void
arreter_repetition (void)
{
    if (K.repetition != 0) {
        g_source_remove (K.repetition);
        K.repetition = 0;
    }
    if (K.tenue != NULL) {
        gtk_widget_remove_css_class (K.tenue, "tenue");
        K.tenue = NULL;
    }
}

static gboolean
on_repetition (gpointer data)
{
    frapper (data, NULL);
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

    /* Le clic annulé emporte l'état :active du bouton : sans cette classe,
     * ⌫ qui efface en continu paraîtrait relâchée. */
    GtkWidget *b = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (g));
    gtk_widget_add_css_class (b, "tenue");
    K.tenue = b;

    frapper (data, NULL);
    K.repetition = g_timeout_add (REPETITION_MS, on_repetition, data);
}

static void
on_tenue_finie (GtkGesture *g, GdkEventSequence *s, gpointer data)
{
    (void) g; (void) s; (void) data;
    arreter_repetition ();
}

/* =========================================================================
 * Le dessin
 * ========================================================================= */
static GtkWidget *
touche_neuve (const Touche *t)
{
    GtkWidget *b = gtk_button_new ();
    GtkWidget *etiquette = gtk_label_new (t->base);
    gtk_label_set_justify (GTK_LABEL (etiquette), GTK_JUSTIFY_CENTER);
    gtk_button_set_child (GTK_BUTTON (b), etiquette);

    /* Ceinture et bretelles. La surface ne prend jamais le clavier
     * (KEYBOARD_MODE_NONE), mais un bouton qui voudrait le focus ne doit
     * pas pouvoir le demander — voir clavier.c, qui l'a payé. */
    gtk_widget_set_can_focus (b, FALSE);
    gtk_widget_set_focus_on_click (b, FALSE);
    gtk_widget_add_css_class (b, "clavier-touche");
    if (t->genre != T_TEXTE && t->genre != T_MORTE)
        gtk_widget_add_css_class (b, "speciale");
    if (t->genre == T_ENTREE)
        gtk_widget_add_css_class (b, "valider");
    if (t->genre == T_ESPACE)
        gtk_widget_add_css_class (b, "espace");
    if (t->maj != NULL && (t->genre == T_TEXTE || t->genre == T_MORTE))
        gtk_widget_add_css_class (b, "double");
    gtk_widget_set_hexpand (b, TRUE);
    gtk_widget_set_vexpand (b, TRUE);

    /* Les tables sont statiques : elles survivent aux boutons. */
    g_object_set_data (G_OBJECT (b), "touche", (gpointer) t);
    g_signal_connect (b, "clicked", G_CALLBACK (on_clic), (gpointer) t);

    if (t->genre == T_EFFACER || t->genre == T_GAUCHE || t->genre == T_DROITE) {
        GtkGesture *tenue = gtk_gesture_long_press_new ();
        g_signal_connect (tenue, "pressed", G_CALLBACK (on_tenue), (gpointer) t);
        g_signal_connect (tenue, "end", G_CALLBACK (on_tenue_finie), NULL);
        g_signal_connect (tenue, "cancel", G_CALLBACK (on_tenue_finie), NULL);
        gtk_widget_add_controller (b, GTK_EVENT_CONTROLLER (tenue));
    }

    switch (t->genre) {
    case T_TEXTE:    g_ptr_array_add (K.textes, b);        break;
    case T_MORTE:    g_ptr_array_add (K.textes, b);
                     g_ptr_array_add (K.boutons_morte, b); break;
    case T_MAJ:      g_ptr_array_add (K.boutons_maj, b);   break;
    case T_VERR_MAJ: g_ptr_array_add (K.boutons_verr, b);  break;
    case T_MODE:     gtk_widget_set_tooltip_text (b, "Changer de disposition"); break;
    case T_MASQUER:  gtk_widget_set_tooltip_text (b, "Masquer le clavier");     break;
    default: break;
    }
    return b;
}

static GtkWidget *
grille (const Couche *c)
{
    GtkWidget *g = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (g), 8);
    gtk_grid_set_column_spacing (GTK_GRID (g), 8);
    gtk_grid_set_row_homogeneous (GTK_GRID (g), TRUE);
    gtk_grid_set_column_homogeneous (GTK_GRID (g), TRUE);

    for (int y = 0; c->rangees[y] != NULL; y++) {
        int x = 0;
        for (const Touche *t = c->rangees[y]; t->base != NULL; t++) {
            if (t->genre != T_VIDE)
                gtk_grid_attach (GTK_GRID (g), touche_neuve (t), x, y,
                                 t->largeur, t->hauteur > 0 ? t->hauteur : 1);
            x += t->largeur;
        }
        /* Une rangée bancale se voit tout de suite. */
        if (x != c->colonnes)
            g_warning ("clavier à l'écran : couche « %s », rangée %d : %d colonnes "
                       "au lieu de %d", c->nom, y, x, c->colonnes);
    }
    return g;
}

static GtkWidget *
pile_neuve (const Couche *couches)
{
    GtkWidget *pile = gtk_stack_new ();
    gtk_widget_add_css_class (pile, "clavier");
    for (const Couche *c = couches; c->nom != NULL; c++)
        gtk_stack_add_named (GTK_STACK (pile), grille (c), c->nom);
    return pile;
}

/* --- Les chiffres du plein format : le pavé de l'écran de connexion ------ */
static const Touche ENTREE_PAVE  = {"↵",   NULL,      T_ENTREE, 0, 0};
static const Touche LETTRES_PAVE = {"abc", "lettres", T_COUCHE, 0, 0};

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

/* --- Les fenêtres --------------------------------------------------------- */
static GtkWidget *
fenetre_neuve (GtkApplication *app, gboolean gauche, gboolean droite,
               int zone, const char *nom, const char *classe, GtkWidget *contenu)
{
    GtkWidget *f = gtk_window_new ();
    gtk_window_set_application (GTK_WINDOW (f), app);
    gtk_widget_add_css_class (f, "shell");

    gtk_layer_init_for_window (GTK_WINDOW (f));
    /* OVERLAY, comme le dock : labwc éteint la couche TOP sous une fenêtre
     * plein écran (vu au banc, voir CLAUDE.md), et un champ peut vivre dans
     * une fenêtre plein écran. */
    gtk_layer_set_layer (GTK_WINDOW (f), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor (GTK_WINDOW (f), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (f), GTK_LAYER_SHELL_EDGE_LEFT, gauche);
    gtk_layer_set_anchor (GTK_WINDOW (f), GTK_LAYER_SHELL_EDGE_RIGHT, droite);
    gtk_layer_set_namespace (GTK_WINDOW (f), nom);
    if (zone == 0)
        gtk_layer_auto_exclusive_zone_enable (GTK_WINDOW (f));
    else
        gtk_layer_set_exclusive_zone (GTK_WINDOW (f), zone);
    /* LA ligne qui compte : le clavier ne prend JAMAIS le focus. Sans elle,
     * le premier appui enlèverait le focus au champ où l'on écrit. */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (f), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GtkWidget *fond = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (fond, "clavier-ecran");
    if (classe != NULL)
        gtk_widget_add_css_class (fond, classe);
    gtk_box_append (GTK_BOX (fond), contenu);
    gtk_window_set_child (GTK_WINDOW (f), fond);
    return f;
}

/* =========================================================================
 * À l'écran ou non
 * ========================================================================= */
static void
ajuster_largeur (void)
{
    int largeur = LARGEUR_PLEIN;
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
    gtk_widget_set_size_request (K.plein_interieur, largeur, -1);
}

/* Tout revient au repos : lettres, Maj relâchée, accent oublié. Le champ
 * suivant ne doit pas hériter de l'état du précédent. */
static void
au_repos (void)
{
    arreter_repetition ();
    gtk_stack_set_visible_child_name (GTK_STACK (K.plein_pile), "lettres");
    gtk_stack_set_visible_child_name (GTK_STACK (K.gauche_pile), "lettres");
    K.morte = NULL;
    K.maj = MAJ_NON;
    afficher_etat ();
}

static void
appliquer (void)
{
    gboolean voir = K.ecrit && K.tablette && ((K.champ && !K.renvoye) || K.demande);

    gboolean plein = voir && K.mode == MODE_PLEIN;
    gboolean deux  = voir && K.mode == MODE_DEUX_MAINS;

    if (plein) {
        ajuster_largeur ();
        /* Un champ qui attend des chiffres ouvre le pavé ; tout autre revient
         * aux lettres s'il en héritait. En mode deux mains, le pavé est déjà
         * sous le pouce droit. */
        const char *voulue = K.but == SHELL_SAISIE_CHIFFRES && K.champ ? "chiffres" : NULL;
        const char *courante = gtk_stack_get_visible_child_name (GTK_STACK (K.plein_pile));
        if (voulue != NULL)
            gtk_stack_set_visible_child_name (GTK_STACK (K.plein_pile), voulue);
        else if (g_strcmp0 (courante, "chiffres") == 0)
            gtk_stack_set_visible_child_name (GTK_STACK (K.plein_pile), "lettres");
    }

    if (plein) gtk_window_present (GTK_WINDOW (K.plein));
    else       gtk_widget_set_visible (K.plein, FALSE);
    if (deux) {
        gtk_window_present (GTK_WINDOW (K.gauche));
        gtk_window_present (GTK_WINDOW (K.droite));
    } else {
        gtk_widget_set_visible (K.gauche, FALSE);
        gtk_widget_set_visible (K.droite, FALSE);
    }

    if (voir != K.visible) {
        K.visible = voir;
        if (!voir)
            au_repos ();
        g_debug ("clavier à l'écran : %s", voir ? "montré" : "masqué");
    }
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
    K.but = but;
    K.renvoye = FALSE;
    appliquer ();
}

/* =========================================================================
 * La disposition choisie, gardée d'une session à l'autre
 *
 * Un état d'usage, pas un réglage : il vit dans ~/.local/state, pas dans
 * shell.conf, que tous les composants surveillent et relisent — changer de
 * disposition au doigt n'a pas à faire reconstruire le dock et la barre.
 * ========================================================================= */
static char *
chemin_etat (void)
{
    return g_build_filename (g_get_user_state_dir (), "claude-os", "clavier.ini", NULL);
}

static void
mode_lire (void)
{
    g_autofree char *chemin = chemin_etat ();
    g_autoptr(GKeyFile) kf = g_key_file_new ();
    g_autoptr(GError) err = NULL;
    if (!g_key_file_load_from_file (kf, chemin, G_KEY_FILE_NONE, &err)) {
        if (!g_error_matches (err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_warning ("clavier à l'écran : %s : %s", chemin, err->message);
        return;
    }
    g_autofree char *d = g_key_file_get_string (kf, "clavier", "disposition", NULL);
    K.mode = g_strcmp0 (d, "deux-mains") == 0 ? MODE_DEUX_MAINS : MODE_PLEIN;
}

static void
mode_ecrire (void)
{
    g_autofree char *chemin = chemin_etat ();
    g_autofree char *dossier = g_path_get_dirname (chemin);
    g_autoptr(GKeyFile) kf = g_key_file_new ();
    g_autoptr(GError) err = NULL;
    g_key_file_set_string (kf, "clavier", "disposition",
                           K.mode == MODE_DEUX_MAINS ? "deux-mains" : "plein");
    if (g_mkdir_with_parents (dossier, 0700) != 0
        || !g_key_file_save_to_file (kf, chemin, &err))
        g_warning ("clavier à l'écran : disposition non gardée (%s) : %s", chemin,
                   err != NULL ? err->message : g_strerror (errno));
}

static void
changer_mode (void)
{
    K.mode = K.mode == MODE_PLEIN ? MODE_DEUX_MAINS : MODE_PLEIN;
    au_repos ();
    mode_ecrire ();
    g_message ("clavier à l'écran : disposition %s",
               K.mode == MODE_PLEIN ? "plein format" : "à deux mains");
    appliquer ();
}

/* =========================================================================
 * L'interface publique
 * ========================================================================= */

/* Tout ce que le clavier peut écrire, pour la disposition fabriquée :
 * chaque texte, sa variante Maj, sa majuscule, et les compositions des
 * touches mortes. */
static void
ajouter_texte (GPtrArray *a, const char *s)
{
    if (s == NULL || *s == '\0')
        return;
    for (guint i = 0; i < a->len; i++)
        if (g_str_equal (g_ptr_array_index (a, i), s))
            return;
    g_ptr_array_add (a, g_strdup (s));
}

static GPtrArray *
tous_les_textes (void)
{
    GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
    const Couche *const groupes[] = { COUCHES_PLEIN, COUCHES_GAUCHE, COUCHES_DROITE };

    ajouter_texte (a, " ");
    for (guint g = 0; g < G_N_ELEMENTS (groupes); g++)
        for (const Couche *c = groupes[g]; c->nom != NULL; c++)
            for (int y = 0; c->rangees[y] != NULL; y++)
                for (const Touche *t = c->rangees[y]; t->base != NULL; t++) {
                    if (t->genre != T_TEXTE && t->genre != T_MORTE)
                        continue;
                    ajouter_texte (a, t->base);
                    ajouter_texte (a, t->maj);
                    if (est_lettre (t->base)) {
                        g_autofree char *haut = g_utf8_strup (t->base, -1);
                        ajouter_texte (a, haut);
                    }
                }
    for (guint i = 0; i < G_N_ELEMENTS (COMPOSITIONS); i++)
        ajouter_texte (a, COMPOSITIONS[i].resultat);
    for (char c = '0'; c <= '9'; c++) {
        char s[2] = { c, '\0' };
        ajouter_texte (a, s);
    }
    g_ptr_array_add (a, NULL);
    return a;
}

void
shell_clavier_ecran_init (GtkApplication *app)
{
    K.textes = g_ptr_array_new ();
    K.boutons_maj = g_ptr_array_new ();
    K.boutons_verr = g_ptr_array_new ();
    K.boutons_morte = g_ptr_array_new ();
    mode_lire ();

    /* Plein format : un bandeau sur toute la largeur, touches centrées. */
    K.plein_pile = pile_neuve (COUCHES_PLEIN);
    gtk_stack_add_named (GTK_STACK (K.plein_pile), chiffres (), "chiffres");
    K.plein_interieur = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign (K.plein_interieur, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (K.plein_interieur), K.plein_pile);
    K.plein = fenetre_neuve (app, TRUE, TRUE, 0, "claude-os-clavier",
                             "clavier-plein", K.plein_interieur);

    /* Deux mains. La gauche réserve sa hauteur, pour que le champ reste
     * au-dessus ; la droite ne réserve rien (-1) : deux zones sur le même
     * bord s'empileraient, et le pavé flotterait au-dessus des lettres. */
    K.gauche_pile = pile_neuve (COUCHES_GAUCHE);
    gtk_widget_set_size_request (K.gauche_pile, LARGEUR_GAUCHE, -1);
    K.gauche = fenetre_neuve (app, TRUE, FALSE, 0, "claude-os-clavier-gauche",
                              "clavier-gauche", K.gauche_pile);
    GtkWidget *pave = pile_neuve (COUCHES_DROITE);
    gtk_widget_set_size_request (pave, LARGEUR_DROITE, -1);
    K.droite = fenetre_neuve (app, FALSE, TRUE, -1, "claude-os-clavier-droite",
                              "clavier-droite", pave);

    afficher_etat ();

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
