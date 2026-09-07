#!/usr/bin/env bash
#
# Claude OS — mise en service de l'écran de connexion, par étapes.
#
# CE QUE CE SCRIPT REMPLACE, ET POURQUOI
#
# La bascule vers greetd se faisait jusqu'ici d'un bloc : déployer, purger,
# activer, redémarrer, espérer. Le 7 septembre 2026 elle a désinstallé labwc
# au passage — la purge nommait « xwayland », dont labwc dépend — et la
# machine a redémarré sur un compositeur absent. Écran noir, pas de champ de
# mot de passe, pas de touches F pour changer de terminal sur ce Chromebook :
# il a fallu SSH depuis une autre machine pour s'en sortir. Voir docs/06.
#
# La leçon tenait en une phrase du journal d'incident : « tester le greeter et
# la session labwc SANS purge, valider leur démarrage, puis seulement retirer
# les composants X11 ». Ce script est cette procédure.
#
# QUATRE ÉTAPES, TROIS RÉVERSIBLES SANS REDÉMARRER
#
#   --verifier   Ne change RIEN. Contrôle que tout ce dont l'écran de
#                connexion a besoin est présent et cohérent.
#
#   --essai      Affiche le VRAI écran de connexion sur un terminal virtuel
#                libre, pendant que la session en cours continue de tourner à
#                côté. Rien n'est activé, rien n'est purgé, et l'on revient
#                seul au bout du délai. C'est l'étape qui manquait.
#
#   --basculer   Arme le filet de sécurité, PUIS bascule le gestionnaire de
#                session. Si l'écran de connexion n'apparaît pas au
#                redémarrage, la machine s'en aperçoit seule, écrit pourquoi,
#                et repart sur un écran où l'on peut entrer.
#
#   --deployer   Recopie rootfs/ vers / — les lanceurs, la configuration de
#                labwc, le filet — sans toucher aux paquets ni au shell
#                compilé. « git pull » met à jour le DÉPÔT, pas la machine :
#                c'est cette étape qui manquait entre les deux.
#
#   --revenir    Défait la bascule.
#
# USAGE
#   sudo bash install/bascule-session.sh --verifier
#   sudo bash install/bascule-session.sh --essai [secondes]   (défaut : 30)
#   sudo bash install/bascule-session.sh --basculer
#   sudo bash install/bascule-session.sh --revenir
#
set -euo pipefail

VT_ESSAI=2
DUREE=30
ACTION=""

while [ $# -gt 0 ]; do
	case "$1" in
		--verifier) ACTION=verifier ;;
		--deployer) ACTION=deployer ;;
		--essai)    ACTION=essai
		            if [ "${2:-}" ] && [ -z "${2//[0-9]/}" ]; then DUREE="$2"; shift; fi ;;
		--basculer) ACTION=basculer ;;
		--revenir)  ACTION=revenir ;;
		--vt)       VT_ESSAI="${2:?numéro de terminal virtuel attendu}"; shift ;;
		-h|--help)  sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "Option inconnue : $1" >&2; exit 2 ;;
	esac
	shift
done

[ -n "$ACTION" ] || { sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo "Ce script doit tourner en root : sudo bash $0 --$ACTION" >&2; exit 1; }

say()  { printf '\n\033[1;34m── %s\033[0m\n' "$*"; }
info() { printf '      %s\n' "$*"; }
warn() { printf '      \033[33m! %s\033[0m\n' "$*"; }
die()  { printf '\n\033[31mÉCHEC : %s\033[0m\n' "$*" >&2; exit 1; }

ANOMALIES=0
ok()   { printf '  \033[32m✓\033[0m %-42s %s\n' "$1" "${2:-}"; }
ko()   { printf '  \033[31m✗\033[0m %-42s %s\n' "$1" "${2:-}"; ANOMALIES=$((ANOMALIES+1)); }

