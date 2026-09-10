/* =========================================================================
 * Claude-OS — Images : la toile
 *
 * Le widget qui dessine l'image, et qui seul sait ce qu'un doigt veut dire.
 *
 * POURQUOI PAS UN GtkPicture
 *
 * GtkPicture affiche une image ajustée, et c'est tout. Il faut ici trois
 * choses qu'il ne sait pas faire :
 *
 *   - faire GLISSER l'image sous le doigt, avec sa voisine qui entre par le
 *     bord — c'est ce qui rend le balayage lisible : on voit où l'on va
 *     avant d'avoir levé le doigt, et l'on peut revenir sur sa décision ;
 *   - zoomer AUTOUR D'UN POINT — celui du pincement, ou le pointeur sous
 *     Ctrl+molette — puis déplacer l'image agrandie ;
 *   - remplacer la texture par une plus fine SANS QUE L'IMAGE BOUGE : la
 *     géométrie est tenue en pixels de l'image d'origine, et la texture
 *     n'est qu'un moyen de la peindre. Voir images.c, sur le décodage à la
 *     taille de l'écran.
 *
 * LE ZOOM est en pixels de l'ÉCRAN par pixel de l'image : 1,0 veut dire
 * « taille réelle », quel que soit le facteur d'échelle de la sortie.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

#define IMAGES_TYPE_VUE (images_vue_get_type ())
G_DECLARE_FINAL_TYPE (ImagesVue, images_vue, IMAGES, VUE, GtkWidget)

GtkWidget *images_vue_new (void);

/* L'image courante. « largeur » et « hauteur » sont celles de l'image
 * d'ORIGINE, orientation EXIF appliquée — pas celles de la texture, qui peut
 * être réduite. Texture NULL : l'image est en cours de décodage, la toile
 * reste vide.
 *
 * Remet le zoom en « ajusté » et annule la rotation : chaque image s'ouvre
 * entière, comme on s'y attend. */
void images_vue_set_image (ImagesVue  *v,
                           GdkTexture *texture,
                           int         largeur,
                           int         hauteur);

/* Même image, texture différente : plus fine après un zoom, ou l'image
 * suivante d'une animation. Ni le zoom ni la position ne bougent. */
void images_vue_set_texture (ImagesVue *v, GdkTexture *texture);

/* Les voisines, dessinées pendant le balayage. sens : -1 la précédente,
 * +1 la suivante.
 *
 * « existe » et « texture » sont distincts à dessein : une voisine peut
 * exister sans être encore décodée. C'est son EXISTENCE qui décide si le
 * balayage peut aboutir ; sans elle, l'image résiste au doigt et revient. */
void images_vue_set_voisine (ImagesVue  *v,
                             int         sens,
                             gboolean    existe,
                             GdkTexture *texture,
                             int         largeur,
                             int         hauteur);

void     images_vue_zoom_ajuste (ImagesVue *v);
void     images_vue_zoom_reel   (ImagesVue *v);
/* Multiplie le zoom, autour du centre de la toile. */
void     images_vue_zoomer      (ImagesVue *v, double facteur);
double   images_vue_get_zoom    (ImagesVue *v);
gboolean images_vue_est_ajustee (ImagesVue *v);

/* Rotation d'AFFICHAGE, par quart de tour : +1 horaire, -1 anti-horaire.
 * Le fichier n'est jamais réécrit. */
void     images_vue_pivoter     (ImagesVue *v, int sens);

/* La texture est-elle trop grossière pour le zoom actuel ? Vrai quand un
 * pixel de la texture couvre plus d'un pixel de l'écran ET qu'il existe mieux
 * dans le fichier. C'est le signal pour décoder en pleine résolution. */
gboolean images_vue_manque_de_details (ImagesVue *v);

/* Masque le pointeur au-dessus de l'image : en plein écran, une fois les
 * commandes effacées, rien ne doit rester devant la photo. */
void     images_vue_masquer_curseur (ImagesVue *v, gboolean masquer);

/* SIGNAUX
 *
 *   « naviguer » (int sens)  un balayage ou un cran de molette demande
 *                            l'image voisine. La toile ne connaît pas la
 *                            liste : c'est à l'application d'y aller, puis de
 *                            rappeler images_vue_set_image.
 *   « zoom-change »          le zoom ou l'ajustement a changé.
 *   « touche »               un appui bref au doigt ou au stylet, sans
 *                            mouvement — pour montrer ou masquer les
 *                            commandes. Annoncé avec un léger retard, le
 *                            temps de savoir qu'aucun second appui ne suit.
 *                            Pas émis pour la souris : un clic n'a pas à
 *                            faire disparaître ce qu'on survole.
 *   « double-clic »          double appui, au doigt comme à la souris.
 *
 * GESTES QUE LA TOILE TRAITE ELLE-MÊME, sans rien annoncer d'autre qu'un
 * changement de zoom : pincer pour zoomer, tourner à deux doigts pour
 * pivoter — calé au quart de tour le plus proche au lever.
 */
