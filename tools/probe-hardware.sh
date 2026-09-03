#!/usr/bin/env bash
#
# Claude OS - Relevé matériel
#
# Objectif : produire un rapport Markdown complet et reproductible du matériel
# de la machine cible (HP Chromebook x360 14b-cb0000sf, board MADOO / Jasper Lake),
# afin de figer les faits avant de dimensionner le noyau, les pilotes et l'image.
#
# S'exécute dans deux contextes :
#   1. Depuis ChromeOS en mode développeur  (crosh -> shell)  AVANT effacement.
#      C'est le SEUL moment où l'on peut lire le HWID, l'état du write-protect
#      et la version du CR50. Ne pas sauter cette étape.
#   2. Depuis un live USB Debian APRÈS flash du firmware UEFI, pour constater
#      ce que le noyau mainline détecte réellement.
#
# Usage :
#   bash probe-hardware.sh                 # écrit ./hardware-report-<ctx>-<date>.md
#   sudo bash probe-hardware.sh            # recommandé : bien plus complet
#   bash probe-hardware.sh -o rapport.md   # chemin de sortie explicite
#
# Le script ne modifie jamais le système : lectures seules uniquement.

set -uo pipefail

VERSION="0.2.0"
OUT=""

# Attentes à confronter au matériel réel. Un écart ici invalide la cible
# firmware : MADOO (Jasper Lake) et BLOOGUARD (Gemini Lake) sont deux
# plateformes différentes derrière le même nom commercial « 14b ».
EXPECT_BOARD="MADOO"
EXPECT_CPU="N6000"
EXPECT_RAM_GB=4

while [ $# -gt 0 ]; do
	case "$1" in
		-o|--output) OUT="${2:-}"; shift 2 ;;
		-h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "Option inconnue : $1" >&2; exit 2 ;;
	esac
done

# ---------------------------------------------------------------- utilitaires

have() { command -v "$1" >/dev/null 2>&1; }

# Contexte d'exécution : chromeos | linux
detect_context() {
	if [ -f /etc/lsb-release ] && grep -qi 'chromeos' /etc/lsb-release 2>/dev/null; then
		echo "chromeos"
	elif have crossystem; then
		echo "chromeos"
	else
		echo "linux"
	fi
}

CTX="$(detect_context)"
DATE_TAG="$(date +%Y%m%d-%H%M%S)"
[ -n "$OUT" ] || OUT="hardware-report-${CTX}-${DATE_TAG}.md"

# Tout ce qui suit est écrit dans le rapport ET résumé sur stdout.
exec 3>"$OUT" || { echo "Impossible d'écrire $OUT" >&2; exit 1; }
w() { printf '%s\n' "$*" >&3; }

# section <titre>
section() { w ""; w "## $1"; w ""; }

# note <texte>
note() { w "> $*"; w ""; }

# cap <label> <commande...>
# Exécute une commande et capture sa sortie dans un bloc de code.
# N'échoue jamais : une commande absente ou en erreur est signalée explicitement,
# ce qui est une information en soi (ex. flashrom absent = contexte non-ChromeOS).
cap() {
	local label="$1"; shift
	local bin="$1"
	local out rc
	w "### $label"
	w ""
	if ! have "$bin"; then
		w '```'
		w "[absent] la commande « $bin » n'existe pas dans ce contexte"
		w '```'
		w ""
		return 0
	fi
	out="$("$@" 2>&1)"; rc=$?
	w '```'
	if [ -n "$out" ]; then
		printf '%s\n' "$out" >&3
	else
		w "[vide] (code retour $rc)"
	fi
	[ "$rc" -ne 0 ] && w "[code retour non nul : $rc]"
	w '```'
	w ""
}

# capfile <label> <chemin>
capfile() {
	local label="$1" path="$2"
	w "### $label"
	w ""
	w '```'
	if [ -r "$path" ]; then
		cat "$path" >&3 2>&1 || w "[erreur de lecture]"
	else
		w "[illisible ou absent] $path"
	fi
	w '```'
	w ""
}

# ------------------------------------------------- vérification des attentes

