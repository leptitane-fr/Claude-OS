/* =========================================================================
 * Claude OS — claviers à l'écran
 *
 * Voir clavier.h pour le pourquoi. Ce fichier ne contient que le dessin.
 *
 * LE PIÈGE QUI TUE CES DEUX CLAVIERS, ET IL EST DANS UNE SEULE LIGNE
 *
 * Un GtkButton prend le focus quand on le presse. Sur cet écran, le focus
 * appartient au champ de saisie : c'est lui qui reçoit la frappe du clavier
 * PHYSIQUE. Sans précaution, le premier appui sur une touche à l'écran le
 * lui vole, et la frappe physique cesse de fonctionner — silencieusement, et
 * seulement après un clic, donc jamais au premier essai.
 *
 * L'utilisateur a demandé les deux modes de saisie. Ils doivent coexister à
 * la milliseconde près, sans qu'on ait à cliquer dans le champ pour revenir
 * au clavier. D'où touche_neuve() ci-dessous, par où passent TOUTES les
 * touches des deux claviers, et qui pose les deux propriétés qu'il faut.
 * ========================================================================= */

#include "clavier.h"

/* La grille des claviers complets fait vingt colonnes. Une touche ordinaire
 * en occupe deux, une touche de commande trois : toutes les rangées tombent
 * juste, et aucune ne se décale d'un pixel par rapport aux autres. */
#define COLONNES 20

/* -------------------------------------------------------------------------
 * La brique commune
 * ------------------------------------------------------------------------- */

/* Libère la donnée d'un rappel quand son signal disparaît.
 *
 * Une fonction plutôt qu'un « (GClosureNotify) g_free » : la signature
 * attendue prend DEUX arguments, g_free un seul, et le cast entre types de
 * fonction incompatibles est précisément ce que -Wcast-function-type
 * signale. Sur une ABI où les arguments ne passent pas par registre, il
 * corromprait la pile. */
static void
libere_donnees (gpointer donnees, GClosure *fermeture)
{
    (void) fermeture;
    g_free (donnees);
}
static GtkWidget *
touche_neuve (const char *etiquette, gboolean speciale)
{
    GtkWidget *b = gtk_button_new_with_label (etiquette);

    /* LES DEUX LIGNES DE L'EN-TÊTE. can-focus interdit au bouton d'entrer
     * dans la chaîne de focus ; focus-on-click interdit la prise de focus
     * au moment du clic. Il faut les DEUX : la première seule laisse encore
     * GTK déplacer le focus sur un clic dans certains conteneurs. */
    gtk_widget_set_can_focus (b, FALSE);
    gtk_widget_set_focus_on_click (b, FALSE);

    gtk_widget_add_css_class (b, "clavier-touche");
    if (speciale)
        gtk_widget_add_css_class (b, "speciale");

    gtk_widget_set_hexpand (b, TRUE);
    gtk_widget_set_vexpand (b, TRUE);
    return b;
}

/* -------------------------------------------------------------------------
 * Le pavé numérique
 * ------------------------------------------------------------------------- */
typedef struct {
    ShellPaveFn fn;
    gpointer    donnees;
    char        c;
} Pave;

static void
on_pave (GtkButton *b, gpointer data)
{
    (void) b;
    Pave *p = data;
    p->fn (p->c, p->donnees);
}

GtkWidget *
shell_clavier_pave (ShellPaveFn sur_touche, gpointer donnees)
{
    GtkWidget *grille = gtk_grid_new ();
    gtk_widget_add_css_class (grille, "clavier-pave");
    gtk_grid_set_row_spacing (GTK_GRID (grille), 10);
    gtk_grid_set_column_spacing (GTK_GRID (grille), 10);
    gtk_grid_set_row_homogeneous (GTK_GRID (grille), TRUE);
    gtk_grid_set_column_homogeneous (GTK_GRID (grille), TRUE);

    /* Disposition du téléphone — 1 en haut à gauche — et non celle du pavé
     * numérique d'un clavier, où le 1 est en bas. C'est celle qu'on a sous
     * les doigts depuis vingt ans dès qu'un écran demande un code. */
    static const char CHIFFRES[] = "123456789";

    for (int i = 0; i < 9; i++) {
        Pave *p = g_new0 (Pave, 1);
        p->fn = sur_touche;
        p->donnees = donnees;
        p->c = CHIFFRES[i];

        char etiquette[2] = { CHIFFRES[i], '\0' };
        GtkWidget *b = touche_neuve (etiquette, FALSE);
        g_signal_connect_data (b, "clicked", G_CALLBACK (on_pave), p,
                               libere_donnees, 0);
        gtk_grid_attach (GTK_GRID (grille), b, i % 3, i / 3, 1, 1);
    }

    /* Le zéro au centre de la dernière rangée, et l'effacement à sa droite.
     * La case de gauche reste VIDE : y mettre « valider » exposerait à
     * l'atteindre en visant le zéro, et la validation se fait de toute façon
     * toute seule au sixième chiffre. */
    Pave *z = g_new0 (Pave, 1);
    z->fn = sur_touche; z->donnees = donnees; z->c = '0';
    GtkWidget *b0 = touche_neuve ("0", FALSE);
    g_signal_connect_data (b0, "clicked", G_CALLBACK (on_pave), z,
                           libere_donnees, 0);
    gtk_grid_attach (GTK_GRID (grille), b0, 1, 3, 1, 1);

    Pave *e = g_new0 (Pave, 1);
    e->fn = sur_touche; e->donnees = donnees; e->c = '\b';
    GtkWidget *be = touche_neuve ("⌫", TRUE);
    gtk_widget_set_tooltip_text (be, "Effacer");
    g_signal_connect_data (be, "clicked", G_CALLBACK (on_pave), e,
                           libere_donnees, 0);
    gtk_grid_attach (GTK_GRID (grille), be, 2, 3, 1, 1);

    return grille;
}

