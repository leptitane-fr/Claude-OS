#!/usr/bin/env bash
#
# Claude OS — Diagnostic de la session graphique
#
# À lancer DEPUIS SSH, en tant qu'utilisateur normal, pendant que la session
# graphique est ouverte sur la machine. Ne modifie rien.
#
#   bash tools/diag-session.sh
#
# Produit un rapport à transmettre tel quel.

set -u

OUT="${1:-diag-session.txt}"
exec > >(tee "$OUT") 2>&1

sec() { printf '\n\033[1;34m── %s\033[0m\n' "$*"; }
val() { printf '  %-34s %s\n' "$1" "$2"; }

UID_="$(id -u)"

sec "1. La session"
loginctl list-sessions --no-legend 2>/dev/null | sed 's/^/  /'
SID="$(loginctl list-sessions --no-legend 2>/dev/null | awk -v u="$(id -un)" '$3==u{print $1; exit}')"
if [ -n "$SID" ]; then
	loginctl show-session "$SID" -p Type -p Class -p Active -p State -p Desktop \
	         -p Display -p Remote 2>/dev/null | sed 's/^/  /'
fi
# LE POINT QUI A COÛTÉ UNE SOIRÉE. LightDM retient la session choisie par
# l'utilisateur et la relance, quoi que dise « user-session » du siège. Une
# préférence périmée renvoie sur une session X vide : fond noir, curseur, et
# un terminal ouvert par le repli de /etc/X11/Xsession.
DESK="$(loginctl show-session "${SID:-}" -p Desktop --value 2>/dev/null)"
case "$DESK" in
	claude-os|labwc) val "SESSION DÉMARRÉE" "$DESK ✓" ;;
	"")              val "SESSION DÉMARRÉE" "<inconnue>" ;;
	*)               val "SESSION DÉMARRÉE" "$DESK ✗  ← ce n'est pas Claude OS" ;;
esac
val "préférence ~/.dmrc" "$(sed -n 's/^Session=//p' "$HOME/.dmrc" 2>/dev/null || echo '<aucune>')"
val "préférence AccountsService" \
    "$(sed -n 's/^Session=//p' "/var/lib/AccountsService/users/$(id -un)" 2>/dev/null || echo '<aucune>')"
val "sessions proposées" "$(ls /usr/share/wayland-sessions /usr/share/xsessions 2>/dev/null | grep '\.desktop' | tr '\n' ' ')"
val "XDG_SESSION_TYPE" "${XDG_SESSION_TYPE:-<vide>}"
val "XDG_RUNTIME_DIR"  "${XDG_RUNTIME_DIR:-<vide>}"

sec "2. Le bus de session — le point critique"
# Sans bus, un GtkApplication ne peut pas s'enregistrer : il quitte aussitôt,
# sans fenêtre et sans message à l'écran. C'est le symptôme « écran noir ».
if [ -S "/run/user/$UID_/bus" ]; then
	val "/run/user/$UID_/bus" "présent ✓"
else
	val "/run/user/$UID_/bus" "ABSENT ✗  ← cause probable"
fi
val "dbus-user-session" "$(dpkg -l dbus-user-session 2>/dev/null | awk '/^ii/{print $3}' || echo 'NON INSTALLÉ')"
val "dbus-x11"          "$(dpkg -l dbus-x11 2>/dev/null | awk '/^ii/{print $3}' || echo 'non installé')"
systemctl --user is-active dbus.socket 2>/dev/null | sed 's/^/  dbus.socket : /'

sec "3. Ce qui tourne"
for p in labwc claude-os-fond claude-os-dock claude-os-status foot lightdm; do
	if pgrep -x "$p" >/dev/null 2>&1; then
		val "$p" "en cours (pid $(pgrep -x "$p" | tr '\n' ' '))"
	else
		val "$p" "absent"
	fi
done
echo "  --- toute ligne de commande contenant « claude-os » ---"
pgrep -a claude-os 2>/dev/null | sed 's/^/  /' || echo "  (aucune)"

