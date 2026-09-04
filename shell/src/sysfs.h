/* =========================================================================
 * Claude-OS Shell — lecture de sysfs
 *
 * La barre d'etat et le panneau lisent tous deux la batterie. Plutot que
 * dupliquer la recherche du repertoire et la lecture des fichiers, les deux
 * tiennent ici.
 *
 * Pourquoi sysfs et pas UPower : un demon de plus en memoire et sur le bus
 * pour des valeurs qui tiennent dans quatre fichiers texte ne se justifie
 * pas sur une machine dont l'autonomie est la raison d'etre.
 * ========================================================================= */
#pragma once

#include <glib.h>

/* Contenu d'un fichier sysfs, espaces retires. NULL si absent ou illisible.
 * A liberer avec g_free(). */
char *shell_sysfs_read (const char *dir, const char *file);

/* Repertoire de la premiere batterie trouvee, ou NULL s'il n'y en a pas.
 * Le nom varie : BAT0 sur beaucoup de portables, BAT1 sur ce Vivobook,
 * BATC ailleurs. A liberer avec g_free(). */
char *shell_battery_dir (void);
