#!/usr/bin/env bash
# Claude OS -- correctif de migration X11/LightDM vers Wayland/greetd.
#
# A lancer depuis SSH, dans un clone a jour du depot :
#   sudo bash install/patch-session-wayland-ssh.sh
#
# Le script ne redemarre pas seul : la connexion SSH reste disponible pour
# lire son resultat et, si besoin, restaurer la sauvegarde indiquee a la fin.
set -Eeuo pipefail

DRY=0
PURGE_X=1

usage() {
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
}

while (($#)); do
    case "$1" in
        --dry-run) DRY=1 ;;
        --keep-x-packages) PURGE_X=0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Option inconnue : $1" >&2; exit 2 ;;
    esac
    shift
done

(( EUID == 0 )) || { echo "Lancer avec sudo." >&2; exit 1; }

run() {
    if (( DRY )); then
        printf '[simulation]'; printf ' %q' "$@"; printf '\n'
    else
        "$@"
    fi
}

die() { echo "ERREUR: $*" >&2; exit 1; }
note() { printf '\n== %s ==\n' "$*"; }

command -v labwc >/dev/null || die "labwc n'est pas installe. Relancer provision.sh d'abord."
systemctl cat greetd.service >/dev/null 2>&1 || die "greetd n'est pas installe. Relancer provision.sh d'abord."
[[ -x /usr/bin/claude-os-connexion ]] || die "claude-os-connexion absent. Relancer provision.sh d'abord."

TARGET_USER=""
if [[ -r /etc/claude-os/utilisateur ]]; then
    TARGET_USER="$(head -n 1 /etc/claude-os/utilisateur)"
fi
[[ -n "$TARGET_USER" ]] || die "Utilisateur Claude OS absent de /etc/claude-os/utilisateur"
TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
[[ -n "$TARGET_HOME" && -d "$TARGET_HOME" ]] || die "Compte cible invalide : $TARGET_USER"

STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP="/root/claude-os-wayland-backup-$STAMP"
note "Sauvegarde recuperable"
run mkdir -p "$BACKUP"

backup_path() {
    local path="$1"
    [[ -e "$path" || -L "$path" ]] || return 0
    local relative="${path#/}"
    run mkdir -p "$BACKUP/$(dirname "$relative")"
    run cp -a -- "$path" "$BACKUP/$relative"
}

note "Suppression des priorites utilisateur de l'ancien bureau"
# labwc prefere ~/.config/labwc a /etc/xdg/labwc : cette copie masque toutes
# les corrections systeme. On la deplace, jamais on ne la detruit.
for path in \
    "$TARGET_HOME/.config/labwc" \
    "$TARGET_HOME/.config/openbox" \
    "$TARGET_HOME/.config/plank" \
    "$TARGET_HOME/.config/tint2" \
    "$TARGET_HOME/.config/picom" \
    "$TARGET_HOME/.config/rofi" \
    "$TARGET_HOME/.config/pcmanfm" \
    "$TARGET_HOME/.xsession" \
    "$TARGET_HOME/.xinitrc"; do
    if [[ -e "$path" || -L "$path" ]]; then
        backup_path "$path"
        run rm -rf -- "$path"
    fi
done

note "Deploiement de la session Wayland"
write_file() {
    local destination="$1" mode="$2"
    if (( DRY )); then
        printf '[simulation] installerait %s (mode %s)\n' "$destination" "$mode"
        cat >/dev/null
    else
        install -D -m "$mode" /dev/stdin "$destination"
    fi
}

write_file /etc/xdg/labwc/environment 0644 <<'EOF'
XKB_DEFAULT_LAYOUT=fr
GDK_BACKEND=wayland
XDG_SESSION_TYPE=wayland
XDG_CURRENT_DESKTOP=labwc
XDG_SESSION_DESKTOP=claude-os
QT_QPA_PLATFORM=wayland
ELECTRON_OZONE_PLATFORM_HINT=wayland
MOZ_ENABLE_WAYLAND=1
EOF

write_file /etc/xdg/labwc-greeter/environment 0644 <<'EOF'
XKB_DEFAULT_LAYOUT=fr
GDK_BACKEND=wayland
XDG_SESSION_TYPE=wayland
XDG_CURRENT_DESKTOP=labwc
XDG_SESSION_DESKTOP=claude-os-greeter
EOF

write_file /etc/xdg/labwc-greeter/rc.xml 0644 <<'EOF'
<?xml version="1.0"?>
<labwc_config>
  <core><decoration>client</decoration><gap>0</gap></core>
  <theme><dropShadows>no</dropShadows></theme>
  <keyboard></keyboard>
  <mouse></mouse>
</labwc_config>
EOF

write_file /etc/xdg/labwc-greeter/autostart 0755 <<'EOF'
#!/bin/sh
exec /usr/local/bin/claude-os-greeter
EOF

