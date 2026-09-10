/* Claude OS -- le moteur de lecture : demux, decodage, synchronisation.
 *
 * CE QUI COMMANDE LE TEMPO, ET CE QUI SUIT.
 *
 *   l'audio           donne l'heure (video-audio.h dit pourquoi)
 *   le compositeur    donne le rythme d'affichage, par le frame clock de GTK
 *   le fil de decodage travaille a l'avance, et s'endort des qu'il a pris
 *                     trois images d'avance
 *
 * Aucun minuteur periodique dans tout le lecteur. C'est la premiere des huit
 * regles d'energie (docs/11 §11.5), et elle a une consequence qu'on obtient
 * gratuitement : QUAND LA FENETRE EST MASQUEE, le compositeur cesse d'envoyer
 * ses « frame callbacks », GTK cesse de battre, plus personne ne depile
 * d'image, le fil de decodage remplit sa file de trois images puis s'endort
 * sur une condition. Le decodage video s'arrete de lui-meme, sans une ligne
 * de code pour le detecter, et sans jamais se tromper -- la ou une
 * heuristique de visibilite se serait trompee.
 *
 * L'audio, lui, continue : c'est ce qu'attend quiconque reduit une fenetre
 * sans vouloir interrompre le son.
 */

#ifndef CLAUDE_OS_VIDEO_MOTEUR_H
#define CLAUDE_OS_VIDEO_MOTEUR_H

#include <gtk/gtk.h>
#include <libavutil/frame.h>

typedef struct _VideoMoteur VideoMoteur;

typedef enum {
    VIDEO_ARRETE = 0,
    VIDEO_LIT,
    VIDEO_EN_PAUSE,
    VIDEO_FINI,
} VideoEtat;

/* Tous ces rappels arrivent dans le fil principal, jamais dans celui du
 * decodage : l'interface n'a donc aucun verrou a prendre. */
typedef struct {
    void (*etat_change)(VideoEtat etat, gpointer u);
    void (*fin_atteinte)(gpointer u);
    void (*erreur)(const char *message, gpointer u);
} VideoRappels;

VideoMoteur *video_moteur_ouvrir(const char *chemin,
                                 const VideoRappels *rappels, gpointer u,
                                 GError **erreur);
void         video_moteur_fermer(VideoMoteur *m);

void         video_moteur_lire(VideoMoteur *m);
void         video_moteur_pause(VideoMoteur *m);
void         video_moteur_basculer(VideoMoteur *m);
VideoEtat    video_moteur_etat(VideoMoteur *m);

double       video_moteur_duree(VideoMoteur *m);     /* secondes, 0 si inconnue */
double       video_moteur_position(VideoMoteur *m);  /* secondes               */
void         video_moteur_sauter(VideoMoteur *m, double secondes);
void         video_moteur_avancer(VideoMoteur *m, double delta);

gboolean     video_moteur_a_audio(VideoMoteur *m);
gboolean     video_moteur_a_video(VideoMoteur *m);
int          video_moteur_largeur(VideoMoteur *m);
int          video_moteur_hauteur(VideoMoteur *m);

/* Le nom du codec et la maniere dont il est decode -- « materiel » ou
 * « logiciel ». L'interface le montre : sur cette machine, la difference
 * entre les deux est de trois quarts de watt, et l'utilisateur a le droit de
 * savoir quand il paie ce prix. */
const char  *video_moteur_codec(VideoMoteur *m);
gboolean     video_moteur_materiel(VideoMoteur *m);

/* L'IMAGE A AFFICHER MAINTENANT, ou NULL s'il n'y a rien de nouveau a
 * montrer. A appeler depuis le frame clock, et de nulle part ailleurs.
 * L'appelant devient proprietaire de la trame rendue et doit la liberer par
 * av_frame_free(). */
AVFrame     *video_moteur_image_due(VideoMoteur *m);

/* Combien d'images ont ete sautees parce qu'elles arrivaient trop tard.
 * Zero est la seule valeur acceptable en regime etabli ; c'est le premier
 * chiffre a regarder quand la lecture parait heurtee. */
gint64       video_moteur_images_sautees(VideoMoteur *m);
gint64       video_moteur_images_vues(VideoMoteur *m);

/* L'ECART DE SYNCHRONISATION, EN SECONDES : de combien l'image affichee est
 * en avance ou en retard sur l'horloge au moment ou on l'affiche. C'est la
 * mesure objective de ce que l'oreille appelle « le son est decale ». Une
 * moyenne sous 10 ms et un maximum sous 40 ms -- une image a 25 im/s -- sont
 * ce qu'on vise ; au-dela, cela se voit sur des levres qui parlent. */
double       video_moteur_ecart_moyen(VideoMoteur *m);
double       video_moteur_ecart_max(VideoMoteur *m);

#endif
