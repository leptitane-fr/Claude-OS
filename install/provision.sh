#!/usr/bin/env bash
#
# Claude OS — Provisionnement
#
# Transforme une installation Debian 13 minimale (sans environnement de bureau)
# en Claude OS. Idempotent : relançable sans dommage.
#
# Pourquoi provisionner plutôt que construire une image ISO : le résultat est
# testable par étapes, réparable en place, et l'on n'a pas à maintenir un
# constructeur d'image. L'installation se fait donc avec le netinst Debian
# officiel (en décochant tout environnement de bureau), puis ce script.
#
# USAGE
#   sudo bash install/provision.sh              # installe
#   sudo bash install/provision.sh --dry-run    # montre sans rien faire
#   sudo bash install/provision.sh --no-claude  # sans Claude Desktop
#
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DRY=0
WITH_CLAUDE=1

while [ $# -gt 0 ]; do
	case "$1" in
		--dry-run)   DRY=1 ;;
		--no-claude) WITH_CLAUDE=0 ;;
		-h|--help)   sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "Option inconnue : $1" >&2; exit 2 ;;
	esac
	shift
done

STEP=0
say()  { STEP=$((STEP+1)); printf '\n\033[1;34m[%d]\033[0m \033[1m%s\033[0m\n' "$STEP" "$*"; }
info() { printf '      %s\n' "$*"; }
warn() { printf '      \033[33m! %s\033[0m\n' "$*"; }
die()  { printf '\n\033[31mÉCHEC : %s\033[0m\n' "$*" >&2; exit 1; }

run() {
	if [ "$DRY" -eq 1 ]; then printf '      \033[2m$ %s\033[0m\n' "$*"; else eval "$@"; fi
}

# ---------------------------------------------------------------- préalables

say "Vérification des préalables"

[ "$(id -u)" -eq 0 ] || die "à lancer avec sudo."

if [ -r /etc/os-release ]; then
	. /etc/os-release
	info "système : ${PRETTY_NAME:-inconnu}"
	case "${VERSION_CODENAME:-}" in
		trixie) ;;
		"")     warn "version Debian indéterminée — poursuite à vos risques" ;;
		*)      warn "attendu Debian 13 (trixie), trouvé « ${VERSION_CODENAME} »" ;;
	esac
fi

[ "$(dpkg --print-architecture)" = "amd64" ] || \
	die "architecture $(dpkg --print-architecture) : Claude Desktop n'existe qu'en amd64/arm64."

# L'utilisateur cible est celui qui a appelé sudo, pas root : c'est sa session
# graphique que l'on configure.
TARGET_USER="${SUDO_USER:-}"
[ -n "$TARGET_USER" ] && [ "$TARGET_USER" != "root" ] || \
	die "impossible d'identifier l'utilisateur cible. Lancer via « sudo » depuis un compte normal."
# « set -e » combiné à « pipefail » avorterait le script sans message si getent
# échouait dans la substitution : d'où le repli explicite, qui laisse le
# diagnostic ci-dessous s'afficher.
TARGET_HOME="$(getent passwd "$TARGET_USER" 2>/dev/null | cut -d: -f6)" || TARGET_HOME=""
[ -n "$TARGET_HOME" ] || die "compte « $TARGET_USER » inconnu du système."
[ -d "$TARGET_HOME" ] || die "répertoire personnel introuvable pour $TARGET_USER ($TARGET_HOME)"
info "utilisateur cible : $TARGET_USER ($TARGET_HOME)"

ping -c1 -W3 deb.debian.org >/dev/null 2>&1 || warn "deb.debian.org injoignable — l'installation va probablement échouer"

[ -f "$REPO_DIR/install/packages.list" ] || die "packages.list introuvable dans $REPO_DIR/install/"

# --------------------------------------------------------------- apt sobre

say "Configuration d'APT"

info "désactivation des paquets recommandés et suggérés"
run "cat > /etc/apt/apt.conf.d/99claude-os-minimal <<'EOF'
// Claude OS : rien n'est installé qui n'ait été demandé explicitement.
// C'est ce réglage qui fait la différence entre un système de 1,5 Go et un
// système de 4 Go.
APT::Install-Recommends \"false\";
APT::Install-Suggests \"false\";
APT::AutoRemove::RecommendsImportant \"false\";
APT::AutoRemove::SuggestsImportant \"false\";
EOF"

run "apt-get update -qq"

# ------------------------------------------------------------- paquets base

say "Installation des paquets"

