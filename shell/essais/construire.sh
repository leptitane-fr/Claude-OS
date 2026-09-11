#!/usr/bin/env bash
#
# Claude OS — Construction des programmes d'essai (lecteur vidéo, dock)
#
# Ces programmes ne sont PAS installés sur la machine et ne figurent pas dans
# meson.build : ce sont des instruments de banc, pas des composants du bureau.
# Ils vivent hors du chantier de l'autre instance, qui travaille dans le même
# dépôt.
#
# Usage :  bash shell/essais/construire.sh [nom…]
#          (sans argument : tout)
#
# Les binaires sortent dans shell/essais/build/, ignoré par git.

set -uo pipefail

ICI="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ICI/build"
mkdir -p "$BUILD"

# -O2 et non -Os : ces programmes mesurent des chemins de décodage, et une
# construction bridée fausserait la mesure qu'ils servent à faire.
# LE MEME DIALECTE QUE MESON, ET C'EST IMPORTANT.
#
# Le banc compilait en gnu11 la ou meson compile en c11 : un code qui passait
# ici echouait la, et comme l'echec n'etait pas lu, on a installe six fois de
# suite un binaire perime. Un banc qui ne compile pas comme la cible ne
# prouve rien.
COMMUN=(-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter)

# LES DEUX DETECTEURS, ET ILS NE CHERCHENT PAS LA MEME CHOSE.
#
#   CLAUDE_OS_SANITIZE=adresse   usages apres liberation, debordements
#   CLAUDE_OS_SANITIZE=fils      courses entre les trois fils du lecteur
#
# Le second est le plus utile ici : le lecteur fait tourner un fil de
# decodage, le fil temps reel de PipeWire et le fil principal de GTK sur les
# memes structures. Une course ne se voit ni a la lecture, ni a la
# compilation, et ne se reproduit pas a volonte.
case "${CLAUDE_OS_SANITIZE:-}" in
	adresse) COMMUN+=(-fsanitize=address -fno-omit-frame-pointer) ;;
	fils)    COMMUN+=(-fsanitize=thread  -fno-omit-frame-pointer) ;;
	"")      ;;
	*) echo "CLAUDE_OS_SANITIZE : « adresse » ou « fils »" >&2; exit 2 ;;
esac

construire() {
	local nom="$1"; shift
	local src="$ICI/$nom.c"
	[ -f "$src" ] || { echo "  $nom : source absente ($src)" >&2; return 1; }

	echo "  $nom…"
	# INVARIANT N°4 : la sortie du compilateur n'est jamais avalée.
	if ! gcc "${COMMUN[@]}" "$src" -o "$BUILD/$nom" "$@"; then
		echo "  $nom : ÉCHEC de la compilation" >&2
		return 1
	fi
	return 0
}

flags() { pkg-config --cflags --libs "$@" || { echo "pkg-config a échoué pour : $*" >&2; exit 1; }; }

CIBLES=("$@")
[ ${#CIBLES[@]} -gt 0 ] || CIBLES=(fabrique-mire sonde-offload video pointeur)

ECHECS=0
echo "Construction des programmes d'essai :"
for c in "${CIBLES[@]}"; do
	case "$c" in
		fabrique-mire)
			# shellcheck disable=SC2046
			construire fabrique-mire $(flags libavcodec libavformat libavutil) -lm || ECHECS=$((ECHECS+1))
			;;
		video)
			# Le lecteur lui-meme, compile ici tant que la phase 1 dure :
			# meson.build appartient aussi a l'autre instance, on n'y touche
			# qu'une fois le programme en etat de marche.
			# shellcheck disable=SC2046
			# SHELL_DATA_DIR : le DEPOT, et non /usr, tant que le lecteur
			# n'est pas installe. C'est ce qui permet d'eprouver une
			# feuille de style modifiee sans passer par « meson install »,
			# donc sans toucher a la machine pendant que l'autre instance
			# travaille. Le binaire produit ici est un binaire de banc.
			DATA_DIR="${CLAUDE_OS_DATA_DIR:-$ICI/..}"
			gcc "${COMMUN[@]}" -I"$ICI/../src" \
			    -DSHELL_DATA_DIR="\"$DATA_DIR\"" \
			    "$ICI/../src/video.c" "$ICI/../src/video-moteur.c" \
			    "$ICI/../src/video-image.c" "$ICI/../src/video-audio.c" \
			    "$ICI/../src/video-bibliotheque.c" \
			    "$ICI/../src/config.c" "$ICI/../src/visibility.c" \
			    "$ICI/../src/sysfs.c" "$ICI/../src/modes-energie.c" \
			    -o "$BUILD/claude-os-video" \
			    $(flags gtk4 gio-unix-2.0 pangocairo libavcodec libavformat libavutil libswscale libswresample libdrm libpipewire-0.3) -lm \
			    || { echo "  video : ÉCHEC de la compilation" >&2; ECHECS=$((ECHECS+1)); }
			echo "  video…"
			;;
		sonde-offload)
			# shellcheck disable=SC2046
			construire sonde-offload $(flags gtk4 libavcodec libavformat libavutil libswscale libdrm) || ECHECS=$((ECHECS+1))
			;;
		pointeur)
			# Le pointeur virtuel du banc du dock. Le protocole n'est
			# empaquete nulle part dans Debian (pas de wlr-protocols) : le
			# XML est verse dans shell/protocols/, comme foreign-toplevel.
			XML="$ICI/../protocols/wlr-virtual-pointer-unstable-v1.xml"
			if wayland-scanner client-header "$XML" "$BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h" \
			   && wayland-scanner private-code "$XML" "$BUILD/wlr-virtual-pointer-protocol.c"; then
				# shellcheck disable=SC2046
				construire pointeur -I"$BUILD" "$BUILD/wlr-virtual-pointer-protocol.c" \
				    $(flags wayland-client) || ECHECS=$((ECHECS+1))
			else
				echo "  pointeur : wayland-scanner a échoué" >&2; ECHECS=$((ECHECS+1))
			fi
			;;
		*)
			echo "  cible inconnue : $c" >&2; ECHECS=$((ECHECS+1)) ;;
	esac
done

if [ "$ECHECS" -gt 0 ]; then
	echo "$ECHECS cible(s) en échec." >&2
	exit 1
fi
echo "Fait — binaires dans $BUILD"
