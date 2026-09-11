/* =========================================================================
 * Claude OS — le mode tablette : l'écran est-il retourné ?
 *
 * La machine est un convertible : passé 180°, l'écran se replie derrière le
 * clavier. Le clavier à l'écran, la rotation de l'image et la taille des
 * cibles tactiles en dépendent ; ce module ne fait que le SAVOIR, et le dire.
 *
 * D'OÙ VIENT L'INFORMATION
 *
 * Du commutateur « Tablet Mode Switch » (pilote chromeos_tbmc), un
 * périphérique d'entrée qui n'émet qu'un événement SW_TABLET_MODE à chaque
 * passage. Mesuré le 11 septembre 2026 : l'EC bascule vers 180° dans les
 * deux sens, et les trois périphériques qui portent ce commutateur
 * concordent à 120 ms près. Voir 70-claude-os-tablette.rules pour le choix
 * du périphérique et pour la permission.
 *
 * Et PAS de l'angle du capot : il faudrait le scruter, et l'EC fait déjà ce
 * calcul, avec son hystérésis, pour décider du commutateur.
 *
 * AUCUNE SCRUTATION. Le descripteur est surveillé par la boucle GLib ; entre
 * deux retournements, le module ne coûte rien.
 *
 * IL VIT DANS claude-os-dock, pour la raison qui a mis les notifications
 * dans la barre : un processus à part coûterait un runtime GTK4 entier
 * (~40 Mo mesurés) sur 4 Go soudés. Le dock est résident, déjà client
 * Wayland, et il est celui qui mène la visibilité — le clavier à l'écran
 * doit s'entendre avec lui pour le bas de l'écran.
 * ========================================================================= */
#pragma once

#include <glib.h>

/* Appelée à chaque changement de mode effectif, jamais pour rien. */
typedef void (*ShellTabletteFunc) (gboolean tablette, gpointer user_data);

/* Ouvre le commutateur, lit son état, et surveille. N'appelle PAS le rappel
 * pour l'état initial : l'appelant le lit par shell_tablette_active().
 *
 * Si le commutateur est introuvable ou refusé, le module le dit dans le
 * journal et reste en mode portable : la machine se comporte comme avant,
 * et le forçage reste possible. */
void shell_tablette_init (ShellTabletteFunc cb, gpointer user_data);

/* Le mode effectif : le forçage s'il y en a un, sinon le commutateur. */
gboolean shell_tablette_active (void);

/* Ce que dit le commutateur, forçage ou non. FALSE s'il n'a pas pu être lu. */
gboolean shell_tablette_commutateur (void);

/* Forçage, pour le banc d'essai et pour qui veut le clavier à l'écran capot
 * ouvert : SHELL_TABLETTE_SUIVRE rend la main au commutateur. */
typedef enum {
    SHELL_TABLETTE_SUIVRE = -1,
    SHELL_TABLETTE_PORTABLE = 0,
    SHELL_TABLETTE_TABLETTE = 1,
} ShellTabletteForcage;

void shell_tablette_forcer (ShellTabletteForcage forcage);
