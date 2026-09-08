/* =========================================================================
 * Claude-OS Shell — panneau de reglages rapides
 *
 * S'ouvre au clic sur la barre d'etat, et seulement la. Rien de ce qu'il
 * contient n'est visible au repos : la barre ne montre que l'heure et les
 * quelques icones qui se lisent d'un coup d'oeil.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

/* CE QUE LA CONSOLE LAISSE SOUS ELLE.
 *
 * Le centre de notifications se pose au-dessus de la Console et doit
 * respecter le meme ecart avec la barre d'etat quand la Console est fermee.
 * Une seule definition, pour que les deux ne derivent pas l'une de l'autre. */
#define PANEL_ECART_BARRE_PX 12

/* Geometrie VISIBLE de la Console, ombre portee exclue. Annoncee a chaque
 * changement : ouverture, fermeture, et depliage de la colonne de detail.
 * `hauteur` vaut 0 quand la Console est fermee ; `largeur` reste celle
 * qu'elle aurait ouverte, car le centre de notifications doit s'y aligner
 * meme lorsqu'elle ne s'affiche pas.
 *
 * LA LARGEUR EST ANNONCEE, ET NON DEVINEE. Celle de la Console ne vaut pas
 * la valeur ecrite dans « .qs { min-width } » : son contenu la pousse
 * au-dela — mesure du 8 septembre 2026, 334 px pour un minimum de 296. Un
 * centre de notifications cale sur le minimum aurait donc ete visiblement
 * plus etroit, et l'exigence est qu'il ait exactement la meme largeur. */
typedef void (*PanelGeometrieFn) (int largeur, int hauteur, gpointer data);
void panel_observer_geometrie (GtkWidget *popover, PanelGeometrieFn fn, gpointer data);

/* Cree le panneau (un GtkPopover pret a etre attache a un GtkMenuButton).
 *
 * `apercu` remplace les sources systeme par des valeurs fixes. C'est une
 * aide au banc d'essai visuel, rien d'autre : elle permet de juger la mise
 * en page sans NetworkManager ni BlueZ dans le conteneur. Elle ne prouve
 * evidemment rien du branchement D-Bus reel. */
GtkWidget *panel_new (gboolean apercu);
