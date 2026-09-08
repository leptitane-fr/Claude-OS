/* =========================================================================
 * Claude OS — le centre de notifications
 *
 * Cette machine n'en avait aucun. Ni dunst, ni mako, ni notification-daemon :
 * verifie le 8 septembre 2026, « NameHasOwner org.freedesktop.Notifications »
 * repondait false et aucun fichier de service n'en declarait un. Chromium et
 * Claude Desktop emettaient donc dans le vide, sans la moindre erreur
 * visible — une notification perdue ne se plaint pas.
 *
 * CE FICHIER EST LE SERVEUR, PAS SEULEMENT L'AFFICHAGE. Il prend le nom
 * org.freedesktop.Notifications sur le bus de session et implemente les
 * quatre methodes de la specification freedesktop, plus ses deux signaux.
 * Toute application capable de notifier passe par la : les deux d'aujourd'hui
 * comme celles de demain, sans rien a configurer de leur cote.
 *
 * POURQUOI DANS LA BARRE D'ETAT, ET PAS DANS UN PROCESSUS A PART
 *
 * La cloche vit dans la barre, le centre s'aligne sur la Console, et
 * l'arrivee d'une notification doit repositionner l'un par rapport a
 * l'autre. Un processus separe aurait demande un protocole entre les deux
 * pour transporter une hauteur en pixels — beaucoup de machinerie pour un
 * entier. Le prix a payer est assume : si la barre d'etat tombe, les
 * notifications tombent avec elle.
 *
 * L'ALIGNEMENT EST HERITE, PAS RECALCULE
 *
 * Le centre et la banniere sont des GtkPopover accroches au MEME bouton que
 * la Console. Ils reprennent donc son bord droit et sa largeur sans qu'on
 * ait a mesurer quoi que ce soit — un alignement calcule aurait derive au
 * premier changement de marge dans la feuille de style. Seule la position
 * VERTICALE est calculee, parce qu'elle depend de la hauteur de la Console,
 * qui change quand on la deplie.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

typedef struct _Notifs Notifs;

/* Cree le centre et prend le nom sur le bus. `apercu` remplit la liste de
 * quelques notifications fixes pour le banc d'essai visuel, et ne touche
 * pas au bus : le banc n'a pas a dependre d'un service reel, ni a se
 * disputer le nom avec la barre d'etat de la vraie session. */
Notifs *notifs_new (gboolean apercu);

/* La cloche, a poser dans la barre a GAUCHE de la pilule. */
GtkWidget *notifs_cloche (Notifs *n);

/* Accroche le centre et la banniere a l'ancre de la Console — le bouton de
 * la barre d'etat. A appeler une fois la barre construite, et avant toute
 * notification. */
void notifs_ancrer (Notifs *n, GtkWidget *ancre);

/* Branche le centre sur la hauteur de la Console. A appeler avec le popover
 * rendu par panel_new(). */
void notifs_suivre_console (Notifs *n, GtkWidget *console);

/* LA NAPPE : ce qui rend le « clic a cote » possible.
 *
 * Un popover qui se cache tout seul prend une saisie du pointeur, et deux
 * saisies ne coexistent pas : la Console se refermerait en ouvrant le centre,
 * et l'inverse aussi. Le centre reste donc sans saisie, et c'est la fenetre
 * de la barre d'etat qui, le temps que le centre soit ouvert, s'etend a tout
 * l'ecran pour recueillir le clic exterieur. Elle est transparente et ne
 * dessine rien : seule sa zone d'entree change.
 *
 * C'est la technique des environnements de bureau pour leurs panneaux, et
 * elle a l'avantage de ne rien retirer a l'empilement : la Console garde sa
 * propre saisie, les deux surfaces restent ouvertes ensemble. */
void notifs_nappe (Notifs *n, GtkWidget *fenetre);
