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
#   bash install/provision.sh --user stef       # en root direct, sans sudo
#
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DRY=0
WITH_CLAUDE=1
USER_OPT=""

while [ $# -gt 0 ]; do
	case "$1" in
		--dry-run)   DRY=1 ;;
		--no-claude) WITH_CLAUDE=0 ;;
		--user)      USER_OPT="${2:-}"; shift ;;
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

# Message explicite : « lancer avec sudo » induisait en erreur quand sudo
# n'est pas installé, ou quand un « su - » a échoué sans qu'on le remarque.
[ "$(id -u)" -eq 0 ] || die "ce script doit tourner en root (identité actuelle : $(id -un)).
      Avec sudo   :  sudo bash install/provision.sh
      Sans sudo   :  su -   puis   bash install/provision.sh --user $(id -un)
      Si « su - » a échoué plus haut, vous êtes resté sur votre compte."

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

# L'utilisateur cible est celui dont on configure la session graphique — pas
# root. Normalement celui qui a appelé sudo ; « --user » permet de s'en passer
# quand on travaille directement en root, ce qui arrive sur une Debian fraîche
# où sudo n'est pas installé (cas d'un mot de passe root défini à
# l'installation).
TARGET_USER="${USER_OPT:-${SUDO_USER:-}}"
[ -n "$TARGET_USER" ] && [ "$TARGET_USER" != "root" ] || die "utilisateur cible non identifié.
      Depuis un compte normal :  sudo bash install/provision.sh
      Depuis root, sans sudo  :  bash install/provision.sh --user <compte>"
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

# En un seul appel : c'est le plus rapide, et apt résout tout d'un coup.
#
# Mais un SEUL nom introuvable — un paquet renommé, retiré de la distribution —
# fait échouer l'installation entière, sans dire lequel. On reprend alors
# paquet par paquet : c'est plus lent, mais on obtient un système fourni et la
# liste exacte de ce qui manque.
if [ "$DRY" -eq 1 ]; then
	run "DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $PKGS"
elif DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $PKGS; then
	:
else
	warn "l'installation groupée a échoué — reprise paquet par paquet"
	ABSENTS=""
	for pkg in $PKGS; do
		DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
			"$pkg" >/dev/null 2>&1 || ABSENTS="$ABSENTS $pkg"
	done
	if [ -n "$ABSENTS" ]; then
		warn "paquets introuvables ou en échec :$ABSENTS"
		warn "le reste est installé ; me transmettre cette liste"
	fi
fi

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

# ------------------------------------------------- retrait de l'ancien bureau

say "Retrait de l'ancienne interface"

# DEUX PIÈGES, ET CE SONT LES PLUS SÉRIEUX DE CETTE MISE À JOUR.
#
# 1. L'essai du shell installait ses binaires dans /usr/local/bin, qui vient
#    AVANT /usr/bin dans le PATH. Laissés en place, ils masqueraient purement
#    et simplement la version qu'on vient d'installer : on croirait tourner
#    sur le neuf, on tournerait sur l'ancien.
#
# 2. Il écrivait aussi ~/.config/labwc/, que labwc préfère à /etc/xdg/labwc/.
#    Cette copie figerait la configuration du jour de l'essai, et aucune mise
#    à jour de rc.xml n'aurait plus le moindre effet.
#
# Les deux se retirent ici, avant d'installer quoi que ce soit.

for b in dock status fond lanceur reglages fichiers shell-basculer; do
	if [ -e "/usr/local/bin/claude-os-$b" ]; then
		info "retrait du binaire d'essai /usr/local/bin/claude-os-$b"
		run "rm -f '/usr/local/bin/claude-os-$b'"
	fi
done
run "rm -rf /usr/local/share/claude-os-shell"
run "rm -f /usr/local/share/applications/claude-os-reglages.desktop"
run "rm -f /usr/local/share/applications/claude-os-fichiers.desktop"

if [ -d "$TARGET_HOME/.config/labwc" ]; then
	info "retrait de ~/.config/labwc — la configuration système reprend la main"
	run "rm -rf '$TARGET_HOME/.config/labwc'"
fi

