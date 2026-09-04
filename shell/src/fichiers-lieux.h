/* =========================================================================
 * Claude-OS Shell — gestionnaire de fichiers : le volet des emplacements
 *
 * Trois sections : les dossiers personnels, les favoris ajoutes a la main,
 * les peripheriques montes. Une quatrieme viendra pour le nuage -- Google
 * Drive, OneDrive -- quand ils seront configures ; le decoupage en sections
 * existe deja pour cela, il n'y aura qu'a en declarer une de plus.
 *
 * LA CORBEILLE POINTE SUR UN VRAI DOSSIER
 *
 * L'adresse « trash:/// » demanderait gvfs, que ce systeme n'installe pas.
 * On ouvre donc directement ~/.local/share/Trash/files, ou la specification
 * freedesktop range ce qui est jete -- et que GLib alimente sans gvfs.
 * Les noms d'origine y sont conserves. En revanche, restaurer un fichier a
 * sa place d'origine n'est pas possible de la : cette information vit dans
 * un fichier .trashinfo separe, que rien ne lit ici.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

/* Appelee quand un emplacement est choisi. */
typedef void (*LieuxNavFunc) (GFile *dossier, gpointer data);

GtkWidget *fichiers_lieux_new (LieuxNavFunc nav, gpointer data);

/* Met en evidence l'emplacement correspondant, ou n'en surligne aucun si le
 * dossier courant n'en est pas un. */
void fichiers_lieux_suivre (GtkWidget *lieux, GFile *dossier);

/* Favoris de l'utilisateur, conserves dans ~/.config/claude-os/favoris. */
void     fichiers_lieux_ajouter (GtkWidget *lieux, GFile *dossier);
void     fichiers_lieux_retirer (GtkWidget *lieux, GFile *dossier);
gboolean fichiers_lieux_est_favori (GtkWidget *lieux, GFile *dossier);
