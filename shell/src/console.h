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

/* --- Alimentation --------------------------------------------------------
 *
 * Eteindre, redemarrer, mettre en veille — par logind sur le bus systeme.
 * Pas d'appel a « systemctl » : polkit autorise deja ces trois actions pour
 * l'utilisateur de la session active, et lancer un processus pour cela
 * ajouterait une dependance a un binaire et une fenetre de course.
 *
 * `popover` est referme avant d'agir : la Console est une surface
 * layer-shell posee par-dessus tout, et la laisser ouverte pendant l'arret
 * donne un ecran fige sur le panneau. */
GtkWidget *console_alimentation_new (GtkWidget *popover, gboolean apercu);
