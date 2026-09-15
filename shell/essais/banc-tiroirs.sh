#!/usr/bin/env bash
#
# Claude OS — Banc des deux tiroirs latéraux
#
# Lance un labwc sans écran, sur un bus de session jetable, avec la barre
# d'état compilée et le pointeur virtuel, puis joue les gestes qui ouvrent et
# ferment chaque tiroir : le pointeur POSÉ contre un bord, le glisser depuis
# ce bord, et le clic à côté. Le glissé au DOIGT, lui, ne s'éprouve pas ici :
# un labwc sans écran n'a aucun périphérique d'entrée. Voir essais/doigt.py,
# qui fabrique un écran tactile par uinput et s'emploie sur la machine. Chaque étape est vérifiée dans le journal de
# la barre — « tiroir gauche : ouvert », « tiroir droite : fermé » —, parce
# qu'une capture montre un volet sorti sans dire lequel des deux côtés l'a
# décidé.
#
# Ce que ce banc ne sait PAS éprouver : le doigt. Le glisser passe ici par le
# pointeur, qui emprunte le même GtkGestureDrag mais pas le même chemin dans
# labwc (touch.c). Le geste réel se juge sur la machine.
#
# Usage :
#   bash shell/essais/construire.sh pointeur
#   BUILD=/chemin/vers/build bash shell/essais/banc-tiroirs.sh [dossier-captures]
#
# BUILD : un répertoire de compilation meson du shell, configuré avec
# --prefix=/usr (les feuilles de style installées servent). Celui du dépôt,
# shell/build, appartient à root après un « --compiler » : en compiler un à
# soi, par exemple
#   meson setup /tmp/shell-banc shell --prefix=/usr && ninja -C /tmp/shell-banc
#
# Code de retour : le nombre d'étapes en échec.

set -uo pipefail

ICI="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$ICI/../build}"
SORTIE="${1:-/tmp/claude-os-banc-tiroirs}"
POINTEUR="$ICI/build/pointeur"

[ -x "$BUILD/claude-os-status" ] || { echo "Pas de barre compilée dans $BUILD" >&2; exit 1; }
[ -x "$POINTEUR" ] || { echo "Compiler d'abord : bash $ICI/construire.sh pointeur" >&2; exit 1; }
for outil in labwc grim dbus-run-session; do
	command -v "$outil" >/dev/null || { echo "$outil est absent" >&2; exit 1; }
done

# UN BUS À NOUS. Sur celui de la session, la barre du banc passerait la main
# à celle du bureau — un GtkApplication mono-instance — et le banc
# éprouverait la session en cours, pas le code compilé.
if [ -z "${BANC_TIROIRS_BUS:-}" ]; then
	export BANC_TIROIRS_BUS=1
	exec dbus-run-session -- bash "$0" "$@"
fi

mkdir -p "$SORTIE"
CONF="$(mktemp -d)"
export XDG_RUNTIME_DIR="$CONF/run"
mkdir -p "$XDG_RUNTIME_DIR" "$CONF/labwc"
chmod 700 "$XDG_RUNTIME_DIR"
: > "$CONF/labwc/autostart"      # AUCUN autostart : le shell du système
                                 # démarrerait dans le banc.
cat > "$CONF/labwc/rc.xml" <<'XML'
<?xml version="1.0"?>
<labwc_config><core><decoration>server</decoration><gap>0</gap></core>
<theme><dropShadows>no</dropShadows></theme></labwc_config>
XML

export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
export WLR_HEADLESS_OUTPUTS=1 GDK_BACKEND=wayland GSK_RENDERER=cairo
unset WAYLAND_DISPLAY DISPLAY

labwc -C "$CONF/labwc" > "$SORTIE/labwc.log" 2>&1 &
trap 'kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$CONF"' EXIT

for _ in $(seq 1 40); do
	s=$(ls "$XDG_RUNTIME_DIR"/wayland-[0-9] 2>/dev/null | head -1)
	[ -n "$s" ] && break
	sleep 0.25
