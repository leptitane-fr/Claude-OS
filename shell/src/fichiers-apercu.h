/* =========================================================================
 * Claude-OS Shell — gestionnaire de fichiers : les vignettes
 *
 * La vue Apercu montre les images par leur contenu. Les vignettes suivent
 * la specification freedesktop : ~/.cache/thumbnails/normal (128 px) ou
 * large (256 px), un PNG par fichier, nomme d'apres le MD5 de son adresse.
 * Ce cache est PARTAGE -- le selecteur de fichiers de GTK le lit, d'autres
 * programmes l'alimentent -- et c'est lui qui rend une deuxieme visite d'un
 * dossier de photos instantanee.
 *
 * FIL SEPARE, DEUX A LA FOIS, LES DERNIERES DEMANDEES D'ABORD
 *
 * Decoder une photo prend des dizaines de millisecondes et des megaoctets ;
 * la machine a 4 Go soudes. Deux decodages simultanes au plus, et la file
 * est servie par la fin : ce qu'on vient de faire defiler a l'ecran passe
 * avant ce qu'on a deja depasse. Une case recyclee retire sa demande si
 * elle n'a pas encore commence.
 *
 * MEMOIRE BORNEE
 *
 * Les textures vivent sur les elements, mais au plus APERCU_MAX a la fois :
 * au-dela, les plus anciennes sont rendues. Les recharger depuis le cache
 * disque coute une milliseconde ; les garder toutes, sur un dossier de
 * trois mille photos, couterait 150 Mo.
 * ========================================================================= */
#pragma once

#include "fichiers.h"

typedef enum {
    APERCU_INCONNU = 0,     /* jamais demande, ou rendu par la borne     */
    APERCU_ATTENTE,         /* en file ou en cours de decodage           */
    APERCU_PRET,            /* it->apercu est pose                       */
    APERCU_AUCUN,           /* pas une image, ou illisible               */
} ApercuEtat;

/* Taille des vignettes en pixels physiques : 128 ou 256 selon l'echelle de
 * l'ecran. Un changement vide ce qui a ete fait a l'autre taille. */
void        fichiers_apercu_regler (int facteur_echelle);

/* La vignette de `it` si elle est prete, NULL sinon -- et dans ce cas elle
 * est demandee, et « apercu-pret » sera emis a son arrivee. */
GdkTexture *fichiers_apercu_obtenir (FichierItem *it);

/* L'element quitte l'ecran : sa demande est retiree si elle attend encore. */
void        fichiers_apercu_oublier (FichierItem *it);

/* On change de dossier : tout ce qui attend est abandonne. */
void        fichiers_apercu_abandonner (void);
