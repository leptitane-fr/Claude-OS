#!/usr/bin/env bash
#
# Claude OS — Diagnostic de l'écran de connexion ABSENT.
#
# Le pendant de diag-session.sh, qui suppose une session graphique ouverte.
# Celui-ci s'exécute quand il n'y en a pas : console texte, écran noir, ou
# retour du filet de sécurité.
#
#   sudo bash tools/diag-connexion.sh
#
# Ne modifie rien. Produit un rapport à transmettre tel quel.
#
# CE QU'IL VA CHERCHER EN PRIORITÉ
#
# Si le filet a rendu la main sur une console, la panne s'est produite au
# démarrage PRÉCÉDENT — celui-ci est déjà le rétablissement. Le journal du
# démarrage en cours ne dira donc rien. Ce que le filet a collecté AVANT de
# redémarrer est la seule trace de l'échec, et c'est la première chose lue
# ci-dessous.
set -u

OUT="${1:-diag-connexion.txt}"
exec > >(tee "$OUT") 2>&1

sec() { printf '\n\033[1;34m── %s\033[0m\n' "$*"; }
val() { printf '  %-32s %s\n' "$1" "$2"; }

# « ls rep | tr » rend TOUJOURS 0 : le code de retour est celui de tr, jamais
# celui de ls. Un « || echo ABSENT » accroché à ce tuyau ne se déclenche donc
# jamais, et un répertoire manquant s'affichait comme une ligne vide — le
# genre de faux négatif qu'on ne veut pas dans un outil de diagnostic.
contenu() {
	[ -d "$1" ] || { echo "ABSENT <<<<"; return; }
	C="$(ls -A "$1" 2>/dev/null | tr '\n' ' ')"
	[ -n "$C" ] && echo "$C" || echo "(vide) <<<<"
}

[ "$(id -u)" -eq 0 ] || echo "!! Sans sudo, les journaux seront incomplets."

sec "0. Verdict rapide"
CIBLE="$(systemctl get-default 2>/dev/null)"
val "cible par défaut" "$CIBLE"
if [ -e /etc/claude-os/filet-arme ]; then
	val "filet" "ARMÉ — il n'a pas encore jugé"
elif [ -f /var/log/claude-os-filet.log ]; then
	val "filet" "a déjà agi (voir §1)"
else
	val "filet" "jamais exécuté"
fi
case "$CIBLE" in
	multi-user.target) echo "  >>> La machine démarre VOLONTAIREMENT en console."
	                   echo "  >>> C'est le repli du filet, pas une panne de plus." ;;
esac

sec "1. CE QUE LE FILET A COLLECTÉ — la trace de l'échec"
if [ -f /var/log/claude-os-filet.log ]; then
	echo "  --- /var/log/claude-os-filet.log ---"
	sed 's/^/  /' /var/log/claude-os-filet.log
else
	echo "  (aucun journal du filet : il ne s'est jamais déclenché)"
fi
RAPPORT="$(ls -1t /var/log/claude-os-echec-*.txt 2>/dev/null | head -1)"
if [ -n "$RAPPORT" ]; then
	echo
	echo "  ================= $RAPPORT ================="
	sed 's/^/  /' "$RAPPORT"
	echo "  ================= fin du rapport ================="
else
	echo "  (aucun rapport d'échec)"
fi

sec "2. Le gestionnaire de session"
for u in greetd lightdm; do
	E="$(systemctl is-enabled "$u" 2>/dev/null || true)"
	case "$E" in ""|not-found) E="non installé" ;; esac
	val "$u activé" "$E"
	A="$(systemctl is-active "$u" 2>/dev/null || true)"
	val "$u en cours" "${A:-inconnu}"
done
if [ -L /etc/systemd/system/display-manager.service ]; then
	val "display-manager.service" "-> $(readlink -f /etc/systemd/system/display-manager.service)"
else
	val "display-manager.service" "absent"
fi
val "default-display-manager" "$(cat /etc/X11/default-display-manager 2>/dev/null || echo '<absent>')"
# greetd occupe un terminal virtuel. Si un getty tient le même, les deux se
# disputent l'écran et l'un des deux perd — souvent sans message clair.
VT="$(sed -n 's/^ *vt *= *\([0-9]*\).*/\1/p' /etc/greetd/config.toml 2>/dev/null | head -1)"
val "terminal virtuel de greetd" "${VT:-<non défini>}"
if [ -n "$VT" ]; then
	G="$(systemctl is-active "getty@tty$VT" 2>/dev/null || true)"
	val "getty@tty$VT" "${G:-inactif}"
	[ "$G" = "active" ] && echo "  >>> CONFLIT POSSIBLE : un getty tient déjà le tty$VT."
