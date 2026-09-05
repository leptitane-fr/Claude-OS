#!/usr/bin/env bash
#
# Claude OS — Audit du système
#
# Répond à quatre questions, sans rien modifier :
#
#   1. Quelles applications sont installées, et le dock sait-il les lancer ?
#   2. Qu'est-ce qui TOURNE, et combien ça coûte en mémoire ?
#   3. Qu'est-ce qui DÉMARRE tout seul ?
#   4. Qu'est-ce qui est installé, et qu'est-ce qui ne sert à rien ?
#
# LECTURE SEULE. Rien n'est purgé, désactivé ni lancé — sauf un essai de
# lancement explicitement demandé par --essai-lancement.
#
#   bash tools/audit-systeme.sh [--essai-lancement] [fichier-de-sortie]
#
# Le rapport est à transmettre tel quel : c'est de lui que sortira la liste
# de ce qu'on retire.

set -u

ESSAI=0
OUT=""
while [ $# -gt 0 ]; do
	case "$1" in
		--essai-lancement) ESSAI=1 ;;
		-h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) OUT="$1" ;;
	esac
	shift
done
OUT="${OUT:-audit-systeme.txt}"
exec > >(tee "$OUT") 2>&1

sec() { printf '\n\033[1;34m── %s\033[0m\n' "$*"; }
val() { printf '  %-32s %s\n' "$1" "$2"; }

# ==========================================================================
sec "1. Les applications, et le lancement depuis le dock"

# Ce que le dock essaie d'afficher.
CONF="$HOME/.config/claude-os/shell.conf"
PINNED="$(sed -n 's/^pinned=//p' "$CONF" 2>/dev/null | tr ';' ' ')"
val "épinglées dans shell.conf" "${PINNED:-<aucune>}"

echo
echo "  Pour chaque épinglée : le binaire, et le .desktop que le dock cherche."
echo "  Un .desktop introuvable donne une icône générique qui ne lance rien."
for id in $PINNED; do
	[ -n "$id" ] || continue
	DESK=""
	for d in "$HOME/.local/share/applications" /usr/local/share/applications \
	         /usr/share/applications; do
		[ -f "$d/$id.desktop" ] && { DESK="$d/$id.desktop"; break; }
	done
	EXEC="$(sed -n 's/^Exec=//p' "$DESK" 2>/dev/null | head -1 | awk '{print $1}')"
	BIN="$(command -v "${EXEC:-$id}" 2>/dev/null)"
	printf '  %-34s desktop=%-46s bin=%s\n' "$id" "${DESK:-INTROUVABLE}" "${BIN:-ABSENT}"
done

echo
echo "  --- les applications que ce système est censé avoir ---"
for p in chromium claude-desktop mousepad foot; do
	ETAT="$(dpkg -l "$p" 2>/dev/null | awk '/^ii/{print "installé " $3}')"
	printf '  %-18s %-28s %s\n' "$p" "${ETAT:-NON INSTALLÉ}" "$(command -v "$p" 2>/dev/null || echo '')"
done

echo
val "entrées .desktop visibles" "$(ls /usr/share/applications/*.desktop 2>/dev/null | wc -l) dans /usr/share/applications"
val "  dont les nôtres" "$(ls /usr/share/applications/claude-os-*.desktop 2>/dev/null | wc -l)"

echo
echo "  --- journal du shell (c'est là que le dock écrit ses erreurs) ---"
JOURNAL="${XDG_STATE_HOME:-$HOME/.local/state}/claude-os/shell.log"
if [ -f "$JOURNAL" ]; then
	tail -30 "$JOURNAL" | sed 's/^/      /'
else
	echo "      absent : $JOURNAL"
fi

if [ "$ESSAI" -eq 1 ]; then
	echo
	echo "  --- essai de lancement par le MÊME chemin que le dock ---"
	# « gio launch » passe par g_app_info_launch sur un GDesktopAppInfo,
	# exactement comme le dock. Si le dock ne lance rien, ceci le dira.
	for id in foot chromium; do
		D="/usr/share/applications/$id.desktop"
		[ -f "$D" ] || { printf '      %-12s pas de .desktop\n' "$id"; continue; }
		if gio launch "$D" 2>&1 | head -3 | sed "s/^/      $id : /"; then
			printf '      %-12s lancé (une fenêtre doit apparaître)\n' "$id"
		fi
	done
fi

# ==========================================================================
sec "2. Ce qui tourne"

