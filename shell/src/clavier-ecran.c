/* =========================================================================
 * Claude OS — le clavier à l'écran de la session. Voir clavier-ecran.h.
 * ========================================================================= */
#include "clavier-ecran.h"

#include <errno.h>
#include <string.h>
#include <gtk4-layer-shell.h>

#include "clavier.h"
#include "mots.h"
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

/* LE MODE CONSOLE — deux claviers collés aux bords, à la façon des manettes
 * d'une console portable, et l'écran recadré entre eux. Dessiné par
 * l'utilisateur le 11 septembre 2026 : à gauche la frappe, sous le pouce
 * gauche (il est gaucher) ; à droite l'espace, les fonctions et les
 * BASCULES qui changent ce que le clavier gauche écrit.
 *
 * Les dimensions sont MESURÉES (shell/essais/sonde-pouces.c, tablette tenue
 * à deux mains, 245 appuis) :
 *
 *   pouce gauche : portée depuis le bord, 95 % en deçà de 262 px, max 305 ;
 *   pouce droit  : 95 % en deçà de 320 px, max 398 ;
 *   hauteur      : les deux pouces travaillent entre y = 180 et 540.
 *
 * Les deux claviers ont la largeur du pouce le PLUS COURT — règle de
 * l'utilisateur, pour ne jamais étirer le pouce le plus limité : 300 px
 * (48 mm). Et les touches commencent à HAUT_GAUCHE/HAUT_DROITE : au-dessus, le pouce
 * n'atteint pas ; en dessous de 540, non plus — et c'est là que la main qui
 * tient la tablette frôle l'écran (six appuis parasites mesurés au bord,
 * y ≈ 990). */
#define LARGEUR_CONSOLE 300
#define HAUT_DROITE     176

/* La colonne gauche a été resserrée et REMONTÉE après le premier essai au
 * doigt : « la ligne du bas est trop basse ». Touches de 52 px au pas de
 * 56, première rangée centrée sur y = 190, dernière sur 470 — 40 px plus
 * haut que la rangée qui gênait. 158 = 190 - 26 (demi-touche) - 6 (marge
 * de la fenêtre). */
#define HAUT_GAUCHE     158

/* La rangée des suggestions coiffe la colonne gauche. Elle est au-dessus de
 * la zone mesurée du pouce (y = 180 au mieux), mais c'est la seule place
 * libre dans la colonne, et une cible large se vise mieux qu'une touche.
 * 112 : de quoi poser 44 px de suggestions et 8 d'écart avant la première
 * rangée, centrée sur 190. */
#define HAUT_SUGGESTIONS 112
#define HAUTEUR_SUGGESTION 44

/* Les bascules du mode console : un calque « une frappe » revient aux
 * lettres après un caractère ; un calque « tient » reste jusqu'au prochain
 * appui. Les accents et les symboles s'écrivent un à un au milieu des
 * lettres ; les chiffres, par séries. */
#define CALQUE_UNE   1
#define CALQUE_TIENT 2

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
    T_CALQUE,       /* mode console : bascule le clavier GAUCHE vers la
                       couche nommée par « maj » ; tenue, le temps qu'on
                       la tient — voir « Les bascules »                     */
    T_MODE,         /* plein format <-> console                             */
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
    int         drapeau;   /* T_CALQUE : CALQUE_UNE ou CALQUE_TIENT          */
} Touche;

#define FIN          {NULL, NULL, T_TEXTE, 0, 0, 0}
#define L(c)         {c, NULL, T_TEXTE, 2, 0, 0}
#define D(c, m)      {c, m, T_TEXTE, 2, 0, 0}

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
    {"⌫", NULL, T_EFFACER, 6, 0, 0}, FIN };
static const Touche P1[] = {
    {"⇥", NULL, T_TABULATION, 3, 0, 0},
    L("a"), L("z"), L("e"), L("r"), L("t"), L("y"), L("u"), L("i"), L("o"), L("p"),
    {"^", "¨", T_MORTE, 2, 0, 0}, D("$","£"),
    {"↵", NULL, T_ENTREE, 3, 2, 0}, FIN };
static const Touche P2[] = {
    {"⇪", NULL, T_VERR_MAJ, 3, 0, 0},
    L("q"), L("s"), L("d"), L("f"), L("g"), L("h"), L("j"), L("k"), L("l"), L("m"),
    D("ù","%"), D("*","µ"),
    {"", NULL, T_VIDE, 3, 0, 0}, FIN };
static const Touche P3[] = {
    {"⇧", NULL, T_MAJ, 3, 0, 0}, D("<",">"),
    L("w"), L("x"), L("c"), L("v"), L("b"), L("n"),
    D(",","?"), D(";","."), D(":","/"), D("!","§"),
    {"⇧", NULL, T_MAJ, 5, 0, 0}, FIN };
static const Touche P4[] = {
    {"&@#", "altgr", T_COUCHE, 4, 0, 0}, {"←", NULL, T_GAUCHE, 3, 0, 0},
    {"espace", NULL, T_ESPACE, 13, 0, 0},
    {"→", NULL, T_DROITE, 3, 0, 0}, {"⇆", NULL, T_MODE, 3, 0, 0},
    {"⌄", NULL, T_MASQUER, 4, 0, 0}, FIN };

