/* =========================================================================
 * Claude-OS Shell — gestionnaire de fichiers : les operations
 *
 * Copier, deplacer, mettre a la corbeille, supprimer. Le tout dans un fil
 * separe, avec une fenetre de progression qui n'apparait que si l'operation
 * dure.
 *
 * POURQUOI UN FIL, ET PAS LES VARIANTES ASYNCHRONES DE GIO
 *
 * GIO ne sait pas copier un dossier : g_file_copy echoue avec
 * G_IO_ERROR_WOULD_RECURSE des qu'on lui en donne un. La descente est donc
 * a notre charge, et une descente ecrite en rappels asynchrones serait bien
 * plus difficile a suivre -- et a interrompre -- que la meme boucle dans un
 * fil.
 *
 * RIEN N'EST JAMAIS ECRASE
 *
 * Quand la destination existe deja, le nom est decale : « notes (copie).txt »,
 * puis « notes (copie 2).txt ». Windows demande quoi faire ; poser la
 * question depuis un fil de travail demanderait de le suspendre a chaque
 * collision. Decaler le nom ne perd jamais rien, et se defait a la main.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

typedef enum {
    OP_COPIER,
    OP_DEPLACER,
    OP_CORBEILLE,
    OP_SUPPRIMER
} OpGenre;

/* Appelee sur le fil principal, l'operation terminee. `erreur` est NULL en
 * cas de succes, et porte G_IO_ERROR_CANCELLED si l'utilisateur a annule. */
typedef void (*OpFiniFunc) (GError *erreur, gpointer data);

/* `sources` : liste de GFile*, empruntee le temps de l'appel.
 * `destination` : le dossier d'arrivee, ignore pour la corbeille et la
 * suppression. */
void fichiers_op (OpGenre genre, GList *sources, GFile *destination,
                  GtkWindow *parent, OpFiniFunc fini, gpointer data);

/* Le nom libre le plus proche de `nom` dans `dossier`. A liberer.
 * Expose parce que la creation d'un dossier en a besoin elle aussi. */
char *fichiers_nom_libre (GFile *dossier, const char *nom);
