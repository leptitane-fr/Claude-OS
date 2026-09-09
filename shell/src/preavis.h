/* =========================================================================
 * Claude-OS Shell — le compte a rebours avant l'attenuation
 *
 * POURQUOI CE MACHIN EXISTE
 *
 * Voir l'ecran baisser au milieu d'un paragraphe est une nuisance
 * disproportionnee : on perd la ligne, on cherche le pave tactile, on
 * relit. Le remede n'est pas d'allonger les delais -- ce serait renoncer a
 * l'economie -- mais de PREVENIR. Un petit decompte percu du coin de
 * l'oeil laisse le temps d'un geste, sans quitter le texte des yeux.
 *
 * IL NE DOIT NI PRENDRE LE FOCUS NI RECEVOIR LE CLIC.
 *
 * Une surface qui capte le clavier volerait la frappe en cours ; une
 * surface qui capte le pointeur creerait un trou mort a l'ecran. La
 * fenetre declare donc un mode clavier « aucun » et une region d'entree
 * VIDE : elle se voit et ne s'attrape pas.
 *
 * UN CADRAN, PAS UN NOMBRE.
 *
 * La premiere version affichait « Veille dans 8 s ». Constate a l'usage le
 * 9 septembre 2026 : un texte appelle la LECTURE. L'oeil quitte le
 * paragraphe pour dechiffrer trois mots, ce qui est exactement
 * l'interruption qu'on voulait eviter -- le remede reproduisait la
 * maladie.
 *
 * Un disque qui se vide se lit sans se lire : on en percoit l'etat d'un
 * coup d'oeil, sans decodage, comme on percoit qu'un verre est a moitie
 * plein. C'est ce qu'on demande a un signal peripherique.
 *
 * GRAND ET TRANSPARENT, ET LES DEUX ENSEMBLE.
 *
 * Le premier cadran faisait 44 px et opaque : il se laissait ignorer trop
 * facilement -- constate a l'usage. Un cadran de la largeur de la barre
 * d'etat alerte vraiment ; la transparence lui evite d'etre intrusif pour
 * autant. L'un sans l'autre donnerait soit un signal qu'on ne voit pas,
 * soit une pastille qui occupe le coin de l'ecran.
 *
 * Elle ne coute rien quand elle ne sert pas : la fenetre n'est construite
 * qu'au premier affichage, et la minuterie ne tourne que pendant le
 * decompte -- quelques secondes par mise en veille, jamais en continu.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

/* Le cadran prend la largeur de CE widget -- en pratique la pilule de la
 * barre d'etat, cloche exclue. Mesuree a l'ecran le 9 septembre 2026 :
 * 172 px. On ne fige pas ce nombre, on interroge le widget : la pilule
 * s'elargit avec la police et avec l'heure affichee, et un cadran qui
 * cesserait de s'aligner sur elle se verrait tout de suite.
 *
 * A appeler une fois la barre construite. Sans reference, le cadran prend
 * une taille de repli. */
void shell_preavis_reference (GtkWidget *pilule);

/* Opacite du cadran, en pourcent (0-100).
 *
 * REGLABLE PLUTOT QU'ARBITRE. Le bon equilibre depend du fond d'ecran et
 * de la vue de chacun ; trois valeurs codees en dur -- disque, fond,
 * piste -- auraient demande un aller-retour a chaque ajustement. Un seul
 * nombre les commande toutes, dans un rapport fixe.
 *
 * Applique par une feuille de style engendree : les TEINTES restent celles
 * du theme, seule leur opacite est reecrite. */
void shell_preavis_opacite (int pourcent);

/* Affiche le decompte et le fait courir depuis `secondes`. Rappelable :
 * un second appel repart de la valeur donnee. */
void shell_preavis_montrer (int secondes);

/* Masque immediatement et arrete la minuterie. Sans effet si rien n'est
 * affiche. A appeler aussi bien quand l'utilisateur revient que lorsque
 * l'attenuation a lieu -- dans les deux cas le decompte n'a plus d'objet. */
void shell_preavis_cacher (void);