/* --- Plein format : ce que donne AltGr sur le physique, et au-delà ------- */
static const Touche Q0[] = {
    L("~"), L("#"), L("{"), L("["), L("|"), L("`"), L("\\"), L("^"), L("@"),
    L("]"), L("}"), L("€"), {"⌫", NULL, T_EFFACER, 6, 0, 0}, FIN };
static const Touche Q1[] = {
    {"⇥", NULL, T_TABULATION, 3, 0, 0},
    L("«"), L("»"), L("“"), L("”"), L("‘"), L("’"), L("–"), L("—"), L("…"),
    L("•"), L("°"), L("²"),
    {"↵", NULL, T_ENTREE, 3, 2, 0}, FIN };
static const Touche Q2[] = {
    {"abc", "lettres", T_COUCHE, 3, 0, 0},
    L("œ"), L("Œ"), L("æ"), L("Æ"), L("ß"), L("ñ"), L("Ñ"), L("¿"), L("¡"),
    L("±"), L("×"), L("÷"),
    {"", NULL, T_VIDE, 3, 0, 0}, FIN };
static const Touche Q3[] = {
    L("¤"), L("≠"), L("≤"), L("≥"), L("¼"), L("½"), L("¾"), L("©"), L("®"),
    L("™"), L("¶"), L("¥"), L("¢"), L("‰"), L("³"), FIN };
static const Touche Q4[] = {
    {"abc", "lettres", T_COUCHE, 4, 0, 0}, {"←", NULL, T_GAUCHE, 3, 0, 0},
    {"espace", NULL, T_ESPACE, 13, 0, 0},
    {"→", NULL, T_DROITE, 3, 0, 0}, {"⇆", NULL, T_MODE, 3, 0, 0},
    {"⌄", NULL, T_MASQUER, 4, 0, 0}, FIN };

/* --- Console, à gauche : la frappe, et elle seule ----------------------
 *
 * UNE DISPOSITION CALCULÉE POUR CE POUCE, et organique : les touches n'ont
 * ni la même taille ni le même alignement. Calcul dans
 * shell/essais/disposition-pouce.py, refaisable — voir son en-tête pour la
 * méthode (fréquences de Lexique, zone du pouce mesurée, loi de Fitts,
 * recuit simulé).
 *
 * TROISIÈME VERSION, 13 septembre 2026, demandée après essai : « un design
 * plus organique, avec les lettres les moins utilisées plus petites », et
 * « une diagonale haut-gauche bas-droit à la trajectoire légèrement
 * arrondie, pour suivre le mouvement du pouce ». Les 26 lettres sont de
 * retour — leur absence « est problématique » —, plus é.
 *
 * CE QUI EST GÉOMÉTRIQUE, ET POURQUOI
 *
 *   - les cellules PAVENT la colonne : aucun appui ne tombe entre deux
 *     touches. Une grille à espacement laisse des zones mortes, qu'un
 *     pouce trouve toujours. Le jeu visuel entre les touches est une
 *     bordure TRANSPARENTE, dans la feuille de style : elle se voit, mais
 *     elle reçoit l'appui ;
 *   - dans une rangée, la touche est d'autant plus large qu'elle est près
 *     de l'arc du pouce ; les rangées du cœur de la zone sont plus hautes ;
 *   - 40 à 70 px de large : les lettres rares y sont petites, et c'est
 *     l'optimisation qui l'a décidé, pas une règle. La loi de Fitts dit
 *     qu'une grande cible s'atteint plus vite : elle a donc mis les lettres
 *     fréquentes sur les grandes touches, et l'espace — 19,4 % des frappes
 *     — sur toute la largeur.
 *
 * Les coordonnées sont en pixels, depuis le coin haut gauche du clavier
 * (soit y = 176 à l'écran, le haut de la zone que le pouce atteint). */
/* `nuance` : huit niveaux de teinte, de la lettre la plus rare à la plus
 * fréquente — demandé par l'utilisateur après avoir vu la maquette. La
 * taille dit déjà l'importance ; la couleur la redit, et c'est ce qui rend
 * le clavier lisible d'un coup d'œil : on vise « e » sans le lire. */
typedef struct {
    Touche touche;
    int    x, y, l, h;
    int    nuance;
} Place;

static const Place CONSOLE_LETTRES[] = {
    { L("k"),   0,   0,  59,  52, 0 },
    { L("f"),  59,   0,  70,  52, 2 },
    { L("v"), 128,   0,  62,  52, 2 },
    { L("h"), 191,   0,  51,  52, 1 },
    { L("w"), 242,   0,  46,  52, 0 },
    { L("y"),   0,  52,  43,  56, 1 },
    { L("m"),  43,  52,  51,  56, 3 },
    { L("i"),  94,  52,  58,  56, 5 },
    { L("o"), 151,  52,  53,  56, 4 },
    { L("c"), 204,  52,  44,  56, 3 },
    { L("x"), 248,  52,  40,  56, 1 },
    { L("b"),   0, 108,  40,  60, 2 },
    { L("l"),  40, 108,  45,  60, 4 },
    { L("a"),  85, 108,  54,  60, 5 },
    { L("n"), 140, 108,  57,  60, 5 },
    { L("u"), 197, 108,  49,  60, 4 },
    { L("q"), 246, 108,  42,  60, 1 },
    { L("g"),   0, 168,  47,  56, 1 },
    { L("r"),  47, 168,  53,  56, 4 },
    { L("e"), 100, 168,  65,  56, 7 },
    { L("t"), 165, 168,  68,  56, 5 },
    { L("é"), 233, 168,  55,  56, 2 },
    { L("z"),   0, 224,  46,  56, 1 },
    { L("p"),  46, 224,  51,  56, 3 },
    { L("s"),  97, 224,  62,  56, 5 },
    { L("d"), 158, 224,  70,  56, 3 },
    { L("j"), 228, 224,  60,  56, 1 },
    { {"espace", NULL, T_ESPACE, 0, 0, 0}, 0, 280, 288, 56, 4 },
};