# Confronte le matériel réel aux attentes du projet et rend un verdict lisible,
# plutôt que de laisser l'utilisateur comparer des chaînes à l'œil.
check_expectations() {
	local hwid board ram_kb ram_gb cpu

	# HWID : via crossystem sous ChromeOS, via le VPD sinon.
	hwid=""
	if have crossystem; then
		hwid="$(crossystem hwid 2>/dev/null)"
	fi
	if [ -z "$hwid" ] && [ -r /sys/firmware/vpd/ro/hwid ]; then
		hwid="$(cat /sys/firmware/vpd/ro/hwid 2>/dev/null)"
	fi

	if [ -n "$hwid" ]; then
		# Format « MADOO-XXXX A6B-C7D-E8F » ou « MADOO A6B-C7D » :
		# le board est le premier mot, avant un éventuel tiret.
		board="${hwid%% *}"
		board="${board%%-*}"
		board="$(printf '%s' "$board" | tr '[:lower:]' '[:upper:]')"
		if [ "$board" = "$EXPECT_BOARD" ]; then
			echo "[OK]     board = $board, conforme à l'attendu"
		else
			echo "[ALERTE] board = $board mais $EXPECT_BOARD était attendu."
			echo "         La cible firmware du projet est FAUSSE pour cette machine."
			echo "         NE PAS FLASHER avant d'avoir revu docs/01."
		fi
		echo "         HWID complet : $hwid"
	else
		echo "[?]      HWID illisible ici — normal hors ChromeOS et sans VPD exposé."
		echo "         Vérifier autrement : chrome://version, ligne « Platform »."
	fi

	# Processeur
	cpu="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//')"
	if [ -n "$cpu" ]; then
		case "$cpu" in
			*"$EXPECT_CPU"*) echo "[OK]     CPU = $cpu" ;;
			*) echo "[!]      CPU = $cpu, or $EXPECT_CPU était attendu — à confirmer" ;;
		esac
	fi

	# Mémoire : MemTotal est toujours inférieur à la capacité physique
	# (le firmware en réserve), d'où l'arrondi au Go le plus proche.
	ram_kb="$(awk '/^MemTotal/{print $2}' /proc/meminfo 2>/dev/null)"
	if [ -n "$ram_kb" ]; then
		ram_gb=$(( (ram_kb + 524288) / 1048576 ))
		if [ "$ram_gb" -eq "$EXPECT_RAM_GB" ]; then
			echo "[OK]     RAM = ${ram_gb} Go (MemTotal ${ram_kb} kB)"
		else
			echo "[!]      RAM = ${ram_gb} Go, or ${EXPECT_RAM_GB} Go étaient attendus."
			echo "         Le budget mémoire de docs/02 est à revoir."
		fi
	fi
}

EXPECT_OUT="$(check_expectations)"

# ------------------------------------------------------------------- en-tête

IS_ROOT=0
[ "$(id -u)" -eq 0 ] && IS_ROOT=1

w "# Relevé matériel — Claude OS"
w ""
w "| | |"
w "|---|---|"
w "| Script | \`tools/probe-hardware.sh\` v${VERSION} |"
w "| Date | $(date -Is 2>/dev/null || date) |"
w "| Contexte | \`${CTX}\` |"
w "| Privilèges | $( [ "$IS_ROOT" -eq 1 ] && echo 'root' || echo 'utilisateur non privilégié (rapport partiel)' ) |"
w "| Noyau | $(uname -srmo 2>/dev/null || uname -a) |"
w ""

section "0. Vérification des attentes"

note "Confrontation immédiate du matériel réel aux hypothèses du projet. Une alerte ici invalide la suite."

w '```'
printf '%s\n' "$EXPECT_OUT" >&3
w '```'
w ""

if [ "$IS_ROOT" -eq 0 ]; then
	note "**Rapport partiel.** Plusieurs relevés déterminants (write-protect, CR50, DMI, EDID) exigent root. Relancer avec \`sudo\` pour un rapport exploitable."
fi

# --------------------------------------------------------- identité machine

section "1. Identité de la machine"

note "Le *board name* conditionne tout : c'est lui, et non le nom commercial, qui détermine le firmware MrChromebox applicable. Attendu ici : \`MADOO\` (Jasper Lake)."

if [ "$CTX" = "chromeos" ]; then
	cap "HWID (identifiant matériel complet)" crossystem hwid
	cap "Board ChromeOS" bash -c 'grep -E "CHROMEOS_RELEASE_BOARD|CHROMEOS_RELEASE_CHROME_MILESTONE" /etc/lsb-release'
	cap "Plateforme (mosys)" mosys platform name
	cap "Modèle (mosys)" mosys platform model