done
[ -n "${s:-}" ] || { echo "labwc n'a pas démarré :" >&2; tail "$SORTIE/labwc.log" >&2; exit 1; }
export WAYLAND_DISPLAY="$(basename "$s")"
wlr-randr --output HEADLESS-1 --custom-mode 1920x1080 \
	|| echo "wlr-randr a échoué : la sortie garde sa taille par défaut" >&2

# Les transitions des tiroirs ne sont écrites qu'en débogage (g_debug).
export G_MESSAGES_DEBUG=all

command -v swaybg >/dev/null && { swaybg -c '#3c4043' > "$SORTIE/swaybg.log" 2>&1 & }
"$BUILD/claude-os-status" > "$SORTIE/status.log" 2>&1 &
sleep 2.5

ECHECS=0
N=0
# Le dernier état connu d'un côté, ou « ferme » tant qu'il n'a rien dit :
# les deux tiroirs sont créés rentrés.
etat() {
	grep -o "tiroir $1 : [a-z]*" "$SORTIE/status.log" | tail -1 | awk '{print $4}'
}
attendre() {
	# $1 : état attendu à gauche ; $2 : à droite ; $3 : libellé de l'étape
	N=$((N + 1))
	sleep 0.7
	local g d; g="$(etat gauche)"; d="$(etat droite)"
	g="${g:-ferme}"; d="${d:-ferme}"
	grim "$SORTIE/$(printf '%02d' "$N")-g-$g-d-$d.png"
	if [ "$g" = "$1" ] && [ "$d" = "$2" ]; then
		printf '  ok    %-52s gauche %s, droite %s\n' "$3" "$g" "$d"
	else
		printf '  ÉCHEC %-52s attendu %s/%s, vu %s/%s\n' "$3" "$1" "$2" "$g" "$d"
		ECHECS=$((ECHECS + 1))
	fi
}

echo "Parcours :"
attendre ferme ferme "au démarrage, les deux volets sont rentrés"

# LE POINTEUR POSÉ, une seconde pleine. On le pose et on ne le bouge plus :
# tout mouvement DANS la bande rearme la minuterie.
"$POINTEUR" va 3 540; sleep 1.4
attendre ouvert ferme "pointeur posé contre le bord gauche"

"$POINTEUR" clic 900 400
attendre ferme ferme "clic à côté : la nappe referme"

"$POINTEUR" glisse 3 300 60 300 8
attendre ouvert ferme "glisser depuis le bord gauche"

"$POINTEUR" clic 900 400
attendre ferme ferme "clic à côté, de nouveau"

# LA LISIÈRE EST LARGE POUR LE DOIGT, PAS POUR LE POINTEUR. Elle fait 24 px
# — un contact rapporté une trame après la pose a déjà quitté les dix
# premiers —, mais le pointeur POSÉ n'arme qu'à moins de 10 px du bord
# (POSE_PX) : une souris immobilisée sur la bordure d'une fenêtre ne doit pas
# faire sortir un volet.
"$POINTEUR" va 16 500; sleep 1.4
attendre ferme ferme "pointeur posé à 16 px : hors de portée de la pose"
"$POINTEUR" va 900 400

"$POINTEUR" va 1917 540; sleep 1.4
attendre ferme ouvert "pointeur posé contre le bord droit"

# LES BANDES SONT SOUS LA NAPPE quand un tiroir est sorti : la fenêtre du
# tiroir est créée après elles, donc au-dessus. Se poser contre l'autre bord
# n'ouvre donc rien tant qu'un volet est dehors — et c'est voulu : le geste
# qui suit l'ouverture d'un tiroir est presque toujours de le refermer.
"$POINTEUR" va 3 540; sleep 1.4
attendre ferme ouvert "l'autre bord n'ouvre rien tant qu'un volet est dehors"

"$POINTEUR" clic 900 400
attendre ferme ferme "clic à côté : le volet droit rentre"

echo
grep -hE "CRITICAL|WARNING \*\*|AddressSanitizer|runtime error" \
	"$SORTIE/status.log" | sed 's/^/  journal : /'
echo "$N étapes, $ECHECS en échec. Captures et journaux : $SORTIE"
exit "$ECHECS"
