#!/usr/bin/env bash
#
# Claude OS — Banc de l'établi
#
# Le premier banc du chantier où il y a quelque chose à REGARDER : le dock
# porte deux faces, et l'une d'elles est remplie par une application.
#
# Ce qu'il éprouve, et qu'aucune lecture de code ne prouve :
#
#   1. Une application qui publie sa barre fait RESTER le dock, au lieu de
#      le faire sortir de l'écran. C'est la rupture avec la règle du
#      11 septembre — voir visibility.h, état ETABLI.
#   2. Une application SANS barre garde l'ancien comportement. Sans cette
#      vérification, on aurait remplacé une règle par une autre au lieu d'en
#      ajouter une.
#   3. La barre arrive entière : lieux, fil d'Ariane, outils, et le lieu
#      courant s'allume.
#   4. Le fil suit l'application quand elle creuse, sans qu'on prévienne le
#      dock — org.gtk.Menus signale ses propres changements.
#   5. Le volet de repli de l'application s'escamote quand le dock prend, et
#      revient quand il rend.
#   6. Le bouton de retour au bureau ramène à la face lanceur.
#
# Usage :
#   bash shell/essais/construire.sh etabli pointeur
#   BUILD=/chemin/vers/build bash shell/essais/banc-etabli.sh [dossier]
#
# BUILD : un répertoire de compilation meson du shell, configuré avec
# --prefix=/usr. Celui du dépôt, shell/build, appartient à root après un
# « --compiler » :
#   meson setup /tmp/shell-banc shell --prefix=/usr && ninja -C /tmp/shell-banc
#
# Code de retour : le nombre de vérifications en échec.

set -uo pipefail

ICI="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$ICI/../build}"
SORTIE="${1:-/tmp/claude-os-banc-etabli}"
TEMOIN="$ICI/build/etabli-essai"
POINTEUR="$ICI/build/pointeur"
APP=os.claude.shell.essai-etabli

[ -x "$BUILD/claude-os-dock" ] || { echo "Pas de dock compilé dans $BUILD" >&2; exit 1; }
[ -x "$TEMOIN" ] || { echo "Compiler d'abord : bash $ICI/construire.sh etabli" >&2; exit 1; }
[ -x "$POINTEUR" ] || { echo "Compiler d'abord : bash $ICI/construire.sh pointeur" >&2; exit 1; }
for outil in labwc grim foot gapplication dbus-run-session wlr-randr; do
	command -v "$outil" >/dev/null || { echo "$outil est absent" >&2; exit 1; }
done

# UN BUS À NOUS. Sur celui de la session, le dock du banc passerait la main
# à celui du bureau, et l'on éprouverait la session en cours.
if [ -z "${BANC_ETABLI_BUS:-}" ]; then
	export BANC_ETABLI_BUS=1
	exec dbus-run-session -- bash "$0" "$@"
fi

mkdir -p "$SORTIE"
CONF="$(mktemp -d)"
export XDG_RUNTIME_DIR="$CONF/run"
mkdir -p "$XDG_RUNTIME_DIR" "$CONF/labwc"
chmod 700 "$XDG_RUNTIME_DIR"
: > "$CONF/labwc/autostart"
cat > "$CONF/labwc/rc.xml" <<'XML'
<?xml version="1.0"?>
<labwc_config><core><decoration>server</decoration><gap>0</gap></core>
<theme><dropShadows>no</dropShadows></theme></labwc_config>
XML

export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
export WLR_HEADLESS_OUTPUTS=1 GDK_BACKEND=wayland GSK_RENDERER=cairo
unset WAYLAND_DISPLAY DISPLAY

labwc -C "$CONF/labwc" > "$SORTIE/labwc.log" 2>&1 &
trap 'kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$CONF"' EXIT

for _ in $(seq 1 40); do
	s=$(ls "$XDG_RUNTIME_DIR"/wayland-[0-9] 2>/dev/null | head -1)
	[ -n "$s" ] && break
	sleep 0.25
done
[ -n "${s:-}" ] || { echo "labwc n'a pas démarré :" >&2; tail "$SORTIE/labwc.log" >&2; exit 1; }
export WAYLAND_DISPLAY="$(basename "$s")"
wlr-randr --output HEADLESS-1 --custom-mode 1920x1080 >/dev/null 2>&1

# Les transitions du dock ne sont écrites qu'en débogage.
export G_MESSAGES_DEBUG=all