/* Les accents, les chiffres, les symboles : même grille de 5 x 5, et la
 * même barre d'espace en bas — elle ne doit pas disparaître quand on
 * bascule de calque. k et w sont ici : 0,02 % et 0,004 % du français, ils
 * ne méritaient pas une place sous le pouce. Les accents sont rangés par
 * fréquence sur les places les plus confortables (à 0,49 %, è 0,33,
 * ê 0,24, ç 0,19 — Lexique) ; é reste sur le calque des lettres, plus
 * fréquent que f, b, g, h, q ou j.
 *
 * Ce que le mode console n'a pas et que le plein format garde : | ` ^ £ §.
 * Vingt-cinq places par calque, il a fallu choisir. */
static const Touche CA0[] = { L("”"), L("«"), L("k"), L("°"), L("’"), FIN };
static const Touche CA1[] = { L("»"), L("â"), L("û"), L("ü"), L("“"), FIN };
static const Touche CA2[] = { L("œ"), L("ê"), L("è"), L("î"), L("…"), FIN };
static const Touche CA3[] = { L("æ"), L("ç"), L("à"), L("ù"), L("w"), FIN };
static const Touche CA4[] = { L("—"), L("ë"), L("ô"), L("ï"), L("–"), FIN };
static const Touche CA5[] = { {"espace", NULL, T_ESPACE, 10, 0, 0}, FIN };

/* Le pavé de calcul, 7 en haut, comme sur un clavier. */
static const Touche CC0[] = { L("7"), L("8"), L("9"), L("+"), L("-"), FIN };
static const Touche CC1[] = { L("4"), L("5"), L("6"), L("*"), L("/"), FIN };
static const Touche CC2[] = { L("1"), L("2"), L("3"), L("="), L("%"), FIN };
static const Touche CC3[] = { L("0"), L(","), L("."), L("€"), L("$"), FIN };
static const Touche CC4[] = { L("("), L(")"), L(":"), L(";"), L("#"), FIN };
static const Touche CC5[] = { {"espace", NULL, T_ESPACE, 10, 0, 0}, FIN };

static const Touche CS0[] = { L("!"), L("?"), L(";"), L(":"), L("\""), FIN };
static const Touche CS1[] = { L("("), L(")"), L("["), L("]"), L("{"), FIN };
static const Touche CS2[] = { L("}"), L("<"), L(">"), L("/"), L("\\"), FIN };
static const Touche CS3[] = { L("@"), L("#"), L("&"), L("_"), L("-"), FIN };
static const Touche CS4[] = { L("+"), L("="), L("*"), L("%"), L("~"), FIN };
static const Touche CS5[] = { {"espace", NULL, T_ESPACE, 10, 0, 0}, FIN };

/* --- Console, à droite : la ponctuation, les fonctions, les bascules ----
 *
 * Douze colonnes : trois places de 4, quatre de 3, ou une pleine. L'ordre
 * suit l'usage, du moins fréquent en haut au plus fréquent au milieu de la
 * bande que le pouce droit atteint (médiane mesurée : 164 px du bord,
 * y ≈ 380) :
 *
 *   ⇥ ⇆ ⌄        ce dont on se sert le moins
 *   éà 123 #&    les bascules du clavier gauche
 *   ' , . ?      la ponctuation, retirée du clavier gauche le 13 septembre
 *   ⇧ ⌫          au cœur de la zone
 *   espace       19,4 % des frappes — elle est aussi à gauche
 *   ← → ↵
 *
 * Même grille que la gauche : six rangées de y = 190 à 470. */
static const Touche CD0[] = { {"⇥", NULL, T_TABULATION, 4, 0, 0},
                              {"⇆", NULL, T_MODE, 4, 0, 0},
                              {"⌄", NULL, T_MASQUER, 4, 0, 0}, FIN };
static const Touche CD1[] = { {"éà", "accents", T_CALQUE, 4, 0, CALQUE_UNE},
                              {"123", "chiffres", T_CALQUE, 4, 0, CALQUE_TIENT},
                              {"#&", "symboles", T_CALQUE, 4, 0, CALQUE_UNE}, FIN };
static const Touche CD2[] = { {"'", NULL, T_TEXTE, 3, 0, 0}, {",", NULL, T_TEXTE, 3, 0, 0},
                              {".", NULL, T_TEXTE, 3, 0, 0}, {"?", "!", T_TEXTE, 3, 0, 0}, FIN };
static const Touche CD3[] = { {"⇧", NULL, T_MAJ, 4, 0, 0},
                              {"⌫", NULL, T_EFFACER, 8, 0, 0}, FIN };
static const Touche CD4[] = { {"espace", NULL, T_ESPACE, 12, 0, 0}, FIN };
static const Touche CD5[] = { {"←", NULL, T_GAUCHE, 3, 0, 0},
                              {"→", NULL, T_DROITE, 3, 0, 0},
                              {"↵", NULL, T_ENTREE, 6, 0, 0}, FIN };

