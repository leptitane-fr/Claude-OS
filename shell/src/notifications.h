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

/* La cloche, a poser dans la barre a GAUCHE de la pilule.
 *
 * PLUS PERSONNE NE LA POSE depuis que la barre a laisse place au coin. Le
 * coin porte son propre temoin, qu'il ne faut PAS confondre avec celle-ci :
 * ce temoin ne s'ouvre pas. La fonction reste pour le banc d'essai visuel
 * et pour le jour ou un widget du tiroir redonnera une entree au centre. */
GtkWidget *notifs_cloche (Notifs *n);

/* Previent a chaque changement de « il reste du non-lu ».
 *
 * C'est par la que le coin allume son temoin. La cloche de ce fichier porte
 * la meme information sous forme de classe CSS, mais elle vit dans un
 * widget que plus rien n'affiche : un rappel, et le coin n'a pas a
 * connaitre la structure interne du centre. */
typedef void (*NotifsNonLuFunc) (gboolean il_y_en_a, gpointer data);
void notifs_sur_non_lu (Notifs *n, NotifsNonLuFunc f, gpointer data);

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
 * et l'inverse aussi. Le centre reste donc sans saisie, et une surface
 * plein ecran, presque transparente, recueille le clic exterieur le temps
 * qu'il est ouvert.
 *
 * `fenetre` est celle de la barre d'etat : la nappe laisse son coin dehors,
 * pour que la pilule et la cloche restent cliquables a travers elle. La
 * barre elle-meme n'est JAMAIS etiree -- voir notifications.c, « La nappe »,
 * pour ce que cela a coute. */
void notifs_nappe (Notifs *n, GtkWidget *fenetre);

/* LA BARRE PEUT ETRE HORS DE L'ECRAN.
 *
 * Depuis le 11 septembre 2026, la barre sort de l'ecran des qu'on travaille
 * dans une application. Or la banniere, le centre et la Console sont
 * accroches a elle : barre partie, une notification arriverait sans que
 * rien ne s'affiche.
 *
 * La barre s'inscrit donc ici. Le rappel est appele chaque fois que le
 * centre a besoin d'elle ou n'en a plus besoin -- banniere a montrer,
 * banniere effacee, centre ouvert ou ferme --, et rend TRUE si elle est deja
 * a sa place. Sinon la banniere attend : la barre remonte, puis appelle
 * notifs_barre_en_place(). Une banniere accrochee a une barre encore en
 * mouvement serait placee une fois pour toutes la ou la barre se trouvait. */
typedef gboolean (*NotifsBarreFunc) (gpointer user_data);
void notifs_suivre_barre (Notifs *n, NotifsBarreFunc f, gpointer user_data);
void notifs_barre_en_place (Notifs *n);

/* Le centre retient-il la barre a l'ecran : banniere affichee ou en
 * attente, ou centre ouvert ? */
gboolean notifs_occupe (Notifs *n);

/* Ferme le centre s'il est ouvert. La banniere, elle, va a son terme. */
void notifs_fermer_centre (Notifs *n);