command -v swaybg >/dev/null && { swaybg -c '#3c4043' > "$SORTIE/swaybg.log" 2>&1 & }
"$BUILD/claude-os-dock" > "$SORTIE/dock.log" 2>&1 &
sleep 2.5

ECHECS=0
N=0
etat() { grep -o 'visibilite : [a-z]*' "$SORTIE/dock.log" | tail -1 | cut -d' ' -f3; }
verifier() {
	N=$((N + 1))
	if [ "$2" = "$3" ]; then printf '  ok    %-52s %s\n' "$1" "$3"
	else printf '  ÉCHEC %-52s attendu %s, vu %s\n' "$1" "$2" "${3:-rien}"
	     ECHECS=$((ECHECS + 1)); fi
}
capture() { N=$((N)); grim "$SORTIE/$1.png" 2>/dev/null; }

echo "Parcours :"

# --- 1. une application SANS barre : la règle du 11 septembre tient -------
foot -e sleep 300 > "$SORTIE/foot.log" 2>&1 &
sleep 1.5
verifier "application sans barre : le dock s'efface" cache "$(etat)"
capture 01-sans-barre

# --- 2. l'application témoin, avec sa barre ------------------------------
"$TEMOIN" > "$SORTIE/temoin.log" 2>&1 &
sleep 2.5
verifier "application avec barre : le dock reste" etabli "$(etat)"
capture 02-etabli

grep -q '\[banc\] prêt' "$SORTIE/temoin.log" \
	|| { echo "le témoin n'a pas démarré :" >&2; tail -20 "$SORTIE/temoin.log" >&2; }

# --- 3. le repli s'est escamoté -----------------------------------------
gapplication action "$APP" etat >/dev/null 2>&1
sleep 0.4
REPLI=$(grep '\[banc\] etat' "$SORTIE/temoin.log" | tail -1 | grep -o 'repli_visible=[01]' | cut -d= -f2)
verifier "le volet interne de l'application est escamoté" 0 "${REPLI:-?}"

PRISE=$(grep '\[banc\] etat' "$SORTIE/temoin.log" | tail -1 | grep -o 'prise=[01]' | cut -d= -f2)
verifier "l'application sait que le dock a pris" 1 "${PRISE:-?}"

# --- 4. la barre est arrivée entière -------------------------------------
#
# Lue dans le journal du dock : l'établi se plaint de ce qu'il ne sait pas
# dessiner, et se tait sur ce qu'il a posé. On vérifie donc l'absence de
# plainte sur les zones connues, et la PRÉSENCE de celle qu'on attend --
# l'auvent, qui est l'étape 4 et qui doit se signaler.
if grep -q 'zone inconnue' "$SORTIE/dock.log"; then
	printf '  ÉCHEC %-52s %s\n' "aucune zone inconnue" "$(grep -o 'zone inconnue «[^»]*»' "$SORTIE/dock.log" | head -1)"
	ECHECS=$((ECHECS + 1))
else
	printf '  ok    %-52s %s\n' "toutes les zones déclarées sont connues" "aucune plainte"
fi
N=$((N + 1))


# --- 5. le lieu courant s'allume, et suit ---------------------------------
#
# PAR LE CHEMIN DU CONTRAT, et pas par « gapplication action » : l'action
# « aller » vit dans le groupe exporté sous /os/claude/shell/outils, pas sur
# l'application. gapplication ne voit que les secondes, et répondait
# silencieusement sans rien faire -- le banc annonçait un échec réel sur une
# cible qui n'existait pas. C'est exactement le chemin que le dock emprunte
# quand on clique un lieu.
gdbus call --session --dest "$APP" --object-path /os/claude/shell/outils \
	--method org.gtk.Actions.Activate \
	aller "[<'file:///home/stef/Images'>]" "{}" >/dev/null 2>&1
sleep 0.8
capture 03-lieu-images
ALLE=$(grep -c '\[banc\] aller file:///home/stef/Images' "$SORTIE/temoin.log")
verifier "l'application a reçu la navigation" 1 "$ALLE"

# --- 6. le fil suit le modèle qui change ---------------------------------
for _ in 1 2 3; do gapplication action "$APP" creuser >/dev/null 2>&1; sleep 0.4; done
# LE MODÈLE ARRIVE EN PLUSIEURS SALVES, et deux secondes ne sont pas du luxe :
# une capture prise trop tôt montrait deux étapes sur quatre, et l'on aurait
# pu conclure à un fil tronqué. Ce n'est pas le fil qui tronque, c'est le bus
# qui n'a pas fini de parler.
sleep 2
capture 04-fil-profond
CREUSE=$(grep -c '\[banc\] fil creusé' "$SORTIE/temoin.log")
verifier "le fil s'est allongé trois fois" 3 "$CREUSE"

