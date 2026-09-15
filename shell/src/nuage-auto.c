/* =========================================================================
 * claude-os-nuage-auto — connecte les lecteurs nuage marques « auto »
 *
 * Lance par l'autostart de labwc, en arriere-plan. Il monte les lecteurs
 * dont la case est cochee, puis rend la main. Il ne reste pas resident : il
 * n'a rien a surveiller.
 *
 * SANS GTK, ET C'EST LE POINT
 *
 * Un runtime GTK4 coute environ 40 Mo mesures sur cette machine, qui n'a que
 * 4 Go soudes. Les payer au demarrage pour un programme sans fenetre, qui
 * vit quelques secondes, serait absurde. Le modele du nuage ne connait que
 * GIO : il se relie donc ici sans une ligne de plus. Meme raisonnement, et
 * meme forme, que claude-os-lecteurs-auto.
 *
 * IL NE FAIT PAS ATTENDRE LA SESSION
 *
 * L'autostart le lance en arriere-plan, et chaque montage est asynchrone. Un
 * service injoignable coute son delai a ce programme seul, pendant que le
 * bureau s'ouvre normalement.
 *
 * LE TROUSSEAU DOIT ETRE OUVERT
 *
 * La configuration de rclone est chiffree, et sa phrase vit au trousseau.
 * PAM le deverrouille a l'ouverture de session, mais rien ne garantit qu'il
 * ait fini quand l'autostart demarre. Un echec ici n'est donc pas forcement
 * definitif -- il est ecrit au journal, et l'utilisateur peut monter d'un
 * clic depuis Fichiers. On ne reessaie pas en boucle : une boucle qui
 * interroge le trousseau toutes les secondes est exactement le genre de
 * scrutation que ce projet refuse.
 * ========================================================================= */
#include "nuage.h"

#include <stdio.h>

static int restants = 0;
static GMainLoop *boucle = NULL;

static void
on_fini (const LecteurNuage *l, GError *erreur, gpointer data)
{
    (void) data;

    /* La sortie part dans ~/.local/state/claude-os/shell.log. Un echec doit
     * s'y lire : l'invariant n°4 vaut aussi pour un programme sans fenetre
     * ou se plaindre.
     *
     * fprintf ET NON g_print : ce dernier transcode vers la locale, et la
     * locale de ce programme est « C ». Mesure du 15 septembre 2026 :
     * « lecteur nuage ? Google Drive ? : connect? ». docs/09 impute le
     * symptome a l'absence de locale sous systemd ; la cause est plus
     * generale et vaut ici, dans une session ou LANG=fr_FR.UTF-8 est
     * pourtant pose -- un programme C n'herite PAS de la locale tout seul,
     * il faut setlocale(), et seul gtk_init() le fait. Un programme GIO pur
     * reste donc en « C » et perd tous ses accents. fprintf, lui, ecrit les
     * octets tels quels. */
    if (erreur != NULL)
        fprintf (stderr, "lecteur nuage « %s » : %s\n", l->nom, erreur->message);
    else
        fprintf (stdout, "lecteur nuage « %s » : connecté\n", l->nom);

    if (--restants <= 0)
        g_main_loop_quit (boucle);
}

int
main (int argc, char **argv)
{
    (void) argc; (void) argv;

    /* rclone absent : le dire une fois, et sortir. Laisser chaque montage
     * echouer separement noierait la cause dans quatre messages de bas
     * niveau qui ne la nomment pas. */
    if (!nuage_outil_present ()) {
        g_autoptr(GPtrArray) l = nuage_charger ();
        if (l->len > 0)
            fputs ("lecteurs nuage : rclone n'est pas installé, aucun montage tenté\n", stderr);
        return 0;
    }

    g_autoptr(GPtrArray) lecteurs = nuage_charger ();
    boucle = g_main_loop_new (NULL, FALSE);

    for (guint i = 0; i < lecteurs->len; i++) {
        LecteurNuage *l = g_ptr_array_index (lecteurs, i);

        if (!l->automatique || nuage_est_connecte (l))
            continue;

        restants++;
        nuage_connecter (l, on_fini, NULL);
    }

    if (restants == 0) {
        g_main_loop_unref (boucle);
        return 0;
    }

    g_main_loop_run (boucle);
    g_main_loop_unref (boucle);
    return 0;
}
