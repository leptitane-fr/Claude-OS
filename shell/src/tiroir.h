/* =========================================================================
 * Claude-OS Shell — les tiroirs des bords lateraux
 *
 * CE QUI S'OUVRAIT AU CLIC S'OUVRE MAINTENANT AU GESTE.
 *
 * La Console etait un popover accroche a la barre d'etat : on visait une
 * pilule de deux lignes, en bas a droite, et elle se depliait. La barre a
 * disparu au profit du coin, qui ne recoit plus ni clic ni survol -- il
 * fallait donc une autre porte, et elle ne pouvait plus etre une cible de
 * quelques pixels.
 *
 * DEUX TIROIRS, UN PAR BORD, ET ILS NE SE CONSULTENT PAS.
 *
 *   - A GAUCHE, LES WIDGETS A VENIR. Le volet est vide, et il le dit ; il
 *     prend toute la hauteur. C'est une place reservee autant qu'une mise en
 *     page : le jour ou le premier widget arrive, le volet n'aura qu'a se
 *     remplir. Il a quitte le bord droit le 15 septembre 2026, ou il
 *     partageait une colonne avec la Console : deux choses sans rapport
 *     empilees au meme bord se lisaient comme une seule.
 *
 *   - A DROITE, LA CONSOLE, centree verticalement. Le volet fait sa hauteur
 *     et pas davantage -- etire, il laisserait la rangee d'alimentation
 *     flotter au bas d'un grand vide.
 *
 * Les deux s'ouvrent et se ferment chacun de leur cote, et partagent LA
 * fenetre plein ecran qui porte la nappe : deux nappes superposees se
 * seraient disputees le clic exterieur.
 *
 * DEUX FACONS DE LES OUVRIR, ET ELLES NE SE VALENT PAS.
 *
 *   - AU DOIGT : un glisser depuis le bord vers l'interieur de l'ecran.
 *     C'est le geste de tous les tiroirs lateraux, celui qu'on essaie sans
 *     qu'on vous l'explique. Le sens compte : un glisser qui s'eloigne de
 *     l'ecran n'ouvre rien.
 *
 *   - AU POINTEUR : le curseur POSE contre le bord, et tenu la une seconde.
 *     Pas un clic, pas une entree : une ATTENTE. Les bords lateraux sont
 *     l'endroit ou finit tout mouvement de souris un peu vif, et un tiroir
 *     qui s'ouvrirait a l'instant du contact s'ouvrirait surtout par
 *     accident. La seconde est le prix a payer pour que le geste soit
 *     toujours volontaire -- demande explicitement, et pour cette raison.
 *
 * TOUT CLIC AILLEURS LES REFERME. Une nappe transparente couvre l'ecran tant
 * qu'un tiroir est ouvert et recueille ce clic -- exactement le mecanisme
 * du dock rappele par-dessus une application, et pour la meme raison :
 * labwc ne signale rien quand on revient a la fenetre deja active.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

/* Cree les deux lisieres et les deux tiroirs, fermes. `console` est le
 * contenu a poser dans le volet de droite -- celui de panel_new(). */
void shell_tiroir_init (GtkApplication *app, GtkWidget *console,
                        gboolean apercu);

/* La Console, au bord droit. */
void shell_tiroir_console_ouvrir   (void);
void shell_tiroir_console_basculer (void);

/* Les widgets, au bord gauche. */
void shell_tiroir_widgets_ouvrir   (void);
void shell_tiroir_widgets_basculer (void);

/* LES DEUX A LA FOIS. Sert de ConsoleFermer a la rangee d'alimentation :
 * ce qu'elle demande avant d'eteindre ou d'ouvrir les Reglages, c'est que
 * l'ecran soit rendu, pas qu'un volet precis rentre. */
void shell_tiroir_fermer (gpointer inutilise);

/* L'un ou l'autre est-il deploye ? */
gboolean shell_tiroir_ouvert (void);