/* -------------------------------------------------------------------------
 * Le clavier complet
 * ------------------------------------------------------------------------- */
typedef enum {
    T_TEXTE,      /* insère son texte                                        */
    T_MAJ,        /* bascule les majuscules                                  */
    T_EFFACER,    /* retire le caractère à gauche du curseur                 */
    T_COUCHE,     /* passe des lettres aux symboles, et retour               */
    T_ENTREE      /* valide, comme la touche Entrée du clavier physique      */
} Genre;

typedef struct {
    const char *etiquette;
    const char *texte;    /* NULL : l'étiquette fait office de texte         */
    Genre       genre;
    int         largeur;  /* en colonnes de la grille, sur COLONNES          */
} Touche;

typedef struct {
    GtkEditable   *cible;
    ShellEntreeFn  sur_entree;
    gpointer       donnees;
    gboolean       maj;
    GtkWidget     *pile;        /* lettres / symboles */
    GtkWidget     *bouton_maj;
    GPtrArray     *lettres;     /* les touches dont l'étiquette suit Maj */
} Clavier;

static void
clavier_libere (gpointer data)
{
    Clavier *k = data;
    g_ptr_array_unref (k->lettres);
    g_free (k);
}

/* --- les dispositions ---------------------------------------------------
 *
 * Azerty, la même que celle du clavier physique (XKB_DEFAULT_LAYOUT=fr).
 * Une rangée de chiffres en tête plutôt qu'une couche à part : un mot de
 * passe en contient presque toujours, et les faire chercher derrière une
 * bascule est une faute d'usage.
 *
 * Chaque rangée totalise exactement COLONNES. Une touche ordinaire vaut 2,
 * une touche de commande 3, la barre d'espace 10. */
#define ORD 2
#define CMD 3

static const Touche L0[] = {
    {"1",NULL,T_TEXTE,ORD},{"2",NULL,T_TEXTE,ORD},{"3",NULL,T_TEXTE,ORD},
    {"4",NULL,T_TEXTE,ORD},{"5",NULL,T_TEXTE,ORD},{"6",NULL,T_TEXTE,ORD},
    {"7",NULL,T_TEXTE,ORD},{"8",NULL,T_TEXTE,ORD},{"9",NULL,T_TEXTE,ORD},
    {"0",NULL,T_TEXTE,ORD},{NULL,NULL,T_TEXTE,0}
};
static const Touche L1[] = {
    {"a",NULL,T_TEXTE,ORD},{"z",NULL,T_TEXTE,ORD},{"e",NULL,T_TEXTE,ORD},
    {"r",NULL,T_TEXTE,ORD},{"t",NULL,T_TEXTE,ORD},{"y",NULL,T_TEXTE,ORD},
    {"u",NULL,T_TEXTE,ORD},{"i",NULL,T_TEXTE,ORD},{"o",NULL,T_TEXTE,ORD},
    {"p",NULL,T_TEXTE,ORD},{NULL,NULL,T_TEXTE,0}
};
static const Touche L2[] = {
    {"q",NULL,T_TEXTE,ORD},{"s",NULL,T_TEXTE,ORD},{"d",NULL,T_TEXTE,ORD},
    {"f",NULL,T_TEXTE,ORD},{"g",NULL,T_TEXTE,ORD},{"h",NULL,T_TEXTE,ORD},
    {"j",NULL,T_TEXTE,ORD},{"k",NULL,T_TEXTE,ORD},{"l",NULL,T_TEXTE,ORD},
    {"m",NULL,T_TEXTE,ORD},{NULL,NULL,T_TEXTE,0}
};
static const Touche L3[] = {
    {"⇧",NULL,T_MAJ,CMD},
    {"w",NULL,T_TEXTE,ORD},{"x",NULL,T_TEXTE,ORD},{"c",NULL,T_TEXTE,ORD},
    {"v",NULL,T_TEXTE,ORD},{"b",NULL,T_TEXTE,ORD},{"n",NULL,T_TEXTE,ORD},
    {"-",NULL,T_TEXTE,ORD},
    {"⌫",NULL,T_EFFACER,CMD},{NULL,NULL,T_TEXTE,0}
};
static const Touche L4[] = {
    {"&#",NULL,T_COUCHE,CMD},{"@",NULL,T_TEXTE,ORD},
    {" ", " ",T_TEXTE,10},
    {".",NULL,T_TEXTE,ORD},{"↵",NULL,T_ENTREE,CMD},{NULL,NULL,T_TEXTE,0}
};

