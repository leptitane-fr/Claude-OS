#!/usr/bin/env bash
#
# Claude OS — Essayer le shell GTK4 sur cette machine
#
# Installe, à côté de la session actuelle, le shell graphique développé dans la
# branche « claude/custom-linux-usb-debian-ceoegh » : dock, barre d'état,
# réglages, écrits en C sur GTK4 + layer-shell, sous le compositeur labwc.
#
# RIEN N'EST CASSÉ NI REMPLACÉ
# Tout va dans /usr/local et dans le dossier personnel. Ni l'amorçage, ni
# LightDM, ni la session « Claude OS » actuelle ne sont touchés : un
# redémarrage revient à l'état d'avant, quoi qu'il arrive.
#
#   bash tools/essayer-shell.sh              # installe et prépare l'essai
#   bash tools/essayer-shell.sh --desinstaller
#
set -uo pipefail

BRANCHE="claude/custom-linux-usb-debian-ceoegh"
ARBRE="$HOME/shell-essai"
DESINSTALLER=0

while [ $# -gt 0 ]; do
	case "$1" in
		--desinstaller) DESINSTALLER=1 ;;
		-h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "Option inconnue : $1" >&2; exit 2 ;;
	esac
	shift
done

STEP=0
say()  { STEP=$((STEP+1)); printf '\n\033[1;34m[%d]\033[0m \033[1m%s\033[0m\n' "$STEP" "$*"; }
info() { printf '      %s\n' "$*"; }
warn() { printf '      \033[33m! %s\033[0m\n' "$*"; }
die()  { printf '\n\033[31mÉCHEC : %s\033[0m\n' "$*" >&2; exit 1; }

DEPOT="$(cd "$(dirname "$0")/.." && pwd)"

# ------------------------------------------------------------- désinstallation

if [ "$DESINSTALLER" -eq 1 ]; then
	say "Désinstallation"
	[ -d "$ARBRE/shell/build" ] && sudo ninja -C "$ARBRE/shell/build" uninstall >/dev/null 2>&1 || true
	sudo rm -f /usr/local/bin/claude-os-shell-basculer /etc/chromium.d/zz-claude-os-wayland
	rm -rf "$HOME/.config/labwc"
	git -C "$DEPOT" worktree remove --force "$ARBRE" 2>/dev/null || rm -rf "$ARBRE"
	info "Retiré. La configuration ~/.config/claude-os/ est conservée :"
	info "elle sert aussi à la session actuelle."
	exit 0
fi

# ------------------------------------------------------------------ préalables

say "Vérification des préalables"

[ "$(id -u)" -ne 0 ] || die "à lancer SANS sudo, depuis votre compte. Le script appelle sudo lui-même."
sudo -n true 2>/dev/null || sudo -v || die "sudo indisponible pour ce compte."
info "utilisateur : $(id -un)"

git -C "$DEPOT" rev-parse --git-dir >/dev/null 2>&1 || die "$DEPOT n'est pas un dépôt git."

# ---------------------------------------------------------------- les paquets

say "Installation des paquets"

# Deux groupes : ce qu'il faut pour EXÉCUTER le shell, et ce qu'il faut pour le
# COMPILER. Le second pourra être retiré ensuite si l'essai est concluant.
EXEC="labwc libgtk4-layer-shell0 papirus-icon-theme fonts-inter libglib2.0-bin xwayland foot"
BUILD="build-essential meson ninja-build pkgconf libgtk-4-dev libgtk4-layer-shell-dev libwayland-bin libwayland-dev"

info "exécution : labwc, gtk4-layer-shell, foot (terminal de secours)…"
info "compilation : meson, ninja, en-têtes GTK4…"
sudo apt-get update -qq || warn "apt-get update a échoué, on tente quand même"
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
	$EXEC $BUILD || die "installation des paquets impossible."

# ------------------------------------------------------------- les sources

say "Récupération du shell"

# Un « worktree » plutôt qu'un clone : la branche actuelle n'est pas touchée,
# et les deux versions cohabitent sans se marcher dessus.
git -C "$DEPOT" fetch origin "$BRANCHE" --quiet || die "impossible de récupérer la branche $BRANCHE."
if [ -d "$ARBRE" ]; then
	info "arbre de travail déjà présent, mise à jour"
	git -C "$ARBRE" fetch origin "$BRANCHE" --quiet 2>/dev/null || true
	git -C "$ARBRE" checkout --quiet "origin/$BRANCHE" 2>/dev/null || true
else
	git -C "$DEPOT" worktree add --detach "$ARBRE" "origin/$BRANCHE" --quiet \
		|| die "création de l'arbre de travail impossible."
fi
[ -f "$ARBRE/shell/meson.build" ] || die "sources du shell introuvables dans $ARBRE/shell."
info "sources dans $ARBRE"

# ------------------------------------------------------------- compilation

say "Compilation"

