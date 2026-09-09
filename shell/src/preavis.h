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
 * Elle ne coute rien quand elle ne sert pas : la fenetre n'est construite
 * qu'au premier affichage, et la minuterie ne tourne que pendant le
 * decompte -- quelques secondes par mise en veille, jamais en continu.
 * ========================================================================= */
#pragma once

#include <glib.h>

/* Affiche le decompte et le fait courir depuis `secondes`. Rappelable :
 * un second appel repart de la valeur donnee. */
void shell_preavis_montrer (int secondes);

/* Masque immediatement et arrete la minuterie. Sans effet si rien n'est
 * affiche. A appeler aussi bien quand l'utilisateur revient que lorsque
 * l'attenuation a lieu -- dans les deux cas le decompte n'a plus d'objet. */
void shell_preavis_cacher (void);
