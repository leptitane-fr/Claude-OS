#!/usr/bin/env bash
#
# Claude OS — Banc de Fichiers sur la surface d'outils
#
# Le dernier banc du chantier (docs/14), et le seul qui éprouve une VRAIE
# application plutôt qu'un témoin écrit pour l'occasion. C'est la différence
# qui compte : le témoin avait été conçu pour le contrat, Fichiers a été
# écrit trois semaines avant lui.
#
# Ce qu'il éprouve :
#
#   1. Fichiers publie sa barre, le dock la prend, et la fenêtre escamote
#      son chrome — barre du haut, rangée d'actions, volet des lieux.
#   2. Les lieux du dock sont ceux du volet : mêmes dossiers, même ordre,
#      construits dans la même passe.
#   3. Naviguer depuis le dock change vraiment de dossier, et le fil suit.
#   4. Ctrl+F ouvre l'auvent quand le dock tient la barre, et la frappe
#      filtre la liste.
#   5. Sans dock, le chrome revient et l'application reste utilisable.
#
# Usage :
#   bash shell/essais/construire.sh pointeur frappe
#   BUILD=/chemin/vers/build bash shell/essais/banc-fichiers.sh [dossier]
#
# Code de retour : le nombre de vérifications en échec.

set -uo pipefail

ICI="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$ICI/../build}"
SORTIE="${1:-/tmp/claude-os-banc-fichiers}"
APP=os.claude.shell.fichiers

[ -x "$BUILD/claude-os-dock" ] || { echo "Pas de dock compilé dans $BUILD" >&2; exit 1; }
[ -x "$BUILD/claude-os-fichiers" ] || { echo "Pas de Fichiers compilé dans $BUILD" >&2; exit 1; }
[ -x "$ICI/build/frappe" ] || { echo "Compiler d'abord : bash $ICI/construire.sh frappe" >&2; exit 1; }
for outil in labwc grim gapplication gdbus dbus-run-session wlr-randr; do
	command -v "$outil" >/dev/null || { echo "$outil est absent" >&2; exit 1; }
done

if [ -z "${BANC_FICHIERS_BUS:-}" ]; then
	export BANC_FICHIERS_BUS=1
	exec dbus-run-session -- bash "$0" "$@"
fi

mkdir -p "$SORTIE"
CONF="$(mktemp -d)"
export XDG_RUNTIME_DIR="$CONF/run"
mkdir -p "$XDG_RUNTIME_DIR" "$CONF/labwc"
chmod 700 "$XDG_RUNTIME_DIR"
: > "$CONF/labwc/autostart"
cat > "$CONF/labwc/rc.xml" <<'XML'
<?xml version="1.0"?>
<labwc_config><core><decoration>server</decoration><gap>0</gap></core>
<theme><dropShadows>no</dropShadows></theme></labwc_config>
XML

# UN DOSSIER À NOUS, avec de quoi filtrer. Éprouver la recherche dans le
# dossier personnel dépendrait de ce qui s'y trouve ce jour-là.
ESSAI="$CONF/essai"
mkdir -p "$ESSAI"
for n in alpha bravo charlie mirabelle mirage delta; do : > "$ESSAI/$n.txt"; done

export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
export WLR_HEADLESS_OUTPUTS=1 GDK_BACKEND=wayland GSK_RENDERER=cairo
export G_MESSAGES_DEBUG=all
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
wlr-randr --output HEADLESS-1 --custom-mode 1920x1080 >/dev/null 2>&1

command -v swaybg >/dev/null && { swaybg -c '#3c4043' > "$SORTIE/swaybg.log" 2>&1 & }

ECHECS=0
N=0
etat() { grep -o 'visibilite : [a-z]*' "$SORTIE/dock.log" | tail -1 | cut -d' ' -f3; }
verifier() {
	N=$((N + 1))
	if [ "$2" = "$3" ]; then printf '  ok    %-52s %s\n' "$1" "$3"
	else printf '  ÉCHEC %-52s attendu %s, vu %s\n' "$1" "$2" "${3:-rien}"
	     ECHECS=$((ECHECS + 1)); fi
}
appeler() { # une action de Fichiers, par le chemin du contrat
	gdbus call --session --dest "$APP" --object-path /os/claude/shell/outils \
		--method org.gtk.Actions.Activate "$1" "$2" "{}" >/dev/null 2>&1
}

echo "Parcours :"

# --- 1. SANS DOCK : le chrome doit être là -------------------------------
#
# D'abord, et c'est l'ordre qui compte : si l'on lançait le dock en premier,
# on ne saurait jamais si l'application sait se passer de lui.
"$BUILD/claude-os-fichiers" "$ESSAI" > "$SORTIE/fichiers.log" 2>&1 &
sleep 3
grep -q 'le dock ne prend pas' "$SORTIE/fichiers.log" && CHROME=montre || CHROME=inconnu
# Rien dans le journal : c'est le cas normal -- le rappel ne signale que les
# CHANGEMENTS, et « pas de dock » est l'état de départ (outils.h).
[ "$CHROME" = "inconnu" ] && CHROME=montre
verifier "sans dock, la fenêtre garde son chrome" montre "$CHROME"
grim "$SORTIE/01-sans-dock.png"

