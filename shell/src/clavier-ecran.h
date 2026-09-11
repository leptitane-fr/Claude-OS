/* =========================================================================
 * Claude OS — le clavier à l'écran de la session, en mode tablette.
 *
 * Écran retourné, libinput écarte le clavier physique (mesuré, docs/12) :
 * ce clavier-ci est alors le seul moyen d'écrire. Il vit dans claude-os-dock,
 * comme la détection du mode tablette dont il dépend.
 *
 * QUAND IL EST À L'ÉCRAN
 *
 *   - en mode tablette seulement ;
 *   - quand un champ de texte prend le focus (input-method-v2, voir
 *     saisie.h), et jusqu'à ce qu'il le perde ;
 *   - ou sur demande : l'icône du dock en mode tablette, ou l'action
 *     « clavier » — pour les applications qui ne signalent pas leurs
 *     champs ;
 *   - la touche ⌄ le renvoie. Il ne revient seul qu'au champ suivant : un
 *     clavier qui réapparaît aussitôt renvoyé ne se laisse pas congédier.
 *
 * Il prend le bas de l'écran en ZONE RÉSERVÉE : les fenêtres agrandies
 * raccourcissent au lieu d'être recouvertes, et le champ où l'on écrit
 * reste visible. Il ne prend JAMAIS le focus clavier.
 *
 * LA DISPOSITION — choisie par l'utilisateur le 11 septembre 2026
 *
 * AZERTY, comme le clavier physique. Une rangée d'accents fixe en tête —
 * é è à ç ù ê â î ô û — plutôt qu'un appui long : tout est visible en un
 * appui. Les chiffres passent donc dans la couche « ?123 ». Maj vaut pour
 * UNE lettre ; un double appui la verrouille.
 *
 * POURQUOI PAS clavier.c
 *
 * L'écran de connexion a ses claviers (clavier.c) : ils écrivent dans un
 * GtkEditable de leur propre fenêtre, et leur Maj TIENT — pensée pour un
 * mot de passe. Celui-ci écrit dans les autres applications, et sa Maj
 * retombe — pensée pour une phrase. Les deux ne partagent que le pavé
 * numérique, repris tel quel pour les champs qui attendent des chiffres.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

/* Crée le clavier (masqué) et se branche sur le compositeur. */
void shell_clavier_ecran_init (GtkApplication *app);

/* Le mode tablette a changé : hors tablette, le clavier ne paraît jamais. */
void shell_clavier_ecran_tablette (gboolean tablette);

/* Montre ou renvoie, sur demande. */
void shell_clavier_ecran_basculer (void);

gboolean shell_clavier_ecran_visible (void);
