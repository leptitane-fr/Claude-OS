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

/* Sommes-nous sur le secteur ?
 *
 * Sur MADOO la question n'a pas une reponse unique : le noyau expose « AC »
 * (type Mains) ET deux « CROS_USBPD_CHARGER » (type USB), parce que la
 * machine se charge aussi bien par son connecteur que par l'un ou l'autre
 * port USB-C. Regarder le seul « AC » ferait passer pour « sur batterie »
 * une machine branchee en USB-C.
 *
 * La regle est donc : une alimentation quelconque, de type Mains ou USB,
 * declaree « online », suffit. Les entrees de type Battery sont ignorees --
 * y compris « hid-...-battery », celle du pave tactile.
 *
 * Renvoie TRUE si rien ne permet de conclure : mieux vaut se croire sur le
 * secteur et ne rien eteindre que s'endormir a tort. */
gboolean shell_sur_secteur (void);

/* Repertoire de la premiere batterie trouvee, ou NULL s'il n'y en a pas.
 * Le nom varie : BAT0 sur beaucoup de portables, BAT1 sur ce Vivobook,
 * BATC ailleurs. A liberer avec g_free(). */
char *shell_battery_dir (void);
