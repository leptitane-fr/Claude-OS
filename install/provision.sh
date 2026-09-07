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

# QUELLE VERSION DU DÉPÔT EST EN TRAIN DE TOURNER.
#
# Ce dépôt porte plusieurs branches « claude/… » et aucune branche par
# défaut. Un « git pull » sur la mauvaise branche répond « Déjà à jour » sans
# rien changer, et l'on relance alors une version périmée en croyant appliquer
# un correctif. C'est arrivé : le correctif de la purge était poussé, la
# machine tournait sur l'ancienne branche, et la purge a redésinstallé labwc.
#
# Trois lignes affichées ici, et l'on sait ce qu'on exécute.
if command -v git >/dev/null 2>&1 && [ -d "$REPO_DIR/.git" ]; then
	info "dépôt   : $REPO_DIR"
	info "branche : $(git -C "$REPO_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo '?')"
	info "commit  : $(git -C "$REPO_DIR" log --oneline -1 2>/dev/null || echo '?')"

	# Le garde-fou de la purge est la correction qui rend ce script sûr.
	# S'il est absent, on tourne sur une version qui peut désinstaller le
	# compositeur : on s'arrête plutôt que de la rejouer.
	if ! grep -q 'VITAUX=' "$0"; then
		die "cette copie de provision.sh est ANTÉRIEURE au correctif de la purge.
      Elle peut désinstaller labwc et laisser la machine sans bureau.
      Se placer sur la branche qui porte le correctif :
          git -C $REPO_DIR fetch origin
          git -C $REPO_DIR checkout claude/examine-project-qgnt80
      puis relancer ce script."
	fi
fi

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

# TOUT CE QUI EST LISTÉ EST « MANUEL », ET LE RESTE.
#
# Un paquet marqué « installé automatiquement » est à la merci du prochain
# apt autoremove. C'est ainsi que Chromium a disparu de cette machine : il
# figurait dans cette liste, mais son marquage disait le contraire, et le
# nettoyage de fin de fourniture l'a emporté sans un mot.
#
# apt-mark est idempotent et ne coûte rien ; il est le garde-fou de
# l'autoremove qui suit.
run "apt-mark manual $PKGS >/dev/null 2>&1 || true"

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

# Le poste cible est maintenant valide en greetd/labwc. Laisser Xorg et
# LightDM en place permettrait a une ancienne session de reprendre la main, et
# masquerait les regressions Wayland que l'on veut voir et corriger. La purge
# ne touche a aucun document utilisateur ; les anciennes configurations sont
# retirees plus haut et le terminal de secours est natif Wayland (foot).
#
# DEUX NOMS SONT ABSENTS DE CETTE LISTE, ET C'EST LE POINT IMPORTANT.
#
# « xwayland » et « x11-common » y figuraient. Or labwc porte « Depends:
# xwayland », et xwayland depend de x11-common par xserver-common : nommer
# l'un ou l'autre ici desinstalle LE COMPOSITEUR. La machine redemarre alors
# sur un greetd qui lance un labwc absent — ecran noir, aucune connexion
# possible, et seul SSH pour s'en sortir. C'est la panne du 7 septembre 2026,
# reproduite et mesuree depuis ; voir docs/06.
#
# « xserver-xorg-core », en revanche, ne porte rien de la pile Wayland : il
# part sans dommage, et c'est l'essentiel du serveur X qui part avec lui.
VIEUX="openbox plank tint2 picom rofi pcmanfm xcape xdotool dunst xwallpaper
        libnotify-bin python3-gi gir1.2-gtk-3.0 network-manager-gnome blueman
        x11-utils x11-xserver-utils gnome-terminal gnome-terminal-data
        lightdm lightdm-gtk-greeter xserver-xorg-core xserver-xorg-input-libinput"

# CE QUE LA PURGE NE DOIT JAMAIS EMPORTER.
#
# Corriger la liste ci-dessus ne suffit pas : elle ne dit que ce qu'on NOMME,
# jamais ce qu'apt va reellement retirer — il emporte aussi tout ce qui depend
# de ce qu'on nomme. Un paquet ajoute ici dans six mois pourrait rouvrir la
# meme panne sans que personne fasse le lien.
#
# On demande donc d'abord a apt ce qu'il ferait, on lit sa reponse, et on
# n'execute que si le bureau y survit. Une simulation coute une seconde ; la
# panne qu'elle evite a coute une soiree et un ecran noir.
VITAUX="labwc xwayland greetd dbus-user-session libgtk4-layer-shell0 network-manager"

A_PURGER=""
for pkg in $VIEUX; do
	dpkg -l "$pkg" 2>/dev/null | grep -q "^ii" && A_PURGER="$A_PURGER $pkg"
