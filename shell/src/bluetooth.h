/* =========================================================================
 * Claude-OS Shell — page Bluetooth des reglages rapides
 *
 * Liste les appareils connus et ceux que la decouverte fait apparaitre,
 * et permet de s'y connecter.
 *
 * CE QU'ELLE NE FAIT PAS : l'appairage d'un appareil qui demande un code.
 * Cela suppose un agent BlueZ, c'est-a-dire un service qui reste enregistre
 * sur le bus pour repondre aux demandes de confirmation. Les appareils dits
 * « Just Works » -- casques, souris, la plupart des telephones en partage de
 * connexion -- s'appairent sans. Les autres sont signales comme tels plutot
 * que d'echouer sans explication.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

GtkWidget *bluetooth_page_new (GtkStack *pile, const char *retour, gboolean apercu);

/* A appeler quand la page devient visible : lance la decouverte et
 * reconstruit la liste. La decouverte s'arrete a la fermeture -- c'est le
 * poste de consommation le plus lourd de cette page. */
void bluetooth_page_ouverte (GtkWidget *page);
void bluetooth_page_fermee  (GtkWidget *page);
