#!/usr/bin/env bash
#
# Claude OS — Banc des tiroirs AU DOIGT, sur la machine
#
# Le banc sans écran (banc-tiroirs.sh) éprouve le pointeur ; il ne peut rien
# dire du doigt, un labwc headless n'ayant aucun périphérique d'entrée. Celui-
# ci joue les mêmes gestes avec un écran tactile fabriqué par uinput
# (essais/doigt.py) : les contacts traversent libinput et le touch.c de labwc
# comme ceux de la dalle, sur la SESSION EN COURS.
#
# Ce qu'il éprouve, et pourquoi chaque cas existe :
#
#   - les glissés à x = 0, 32, 37 et 47 : la dalle de MADOO ne rapporte RIEN
#     en deçà de 32 px à gauche (mesuré, essais/sonde-contacts.py), et les
#     gestes réels s'y posent entre 32 et 37. La lisière gauche fait donc
#     48 px là où celle de droite, où le contact se pose au dernier pixel,
#     garde 24 ;
#   - le glissé hors lisière, à rebours, et la tape sans trajet : ce qui ne
#     doit RIEN ouvrir ;
#   - le FANTÔME suivi du vrai glissé : un contact fugace naît sur la lisière
#     juste avant l'index — la main qui entre par le bord frôle le châssis.
#     GtkGestureDrag, qui ne suit qu'une suite de contacts, donnait le geste
#     au fantôme et ignorait l'index : un coup sur deux, sans rien qui le
#     distingue pour celui qui le fait. C'est le cas qui a motivé le suivi
#     brut des contacts dans tiroir.c ; il ÉCHOUE sur toute version
#     antérieure au 15 septembre 2026 au soir.
#
# Usage :  bash shell/essais/banc-doigt.sh [binaire-barre]
#          (par défaut /usr/bin/claude-os-status — donc la version installée)
#
# IL FAUT root : uinput appartient à root. Il relance la barre d'état en mode
# bavard pour lire ses transitions, et la remet comme elle était en sortant.
#
# Code de retour : le nombre de cas en échec.

set -uo pipefail

ICI="$(cd "$(dirname "$0")" && pwd)"
BARRE="${1:-/usr/bin/claude-os-status}"
POINTEUR="$ICI/build/pointeur"
JOURNAL="${XDG_STATE_HOME:-$HOME/.local/state}/claude-os/banc-doigt.log"

[ -x "$BARRE" ]    || { echo "barre introuvable : $BARRE" >&2; exit 1; }
[ -x "$POINTEUR" ] || { echo "compiler d'abord : bash $ICI/construire.sh pointeur" >&2; exit 1; }
command -v claude-os-root >/dev/null || { echo "claude-os-root est absent" >&2; exit 1; }

doigt() { claude-os-root python3 "$ICI/doigt.py" "$@" >/dev/null 2>&1; }

# LE MOTIF DE FIN DE LIGNE EST INDISPENSABLE. « pkill -f claude-os-status »
# tue aussi le shell qui porte ces mots dans sa ligne de commande — payé deux
# fois le 15 septembre 2026, le banc s'arrêtant lui-même en plein essai.
tuer_barre() {
	for p in $(ps -eo pid,args | awk '$2 ~ /claude-os-status$/ {print $1}'); do
		kill "$p"
	done
	sleep 1.5
}

relancer_bavarde() {
	tuer_barre
	: > "$JOURNAL"
	( G_MESSAGES_DEBUG=all setsid "$BARRE" >> "$JOURNAL" 2>&1 < /dev/null & )
	sleep 3
}

# Refermer se fait par un clic DANS la nappe, jamais sur le fond d'écran : un
# clic sur le bureau ouvre le menu racine de labwc, qui capte tout ce qui
# suit. Piège documenté en tête de pointeur.c, et tombé dedans quand même.
fermer() { "$POINTEUR" clic 950 520 >/dev/null 2>&1; sleep 0.7; }

ECHECS=0
cas() {   # $1 libellé, $2 côté, $3 attendu (ouvre|rien), puis la commande de doigt.py
	local lib="$1" cote="$2" attendu="$3"; shift 3
	fermer
	local avant; avant="$(grep -ac "tiroir $cote : ouvert" "$JOURNAL")"
	doigt "$@"
	sleep 1
	local apres; apres="$(grep -ac "tiroir $cote : ouvert" "$JOURNAL")"
	local vu=rien; [ "$apres" -gt "$avant" ] && vu=ouvre
	if [ "$vu" = "$attendu" ]; then
		printf '  ok    %-46s %s\n' "$lib" "$vu"
	else
		printf '  ÉCHEC %-46s attendu %s, vu %s\n' "$lib" "$attendu" "$vu"
		ECHECS=$((ECHECS + 1))
	fi
}

claude-os-root modprobe uinput || { echo "uinput indisponible" >&2; exit 1; }
relancer_bavarde

echo "Gestes au doigt — $BARRE"
# LES CHIFFRES DE GAUCHE SONT CEUX DE LA DALLE, PAS DES VALEURS RONDES. Le
# Goodix de MADOO ne rapporte jamais un contact en deçà de x = 32 : les quatre
# glissés mesurés le 15 septembre 2026 se sont posés à 32, 32, 37 et 37. Le
# cas « x=37 » est donc le geste réel de l'utilisateur, et c'est celui qui
# échouait quand la lisière faisait 24 px.
cas "glissé depuis le cadre (x=0)"        gauche ouvre glisse 0 600 210 600 260
cas "glissé au plancher de la dalle (32)" gauche ouvre glisse 32 600 240 600 260
cas "glissé comme mesuré (x=37)"          gauche ouvre glisse 37 600 320 600 260
cas "glissé au bout de la lisière (47)"   gauche ouvre glisse 47 600 260 600 260
cas "glissé hors lisière (x=56)"          gauche rien  glisse 56 600 270 600 260
cas "glissé à rebours (47 → 0)"           gauche rien  glisse 47 600 0 600 260
cas "tape sans trajet"                    gauche rien  touche 5 600
cas "fantôme puis glissé"                 gauche ouvre fantome 37 600 320 600 5 380
cas "bord droit, vers l'intérieur"        droite ouvre glisse 1914 600 1700 600 260
cas "bord droit, hors lisière (1890)"     droite rien  glisse 1890 600 1700 600 260
cas "bord droit, fantôme puis glissé"     droite ouvre fantome 1914 600 1700 600 1918 380
fermer

# La session retrouve sa barre ordinaire, et le module s'en va comme il est
# venu : un banc ne laisse rien derrière lui.
tuer_barre
( setsid /usr/bin/claude-os-status >> "${XDG_STATE_HOME:-$HOME/.local/state}/claude-os/shell.log" 2>&1 < /dev/null & )
claude-os-root modprobe -r uinput || true

echo
echo "11 cas, $ECHECS en échec. Journal : $JOURNAL"
exit "$ECHECS"