# ===========================================================================
#  VÉRIFIER — ne touche à rien
# ===========================================================================
verifier() {
	say "Ce dont l'écran de connexion a besoin"

	# LE PREMIER CONTRÔLE EST CELUI QUI AURAIT ÉVITÉ LA PANNE.
	#
	# labwc porte « Depends: xwayland ». Si xwayland a disparu, labwc est
	# parti avec lui, et greetd démarrera sur un compositeur inexistant.
	# C'est très exactement ce qui s'est produit, et cela se voit ici en
	# une ligne.
	for b in /usr/bin/labwc /usr/sbin/greetd /usr/bin/claude-os-connexion; do
		[ -x "$b" ] && ok "$(basename "$b")" "$b" \
		            || ko "$(basename "$b")" "ABSENT — sudo apt-get install --reinstall $(basename "$b")"
	done

	if dpkg-query -W -f='${db:Status-Status}' xwayland 2>/dev/null | grep -qx installed; then
		ok "xwayland" "présent (labwc en dépend)"
	else
		ko "xwayland" "ABSENT — labwc ne peut pas être installé sans lui"
	fi

	for b in /usr/local/bin/claude-os-greeter /usr/local/bin/claude-os-session; do
		[ -x "$b" ] && ok "$(basename "$b")" "exécutable" \
		            || ko "$(basename "$b")" "absent ou non exécutable"
	done

	# UN BINAIRE PRÉSENT N'EST PAS UN BINAIRE QUI DÉMARRE.
	#
	# « /usr/bin/claude-os-connexion existe » ne prouve rien : s'il lui manque
	# une bibliothèque partagée, l'éditeur de liens le tue AVANT sa première
	# ligne de code. Le processus n'apparaît nulle part, rien ne s'affiche, et
	# sous greetd le message part dans le vide. Symptôme à l'écran : un fond
	# noir et le pointeur de la souris — labwc tourne, son client est mort.
	#
	# ldd le dit en une seconde, et ce contrôle couvre les six programmes.
	MANQUANTES=""
	for b in /usr/bin/claude-os-connexion /usr/bin/claude-os-fond \
	         /usr/bin/claude-os-dock /usr/bin/claude-os-status \
	         /usr/bin/claude-os-lanceur /usr/bin/claude-os-fichiers; do
		[ -x "$b" ] || continue
		L="$(ldd "$b" 2>/dev/null | awk '/not found/{print $1}' | sort -u | tr '\n' ' ')"
		[ -n "$L" ] && MANQUANTES="$MANQUANTES $(basename "$b"):$L"
	done
	if [ -z "$MANQUANTES" ]; then
		ok "bibliothèques des binaires" "toutes résolues"
	else
		ko "BIBLIOTHÈQUES MANQUANTES" "$MANQUANTES"
		info "        Ces programmes ne peuvent pas démarrer du tout."
		info "        Réinstaller le paquet fautif, puis relancer provision.sh."
	fi

	# gtk4-layer-shell ancre le dock, la barre et l'écran de connexion hors du
	# flux des fenêtres. Sans lui, aucun des six ne s'affiche.
	dpkg-query -W -f='${db:Status-Status}' libgtk4-layer-shell0 2>/dev/null | grep -qx installed \
		&& ok "libgtk4-layer-shell0" "présent" \
		|| ko "libgtk4-layer-shell0" "ABSENT — rien ne peut s'afficher"

	say "La configuration du compositeur de connexion"
	for f in /etc/xdg/labwc-greeter/rc.xml /etc/xdg/labwc-greeter/environment; do
		[ -r "$f" ] && ok "$(basename "$f")" "$f" || ko "$(basename "$f")" "illisible : $f"
	done
	if [ -x /etc/xdg/labwc-greeter/autostart ]; then
		ok "autostart" "exécutable"
	else
		ko "autostart" "PAS exécutable — labwc ne lancera pas le greeter"
	fi

	say "greetd"
	if [ -r /etc/greetd/config.toml ]; then
		CMD="$(sed -n 's/^ *command *= *"\(.*\)"/\1/p' /etc/greetd/config.toml | head -1)"
		case "$CMD" in
			*labwc*labwc-greeter*) ok "config.toml" "$CMD" ;;
			"")  ko "config.toml" "aucune commande de session" ;;
			*)   ko "config.toml" "commande inattendue : $CMD" ;;
		esac
	else
		ko "config.toml" "/etc/greetd/config.toml absent"
	fi

	say "Le compte à ouvrir"
	if [ -r /etc/claude-os/utilisateur ]; then
		COMPTE="$(head -n 1 /etc/claude-os/utilisateur)"
		if getent passwd "$COMPTE" >/dev/null; then
			ok "utilisateur" "$COMPTE"
		else
			ko "utilisateur" "« $COMPTE » n'existe pas sur cette machine"
		fi
	else
		# Sans ce fichier le greeter cherche seul le compte humain unique ;
		# ce n'est donc pas bloquant, mais autant le dire.
		warn "/etc/claude-os/utilisateur absent — le greeter cherchera seul"
	fi

	say "Le bureau qui s'ouvrira derrière"
	for b in claude-os-fond claude-os-dock claude-os-status; do
		command -v "$b" >/dev/null 2>&1 && ok "$b" "$(command -v "$b")" \
		                               || ko "$b" "INTROUVABLE"
	done
	[ -d /usr/share/claude-os-shell/style ] \
		&& ok "feuilles de style" "$(ls /usr/share/claude-os-shell/style | tr '\n' ' ')" \
		|| ko "feuilles de style" "/usr/share/claude-os-shell/style absent"
	dpkg-query -W -f='${db:Status-Status}' dbus-user-session 2>/dev/null | grep -qx installed \
		&& ok "dbus-user-session" "présent" \
		|| ko "dbus-user-session" "absent — le bureau s'ouvrirait vide"

	say "État actuel du gestionnaire de session"
	# « systemctl is-enabled » écrit « not-found » sur sa sortie standard ET
	# rend un code d'erreur : un « || echo » afficherait les deux.
	etat_unite() {
		local e; e="$(systemctl is-enabled "$1" 2>/dev/null || true)"
		case "$e" in ""|not-found) echo "non installé" ;; *) echo "$e" ;; esac
	}
	info "greetd  : $(etat_unite greetd)"
	info "lightdm : $(etat_unite lightdm)"
	info "cible   : $(systemctl get-default 2>/dev/null)"
	if [ -e /etc/claude-os/filet-arme ]; then
		info "filet   : ARMÉ (il jugera au prochain démarrage)"
	else
		info "filet   : non armé"
	fi

	echo
	if [ "$ANOMALIES" -eq 0 ]; then
		printf '  \033[32mTout est en place. L'"'"'essai peut être lancé :\033[0m\n'
		printf '      sudo bash %s --essai\n' "$0"
		return 0
	fi
	printf '  \033[31m%d anomalie(s). NE PAS basculer avant correction.\033[0m\n' "$ANOMALIES"
	printf '      La plupart se corrigent en relançant :  sudo bash install/provision.sh\n'
	return 1
}

