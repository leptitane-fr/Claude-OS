#!/usr/bin/env bash
#
# Claude OS — Banc du dock qui sort de l'écran
#
# Lance un labwc sans écran, sur un bus de session jetable, avec le dock et
# la barre d'état compilés, un terminal comme application, et le pointeur
# virtuel pour cliquer et glisser. Joue le parcours de la règle écrite dans
# shell/src/visibility.h et vérifie, dans le journal du dock, que chaque
# geste mène à l'état attendu.
#
# Ce que ce banc ne sait PAS éprouver : le doigt. Le glisser passe ici par le
# pointeur, qui emprunte le même GtkGestureDrag mais pas le même chemin dans
# labwc (touch.c). Le geste réel se juge sur la machine.
#
# Usage :
#   bash shell/essais/construire.sh pointeur
#   BUILD=/chemin/vers/build bash shell/essais/banc-dock.sh [dossier-captures]
#
# BUILD : un répertoire de compilation meson du shell, configuré avec
# --prefix=/usr (les feuilles de style installées servent). Celui du dépôt,
# shell/build, appartient à root après un « --compiler » : en compiler un à
# soi, par exemple
#   meson setup /tmp/shell-banc shell --prefix=/usr && ninja -C /tmp/shell-banc
#
# Les captures et les journaux restent dans le dossier donné (par défaut
# /tmp/claude-os-banc-dock). Code de retour : le nombre d'étapes en échec.

set -uo pipefail

ICI="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$ICI/../build}"
SORTIE="${1:-/tmp/claude-os-banc-dock}"
POINTEUR="$ICI/build/pointeur"

[ -x "$BUILD/claude-os-dock" ] && [ -x "$BUILD/claude-os-status" ] \
	|| { echo "Pas de dock ni de barre compilés dans $BUILD" >&2; exit 1; }
[ -x "$POINTEUR" ] || { echo "Compiler d'abord : bash $ICI/construire.sh pointeur" >&2; exit 1; }
for outil in labwc grim foot gapplication gdbus dbus-run-session; do
	command -v "$outil" >/dev/null || { echo "$outil est absent" >&2; exit 1; }
done

# UN BUS À NOUS. Sur celui de la session, le dock et la barre du banc
# passeraient la main à ceux du bureau — des GtkApplication mono-instance —
# et le banc éprouverait la session en cours, pas le code compilé.
if [ -z "${BANC_DOCK_BUS:-}" ]; then
	export BANC_DOCK_BUS=1
	exec dbus-run-session -- bash "$0" "$@"
fi

mkdir -p "$SORTIE"
CONF="$(mktemp -d)"
export XDG_RUNTIME_DIR="$CONF/run"
mkdir -p "$XDG_RUNTIME_DIR" "$CONF/labwc"
chmod 700 "$XDG_RUNTIME_DIR"
: > "$CONF/labwc/autostart"      # AUCUN autostart : le dock et la barre du
                                 # système démarreraient dans le banc.
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

# Les transitions du dock ne sont écrites qu'en débogage (g_debug).
export G_MESSAGES_DEBUG=all

command -v swaybg >/dev/null && { swaybg -c '#3c4043' > "$SORTIE/swaybg.log" 2>&1 & }
"$BUILD/claude-os-dock"   > "$SORTIE/dock.log"   2>&1 &
sleep 0.5
"$BUILD/claude-os-status" > "$SORTIE/status.log" 2>&1 &
sleep 2.5

ECHECS=0
N=0
dernier_etat() { grep -o 'visibilite : [a-z]*' "$SORTIE/dock.log" | tail -1 | cut -d' ' -f3; }
attendre() {
	# $1 : état attendu ; $2 : libellé de l'étape
	N=$((N + 1))
	sleep 0.7
	local vu; vu="$(dernier_etat)"
	grim "$SORTIE/$(printf '%02d' "$N")-$vu.png"
	if [ "$vu" = "$1" ]; then
		printf '  ok    %-52s %s\n' "$2" "$vu"
	else
		printf '  ÉCHEC %-52s attendu %s, vu %s\n' "$2" "$1" "${vu:-rien}"
		ECHECS=$((ECHECS + 1))
	fi
}

echo "Parcours :"
foot -e sleep 300 > "$SORTIE/foot.log" 2>&1 &
sleep 1
attendre cache    "une application s'ouvre"
gapplication action os.claude.shell.dock basculer
attendre convoque "touche Loupe"
"$POINTEUR" clic 900 400
attendre cache    "clic à côté"
"$POINTEUR" glisse 400 1078 400 1030 6
attendre convoque "glisser depuis le bord bas"
"$POINTEUR" clic 300 1020
attendre convoque "clic dans la bande basse : ne renvoie pas"
gapplication action os.claude.shell.dock basculer
attendre cache    "touche Loupe, second appui"
gdbus call --session --dest org.freedesktop.Notifications \
	--object-path /org/freedesktop/Notifications \
	--method org.freedesktop.Notifications.Notify \
	"Banc" 0 "" "Bannière" "La barre remonte pour elle" '[]' '{}' 1500 \
	>> "$SORTIE/notification.log"
sleep 0.5; grim "$SORTIE/banniere.png"
attendre cache    "notification : le dock, lui, ne bouge pas"
pkill -f "foot -e sleep 300"
attendre bureau   "plus aucune fenêtre"
gapplication action os.claude.shell.dock basculer
attendre cache    "touche Loupe sur le bureau vide"
"$POINTEUR" glisse 1500 1078 1500 1030 6
attendre bureau   "glisser sur le bureau vide"
foot --fullscreen -e sleep 300 > "$SORTIE/foot2.log" 2>&1 &
sleep 1
attendre cache    "application plein écran"
gapplication action os.claude.shell.dock basculer
attendre convoque "touche Loupe par-dessus le plein écran"

echo
grep -hE "CRITICAL|WARNING \*\*|AddressSanitizer|runtime error" \
	"$SORTIE/dock.log" "$SORTIE/status.log" | sed 's/^/  journal : /'
echo "$N étapes, $ECHECS en échec. Captures et journaux : $SORTIE"
exit "$ECHECS"