typedef struct {
    const char           *nom;
    const Touche *const  *rangees;
    int                   colonnes;
} Couche;

static const Touche *const PLEIN_L[]    = { P0, P1, P2, P3, P4, NULL };
static const Touche *const PLEIN_S[]    = { Q0, Q1, Q2, Q3, Q4, NULL };
static const Touche *const CONSOLE_A[]  = { CA0, CA1, CA2, CA3, CA4, CA5, NULL };
static const Touche *const CONSOLE_C[]  = { CC0, CC1, CC2, CC3, CC4, CC5, NULL };
static const Touche *const CONSOLE_S[]  = { CS0, CS1, CS2, CS3, CS4, CS5, NULL };
static const Touche *const CONSOLE_D[]  = { CD0, CD1, CD2, CD3, CD4, CD5, NULL };

static const Couche COUCHES_PLEIN[]     = { {"lettres", PLEIN_L, 30},
                                            {"altgr", PLEIN_S, 30}, {NULL, NULL, 0} };
/* Les lettres ne sont plus une grille : elles ont leur table de places
 * (CONSOLE_LETTRES) et leur propre fonction de dessin. */
static const Couche COUCHES_CONSOLE_G[] = { {"accents", CONSOLE_A, 10},
                                            {"chiffres", CONSOLE_C, 10},
                                            {"symboles", CONSOLE_S, 10}, {NULL, NULL, 0} };
static const Couche COUCHES_CONSOLE_D[] = { {"fonctions", CONSOLE_D, 12}, {NULL, NULL, 0} };

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
typedef enum { MODE_PLEIN, MODE_CONSOLE } Mode;

static struct {
    GtkWidget *plein;           /* fenêtre du plein format                */
    GtkWidget *plein_interieur; /* sa largeur bornée                      */
    GtkWidget *plein_pile;
    GtkWidget *gauche;          /* fenêtres du mode console               */
    GtkWidget *gauche_pile;
    GtkWidget *droite;

    GPtrArray *textes;          /* boutons T_TEXTE : étiquette selon Maj  */
    GPtrArray *boutons_maj;
    GPtrArray *boutons_verr;
    GPtrArray *boutons_morte;
    GPtrArray *boutons_calque;

    /* Les bascules du mode console — voir « Les bascules ». */
    const Touche *calque_tenu;  /* bascule sous le pouce droit, ou NULL   */
    gboolean   calque_servi;    /* une frappe à gauche pendant la tenue   */
    const char *calque_avant;   /* couche gauche avant l'appui            */
    gboolean   calque_une;      /* revenir aux lettres après une frappe   */
    gint64     dernier_calque;

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

    /* Les suggestions — mode console, colonne gauche. */
    gboolean   mots_ok;         /* le dictionnaire est là                 */
    GtkWidget *suggestions;     /* la rangée, toujours visible            */
    GtkWidget *sugg[3];
    GString   *mot;             /* ce qui est tapé du mot en cours        */
    GString   *precedent;       /* le mot d'avant : il oriente la suite   */
    gboolean   espace_auto;     /* la dernière espace vient d'une suggestion */
} K;

/* Trois propositions : au-delà, elles deviennent trop étroites pour être
 * lues d'un coup d'œil, et l'on perd plus à choisir qu'à taper. */
#define SUGGESTIONS 3

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

    /* Une bascule s'allume quand sa couche est à gauche ; elle se souligne
     * quand la couche TIENT — verrouillée, ou chiffres par nature. */
    const char *couche = K.gauche_pile != NULL
        ? gtk_stack_get_visible_child_name (GTK_STACK (K.gauche_pile)) : NULL;
    for (guint i = 0; K.boutons_calque != NULL && i < K.boutons_calque->len; i++) {
        GtkWidget *b = g_ptr_array_index (K.boutons_calque, i);
        const Touche *t = g_object_get_data (G_OBJECT (b), "touche");
        gboolean ici = g_strcmp0 (couche, t->maj) == 0;
        if (ici) gtk_widget_add_css_class (b, "active");
        else     gtk_widget_remove_css_class (b, "active");
        if (ici && !K.calque_une) gtk_widget_add_css_class (b, "verrouille");
        else                      gtk_widget_remove_css_class (b, "verrouille");
    }
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
 * Les bascules du mode console
 *
 * Le pouce droit choisit ce qu'écrit le gauche. Deux gestes, et le premier
 * suffit pour commencer :
 *
 *   APPUI BREF    la couche s'affiche à gauche. Accents et symboles
 *                 reviennent aux lettres après UNE frappe — on n'en tape
 *                 guère deux d'affilée ; un double appui les verrouille.
 *                 Les chiffres tiennent jusqu'au prochain appui. Appuyer sur
 *                 la bascule de la couche affichée revient aux lettres.
 *   APPUI TENU    la couche s'affiche le temps qu'on la tient, et le pouce
 *                 gauche tape dedans. Au relâcher, retour aux lettres. C'est
 *                 le geste des claviers à calques : le plus rapide une fois
 *                 en main.
 *
 * Les deux se distinguent au relâcher : si une touche a été frappée à
 * gauche pendant la tenue, c'était une tenue ; sinon, un appui bref.
 *
 * Par un GtkGestureDrag et non par « clicked » : le pouce qui tient bouge
 * un peu, et un clic se dissout passé quelques pixels — la couche resterait
 * affichée sans que rien la ramène. Un glisser, lui, finit toujours.
 * ========================================================================= */