cd "$ARBRE/shell" || die "accès impossible à $ARBRE/shell"
# --prefix=/usr/local : aucun fichier géré par dpkg n'est écrasé.
meson setup build --prefix=/usr/local --wipe >/dev/null 2>&1 \
	|| meson setup build --prefix=/usr/local >/dev/null 2>&1 \
	|| die "meson setup a échoué. Relancer à la main pour voir le détail :
      cd $ARBRE/shell && meson setup build --prefix=/usr/local"
ninja -C build || die "la compilation a échoué."
sudo meson install -C build >/dev/null || die "l'installation a échoué."
info "binaires installés dans /usr/local/bin"

# -------------------------------------------------------------- configuration

say "Configuration"

mkdir -p "$HOME/.config/labwc" "$HOME/.config/claude-os"

cp "$ARBRE/overlay/etc/xdg/labwc/rc.xml"    "$HOME/.config/labwc/"
cp "$ARBRE/overlay/etc/xdg/labwc/autostart" "$HOME/.config/labwc/"
sudo install -m755 "$ARBRE/overlay/usr/local/bin/claude-os-shell-basculer" /usr/local/bin/

# L'environnement de session, adapté à cette machine.
#
# ELECTRON_OZONE_PLATFORM_HINT est l'ajout décisif : Claude Desktop est une
# application Electron, et sans cette variable elle démarre via Xwayland — un
# serveur X entier en mémoire, un rendu plus flou sur écran dense, et le
# presse-papier mal partagé. Chromium a son propre fichier de réglages, plus
# bas ; Electron, lui, ne lit que l'environnement.
cat > "$HOME/.config/labwc/environment" <<'FIN'
# Disposition du clavier : azerty.
XKB_DEFAULT_LAYOUT=fr

# GTK parle Wayland directement, sans repasser par Xwayland.
GDK_BACKEND=wayland

# Claude Desktop est une application Electron : c'est cette variable, et elle
# seule, qui lui fait utiliser Wayland nativement.
ELECTRON_OZONE_PLATFORM_HINT=auto
FIN

# Le terminal de secours du raccourci Super+Entrée : la branche d'origine
# lançait xfce4-terminal, absent ici. « foot » est natif Wayland et pèse peu.
sed -i 's|command="xfce4-terminal"|command="foot"|' "$HOME/.config/labwc/rc.xml"

# Les applications épinglées : uniquement ce qui est réellement installé, sinon
# le dock affiche un pictogramme générique qui ne lance rien.
PINNED="chromium"
command -v claude-desktop >/dev/null 2>&1 && PINNED="$PINNED;claude-desktop"
command -v mousepad       >/dev/null 2>&1 && PINNED="$PINNED;mousepad"
PINNED="$PINNED;claude-os-reglages"

cat > "$HOME/.config/claude-os/shell.conf" <<FIN
[dock]
pinned=$PINNED
reserve_space=false

[appearance]
font=Inter
icon_theme=Papirus
theme=claude-sombre

[wallpaper]
image=/usr/share/claude-os/wallpaper/default.png
fill=true
FIN
info "dock épinglé sur : $PINNED"

# Chromium tente X11 par défaut et refuserait de démarrer sans serveur X.
# Fichier séparé : une seule chose à supprimer pour revenir en arrière.
echo 'CHROMIUM_FLAGS="$CHROMIUM_FLAGS --ozone-platform-hint=auto"' \
	| sudo tee /etc/chromium.d/zz-claude-os-wayland >/dev/null

# ------------------------------------------------------------------- la suite

say "Prêt"

VT_ACTUEL="$(loginctl list-sessions --no-legend 2>/dev/null | awk '{print $1}' \
             | while read -r s; do loginctl show-session "$s" -p VTNr -p Type --value 2>/dev/null \
             | paste -sd' ' -; done | grep -i x11 | awk '{print $1}' | head -1)"

cat <<FIN

  Votre clavier n'a pas de touches F : le changement de terminal virtuel se
  pilote donc depuis SSH, avec « chvt ».

  1. Repérer où tourne la session actuelle, pour savoir où revenir :

       loginctl list-sessions
       loginctl show-session <ID> -p VTNr

  2. Basculer sur un terminal libre, DEPUIS SSH :

       sudo chvt 3

  3. SUR LE CHROMEBOOK : se connecter sur tty3, puis lancer l'essai :

       dbus-run-session labwc > ~/essai-shell.log 2>&1

  4. Pour revenir à la session actuelle, DEPUIS SSH :

       sudo chvt ${VT_ACTUEL:-7}

     La session d'essai continue de tourner. « pkill labwc » la ferme.

  Ce qui doit fonctionner :

     touche Loupe (Super)   masque et affiche dock + barre d'état
     Super + Entrée         terminal
     Super + B              Chromium
     clic sur la barre      Wi-Fi, Bluetooth, batterie
     glisser une icône      réorganise le dock, ordre enregistré
     Super + Maj + Q        quitter la session d'essai

  Si rien ne s'affiche :   tail -40 ~/essai-shell.log
  Pour tout retirer   :   bash tools/essayer-shell.sh --desinstaller

FIN