fi

cap "DMI — fabricant / produit / version" bash -c '
for f in sys_vendor product_name product_version board_name board_vendor bios_vendor bios_version bios_date; do
	p="/sys/class/dmi/id/$f"
	[ -r "$p" ] && printf "%-16s %s\n" "$f:" "$(cat "$p")"
done'

cap "Résumé DMI (dmidecode)" dmidecode -t system -t baseboard -t bios

# ------------------------------------------------------ firmware & sécurité

section "2. Firmware et write-protect"

note "**Étape critique et irréversible en pratique.** Le flash d'un firmware UEFI complet exige que le write-protect matériel soit désactivé. Sur MADOO (Jasper Lake / famille Dedede) la protection est pilotée par la puce de sécurité **CR50**, et non par une vis : elle se lève via CCD (Closed Case Debugging) ou par déconnexion de la batterie. Relever ici l'état exact AVANT toute manipulation."

if [ "$CTX" = "chromeos" ]; then
	cap "crossystem (état complet)" crossystem
	cap "Write-protect matériel (wpsw_cur : 1 = actif, 0 = levé)" crossystem wpsw_cur
	cap "Version CR50 / GSC" gsctool -a -f
	cap "État CCD (Closed Case Debugging)" gsctool -a -I
	cap "Write-protect SPI vu par flashrom" flashrom -p host --wp-status
	cap "Taille et identification de la puce SPI" flashrom -p host --flash-name
fi

cap "Mode de démarrage (UEFI si le répertoire existe)" bash -c '
if [ -d /sys/firmware/efi ]; then
	echo "UEFI — firmware déjà remplacé, ou live USB démarré en UEFI"
	echo "Variables EFI : $( [ -d /sys/firmware/efi/efivars ] && echo disponibles || echo absentes )"
else
	echo "Pas de /sys/firmware/efi -> démarrage hérité / firmware ChromeOS d origine"
fi'

cap "Secure Boot" bash -c 'mokutil --sb-state 2>/dev/null || echo "[mokutil absent]"'

# ------------------------------------------------------------------- CPU

section "3. Processeur"

note "Attendu : Intel Jasper Lake (Celeron N4500 / N5100 ou Pentium Silver N6000). Le nombre de cœurs et la présence d'AVX2 orientent le choix des optimisations de compilation."

cap "Modèle et cœurs" bash -c '
grep -m1 "model name" /proc/cpuinfo
echo "cœurs logiques : $(grep -c ^processor /proc/cpuinfo)"
grep -m1 flags /proc/cpuinfo | tr " " "\n" | grep -xE "avx|avx2|sse4_2|aes|sha_ni" | paste -sd" " -'
cap "Topologie (lscpu)" lscpu
cap "Microcode" bash -c 'grep -m1 microcode /proc/cpuinfo'
cap "Gouverneurs de fréquence disponibles" bash -c '
d=/sys/devices/system/cpu/cpu0/cpufreq
if [ -d "$d" ]; then
	echo "pilote  : $(cat "$d/scaling_driver" 2>/dev/null)"
	echo "gouv.   : $(cat "$d/scaling_available_governors" 2>/dev/null)"
	echo "actuel  : $(cat "$d/scaling_governor" 2>/dev/null)"
else
	echo "[cpufreq non exposé]"
fi'

# ---------------------------------------------------------------- mémoire

section "4. Mémoire"

note "Contrainte dimensionnante n°1. Claude Desktop est une application Electron : compter 500 Mo à 1 Go de RSS en usage réel. Sur une machine à 4 Go, le reste du système doit tenir sous ~400 Mo au repos."

cap "Mémoire totale et disponible" bash -c 'free -h 2>/dev/null || grep -E "MemTotal|MemAvailable|SwapTotal" /proc/meminfo'
cap "Détail /proc/meminfo (extrait)" bash -c 'grep -E "^(MemTotal|MemFree|MemAvailable|Cached|SwapTotal|SwapFree)" /proc/meminfo'
cap "Barrettes / mémoire soudée (dmidecode)" dmidecode -t memory
cap "zram déjà actif ?" bash -c 'command -v zramctl >/dev/null && zramctl || echo "[zramctl absent ou aucun périphérique zram]"'

