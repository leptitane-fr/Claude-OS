#!/usr/bin/env bash
#
# Claude OS — Relevé des touches du clavier Chromebook
#
# POURQUOI
# La rangée supérieure d'un Chromebook n'a pas de touches F1–F12 : elle porte
# des fonctions (retour, actualiser, plein écran, aperçu, luminosité, volume)
# et une touche Loupe à la place du verrouillage majuscules. Sous ChromeOS
# c'est le système qui les interprète. Sous Linux avec un firmware UEFI, ce
# qu'elles émettent réellement dépend du noyau et de la carte : on ne peut pas
# le deviner, il faut le mesurer.
#
# Ce script écoute le clavier et affiche le nom de chaque touche pressée, une
# seule fois, dans l'ordre. Il produit ensuite les lignes de configuration
# labwc correspondantes.
#
# SOUS WAYLAND, ET NON PLUS SOUS X11
# La session ne comporte plus de serveur X : « xev » n'a rien à écouter. Son
# équivalent Wayland est « wev », qui reçoit les événements du compositeur par
# le protocole standard.
#
# USAGE
#   bash probe-keys.sh [fichier-de-sortie]
#
# Une petite fenêtre s'ouvre : elle doit garder le focus pendant la saisie.
# Presser les touches de la rangée du haut, de gauche à droite, puis la touche
# Loupe. Terminer par Ctrl+C.

set -u

command -v wev >/dev/null 2>&1 || {
	echo "wev est absent. L'installer :  sudo apt install wev" >&2
	exit 1
}
[ -n "${WAYLAND_DISPLAY:-}" ] || {
	echo "Aucune session Wayland détectée (WAYLAND_DISPLAY est vide)." >&2
	echo "Ce relevé doit se faire depuis la session Claude OS, pas depuis un TTY." >&2
	exit 1
}

OUT="${1:-touches-relevees.txt}"
: > "$OUT"

cat <<'INTRO'

  Relevé des touches — Claude OS
  ─────────────────────────────────────────────────────────────────
  Une petite fenêtre « wev » va s'ouvrir. Elle doit RESTER AU PREMIER
  PLAN pendant toute la saisie, sinon les touches ne sont pas captées.

  Presser, une par une :
     1. la rangée supérieure, de gauche à droite
     2. la touche Loupe (à la place du verrouillage majuscules)

  Puis revenir ici et faire Ctrl+C.

  Attention : la touche Loupe est déjà câblée sur la bascule du dock. Si
  la pression la déclenche au lieu d'atteindre wev, c'est déjà la réponse
  — et il faudra la relever depuis un autre compositeur.

INTRO
printf '  Démarrage dans 3 secondes'
for _ in 1 2 3; do printf '.'; sleep 1; done
echo; echo

# wev écrit une ligne par événement, du type :
#   [14:  wl_keyboard] key: serial: 42; time: 1234; key: 59; state: 1 (pressed)
#   sym: XF86MonBrightnessUp (269025027), utf8: ''
#
# On ne garde que les appuis — sans quoi chaque touche apparaîtrait deux fois —
# et on dédoublonne au fil de l'eau. Le nom du symbole est ce qui se met dans
# rc.xml ; le code du noyau est noté à côté, il sert quand aucun symbole n'est
# associé à la touche.
seen=""
wev -f wl_keyboard 2>/dev/null \
  | grep --line-buffered -E 'state: 1 \(pressed\)|^ *sym:' \
  | while IFS= read -r ligne; do
        case "$ligne" in
            *"state: 1 (pressed)"*)
                code="$(printf '%s' "$ligne" | sed -n 's/.*key: \([0-9]*\);.*/\1/p')"
                continue ;;
        esac

        sym="$(printf '%s' "$ligne" | sed -n 's/^ *sym: \([A-Za-z0-9_+]*\).*/\1/p')"
        [ -n "$sym" ] || continue
        case " $seen " in *" $sym "*) continue ;; esac

        seen="$seen $sym"
        n=$(( $(wc -l < "$OUT") + 1 ))
        printf '  %2d.  %-28s (code noyau %s)\n' "$n" "$sym" "${code:-?}"
        printf '%s\tcode=%s\n' "$sym" "${code:-?}" >> "$OUT"
    done

echo
echo "  Relevé écrit dans : $OUT"
echo "  Le transmettre tel quel : les liaisons labwc en découleront, sous la"
echo "  forme  <keybind key=\"XF86...\"><action name=\"Execute\" .../></keybind>"
