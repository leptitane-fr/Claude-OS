/* Claude OS -- de l'image decodee a la texture GTK.
 *
 * C'est ici que se joue tout le benefice energetique du lecteur, et il tient
 * en une phrase : L'IMAGE NE DOIT JAMAIS ETRE RECOPIEE.
 *
 * Le chemin vise, mesure le 10 septembre 2026 sur MADOO (voir docs/11) :
 *
 *     VA-API -> av_hwframe_map -> dmabuf -> GdkDmabufTexture
 *
 * Ce que coutent les autres chemins, sur la meme mire 1080p30, repos a
 * 2,92 W :
 *
 *     ce chemin                            3,24 W
 *     decodage logiciel                    4,08 W
 *     materiel PUIS av_hwframe_transfer_data   5,45 W
 *
 * Le chemin naif -- decoder au materiel puis redescendre l'image en memoire
 * centrale -- coute donc HUIT FOIS le chemin retenu, et davantage que de
 * tout decoder au logiciel. Relire une surface tuilee depuis la memoire du
 * GPU est lent et cher, sans apparaitre comme du temps processeur. C'est le
 * piege principal de ce module, et la raison de son existence separee.
 */

#ifndef CLAUDE_OS_VIDEO_IMAGE_H
#define CLAUDE_OS_VIDEO_IMAGE_H

#include <gtk/gtk.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>

typedef struct {
    struct SwsContext *conv;        /* repli logiciel seulement            */
    AVFrame           *recue;       /* image redescendue, repli seulement  */
    gboolean           dit_dmabuf;  /* le diagnostic n'est ecrit qu'une fois */
    gboolean           dit_repli;
    gint64             sans_copie;  /* combien d'images par chaque chemin, */
    gint64             recopiees;   /* pour que le journal puisse le dire  */
} VideoImage;

void        video_image_init(VideoImage *vi);
void        video_image_fin(VideoImage *vi);

/* Fabrique une texture a partir d'une image decodee. Prend le chemin sans
 * copie quand c'est possible, retombe sur la conversion logicielle sinon --
 * en le DISANT au journal, une fois, plutot qu'en le taisant. La trame reste
 * la propriete de l'appelant : la texture prend ce qu'il lui faut. */
GdkTexture *video_image_texture(VideoImage *vi, AVFrame *trame,
                                GdkDisplay *ecran);

#endif
