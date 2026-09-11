/* Claude OS -- la cinematheque du lecteur video.
 *
 * CE QU'ON VOIT QUAND AUCUN FILM N'EST OUVERT.
 *
 * Une fenetre vide n'est pas un etat : c'est une absence. A la place, deux
 * volets --
 *
 *   Recents       les films deja vus, le dernier d'abord
 *   Bibliotheque  tout ce que contiennent les dossiers qu'on a declares
 *
 * -- et, a la toute premiere ouverture, un seul bouton : « Sélectionner un
 * dossier… ». On ne demande pas a l'utilisateur de comprendre une interface
 * vide.
 *
 * TROIS CONTRAINTES ONT DESSINE CE MODULE, et elles ne sont pas negociables
 * sur cette machine :
 *
 *   -- LES DOSSIERS SONT SOUVENT SUR LE RESEAU. Tout parcours est donc
 *      asynchrone, sans exception : une enumeration synchrone sur un serveur
 *      endormi gele la fenetre entiere jusqu'au delai TCP. Fichiers a paye
 *      cette lecon, le lecteur l'a repayee sur ses lectures.
 *   -- UNE VIGNETTE COUTE UN DECODAGE. Elles sont donc fabriquees par UN
 *      seul fil, a la demande, et gardees en cache sur le disque. Un dossier
 *      de deux cents films ne doit pas declencher deux cents decodages a
 *      l'ouverture.
 *   -- RIEN N'EST FABRIQUE POUR CE QU'ON NE REGARDE PAS. Une vignette n'est
 *      demandee qu'au moment ou sa carte apparait a l'ecran.
 */

#ifndef CLAUDE_OS_VIDEO_BIBLIOTHEQUE_H
#define CLAUDE_OS_VIDEO_BIBLIOTHEQUE_H

#include <gtk/gtk.h>

typedef struct _VideoBib VideoBib;

/* Appele quand l'utilisateur choisit un film, et quand il demande la boite
 * d'ouverture ordinaire. */
typedef void (*VideoBibChoix)(const char *chemin, gpointer u);
typedef void (*VideoBibOuvrir)(gpointer u);

VideoBib   *video_bib_nouveau(VideoBibChoix choix, VideoBibOuvrir ouvrir,
                              gpointer usager);
void        video_bib_liberer(VideoBib *b);

/* Le widget a poser dans la fenetre. Il appartient a la bibliotheque. */
GtkWidget  *video_bib_widget(VideoBib *b);

/* Un film vient d'etre ouvert : il passe en tete des recents, et son dossier
 * entre dans la bibliotheque s'il n'y etait pas. C'est ce qui fait qu'on n'a
 * jamais a declarer un dossier deux fois. */
void        video_bib_vue(VideoBib *b, const char *chemin);

/* Relit le catalogue et refait les deux volets. */
void        video_bib_rafraichir(VideoBib *b);

#endif
