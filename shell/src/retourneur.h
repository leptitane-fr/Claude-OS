/* =========================================================================
 * Claude-OS Shell — le retourneur : une surface à deux faces
 *
 * Le dock a deux faces — le BUREAU (lanceur, applications) et l'ÉTABLI (les
 * outils de l'application active, voir outils.h). Ce widget les porte toutes
 * les deux et les échange par une rotation autour de son axe horizontal.
 *
 * Pourquoi un retournement, et pas un fondu ou un glissé : les deux faces
 * ne sont pas deux pages d'un même livre, c'est le MÊME OBJET vu de
 * l'autre côté. Le geste doit le dire. Un fondu laisserait croire qu'on a
 * remplacé le dock ; le retournement dit qu'on l'a tourné.
 *
 * -------------------------------------------------------------------------
 * LA CONTRAINTE QUI A DICTÉ TOUTE LA CONCEPTION
 * -------------------------------------------------------------------------
 *
 * labwc 0.8.3 replace un popover ouvert depuis l'ANCIENNE origine de la
 * surface qui le porte : redimensionner cette surface l'envoie hors de
 * l'écran. Ce projet l'a payé trois fois — le centre de notifications, la
 * nappe du dock, les tiroirs. Or les deux faces n'ont pas la même largeur.
 *
 * DEUX PARADES, ET ELLES TIENNENT ENSEMBLE :
 *
 *   1. LE RETOURNEUR MESURE TOUJOURS AU PLUS LARGE des deux faces. La
 *      surface a donc, au repos comme en mouvement, la taille de la plus
 *      encombrante : ELLE NE CHANGE PAS PENDANT LE FLIP. Ce qui s'élargit,
 *      c'est la pilule — le fond arrondi, dessiné en CSS sur la face, pas
 *      sur la fenêtre.
 *
 *   2. `sur_depart` est appelé AVANT la première image. Le dock y ferme ses
 *      popovers. Ce n'est pas une politesse : une liste de fenêtres ouverte
 *      au survol, laissée en place pendant que sa face part en rotation,
 *      resterait plantée au milieu de l'écran.
 *
 * Il reste un cas que le retourneur ne peut pas traiter seul : quand le
 * CONTENU d'une face change — un fil d'Ariane qui s'allonge —, le maximum
 * change, donc la surface aussi. C'est au dock de fermer ses surfaces à ce
 * moment-là ; voir dock.c.
 *
 * -------------------------------------------------------------------------
 * DISCIPLINE D'ÉNERGIE
 * -------------------------------------------------------------------------
 *
 * L'horloge des images ne bat que pendant le mouvement — environ 260 ms —
 * puis le rappel se retire de lui-même. Au repos, le retourneur ne coûte
 * rien : c'est la règle de la glissière, et elle vaut ici pour la même
 * raison. Si l'utilisateur a coupé les animations (`gtk-enable-animations`),
 * la bascule est immédiate et aucune horloge n'est armée.
 *
 * LA FACE CACHÉE N'EST NI DESSINÉE NI CLIQUABLE : `gtk_widget_set_child_
 * visible` la retire des deux à la fois. Sans cela, un clic tombant sur la
 * face arrière atteindrait un bouton qu'on ne voit pas — et la rotation
 * n'étant qu'un effet de dessin, GTK n'aurait aucune raison de l'empêcher.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SHELL_TYPE_RETOURNEUR (shell_retourneur_get_type ())
G_DECLARE_FINAL_TYPE (ShellRetourneur, shell_retourneur, SHELL, RETOURNEUR, GtkWidget)

/* Avant la première image d'un mouvement, et seulement si un mouvement a
 * bien lieu. LE DOCK Y FERME SES POPOVERS -- voir l'en-tête. */
typedef void (*ShellRetourneurDepart) (gpointer user_data);

/* Fin d'un mouvement : `arriere` dit sur quelle face il a abouti. */
typedef void (*ShellRetourneurFin) (gboolean arriere, gpointer user_data);

/* Après chaque allocation : la géométrie de la FACE VISIBLE, en coordonnées
 * de la fenêtre. Le dock s'en sert pour poser sa zone d'entrée -- la
 * surface étant taillée au plus large des deux faces, elle déborde de ce
 * qu'on voit, et une région d'entrée calquée sur la fenêtre avalerait des
 * clics destinés à l'application dessous. */
typedef void (*ShellRetourneurAllocation) (int x, int y, int largeur, int hauteur,
                                           gpointer user_data);

/* Les deux faces. Le retourneur les adopte ; il part sur l'avant. */
GtkWidget *shell_retourneur_new (GtkWidget *avant, GtkWidget *arriere);

/* Tourner vers une face. Rappeler avec la face déjà montrée ne fait rien,
 * et n'appelle donc ni `sur_depart` ni `sur_fin`. Demander l'autre face en
 * plein mouvement inverse la rotation depuis où elle en est, sans à-coup. */
void shell_retourneur_montrer (ShellRetourneur *r, gboolean arriere);

/* La face visée -- celle où le mouvement en cours aboutira. */
gboolean shell_retourneur_face (ShellRetourneur *r);

/* Arrivé, et immobile. C'est la condition pour accrocher un popover à ce
 * qu'il porte : posé sur une face en rotation, il serait placé une fois
 * pour toutes là où la face se trouvait. */
gboolean shell_retourneur_en_place (ShellRetourneur *r);

void shell_retourneur_sur_depart     (ShellRetourneur *r,
                                      ShellRetourneurDepart f, gpointer data);
void shell_retourneur_sur_fin        (ShellRetourneur *r,
                                      ShellRetourneurFin f, gpointer data);
void shell_retourneur_sur_allocation (ShellRetourneur *r,
                                      ShellRetourneurAllocation f, gpointer data);

G_END_DECLS