# L'arbre de travail de l'essai, et la référence que le dépôt en garde.
if [ -d "$TARGET_HOME/shell-essai" ]; then
	info "retrait de l'arbre d'essai ~/shell-essai"
	run "rm -rf '$TARGET_HOME/shell-essai'"
	# safe.directory : ce script tourne en root, le dépôt appartient à
	# l'utilisateur, et git refuse sinon d'y toucher.
	run "git -c safe.directory='$REPO_DIR' -C '$REPO_DIR' worktree prune >/dev/null 2>&1 || true"
fi

# --- l'interface X11, abandonnée -----------------------------------------
#
# Elle a été construite, installée, et n'a pas fonctionné (voir docs/02 §2.4).
# Ses fichiers ne servent plus à rien et ses paquets pèsent une centaine de
# mégaoctets sur un eMMC déjà petit.

# « session » n'est PAS dans cette liste : le nom est réutilisé par le
# lanceur de session Wayland, déployé quelques lignes plus bas. Le supprimer
# ici fonctionnerait par chance — le rootfs est copié après — mais compter sur
# l'ordre des étapes pour ne pas effacer un fichier qu'on vient d'écrire est
# le genre de fragilité qui se paie au premier remaniement.
for f in launcher settings toggle-shelf plank-setup; do
	run "rm -f '/usr/local/bin/claude-os-$f'"
done
run "rm -rf /usr/share/claude-os/openbox /usr/share/claude-os/picom"
run "rm -rf /usr/share/claude-os/plank  /usr/share/claude-os/rofi"
run "rm -rf /usr/share/claude-os/tint2"
run "rm -rf /usr/share/themes/ClaudeOS /usr/share/plank/themes/ClaudeOS"
run "rm -f /usr/local/share/applications/claude-os-chromium.desktop"
run "rm -f /usr/local/share/applications/claude-os-claude.desktop"
run "rm -f /usr/local/share/applications/claude-os-launcher.desktop"
run "rm -f /usr/local/share/applications/claude-os-notes.desktop"
run "rm -f /usr/local/share/applications/claude-os-settings.desktop"
run "rm -rf '$TARGET_HOME/.config/plank' '$TARGET_HOME/.config/pcmanfm'"

# Le serveur X et LightDM ne sont PAS purgés ici, et c'est délibéré : tant
# que greetd n'a pas fait ses preuves sur cette machine, ils sont le seul
# retour en arrière possible. Un écran de connexion qui refuse de s'afficher
# enferme dehors, et ce Chromebook n'a pas de touches F pour changer de
# terminal virtuel.
#
# Leur purge est le dernier geste, une fois greetd confirmé :
#   sudo apt purge lightdm lightdm-gtk-greeter xserver-xorg-core xinit \
#                  xserver-xorg-input-libinput x11-common
VIEUX="openbox plank tint2 picom rofi pcmanfm xcape xdotool dunst xwallpaper
       libnotify-bin python3-gi gir1.2-gtk-3.0 network-manager-gnome blueman
       x11-utils x11-xserver-utils gnome-terminal gnome-terminal-data"
A_PURGER=""
for pkg in $VIEUX; do
	dpkg -l "$pkg" 2>/dev/null | grep -q "^ii" && A_PURGER="$A_PURGER $pkg"
done
if [ -n "$A_PURGER" ]; then
	info "purge de l'ancienne pile :$A_PURGER"
	run "DEBIAN_FRONTEND=noninteractive apt-get purge -y $A_PURGER >/dev/null 2>&1 || true"
else
	info "aucun paquet de l'ancienne pile à retirer"
fi

# ---------------------------------------------------- compilation du shell

say "Compilation du shell"

# Le shell est compilé ici, sur la machine, plutôt que distribué en binaires.
# Une minute sur le N6000, et le dépôt reste du source.
#
# --prefix=/usr et non /usr/local : c'est un composant du système, au même
# titre que labwc. Aucun fichier géré par dpkg n'est écrasé, les noms sont
# les nôtres.
BUILD_DIR="$REPO_DIR/shell/build"
run "rm -rf '$BUILD_DIR'"
run "meson setup '$BUILD_DIR' '$REPO_DIR/shell' --prefix=/usr --buildtype=release >/dev/null" \
	|| die "meson setup a échoué. Détail :
      meson setup $BUILD_DIR $REPO_DIR/shell --prefix=/usr"
