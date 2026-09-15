/* =========================================================================
 * Claude-OS Shell — le tiroir du bord droit
 *
 * CE QUI S'OUVRAIT AU CLIC S'OUVRE MAINTENANT AU GESTE.
 *
 * La Console etait un popover accroche a la barre d'etat : on visait une
 * pilule de deux lignes, en bas a droite, et elle se depliait. La barre a
 * disparu au profit du coin, qui ne recoit plus ni clic ni survol -- il
 * fallait donc une autre porte, et elle ne pouvait plus etre une cible de
 * quelques pixels.
 *
 * DEUX VOLETS, EMPILES, ET C'EST UNE PLACE RESERVEE AUTANT QU'UNE MISE EN
 * PAGE. Celui du bas porte la Console. Celui du haut attend les widgets a
 * venir : il est vide, et il le dit. Un tiroir a un seul volet aurait
 * demande d'etre redessine le jour ou le second arrive ; celui-ci n'aura
 * qu'a se remplir.
 *
 * DEUX FACONS DE L'OUVRIR, ET ELLES NE SE VALENT PAS.
 *
 *   - AU DOIGT : un glisser depuis le bord droit vers la gauche. C'est le
 *     geste de tous les tiroirs lateraux, celui qu'on essaie sans qu'on
 *     vous l'explique.
 *
 *   - AU POINTEUR : le curseur POSE contre le bord droit, et tenu la une
 *     seconde. Pas un clic, pas une entree : une ATTENTE. Le bord droit de
 *     l'ecran est l'endroit ou finit tout mouvement de souris un peu vif,
 *     et un tiroir qui s'ouvrirait a l'instant du contact s'ouvrirait
 *     surtout par accident. La seconde est le prix a payer pour que le
 *     geste soit toujours volontaire -- demande explicitement, et pour
 *     cette raison.
 *
 * TOUT CLIC AILLEURS LE REFERME. Une nappe transparente couvre l'ecran tant
 * que le tiroir est ouvert et recueille ce clic -- exactement le mecanisme
 * du dock rappele par-dessus une application, et pour la meme raison :
 * labwc ne signale rien quand on revient a la fenetre deja active.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

/* Cree la bande du bord et le tiroir, referme. `console` est le contenu a
 * poser dans le volet du bas -- celui de panel_new(). */
void shell_tiroir_init (GtkApplication *app, GtkWidget *console,
                        gboolean apercu);

/* Ouvre, ferme, bascule. `shell_tiroir_fermer` sert de ConsoleFermer a la
 * rangee d'alimentation : elle ferme le tiroir avant d'eteindre. */
void shell_tiroir_ouvrir  (void);
void shell_tiroir_fermer  (gpointer inutilise);
void shell_tiroir_basculer (void);

/* Le tiroir est-il deploye ? */
gboolean shell_tiroir_ouvert (void);
