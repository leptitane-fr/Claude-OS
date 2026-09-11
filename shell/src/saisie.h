/* =========================================================================
 * Claude OS — la saisie au clavier à l'écran : ce que le compositeur dit,
 * et ce qu'on lui envoie.
 *
 * DEUX PROTOCOLES, ET CHACUN POUR CE QU'IL FAIT BIEN
 *
 *   input-method-v2      Le compositeur prévient quand un champ de texte
 *                        prend le focus (activate) et le perd (deactivate),
 *                        et dit ce qu'il attend : texte, chiffres, mot de
 *                        passe. C'est ce qui permet au clavier d'apparaître
 *                        « quand c'est nécessaire », et seulement alors.
 *                        Mais il ne parle qu'aux applications qui implémentent
 *                        text-input-v3 — GTK, Qt ; Chromium sur option.
 *
 *   virtual-keyboard-v1  On FRAPPE par lui : de vrais événements clavier,
 *                        que TOUTE application comprend, terminal compris.
 *                        Envoyer le texte par input-method (commit_string)
 *                        aurait laissé muettes celles qui ignorent
 *                        text-input, et n'aurait pas su dire Entrée ni les
 *                        flèches.
 *
 * Mesuré le 11 septembre 2026 : labwc 0.8.3 expose les deux (docs/12).
 * Les XML sont ceux de wlroots 0.18.2, la version de labwc, versés dans
 * shell/protocols/ : aucun paquet Debian ne les porte.
 *
 * LA DISPOSITION EST FABRIQUÉE, PAS EMPRUNTÉE
 *
 * Le clavier virtuel envoie des CODES de touches, que le client traduit
 * avec la disposition XKB qu'on lui a transmise. Emprunter la disposition
 * « fr » obligerait à rejouer une touche morte pour « ê », et ne contiendrait
 * pas « … » ni « ≠ ». On fabrique donc une disposition qui contient
 * exactement les caractères du clavier à l'écran, chacun à une place fixe :
 * une position de touche ordinaire et un niveau (aucun, Maj, AltGr,
 * Maj+AltGr). Les touches de commande — ⌫, ↵, flèches — portent leur vrai
 * code. Voir saisie.c : les CODES comptent pour Chromium, et l'ignorer a
 * fait sauter le curseur au lieu d'écrire « ; ». Le clavier physique garde
 * sa disposition ; en mode tablette il est de toute façon écarté par
 * libinput.
 * ========================================================================= */
#pragma once

#include <glib.h>

/* Ce que text-input-v3 appelle « purpose » : ce que le champ attend. */
typedef enum {
    SHELL_SAISIE_TEXTE,
    SHELL_SAISIE_CHIFFRES,     /* digits, number, pin, phone */
    SHELL_SAISIE_MOT_DE_PASSE,
} ShellSaisieBut;

/* Un champ de texte vient de prendre (actif) ou de perdre le focus. */
typedef void (*ShellSaisieFunc) (gboolean actif, ShellSaisieBut but,
                                 gpointer user_data);

/* Les touches qui ne sont pas du texte. */
typedef enum {
    SHELL_TOUCHE_EFFACER,
    SHELL_TOUCHE_ENTREE,
    SHELL_TOUCHE_GAUCHE,
    SHELL_TOUCHE_DROITE,
    SHELL_TOUCHE_TABULATION,
    SHELL_TOUCHE_N
} ShellTouche;

/* Se branche sur la connexion Wayland de GTK. `textes` : tous les
 * caractères que le clavier à l'écran peut produire, chacun une chaîne
 * UTF-8 d'un caractère, tableau terminé par NULL. La disposition est
 * construite une fois, avec eux.
 *
 * Rend FALSE si le clavier virtuel est indisponible : le clavier à l'écran
 * ne pourrait rien écrire, et le dit dans le journal. Sans input-method, il
 * écrit encore, mais n'apparaît plus seul. */
gboolean shell_saisie_init (const char *const *textes,
                            ShellSaisieFunc cb, gpointer user_data);

/* Frappe un caractère (appui puis relâche). Un caractère absent de la
 * disposition l'y ajoute — au prix d'une disposition renvoyée — et le
 * journal le dit, pour qu'on l'ajoute à la liste. */
void shell_saisie_texte (const char *caractere);

/* Frappe une touche de commande ; `maj` la frappe avec Maj enfoncée. */
void shell_saisie_touche (ShellTouche t, gboolean maj);