static void
calque_montrer (const char *couche)
{
    gtk_stack_set_visible_child_name (GTK_STACK (K.gauche_pile), couche);
    afficher_etat ();
}

/* Une frappe à gauche vient d'avoir lieu. */
static void
calque_frappe (void)
{
    if (K.calque_tenu != NULL) {
        K.calque_servi = TRUE;
    } else if (K.calque_une) {
        K.calque_une = FALSE;
        calque_montrer ("lettres");
    }
}

static void
on_calque_appui (GtkGestureDrag *g, double x, double y, gpointer data)
{
    (void) x; (void) y;
    const Touche *t = data;
    /* Réclamé : le bouton ne fera rien de son côté. L'allumage de la touche
     * passe par la classe « tenue », que GTK ne retirera pas. */
    gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_widget_add_css_class (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (g)),
                              "tenue");
    K.calque_avant = gtk_stack_get_visible_child_name (GTK_STACK (K.gauche_pile));
    K.calque_tenu = t;
    K.calque_servi = FALSE;
    calque_montrer (t->maj);
}

static void
calque_lacher (GtkGesture *g, const Touche *t)
{
    gtk_widget_remove_css_class (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (g)),
                                 "tenue");
    if (K.calque_tenu != t)
        return;                      /* déjà relâchée (fin puis annulation) */
    K.calque_tenu = NULL;

    gint64 maintenant = g_get_monotonic_time ();
    if (K.calque_servi) {
        /* Une tenue : la couche ne valait que pendant. */
        K.calque_une = FALSE;
        calque_montrer ("lettres");
    } else if (g_strcmp0 (K.calque_avant, t->maj) != 0) {
        /* Appui bref sur une autre couche : elle s'affiche. */
        K.calque_une = t->drapeau == CALQUE_UNE;
        calque_montrer (t->maj);
    } else if (K.calque_une && maintenant - K.dernier_calque < DOUBLE_APPUI_US) {
        /* Second appui rapproché sur la couche « une frappe » : verrou. */
        K.calque_une = FALSE;
        afficher_etat ();
    } else {
        /* Appui sur la bascule de la couche affichée : retour aux lettres. */
        K.calque_une = FALSE;
        calque_montrer ("lettres");
    }
    K.dernier_calque = maintenant;
}

static void
on_calque_fin (GtkGestureDrag *g, double dx, double dy, gpointer data)
{
    (void) dx; (void) dy;
    calque_lacher (GTK_GESTURE (g), data);
}

static void
on_calque_annule (GtkGesture *g, GdkEventSequence *s, gpointer data)
{
    (void) s;
    calque_lacher (g, data);
}

/* =========================================================================
 * Les suggestions, et ce qui les accompagne
 *
 * Demandées par l'utilisateur le 13 septembre 2026 : « n'importe quel
 * clavier tactile de taille réduite serait laborieux sans cette fonction ».
 * Vingt-cinq touches sous le pouce, c'est peu ; les suggestions rendent
 * aussi ce qu'on a retiré du calque — l'apostrophe, k, w, les accents
 * rares : « aujourd » propose « aujourd'hui ».
 *
 * LE MOT EN COURS EST CELUI QU'ON A TAPÉ, pas celui qui est à l'écran. Le
 * clavier ne lit pas le champ : il se souvient de ce qu'il a envoyé depuis
 * la dernière espace. Un doigt posé ailleurs dans le texte le trompe
 * jusqu'au mot suivant — c'est la limite assumée, et elle disparaîtra le
 * jour où les applications transmettront leur texte alentour
 * (surrounding_text, que Chromium n'offre pas sans option).
 *
 * TROIS AUTOMATISMES, demandés eux aussi :
 *   - une espace est posée après un mot choisi ;
 *   - une ponctuation tapée juste après remplace cette espace, et se fait
 *     suivre d'une espace : « mot . » devient « mot. » ;
 *   - après « . », « ! », « ? » ou une entrée, la majuscule s'arme seule.
 * ========================================================================= */
static void
suggestions_rafraichir (void)
{
    if (!K.mots_ok)
        return;
    char *mots[SUGGESTIONS] = { NULL };
    /* Mot en cours vide : ce sont des PRÉDICTIONS — ce qui suit d'ordinaire
     * le mot précédent. C'est ce qui fait la différence entre un
     * dictionnaire et un clavier. */
    guint n = shell_mots_suggerer (K.mot->str, K.precedent->len > 0 ? K.precedent->str : NULL,
                                   mots, SUGGESTIONS);
    for (guint i = 0; i < SUGGESTIONS; i++) {
        gtk_button_set_label (GTK_BUTTON (K.sugg[i]), i < n ? mots[i] : "");
        gtk_widget_set_sensitive (K.sugg[i], i < n);
        g_object_set_data_full (G_OBJECT (K.sugg[i]), "mot",
                                i < n ? g_strdup (mots[i]) : NULL, g_free);
        g_free (mots[i]);
    }
}

/* Le mot en cours : on n'y garde que ce qui fait un mot. */
static void
mot_ajouter (const char *texte)
{
    gunichar c = g_utf8_get_char (texte);
    if (g_unichar_isalpha (c) || g_str_equal (texte, "'") || g_str_equal (texte, "-"))
        g_string_append (K.mot, texte);
    else
        g_string_truncate (K.mot, 0);
    suggestions_rafraichir ();
}

