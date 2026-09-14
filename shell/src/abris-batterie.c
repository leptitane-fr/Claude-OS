/* =========================================================================
 * Claude-OS Shell — la definition des abris de fin de batterie
 *
 * SEPARE DE batterie.c POUR LA MEME RAISON QUE modes-energie.c L'EST DE
 * energie.c : le panneau de reglages ne veut qu'une liste de libelles, et il
 * n'a aucune raison de lier le module de surveillance -- ses minuteries, sa
 * lecture de sysfs et ses appels a logind -- pour afficher quatre lignes
 * dans une liste deroulante.
 *
 * Elle vit donc dans « commun », a cote de la configuration. C'est LA
 * definition des abris : le panneau y prend ses libelles, batterie.c y prend
 * son comportement, et les deux ne peuvent pas se contredire.
 * ========================================================================= */
#include "batterie.h"

/* L'ORDRE COMPTE : le premier de la table est le repli d'un identifiant
 * inconnu, et c'est aussi celui qu'on veut par defaut. L'hibernation est le
 * seul etat qui survive a une batterie vide -- une suspension garde la
 * session en memoire, et la memoire a besoin de courant.
 *
 * batterie.c demande a logind s'il sait faire avant d'agir, et se rabat sur
 * l'extinction sinon : un abri annonce mais impossible serait le pire des
 * deux mondes. */
static const ShellAbriBatterie ABRIS[] = {
    { "hiberner",  "Mettre à l'abri sur disque",
      "La session est écrite sur le disque, puis la machine s'éteint. C'est "
      "le seul état qui survive à une batterie vide : au retour, tout est "
      "là où vous l'aviez laissé." },
    { "suspendre", "Suspendre",
      "La session reste en mémoire. Rapide à reprendre, mais la mémoire "
      "consomme : une batterie à bout finira par tout emporter." },
    { "eteindre",  "Éteindre",
      "Fermeture propre de la session. Rien n'est perdu de ce qui a été "
      "enregistré ; ce qui ne l'a pas été l'est." },
    { "rien",      "Ne rien faire",
      "Prévenir seulement. La machine s'arrêtera d'elle-même quand la "
      "batterie sera vide, sans rien mettre à l'abri." },
    { NULL, NULL, NULL }
};

const ShellAbriBatterie *
shell_batterie_abris (void)
{
    return ABRIS;
}

const ShellAbriBatterie *
shell_batterie_abri_actif (const ShellConfig *cfg)
{
    for (const ShellAbriBatterie *a = ABRIS; a->id != NULL; a++)
        if (g_strcmp0 (a->id, cfg->energie_bat_abri_action) == 0)
            return a;
    return &ABRIS[0];
}