done
if [ -n "$A_PURGER" ]; then
	info "purge envisagée :$A_PURGER"

	CASCADE="$(DEBIAN_FRONTEND=noninteractive apt-get -s purge -y $A_PURGER 2>/dev/null \
	           | sed -n 's/^\(Purg\|Remv\) \([^ ]*\).*/\2/p' || true)"
	MENACES=""
	for v in $VITAUX; do
		printf '%s\n' "$CASCADE" | grep -qx "$v" && MENACES="$MENACES $v"
	done

	if [ -n "$MENACES" ]; then
		warn "PURGE ABANDONNÉE : apt retirerait aussi :$MENACES"
		warn "Ces paquets portent la session graphique. RIEN n'a été retiré."
		warn "Corriger la liste VIEUX de ce script avant de recommencer."
	else
		# La sortie n'est PAS avalée. Une purge silencieuse a déjà coûté
		# Chromium à ce projet, puis le compositeur lui-même ; on lit
		# désormais ce qui s'en va.
		run "DEBIAN_FRONTEND=noninteractive apt-get purge -y $A_PURGER" \
			|| warn "la purge a échoué — sans conséquence pour la suite"
	fi
else
	info "aucun paquet de l'ancienne pile à retirer"
fi

# Contrôle final, indépendant de tout ce qui précède. Ce script ne doit jamais
# rendre la main sur une machine dont le compositeur ou l'écran de connexion
# ont disparu : c'est le seul état dont on ne se sort pas sans SSH.
if [ "$DRY" -eq 0 ]; then
	for v in labwc greetd; do
		command -v "$v" >/dev/null 2>&1 || [ -x "/usr/sbin/$v" ] || \
			die "« $v » a disparu du système — la machine n'a plus de bureau.
      Réparer AVANT de redémarrer :  sudo apt-get install --reinstall $v"
	done
	info "compositeur et écran de connexion présents ✓"
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
run "chmod +x /usr/local/lib/claude-os/filet-session"

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

# LE JOURNAL DE L'ÉCRAN DE CONNEXION, CRÉÉ POUR « _greetd ».
#
# Le greeter tourne sous ce compte, qui n'écrit pas dans /var/log. Son
# message d'erreur se perdait donc exactement dans le cas où l'on en a
# besoin : quand l'écran de connexion ne s'affiche pas. Le fichier est créé
# ici, à lui, une fois pour toutes.
if getent passwd _greetd >/dev/null 2>&1; then
	run "touch /var/log/claude-os-connexion.log"
	run "chown _greetd:_greetd /var/log/claude-os-connexion.log"
	run "chmod 0644 /var/log/claude-os-connexion.log"
	info "journal de l'écran de connexion accessible à _greetd"
else
	warn "compte « _greetd » absent : greetd est-il bien installé ?"
fi

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

	# LE SYMLIEN QUI BLOQUAIT TOUT.
	#
	# greetd.service porte « Alias=display-manager.service », comme tout
	# gestionnaire de session. Or /etc/systemd/system/display-manager.service
	# existe déjà : il pointe sur lightdm, posé par son paquet et non par
	# « systemctl enable ». « systemctl disable lightdm » ne le retire donc
	# pas, et « systemctl enable greetd » échoue sur « File exists ».
	#
	# La sortie de cette commande partait dans /dev/null : l'activation
	# échouait sans un mot, la machine redémarrait sur LightDM, et le
	# correctif semblait n'avoir « rien changé ». Constaté sur la machine.
	run "rm -f /etc/systemd/system/display-manager.service"
	run "systemctl enable greetd"

	# On VÉRIFIE. Une bascule de gestionnaire de session qu'on croit faite
	# et qui ne l'est pas coûte un redémarrage et une soirée.
	if [ "$DRY" -eq 0 ] && ! systemctl is-enabled greetd >/dev/null 2>&1; then
		warn "l'activation de greetd a échoué — LightDM est remis en service"
		warn "pour ne pas laisser la machine sans écran de connexion."
		run "systemctl enable lightdm >/dev/null 2>&1 || true"
	else
		info "greetd activé ; LightDM ne démarrera plus"

		# LE FILET, ARMÉ POUR LE PREMIER DÉMARRAGE.
		#
		# C'est ici que la machine devient vulnérable : au prochain
		# allumage, tout repose sur un greetd qui n'a encore jamais servi.
		# Le 7 septembre 2026, ce démarrage-là s'est fait sur un
		# compositeur désinstallé, et il a fallu SSH depuis une autre
		# machine pour reprendre la main.
		#
		# Armé, le filet constate quatre minutes après le démarrage que
		# l'écran de connexion est bien apparu. Sinon il écrit pourquoi
		# dans /var/log/, range greetd et redémarre sur un écran où l'on
		# peut entrer. Il se désarme seul dès que la session a fait ses
		# preuves — voir rootfs/usr/local/lib/claude-os/filet-session.
		run "mkdir -p /etc/claude-os"
		run "date '+%Y-%m-%d %H:%M:%S' > /etc/claude-os/filet-arme"
		run "systemctl daemon-reload"
		if run "systemctl enable claude-os-filet.timer >/dev/null 2>&1"; then
			info "filet de sécurité armé pour le premier démarrage"
		else
			warn "le filet de sécurité n'a pas pu être armé."
			warn "Garder un accès SSH ouvert au premier redémarrage."
		fi
	fi
