/* =========================================================================
 * Claude OS — les suggestions de mots du clavier à l'écran.
 *
 * POURQUOI ELLES NE SONT PAS UN LUXE
 *
 * Le mode console n'offre que vingt-cinq touches sous le pouce gauche, et
 * l'utilisateur l'a dit en demandant cette fonction : sans suggestions, un
 * clavier tactile réduit est laborieux. Elles rendent aussi les lettres
 * qu'on a retirées du calque — k, w, l'apostrophe, les accents rares :
 * « aujourd » propose « aujourd'hui », « eleve » ne propose rien mais
 * « él » propose « élève ».
 *
 * D'OÙ VIENNENT LES MOTS
 *
 * De Lexique 3.83 (CC BY-SA), 119 688 formes du français avec leur
 * fréquence d'usage, préparées par tools/fabrique-mots.py et installées en
 * mots-fr.txt. Le fichier est trié SUR LA FORME SANS ACCENTS, et on y
 * cherche un préfixe par DICHOTOMIE, dans une projection en lecture seule —
 * rien n'est recopié en mémoire, et les Mo restent dans le cache de pages,
 * partagés, sur une machine qui n'a que 4 Go.
 *
 * ET DES SUITES DE MOTS
 *
 * Un dictionnaire de mots isolés ne sait pas que « vous » suit « comment ».
 * Les suites viennent de Tatoeba (CC BY 2.0 FR), 726 280 phrases françaises
 * — de la langue parlée, celle qu'on écrit sur une tablette. Préparées elles
 * aussi par tools/fabrique-mots.py, en suites-fr.txt.
 *
 * ET DE CE QUE L'ON ÉCRIT
 *
 * Tout mot choisi dans les suggestions est retenu dans un fichier à part,
 * avec son compte. Un mot appris remonte dans la liste, et un mot inconnu
 * du dictionnaire — un nom propre, un mot de métier — finit par être
 * proposé lui aussi. C'est ce qui fait qu'un clavier devient le vôtre.
 * ========================================================================= */
#pragma once

#include <glib.h>

/* Projette le dictionnaire et relit les mots appris. FALSE si le fichier
 * manque : le clavier marche alors sans suggestions, et le dit. */
gboolean shell_mots_init (void);

/* Les meilleures suggestions, dans l'ordre. Rend leur nombre (au plus
 * `max`) et remplit `sortie` de chaînes à libérer.
 *
 *   prefixe    ce qui est tapé du mot en cours. Comparé SANS ACCENTS :
 *              « eleve » propose « élève ». Un préfixe vide demande une
 *              prédiction : quels mots suivent d'ordinaire le précédent.
 *   precedent  le mot d'avant, ou NULL au début d'une phrase. C'est lui
 *              qui fait la différence entre un dictionnaire et un clavier :
 *              après « comment », « vous » passe devant « voiture ». */
guint shell_mots_suggerer (const char *prefixe, const char *precedent,
                           char **sortie, guint max);

/* Retient un mot choisi. L'écriture sur disque est différée : on ne touche
 * pas l'eMMC à chaque mot. */
void shell_mots_apprendre (const char *mot);