# ===========================================================================
#  DÉPLOYER — recopier rootfs/ sans relancer toute la fourniture
# ===========================================================================
#
#  « git pull » met à jour le dépôt ; il ne touche pas à /usr/local/bin ni à
#  /etc/xdg. Un correctif apporté à un lanceur restait donc dans le dépôt
#  pendant qu'on essayait l'ancienne version sur la machine, sans que rien ne
#  le signale. C'est arrivé ici : le greeter corrigé pour ne plus perdre son
#  journal n'était pas celui qui tournait.
#
#  provision.sh fait ce déploiement, mais il réinstalle aussi les paquets et
#  recompile le shell — plusieurs minutes qu'on ne veut pas payer pour copier
#  quatre fichiers pendant un diagnostic.
deployer() {
	DEPOT="$(cd "$(dirname "$0")/.." && pwd)"
	[ -d "$DEPOT/rootfs" ] || die "rootfs/ introuvable dans $DEPOT"

	say "Déploiement de rootfs/ depuis $DEPOT"
	info "branche : $(git -C "$DEPOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo '?')"
	info "commit  : $(git -C "$DEPOT" log --oneline -1 2>/dev/null || echo '?')"

	cp -a "$DEPOT/rootfs/." /
	chmod +x /usr/local/bin/claude-os-claude /usr/local/bin/claude-os-shell-basculer \
	         /usr/local/bin/claude-os-session /usr/local/bin/claude-os-greeter \
	         /etc/xdg/labwc/autostart /etc/xdg/labwc-greeter/autostart \
	         /usr/local/lib/claude-os/filet-session 2>/dev/null || true
	ok "fichiers copiés" "/usr/local/bin, /etc/xdg, /usr/local/lib"

	# Le journal de l'écran de connexion doit appartenir à « _greetd », sans
	# quoi le greeter ne peut pas y écrire et sa panne reste muette.
	if getent passwd _greetd >/dev/null 2>&1; then
		touch /var/log/claude-os-connexion.log
		chown _greetd:_greetd /var/log/claude-os-connexion.log
		chmod 0644 /var/log/claude-os-connexion.log
		ok "journal du greeter" "accessible à _greetd"
	else
		ko "compte _greetd" "absent — greetd est-il installé ?"
	fi

	systemctl daemon-reload 2>/dev/null || true
	echo
	info "Déployé. L'essai peut être relancé :"
	info "    sudo bash $0 --essai"
}