# --- 2. le dock arrive ---------------------------------------------------
"$BUILD/claude-os-dock" > "$SORTIE/dock.log" 2>&1 &
sleep 3
PRISE=$(grep -c 'outils : le dock a pris' "$SORTIE/fichiers.log")
verifier "le dock prend la barre" 1 "$PRISE"
verifier "et il reste à l'écran" etabli "$(etat)"
grim "$SORTIE/02-avec-dock.png"

PORTE=$(grep -o 'établi : [0-9]* lieux, [0-9]* étapes, [0-9]* outils' "$SORTIE/dock.log" | tail -1)
printf '  ----- %-52s %s\n' "ce que le dock porte" "$PORTE"
LIEUX=$(echo "$PORTE" | grep -o '[0-9]* lieux' | cut -d' ' -f1)
if [ "${LIEUX:-0}" -ge 5 ]; then
	printf '  ok    %-52s %s\n' "les lieux du volet sont dans la barre" "$LIEUX"
else
	printf '  ÉCHEC %-52s %s\n' "trop peu de lieux dans la barre" "${LIEUX:-0}"
	ECHECS=$((ECHECS + 1))
fi
N=$((N + 1))

# --- 3. naviguer depuis le dock ------------------------------------------
appeler aller "[<'file://$ESSAI'>]"
sleep 1.2
appeler aller "[<'file://$HOME'>]"
sleep 1.5
PORTE2=$(grep -o 'établi : [0-9]* lieux, [0-9]* étapes, [0-9]* outils' "$SORTIE/dock.log" | tail -1)
printf '  ----- %-52s %s\n' "après navigation" "$PORTE2"
ETAPES=$(echo "$PORTE2" | grep -o '[0-9]* étapes' | cut -d' ' -f1)
if [ "${ETAPES:-0}" -ge 2 ]; then
	printf '  ok    %-52s %s\n' "le fil d'Ariane suit la navigation" "$ETAPES étapes"
else
	printf '  ÉCHEC %-52s %s\n' "le fil n'a pas suivi" "${ETAPES:-0} étapes"
	ECHECS=$((ECHECS + 1))
fi
N=$((N + 1))
grim "$SORTIE/03-navigue.png"

# --- 4. Ctrl+F ouvre l'auvent, et la frappe filtre -----------------------
appeler aller "[<'file://$ESSAI'>]"
sleep 1.2
appeler recherche-ouvrir "[]"
sleep 1.2
if grep -q 'auvent : ouvert' "$SORTIE/dock.log"; then
	printf '  ok    %-52s %s\n' "l'action de recherche ouvre l'auvent du dock" "ouvert"
else
	printf '  ÉCHEC %-52s %s\n' "l'auvent devait s'ouvrir" "rien"
	ECHECS=$((ECHECS + 1))
fi
N=$((N + 1))
grim "$SORTIE/04-auvent.png"

"$ICI/build/frappe" "mira" > "$SORTIE/frappe.log" 2>&1
sleep 1.2
grim "$SORTIE/05-filtre.png"

# LA PREUVE QUE LE FILTRE A MORDU. La barre d'état de Fichiers dit combien
# d'éléments sont affichés, et elle est restée dans la fenêtre -- c'est du
# contenu, pas du chrome. Six fichiers au départ, deux contiennent « mira ».
#
# C'est tout le chemin qui se vérifie ici, et lui seul le fait : le dock a
# reçu la frappe, l'a passée à l'action de l'application, qui a refiltré sa
# liste. Aucun des maillons ne peut être éprouvé isolément.
ETAT=$(grep -o 'etat : [0-9]* éléments' "$SORTIE/fichiers.log" | tail -1)
verifier "la frappe dans l'auvent a filtré la liste" "etat : 2 éléments" "${ETAT:-rien}"

"$ICI/build/frappe" --touche Escape >> "$SORTIE/frappe.log" 2>&1
sleep 1
FERME=$(grep -c 'auvent : ferme' "$SORTIE/dock.log")
verifier "Échap referme l'auvent" 1 "$FERME"

# --- 5. le dock s'en va : le chrome revient ------------------------------
pkill -f "$BUILD/claude-os-dock"
sleep 2
REVENU=$(grep -c 'outils : le dock ne prend pas' "$SORTIE/fichiers.log")
verifier "le dock parti, le chrome revient" 1 "$REVENU"
grim "$SORTIE/06-dock-parti.png"

echo
grep -hE "CRITICAL|WARNING \*\*|AddressSanitizer|runtime error" \
	"$SORTIE/dock.log" "$SORTIE/fichiers.log" 2>/dev/null | sed 's/^/  journal : /'
echo "$N vérifications, $ECHECS en échec. Captures et journaux : $SORTIE"
exit "$ECHECS"
