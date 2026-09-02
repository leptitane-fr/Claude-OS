/* =========================================================================
 * Claude-OS Shell — visibilite du dock et de la barre
 *
 * Ce module ne fait qu'une chose : la bascule manuelle au clavier.
 *
 * LE PLEIN ECRAN N'EST PAS TRAITE ICI, ET C'EST DELIBERE
 *
 * labwc 0.8.3 -- la version de Debian trixie -- desactive lui-meme toute la
 * couche TOP du layer-shell des qu'une fenetre plein ecran n'a aucune
 * fenetre au-dessus d'elle (src/desktop.c, desktop_update_top_layer_
 * visibility). Le dock et la barre disparaissent donc sans une ligne de
 * notre part, et selon une regle meilleure que celle qu'on aurait ecrite :
 * elle raisonne sur l'empilement reel, pas seulement sur le focus.
 *
 * Une premiere version de ce fichier suivait les fenetres par
 * wlr-foreign-toplevel-management-v1 pour deduire le plein ecran. Elle
 * fonctionnait, mais dupliquait une politique qui appartient au
 * compositeur, avec un protocole de plus a embarquer. Elle a ete retiree.
 *
 * A verifier au premier demarrage sur la machine : passer Chromium en plein
 * ecran doit faire disparaitre dock et barre. Si ce n'est pas le cas, c'est
 * ici que le suivi des fenetres devra revenir.
 * ========================================================================= */
#pragma once

#include <glib.h>

typedef void (*ShellVisibilityFunc) (gboolean visible, gpointer user_data);

/* Enregistre le composant. A appeler une fois la fenetre presentee. */
void shell_visibility_init (ShellVisibilityFunc cb, gpointer user_data);

/* Inverse ce qui est actuellement a l'ecran. */
void shell_visibility_toggle (void);