# ---------------------------------------------------------------------------
#  Ce qu'il faut savoir quand l'essai échoue.
#
#  Collecté PENDANT que greetd tourne encore : la moitié de ces
#  renseignements — la session logind de « _greetd », les droits sur la carte
#  graphique — disparaissent à la seconde où le service s'arrête. Les
#  demander après coup ne rend que des lignes vides.
# ---------------------------------------------------------------------------
postmortem_essai() {
	warn "L'essai a échoué. Voici de quoi en établir la cause."

	echo
	info "── journal complet du service d'essai ──"
	journalctl -u claude-os-essai-greeter -b --no-pager 2>/dev/null | sed 's/^/        /'

	echo
	info "── la session logind de « _greetd » existe-t-elle ? ──"
	# Sans session logind, pas de siège ; sans siège, pas de maîtrise du DRM,
	# et labwc ne peut pas dessiner. C'est pam_systemd qui la crée.
	loginctl list-sessions --no-legend 2>/dev/null | sed 's/^/        /' \
		|| echo "        (aucune)"
	SID_G="$(loginctl list-sessions --no-legend 2>/dev/null | awk '$3=="_greetd"{print $1; exit}')"
	if [ -n "$SID_G" ]; then
		ok "session _greetd" "$SID_G"
		loginctl show-session "$SID_G" -p Type -p Class -p Active -p State \
			-p VTNr -p Seat -p Remote 2>/dev/null | sed 's/^/        /'
	else
		ko "AUCUNE session logind pour _greetd"
		info "        C'est la piste n°1 : sans session, aucun accès à l'écran."
		info "        pam_systemd.so est-il appelé ? Voir le fichier PAM ci-dessous."
	fi

	echo
	info "── /etc/pam.d/greetd-greeter ──"
	if [ -r /etc/pam.d/greetd-greeter ]; then
		sed 's/^/        /' /etc/pam.d/greetd-greeter
		# CHERCHER pam_systemd DANS LE SEUL FICHIER EST UN FAUX NÉGATIF.
		#
		# Debian écrit couramment « @include login » : pam_systemd est alors
		# tiré par le fichier inclus, et le mot n'apparaît nulle part dans
		# celui-ci. Annoncer « ABSENT » enverrait chercher une panne qui
		# n'existe pas — précisément ce qu'un outil de diagnostic ne doit
		# jamais faire. On suit donc les inclusions.
		pam_contient() {          # $1 = fichier, $2 = profondeur restante
			[ -r "$1" ] || return 1
			[ "${2:-3}" -gt 0 ] || return 1
			grep -qE '^[^#]*pam_systemd' "$1" && return 0
			inc="$(sed -n 's/^[[:space:]]*@include[[:space:]]\+\([^[:space:]]*\).*/\1/p' "$1")"
			for f in $inc; do
				pam_contient "/etc/pam.d/$f" "$(( ${2:-3} - 1 ))" && return 0
			done
			return 1
		}
		if pam_contient /etc/pam.d/greetd-greeter 3; then
			ok "pam_systemd" "atteint (directement ou par @include)"
		else
			ko "pam_systemd INTROUVABLE, inclusions suivies"
			info "        Sans lui, aucune session logind : pas de siège, pas d'écran."
		fi
	else
		ko "/etc/pam.d/greetd-greeter" "absent"
	fi

	echo
	info "── la carte graphique, et qui peut y toucher ──"
	val_dri="$(ls -A /dev/dri 2>/dev/null | tr '\n' ' ')"
	[ -n "$val_dri" ] && ok "/dev/dri" "$val_dri" || ko "/dev/dri" "vide ou absent"
	for n in /dev/dri/card*; do
		[ -e "$n" ] || continue
		info "        $(ls -l "$n" | awk '{print $1, $3, $4, $NF}')"
		getfacl -p "$n" 2>/dev/null | grep -E '^user:' | sed 's/^/          /'
	done
	info "        groupes de _greetd : $(id -nG _greetd 2>/dev/null || echo '<compte absent>')"
	info "        pilote i915 : $(lsmod 2>/dev/null | awk '$1=="i915"{print "chargé"}' || echo 'non chargé')"

	echo
	info "── sortie du greeter, si elle a pu être écrite ──"
	sed 's/^/        /' /var/log/claude-os-connexion.log 2>/dev/null \
		|| echo "        (aucune — normal : _greetd n'écrit pas dans /var/log)"

	echo
	info "── terminal virtuel ──"
	info "        actif : $(fgconsole 2>/dev/null || echo '?')   attendu : $VT_ESSAI"
}

