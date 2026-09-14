/* =========================================================================
 * Claude-OS Shell — la definition de ce que fait le capot
 *
 * Separe de capot.c comme abris-batterie.c l'est de batterie.c, et
 * modes-energie.c de energie.c : le panneau de reglages ne veut que des
 * libelles, pas l'evdev ni l'inhibiteur logind.
 * ========================================================================= */
#include "capot.h"

/* L'ORDRE COMPTE : « suspendre » est en tete, donc le repli d'un
 * identifiant inconnu. C'est aussi ce que logind faisait avant que le shell
 * ne prenne la main -- un fichier illisible ne change donc rien au
 * comportement de la machine.
 *
 * « methode » est le nom de la methode logind a appeler. NULL veut dire que
 * le shell agit lui-meme : c'est le cas du verrouillage, qui ne concerne que
 * l'ecran, et de « rien ». */
static const ShellCapotAction ACTIONS[] = {
    { "suspendre", "Suspendre", "Suspend",
      "La session reste en mémoire, le réveil est immédiat. Mais la mémoire "
      "consomme : capot fermé plusieurs heures sans prise, la batterie "
      "s'épuise et la session est perdue." },

    { "suspendre-hiberner", "Suspendre, puis hiberner", "SuspendThenHibernate",
      "Le meilleur des deux : réveil immédiat si vous revenez vite, et la "
      "session est écrite sur le disque avant que la batterie ne s'épuise. "
      "Le délai est celui de systemd (/etc/systemd/sleep.conf)." },

    { "hiberner", "Hiberner", "Hibernate",
      "La session part sur le disque et la machine s'éteint vraiment. "
      "Consommation nulle, rien à perdre — mais chaque ouverture coûte le "
      "temps de relire la mémoire depuis l'eMMC." },

    { "verrouiller", "Verrouiller l'écran", NULL,
      "La machine continue de tourner, capot fermé : les téléchargements "
      "et les compilations vont à leur terme. Seul l'écran est verrouillé." },

    { "eteindre", "Éteindre", "PowerOff",
      "Fermeture propre de la session à chaque fermeture du capot." },

    { "rien", "Ne rien faire", NULL,
      "Le capot ne déclenche rien du tout. L'écran s'éteint parce qu'il est "
      "replié, et la veille progressive continue de compter comme si de "
      "rien n'était." },

    { NULL, NULL, NULL, NULL }
};

const ShellCapotAction *
shell_capot_actions (void)
{
    return ACTIONS;
}

const ShellCapotAction *
shell_capot_action_active (const ShellConfig *cfg)
{
    for (const ShellCapotAction *a = ACTIONS; a->id != NULL; a++)
        if (g_strcmp0 (a->id, cfg->energie_capot_action) == 0)
            return a;
    return &ACTIONS[0];
}