else
	warn "greetd n'est pas installé : l'écran de connexion reste celui de LightDM"
fi

# Debian retient le gestionnaire de session choisi dans ce fichier, que
# certains scripts de paquets relisent. Il doit dire la même chose que
# systemd, sinon une mise à jour de lightdm peut tout ramener en arrière.
run "printf '%s\n' /usr/sbin/greetd > /etc/X11/default-display-manager"

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
# --- services qui tournent sans servir ------------------------------------
#
# Chacun est justifié individuellement. Rien n'est désactivé « parce que ça a
# l'air inutile » : cron et anacron restent, par exemple, parce que Debian y
# fait tourner la rotation des journaux et l'indexation des pages de manuel —
# les couper remplirait le disque en silence.

# Unités SYSTÈME.
if systemctl list-unit-files NetworkManager-wait-online.service 2>/dev/null \
   | grep -q NetworkManager-wait-online; then
	info "NetworkManager-wait-online : retarde le démarrage jusqu'à ce que"
	info "  le réseau réponde. Rien ici n'attend le réseau pour démarrer."
	run "systemctl disable NetworkManager-wait-online.service >/dev/null 2>&1 || true"
fi

# Unités UTILISATEUR, masquées globalement.
#
# « systemctl --user » demanderait le bus de l'utilisateur, que ce script —
# lancé en root — n'a pas. « --global mask » écrit dans /etc/systemd/user et
# ne demande aucun bus. C'est réversible : systemctl --global unmask.
for svc in foot-server.socket foot-server.service \
           mpris-proxy.service filter-chain.service; do
	case "$svc" in
		foot-server.*)      quoi="mode serveur de foot, dont rien ne se sert ici" ;;
		mpris-proxy.service) quoi="relais des touches multimédia Bluetooth, non câblées" ;;
		filter-chain.service) quoi="chaîne de filtres PipeWire, vide sur cette machine" ;;
	esac
	info "$svc : $quoi"
	run "systemctl --global mask '$svc' >/dev/null 2>&1 || true"
done

info "firmware Wi-Fi : purger les familles inutilisées après validation"
info "  lspci -nnk | grep -A3 -i network    puis  apt purge firmware-<inutile>"

# L'AUTOREMOVE NE DOIT PAS ÊTRE MUET.
#
# C'est lui qui a emporté Chromium, et sa sortie partait dans /dev/null : la
# fourniture s'est terminée « sans erreur » sur un système amputé. On regarde
# d'abord ce qu'il compte retirer, on le dit, et on ne le fait qu'ensuite.
info "nettoyage des paquets orphelins"
ORPHELINS="$(apt-get -s autoremove 2>/dev/null | sed -n 's/^Remv \([^ ]*\).*/\1/p' | tr '\n' ' ')"
if [ -n "$ORPHELINS" ]; then
	info "à retirer :$ORPHELINS"
	run "apt-get autoremove -y --purge >/dev/null 2>&1 || true"
else
	info "aucun orphelin"
fi
run "apt-get clean"

# ------------------------------------------------------- contrôle final

say "Contrôle des applications"

# UNE APPLICATION ABSENTE NE SE VOIT PAS.
#
# Le dock affiche alors une icône générique qui ne lance rien, et rien à
# l'écran ne dit pourquoi — il faut aller lire le journal du shell. Chromium
# a disparu de cette machine sans que la fourniture s'en aperçoive : ce
# contrôle est là pour que cela ne recommence pas.
#
# Le .desktop compte autant que le binaire : c'est LUI que le dock cherche.
A_REPARER=""
for app in chromium mousepad foot claude-os-fichiers claude-os-reglages; do
	BIN=""; DESK=""
	command -v "$app" >/dev/null 2>&1 && BIN=oui
	for d in /usr/local/share/applications /usr/share/applications \
	         "$TARGET_HOME/.local/share/applications"; do
		[ -f "$d/$app.desktop" ] && { DESK=oui; break; }
	done

	if [ -n "$BIN" ] && [ -n "$DESK" ]; then
		continue
	fi
	warn "$app : binaire=${BIN:-ABSENT} entrée .desktop=${DESK:-ABSENTE}"
	# Nos propres binaires viennent de la compilation, pas d'apt.
	case "$app" in claude-os-*) continue ;; esac
	A_REPARER="$A_REPARER $app"
done

if [ -n "$A_REPARER" ]; then
	info "réinstallation de :$A_REPARER"
	run "DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends$A_REPARER" \
		|| warn "la réinstallation a échoué — me transmettre le message ci-dessus"
else
	info "les cinq applications sont présentes, binaire et entrée .desktop"
fi

# Claude Desktop se contrôle à part : il vient de son propre dépôt, et
# --no-claude permet de s'en passer.
if [ "$WITH_CLAUDE" -eq 1 ]; then
	command -v claude-desktop >/dev/null 2>&1 \
		&& info "claude-desktop présent" \
		|| warn "claude-desktop absent malgré l'installation"
fi

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
