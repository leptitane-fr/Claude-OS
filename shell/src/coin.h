/* =========================================================================
 * Claude-OS Shell — le coin : ce que la machine dit d'elle en permanence
 *
 * IL REMPLACE LA BARRE D'ETAT, ET IL NE LUI RESSEMBLE PAS.
 *
 * La barre etait une pilule opaque, posee en bas a droite, qu'on cliquait
 * pour ouvrir la Console et qui sortait de l'ecran des qu'une application
 * passait au premier plan. Trois proprietes, et les trois sont abandonnees :
 *
 *   - PLUS DE SURFACE. Le coin ne dessine ni fond, ni bordure, ni ombre :
 *     rien que des traces clairs poses sur le fond d'ecran. Ce qui reste a
 *     l'ecran en permanence ne doit pas y occuper de place.
 *
 *   - PLUS DE CLIC, NI DE SURVOL. Region d'entree vide et mode clavier
 *     « aucun », comme les avis systeme : le coin se voit et ne s'attrape
 *     pas. Ce qui s'y REGLAIT est passe dans le tiroir du bord droit, ou
 *     l'on va le chercher d'un geste franc plutot que de risquer un clic
 *     malheureux sur l'heure.
 *
 *   - PLUS DE DISPARITION. Il ne suit plus le dock hors de l'ecran. Une
 *     information permanente qui s'absente quand une fenetre s'ouvre n'est
 *     pas permanente ; c'etait le prix a payer tant qu'elle occupait une
 *     surface opaque, et ce prix n'a plus lieu d'etre.
 *
 * CE QU'IL MONTRE, ET DANS QUEL ORDRE.
 *
 * A gauche l'heure et la date, les seules choses qu'on vient vraiment y
 * lire. Par-dessus leur droite, en grand, LE MODE D'ENERGIE : c'est le seul
 * reglage du bureau dont l'effet se produit quand on ne regarde pas, et le
 * seul qu'on doive donc pouvoir verifier sans rien ouvrir. A droite, en
 * colonne et en petit, ce qui se lit d'un coup d'oeil : non-lu, reseau,
 * Bluetooth, charge -- et la fiche secteur quand elle est branchee.
 *
 * LA CLOCHE N'EST QU'UN TEMOIN. Elle dit qu'il reste du non-lu ; elle
 * n'ouvre plus rien, puisque rien ici ne s'ouvre. Le centre de
 * notifications attend qu'un widget du tiroir lui donne une entree.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>
#include "config.h"

/* Construit la surface et la presente. Une seule par processus. */
void shell_coin_init (GtkApplication *app, const ShellConfig *cfg,
                      gboolean apercu);

/* Repose tout ce qui se regle dans l'aspect du coin : l'opacite de l'encre
 * -- la meme que celle des avis systeme, par le meme reglage, parce que ce
 * qui flotte sur le fond d'ecran doit le faire d'une seule voix -- et les
 * quatre nombres de l'ombre portee (voir config.h).
 *
 * UNE SEULE ENTREE, ET NON UNE PAR REGLAGE : les cinq valeurs vivent dans
 * la meme regle CSS engendree, et separer les entrees reviendrait a la
 * reecrire plusieurs fois avec des moities perimees. */
void shell_coin_apparence (const ShellConfig *cfg);

/* Chacune de ces fonctions ne touche qu'a ce qu'elle nomme. Elles sont
 * appelees par status.c, qui tient les sources -- le coin ne lit rien de
 * lui-meme, a une exception pres, le Bluetooth, dont personne d'autre ne
 * suit l'etat en permanence. */
void shell_coin_horloge    (const char *heure, const char *date);
void shell_coin_mode       (const ShellConfig *cfg);
void shell_coin_batterie   (int pourcent, gboolean sur_secteur,
                            const char *icone);
void shell_coin_reseau     (const char *icone);
void shell_coin_non_lu     (gboolean il_y_en_a);

/* Le coin s'efface-t-il ? Appelee quand une fenetre entre ou sort du plein
 * ecran -- voir shell_toplevels_plein_ecran().
 *
 * C'EST LA SEULE CHOSE QUI LE FAIT DISPARAITRE, et elle a ete concedee a
 * contrecoeur : « permanent » etait la propriete qu'on lui demandait. Mais
 * une heure posee sur un film n'est plus un service, et les commandes de
 * lecture de Netflix vivent exactement au meme endroit -- deux tracés clairs
 * l'un sur l'autre, illisibles tous les deux. Il n'y a pas d'arrangement :
 * l'un des deux doit partir, et ce n'est pas au film de s'effacer. */
void shell_coin_plein_ecran (gboolean actif);

/* Le widget auquel accrocher une surface qui doit se poser AU-DESSUS du
 * coin -- la banniere du centre de notifications, aujourd'hui.
 *
 * ELLE S'ACCROCHAIT A LA PILULE DE LA BARRE, dont elle heritait le bord
 * droit et la largeur sans rien mesurer. Le coin joue le meme role, et pour
 * la meme raison : un alignement calcule aurait derive au premier
 * changement de marge dans la feuille de style. Que le coin ne recoive
 * aucun clic ne gene pas -- un popover est une surface a lui, avec sa
 * propre region d'entree. */
GtkWidget *shell_coin_ancre (void);