# --------------------------------------------------------------- stockage

section "5. Stockage"

note "Attendu : eMMC 64 Go. Faible capacité + faibles IOPS : cela justifie btrfs + compression zstd (gain d'espace et de lecture) et proscrit toute distribution volumineuse."

cap "Périphériques bloc" bash -c 'lsblk -o NAME,SIZE,TYPE,TRAN,ROTA,MODEL,MOUNTPOINT 2>/dev/null || lsblk'
cap "Table de partitions" bash -c 'for d in /dev/mmcblk[0-9] /dev/nvme[0-9]n[0-9] /dev/sd[a-z]; do [ -b "$d" ] && { echo "== $d"; fdisk -l "$d" 2>/dev/null; }; done'
cap "Occupation des systèmes de fichiers" df -hT
cap "Détail eMMC (sysfs)" bash -c '
for d in /sys/class/mmc_host/*/mmc*:*/; do
	[ -d "$d" ] || continue
	echo "== $d"
	for f in name type manfid oemid fwrev hwrev; do
		[ -r "$d$f" ] && printf "  %-8s %s\n" "$f" "$(cat "$d$f")"
	done
done'

# ------------------------------------------------------------ bus PCI/USB

section "6. Bus PCI et USB"

cap "PCI (avec identifiants numériques)" lspci -nnk
cap "USB" lsusb
cap "Périphériques I2C (tactile, capteurs)" bash -c 'ls -l /sys/bus/i2c/devices/ 2>/dev/null || echo "[aucun]"'

# ------------------------------------------------------------------ audio

section "7. Audio"

note "**Point de risque n°1 sous Linux mainline.** Les Chromebooks Jasper Lake utilisent SOF (Sound Open Firmware) avec un codec discret (souvent RT5682) et des amplificateurs de haut-parleurs (MAX98357A/MAX98360A) pilotés séparément. Sans le bon firmware SOF *et* le bon profil UCM, les haut-parleurs internes restent muets alors que le casque fonctionne. Ce relevé sert à identifier précisément la chaîne audio."

cap "Cartes son ALSA" bash -c 'cat /proc/asound/cards 2>/dev/null || echo "[/proc/asound absent]"'
cap "Codecs et topologie SOF" bash -c '
for f in /proc/asound/card*/codec* /proc/asound/card*/id; do
	[ -r "$f" ] && { echo "== $f"; head -30 "$f"; }
done'
cap "Modules noyau audio chargés" bash -c 'lsmod 2>/dev/null | grep -E "^(snd|sof)" || echo "[aucun module snd/sof]"'
cap "Messages noyau SOF / ALSA" bash -c 'dmesg 2>/dev/null | grep -iE "sof|soundwire|rt5682|max98|acp|hdaudio" | tail -40 || echo "[dmesg inaccessible sans root]"'
cap "Profils UCM présents" bash -c 'ls /usr/share/alsa/ucm2/ 2>/dev/null | head -40 || echo "[ucm2 absent]"'

# ----------------------------------------------------------------- entrées

section "8. Périphériques d'entrée"

note "Machine convertible : clavier, pavé tactile, écran tactile et éventuellement stylet. Vérifier que chacun est détecté, et relever l'orientation de l'accéléromètre pour la rotation automatique."

capfile "/proc/bus/input/devices" /proc/bus/input/devices
cap "libinput" bash -c 'libinput list-devices 2>/dev/null || echo "[libinput absent]"'
cap "Capteurs IIO (accéléromètre pour rotation auto)" bash -c '
if [ -d /sys/bus/iio/devices ] && [ -n "$(ls -A /sys/bus/iio/devices 2>/dev/null)" ]; then
	for d in /sys/bus/iio/devices/*/; do
		echo "== $d"
		[ -r "$d/name" ] && echo "  name: $(cat "$d/name")"
	done
else
	echo "[aucun capteur IIO exposé]"
fi'

# ------------------------------------------------------------------ écran

section "9. Écran et GPU"

cap "Connecteurs DRM et état" bash -c '
for c in /sys/class/drm/card*/; do
	[ -r "$c/status" ] || continue
	echo "$(basename "$c") : $(cat "$c/status") | modes: $(head -1 "$c/modes" 2>/dev/null)"