write_file /usr/local/bin/claude-os-session 0755 <<'EOF'
#!/bin/sh
set -u
: "${XDG_RUNTIME_DIR:=/run/user/$(id -u)}"
export XDG_RUNTIME_DIR
unset DISPLAY
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=labwc
export XDG_SESSION_DESKTOP=claude-os
export GDK_BACKEND=wayland
export QT_QPA_PLATFORM=wayland
export ELECTRON_OZONE_PLATFORM_HINT=wayland
export MOZ_ENABLE_WAYLAND=1
if [ -S "$XDG_RUNTIME_DIR/bus" ] || [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    exec labwc "$@"
fi
if command -v dbus-run-session >/dev/null 2>&1; then
    exec dbus-run-session -- labwc "$@"
fi
echo "claude-os-session : aucun bus de session, et dbus-run-session est absent." >&2
exec labwc "$@"
EOF

write_file /usr/local/bin/claude-os-greeter 0755 <<'EOF'
#!/bin/sh
set -u
JOURNAL=/var/log/claude-os-connexion.log
COMPTE=""
[ -r /etc/claude-os/utilisateur ] && COMPTE="$(cat /etc/claude-os/utilisateur)"
if [ -n "$COMPTE" ]; then set -- --utilisateur "$COMPTE"; else set --; fi
if : > "$JOURNAL" 2>/dev/null; then
    exec /usr/bin/claude-os-connexion "$@" >> "$JOURNAL" 2>&1
fi
exec /usr/bin/claude-os-connexion "$@"
EOF

write_file /usr/share/wayland-sessions/claude-os.desktop 0644 <<'EOF'
[Desktop Entry]
Name=Claude OS
Comment=Dock centre, barre d'etat, lanceur — sur labwc
Exec=/usr/local/bin/claude-os-session
TryExec=/usr/local/bin/claude-os-session
Type=Application
DesktopNames=labwc
EOF

# Anciennes entrees X11 : elles peuvent continuer d'etre proposees par LightDM
# ou un display manager residuel. Les supprimer ne touche pas aux documents.
for path in \
    /usr/share/xsessions/claude-os.desktop \
    /usr/local/share/applications/claude-os-launcher.desktop \
    /usr/local/share/applications/claude-os-settings.desktop \
    /usr/local/share/applications/claude-os-chromium.desktop; do
    backup_path "$path"
    run rm -f -- "$path"
done

note "Bascule du gestionnaire de session"
backup_path /etc/X11/default-display-manager
backup_path /etc/systemd/system/display-manager.service

# On ne doit jamais supprimer un vrai fichier d'administration inconnu.
if [[ -e /etc/systemd/system/display-manager.service && ! -L /etc/systemd/system/display-manager.service ]]; then
    die "/etc/systemd/system/display-manager.service n'est pas un lien : abandon sans ecrasement"
fi

if systemctl cat lightdm.service >/dev/null 2>&1; then
    run systemctl disable lightdm.service
    run systemctl stop lightdm.service
else
    echo "LightDM n'est deja plus present."
fi
run rm -f /etc/systemd/system/display-manager.service
run systemctl daemon-reload
run systemctl enable greetd.service
run sh -c "printf '%s\\n' /usr/sbin/greetd > /etc/X11/default-display-manager"

if (( PURGE_X )); then
    note "Purge des composants X11 abandonnes"
    # Liste volontairement limitee aux anciens composants du bureau. Aucun
    # autoremove global : il pourrait retirer des paquets non lies au patch.
    packages=(
        lightdm lightdm-gtk-greeter
        xserver-xorg-core xserver-xorg-input-libinput x11-common xwayland
        openbox plank tint2 picom rofi pcmanfm xcape xdotool dunst xwallpaper
        network-manager-gnome blueman
    )
    installed=()
    for package in "${packages[@]}"; do
        dpkg-query -W -f='${db:Status-Status}' "$package" 2>/dev/null | grep -qx installed \
            && installed+=("$package")
    done
    if ((${#installed[@]})); then
        run env DEBIAN_FRONTEND=noninteractive apt-get purge -y "${installed[@]}"
    else
        echo "Aucun paquet X11 de l'ancienne pile n'est installe."
    fi
fi

note "Verification"
if (( ! DRY )); then
    systemctl is-enabled --quiet greetd.service || die "greetd n'est pas active"
    systemctl start greetd.service
    systemctl is-active --quiet greetd.service || die "greetd ne demarre pas; restaurer $BACKUP"
    [[ ! -e "$TARGET_HOME/.config/labwc" ]] || die "La configuration labwc utilisateur masque encore /etc/xdg"
    grep -qx 'XDG_SESSION_TYPE=wayland' /etc/xdg/labwc/environment \
        || die "Environnement Wayland incomplet"
fi

echo
echo "Correctif installe. Sauvegarde : $BACKUP"
echo "La session graphique a ete basculee sur greetd/labwc."
echo "Redemarrer des que vous avez termine la verification SSH : sudo systemctl reboot"
