/* =========================================================================
 * Claude-OS Shell — panneau de reglages rapides
 *
 * S'ouvre au clic sur la barre d'etat, et seulement la. Rien de ce qu'il
 * contient n'est visible au repos : la barre ne montre que l'heure et les
 * quelques icones qui se lisent d'un coup d'oeil.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

/* Cree le panneau (un GtkPopover pret a etre attache a un GtkMenuButton).
 *
 * `apercu` remplace les sources systeme par des valeurs fixes. C'est une
 * aide au banc d'essai visuel, rien d'autre : elle permet de juger la mise
 * en page sans NetworkManager ni BlueZ dans le conteneur. Elle ne prouve
 * evidemment rien du branchement D-Bus reel. */
GtkWidget *panel_new (gboolean apercu);