done'
cap "Pilote graphique et rendu" bash -c 'lspci -nnk 2>/dev/null | grep -A3 -iE "vga|display|3d" || echo "[lspci absent]"'
cap "Rétroéclairage" bash -c 'ls /sys/class/backlight/ 2>/dev/null || echo "[aucun]"'

# ---------------------------------------------------------------- batterie

section "10. Alimentation et batterie"

cap "Batterie" bash -c '
for b in /sys/class/power_supply/*/; do
	echo "== $(basename "$b")"
	for f in type status capacity energy_full energy_full_design cycle_count manufacturer model_name; do
		[ -r "$b$f" ] && printf "  %-20s %s\n" "$f" "$(cat "$b$f")"
	done
done'
cap "Interface ACPI / EC" bash -c 'ls /sys/class/chromeos/ 2>/dev/null || echo "[pas de sysfs chromeos — EC non exposé]"'

# ----------------------------------------------------------------- réseau

section "11. Réseau"

note "Attendu : Wi-Fi Intel AX201 ou équivalent (firmware iwlwifi requis, non libre) sur Jasper Lake. Le firmware doit être présent dans l'image, sinon pas de réseau à la première installation."

cap "Interfaces" bash -c 'ip -br link 2>/dev/null || ifconfig -a 2>/dev/null || echo "[ni ip ni ifconfig]"'
cap "Contrôleurs réseau PCI/USB" bash -c 'lspci -nnk 2>/dev/null | grep -A3 -iE "network|ethernet|wireless" || echo "[lspci absent]"'
cap "Bluetooth" bash -c 'hciconfig -a 2>/dev/null || echo "[hciconfig absent]"'

# ------------------------------------------------------ noyau et firmware

section "12. Noyau, modules et firmware manquant"

note "Les lignes « firmware: failed to load » de dmesg listent exactement les blobs à embarquer dans l'image. C'est la liste de courses des paquets \`firmware-*\`."

cap "Version du noyau" bash -c 'uname -a; echo; cat /proc/version'
cap "Firmware manquant signalé par le noyau" bash -c 'dmesg 2>/dev/null | grep -iE "firmware|failed to load|direct firmware load" | tail -40 || echo "[dmesg inaccessible sans root]"'
cap "Erreurs noyau" bash -c 'dmesg --level=err,warn 2>/dev/null | tail -40 || echo "[dmesg inaccessible sans root]"'
cap "Modules chargés" bash -c 'lsmod 2>/dev/null | head -60 || echo "[lsmod absent]"'

# ------------------------------------------------------------------ résumé

section "13. Synthèse à vérifier"

w "À remplir/confirmer après lecture du rapport :"
w ""
w "- [ ] Board name confirmé = \`MADOO\` (sinon, le firmware MrChromebox ciblé est faux)"
w "- [ ] Quantité de RAM exacte (4 Go ou 8 Go) — détermine l'agressivité du budget mémoire"
w "- [ ] Capacité eMMC exacte"
w "- [ ] Modèle CPU exact"
w "- [ ] Chaîne audio identifiée (codec + amplis)"
w "- [ ] Contrôleur Wi-Fi identifié et firmware correspondant"
w "- [ ] État du write-protect relevé avant toute manipulation"
w "- [ ] Écran tactile et accéléromètre détectés"
w ""

exec 3>&-

# ------------------------------------------------------------- sortie stdout

echo "Rapport écrit : $OUT"
echo
echo "----- Synthèse rapide -----"
printf "%-14s %s\n" "Contexte"  "$CTX"
printf "%-14s %s\n" "Privilèges" "$( [ "$IS_ROOT" -eq 1 ] && echo root || echo 'non-root (rapport partiel)' )"
have crossystem && printf "%-14s %s\n" "HWID" "$(crossystem hwid 2>/dev/null || echo '?')"
have crossystem && printf "%-14s %s\n" "WP matériel" "$(crossystem wpsw_cur 2>/dev/null || echo '?') (1 = actif, 0 = levé)"
printf "%-14s %s\n" "CPU" "$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//')"
printf "%-14s %s\n" "RAM" "$(awk '/MemTotal/{printf "%.1f Go", $2/1048576}' /proc/meminfo 2>/dev/null)"
echo
echo "----- Vérification des attentes -----"
printf '%s\n' "$EXPECT_OUT"
echo
echo "Relire le rapport, puis le committer ou le coller dans la conversation."
