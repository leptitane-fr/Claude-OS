/* =========================================================================
 * Claude-OS Shell — les inhibiteurs de logind, en un seul endroit
 *
 * Deux modules en posent, pour deux raisons differentes :
 *
 *   capot.c    « handle-lid-switch » en BLOCK, pour que le panneau Energie
 *              decide ce que fait le capot plutot que /etc.
 *   energie.c  « sleep » en DELAY, pour verrouiller l'ecran AVANT que la
 *              machine ne s'endorme.
 *
 * La mecanique est la meme et tient en vingt lignes de GDBus, dont un
 * passage de descripteur par GUnixFDList que l'on se trompe une fois sur
 * deux a reecrire. Elle vit donc ici, et pas deux fois.
 * ========================================================================= */
#pragma once

#include <glib.h>

/* Pose un inhibiteur et rend SON DESCRIPTEUR, ou -1 en cas d'echec.
 *
 * LE DESCRIPTEUR EST L'INHIBITEUR : le fermer le leve, et le laisser ouvert
 * le maintient. C'est pourquoi cette fonction rend un int nu et non un
 * booleen -- l'appelant doit garder ce qu'il recoit, et savoir qu'en le
 * perdant il perd l'inhibition.
 *
 *   quoi     « sleep », « handle-lid-switch », « shutdown »… (voir la
 *            documentation de logind ; plusieurs se separent par « : »)
 *   pourquoi la phrase que « systemd-inhibit --list » montrera. Elle est
 *            lue par un humain qui cherche pourquoi sa machine ne dort
 *            pas : la rendre explicite coute une ligne.
 *   mode     « block » (logind ne fait rien) ou « delay » (logind attend,
 *            au plus InhibitDelayMaxSec, puis agit quand meme).
 */
int shell_logind_inhiber (const char *quoi, const char *pourquoi,
                          const char *mode);

/* logind sait-il faire « Can<methode> » ? Rend TRUE sur la seule reponse
 * « yes » : « no », « na » (pas de materiel pour ca) et « challenge » (il
 * faudrait s'authentifier) valent tous non pour nous.
 *
 * A INTERROGER AVANT D'AGIR. Demander une hibernation impossible rend une
 * erreur que personne ne lit, et la machine ne fait alors RIEN -- alors
 * qu'on la croyait a l'abri. */
gboolean shell_logind_sait_faire (const char *methode);

/* Appelle une methode de logind sans attendre la reponse. « false » est
 * toujours passe en argument : on ne force pas, et un inhibiteur pose par
 * une application qui a demande a ne pas etre interrompue l'emporte. */
void shell_logind_appeler (const char *methode);