PKGS="$(grep -vE '^\s*(#|$)' "$REPO_DIR/install/packages.list" | sed 's/#.*//' | tr -d ' \t' | tr '\n' ' ')"
info "$(echo "$PKGS" | wc -w) paquets"
run "DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $PKGS"

# --------------------------------------------------------- Claude Desktop

if [ "$WITH_CLAUDE" -eq 1 ]; then
	say "Claude Desktop"

	KEYRING=/usr/share/keyrings/claude-desktop-archive-keyring.asc
	FPR_ATTENDUE="31DDDE24DDFAB679F42D7BD2BAA929FF1A7ECACE"

	run "apt-get install -y --no-install-recommends curl gnupg"

	info "téléchargement de la clé de signature Anthropic"
	run "curl -fsSLo $KEYRING https://downloads.claude.ai/claude-desktop/key.asc"

	# Vérification de l'empreinte : sans elle, on ferait confiance à ce que le
	# réseau a bien voulu renvoyer.
	if [ "$DRY" -eq 0 ]; then
		FPR="$(gpg --show-keys --with-colons "$KEYRING" 2>/dev/null | awk -F: '/^fpr:/{print $10; exit}')"
		if [ "$FPR" != "$FPR_ATTENDUE" ]; then
			rm -f "$KEYRING"
			die "empreinte GPG inattendue.
      attendue : $FPR_ATTENDUE
      obtenue  : ${FPR:-<aucune>}
      La clé a été supprimée. Ne pas poursuivre sans comprendre pourquoi."
		fi
		info "empreinte vérifiée : $FPR"
	fi

	run "echo 'deb [arch=amd64,arm64 signed-by=$KEYRING] https://downloads.claude.ai/claude-desktop/apt/stable stable main' > /etc/apt/sources.list.d/claude-desktop.list"
	run "apt-get update -qq"

	# --no-install-recommends écarte volontairement QEMU/OVMF/virtiofsd, qui ne
	# servent qu'à Cowork. Cowork lance une machine virtuelle : sur 4 Go de RAM,
	# à côté d'une application Electron, ce n'est pas exploitable. Plusieurs
	# centaines de Mo de disque économisés sur un eMMC déjà petit.
	run "DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends claude-desktop"
	info "Cowork non installé (QEMU/KVM volontairement écartés — voir docs/04)"
fi

# ---------------------------------------------------------------- réseau

say "Bascule du Wi-Fi vers NetworkManager"

# L'installateur Debian configure le Wi-Fi dans /etc/network/interfaces, géré
# par ifupdown. NetworkManager, qui vient d'être installé, ignore par défaut
# les interfaces qui y figurent : sans intervention, plus personne ne gère la
# carte après redémarrage et la machine se retrouve sans réseau — juste au
# moment où l'on en a le plus besoin.
#
# On reprend donc les identifiants pour en faire un profil NetworkManager,
# PUIS seulement on neutralise la strophe ifupdown. Si les identifiants sont
# introuvables, on ne touche à rien : deux gestionnaires qui coexistent valent
# mieux qu'une machine muette.

IFACES=/etc/network/interfaces
if [ -f "$IFACES" ] && grep -qE '^[[:space:]]*(auto|allow-hotplug|iface)[[:space:]]+wl' "$IFACES"; then
	SSID="$(awk '/wpa-ssid/{$1=""; sub(/^ /,""); print; exit}' "$IFACES" 2>/dev/null | tr -d '"')"
	PSK="$(awk  '/wpa-psk/ {$1=""; sub(/^ /,""); print; exit}' "$IFACES" 2>/dev/null | tr -d '"')"

	if [ -n "$SSID" ] && [ -n "$PSK" ]; then
		info "réseau « $SSID » repris depuis ifupdown"
		run "nmcli connection add type wifi con-name '$SSID' ssid '$SSID' wifi-sec.key-mgmt wpa-psk wifi-sec.psk '$PSK' connection.autoconnect yes >/dev/null 2>&1 || true"

		info "neutralisation de la strophe ifupdown (sauvegarde conservée)"
		run "cp -a '$IFACES' '$IFACES.avant-claude-os'"
		run "awk '/^[[:space:]]*(auto|allow-hotplug)[[:space:]]+wl/ { print \"# \" \$0; next } /^[[:space:]]*iface[[:space:]]+wl/ { blk=1; print \"# \" \$0; next } blk && /^[[:space:]]+[^[:space:]]/ { print \"# \" \$0; next } blk && /^[^[:space:]#]/ { blk=0 } { print }' '$IFACES.avant-claude-os' > '$IFACES'"
	else
		warn "identifiants Wi-Fi introuvables dans $IFACES"
		info "ifupdown reste en place : le réseau continuera de fonctionner, mais"
		info "l'icône Wi-Fi de la barre d'état ne le pilotera pas. Pour basculer"
		info "plus tard, commenter la strophe « wl » puis se reconnecter par l'icône."
	fi