val "mémoire utilisée" "$(free -m | awk '/^Mem:/{print $3 " Mo sur " $2}')"
echo
echo "  --- les 20 processus les plus gourmands ---"
ps -eo rss,pid,user,comm --sort=-rss 2>/dev/null | head -21 \
	| awk 'NR==1{printf "      %8s %7s %-10s %s\n","Mo","PID","USER","COMMANDE"; next}
	       {printf "      %8.1f %7s %-10s %s\n", $1/1024, $2, $3, $4}'

echo
echo "  --- services système actifs ---"
systemctl list-units --type=service --state=running --no-legend --no-pager 2>/dev/null \
	| awk '{print "      " $1}'

echo
echo "  --- services utilisateur actifs ---"
systemctl --user list-units --type=service --state=running --no-legend --no-pager 2>/dev/null \
	| awk '{print "      " $1}' || echo "      (aucun gestionnaire utilisateur)"

# ==========================================================================
sec "3. Ce qui démarre tout seul"

echo "  --- /etc/xdg/autostart : lancé par TOUTE session de bureau ---"
# C'est ici que se cachent les agents des environnements installés puis
# oubliés. Chaque fichier est un processus de plus à chaque ouverture.
if [ -d /etc/xdg/autostart ]; then
	for f in /etc/xdg/autostart/*.desktop; do
		[ -e "$f" ] || continue
		printf '      %-46s %s\n' "$(basename "$f")" \
		       "$(sed -n 's/^Name=//p' "$f" | head -1)"
	done
else
	echo "      (répertoire absent)"
fi

echo
echo "  --- ~/.config/autostart ---"
ls "$HOME/.config/autostart"/*.desktop 2>/dev/null | sed 's|.*/|      |' || echo "      (aucun)"

echo
echo "  --- unités utilisateur activées ---"
systemctl --user list-unit-files --state=enabled --no-legend --no-pager 2>/dev/null \
	| awk '{print "      " $1}' || echo "      (aucune)"

echo
echo "  --- services système activés au démarrage ---"
systemctl list-unit-files --type=service --state=enabled --no-legend --no-pager 2>/dev/null \
	| awk '{print "      " $1}'

# ==========================================================================
sec "4. Ce qui est installé"

val "paquets installés" "$(dpkg -l 2>/dev/null | grep -c '^ii')"
val "occupation" "$(df -h / | awk 'NR==2{print $3 " utilisés sur " $2}')"

echo
echo "  --- les 40 plus gros, hors bibliothèques ---"
dpkg-query -Wf '${Installed-Size}\t${Package}\t${Priority}\n' 2>/dev/null \
	| sort -rn | grep -vE '\slib[a-z0-9]' | head -40 \
	| awk '{printf "      %8.1f Mo  %-38s %s\n", $1/1024, $2, $3}'

echo
echo "  --- environnements de bureau étrangers ---"
# Ces familles n'ont rien à faire ici : le bureau est écrit sur mesure.
ETRANGERS=""
for motif in gnome- xfce4- xfce mate- lxde lxqt kde plasma cinnamon budgie \
             nautilus thunar dolphin caja pcmanfm evolution libreoffice \
             gnome-terminal xterm rxvt totem rhythmbox gimp; do
	T="$(dpkg -l 2>/dev/null | awk -v m="$motif" '$1=="ii" && index($2,m)==1 {print $2}')"
	[ -n "$T" ] && ETRANGERS="$ETRANGERS $T"
done
if [ -n "$ETRANGERS" ]; then
	for p in $ETRANGERS; do echo "      $p"; done
else
	echo "      aucun ✓"
fi

echo
echo "  --- paquets marqués « installés automatiquement » et sans dépendant ---"
# Ce que « apt autoremove » retirerait. À lire avant de le lancer.
apt-get -s autoremove 2>/dev/null | sed -n 's/^Remv \([^ ]*\).*/      \1/p' | head -40
[ "$(apt-get -s autoremove 2>/dev/null | grep -c '^Remv')" = "0" ] && echo "      aucun ✓"

# ==========================================================================
sec "5. Éléments de bureau étrangers, côté fichiers"

val "sessions X proposées"  "$(ls /usr/share/xsessions/ 2>/dev/null | tr '\n' ' ')"
val "sessions Wayland"      "$(ls /usr/share/wayland-sessions/ 2>/dev/null | tr '\n' ' ')"
val "x-terminal-emulator"   "$(readlink -f /etc/alternatives/x-terminal-emulator 2>/dev/null || echo '<aucun>')"
val "x-www-browser"         "$(readlink -f /etc/alternatives/x-www-browser 2>/dev/null || echo '<aucun>')"

echo
echo "Rapport écrit dans : $OUT — le transmettre tel quel."