# ===========================================================================
#  ESSAI — le vrai écran de connexion, sur un terminal virtuel libre
# ===========================================================================
essai() {
	verifier || die "des composants manquent ; l'essai n'apprendrait rien."

	command -v chvt >/dev/null 2>&1 || \
		die "« chvt » est absent. Il sert à montrer l'essai à l'écran puis à
      revenir. L'installer :  sudo apt-get install kbd"

	VT_COURANT="$(fgconsole 2>/dev/null || echo 1)"

	say "Essai de l'écran de connexion sur le terminal virtuel $VT_ESSAI"
	info "Le gestionnaire de session actuel n'est PAS touché."
	info "Rien n'est activé, rien n'est purgé, rien ne redémarre."
	echo

	# Le ménage est armé AVANT d'allumer quoi que ce soit : une interruption
	# au clavier ou une erreur ne doit jamais laisser un greetd d'essai
	# tourner sur un terminal virtuel, ni l'affichage bloqué dessus.
	nettoyer_essai() {
		chvt "$VT_COURANT" 2>/dev/null || true
		systemctl stop claude-os-essai-greeter.service >/dev/null 2>&1 || true
		rm -f /etc/greetd/config-essai.toml
	}
	trap nettoyer_essai EXIT INT TERM

	# Une configuration greetd distincte, sur un autre terminal virtuel. Celle
	# du système n'est pas relue, pas modifiée, pas même approchée.
	cat > /etc/greetd/config-essai.toml <<EOF
# Écrit par bascule-session.sh --essai. Fichier jetable.
[terminal]
vt = $VT_ESSAI

[default_session]
command = "labwc -C /etc/xdg/labwc-greeter"
user = "_greetd"
EOF

	# systemd-run plutôt que « greetd & » : le démon hérite ainsi d'un cgroup
	# propre et d'un siège, comme au démarrage réel. Un greetd lancé depuis un
	# shell SSH n'aurait pas les mêmes droits sur le terminal virtuel, et
	# l'essai ne prouverait rien.
	systemd-run --unit=claude-os-essai-greeter --description="Essai greetd Claude OS" \
		--collect /usr/sbin/greetd --config /etc/greetd/config-essai.toml >/dev/null

	sleep 3
	chvt "$VT_ESSAI" 2>/dev/null || warn "impossible de basculer l'affichage sur le VT $VT_ESSAI"

	printf '      \033[1mREGARDER L'"'"'ÉCRAN DE LA MACHINE MAINTENANT.\033[0m\n'
	info "Le champ de mot de passe doit s'y afficher pendant $DUREE secondes."
	info "L'affichage revient tout seul ensuite. Ne rien taper dessus."
	echo

	# Le constat automatique, en parallèle du constat visuel. Les deux
	# comptent : le processus peut tourner sans que rien ne s'affiche.
	#
	# DOUZE SECONDES DE PLUS, ET POURQUOI. Juger au bout de cinq secondes
	# était trop tôt : sur un N6000 démarrant depuis un eMMC, labwc puis un
	# greeter GTK4 mettent plusieurs secondes à ouvrir leur fenêtre. Un
	# verdict rendu trop tôt déclare mort ce qui est seulement lent.
	sleep 12

	# LA DISTINCTION QUI ORIENTE TOUT LE RESTE.
	#
	# « Pas de champ de mot de passe » recouvre deux pannes opposées :
	# le compositeur n'a pas démarré — c'est alors un problème de siège, de
	# carte graphique ou de droits de « _greetd » — ou bien il tourne et
	# c'est le greeter qui meurt dessus. Les chercher ensemble fait perdre
	# une soirée ; les séparer prend une ligne.
	LABWC_PID="$(pgrep -f 'labwc -C /etc/xdg/labwc-greeter' 2>/dev/null | tr '\n' ' ')"
	GREETER_PID="$(pgrep -f '/claude-os-connexion' 2>/dev/null | tr '\n' ' ')"

	[ -n "$LABWC_PID" ]   && ok "le compositeur tourne" "pid $LABWC_PID" \
	                      || ko "le COMPOSITEUR n'a pas démarré"
	[ -n "$GREETER_PID" ] && ok "le greeter tourne" "pid $GREETER_PID" \
	                      || ko "le GREETER ne tourne pas"

	if [ -z "$GREETER_PID" ]; then
		# L'AUTOPSIE VA DANS UN FICHIER, PAS À L'ÉCRAN.
		#
		# Elle fait plusieurs dizaines de lignes, et une sortie de console
		# recopiée à la main se fait couper — on a déjà perdu deux allers-
		# retours sur des journaux tronqués en cours de route. L'écran ne
		# reçoit que le verdict ; le fichier porte tout, et se transmet
		# d'un bloc.
		RAPPORT="/var/log/claude-os-essai-$(date '+%Y%m%d-%H%M%S').txt"
		postmortem_essai > "$RAPPORT" 2>&1
		chmod 0644 "$RAPPORT" 2>/dev/null || true
		resume_essai "$RAPPORT"
	fi

	RESTE=$((DUREE - 15))
	[ "$RESTE" -gt 0 ] && sleep "$RESTE"

	nettoyer_essai
	trap - EXIT

	say "Essai terminé, affichage rendu au terminal virtuel $VT_COURANT"
	if pgrep -f '/claude-os-connexion' >/dev/null 2>&1; then
		warn "un greeter tourne encore — le signaler, ce n'est pas normal"
	fi
	info "Si le champ de mot de passe s'est affiché, la bascule est sûre :"
	info "    sudo bash $0 --basculer"
	info "Sinon, ne rien basculer et transmettre TOUT le texte ci-dessus."
}

