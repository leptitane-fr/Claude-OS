/* =========================================================================
 * Claude-OS Shell — les avis systeme
 *
 * UNE SEULE SURFACE, AU MEME ENDROIT, POUR TOUT CE QUE LE SYSTEME A A DIRE
 * EN PASSANT.
 *
 * Ce module n'affichait qu'une chose : le compte a rebours avant que
 * l'ecran ne baisse. Il en affiche desormais deux -- ce cadran, et de
 * courts messages comme « En charge » ou « Batterie faible » -- et il en
 * affichera d'autres.
 *
 * C'EST LE LIEU QUI FAIT L'AVIS, pas le module qui l'emet. Un signal
 * peripherique ne vaut que si l'oeil sait d'avance ou le trouver : deux
 * coins d'ecran differents pour deux messages du meme genre obligeraient a
 * chercher, ce qui est exactement le contraire du service rendu. Tout ce
 * qui passe par ici partage donc la meme place, la meme couleur, la meme
 * opacite, et la meme absence de prise.
 *
 * CE N'EST PAS LE CENTRE DE NOTIFICATIONS, ET LES DEUX NE SE REMPLACENT
 * PAS. Un avis est FUGACE et ne laisse aucune trace : on le voit ou on ne
 * le voit pas. Ce qui doit se retrouver plus tard passe par
 * org.freedesktop.Notifications, la cloche et l'historique -- batterie.c
 * fait les deux, et c'est delibere : l'avis est l'echo immediat, la
 * notification est l'archive.
 *
 * IL NE DOIT NI PRENDRE LE FOCUS NI RECEVOIR LE CLIC.
 *
 * Une surface qui capte le clavier volerait la frappe en cours ; une
 * surface qui capte le pointeur creerait un trou mort a l'ecran. La
 * fenetre declare donc un mode clavier « aucun » et une region d'entree
 * VIDE : elle se voit et ne s'attrape pas. Ni clic, ni survol, ni curseur
 * qui change de forme en passant dessus.
 *
 * AU CENTRE, AU-DESSUS DU DOCK, ET A UNE PLACE FIXE.
 *
 * Le cadran vivait en bas a droite, cale sur la pilule de la barre d'etat
 * dont il reprenait la largeur au pixel pres. C'etait juste tant qu'il
 * n'annoncait qu'une chose, et que cette chose concernait la barre. Un
 * avis qui parle de la machine entiere appartient a l'axe du regard, pas a
 * un coin -- et un message de deux mots n'a aucune raison de mesurer la
 * largeur d'une horloge. Le diametre est donc fixe, la position aussi, et
 * le module n'a plus besoin qu'on lui designe un widget de reference.
 *
 * UN CADRAN, PAS UN NOMBRE -- pour le compte a rebours.
 *
 * La premiere version affichait « Veille dans 8 s ». Constate a l'usage le
 * 9 septembre 2026 : un texte appelle la LECTURE. L'oeil quitte le
 * paragraphe pour dechiffrer trois mots, ce qui est exactement
 * l'interruption qu'on voulait eviter -- le remede reproduisait la
 * maladie. Un disque qui se vide se lit sans se lire.
 *
 * Les messages, eux, sont du texte parce qu'ils n'ont pas d'echelle a
 * montrer : « En charge » n'a pas de fraction. Ils restent tres courts
 * pour la meme raison qui avait fait retirer « Veille dans 8 s » -- deux
 * mots se percoivent, une phrase se lit.
 *
 * Elle ne coute rien quand elle ne sert pas : la fenetre n'est construite
 * qu'au premier affichage, et la minuterie ne tourne que pendant le
 * decompte -- quelques secondes par mise en veille, jamais en continu.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

/* Opacite des avis, en pourcent (0-100).
 *
 * REGLABLE PLUTOT QU'ARBITREE. Le bon equilibre depend du fond d'ecran et
 * de la vue de chacun ; des valeurs codees en dur auraient demande un
 * aller-retour a chaque ajustement. Elle vaut pour le cadran comme pour
 * les messages -- « la meme configuration » est ce qui fait qu'on les
 * reconnait comme une seule voix.
 *
 * Appliquee par une feuille de style engendree : seule l'opacite est
 * reecrite, la teinte reste celle du jeton @avis. */
void shell_avis_opacite (int pourcent);

/* Affiche le compte a rebours et le fait courir depuis `secondes`.
 * Rappelable : un second appel repart de la valeur donnee. */
void shell_avis_cadran (int secondes);

/* Affiche un message pendant `secondes`, puis le retire tout seul.
 *
 * DEUX MOTS, TROIS AU PLUS. Voir plus haut : ce qui demande a etre lu n'a
 * pas sa place ici, et doit partir en notification.
 *
 * UNE ICONE DEVANT, ET ELLE N'EST PAS UN ORNEMENT. C'est elle qu'on
 * reconnait de loin, avant meme d'avoir lu : « En charge » et « Sur
 * batterie » se distinguent d'un coup d'oeil par la fiche ou la pile,
 * jamais par la longueur du mot. `icone` est un nom du theme d'icones ;
 * les noms en « -symbolic » sont recolores par GTK et prennent donc
 * exactement la couleur et l'opacite du texte. NULL pour n'avoir que le
 * texte.
 *
 * SANS EFFET PENDANT UN COMPTE A REBOURS, et c'est voulu : les deux se
 * disputeraient la meme surface, et le decompte est le seul des deux qui
 * ait une echeance. Rien n'est perdu pour autant -- ce qui emet un message
 * ici emet aussi une notification, qui elle demeure. */
void shell_avis_message (const char *icone, const char *texte, int secondes);

/* Masque immediatement, cadran ou message, et arrete les minuteries. Sans
 * effet si rien n'est affiche. A appeler aussi bien quand l'utilisateur
 * revient que lorsque l'attenuation a lieu -- dans les deux cas le
 * decompte n'a plus d'objet. */
void shell_avis_cacher (void);
