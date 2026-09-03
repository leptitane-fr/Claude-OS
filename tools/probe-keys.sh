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
# Openbox correspondantes.
#
# USAGE
#   bash probe-keys.sh
#
# Une petite fenêtre s'ouvre : elle doit garder le focus pendant la saisie.
# Presser les touches de la rangée du haut, de gauche à droite, puis la touche
# Loupe. Terminer par Ctrl+C.

set -u

command -v xev >/dev/null 2>&1 || {
	echo "xev est absent. Installer le paquet « x11-utils »." >&2
	exit 1
}
[ -n "${DISPLAY:-}" ] || {
	echo "Aucune session graphique détectée (DISPLAY vide)." >&2
	echo "Ce relevé doit se faire depuis la session Claude OS, pas depuis un TTY." >&2
	exit 1
}

OUT="${1:-touches-relevees.txt}"
: > "$OUT"

cat <<'INTRO'

  Relevé des touches — Claude OS
  ─────────────────────────────────────────────────────────────────
  Une fenêtre « Event Tester » va s'ouvrir. Elle doit RESTER AU PREMIER
  PLAN pendant toute la saisie, sinon les touches ne sont pas captées.

  Presser, une par une :
     1. la rangée supérieure, de gauche à droite
     2. la touche Loupe (à la place du verrouillage majuscules)

  Puis revenir ici et faire Ctrl+C.

INTRO
printf '  Démarrage dans 3 secondes'
for _ in 1 2 3; do printf '.'; sleep 1; done
echo; echo

# xev n'émet le nom du symbole qu'à l'appui. On filtre les relâchements pour
# ne pas afficher chaque touche deux fois, et on dédoublonne au fil de l'eau.
seen=""
xev -event keyboard 2>/dev/null \
  | grep --line-buffered -B2 'KeyPress' -A3 2>/dev/null \
  | grep --line-buffered -o 'keysym 0x[0-9a-f]*, [A-Za-z0-9_]*' \
  | sed -u 's/keysym 0x[0-9a-f]*, //' \
  | while IFS= read -r sym; do
        case " $seen " in *" $sym "*) continue ;; esac
        seen="$seen $sym"
        n=$(( $(wc -l < "$OUT") + 1 ))
        printf '  %2d.  %s\n' "$n" "$sym"
        printf '%s\n' "$sym" >> "$OUT"
    done

echo
echo "  Relevé écrit dans : $OUT"
echo "  Le transmettre tel quel : les liaisons Openbox en découleront."
