#!/usr/bin/env bash
#
# Claude OS — Validation de l'installation
#
# À lancer APRÈS provision.sh, depuis la session graphique de préférence.
# Passe en revue tout ce qui a été supposé pendant la conception et rend un
# verdict par poste. Ne modifie rien.
#
#   bash validate-install.sh              # rapport à l'écran + fichier
#   bash validate-install.sh -o bilan.md
#
set -uo pipefail

OUT=""
while [ $# -gt 0 ]; do
	case "$1" in
		-o|--output) OUT="${2:-}"; shift 2 ;;
		-h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "Option inconnue : $1" >&2; exit 2 ;;
	esac
done
[ -n "$OUT" ] || OUT="bilan-claude-os-$(date +%Y%m%d-%H%M).md"

PASS=0; WARN=0; FAIL=0
have() { command -v "$1" >/dev/null 2>&1; }

exec 3>"$OUT"
w() { printf '%s\n' "$*" >&3; }

sec()  { printf '\n\033[1;34m── %s\033[0m\n' "$1"; w ""; w "## $1"; w ""; }
ok()   { PASS=$((PASS+1)); printf '  \033[32m✓\033[0m %s\n' "$1"; w "- ✅ $1"; }
warn() { WARN=$((WARN+1)); printf '  \033[33m!\033[0m %s\n' "$1"; w "- ⚠️ $1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31m✗\033[0m %s\n' "$1"; w "- ❌ $1"; }
note() { printf '    %s\n' "$1"; w "  - $1"; }
raw()  { w ""; w '```'; printf '%s\n' "$1" >&3; w '```'; w ""; }

echo "Claude OS — validation de l'installation"
w "# Claude OS — validation de l'installation"
w ""
w "Généré le $(date -Is 2>/dev/null || date) sur \`$(uname -n)\`."

# ------------------------------------------------------------------ système
sec "1. Système"

KV="$(uname -r)"
case "$KV" in
	6.1[2-9]*|6.[2-9][0-9]*|[7-9].*) ok "noyau $KV (6.12+ attendu pour l'audio SOF)" ;;
	*) warn "noyau $KV — antérieur au 6.12 visé, l'audio peut en pâtir" ;;
esac

if [ -d /sys/firmware/efi ]; then
	ok "démarrage UEFI — le firmware MrChromebox fonctionne"
else
	bad "pas de /sys/firmware/efi : démarrage non-UEFI"
fi

CPU="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//')"
case "$CPU" in
	*N6000*) ok "processeur : $CPU" ;;
	*) warn "processeur : $CPU (N6000 attendu)" ;;
esac

RAM_KB="$(awk '/^MemTotal/{print $2}' /proc/meminfo)"
RAM_GB=$(( (RAM_KB + 524288) / 1048576 ))
note "mémoire physique : ${RAM_GB} Go"

if [ -n "$(swapon --show=NAME --noheadings 2>/dev/null | grep zram || true)" ]; then
	ok "zram actif : $(swapon --show=NAME,SIZE --noheadings 2>/dev/null | tr '\n' ' ')"
else
	bad "zram inactif — indispensable à 4 Go. Vérifier systemd-zram-generator."
fi

ROOTFS="$(findmnt -no FSTYPE / 2>/dev/null)"
note "système de fichiers racine : ${ROOTFS:-inconnu}"

# ------------------------------------------------------------------- réseau
sec "2. Wi-Fi"

WNIC="$(ls /sys/class/net 2>/dev/null | grep -E '^wl' | head -1)"
if [ -n "$WNIC" ]; then
	DRV="$(basename "$(readlink -f "/sys/class/net/$WNIC/device/driver" 2>/dev/null)" 2>/dev/null)"
	ok "interface $WNIC présente, pilote « ${DRV:-inconnu} »"
	case "$DRV" in
		rtw_8822ce|rtw88*) note "Realtek confirmé — l'hypothèse RTL8822CE était la bonne" ;;
		iwlwifi)           note "Intel finalement : firmware-realtek peut être purgé" ;;
		*)                 note "pilote inattendu, à signaler" ;;
	esac
	if have nmcli; then
		N="$(nmcli -t -f SSID device wifi list 2>/dev/null | grep -c . || echo 0)"
		[ "$N" -gt 0 ] && ok "$N réseaux détectés" || warn "aucun réseau détecté (antenne ? portée ?)"
		nmcli -t -f STATE general 2>/dev/null | grep -q connected \
			&& ok "connecté au réseau" || warn "non connecté"
	fi