# ---------------------------------------------------------------------------
#  Le verdict à l'écran : court, pour être lu et recopié sans être tronqué.
# ---------------------------------------------------------------------------
resume_essai() {
	R="$1"
	echo
	warn "L'essai a échoué."
	echo
	info "── les constats ──"
	grep -aE '✓|✗' "$R" 2>/dev/null | sed 's/^/  /'
	echo
	info "── journal de greetd, 12 dernières lignes ──"
	journalctl -u claude-os-essai-greeter -b --no-pager 2>/dev/null \
		| tail -12 | sed 's/^/      /'
	echo
	info "── ce qu'a dit l'écran de connexion ──"
	TROUVE=0
	for f in /var/log/claude-os-connexion.log \
	         /run/user/*/claude-os-connexion.log \
	         /tmp/claude-os-connexion.log; do
		[ -s "$f" ] || continue
		info "  ($f)"
		tail -20 "$f" | sed 's/^/      /'
		TROUVE=1
	done
	if [ "$TROUVE" -eq 0 ]; then
		warn "  AUCUN journal du greeter nulle part."
		warn "  Cela veut dire qu'il n'a même pas été LANCÉ par labwc,"
		warn "  ou que /usr/local/bin/claude-os-greeter est l'ancienne"
		warn "  version, muette. Déployer puis recommencer :"
		warn "      sudo bash $0 --deployer"
	fi
	echo
	printf '  \033[1mRapport complet à transmettre : %s\033[0m\n' "$R"
}

# ===========================================================================
#  BASCULER — avec le filet armé AVANT
# ===========================================================================
basculer() {
	verifier || die "des composants manquent ; corriger avant de basculer."

	say "Armement du filet de sécurité"
	# L'ORDRE COMPTE. Le filet est armé AVANT que le gestionnaire de session
	# ne change, jamais après : entre les deux se trouve la seule fenêtre où
	# un redémarrage accidentel laisserait la machine sans issue.
	[ -x /usr/local/lib/claude-os/filet-session ] || \
		die "/usr/local/lib/claude-os/filet-session absent. Relancer provision.sh."
	mkdir -p /etc/claude-os
	date '+%Y-%m-%d %H:%M:%S' > /etc/claude-os/filet-arme
	systemctl daemon-reload
	systemctl enable claude-os-filet.timer >/dev/null
	info "au prochain démarrage, la machine vérifiera seule qu'on peut entrer ;"
	info "si l'écran de connexion manque, elle écrit pourquoi et repart"
	info "sur un écran où l'on peut se connecter. Sans seconde machine."

	say "Bascule du gestionnaire de session"
	# greetd.service porte « Alias=display-manager.service ». Le lien posé par
	# le paquet lightdm existe déjà et n'est pas retiré par « disable » :
	# « enable greetd » échouerait sur « File exists ». Constaté sur la
	# machine, et la sortie partait alors dans /dev/null — la bascule semblait
	# faite sans l'être.
	systemctl disable lightdm >/dev/null 2>&1 || true
	rm -f /etc/systemd/system/display-manager.service
	systemctl daemon-reload
	systemctl enable greetd

	systemctl is-enabled greetd >/dev/null 2>&1 \
		|| die "greetd n'a pas pu être activé. Rien n'a été changé d'irréversible :
      sudo bash $0 --revenir"
	systemctl set-default graphical.target >/dev/null 2>&1 || true
	printf '%s\n' /usr/sbin/greetd > /etc/X11/default-display-manager 2>/dev/null || true

	ok "greetd" "activé"
	ok "filet" "armé"
	echo
	printf '  \033[1mRedémarrer maintenant :  sudo systemctl reboot\033[0m\n'
	info "Garder cette session SSH ouverte pendant le redémarrage."
	info "Si l'écran reste noir, ne rien faire : la machine se rétablit en"
	info "quatre minutes et le rapport sera dans /var/log/claude-os-echec-*.txt"
}

# ===========================================================================
#  REVENIR
# ===========================================================================
revenir() {
	say "Retour en arrière"
	systemctl disable greetd >/dev/null 2>&1 || true
	rm -f /etc/systemd/system/display-manager.service
	rm -f /etc/claude-os/filet-arme
	systemctl disable claude-os-filet.timer >/dev/null 2>&1 || true
	systemctl daemon-reload

	if systemctl cat lightdm.service >/dev/null 2>&1; then
		systemctl enable lightdm >/dev/null 2>&1 || true
		systemctl set-default graphical.target >/dev/null 2>&1 || true
		printf '%s\n' /usr/sbin/lightdm > /etc/X11/default-display-manager 2>/dev/null || true
		ok "LightDM" "réactivé"
	else
		systemctl set-default multi-user.target >/dev/null 2>&1 || true
		ok "console texte" "la machine démarrera sur une console"
		info "Pour retrouver un écran graphique complet :"
		info "    sudo bash install/restore-session-x11-ssh.sh"
	fi
	info "Effectif au prochain redémarrage."
}

case "$ACTION" in
	verifier) verifier ;;
	deployer) deployer ;;
	essai)    essai ;;
	basculer) basculer ;;
	revenir)  revenir ;;
esac