static const Touche S0[] = {
    {"1",NULL,T_TEXTE,ORD},{"2",NULL,T_TEXTE,ORD},{"3",NULL,T_TEXTE,ORD},
    {"4",NULL,T_TEXTE,ORD},{"5",NULL,T_TEXTE,ORD},{"6",NULL,T_TEXTE,ORD},
    {"7",NULL,T_TEXTE,ORD},{"8",NULL,T_TEXTE,ORD},{"9",NULL,T_TEXTE,ORD},
    {"0",NULL,T_TEXTE,ORD},{NULL,NULL,T_TEXTE,0}
};
static const Touche S1[] = {
    {"@",NULL,T_TEXTE,ORD},{"#",NULL,T_TEXTE,ORD},{"€",NULL,T_TEXTE,ORD},
    {"_",NULL,T_TEXTE,ORD},{"&",NULL,T_TEXTE,ORD},{"-",NULL,T_TEXTE,ORD},
    {"+",NULL,T_TEXTE,ORD},{"(",NULL,T_TEXTE,ORD},{")",NULL,T_TEXTE,ORD},
    {"/",NULL,T_TEXTE,ORD},{NULL,NULL,T_TEXTE,0}
};
static const Touche S2[] = {
    {"*",NULL,T_TEXTE,ORD},{"\"",NULL,T_TEXTE,ORD},{"'",NULL,T_TEXTE,ORD},
    {":",NULL,T_TEXTE,ORD},{";",NULL,T_TEXTE,ORD},{"!",NULL,T_TEXTE,ORD},
    {"?",NULL,T_TEXTE,ORD},{"=",NULL,T_TEXTE,ORD},{"%",NULL,T_TEXTE,ORD},
    {"$",NULL,T_TEXTE,ORD},{NULL,NULL,T_TEXTE,0}
};
static const Touche S3[] = {
    {"\\",NULL,T_TEXTE,CMD},
    {"<",NULL,T_TEXTE,ORD},{">",NULL,T_TEXTE,ORD},{"{",NULL,T_TEXTE,ORD},
    {"}",NULL,T_TEXTE,ORD},{"[",NULL,T_TEXTE,ORD},{"]",NULL,T_TEXTE,ORD},
    {"~",NULL,T_TEXTE,ORD},
    {"⌫",NULL,T_EFFACER,CMD},{NULL,NULL,T_TEXTE,0}
};
static const Touche S4[] = {
    {"abc",NULL,T_COUCHE,CMD},{"^",NULL,T_TEXTE,ORD},
    {" ", " ",T_TEXTE,10},
    {",",NULL,T_TEXTE,ORD},{"↵",NULL,T_ENTREE,CMD},{NULL,NULL,T_TEXTE,0}
};

/* --- l'action d'une touche ---------------------------------------------- */
static void
inserer (Clavier *k, const char *texte)
{
    g_autofree char *majuscule = NULL;

    /* Seules les lettres suivent la bascule. Les chiffres et la ponctuation
     * de ce clavier n'ont pas de « seconde valeur » : le clavier physique en
     * a une, à l'écran ce serait deviner. */
    if (k->maj && g_ascii_isalpha (texte[0]) && texte[1] == '\0') {
        majuscule = g_strdup (texte);
        majuscule[0] = g_ascii_toupper (majuscule[0]);
        texte = majuscule;
    }

    int position = gtk_editable_get_position (k->cible);
    gtk_editable_insert_text (k->cible, texte, -1, &position);
    gtk_editable_set_position (k->cible, position);
}

