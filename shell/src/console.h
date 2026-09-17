/* =========================================================================
 * Claude OS — les rangees de la Console
 *
 * La Console, c'est la barre d'etat ouverte. Elle portait deux bascules et
 * une carte batterie ; elle porte desormais aussi le son, la luminosite et
 * l'alimentation — c'est-a-dire tout ce qu'on regle sans ouvrir une
 * application.
 *
 * CE FICHIER NE DESSINE QUE DES RANGEES. L'assemblage et l'ordre restent
 * dans panel.c, qui reste le seul endroit ou l'on juge la mise en page.
 *
 * DISCIPLINE D'ENERGIE, LA MEME QUE LE RESTE DU PANNEAU
 *
 * Rien ici ne consulte quoi que ce soit tant que la Console est fermee.
 * Ouverte, elle se relit en boucle — les touches du clavier changent le
 * volume et la luminosite sans passer par nous, et un curseur qui reste
 * fige pendant qu'on appuie donne une Console qui ment. La minuterie vit
 * dans panel.c, entre « show » et « closed », et nulle part ailleurs.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

/* --- Son -----------------------------------------------------------------
 *
 * Passe par « wpctl », fourni par wireplumber, plutot que par libpulse : le
 * shell n'aurait sinon aucune raison de lier une bibliotheque audio entiere
 * pour lire et ecrire un nombre.
 *
 * SUR CETTE MACHINE, LA CARTE SON NE S'INITIALISE PAS (voir docs/01 §1.8).
 * La rangee se desactive alors d'elle-meme et le dit, plutot que d'afficher
 * un curseur qui ne commande rien. */
GtkWidget *console_son_new (gboolean apercu);

/* Relit la valeur reelle. Appele a l'ouverture de la Console, puis
 * periodiquement tant qu'elle reste ouverte. Sans effet pendant les
 * quelques centaines de millisecondes qui suivent une action de
 * l'utilisateur : sinon la relecture reposerait l'ancienne valeur sous le
 * doigt qui vient de deplacer le curseur. */
void console_son_relire (GtkWidget *rangee);

/* --- Luminosite ----------------------------------------------------------
 *
 * Ecrit directement dans /sys/class/backlight. L'ecriture exige que le
 * compte appartienne au groupe « video » ET qu'une regle udev ouvre le
 * fichier a ce groupe — les deux sont posees par provision.sh. Sans elles la
 * lecture fonctionne et l'ecriture echoue en silence : la rangee le detecte
 * et se desactive.
 *
 * Le mode automatique n'apparait que si un capteur de luminosite ambiante
 * est reellement expose sur le bus. Sur MADOO, sa presence n'a pas encore
 * ete constatee. */
GtkWidget *console_lumiere_new (gboolean apercu);
void       console_lumiere_relire (GtkWidget *rangee);

/* --- Systeme : memoire, disque, processeur --------------------------------
 *
 * La carte d'etat de la machine, posee sous la carte batterie : deux
 * cartes qui disent ce que la machine a, l'une l'energie, l'autre les
 * ressources.
 *
 * Trois lignes, meme forme : le nom a gauche, ce qui RESTE a droite, et la
 * jauge de ce qui est PRIS dessous. La memoire affichee est
 * « MemAvailable » et non « MemFree » -- le cache est rendu des qu'on le
 * reclame, et compter la memoire libre ferait passer pour exsangue une
 * machine qui respire.
 *
 * Aucune dependance nouvelle : /proc/meminfo, /proc/stat, statvfs(), et
 * une zone thermique cherchee par son type. Le detail, les seuils et les
 * pieges sont en tete de la section correspondante de console.c.
 *
 * Comme le son et la luminosite : rien n'est lu tant que la Console est
 * fermee, et la relecture suit la minuterie de panel.c. */
GtkWidget *console_systeme_new (gboolean apercu);

/* Relit les trois mesures. La charge du processeur etant une difference
 * entre deux lectures de /proc/stat, elle reste en tiret jusqu'a la
 * deuxieme -- et repart de zero si la Console est restee fermee plus de
 * quelques secondes, l'ancienne reference ne valant plus rien. */
void console_systeme_relire (GtkWidget *rangee);

/* --- Veille de l'ecran ---------------------------------------------------
 *
 * Choisit le profil de mise en veille progressive. « Auto » suit la prise :
 * Normal sur secteur, Econome sur batterie. Les deux autres forcent, ce qui
 * sert dans les deux sens -- garder l'ecran allume pendant une presentation
 * alors qu'on est sur batterie, ou economiser alors qu'on est branche.
 *
 * Trois boutons, icone au-dessus du nom -- l'icone est aussi celle que la
 * barre d'etat montre pour le mode en vigueur. Plus de titre ni de roue
 * crantee : les durees se reglent par le bouton « Réglages » de la Console.
 *
 * La rangee N'APPELLE PAS le module d'energie. Elle ecrit shell.conf, et
 * c'est shell_config_watch qui previent tout le monde -- y compris ce
 * processus. C'est le mecanisme deja utilise par le panneau de reglages :
 * la configuration reste la seule source de verite, et il n'y a aucun
 * protocole a inventer entre une rangee et un module du meme binaire. */
GtkWidget *console_energie_new (gboolean apercu);

/* Remet les boutons en accord avec shell.conf. Comme pour le son et la
 * luminosite : le fichier a pu changer depuis le panneau de reglages
 * pendant que la Console etait fermee. */
void console_energie_relire (GtkWidget *rangee);

/* Ce que la rangee d'alimentation appelle avant d'agir.
 *
 * ELLE NE CONNAIT PLUS LE POPOVER, PARCE QU'IL N'Y EN A PLUS. La Console
 * vivait dans un GtkPopover, qu'il suffisait de « popdown » avant
 * d'eteindre ; elle vit desormais dans le tiroir du bord droit, qui se
 * ferme autrement -- et qui s'anime. Un rappel plutot qu'un widget : la
 * rangee dit « ferme-toi » et ne sait pas a quoi elle parle. */
typedef void (*ConsoleFermer) (gpointer data);

/* --- Alimentation --------------------------------------------------------
 *
 * Verrouiller, fermer la session, mettre en veille, redemarrer, eteindre.
 * Les trois derniers par logind sur le bus systeme -- pas d'appel a
 * « systemctl » : polkit les autorise deja pour l'utilisateur de la session
 * active, et lancer un processus pour cela ajouterait une dependance a un
 * binaire et une fenetre de course. Verrouiller passe par le module
 * d'energie (claude-os-verrou), fermer la session arrete labwc.
 *
 * Icones seules : le nom de chaque bouton est dans son infobulle.
 *
 * La Console est refermee avant d'agir : c'est une surface layer-shell
 * posee par-dessus tout, et la laisser ouverte pendant l'arret donne un
 * ecran fige sur le panneau -- elle passerait meme devant l'ecran de
 * verrouillage le temps qu'il monte. */
GtkWidget *console_alimentation_new (ConsoleFermer fermer, gpointer data,
                                     gboolean apercu);