static void
mot_effacer_dernier (void)
{
    if (K.mot->len == 0)
        return;
    const char *dernier = g_utf8_find_prev_char (K.mot->str, K.mot->str + K.mot->len);
    g_string_truncate (K.mot, dernier - K.mot->str);
    suggestions_rafraichir ();
}

/* Le mot est fini : il devient le contexte du suivant. */
static void
mot_fini (void)
{
    if (K.mot->len > 0)
        g_string_assign (K.precedent, K.mot->str);
    g_string_truncate (K.mot, 0);
    suggestions_rafraichir ();
}

/* Nouvelle phrase, ou curseur déplacé : plus de contexte. */
static void
phrase_nouvelle (void)
{
    g_string_truncate (K.mot, 0);
    g_string_truncate (K.precedent, 0);
    suggestions_rafraichir ();
}

/* Écrit un mot entier, caractère par caractère. */
static void
ecrire_mot (const char *mot, gboolean majuscule)
{
    for (const char *p = mot; *p != '\0'; p = g_utf8_next_char (p)) {
        g_autofree char *un = g_strndup (p, g_utf8_next_char (p) - p);
        if (majuscule && p == mot) {
            g_autofree char *haut = g_utf8_strup (un, -1);
            shell_saisie_texte (haut);
        } else {
            shell_saisie_texte (un);
        }
    }
}

static void
on_suggestion (GtkButton *b, gpointer data)
{
    (void) data;
    const char *mot = g_object_get_data (G_OBJECT (b), "mot");
    if (mot == NULL || *mot == '\0')
        return;

    /* Effacer ce qui a été tapé du mot, puis écrire le mot entier : c'est la
     * seule façon qui marche PARTOUT, y compris là où l'application ne dit
     * rien de son champ. */
    for (glong i = g_utf8_strlen (K.mot->str, -1); i > 0; i--)
        shell_saisie_touche (SHELL_TOUCHE_EFFACER, FALSE);

    ecrire_mot (mot, K.maj != MAJ_NON);
    maj_poser (K.maj == MAJ_VERROU ? MAJ_VERROU : MAJ_NON);
    shell_saisie_texte (" ");
    K.espace_auto = TRUE;
    shell_mots_apprendre (mot);
    g_string_assign (K.mot, mot);
    mot_fini ();                 /* le mot choisi devient le contexte */
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
        const char *texte = sortie (t, &libre);
        gboolean ponctuation = strchr (",.;:!?", texte[0]) != NULL && texte[1] == '\0';

        /* Une ponctuation juste après une espace automatique prend sa place :
         * « mot . » n'est pas ce qu'on voulait écrire. */
        if (ponctuation && K.espace_auto)
            shell_saisie_touche (SHELL_TOUCHE_EFFACER, FALSE);
        K.espace_auto = FALSE;

        ecrire (texte);
        if (ponctuation) {
            shell_saisie_texte (" ");
            K.espace_auto = TRUE;
        }
        maj_consommer ();
        if (strchr (".!?", texte[0]) != NULL && texte[1] == '\0') {
            /* Fin de phrase : majuscule armée, et plus de contexte. */
            maj_poser (MAJ_UNE);
            phrase_nouvelle ();
        } else if (ponctuation) {
            mot_fini ();
        } else {
            mot_ajouter (texte);
        }
        calque_frappe ();
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
        if (K.morte != NULL) {
            morte_liberer ();
        } else if (K.espace_auto) {
            /* Elle est déjà là : la suggestion vient de la poser. Une
             * seconde espace ne serait qu'une faute à corriger. */
            K.espace_auto = FALSE;
        } else {
            shell_saisie_texte (" ");
        }
        maj_consommer ();
        mot_fini ();
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
        if (K.morte != NULL) {
            morte_poser (NULL);
        } else {
            shell_saisie_touche (SHELL_TOUCHE_EFFACER, FALSE);
            K.espace_auto = FALSE;
            mot_effacer_dernier ();
        }
        break;
    case T_ENTREE:
        morte_liberer ();
        /* Maj+Entrée est le retour à la ligne de Claude Desktop, et de bien
         * des messageries : Maj enclenchée l'envoie comme tel. */
        shell_saisie_touche (SHELL_TOUCHE_ENTREE, K.maj != MAJ_NON);
        maj_consommer ();
        K.espace_auto = FALSE;
        phrase_nouvelle ();
        maj_poser (MAJ_UNE);       /* nouvelle ligne, nouvelle phrase */
        break;
    case T_TABULATION:
        morte_liberer ();
        shell_saisie_touche (SHELL_TOUCHE_TABULATION, FALSE);
        break;
    case T_GAUCHE:
        morte_poser (NULL);
        shell_saisie_touche (SHELL_TOUCHE_GAUCHE, FALSE);
        /* Le curseur a bougé : ce qu'on croyait savoir du mot en cours ne
         * vaut plus. */
        K.espace_auto = FALSE;
        phrase_nouvelle ();
        break;
    case T_DROITE:
        morte_poser (NULL);
        shell_saisie_touche (SHELL_TOUCHE_DROITE, FALSE);
        /* Le curseur a bougé : ce qu'on croyait savoir du mot en cours ne
         * vaut plus. */
        K.espace_auto = FALSE;
        phrase_nouvelle ();
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
    case T_CALQUE:          /* par son geste, voir « Les bascules »       */
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
    if (t->genre == T_CALQUE) {
        /* En phase de capture : le geste passe avant le clic du bouton, et
         * le réclame. Voir « Les bascules » pour le pourquoi du glisser. */
        GtkGesture *geste = gtk_gesture_drag_new ();
        gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (geste),
                                                    GTK_PHASE_CAPTURE);
        g_signal_connect (geste, "drag-begin", G_CALLBACK (on_calque_appui), (gpointer) t);
        g_signal_connect (geste, "drag-end", G_CALLBACK (on_calque_fin), (gpointer) t);
        g_signal_connect (geste, "cancel", G_CALLBACK (on_calque_annule), (gpointer) t);
        gtk_widget_add_controller (b, GTK_EVENT_CONTROLLER (geste));
        g_ptr_array_add (K.boutons_calque, b);
    } else {
        g_signal_connect (b, "clicked", G_CALLBACK (on_clic), (gpointer) t);
    }

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
grille (const Couche *c, int espace)
{
    GtkWidget *g = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (g), espace);
    gtk_grid_set_column_spacing (GTK_GRID (g), espace);
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

