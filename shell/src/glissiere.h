/* =========================================================================
 * Claude-OS Shell — glissiere : faire sortir une surface par le bas
 *
 * Un conteneur a un enfant, pose entre la fenetre layer-shell et son contenu.
 * Il ne change jamais de taille : il DEPLACE son enfant, par une translation
 * appliquee a l'allocation. La fenetre garde ses dimensions, le compositeur
 * n'est donc pas sollicite image par image -- seul le contenu bouge, dans
 * une surface qui ne bouge pas.
 *
 * Cacher : l'enfant descend hors de la surface, PUIS la fenetre est retiree.
 * Retiree, et pas seulement vide : une surface transparente laissee en place
 * garderait sa zone d'entree et avalerait les clics destines a l'application
 * dessous.
 *
 * Montrer : la fenetre est remise, l'enfant part d'en bas et remonte.
 *
 * DISCIPLINE D'ENERGIE. L'horloge des images ne bat que pendant le mouvement
 * -- environ 220 ms -- puis le rappel se retire de lui-meme. Au repos, la
 * glissiere ne coute rien. Si l'utilisateur a coupe les animations
 * (gtk-enable-animations), le deplacement est immediat.
 *
 * Le shell avait d'abord fait le choix inverse -- apparition instantanee,
 * « deplacer une surface demanderait un reveil par image ». Deux choses ont
 * change : la sortie par le bas est demandee (11 septembre 2026), et l'on ne
 * deplace pas la surface, seulement son contenu. Mesure au banc dans la
 * trace Wayland : une quinzaine d'images par mouvement, et AUCUNE pendant
 * les trois secondes de repos qui suivaient.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SHELL_TYPE_GLISSIERE (shell_glissiere_get_type ())
G_DECLARE_FINAL_TYPE (ShellGlissiere, shell_glissiere, SHELL, GLISSIERE, GtkWidget)

/* Fin d'un mouvement : `visible` dit ou il a abouti. */
typedef void (*ShellGlissiereFin) (gboolean visible, gpointer user_data);

/* Apres chaque allocation, avec la taille de la glissiere -- donc de la
 * fenetre. Le dock s'en sert pour recalculer sa zone d'entree. */
typedef void (*ShellGlissiereAllocation) (int largeur, int hauteur,
                                          gpointer user_data);

GtkWidget *shell_glissiere_new (GtkWidget *enfant);

void shell_glissiere_sur_fin        (ShellGlissiere *g, ShellGlissiereFin f,
                                     gpointer user_data);
void shell_glissiere_sur_allocation (ShellGlissiere *g, ShellGlissiereAllocation f,
                                     gpointer user_data);

/* Remet la fenetre si besoin, et fait remonter le contenu. */
void shell_glissiere_montrer (ShellGlissiere *g);

/* Fait descendre le contenu, puis retire la fenetre. */
void shell_glissiere_cacher (ShellGlissiere *g);

/* Visible, a sa place, et immobile. C'est la condition pour y accrocher un
 * popover : pose sur un contenu en mouvement, il serait place une fois pour
 * toutes a l'endroit ou le contenu se trouvait. */
gboolean shell_glissiere_en_place (ShellGlissiere *g);

G_END_DECLS