else
	info "aucune configuration Wi-Fi ifupdown — NetworkManager gère seul"
fi

# ------------------------------------------------------- fichiers du système

say "Déploiement de l'environnement"

info "copie de rootfs/ vers /"
run "cp -a '$REPO_DIR/rootfs/.' /"
run "chmod +x /usr/local/bin/claude-os-session"

info "enregistrement de la session auprès de LightDM"
run "mkdir -p /usr/share/xsessions"
run "cat > /usr/share/xsessions/claude-os.desktop <<'EOF'
[Desktop Entry]
Name=Claude OS
Comment=Environnement Claude OS
Exec=/usr/local/bin/claude-os-session
Type=Application
DesktopNames=ClaudeOS
EOF"

run "install -o '$TARGET_USER' -g '$TARGET_USER' -m 0644 /dev/null '$TARGET_HOME/.xsession'"
run "printf '#!/bin/sh\nexec /usr/local/bin/claude-os-session\n' > '$TARGET_HOME/.xsession'"
run "chmod +x '$TARGET_HOME/.xsession'"
run "chown '$TARGET_USER:$TARGET_USER' '$TARGET_HOME/.xsession'"

info "dock plank : réglages et icônes épinglées"
# Ces fichiers appartiennent à l'utilisateur : plank les réécrit lui-même dès
# qu'on réorganise le dock à la souris. On ne les écrase donc que s'ils
# n'existent pas déjà, sous peine de perdre l'ordre choisi à chaque
# relancement du provisionnement.
run "mkdir -p '$TARGET_HOME/.config/plank/dock1/launchers'"
if [ "$DRY" -eq 1 ] || [ ! -f "$TARGET_HOME/.config/plank/dock1/settings" ]; then
	run "cp '$REPO_DIR/rootfs/usr/share/claude-os/plank/settings' '$TARGET_HOME/.config/plank/dock1/settings'"
	run "cp '$REPO_DIR/rootfs/usr/share/claude-os/plank/launchers/'*.dockitem '$TARGET_HOME/.config/plank/dock1/launchers/'"
else
	info "réglages plank déjà présents — conservés (ordre des icônes préservé)"
fi
run "chown -R '$TARGET_USER:$TARGET_USER' '$TARGET_HOME/.config/plank'"

info "thème GTK sombre"
run "mkdir -p '$TARGET_HOME/.config/gtk-3.0'"
run "cat > '$TARGET_HOME/.config/gtk-3.0/settings.ini' <<'EOF'
[Settings]
gtk-theme-name=Adwaita-dark
gtk-icon-theme-name=Papirus-Dark
gtk-font-name=Inter 10
gtk-application-prefer-dark-theme=1
gtk-cursor-theme-name=Adwaita
gtk-enable-animations=1
EOF"
run "chown -R '$TARGET_USER:$TARGET_USER' '$TARGET_HOME/.config'"

# -------------------------------------------------------------------- énergie

say "Gestion d'énergie"

info "activation de TLP"
run "systemctl enable tlp.service >/dev/null 2>&1 || true"
# TLP et rfkill de systemd se disputent la gestion radio : la documentation TLP
# demande de masquer ces deux unités.
run "systemctl mask systemd-rfkill.service systemd-rfkill.socket >/dev/null 2>&1 || true"

info "paramètres noyau (compression du tampon d'affichage, veille s2idle)"
if [ "$DRY" -eq 0 ] && command -v update-grub >/dev/null 2>&1; then
	run "update-grub >/dev/null 2>&1 || true"
else
	info "update-grub à lancer manuellement si absent ici"
fi

# ------------------------------------------------------------------- mémoire

say "Réglages mémoire (4 Go)"

info "zram : swap compressé en RAM, moitié de la mémoire physique"
run "cat > /etc/systemd/zram-generator.conf <<'EOF'
# Sur eMMC lent, comprimer en mémoire vaut toujours mieux qu'écrire sur disque.
[zram0]
zram-size = ram / 2
compression-algorithm = zstd
swap-priority = 100
fs-type = swap
EOF"

