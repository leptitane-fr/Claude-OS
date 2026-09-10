#!/usr/bin/env bash
#
# Claude OS — Banc du lecteur vidéo, sans écran
#
# Lance un labwc sans écran et le lecteur dedans. Sert à éprouver la logique
# — battement, synchronisation, saut, pause, chemin sans copie — quand
# l'écran de la machine n'est pas disponible.
#
# ET IL Y A DEUX FAÇONS DE NE PAS L'AVOIR, toutes deux rencontrées le
# 10 septembre 2026, toutes deux muettes :
#
#   1. La veille progressive a éteint le rétroéclairage. Plus rien n'est
#      composé, donc plus un « frame callback », donc le lecteur s'arrête —
#      correctement, mais on croit à une panne.
#   2. Le verrou de session est monté. Sous ext-session-lock-v1 le
#      compositeur masque TOUTES les fenêtres : même symptôme, même
#      diagnostic erroné.
#
# Dans les deux cas une mesure de consommation faite à ce moment-là ne mesure
# rien de ce qu'on croit — voir docs/11 §11.4. Ce banc, lui, a son propre
# compositeur et sa propre sortie : il bat toujours.
#
# Usage :
#   bash shell/essais/banc-video.sh mire.mp4 [secondes]
#   bash shell/essais/banc-video.sh mire.mp4 10 --sonde     # la sonde plutôt
#   bash shell/essais/banc-video.sh mire.mp4 10 --capture=/tmp/vu.png
#
# Le journal complet reste dans /tmp/claude-os-banc-video.log

set -uo pipefail

ICI="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ICI/build"
FICHIER="${1:?fichier vidéo à lire}"
SECONDES="${2:-10}"
shift 2 2>/dev/null || shift $#
SONDE=""; SCENARIO=""; CAPTURE=""; REVELE=""; SOUSTITRES=""
for arg in "$@"; do
	[ "$arg" = "--sonde" ] && SONDE=1
	[ "$arg" = "--scenario" ] && SCENARIO=" --scenario"
	[ "$arg" = "--revele" ] && REVELE=" --revele"
	[ "$arg" = "--sous-titres" ] && SOUSTITRES=" --sous-titres"
	case "$arg" in --capture=*) CAPTURE="${arg#--capture=}" ;; esac
done

[ -x "$BUILD/claude-os-video" ] || { echo "Compiler d'abord : bash $ICI/construire.sh" >&2; exit 1; }
[ -f "$FICHIER" ] || { echo "Fichier introuvable : $FICHIER" >&2; exit 1; }

JOURNAL=/tmp/claude-os-banc-video.log
CONF=$(mktemp -d)
mkdir -p "$CONF/labwc"
: > "$CONF/labwc/autostart"      # AUCUN autostart : sinon le dock, la barre
                                 # et le fond démarrent dans le banc.
trap 'rm -rf "$CONF"' EXIT

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_HEADLESS_OUTPUTS=1
unset WAYLAND_DISPLAY            # sinon labwc devient client de la session

# LE SUJET PASSE PAR UN SCRIPT, ET PAS PAR UNE LIGNE CITEE.
#
# « labwc -s » exécute la chaîne reçue par un shell ; l'y écrire directement
# demandait trois niveaux de guillemets pour porter une redirection, et le
# premier essai s'est perdu en silence — le journal n'était pas créé, et le
# banc annonçait « 0 image » comme s'il s'agissait du lecteur.
LANCEUR="$CONF/lancer.sh"
{
	echo '#!/bin/sh'
	echo "exec > '$JOURNAL' 2>&1"
	echo 'export GDK_DEBUG=offload'
	# LA CAPTURE : grim, dans le compositeur imbrique, apres que le lecteur
	# a eu le temps d'afficher. C'est le seul moyen de VOIR l'interface
	# quand l'ecran de la machine est eteint ou verrouille.
	if [ -n "$CAPTURE" ]; then
		echo "( sleep 4; grim '$CAPTURE' ) &"
	fi
	if [ -n "$SONDE" ]; then
		echo "exec '$BUILD/sonde-offload' '$FICHIER' --mode=offload --duree=$SECONDES"
	else
		echo "exec '$BUILD/claude-os-video' '$FICHIER' --essai=$SECONDES$SCENARIO$REVELE$SOUSTITRES"
	fi
} > "$LANCEUR"
chmod +x "$LANCEUR"

echo "Banc sans écran : $SECONDES s sur $(basename "$FICHIER")"
rm -f "$JOURNAL"
timeout $((SECONDES + 25)) labwc -C "$CONF/labwc" -s "$LANCEUR" \
	> /tmp/claude-os-banc-labwc.log 2>&1
CODE=$?

# INVARIANT N°4 : on lit le code de retour, et on le dit.
[ "$CODE" -ne 0 ] && echo "labwc a rendu $CODE (124 = délai dépassé)" >&2

ATTACHES=$(grep -c "Attaching" "$JOURNAL" 2>/dev/null || echo 0)
echo
echo "== Résultat =="
echo "  images confiées au compositeur (sans copie) : $ATTACHES"
grep -E "^\*\* Message|WARNING|CRITICAL|erreur|échec" "$JOURNAL" \
	| grep -vE "Gdk-DEBUG" \
	| sed 's/^/  /'
echo
[ -n "$CAPTURE" ] && [ -f "$CAPTURE" ] && \
	echo "  capture : $CAPTURE ($(file -b "$CAPTURE" | cut -d, -f2 | tr -d ' '))"
echo
echo "Journal complet : $JOURNAL"
