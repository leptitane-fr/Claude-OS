#!/usr/bin/env bash
#
# Claude OS — Construction des programmes d'essai du lecteur vidéo
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
COMMUN=(-std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter)

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
[ ${#CIBLES[@]} -gt 0 ] || CIBLES=(fabrique-mire sonde-offload video)

ECHECS=0
echo "Construction des essais du lecteur vidéo :"
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
			gcc "${COMMUN[@]}" -I"$ICI/../src" \
			    "$ICI/../src/video.c" "$ICI/../src/video-moteur.c" \
			    "$ICI/../src/video-image.c" "$ICI/../src/video-audio.c" \
			    -o "$BUILD/claude-os-video" \
			    $(flags gtk4 libavcodec libavformat libavutil libswscale libswresample libdrm libpipewire-0.3) -lm \
			    || { echo "  video : ÉCHEC de la compilation" >&2; ECHECS=$((ECHECS+1)); }
			echo "  video…"
			;;
		sonde-offload)
			# shellcheck disable=SC2046
			construire sonde-offload $(flags gtk4 libavcodec libavformat libavutil libswscale libdrm) || ECHECS=$((ECHECS+1))
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
