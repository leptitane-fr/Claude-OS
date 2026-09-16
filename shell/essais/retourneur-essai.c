/* =========================================================================
 * Claude OS — Banc du retourneur : une surface à deux faces, sans le dock
 *
 * Le retourneur est la pièce dont dépend toute la surface d'outils
 * (docs/14) : si labwc 0.8.3 ne supporte pas une pilule qui change de
 * largeur sous une rotation, la thèse entière se renégocie. Mieux vaut le
 * savoir maintenant que cinq étapes plus loin.
 *
 * Ce programme monte donc le retourneur SEUL, dans une fenêtre layer-shell
 * ancrée en bas comme le dock, avec deux faces factices de largeurs très
 * différentes — c'est le cas défavorable, celui qui ferait bouger la
 * surface si la parade ne tenait pas.
 *
 * CE QU'IL MESURE, et qu'on ne peut pas juger à l'œil :
 *
 *   - LA TAILLE DE LA SURFACE À CHAQUE IMAGE. Si elle change pendant la
 *     rotation, la parade de retourneur.h ne tient pas, et tout popover
 *     ouvert partirait hors de l'écran.
 *   - LE NOMBRE D'IMAGES PAR RETOURNEMENT, et surtout QU'IL N'Y EN A AUCUNE
 *     AU REPOS. C'est la discipline d'énergie du projet, et elle s'était
 *     déjà démentie une fois sur le lecteur vidéo.
 *   - QUE LE POPOVER EST BIEN FERMÉ avant le mouvement. Le rappel existe ;
 *     reste à prouver qu'il est appelé, et une seule fois.
 *
 * Il se pilote par le bus, comme le dock :
 *
 *   gapplication action os.claude.shell.essai-retourneur retourner
 *   gapplication action os.claude.shell.essai-retourneur popover
 *   gapplication action os.claude.shell.essai-retourneur compter
 *   gapplication action os.claude.shell.essai-retourneur remettre
 *
 * Chaque ligne de trace commence par « [banc] » : c'est ce que
 * banc-retourneur.sh lit.
 * ========================================================================= */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

#include "retourneur.h"

static struct {
    GtkWidget       *fenetre;
    ShellRetourneur *r;
    GtkWidget       *popover;
    GtkWidget       *ancre;

    /* Ce qui se compte. Les allocations tiennent lieu d'images : le
     * retourneur en demande une par battement d'horloge, et aucune quand il
     * dort. */
    int  allocations;
    int  departs;
    int  fins;
    int  popovers_fermes;

    /* La taille de la surface, pour voir si elle bouge. */
    int  largeur_vue, hauteur_vue;
    int  changements_de_taille;
} E;

/* -------------------------------------------------------------------------
 * Deux faces de largeurs très différentes
 * ------------------------------------------------------------------------- */
