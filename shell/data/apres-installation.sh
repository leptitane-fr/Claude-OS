#!/bin/sh
#
# Claude OS — ce qu'il reste à faire APRÈS « meson install ».
#
# POSER UN .desktop NE SUFFIT PAS. Le système ne lit pas les fichiers un par
# un pour savoir qui sait ouvrir quoi : il lit « mimeinfo.cache », que
# « update-desktop-database » régénère. Sans cette étape, une application
# parfaitement installée, avec ses types déclarés, reste introuvable — et
# Fichiers répond « aucune application n'est installée pour ce type de
# fichier ».
#
# Constaté sur MADOO le 10 septembre 2026 : le lecteur vidéo était en place,
# son .desktop aussi, et le cache datait de deux jours plus tôt. La
# visionneuse d'images avait le même défaut, masqué par le fait que quelqu'un
# l'avait désignée à la main dans mimeapps.list.
#
# INVARIANT N°4 : cette étape parle, en bien comme en mal.

set -u

CIBLE="${MESON_INSTALL_DESTDIR_PREFIX:-/usr}/share/applications"

if ! command -v update-desktop-database > /dev/null 2>&1; then
	echo "après-installation : update-desktop-database est ABSENT." >&2
	echo "  Les applications ne seront proposées pour aucun type de fichier." >&2
	echo "  Installer « desktop-file-utils »." >&2
	exit 0          # ne pas faire échouer l'installation pour autant
fi

if [ ! -d "$CIBLE" ]; then
	echo "après-installation : $CIBLE n'existe pas, rien à faire."
	exit 0
fi

if update-desktop-database "$CIBLE"; then
	echo "après-installation : base des types MIME régénérée dans $CIBLE"
else
	echo "après-installation : update-desktop-database a ÉCHOUÉ ($?)." >&2
	echo "  Les nouvelles applications ne seront proposées pour aucun type." >&2
fi
