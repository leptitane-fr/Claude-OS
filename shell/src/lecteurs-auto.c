/* =========================================================================
 * claude-os-lecteurs-auto — connecte les lecteurs marques « auto »
 *
 * Lance par l'autostart de labwc, en arriere-plan. Il connecte les lecteurs
 * dont la case « Se connecter a l'ouverture de session » est cochee, puis
 * il rend la main. Il ne reste pas resident : il n'a rien a surveiller.
 *
 * SANS GTK, ET C'EST LE POINT
 *
 * Un runtime GTK4 coute environ 40 Mo mesures sur cette machine, qui n'a que
 * 4 Go soudes. Les payer au demarrage pour un programme sans fenetre, qui
 * vit trois secondes, serait absurde. Le modele des lecteurs ne connait que
 * GIO : il se relie donc ici sans une ligne de plus.
 *
 * IL NE FAIT PAS ATTENDRE LA SESSION
 *
 * L'autostart le lance en arriere-plan, et chaque connexion est asynchrone.
 * Un serveur eteint coute son delai TCP -- a ce programme seul, pendant que
 * le bureau s'ouvre normalement. Un partage injoignable ne doit jamais etre
 * la raison pour laquelle une session met une minute a apparaitre.
 * ========================================================================= */
#include "reseau.h"

static int restants = 0;
static GMainLoop *boucle = NULL;

static void
on_fini (const Lecteur *l, GError *erreur, gpointer data)
{
    (void) data;

    /* La sortie part dans ~/.local/state/claude-os/shell.log. Un echec doit
     * s'y lire : l'invariant n°4 de ce projet vaut aussi pour un programme
     * qui n'a pas de fenetre ou se plaindre. */
    if (erreur != NULL)
        g_printerr ("lecteur « %s » : %s\n", l->nom, erreur->message);
    else
        g_print ("lecteur « %s » : connecté\n", l->nom);

    if (--restants <= 0)
        g_main_loop_quit (boucle);
}

int
main (int argc, char **argv)
{
    (void) argc; (void) argv;

    g_autoptr(GPtrArray) lecteurs = reseau_charger ();
    boucle = g_main_loop_new (NULL, FALSE);

    for (guint i = 0; i < lecteurs->len; i++) {
        Lecteur *l = g_ptr_array_index (lecteurs, i);

        if (!l->automatique || reseau_est_connecte (l))
            continue;

        restants++;
        reseau_connecter (l, NULL, FALSE, on_fini, NULL);
    }

    if (restants == 0) {
        g_main_loop_unref (boucle);
        return 0;
    }

    g_main_loop_run (boucle);
    g_main_loop_unref (boucle);
    return 0;
}