/* `espace` : entre deux colonnes de la grille. Chaque touche en couvre
 * deux, l'espace compte donc aussi DANS la touche : à 8 px sur les dix
 * colonnes de la console, les touches tomberaient à 50 px (8 mm) ; à 4 px,
 * 54 px. Le plein format, qui a de la place, garde 8. */
/* La couche des lettres : des touches de tailles différentes, posées
 * exactement où le calcul les veut. GtkFixed et non GtkGrid : une grille
 * suppose des lignes et des colonnes, et c'est justement ce dont on sort. */
static GtkWidget *
disposition_organique (void)
{
    GtkWidget *fixe = gtk_fixed_new ();
    gtk_widget_add_css_class (fixe, "clavier-organique");
    int bas = 0;
    for (guint i = 0; i < G_N_ELEMENTS (CONSOLE_LETTRES); i++) {
        const Place *p = &CONSOLE_LETTRES[i];
        GtkWidget *b = touche_neuve (&p->touche);
        gtk_widget_set_hexpand (b, FALSE);
        gtk_widget_set_vexpand (b, FALSE);
        gtk_widget_set_size_request (b, p->l, p->h);
        /* La taille de la touche dit son importance : autant que
         * l'étiquette la dise aussi. */
        if (p->l >= 62)
            gtk_widget_add_css_class (b, "grande");
        else if (p->l <= 46)
            gtk_widget_add_css_class (b, "petite");
        g_autofree char *nuance = g_strdup_printf ("nuance-%d", p->nuance);
        gtk_widget_add_css_class (b, nuance);
        gtk_fixed_put (GTK_FIXED (fixe), b, p->x, p->y);
        bas = MAX (bas, p->y + p->h);
    }
    gtk_widget_set_size_request (fixe, LARGEUR_CONSOLE - 12, bas);
    return fixe;
}

static GtkWidget *
pile_neuve (const Couche *couches, int espace)
{
    GtkWidget *pile = gtk_stack_new ();
    gtk_widget_add_css_class (pile, "clavier");
    for (const Couche *c = couches; c->nom != NULL; c++)
        gtk_stack_add_named (GTK_STACK (pile), grille (c, espace), c->nom);
    return pile;
}

/* --- Les chiffres du plein format : le pavé de l'écran de connexion ------ */
static const Touche ENTREE_PAVE  = {"↵",   NULL,      T_ENTREE, 0, 0, 0};
static const Touche LETTRES_PAVE = {"abc", "lettres", T_COUCHE, 0, 0, 0};

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
fenetre_neuve (GtkApplication *app, gboolean haut, gboolean gauche, gboolean droite,
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
    gtk_layer_set_anchor (GTK_WINDOW (f), GTK_LAYER_SHELL_EDGE_TOP, haut);
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
    K.calque_une = FALSE;
    K.calque_tenu = NULL;
    afficher_etat ();
}

