/* =========================================================================
 * Claude OS — la rotation de l'écran, en mode tablette seulement.
 *
 * Capot ouvert, l'écran est en paysage et le reste : l'image ne tourne pas
 * parce qu'on penche la machine sur ses genoux. Écran retourné, elle suit la
 * façon dont on tient la tablette.
 *
 * LE CAPTEUR
 *
 * L'accéléromètre de l'écran (« accel-display », cros-ec-accel). Mesuré le
 * 11 septembre 2026 sur MADOO : 1 g vaut environ 16384 en brut ; +y pointe
 * vers le haut de l'écran, +z hors de l'écran, vers l'utilisateur. Le sens
 * de x n'a pas été mesuré séparément : il est tenu pour dirigé vers la
 * droite, ce qui fait un repère direct — voir SENS_X dans rotation.c.
 *
 * UNE SCRUTATION, ET ELLE EST ASSUMÉE
 *
 * Ce projet n'en veut pas ; celle-ci est la seule exception, et elle est
 * bornée. L'EC échantillonne déjà ce capteur à 15,6 Hz en permanence — il
 * en a besoin pour calculer l'angle du capot et décider du mode tablette —,
 * donc le lire ne réveille aucun matériel : c'est une commande à l'EC, deux
 * fois par seconde, ET SEULEMENT en mode tablette. Capot ouvert, aucun
 * minuteur n'existe. La voie par tampon IIO (/dev/iio:deviceN) exigerait
 * d'écrire en root dans sysfs pour l'armer, pour un gain nul.
 *
 * APPLIQUER
 *
 * Par wlr-randr, qui parle wlr-output-management au compositeur. Un
 * processus par rotation — un événement rare — plutôt qu'un client du
 * protocole à entretenir dans le dock. Le tactile et le stylet suivent parce
 * que rc.xml les associe à la sortie (mapToOutput) : sans cette association,
 * wlroots les rapporte à l'écran non tourné et le doigt tombe à côté.
 * ========================================================================= */
#pragma once

#include <glib.h>

/* Démarre ou arrête le suivi. En quittant, l'écran revient en paysage. */
void shell_rotation_suivre (gboolean actif);

/* Verrou d'orientation : l'écran reste où il est, même en mode tablette. */
void shell_rotation_verrouiller (gboolean verrou);
gboolean shell_rotation_verrouillee (void);
