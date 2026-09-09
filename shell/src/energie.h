/* =========================================================================
 * Claude-OS Shell — mise en veille progressive
 *
 * Le retroeclairage est le premier poste de consommation de la machine :
 * environ 1 a 2 W sur les 6,8 W mesures a la batterie le 8 septembre 2026.
 * Le baisser quand personne ne regarde est le plus gros levier d'autonomie
 * qui reste, et il est entierement logiciel.
 *
 * DEUX PROFILS, choisis sur la source d'alimentation :
 *
 *   secteur  « Normal »   attenuer, puis eteindre l'ecran
 *   batterie « Econome »  attenuer, eteindre, puis suspendre
 *
 * L'ETAGE « SUSPENDRE » EST DESACTIVE PAR DEFAUT, ET CE N'EST PAS UN OUBLI.
 * Le 9 septembre 2026, onze suspensions consecutives declenchees par la
 * fermeture du capot n'ont JAMAIS repris : le journal s'arrete net sur
 * « PM: suspend entry (s2idle) » et la machine reapparait avec un nouvel
 * identifiant de demarrage. Endormir automatiquement une machine qui ne se
 * reveille pas, c'est lui faire perdre la session de l'utilisateur. L'etage
 * existe, il se regle, mais il reste ferme tant que la reprise n'est pas
 * fiable.
 *
 * AUCUNE SCRUTATION -- c'est la regle du projet, et elle vaut doublement
 * pour un module dont l'objet est d'economiser. Le compositeur previent par
 * ext-idle-notify-v1 ; entre deux evenements, ce module ne coute rien. Les
 * trois signaux qui affinent la decision (charge, son, source) sont lus une
 * seule fois, au moment ou un etage va se declencher.
 *
 * LES INHIBITEURS SONT GRATUITS. La specification du protocole impose au
 * compositeur de NE PAS rendre la notification inactive tant qu'un
 * zwp_idle_inhibitor_v1 existe sur une surface visible. Un lecteur video ne
 * sera donc jamais interrompu, sans une ligne de code ici. Verifie sur la
 * machine le 9 septembre 2026 : aucun evenement tant que Claude Desktop
 * tenait son verrou d'eveil, les trois etages a l'heure des sa liberation.
 * ========================================================================= */
#pragma once

#include <glib.h>
#include "config.h"

/* A appeler une fois la fenetre presentee : la connexion Wayland de GTK doit
 * deja exister. Sans compositeur compatible, ou sans retroeclairage
 * pilotable, le module s'efface et la barre continue normalement. */
void shell_energie_init (const ShellConfig *cfg);

/* Relit les delais et reconstruit les minuteries. A appeler depuis le rappel
 * de shell_config_watch : le panneau de reglages ecrit shell.conf, chaque
 * composant relit, et la veille suit sans qu'on ait rien a redemarrer. */
void shell_energie_reconfigurer (const ShellConfig *cfg);
