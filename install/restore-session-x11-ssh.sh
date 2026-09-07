#!/usr/bin/env bash
# Claude OS -- retour d'urgence vers LightDM/Xorg.
#
# A lancer depuis SSH :
#   sudo bash install/restore-session-x11-ssh.sh
#
# Cette procedure ne redemarre pas la machine seule afin de conserver SSH.
set -Eeuo pipefail

(( EUID == 0 )) || { echo "Lancer avec sudo." >&2; exit 1; }

die() { echo "ERREUR: $*" >&2; exit 1; }
note() { printf '\n== %s ==\n' "$*"; }

TARGET_USER=""
if [[ -r /etc/claude-os/utilisateur ]]; then
    TARGET_USER="$(head -n 1 /etc/claude-os/utilisateur)"
fi
[[ -n "$TARGET_USER" ]] || die "Utilisateur Claude OS introuvable."
TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
[[ -d "$TARGET_HOME" ]] || die "Dossier personnel introuvable : $TARGET_USER"

# Le patch Wayland conserve une copie des fichiers qu'il a remplaces ou
# deplaces. La plus recente est celle a restaurer ; son absence reste un cas
# supporte, car LightDM/Openbox peut alors demarrer avec ses reglages standards.
BACKUP="$(find /root -maxdepth 1 -mindepth 1 -type d -name 'claude-os-wayland-backup-*' -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -n 1 | cut -d' ' -f2-)"
if [[ -n "$BACKUP" ]]; then
    echo "Sauvegarde utilisee : $BACKUP"
else
    echo "Aucune sauvegarde du patch trouvee : restauration de la pile X11 standard."
fi

note "Reinstallation de la pile graphique de secours"
export DEBIAN_FRONTEND=noninteractive
dpkg --configure -a
apt-get update
apt-get install -y \
    lightdm lightdm-gtk-greeter \
    xserver-xorg-core xserver-xorg-input-libinput x11-common xwayland \
    openbox tint2 rofi pcmanfm \
    network-manager-gnome blueman \
    xterm

restore_path() {
    local path="$1"
    [[ -n "$BACKUP" && -e "$BACKUP/${path#/}" ]] || return 0
    mkdir -p "$(dirname "$path")"
    rm -rf -- "$path"
    cp -a -- "$BACKUP/${path#/}" "$path"
}

note "Restauration des reglages precedents"
restore_path "$TARGET_HOME/.config/labwc"
restore_path "$TARGET_HOME/.config/openbox"
restore_path "$TARGET_HOME/.config/plank"
restore_path "$TARGET_HOME/.config/tint2"
restore_path "$TARGET_HOME/.config/picom"
restore_path "$TARGET_HOME/.config/rofi"
restore_path "$TARGET_HOME/.config/pcmanfm"
restore_path "$TARGET_HOME/.xsession"
restore_path "$TARGET_HOME/.xinitrc"
restore_path /etc/X11/default-display-manager
restore_path /etc/systemd/system/display-manager.service
chown -R "$TARGET_USER:$TARGET_USER" "$TARGET_HOME/.config" 2>/dev/null || true

note "Retour au gestionnaire de connexion LightDM"
# L'alias display-manager.service ne peut pointer que vers un seul gestionnaire.
systemctl disable --now greetd.service 2>/dev/null || true
rm -f /etc/systemd/system/display-manager.service
systemctl daemon-reload
systemctl enable lightdm.service

# Si le fichier n'existait pas avant le patch, ecrire une valeur Debian valide.
if [[ ! -s /etc/X11/default-display-manager ]]; then
    printf '%s\n' /usr/sbin/lightdm > /etc/X11/default-display-manager
fi

systemctl start lightdm.service
systemctl is-active --quiet lightdm.service || die "LightDM ne demarre pas. Consulter : journalctl -u lightdm -b"

echo
echo "Retour X11/LightDM termine. SSH reste actif."
echo "Redemarrer maintenant : sudo reboot"