static void
appliquer (void)
{
    /* Le plein format vient quand un champ le demande, ou sur demande. La
     * console, elle, RESTE tant qu'elle est choisie — décision de
     * l'utilisateur : comme les manettes d'une console, et pour que l'écran
     * ne se recadre pas à chaque champ touché. ⌄ la range jusqu'au champ
     * suivant, ou jusqu'à l'icône du dock. */
    gboolean plein = K.ecrit && K.tablette && K.mode == MODE_PLEIN
                   && ((K.champ && !K.renvoye) || K.demande);
    gboolean deux  = K.ecrit && K.tablette && K.mode == MODE_CONSOLE && !K.renvoye;
    gboolean voir  = plein || deux;

    if (plein) {
        ajuster_largeur ();
        /* Un champ qui attend des chiffres ouvre le pavé ; tout autre revient
         * aux lettres s'il en héritait. */
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
    /* Un champ neuf : rien n'y est écrit de notre fait, et une phrase
     * commence. */
    K.espace_auto = FALSE;
    phrase_nouvelle ();
    if (but != SHELL_SAISIE_MOT_DE_PASSE && but != SHELL_SAISIE_CHIFFRES)
        maj_poser (MAJ_UNE);
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
    K.mode = g_strcmp0 (d, "console") == 0 ? MODE_CONSOLE : MODE_PLEIN;
}

static void
mode_ecrire (void)
{
    g_autofree char *chemin = chemin_etat ();
    g_autofree char *dossier = g_path_get_dirname (chemin);
    g_autoptr(GKeyFile) kf = g_key_file_new ();
    g_autoptr(GError) err = NULL;
    g_key_file_set_string (kf, "clavier", "disposition",
                           K.mode == MODE_CONSOLE ? "console" : "plein");
    if (g_mkdir_with_parents (dossier, 0700) != 0
        || !g_key_file_save_to_file (kf, chemin, &err))
        g_warning ("clavier à l'écran : disposition non gardée (%s) : %s", chemin,
                   err != NULL ? err->message : g_strerror (errno));
}

static void
changer_mode (void)
{
    K.mode = K.mode == MODE_PLEIN ? MODE_CONSOLE : MODE_PLEIN;
    au_repos ();
    mode_ecrire ();
    g_message ("clavier à l'écran : disposition %s",
               K.mode == MODE_PLEIN ? "plein format" : "console");
    /* On vient de toucher le clavier : il reste à l'écran dans sa nouvelle
     * forme, champ ou non. */
    K.demande = TRUE;
    K.renvoye = FALSE;
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
    const Couche *const groupes[] = { COUCHES_PLEIN, COUCHES_CONSOLE_G, COUCHES_CONSOLE_D };

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
    for (guint i = 0; i < G_N_ELEMENTS (CONSOLE_LETTRES); i++) {
        const Touche *t = &CONSOLE_LETTRES[i].touche;
        if (t->genre != T_TEXTE)
            continue;
        ajouter_texte (a, t->base);
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
    K.boutons_calque = g_ptr_array_new ();
    K.mot = g_string_new (NULL);
    K.precedent = g_string_new (NULL);
    K.mots_ok = shell_mots_init ();
    mode_lire ();

    /* Plein format : un bandeau sur toute la largeur, touches centrées. */
    K.plein_pile = pile_neuve (COUCHES_PLEIN, 8);
    gtk_stack_add_named (GTK_STACK (K.plein_pile), chiffres (), "chiffres");
    K.plein_interieur = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign (K.plein_interieur, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (K.plein_interieur), K.plein_pile);
    K.plein = fenetre_neuve (app, FALSE, TRUE, TRUE, 0, "claude-os-clavier",
                             "clavier-plein", K.plein_interieur);

    /* La console : deux colonnes sur toute la hauteur, collées aux bords,
     * chacune réservant sa largeur. labwc recadre alors de lui-même les
     * fenêtres agrandies entre elles — c'est le « recadrage » demandé.
     * Les touches descendent de HAUT_GAUCHE / HAUT_DROITE, dans la bande que les pouces
     * atteignent ; le reste de la colonne est vide. */
    K.gauche_pile = pile_neuve (COUCHES_CONSOLE_G, 4);
    gtk_stack_add_named (GTK_STACK (K.gauche_pile), disposition_organique (), "lettres");
    gtk_stack_set_visible_child_name (GTK_STACK (K.gauche_pile), "lettres");
    gtk_widget_set_size_request (K.gauche_pile, LARGEUR_CONSOLE - 12, -1);
    gtk_widget_set_valign (K.gauche_pile, GTK_ALIGN_START);
    gtk_widget_set_vexpand (K.gauche_pile, FALSE);

    /* Les suggestions par-dessus les touches, dans la même colonne : elles
     * restent en place quand on change de calque, d'où leur place hors de
     * la pile. */
    K.suggestions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class (K.suggestions, "clavier-suggestions");
    gtk_widget_set_margin_top (K.suggestions, HAUT_SUGGESTIONS);
    for (guint i = 0; i < SUGGESTIONS; i++) {
        K.sugg[i] = gtk_button_new_with_label ("");
        gtk_widget_set_can_focus (K.sugg[i], FALSE);
        gtk_widget_set_focus_on_click (K.sugg[i], FALSE);
        gtk_widget_add_css_class (K.sugg[i], "clavier-suggestion");
        gtk_widget_set_hexpand (K.sugg[i], TRUE);
        gtk_widget_set_sensitive (K.sugg[i], FALSE);
        gtk_widget_set_size_request (K.sugg[i], -1, HAUTEUR_SUGGESTION);
        g_signal_connect (K.sugg[i], "clicked", G_CALLBACK (on_suggestion), NULL);
        gtk_box_append (GTK_BOX (K.suggestions), K.sugg[i]);
    }
    GtkWidget *colonne = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append (GTK_BOX (colonne), K.suggestions);
    gtk_box_append (GTK_BOX (colonne), K.gauche_pile);
    K.gauche = fenetre_neuve (app, TRUE, TRUE, FALSE, 0, "claude-os-clavier-gauche",
                              "console-gauche", colonne);
    GtkWidget *fonctions = pile_neuve (COUCHES_CONSOLE_D, 4);
    gtk_widget_set_size_request (fonctions, LARGEUR_CONSOLE - 12, -1);
    gtk_widget_set_margin_top (fonctions, HAUT_DROITE);
    gtk_widget_set_valign (fonctions, GTK_ALIGN_START);
    gtk_widget_set_vexpand (fonctions, FALSE);
    K.droite = fenetre_neuve (app, TRUE, FALSE, TRUE, 0, "claude-os-clavier-droite",
                              "console-droite", fonctions);

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