else
	bad "aucune interface Wi-Fi. Firmware manquant ? Voir la section 9."
fi

sec "3. Bluetooth"
if have bluetoothctl && bluetoothctl show 2>/dev/null | grep -q 'Powered: yes'; then
	ok "contrôleur Bluetooth actif"
	note "$(bluetoothctl show 2>/dev/null | grep -m1 'Name:' | sed 's/^\s*//')"
elif have bluetoothctl && bluetoothctl show 2>/dev/null | grep -q Controller; then
	warn "contrôleur présent mais éteint — « bluetoothctl power on »"
else
	bad "aucun contrôleur Bluetooth. Firmware btrtl manquant ?"
fi

# -------------------------------------------------------------------- audio
sec "4. Audio"
w "> Point de risque n°1 du projet. Le symptôme classique sur ces Chromebooks"
w "> est un casque fonctionnel et des haut-parleurs internes muets, faute de"
w "> profil UCM."
w ""

if [ -r /proc/asound/cards ] && grep -q '[0-9]' /proc/asound/cards 2>/dev/null; then
	ok "carte son détectée"
	raw "$(cat /proc/asound/cards)"
	grep -qi sof /proc/asound/cards 2>/dev/null && note "pile SOF en service" \
		|| note "SOF non mentionné — vérifier le pilote"
else
	bad "aucune carte son détectée"
fi

if have wpctl && wpctl status >/dev/null 2>&1; then
	ok "PipeWire répond"
	SINKS="$(wpctl status 2>/dev/null | sed -n '/Sinks:/,/^$/p' | grep -c '\.' || echo 0)"
	[ "$SINKS" -gt 0 ] && ok "$SINKS sortie(s) audio disponible(s)" || bad "aucune sortie audio"
	raw "$(wpctl status 2>/dev/null | sed -n '/Audio/,/Video/p' | head -30)"
else
	bad "PipeWire ne répond pas (wpctl absent ou service arrêté)"
fi

UCM="$(ls /usr/share/alsa/ucm2/conf.d 2>/dev/null | head -20)"
[ -n "$UCM" ] && note "profils UCM présents" || warn "aucun profil UCM sous conf.d"

echo
printf '  \033[1mTest manuel requis :\033[0m écouter les HAUT-PARLEURS puis le CASQUE.\n'
printf '    speaker-test -c2 -twav -l1\n'
w "- ⏳ **Test manuel** : \`speaker-test -c2 -twav -l1\` sur haut-parleurs **et** casque."

# ---------------------------------------------------------------- graphique
sec "5. Graphique et décodage vidéo"

if have vainfo; then
	VA="$(vainfo 2>&1)"
	if echo "$VA" | grep -q 'VAProfile'; then
		ok "VA-API opérationnel"
		echo "$VA" | grep -q 'VAProfileVP9' && ok "VP9 accéléré (codec cible pour YouTube)" \
			|| warn "VP9 non listé"
		echo "$VA" | grep -q 'VAProfileAV1' \
			&& note "AV1 listé — inattendu sur Jasper Lake" \
			|| ok "AV1 absent, conforme : il doit être évité côté navigateur"
		raw "$(echo "$VA" | grep -E 'VAProfile(H264|VP9|HEVC|AV1)' | head -12)"
	else
		bad "VA-API ne répond pas — décodage logiciel, autonomie dégradée"
		raw "$(echo "$VA" | head -6)"
	fi
else
	warn "vainfo absent"
fi

[ -n "$(ls /sys/class/backlight/ 2>/dev/null)" ] \
	&& ok "rétroéclairage pilotable : $(ls /sys/class/backlight/ | tr '\n' ' ')" \
	|| warn "aucun contrôle de rétroéclairage exposé"

# ------------------------------------------------------------------ énergie
sec "6. Énergie"

if have tlp-stat; then
	systemctl is-enabled tlp.service >/dev/null 2>&1 \
		&& ok "TLP activé" || warn "TLP installé mais non activé"
else
	warn "TLP absent"
fi

GOV="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
[ "$GOV" = "powersave" ] && ok "gouverneur : powersave (attendu avec intel_pstate)" \
	|| warn "gouverneur : ${GOV:-inconnu}"

PW="$(cat /sys/class/power_supply/BAT*/power_now 2>/dev/null | head -1)"
if [ -n "$PW" ] && [ "$PW" -gt 0 ] 2>/dev/null; then
	note "puissance instantanée : $(awk -v p="$PW" 'BEGIN{printf "%.1f W", p/1000000}')"
	note "(relever au repos, écran allumé : c'est le chiffre de référence)"