fi

sec "3. Les composants"
for b in /usr/bin/labwc /usr/sbin/greetd /usr/bin/claude-os-connexion \
         /usr/local/bin/claude-os-greeter /usr/local/bin/claude-os-session \
         /usr/local/lib/claude-os/filet-session; do
	[ -x "$b" ] && val "$(basename "$b")" "présent" || val "$(basename "$b")" "ABSENT <<<<"
done
for p in labwc xwayland greetd dbus-user-session libgtk4-layer-shell0; do
	val "paquet $p" "$(dpkg-query -W -f='${db:Status-Status} ${Version}' "$p" 2>/dev/null || echo 'NON INSTALLÉ <<<<')"
done
val "styles" "$(contenu /usr/share/claude-os-shell/style)"

sec "4. La configuration de greetd"
sed 's/^/  /' /etc/greetd/config.toml 2>/dev/null || echo "  /etc/greetd/config.toml ABSENT"
val "compte à ouvrir" "$(cat /etc/claude-os/utilisateur 2>/dev/null || echo '<non défini>')"
val "compte _greetd" "$(getent passwd _greetd >/dev/null && echo 'existe' || echo 'MANQUANT <<<<')"
val "/etc/xdg/labwc-greeter" "$(contenu /etc/xdg/labwc-greeter)"
[ -x /etc/xdg/labwc-greeter/autostart ] \
	&& val "autostart exécutable" "oui" || val "autostart exécutable" "NON <<<<"

sec "5. Le siège et la carte graphique"
# labwc ne peut pas dessiner sans maîtrise du DRM. Un siège mal formé ou un
# /dev/dri absent produit un écran noir sans message côté greeter.
loginctl list-seats 2>/dev/null | sed 's/^/  /'
loginctl seat-status seat0 2>/dev/null | head -20 | sed 's/^/  /'
val "/dev/dri" "$(contenu /dev/dri)"
val "pilote i915" "$(lsmod 2>/dev/null | awk '$1=="i915"{print "chargé"}' || echo '?')"

sec "6. Journaux du démarrage EN COURS"
echo "  --- greetd ---"
journalctl -b -u greetd --no-pager 2>/dev/null | tail -40 | sed 's/^/  /'
echo "  --- labwc ---"
journalctl -b _COMM=labwc --no-pager 2>/dev/null | tail -30 | sed 's/^/  /'
echo "  --- erreurs ---"
journalctl -b --priority=err..alert --no-pager 2>/dev/null | tail -40 | sed 's/^/  /'

sec "7. Journaux du démarrage PRÉCÉDENT — celui qui a échoué"
if journalctl -b -1 -n1 >/dev/null 2>&1; then
	echo "  --- greetd (démarrage -1) ---"
	journalctl -b -1 -u greetd --no-pager 2>/dev/null | tail -40 | sed 's/^/  /'
	echo "  --- erreurs (démarrage -1) ---"
	journalctl -b -1 --priority=err..alert --no-pager 2>/dev/null | tail -40 | sed 's/^/  /'
else
	echo "  Journal NON PERSISTANT : le démarrage précédent n'est pas conservé."
	echo "  Le rapport du §1 est alors la seule trace de l'échec."
	echo "  Pour conserver les prochains :"
	echo "      sudo mkdir -p /var/log/journal && sudo systemd-journald --flush"
fi

sec "8. Sortie de l'écran de connexion"
sed 's/^/  /' /var/log/claude-os-connexion.log 2>/dev/null || echo "  (aucune)"

echo
echo "Rapport écrit dans : $OUT — le transmettre tel quel."
echo
echo "SI TOUT SEMBLE EN PLACE CI-DESSUS, l'essai décisif se fait DEVANT LA"
echo "MACHINE (pas par SSH : il faut un vrai siège pour ouvrir l'écran) :"
echo
echo "    sudo labwc -C /etc/xdg/labwc-greeter"
echo
echo "L'écran de connexion doit apparaître. S'il n'apparaît pas, le message"
echo "d'erreur s'affiche dans la console — c'est lui qu'il faut transmettre."
echo "Pour en sortir sans se connecter : Ctrl-C dans la console."
