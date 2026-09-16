/* =========================================================================
 * Claude-OS Shell — ce que fait la machine quand on rabat le capot
 *
 * POURQUOI LE SHELL PREND LA MAIN
 *
 * Jusqu'ici, le capot appartenait a logind : « HandleLidSwitch=suspend »,
 * un reglage systeme dans /etc, le meme pour les trois modes d'energie et
 * hors de portee du panneau. (Le shell a d'abord repris la main SANS lever
 * le second reproche : un reglage unique, toujours le meme pour les trois
 * modes. C'est repare depuis le 16 septembre 2026 -- voir plus bas.) Le projet demande l'inverse : les reglages se
 * choisissent dans le panneau Energie, et le panneau ecrit shell.conf. Un
 * panneau qui irait ecrire dans /etc a chaque changement demanderait une
 * elevation de privileges pour choisir ce que fait un capot.
 *
 * Le shell pose donc un inhibiteur « handle-lid-switch » de type BLOCK --
 * logind ne touche plus au capot -- et decide lui-meme. C'est ce que font
 * GNOME et KDE, pour la meme raison.
 *
 * CONTREPARTIE ASSUMEE, et c'est la meme que pour les notifications et la
 * veille : si la barre d'etat tombe, l'inhibiteur tombe avec elle et logind
 * reprend la main -- donc l'ancien comportement, « suspendre ». Le capot ne
 * devient jamais inerte : il redevient ce qu'il etait.
 *
 * -------------------------------------------------------------------------
 * D'OU VIENT L'INFORMATION
 *
 * Du commutateur « Lid Switch » (ACPI PNP0C0D), un peripherique d'entree
 * dedie qui ne porte QUE SW_LID -- releve le 14 septembre 2026 sur MADOO,
 * /dev/input/event7. Le meme choix que pour le mode tablette : un
 * peripherique qui ne porte que ce qu'on veut lire. « cros_ec_buttons »
 * signale le capot lui aussi, mais porte en plus les boutons
 * d'alimentation et de volume ; l'ouvrir exposerait plus que necessaire.
 *
 * AUCUNE SCRUTATION : le descripteur est surveille par la boucle GLib.
 * Entre deux mouvements du capot, ce module ne coute rien. Contrairement a
 * la batterie, le noyau previent ici -- et c'est pourquoi on l'ecoute.
 *
 * IL FAUT LA REGLE UDEV. Sans 70-claude-os-tablette.rules etendue au
 * « Lid Switch », l'ouverture rend EACCES : le module le dit dans le
 * journal, n'inhibe rien, et logind garde la main. La machine se comporte
 * alors exactement comme avant.
 * ========================================================================= */
#pragma once

#include <glib.h>
#include "config.h"
#include "energie.h"   /* ShellModeEnergie : le capot depend du mode */

/* Ce que le capot ferme declenche. L'identifiant est ce qui s'ecrit dans
 * shell.conf ; la table vit dans actions-capot.c et fait foi -- le panneau y
 * prend ses libelles au lieu de les redire. */
typedef struct {
    const char *id;       /* « suspendre », « suspendre-hiberner », …       */
    const char *nom;      /* « Suspendre, puis hiberner »                   */
    const char *methode;  /* la methode logind, ou NULL si le shell agit    */
    const char *resume;   /* une phrase, montree sous le choix              */
} ShellCapotAction;

/* Table terminee par un id NULL. */
const ShellCapotAction *shell_capot_actions (void);

/* L'action d'un mode DONNE, jamais NULL. Le panneau de reglages en a besoin
 * pour decrire les trois cartes a la fois, comme il le fait deja des durees
 * : une carte « Travail » qui annoncerait le capot du mode en vigueur
 * mentirait sur deux cartes sur trois.
 *
 * Un mode NULL vaut celui en vigueur. Un identifiant inconnu -- fichier
 * d'une version anterieure -- renvoie « suspendre », ce que logind faisait
 * avant que le shell ne prenne la main : le defaut ne surprend personne. */
const ShellCapotAction *shell_capot_action_mode (const ShellConfig *cfg,
                                                 const ShellModeEnergie *mode);

/* L'action du mode EN VIGUEUR, jamais NULL. C'est elle que capot.c applique.
 *
 * DEPUIS LE 16 SEPTEMBRE 2026 ELLE DEPEND DU MODE, et c'est un changement de
 * comportement : le capot etait le dernier reglage d'energie que le mode ne
 * gouvernait pas. En « Travail » -- veille_ordi = FALSE dans la table des
 * modes, donc aucune inactivite ne peut endormir la machine -- rabattre
 * l'ecran la suspendait quand meme. Changer de mode change donc maintenant
 * ce que fait le capot, sans qu'on ait a y revenir. */
const ShellCapotAction *shell_capot_action_active (const ShellConfig *cfg);

/* Ouvre le commutateur, pose l'inhibiteur, et surveille.
 *
 * Si le commutateur est introuvable ou refuse, AUCUN inhibiteur n'est pose :
 * prendre la main sans pouvoir lire le capot rendrait celui-ci inerte, ce
 * qui est bien pire que de laisser logind faire. */
void shell_capot_init (const ShellConfig *cfg);

/* Relit l'action. A appeler depuis le rappel de shell_config_watch, comme
 * shell_energie_reconfigurer et shell_batterie_reconfigurer. */
void shell_capot_reconfigurer (const ShellConfig *cfg);