static GtkWidget *
face (const char *classe, const char *prefixe, int combien)
{
    GtkWidget *b = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class (b, "dock");
    gtk_widget_add_css_class (b, classe);
    gtk_widget_set_halign (b, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (b, GTK_ALIGN_END);

    for (int i = 0; i < combien; i++) {
        g_autofree char *t = g_strdup_printf ("%s%d", prefixe, i);
        GtkWidget *bouton = gtk_button_new_with_label (t);
        gtk_widget_add_css_class (bouton, "dock-item");
        gtk_box_append (GTK_BOX (b), bouton);
    }
    return b;
}

/* -------------------------------------------------------------------------
 * Les rappels du retourneur
 * ------------------------------------------------------------------------- */
static void
sur_depart (gpointer data)
{
    (void) data;
    E.departs++;

    /* CE QUE LE DOCK FERA ICI. Un popover laissé ouvert pendant que sa face
     * tourne resterait planté au milieu de l'écran -- et labwc le replacerait
     * depuis une origine périmée si la surface venait à changer de taille. */
    if (E.popover != NULL && gtk_widget_get_visible (E.popover)) {
        gtk_popover_popdown (GTK_POPOVER (E.popover));
        E.popovers_fermes++;
        g_print ("[banc] popover fermé au départ\n");
    }
    g_print ("[banc] depart %d\n", E.departs);
}

static void
sur_fin (gboolean arriere, gpointer data)
{
    (void) data;
    E.fins++;
    g_print ("[banc] fin %d face=%s allocations=%d taille=%dx%d changements=%d\n",
             E.fins, arriere ? "arriere" : "avant",
             E.allocations, E.largeur_vue, E.hauteur_vue,
             E.changements_de_taille);
}

static void
sur_allocation (int x, int y, int largeur, int hauteur, gpointer data)
{
    (void) data;
    E.allocations++;

    /* LA MESURE QUI COMPTE. La surface -- donc le retourneur, qui l'occupe
     * entière -- doit garder la même taille d'un bout à l'autre du
     * mouvement. C'est elle qu'on surveille, pas la face : la face, elle,
     * a le droit de changer, c'est même tout l'intérêt. */
    int lw = gtk_widget_get_width (GTK_WIDGET (E.r));
    int lh = gtk_widget_get_height (GTK_WIDGET (E.r));

    if (lw > 0 && (lw != E.largeur_vue || lh != E.hauteur_vue)) {
        if (E.largeur_vue != 0) {
            E.changements_de_taille++;
            g_print ("[banc] LA SURFACE A CHANGÉ : %dx%d → %dx%d\n",
                     E.largeur_vue, E.hauteur_vue, lw, lh);
        }
        E.largeur_vue = lw;
        E.hauteur_vue = lh;
    }

    g_print ("[banc] alloc %d face=%d,%d %dx%d surface=%dx%d\n",
             E.allocations, x, y, largeur, hauteur, lw, lh);
}

/* -------------------------------------------------------------------------
 * Les actions du bus
 * ------------------------------------------------------------------------- */
static void
act_retourner (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    gboolean vers = !shell_retourneur_face (E.r);
    g_print ("[banc] retourner vers %s\n", vers ? "arriere" : "avant");
    shell_retourneur_montrer (E.r, vers);
}

static void
act_popover (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;

    /* Un popover ne s'accroche qu'à une face ARRIVÉE ET IMMOBILE : posé sur
     * une face en rotation, il serait placé une fois pour toutes là où elle
     * se trouvait. La règle est dans retourneur.h ; le banc la respecte
     * pour que ce qu'il éprouve soit ce que le dock fera. */
    if (!shell_retourneur_en_place (E.r)) {
        g_print ("[banc] popover refusé : le retourneur bouge\n");
        return;
    }
    gtk_popover_popup (GTK_POPOVER (E.popover));
    g_print ("[banc] popover ouvert\n");
}

static void
act_compter (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    g_print ("[banc] compte allocations=%d departs=%d fins=%d "
             "popovers_fermes=%d changements=%d surface=%dx%d\n",
             E.allocations, E.departs, E.fins, E.popovers_fermes,
             E.changements_de_taille, E.largeur_vue, E.hauteur_vue);
}

static void
act_remettre (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    E.allocations = 0;
    g_print ("[banc] compteur d'allocations remis à zéro\n");
}

static const GActionEntry ACTIONS[] = {
    { "retourner", act_retourner, NULL, NULL, NULL, { 0 } },
    { "popover",   act_popover,   NULL, NULL, NULL, { 0 } },
    { "compter",   act_compter,   NULL, NULL, NULL, { 0 } },
    { "remettre",  act_remettre,  NULL, NULL, NULL, { 0 } },
};

/* ------------------------------------------------------------------------- */
static void
on_activate (GtkApplication *app, gpointer data)
{
    (void) data;

    GtkWidget *avant   = face ("banc-avant",   "A", 5);
    GtkWidget *arriere = face ("banc-arriere", "B", 14);

    E.r = SHELL_RETOURNEUR (shell_retourneur_new (avant, arriere));
    shell_retourneur_sur_depart     (E.r, sur_depart, NULL);
    shell_retourneur_sur_fin        (E.r, sur_fin, NULL);
    shell_retourneur_sur_allocation (E.r, sur_allocation, NULL);

    E.fenetre = gtk_application_window_new (app);
    gtk_widget_add_css_class (E.fenetre, "shell");
    gtk_window_set_child (GTK_WINDOW (E.fenetre), GTK_WIDGET (E.r));

    /* Le popover s'accroche au premier bouton de la face avant : c'est le
     * cas que le dock connaîtra, la liste des fenêtres au survol. */
    E.ancre = gtk_widget_get_first_child (avant);
    E.popover = gtk_popover_new ();
    gtk_popover_set_autohide (GTK_POPOVER (E.popover), FALSE);
    gtk_popover_set_position (GTK_POPOVER (E.popover), GTK_POS_TOP);
    gtk_popover_set_child (GTK_POPOVER (E.popover),
                           gtk_label_new ("une liste de fenêtres"));
    gtk_widget_set_parent (E.popover, E.ancre);

    gtk_layer_init_for_window (GTK_WINDOW (E.fenetre));
    gtk_layer_set_layer (GTK_WINDOW (E.fenetre), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor (GTK_WINDOW (E.fenetre), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_exclusive_zone (GTK_WINDOW (E.fenetre), 0);

    g_action_map_add_action_entries (G_ACTION_MAP (app), ACTIONS,
                                     G_N_ELEMENTS (ACTIONS), NULL);

    gtk_window_present (GTK_WINDOW (E.fenetre));
    g_print ("[banc] prêt\n");
}

int
main (int argc, char **argv)
{
    GtkApplication *app = gtk_application_new ("os.claude.shell.essai-retourneur",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
    int r = g_application_run (G_APPLICATION (app), argc, argv);
    g_object_unref (app);
    return r;
}
