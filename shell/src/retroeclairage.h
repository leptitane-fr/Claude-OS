/* =========================================================================
 * Claude-OS Shell — pilotage du retroeclairage
 *
 * Extrait de console.c, ou il etait soude au widget du curseur. Le module
 * d'energie doit ecrire la meme luminosite, par la meme voie, sans passer
 * par une interface graphique : deux implementations qui ecrivent tour a
 * tour dans le meme fichier finiraient par se contredire.
 *
 * DEUX VOIES, ET LOGIND D'ABORD -- la doctrine de console.c, reprise telle
 * quelle parce qu'elle a ete payee : la regle udev ouvre « brightness » au
 * groupe « video », mais une appartenance a un groupe ne prend effet qu'a la
 * session SUIVANTE. logind, lui, n'exige aucun groupe : il verifie que
 * l'appelant est la session active du siege et ecrit pour lui.
 *
 * console.c n'a PAS encore ete converti a ce module : le faire demandait de
 * toucher a du code qui fonctionne, dans la meme seance que l'ecriture du
 * module d'energie. A faire dans une passe dediee.
 * ========================================================================= */
#pragma once

#include <glib.h>

/* Cherche l'ecran retroeclaire et ouvre logind. Sans effet si deja fait.
 * Ne change jamais la luminosite. */
void shell_retro_init (void);

/* FALSE si la machine n'a pas de retroeclairage pilotable, ou si aucune des
 * deux voies n'est plausible. Le module d'energie s'efface alors : mieux
 * vaut ne rien faire que d'echouer a chaque etage. */
gboolean shell_retro_disponible (void);

/* Luminosite courante en pourcent, ou -1. Relue dans sysfs a chaque appel :
 * les touches du clavier agissent sans que personne en soit averti, et une
 * valeur gardee en memoire serait fausse la moitie du temps. */
int shell_retro_lire (void);

/* Ecrit une luminosite en pourcent, bornee a [0, 100].
 *
 * ZERO EST AUTORISE ICI, contrairement au curseur de la Console qui refuse
 * de descendre sous 1. Pour un curseur, un ecran noir n'est pas un reglage
 * mais une panne apparente ; pour l'etage « eteindre » du module d'energie,
 * c'est exactement ce qu'on demande. */
void shell_retro_ecrire (int pourcent);
