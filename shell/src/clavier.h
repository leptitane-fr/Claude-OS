/* =========================================================================
 * Claude OS — claviers à l'écran
 *
 * Deux claviers, pour le seul écran qui en a besoin : celui de la connexion.
 *
 * POURQUOI ILS EXISTENT
 *
 * La machine est un HP Chromebook x360 : un convertible, avec un écran
 * tactile Goodix et un « Tablet Mode Switch ». Capot retourné, l'EC coupe le
 * clavier physique. Jusqu'ici, dans cette position, il était tout simplement
 * impossible d'ouvrir sa session — l'écran de connexion n'avait rien à
 * toucher.
 *
 * POURQUOI PAS UN CLAVIER VIRTUEL WAYLAND
 *
 * squeekboard, wvkbd et leurs semblables passent par
 * zwp_virtual_keyboard_v1, que labwc n'expose pas, et ils demanderaient un
 * second processus sur l'écran de connexion — donc une seconde surface à
 * gérer avant authentification. Ces claviers-ci ne sont que des boutons GTK
 * dans la même fenêtre : rien à installer, rien à lancer, rien à surveiller.
 * ========================================================================= */

#ifndef CLAUDE_OS_CLAVIER_H
#define CLAUDE_OS_CLAVIER_H

#include <gtk/gtk.h>

/* Une touche du pavé numérique. « c » vaut '0' à '9', ou '\b' pour effacer. */
typedef void (*ShellPaveFn) (char c, gpointer donnees);

/* La touche Entrée du clavier complet. */
typedef void (*ShellEntreeFn) (gpointer donnees);

/* Pavé numérique, trois colonnes.
 *
 * Il ne s'attache à aucun champ : le code PIN ne vit pas dans un GtkEntry
 * mais dans six pastilles, et c'est l'appelant qui tient la chaîne. */
GtkWidget *shell_clavier_pave (ShellPaveFn sur_touche, gpointer donnees);

/* Clavier azerty complet, attaché à un champ.
 *
 * Azerty parce que /etc/xdg/labwc-greeter/environment pose déjà
 * XKB_DEFAULT_LAYOUT=fr : les deux saisies, à l'écran et au clavier
 * physique, doivent montrer les mêmes lettres aux mêmes endroits. */
GtkWidget *shell_clavier_azerty (GtkEditable   *cible,
                                 ShellEntreeFn  sur_entree,
                                 gpointer       donnees);

#endif /* CLAUDE_OS_CLAVIER_H */
