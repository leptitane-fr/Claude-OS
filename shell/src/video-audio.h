/* Claude OS -- sortie audio du lecteur video, et horloge maitre.
 *
 * POURQUOI L'AUDIO PORTE L'HORLOGE, ET PAS UN MINUTEUR.
 *
 * Un lecteur doit choisir ce qui donne le tempo. Trois candidats :
 *
 *   -- un minuteur a la cadence de la video. C'est le plus simple, et c'est
 *      faux : l'horloge du systeme et celle de la carte son ne battent pas au
 *      meme rythme. La derive est de l'ordre de la seconde par heure, et elle
 *      s'entend bien avant de se voir.
 *   -- l'horloge du compositeur. Elle est reguliere, mais elle ne sait rien
 *      de ce qui sort du haut-parleur.
 *   -- l'audio lui-meme. Le son ne se rattrape pas : une image en retard se
 *      laisse sauter sans que personne ne le remarque, un echantillon en
 *      retard s'entend immediatement. C'est donc l'audio qui commande, et la
 *      video qui suit.
 *
 * PipeWire donne mieux qu'un compteur d'echantillons : pw_time.delay dit
 * combien de temps le prochain echantillon mettra a atteindre le
 * haut-parleur, filtres du graphe compris. L'horloge rendue ici est donc
 * celle de ce qu'on ENTEND, et non celle de ce qu'on a ecrit.
 *
 * UN MOT SUR L'ENERGIE. Le quantum demande est volontairement long --
 * 2048 echantillons, environ 43 ms. Un lecteur video n'a aucun besoin de
 * latence basse : personne n'interagit avec le son. Doubler le quantum divise
 * par deux le nombre de reveils du processeur, et ces reveils sont le poste
 * qu'aucune moyenne en watts ne montre.
 */

#ifndef CLAUDE_OS_VIDEO_AUDIO_H
#define CLAUDE_OS_VIDEO_AUDIO_H

#include <glib.h>

typedef struct _VideoAudio VideoAudio;

/* Ouvre la sortie. « taux » et « canaux » sont ceux du fichier : PipeWire
 * reechantillonne s'il le faut, et le fait mieux que nous. Rend NULL et
 * renseigne « erreur » en cas d'echec -- jamais en silence. */
VideoAudio *video_audio_ouvrir(int taux, int canaux, GError **erreur);

void        video_audio_fermer(VideoAudio *a);

/* Ecrit des echantillons entrelaces float. Rend le nombre d'images
 * effectivement acceptees, qui peut etre inferieur a « images » quand la file
 * est pleine : c'est au demandeur d'attendre, et cette attente est ce qui
 * empeche le decodeur de courir devant la lecture.
 *
 * « pts » est l'horodatage, en secondes, du PREMIER echantillon du bloc. */
int         video_audio_ecrire(VideoAudio *a, const float *entrelace,
                               int images, double pts);

/* Place disponible, en images. Zero signifie « repasse plus tard ». */
int         video_audio_place(VideoAudio *a);

/* L'horloge : position, en secondes, de ce qui SORT du haut-parleur en ce
 * moment. Rend FALSE tant que rien n'a encore ete joue -- le demandeur doit
 * alors se rabattre sur autre chose, et surtout pas sur zero. */
gboolean    video_audio_horloge(VideoAudio *a, double *secondes);

void        video_audio_pause(VideoAudio *a, gboolean en_pause);

/* Jette tout ce qui est en file. A appeler sur un saut : sans cela, on
 * entend encore l'ancienne position pendant une demi-seconde. */
void        video_audio_vider(VideoAudio *a);

/* Le son a-t-il atteint la fin de ce qui lui a ete donne ? Sert a decider de
 * la fin de la lecture, une fois le fichier epuise. */
gboolean    video_audio_epuise(VideoAudio *a);

#endif