run "ninja -C '$BUILD_DIR'" || die "la compilation du shell a échoué."
run "meson install -C '$BUILD_DIR' >/dev/null" || die "l'installation du shell a échoué."
info "six binaires installés dans /usr/bin"

# ------------------------------------------------------- fichiers du système

say "Déploiement de l'environnement"

info "copie de rootfs/ vers /"
run "cp -a '$REPO_DIR/rootfs/.' /"
run "chmod +x /usr/local/bin/claude-os-claude /usr/local/bin/claude-os-shell-basculer /usr/local/bin/claude-os-session /usr/local/bin/claude-os-greeter"
run "chmod +x /etc/xdg/labwc/autostart /etc/xdg/labwc-greeter/autostart"

# La session est WAYLAND, et l'écran de connexion aussi. L'ancienne session
# X11 doit disparaître, sinon elle reste proposée à la connexion et un choix
# malheureux ramène une interface qui n'existe plus.
info "écran de connexion : greetd + claude-os-connexion"
run "rm -f /usr/share/xsessions/claude-os.desktop"
run "rm -f '$TARGET_HOME/.xsession'"

# Le compte à ouvrir. Le greeter sait le trouver seul — le seul UID entre
# 1000 et 60000 — mais l'écrire ici lève toute ambiguïté si un second compte
# apparaît un jour.
run "mkdir -p /etc/claude-os"
run "printf '%s\n' '$TARGET_USER' > /etc/claude-os/utilisateur"

# greetd n'affiche rien de lui-même : il lance un compositeur, qui lance
# notre champ de mot de passe. Le compte « _greetd » vient du paquet.
run "mkdir -p /etc/greetd"
run "cat > /etc/greetd/config.toml <<'EOF'
# Claude OS — ouverture de session.
#
# greetd ne dessine rien. Il lance labwc avec une configuration NUE — aucun
# raccourci clavier, aucun menu — qui lance claude-os-connexion. Une fois le
# mot de passe accepté, greetd remplace le tout par la session.

[terminal]
# Le premier terminal virtuel, celui qu'on voit au démarrage.
vt = 1

[default_session]
command = \"labwc -C /etc/xdg/labwc-greeter\"
user = \"_greetd\"
EOF"

# LA BASCULE DU GESTIONNAIRE DE SESSION.
#
# On désactive sans arrêter : couper LightDM maintenant fermerait la session
# en cours, celle depuis laquelle ce script tourne peut-être. La bascule
# prend effet au redémarrage.
#
# LightDM n'est PAS purgé à cette étape, et c'est délibéré. Un écran de
# connexion qui refuse de s'afficher enferme dehors — le clavier de ce
# Chromebook n'a pas de touches F pour changer de terminal virtuel, il ne
# resterait que SSH. Tant que greetd n'a pas fait ses preuves, le retour en
# arrière doit tenir en une commande :
#
#     sudo systemctl disable greetd && sudo systemctl enable lightdm
#
if systemctl list-unit-files 2>/dev/null | grep -q '^greetd\.service'; then
	info "activation de greetd, désactivation de LightDM"
	run "systemctl disable lightdm >/dev/null 2>&1 || true"
	run "systemctl enable greetd >/dev/null 2>&1"
else
	warn "greetd n'est pas installé : l'écran de connexion reste celui de LightDM"
fi

# Le fichier de configuration du shell. ÉCRIT UNE SEULE FOIS.
#
# Il appartient ensuite à l'utilisateur : le dock y enregistre l'ordre des
# icônes au glisser-déposer, le lanceur et le clic droit y ajoutent et
# retirent des applications, le panneau de réglages y écrit le thème. Le
# réécrire à chaque fourniture effacerait tout cela sans prévenir.
CONF="$TARGET_HOME/.config/claude-os/shell.conf"
if [ -f "$CONF" ]; then
	info "configuration du shell conservée : $CONF"

	# Une seule exception à « on ne touche pas au fichier de l'utilisateur ».
	#
	# Les Réglages ne sont plus une icône du dock : ils s'ouvrent depuis le
	# panneau de la barre d'état. Un fichier écrit avant ce changement les
	# épingle encore, et l'icône resterait là sans que rien ne l'explique.
	# On retire cette entrée-là, et elle seule : ni l'ordre, ni le thème, ni
	# les autres applications ne sont touchés.
	if grep -q '^pinned=.*claude-os-reglages' "$CONF"; then
		info "les Réglages quittent le dock — ils sont dans la barre d'état"
		run "sed -i -e 's/;claude-os-reglages//' -e 's/claude-os-reglages;//' -e 's/^pinned=claude-os-reglages\$/pinned=/' '$CONF'"
	fi

	info "le gestionnaire de fichiers n'est pas épinglé d'office :"
	info "  le glisser depuis le lanceur vers le dock, ou clic droit dessus"
