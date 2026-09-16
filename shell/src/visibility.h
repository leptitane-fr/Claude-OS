/* =========================================================================
 * Claude-OS Shell — quand le dock et la barre sont-ils a l'ecran ?
 *
 * La regle, demandee par l'utilisateur le 11 septembre 2026 :
 *
 *   - on travaille dans une application au premier plan : le dock et la
 *     barre sortent de l'ecran par le bas. Ils partent des qu'une fenetre
 *     est activee -- un clic pour la ramener devant, une application qui
 *     s'ouvre, un Alt-Tab ;
 *   - la touche Loupe, ou un court glisser du doigt depuis le bord bas, les
 *     fait remonter ;
 *   - une fois rappeles par-dessus une application, un clic a cote les
 *     renvoie.
 *
 * Et une regle qui s'en deduit : quand AUCUNE fenetre n'est active --
 * bureau vide, tout reduit --, le dock est le seul moyen d'aller quelque
 * part. Il est donc la, sans qu'on le demande.
 *
 * Trois etats, et non un booleen, parce que « visible » a deux sens qui
 * n'appellent pas le meme comportement :
 *
 *   CACHE     hors de l'ecran ; seule la bande du bord guette le doigt.
 *   BUREAU    a l'ecran, rien dessous a proteger : un clic sur le fond
 *             d'ecran ne doit rien renvoyer, il n'y a pas d'application a
 *             laquelle revenir.
 *   CONVOQUE  rappele par-dessus une application : le dock tend sa nappe,
 *             et le premier clic a cote le congedie.
 *   ETABLI    l'application active a publie sa barre d'outils (voir
 *             outils.h), et le dock la porte. IL NE S'EFFACE PLUS.
 *
 * -------------------------------------------------------------------------
 * POURQUOI ETABLI CONTREDIT LA REGLE DE DEPART, ET POURQUOI C'EST JUSTE
 * -------------------------------------------------------------------------
 *
 * Le 11 septembre 2026, la regle etait : une fenetre s'active, le dock s'en
 * va. Elle valait tant que le dock n'etait qu'un lanceur -- on ne lance pas
 * une application pendant qu'on travaille dedans.
 *
 * Depuis le 16 septembre, le dock porte aussi les OUTILS de l'application au
 * premier plan. Un lanceur s'efface ; une barre d'outils reste. C'est la
 * meme surface, et ce n'est plus le meme objet.
 *
 * ETABLI RESERVE SA PLACE, et c'est une rupture avec les trois autres etats.
 * Le dock a passe une semaine a ne rien reserver -- reserver faisait
 * retrecir les fenetres maximisees a chaque apparition. Mais une barre
 * d'outils qui recouvre le bas de la fenetre qu'elle sert est une nuisance,
 * et la fenetre ne se redimensionne plus a chaque va-et-vient : en ETABLI,
 * le dock ne va nulle part. Decision de l'utilisateur, 16 septembre 2026.
 *
 * LE DOCK MENE. Ce module ne vit que dans claude-os-dock, le seul des deux
 * processus qui suive les fenetres. La barre d'etat ne decide rien : elle
 * recoit « afficher » ou « masquer » sur le bus. Deux automates qui
 * observeraient chacun de son cote finiraient par se contredire -- c'etait
 * deja le risque de l'ancienne bascule, ou chacun inversait son propre etat.
 *
 * AUCUNE SCRUTATION. Tout part d'un evenement : le compositeur qui signale
 * une fenetre activee, une touche, un doigt. La seule minuterie est celle
 * du passage a « aucune fenetre active », et elle ne s'arme qu'a ce moment.
 * ========================================================================= */
#pragma once

#include <glib.h>

typedef enum {
    SHELL_VIS_CACHE,
    SHELL_VIS_BUREAU,
    SHELL_VIS_CONVOQUE,
    SHELL_VIS_ETABLI,
} ShellVisEtat;

/* Appelee a chaque changement d'etat, jamais pour rien. */
typedef void (*ShellVisibilityFunc) (ShellVisEtat etat, gpointer user_data);

/* Part de l'etat BUREAU -- a l'ouverture de session, il n'y a encore
 * aucune fenetre -- sans appeler le rappel : le composant est deja affiche. */
void shell_visibility_init (ShellVisibilityFunc cb, gpointer user_data);

/* La fenetre active a-t-elle change ? `serie` l'identifie, 0 pour aucune.
 * A appeler apres chaque lot d'evenements du compositeur ; rappeler avec la
 * meme valeur ne fait rien. */
void shell_visibility_fenetre_active (guint64 serie);

/* L'application active porte-t-elle une barre d'outils en place ?
 *
 * C'est le dock qui le sait -- il tient le registre de qui s'est presente
 * (outils.h) -- et l'automate qui en tire la consequence. A appeler apres
 * chaque changement de fenetre active, et quand une barre est posee ou
 * retiree. Rappeler avec la meme valeur ne fait rien. */
void shell_visibility_etabli (gboolean tenu);

/* La touche Loupe : ce qui est a l'ecran s'en va, ce qui n'y est pas vient. */
void shell_visibility_basculer (void);

/* Le glisser depuis le bord : ne fait que rappeler, jamais renvoyer. */
void shell_visibility_convoquer (void);

/* Un clic a cote, ou une fenetre ramenee depuis le dock : renvoie ce qui a
 * ete rappele par-dessus une application. Sans effet dans les autres etats
 * -- au bureau, il n'y a rien a quoi revenir. */
void shell_visibility_congedier (void);

/* Renvoie, quel que soit l'etat. Pour les scripts et le banc d'essai. */
void shell_visibility_cacher (void);

ShellVisEtat shell_visibility_etat (void);