run "cat > /etc/sysctl.d/99-claude-os.conf <<'EOF'
# Réglages adaptés à un swap zram : on échange volontiers vers la RAM
# compressée, ce qui n'a rien à voir avec un swap sur disque.
vm.swappiness = 180
vm.watermark_boost_factor = 0
vm.watermark_scale_factor = 125
vm.page-cluster = 0
EOF"
run "sysctl --system >/dev/null 2>&1 || true"

# ---------------------------------------------------------------- Chromium

say "Chromium"

# Le décodage matériel décide de la fluidité et de l'autonomie sur un CPU 6 W.
# À valider après installation avec chrome://gpu et vainfo : ces indicateurs
# évoluent d'une version de Chromium à l'autre.
run "mkdir -p /etc/chromium.d"
run "cat > /etc/chromium.d/99-claude-os <<'EOF'
# Décodage vidéo matériel (VA-API). Vérifier chrome://gpu après installation :
# « Video Decode » doit indiquer « Hardware accelerated ».
export CHROMIUM_FLAGS=\"\${CHROMIUM_FLAGS} --enable-features=VaapiVideoDecodeLinuxGL,VaapiVideoDecoder\"
export CHROMIUM_FLAGS=\"\${CHROMIUM_FLAGS} --ozone-platform=x11\"
# Rendu plus fluide des listes et du défilement sur GPU intégré modeste
export CHROMIUM_FLAGS=\"\${CHROMIUM_FLAGS} --enable-gpu-rasterization --enable-zero-copy\"
EOF"
info "AV1 : Jasper Lake ne le décode PAS en matériel (voir docs/02)."
info "      Mitigation à appliquer côté navigateur — voir docs/04."

# ------------------------------------------------------------------ services

say "Retrait des fonctions inutiles"

# La tâche « Utilitaires usuels du système » de l'installateur Debian tire un
# agent de transport de courrier. Sur une machine orientée web, c'est un démon
# résident qui n'enverra jamais rien : autant le retirer.
for pkg in exim4-daemon-light exim4-base exim4-config; do
	if dpkg -l "$pkg" 2>/dev/null | grep -q "^ii"; then
		info "retrait de $pkg (agent de courrier inutile ici)"
		run "DEBIAN_FRONTEND=noninteractive apt-get purge -y $pkg >/dev/null 2>&1 || true"
	fi
done

for svc in ModemManager.service; do
	if systemctl list-unit-files 2>/dev/null | grep -q "^$svc"; then
		info "désactivation de $svc"
		run "systemctl disable --now $svc >/dev/null 2>&1 || true"
		run "systemctl mask $svc >/dev/null 2>&1 || true"
	fi
done

# Trois familles de firmware Wi-Fi sont embarquées faute de certitude sur le
# module. Une fois la machine démarrée, celles qui ne servent pas peuvent
# partir : c'est quelques dizaines de Mo sur un eMMC déjà petit.
info "firmware Wi-Fi : purger les familles inutilisées après validation"
info "  lspci -nnk | grep -A3 -i network    puis  apt purge firmware-<inutile>"

info "nettoyage des paquets orphelins"
run "apt-get autoremove -y --purge >/dev/null 2>&1 || true"
run "apt-get clean"

# -------------------------------------------------------------------- bilan

say "Terminé"

if [ "$DRY" -eq 1 ]; then
	echo
	info "Simulation : aucune modification n'a été faite."
	exit 0
fi

echo
info "Redémarrer, puis se connecter à la session « Claude OS »."
echo
info "À essayer une fois la session ouverte :"
info "  touche Loupe                      masque / affiche dock et barre d'état"
info "  Super + Espace                    lanceur d'applications"
info "  clic droit sur le bureau          menu, dont « Réglages d'affichage »"
info "  glisser une icône du dock         réorganisation, enregistrée par plank"
echo
info "Vérifications à faire à la première ouverture de session :"
info "  vainfo | head -5                  décodage vidéo matériel"
info "  nmcli device wifi list            Wi-Fi"
info "  bluetoothctl show                 Bluetooth"
info "  aplay -l && wpctl status          audio (haut-parleurs internes !)"
info "  free -h                           empreinte mémoire au repos"
info "  tlp-stat -s -c                    gestion d'énergie active"
info "  powertop --auto-tune=false        consommation par poste"
info "  bash tools/probe-keys.sh          codes des touches Chromebook"
echo
warn "L'audio est le point de risque n°1 sur cette machine : casque"
warn "fonctionnel mais haut-parleurs muets est le symptôme classique."