# CE QUE L'ÉTABLI PORTE VRAIMENT, lu dans son propre compte rendu : on ne
# compte pas des widgets depuis un autre processus.
PORTE=$(grep -o 'établi : [0-9]* lieux, [0-9]* étapes, [0-9]* outils' "$SORTIE/dock.log" | tail -1)
verifier "la barre est arrivée entière" "établi : 4 lieux, 4 étapes, 1 outils" "$PORTE"
verifier "le dock est toujours en établi" etabli "$(etat)"

# --- 6ter. L'AUVENT -------------------------------------------------------
#
# Le volet qui monte, et la seule partie du contrat où le dock prend le
# clavier. Trois questions, et la troisième est celle qui fait peur :
#
#   - le volet s'ouvre-t-il, et la hauteur de la surface change-t-elle ?
#   - la frappe arrive-t-elle à l'application ? C'est ce pour quoi
#     `frappe` a été écrit : aucun banc d'ici ne savait taper.
#   - le clavier repart-il à la fermeture ? Un dock qui garde le clavier de
#     la session pour un champ refermé serait pire que pas d'auvent du tout.
gapplication action os.claude.shell.dock outil 0 >/dev/null 2>&1
sleep 1
capture 06-auvent

if grep -q 'auvent : ouvert, clavier exclusif' "$SORTIE/dock.log"; then
	printf '  ok    %-52s %s\n' "l'auvent s'ouvre et le dock prend le clavier" "exclusif"
else
	printf '  ÉCHEC %-52s %s\n' "l'auvent devait s'ouvrir et prendre le clavier" "rien"
	ECHECS=$((ECHECS + 1))
fi
N=$((N + 1))

"$ICI/build/frappe" "mire" > "$SORTIE/frappe.log" 2>&1
sleep 1
TAPE=$(grep '\[banc\] chercher' "$SORTIE/temoin.log" | tail -1 | sed 's/.*« \(.*\) ».*/\1/')
verifier "la frappe est arrivée à l'application" "mire" "${TAPE:-rien}"
capture 07-auvent-saisi

# Échap referme : le geste qu'on essaie sans qu'on vous l'explique.
"$ICI/build/frappe" --touche Escape >> "$SORTIE/frappe.log" 2>&1
sleep 1
VIDE=$(grep -c '\[banc\] chercher «  »' "$SORTIE/temoin.log")
verifier "la fermeture envoie la valeur vide" 1 "${VIDE:-0}"

if grep -q 'auvent : ferme, clavier none' "$SORTIE/dock.log"; then
	printf '  ok    %-52s %s\n' "le clavier est rendu à l'application" "none"
else
	printf '  ÉCHEC %-52s %s\n' "le clavier devait être rendu" "toujours pris"
	ECHECS=$((ECHECS + 1))
fi
N=$((N + 1))
capture 08-auvent-ferme

# --- 6quater. un clic à côté referme l'auvent ----------------------------
#
# LA TROISIÈME PORTE DE SORTIE, et la plus importante : le volet confisque le
# clavier (EXCLUSIVE). S'il ne se refermait qu'à Échap, une application
# resterait muette sans qu'on sache pourquoi.
gapplication action os.claude.shell.dock outil 0 >/dev/null 2>&1
sleep 1
"$POINTEUR" clic 300 300 >/dev/null 2>&1
sleep 1
FERME=$(grep -c 'auvent : ferme' "$SORTIE/dock.log")
verifier "un clic à côté referme l'auvent" 2 "${FERME:-0}"
verifier "et le dock est toujours en établi" etabli "$(etat)"

# --- 6bis. le bouton de retour au bureau ---------------------------------
#
# Par l'action du bus, qui est exactement ce que fait le bouton : le banc ne
# sait pas où labwc a posé un widget, et un clic à coordonnées fixes
# éprouverait surtout notre capacité à deviner.
#
# CE QU'IL NE DOIT PAS FAIRE : toucher à la visibilité. Le premier jet
# appelait cacher() puis convoquer() -- et convoquer, relisant l'état
# souhaité, revenait à l'établi. Le bouton ne faisait rien, et le journal
# disait « etabli » dans les deux cas. D'où cette vérification : l'état reste
# ETABLI, et c'est la FACE qui change.
face() { grep -o 'face : [a-z]*' "$SORTIE/dock.log" | tail -1 | cut -d' ' -f3; }