else
	# Uniquement ce qui est réellement installé : une icône épinglée sans
	# application derrière affiche un pictogramme générique qui ne lance rien.
	PINNED="chromium"
	command -v claude-desktop >/dev/null 2>&1 && PINNED="$PINNED;claude-desktop"
	command -v mousepad       >/dev/null 2>&1 && PINNED="$PINNED;mousepad"
	PINNED="$PINNED;claude-os-fichiers"

	run "mkdir -p '$TARGET_HOME/.config/claude-os'"
	run "cat > '$CONF' <<EOF
[dock]
pinned=$PINNED
reserve_space=false

[appearance]
theme=claude-sombre
icon_theme=Papirus
font=

[wallpaper]
image=/usr/share/claude-os/wallpaper/default.png
fill=true
EOF"
	info "dock épinglé sur : $PINNED"
fi

# Les applications ordinaires — Chromium, le bloc-notes, les dialogues de
# Claude Desktop — ne sont pas redessinées par la feuille de style du shell.
# Sans ceci elles resteraient claires au milieu d'un bureau sombre.
info "thème GTK sombre pour les applications"
run "mkdir -p '$TARGET_HOME/.config/gtk-3.0' '$TARGET_HOME/.config/gtk-4.0'"
for v in 3.0 4.0; do
	run "cat > '$TARGET_HOME/.config/gtk-$v/settings.ini' <<'EOF'
[Settings]
gtk-theme-name=Adwaita-dark
gtk-icon-theme-name=Papirus
gtk-font-name=Inter 10
gtk-application-prefer-dark-theme=1
gtk-cursor-theme-name=Adwaita
EOF"
done
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
# Wayland natif : sans ce drapeau Chromium démarre sous Xwayland, ce qui
# ajoute un serveur X entier en mémoire, rend le texte plus flou sur écran
# dense et partage mal le presse-papier.
export CHROMIUM_FLAGS=\"\${CHROMIUM_FLAGS} --ozone-platform-hint=auto\"
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
info "Se déconnecter puis se reconnecter — ou redémarrer."
info "LightDM ouvre « Claude OS » : c'est la préférence qui vient d'être écrite."
info "Le menu en haut à droite de l'écran de connexion permet d'en changer ;"
info "« labwc » y donne le même bureau, sans notre lanceur de session."
echo
info "À essayer une fois la session ouverte :"
info "  touche Loupe (Super)              masque / affiche dock et barre d'état"
info "  bouton rond à gauche du dock      lanceur d'applications"
info "  Super + A                         idem, au clavier"
info "  clic sur la barre d'état          Wi-Fi, Bluetooth, batterie, Réglages"
info "  clic droit sur une icône du dock  épingler, retirer, fermer"
info "  glisser une icône du dock         réorganisation, enregistrée aussitôt"
info "  Super + Entrée                    terminal de secours (foot)"
info "  Super + Maj + Q                   fermer la session"
echo
info "Vérifications à faire à la première ouverture de session :"
info "  vainfo | head -5                  décodage vidéo matériel"
info "  nmcli device wifi list            Wi-Fi"
info "  bluetoothctl show                 Bluetooth"
info "  aplay -l && wpctl status          audio (haut-parleurs internes !)"
info "  free -h                           empreinte mémoire au repos"
info "  tlp-stat -s -c                    gestion d'énergie active"
info "  powertop --auto-tune=false        consommation par poste"
info "  bash tools/probe-keys.sh          codes des touches Chromebook (Wayland)"
info "  bash tools/validate-install.sh    contrôle complet de l'installation"
echo
warn "L'audio est le point de risque n°1 sur cette machine : casque"
warn "fonctionnel mais haut-parleurs muets est le symptôme classique."