fi

# ------------------------------------------------------------------ session
sec "7. Session Claude OS"

# Le compositeur et les trois composants résidents du shell. Les autres —
# lanceur, gestionnaire de fichiers, réglages — se lancent à la demande : leur
# absence ici est normale.
for p in labwc claude-os-fond claude-os-dock claude-os-status; do
	pgrep -x "$p" >/dev/null 2>&1 && ok "$p en cours" || bad "$p absent"
done

# La session doit être WAYLAND. Une session X11 signifierait que LightDM a
# ouvert autre chose que « Claude OS » — l'ancienne interface, ou un repli.
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
	ok "session Wayland ($WAYLAND_DISPLAY)"
else
	bad "pas de session Wayland — WAYLAND_DISPLAY est vide"
fi
[ -z "${DISPLAY:-}" ] && note "aucun serveur X : Xwayland n'a pas été réveillé" \
	|| note "Xwayland actif ($DISPLAY) — un client X11 tourne quelque part"

# Les six binaires du shell doivent être installés, pas seulement compilés.
MANQUE=""
for b in claude-os-fond claude-os-dock claude-os-status \
         claude-os-lanceur claude-os-fichiers claude-os-reglages; do
	command -v "$b" >/dev/null 2>&1 || MANQUE="$MANQUE $b"
done
[ -z "$MANQUE" ] && ok "les six binaires du shell sont installés" \
	|| bad "binaires manquants :$MANQUE"

[ -f "$HOME/.config/claude-os/shell.conf" ] \
	&& ok "configuration du shell présente" \
	|| warn "~/.config/claude-os/shell.conf absent : les défauts s'appliquent"

# L'ancienne interface ne doit plus traîner nulle part.
RESTES=""
for p in openbox plank tint2 picom rofi pcmanfm xcape; do
	dpkg -l "$p" 2>/dev/null | grep -q "^ii" && RESTES="$RESTES $p"
done
[ -f /usr/share/xsessions/claude-os.desktop ] && RESTES="$RESTES session-X11"
[ -z "$RESTES" ] && ok "aucun reste de l'ancienne interface X11" \
	|| warn "restes à purger :$RESTES"

USED="$(free -m | awk '/^Mem:/{print $3}')"
if [ -n "$USED" ]; then
	if [ "$USED" -lt 450 ]; then ok "empreinte mémoire : ${USED} Mo (cible < 400)"
	elif [ "$USED" -lt 700 ]; then warn "empreinte mémoire : ${USED} Mo, au-dessus de la cible"
	else bad "empreinte mémoire : ${USED} Mo — bien au-delà de la cible"
	fi
	raw "$(free -h)"
fi

# ------------------------------------------------------------------ entrées
sec "8. Entrées"

grep -qi 'touchscreen\|Touchscreen' /proc/bus/input/devices 2>/dev/null \
	&& ok "écran tactile détecté" || warn "aucun écran tactile détecté"
grep -qi touchpad /proc/bus/input/devices 2>/dev/null \
	&& ok "pavé tactile détecté" || warn "pavé tactile non identifié"
if [ -d /sys/bus/iio/devices ] && [ -n "$(ls -A /sys/bus/iio/devices 2>/dev/null)" ]; then
	ok "capteur IIO présent (rotation automatique envisageable)"
else
	note "aucun capteur IIO : pas de rotation automatique"
fi

# ---------------------------------------------------------------- firmware
sec "9. Firmware manquant"
MISS="$(dmesg 2>/dev/null | grep -i 'firmware.*fail\|Direct firmware load.*failed' | tail -12)"
if [ -z "$MISS" ]; then
	ok "aucun échec de chargement de firmware signalé"
else
	warn "le noyau signale des firmwares manquants"
	raw "$MISS"
fi

# ------------------------------------------------------------------ verdict
sec "10. Verdict"
w "| | |"
w "|---|---|"
w "| Conforme | $PASS |"
w "| Réserves | $WARN |"
w "| Échecs | $FAIL |"

echo
printf '  \033[32m%d conformes\033[0m · \033[33m%d réserves\033[0m · \033[31m%d échecs\033[0m\n' "$PASS" "$WARN" "$FAIL"
exec 3>&-
echo
echo "  Rapport : $OUT"
echo "  Le transmettre tel quel."
[ "$FAIL" -eq 0 ]