sec "4. Les binaires et les styles"
for b in claude-os-fond claude-os-dock claude-os-status \
         claude-os-lanceur claude-os-fichiers claude-os-reglages; do
	val "$b" "$(command -v "$b" 2>/dev/null || echo 'INTROUVABLE')"
done
val "styles" "$(ls /usr/share/claude-os-shell/style/ 2>/dev/null | tr '\n' ' ' || echo 'ABSENTS')"
# Un binaire du shell resté dans /usr/local/bin masquerait celui de /usr/bin.
# Trois noms y vivent LÉGITIMEMENT : le lanceur de session, la bascule du
# dock, et le lanceur Wayland de Claude Desktop. Les compter comme des restes
# était un faux positif de la première version de ce diagnostic.
RESTE="$(ls /usr/local/bin/claude-os-* 2>/dev/null | grep -vE 'claude-os-(claude|shell-basculer|session)$' | tr '\n' ' ')"
[ -n "$RESTE" ] && val "restes /usr/local/bin" "$RESTE  ← masquent /usr/bin" \
                || val "restes /usr/local/bin" "aucun ✓"

sec "5. La configuration de labwc"
val "/etc/xdg/labwc"   "$(ls /etc/xdg/labwc 2>/dev/null | tr '\n' ' ' || echo 'ABSENT')"
val "~/.config/labwc"  "$(ls "$HOME/.config/labwc" 2>/dev/null | tr '\n' ' ' || echo 'absent ✓')"
val "XDG_CONFIG_DIRS"  "${XDG_CONFIG_DIRS:-<vide, défaut /etc/xdg>}"
echo "  --- autostart réellement lu ---"
for d in "$HOME/.config" ${XDG_CONFIG_DIRS:-/etc/xdg}; do
	[ -f "$d/labwc/autostart" ] && { echo "  >>> $d/labwc/autostart"; sed 's/^/      /' "$d/labwc/autostart"; break; }
done

sec "6. La session déclarée"
val "wayland-sessions" "$(ls /usr/share/wayland-sessions/ 2>/dev/null | tr '\n' ' ')"
val "xsessions"        "$(ls /usr/share/xsessions/ 2>/dev/null | tr '\n' ' ' || echo 'aucune ✓')"
cat /etc/lightdm/lightdm.conf.d/*.conf 2>/dev/null | sed 's/^/  /'

sec "7. Le shell.conf"
if [ -f "$HOME/.config/claude-os/shell.conf" ]; then
	sed 's/^/  /' "$HOME/.config/claude-os/shell.conf"
else
	echo "  absent — les valeurs par défaut s'appliquent"
fi

sec "8. Essai à la main : ce que dit le dock quand il démarre"
# C'est LE test décisif. Il rend le message d'erreur que la session avale.
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$UID_}"
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
	W="$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -1)"
	[ -n "$W" ] && export WAYLAND_DISPLAY="$(basename "$W")"
fi
val "WAYLAND_DISPLAY" "${WAYLAND_DISPLAY:-<introuvable>}"
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
	echo "  --- sortie de « claude-os-dock », 5 secondes ---"
	timeout 5 claude-os-dock 2>&1 | sed 's/^/      /' | head -25
	echo "      (code de sortie : $? — 124 = toujours vivant au bout de 5 s, donc OK)"
else
	echo "  Impossible : aucune socket Wayland. La session est-elle bien ouverte ?"
fi

sec "9. Journaux"
echo "  --- journal utilisateur ---"
journalctl --user -b --no-pager 2>/dev/null | tail -25 | sed 's/^/  /' || echo "  (pas de gestionnaire utilisateur)"
echo "  --- lightdm / labwc / claude-os ---"
journalctl -b --no-pager 2>/dev/null | grep -iE 'lightdm|labwc|claude-os' | tail -25 | sed 's/^/  /'

echo
echo "Rapport écrit dans : $OUT — le transmettre tel quel."
