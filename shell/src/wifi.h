/* =========================================================================
 * Claude-OS Shell — page Wi-Fi des reglages rapides
 *
 * Liste les reseaux visibles et permet de s'y connecter. Ecrit contre les
 * fichiers d'introspection de NetworkManager 1.52 (recuperes chez
 * freedesktop) plutot que de memoire : noms de methodes, signatures et
 * proprietes en sont copies.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

/* Cree la page. `retour` est le nom de la page vers laquelle revient la
 * fleche de l'en-tete. */
GtkWidget *wifi_page_new (GtkStack *pile, const char *retour, gboolean apercu);

/* A appeler quand la page devient visible : declenche un balayage et
 * reconstruit la liste. Rien ne tourne quand la page est fermee. */
void wifi_page_ouverte (GtkWidget *page);