gapplication action os.claude.shell.dock bureau >/dev/null 2>&1
sleep 1.2
verifier "le retour au bureau ne change pas l'état" etabli "$(etat)"
verifier "mais il change la face" bureau "$(face)"
capture 05-retour-bureau

# ET LE RETOUR DU RETOUR. Un aller sans retour n'est pas une bascule : une
# fois revenu au lanceur, il faut pouvoir retrouver les outils de la fenêtre
# devant sans passer par une autre application. Constaté manquant à l'écran
# sur MADOO le 16 septembre 2026.
gapplication action os.claude.shell.dock outils >/dev/null 2>&1
sleep 1.2
verifier "et l'on peut revenir aux outils" etabli "$(face)"
capture 05b-retour-outils

# --- 7. revenir à une application sans barre ------------------------------
#
# EN OUVRANT UNE FENÊTRE, ET PAS EN CLIQUANT À L'AVEUGLE. Un clic à des
# coordonnées fixes suppose de savoir où labwc a posé quoi ; il tombait sur
# le témoin et le banc accusait le dock d'un immobilisme qui était le sien.
# Une fenêtre qui s'ouvre s'active : c'est déterministe, et c'est le cas
# d'usage réel.
foot -e sleep 300 > "$SORTIE/foot2.log" 2>&1 &
sleep 1.5
verifier "une application sans barre passe devant : le dock s'efface" cache "$(etat)"
capture 05b-retour-foot

# --- 8. et revenir au témoin le ramène ------------------------------------
#
# En fermant celle du dessus : le témoin redevient la fenêtre active, et le
# dock doit retrouver son établi sans qu'on lui dise rien.
pkill -f "foot -e sleep 300"
sleep 1.8
verifier "le témoin redevient actif : l'établi revient" etabli "$(etat)"
capture 06-retour-temoin

# --- 8bis. LA PILULE RESTE EN BAS, nappe tendue --------------------------
#
# Le dock tend sa fenêtre à tout l'écran quand il est convoqué par-dessus une
# application, et quand l'auvent est ouvert. La face doit rester collée au
# bas : centrée dans 1080 px, la pilule partirait au milieu de l'écran. C'est
# arrivé, et aucun test d'état ne l'aurait dit -- le journal annonçait le bon
# état, et la pilule était introuvable.
# On ne regarde QUE les allocations où la fenêtre fait tout l'écran : c'est
# le seul cas où la question se pose, et une ligne prise au hasard mesurerait
# une pilule dont la surface a déjà la bonne taille.
PLEIN=$(grep -o 'pilule : [0-9]*,[0-9]* [0-9]*x[0-9]* dans 1920x1080' "$SORTIE/dock.log")
NB=$(printf '%s\n' "$PLEIN" | grep -c . || true)
HORS=$(printf '%s\n' "$PLEIN" | awk 'NF {split($3,p,","); split($4,t,"x"); if (p[2]+t[2] < 1060) n++} END {print n+0}')
printf '  ----- %-52s %s\n' "allocations plein écran examinées" "$NB"
if [ "${NB:-0}" -eq 0 ]; then
	printf '  ÉCHEC %-52s %s\n' "aucune allocation plein écran : rien mesuré" "0"
	ECHECS=$((ECHECS + 1))
	N=$((N + 1))
else
	verifier "la pilule reste collée au bas, nappe tendue" 0 "$HORS"
fi
capture 09-nappe

# --- 9. le témoin s'en va : la barre part avec lui ------------------------
pkill -f "etabli-essai"
sleep 1.5
ETAT9="$(etat)"
if [ "$ETAT9" = "cache" ] || [ "$ETAT9" = "bureau" ]; then
	printf '  ok    %-52s %s\n' "l'application partie, le dock quitte l'établi" "$ETAT9"
else
	printf '  ÉCHEC %-52s attendu cache ou bureau, vu %s\n' "l'application partie" "$ETAT9"
	ECHECS=$((ECHECS + 1))
fi
N=$((N + 1))
capture 07-sans-temoin

echo
grep -hE "CRITICAL|WARNING \*\*|AddressSanitizer|runtime error" \
	"$SORTIE/dock.log" "$SORTIE/temoin.log" | sed 's/^/  journal : /'
echo "$N vérifications, $ECHECS en échec. Captures et journaux : $SORTIE"
exit "$ECHECS"
