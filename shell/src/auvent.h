/* =========================================================================
 * Claude-OS Shell — l'auvent : ce qui ne tient pas sur une ligne
 *
 * Un volet qui monte au-dessus de la pilule du dock, ouvert par un bouton de
 * la zone « outils » (voir outils.h). C'est là que va tout ce qu'une ligne
 * de 72 px ne peut pas porter : une saisie, une liste, un réglage.
 *
 * -------------------------------------------------------------------------
 * LE DOCK DESSINE, L'APPLICATION NE VOIT QUE LA VALEUR
 * -------------------------------------------------------------------------
 *
 * Une application ne décrit pas un champ, elle demande UNE SAISIE. Le dock
 * choisit le widget, la police, les marges ; l'application reçoit le texte
 * par une action, et rien d'autre.
 *
 * Ce n'est pas une limite qu'on lèvera plus tard : c'est la raison d'être du
 * contrat. Deux processus, une seule surface -- une application ne PEUT PAS
 * dessiner dans celle du dock. Et un vocabulaire fermé garantit par
 * construction que deux applications se ressemblent, là où un langage de
 * description d'interface garantirait le contraire.
 *
 * Un contrôle écrit à ce jour : « saisie ». « liste » et « choix » sont
 * prévus et se refusent en le disant, plutôt que d'ouvrir un volet vide.
 *
 * -------------------------------------------------------------------------
 * LE CLAVIER, ET C'EST LE VRAI SUJET
 * -------------------------------------------------------------------------
 *
 * Le dock n'a jamais pris le clavier : « la saisie continue d'aller à la
 * fenêtre active même quand la souris le survole » (dock.c). C'était une
 * règle simple parce que rien, dans le dock, ne se tapait.
 *
 * Une saisie change cela. La surface layer-shell doit passer en
 * KEYBOARD_MODE_ON_DEMAND le temps que l'auvent est ouvert, et revenir à
 * NONE ensuite -- sans quoi le dock garderait le clavier de la session
 * entière pour un champ refermé depuis longtemps.
 *
 * C'EST LE DOCK QUI LE FAIT, PAS L'AUVENT : le mode se pose sur la fenêtre,
 * et l'auvent ne connaît pas la sienne. Il prévient par `sur_ouverture`, et
 * le dock en tire la conséquence -- comme il en tire la fermeture de ses
 * popovers.
 *
 * -------------------------------------------------------------------------
 * LA TAILLE DE LA SURFACE CHANGE, ET C'EST INÉVITABLE
 * -------------------------------------------------------------------------
 *
 * Le retourneur a pu éviter que la largeur bouge, en mesurant au plus large
 * des deux faces. L'auvent ne peut pas en faire autant : réserver sa hauteur
 * en permanence laisserait un vide au-dessus du dock tout le temps.
 *
 * La hauteur change donc, une fois à l'ouverture et une fois à la fermeture.
 * Les popovers sont fermés aux deux, par le même chemin que le retournement
 * -- labwc 0.8.3 les replace depuis l'ancienne origine de leur surface.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SHELL_TYPE_AUVENT (shell_auvent_get_type ())
G_DECLARE_FINAL_TYPE (ShellAuvent, shell_auvent, SHELL, AUVENT, GtkWidget)

/* La valeur du contrôle, à chaque changement. Pour une saisie : le texte,
 * frappe par frappe -- une recherche doit filtrer pendant qu'on tape. La
 * fermeture en envoie une dernière, VIDE : c'est ainsi qu'une application
 * sait qu'il faut rendre la liste complète. */
typedef void (*ShellAuventValeur) (const char *valeur, gpointer user_data);

/* L'auvent s'ouvre ou se ferme. LE DOCK Y POSE LE MODE CLAVIER DE SA
 * SURFACE, et y ferme ses popovers : la hauteur va changer. */
typedef void (*ShellAuventOuverture) (gboolean ouvert, gpointer user_data);

GtkWidget *shell_auvent_new (void);

void shell_auvent_sur_ouverture (ShellAuvent *a, ShellAuventOuverture f,
                                 gpointer data);

/* Ouvrir sur un contrôle. `controle` est un des mots du contrat
 * (SHELL_OUTILS_SAISIE…) ; un mot inconnu se refuse et se dit.
 *
 * Rappeler sur un auvent déjà ouvert AVEC LE MÊME contrôle ne le referme
 * pas : c'est le même outil qu'on redemande, et une bascule ferait
 * disparaître ce qu'on était en train d'écrire. Un contrôle différent
 * remplace le contenu sans refermer le volet. */
gboolean shell_auvent_ouvrir (ShellAuvent *a, const char *controle,
                              const char *invite, ShellAuventValeur f,
                              gpointer data);

/* Referme, et envoie la valeur vide. Sans effet s'il est déjà fermé. */
void shell_auvent_fermer (ShellAuvent *a);

gboolean shell_auvent_ouvert (ShellAuvent *a);

/* Le contrôle en place, ou NULL. Sert à savoir si l'on redemande le même. */
const char *shell_auvent_controle (ShellAuvent *a);

G_END_DECLS
