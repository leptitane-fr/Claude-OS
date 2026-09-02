/* =========================================================================
 * Claude-OS Shell — visibilite du dock et de la barre
 *
 * Ce module ne fait qu'une chose : la bascule manuelle au clavier.
 *
 * LE PLEIN ECRAN NE MASQUE RIEN, ET C'EST VOULU
 *
 * Le dock et la barre restent affiches PAR-DESSUS une fenetre plein ecran,
 * qui de son cote occupe bien tout l'ecran : une surface layer-shell ne
 * reserve rien face a une fenetre plein ecran, seules les fenetres
 * maximisees s'arretent au-dessus du dock.
 *
 * C'est le choix de l'utilisateur, pris apres essai sur la machine, et il
 * est plus pratique que le masquage automatique : on garde l'heure, la
 * batterie et le dock sous les yeux pendant une video ou une visio, et la
 * touche Windows suffit a degager l'ecran quand on le veut vraiment.
 *
 * Note pour plus tard : j'avais annonce l'inverse, en lisant dans le code de
 * labwc 0.8.3 un desactivation de la couche TOP sous une fenetre plein ecran
 * (desktop_update_top_layer_visibility). Sur la machine, cela ne se produit
 * pas. Lire le code d'un compositeur dit ce qu'il contient, pas ce qu'il
 * fait dans une situation donnee.
 * ========================================================================= */
#pragma once

#include <glib.h>

typedef void (*ShellVisibilityFunc) (gboolean visible, gpointer user_data);

/* Enregistre le composant. A appeler une fois la fenetre presentee. */
void shell_visibility_init (ShellVisibilityFunc cb, gpointer user_data);

/* Inverse ce qui est actuellement a l'ecran. */
void shell_visibility_toggle (void);