static void
maj_etiquettes (Clavier *k)
{
    for (guint i = 0; i < k->lettres->len; i++) {
        GtkWidget *b = g_ptr_array_index (k->lettres, i);
        const char *base = g_object_get_data (G_OBJECT (b), "lettre");
        char etiquette[2] = { k->maj ? g_ascii_toupper (base[0]) : base[0], '\0' };
        gtk_button_set_label (GTK_BUTTON (b), etiquette);
    }

    if (k->maj)
        gtk_widget_add_css_class (k->bouton_maj, "active");
    else
        gtk_widget_remove_css_class (k->bouton_maj, "active");
}

typedef struct {
    Clavier    *k;
    const Touche *t;
} Appui;

static void
on_touche (GtkButton *b, gpointer data)
{
    (void) b;
    Appui *a = data;
    Clavier *k = a->k;

    switch (a->t->genre) {
    case T_TEXTE:
        inserer (k, a->t->texte != NULL ? a->t->texte : a->t->etiquette);
        break;

    case T_MAJ:
        /* Une bascule qui TIENT, et non un « une seule fois » à la mode des
         * téléphones. Un mot de passe contient volontiers plusieurs
         * majuscules d'affilée, et une bascule qui retombe toute seule
         * oblige à la represser entre chaque — sans jamais dire qu'elle est
         * retombée. */
        k->maj = !k->maj;
        maj_etiquettes (k);
        break;

    case T_EFFACER: {
        int position = gtk_editable_get_position (k->cible);
        if (position > 0) {
            gtk_editable_delete_text (k->cible, position - 1, position);
            gtk_editable_set_position (k->cible, position - 1);
        }
        break;
    }

    case T_COUCHE:
        gtk_stack_set_visible_child_name (
            GTK_STACK (k->pile),
            g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (k->pile)),
                       "lettres") == 0 ? "symboles" : "lettres");
        break;

    case T_ENTREE:
        if (k->sur_entree != NULL)
            k->sur_entree (k->donnees);
        break;
    }
}

static void
rangee (GtkWidget *grille, int y, const Touche *touches, Clavier *k)
{
    int x = 0;

    for (const Touche *t = touches; t->etiquette != NULL; t++) {
        GtkWidget *b = touche_neuve (t->etiquette, t->genre != T_TEXTE);

        if (t->genre == T_ENTREE)
            gtk_widget_add_css_class (b, "valider");

        /* Les lettres sont retenues : leur étiquette change avec Maj. */
        if (t->genre == T_TEXTE && g_ascii_isalpha (t->etiquette[0])
            && t->etiquette[1] == '\0') {
            g_object_set_data_full (G_OBJECT (b), "lettre",
                                    g_strdup (t->etiquette), g_free);
            g_ptr_array_add (k->lettres, b);
        }

        if (t->genre == T_MAJ)
            k->bouton_maj = b;

        Appui *a = g_new0 (Appui, 1);
        a->k = k;
        a->t = t;                 /* tables statiques : elles survivent au clavier */
        g_signal_connect_data (b, "clicked", G_CALLBACK (on_touche), a,
                               libere_donnees, 0);

        gtk_grid_attach (GTK_GRID (grille), b, x, y, t->largeur, 1);
        x += t->largeur;
    }

    g_return_if_fail (x == COLONNES);   /* une rangée bancale se voit tout de suite */
}

static GtkWidget *
couche (const Touche *const rangees[5], Clavier *k)
{
    GtkWidget *grille = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grille), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grille), 8);
    gtk_grid_set_row_homogeneous (GTK_GRID (grille), TRUE);
    gtk_grid_set_column_homogeneous (GTK_GRID (grille), TRUE);

    for (int y = 0; y < 5; y++)
        rangee (grille, y, rangees[y], k);

    return grille;
}

GtkWidget *
shell_clavier_azerty (GtkEditable *cible, ShellEntreeFn sur_entree,
                      gpointer donnees)
{
    g_return_val_if_fail (GTK_IS_EDITABLE (cible), NULL);

    Clavier *k = g_new0 (Clavier, 1);
    k->cible = cible;
    k->sur_entree = sur_entree;
    k->donnees = donnees;
    k->lettres = g_ptr_array_new ();

    k->pile = gtk_stack_new ();
    gtk_widget_add_css_class (k->pile, "clavier");

    static const Touche *const LETTRES[5]  = { L0, L1, L2, L3, L4 };
    static const Touche *const SYMBOLES[5] = { S0, S1, S2, S3, S4 };

    gtk_stack_add_named (GTK_STACK (k->pile), couche (LETTRES, k),  "lettres");
    gtk_stack_add_named (GTK_STACK (k->pile), couche (SYMBOLES, k), "symboles");
    gtk_stack_set_visible_child_name (GTK_STACK (k->pile), "lettres");

    /* La structure meurt avec la pile : aucun rappel ne peut la survivre. */
    g_object_set_data_full (G_OBJECT (k->pile), "clavier", k, clavier_libere);

    return k->pile;
}
