#!/usr/bin/env bash
#
# Claude OS — Banc du retourneur
#
# Le retourneur est la pièce dont dépend toute la surface d'outils (docs/14).
# Ce banc répond à quatre questions, et il les pose à labwc, pas au code :
#
#   1. LA SURFACE CHANGE-T-ELLE DE TAILLE pendant le retournement ?
#      Elle ne doit pas : labwc 0.8.3 replace un popover ouvert depuis
#      l'ancienne origine de sa surface, et ce projet l'a payé trois fois.
#      C'est la parade de retourneur.h — mesurer au plus large des deux
#      faces — qui se joue ici.
#
#   2. COMBIEN D'IMAGES par retournement, et surtout COMBIEN AU REPOS ?
#      Aucune au repos, sans quoi la discipline d'énergie du projet n'est
#      qu'une intention. La même mesure que pour la glissière.
#
#   3. LE POPOVER EST-IL FERMÉ avant le mouvement ? Le rappel existe ;
#      reste à prouver qu'il est appelé.
#
#   4. LA FACE CACHÉE EST-ELLE HORS D'ATTEINTE ? Un clic là où l'autre face
#      a ses boutons ne doit rien déclencher.
#
# Usage :
#   bash shell/essais/construire.sh retourneur pointeur
#   bash shell/essais/banc-retourneur.sh [dossier-captures]
#
# Les captures et les journaux restent dans le dossier donné (par défaut
# /tmp/claude-os-banc-retourneur). Code de retour : le nombre d'échecs.

set -uo pipefail

ICI="$(cd "$(dirname "$0")" && pwd)"
SORTIE="${1:-/tmp/claude-os-banc-retourneur}"
ESSAI="$ICI/build/retourneur-essai"
POINTEUR="$ICI/build/pointeur"
APP=os.claude.shell.essai-retourneur

[ -x "$ESSAI" ] || { echo "Compiler d'abord : bash $ICI/construire.sh retourneur" >&2; exit 1; }
[ -x "$POINTEUR" ] || { echo "Compiler d'abord : bash $ICI/construire.sh pointeur" >&2; exit 1; }
for outil in labwc grim gapplication dbus-run-session wlr-randr; do
	command -v "$outil" >/dev/null || { echo "$outil est absent" >&2; exit 1; }
done

# UN BUS À NOUS. Sur celui de la session, l'essai — un GtkApplication
# mono-instance — passerait la main à une éventuelle instance déjà lancée.
if [ -z "${BANC_RET_BUS:-}" ]; then
	export BANC_RET_BUS=1
	exec dbus-run-session -- bash "$0" "$@"
fi

mkdir -p "$SORTIE"
CONF="$(mktemp -d)"
export XDG_RUNTIME_DIR="$CONF/run"
mkdir -p "$XDG_RUNTIME_DIR" "$CONF/labwc"
chmod 700 "$XDG_RUNTIME_DIR"
: > "$CONF/labwc/autostart"      # AUCUN autostart : le dock du système
                                 # démarrerait dans le banc.
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
wlr-randr --output HEADLESS-1 --custom-mode 1920x1080 \
	|| echo "wlr-randr a échoué : la sortie garde sa taille par défaut" >&2

# LA TRACE WAYLAND, et c'est elle qui tranche la question 1.
#
# Les compteurs du programme disent ce que GTK croit ; la trace dit ce qui
# est réellement passé au compositeur. Ce qu'on y lira, et comment, est
# expliqué à l'étape 6 -- avec les deux pièges de mesure qui s'y trouvent.
WAYLAND_DEBUG=1 "$ESSAI" > "$SORTIE/essai.log" 2> "$SORTIE/wayland.log" &
sleep 2.5

grep -q '\[banc\] prêt' "$SORTIE/essai.log" \
	|| { echo "l'essai n'a pas démarré :" >&2; tail -20 "$SORTIE/essai.log" >&2; exit 1; }

ECHECS=0
N=0
verifier() {
	# $1 : libellé ; $2 : ce qu'on attend ; $3 : ce qu'on a vu
	N=$((N + 1))
	if [ "$2" = "$3" ]; then
		printf '  ok    %-50s %s\n' "$1" "$3"
	else
		printf '  ÉCHEC %-50s attendu %s, vu %s\n' "$1" "$2" "${3:-rien}"
		ECHECS=$((ECHECS + 1))
	fi
}
compteur() { # $1 : nom du compteur, lu sur la dernière ligne « compte »
	gapplication action "$APP" compter >/dev/null 2>&1
	sleep 0.4
	grep '\[banc\] compte' "$SORTIE/essai.log" | tail -1 \
		| grep -o "$1=[0-9-]*" | cut -d= -f2
}
marque() { echo "=== $* ===" >> "$SORTIE/wayland.log"; }

echo "Parcours :"

# --- 1. le repos : rien ne doit battre -------------------------------------
gapplication action "$APP" remettre >/dev/null 2>&1
sleep 3
verifier "trois secondes de repos : aucune image" 0 "$(compteur allocations)"
grim "$SORTIE/01-avant.png"

# --- 2. un retournement ----------------------------------------------------
marque "retournement 1"
gapplication action "$APP" remettre >/dev/null 2>&1
gapplication action "$APP" retourner >/dev/null 2>&1
sleep 0.12; grim "$SORTIE/02-en-cours.png"     # pris pendant la rotation
sleep 1.2;  grim "$SORTIE/03-arriere.png"

IMAGES="$(compteur allocations)"
printf '  ----- %-50s %s\n' "images pendant le retournement" "$IMAGES"
if [ "${IMAGES:-0}" -ge 5 ] && [ "${IMAGES:-0}" -le 60 ]; then
	printf '  ok    %-50s %s\n' "le mouvement est animé, sans excès" "$IMAGES images"
else
	printf '  ÉCHEC %-50s %s\n' "images par retournement hors bornes 5..60" "${IMAGES:-rien}"
	ECHECS=$((ECHECS + 1))
fi
N=$((N + 1))

verifier "la surface n'a pas changé de taille" 0 "$(compteur changements)"
verifier "le départ a été signalé une fois"    1 "$(compteur departs)"
verifier "la fin a été signalée une fois"      1 "$(compteur fins)"

# --- 3. le repos après mouvement : l'horloge s'est bien retirée ------------
gapplication action "$APP" remettre >/dev/null 2>&1
sleep 3
verifier "trois secondes après : aucune image" 0 "$(compteur allocations)"

# --- 4. le popover est fermé au départ ------------------------------------
marque "popover puis retournement"
gapplication action "$APP" popover >/dev/null 2>&1
sleep 0.6
grep -q '\[banc\] popover ouvert' "$SORTIE/essai.log"
verifier "le popover s'ouvre sur une face immobile" 0 "$?"
grim "$SORTIE/04-popover.png"

gapplication action "$APP" retourner >/dev/null 2>&1
sleep 1.2
verifier "le popover a été fermé au départ" 1 "$(compteur popovers_fermes)"
grim "$SORTIE/05-retour-avant.png"

# --- 5. un popover demandé en plein mouvement est refusé ------------------
gapplication action "$APP" retourner >/dev/null 2>&1
sleep 0.08
gapplication action "$APP" popover >/dev/null 2>&1
sleep 1.4
if grep -q '\[banc\] popover refusé' "$SORTIE/essai.log"; then
	printf '  ok    %-50s %s\n' "popover refusé pendant le mouvement" "refusé"
else
	printf '  ÉCHEC %-50s %s\n' "popover refusé pendant le mouvement" "accepté"
	ECHECS=$((ECHECS + 1))
fi
N=$((N + 1))

# --- 6. LA QUESTION QUI COMMANDE TOUT : la surface a-t-elle bougé ? -------
#
# DANS LA TRACE WAYLAND, et non dans les compteurs du programme : ceux-ci
# disent ce que GTK croit, la trace dit ce qui est passé au compositeur. Et
# c'est cette parole-là qui fait déplacer les popovers de labwc.
#
# DEUX PIÈGES DE MESURE, PAYÉS LE 16 SEPTEMBRE 2026 :
#
#   - La trace de WAYLAND_DEBUG nomme les objets « nom#id », PAS « nom@id ».
#     Un motif écrit avec « @ » ne trouve rien et le banc annonce zéro : un
#     test qui passe parce qu'il ne mesure rien.
#   - gtk4-layer-shell N'ÉMET JAMAIS set_size dans ce montage. Ancrée sur un
#     seul bord, la surface prend la taille de son buffer, et c'est le
#     compositeur qui renvoie un « configure ». Compter les set_size revient
#     donc à compter zéro quoi qu'il arrive.
#
# On compte donc les TAILLES DISTINCTES annoncées par les configure. Une
# seule taille sur toute la séance = la surface n'a jamais bougé.
CONFIGURE='zwlr_layer_surface_v1#[0-9]+\.configure'
TOTAL=$(grep -cE "$CONFIGURE" "$SORTIE/wayland.log" || true)
TAILLES=$(grep -oE "$CONFIGURE\([0-9]+, [0-9]+, [0-9]+\)" "$SORTIE/wayland.log" \
	| sed -E 's/.*\(([0-9]+), ([0-9]+), ([0-9]+)\)/\2x\3/' | sort -u)
DISTINCTES=$(printf '%s\n' "$TAILLES" | grep -c . || true)

printf '  ----- %-50s %s\n' "configure reçus en tout" "$TOTAL"
printf '  ----- %-50s %s\n' "tailles de surface vues" "$(echo $TAILLES)"

# Zéro configure signifierait que le motif ne trouve rien -- le faux négatif
# qu'on vient de payer. On l'exige donc non nul AVANT de juger le reste.
if [ "${TOTAL:-0}" -eq 0 ]; then
	printf '  ÉCHEC %-50s %s\n' "la trace ne dit rien : motif à revoir" "0 configure"
	ECHECS=$((ECHECS + 1))
else
	printf '  ok    %-50s %s\n' "la trace parle (sinon rien n'est mesuré)" "$TOTAL configure"
fi
N=$((N + 1))
verifier "une seule taille de surface sur toute la séance" 1 "$DISTINCTES"

# --- 6bis. le repli sans 3D ----------------------------------------------
#
# CE BANC TOURNE EN RENDU LOGICIEL, et c'est justement le cas où la
# rotation ne peut pas être dessinée : GskCairoRenderer peint en rose vif
# tout nœud portant une perspective. Le retourneur doit donc avoir basculé
# sur l'écrasement -- et l'avoir DIT. Si ce message disparaît un jour, c'est
# que la détection ne marche plus, et le dock virera au rose sur toute
# machine tombée en rendu logiciel.
# DANS LES DEUX JOURNAUX : g_message écrit sur la sortie d'erreur, qui porte
# ici la trace Wayland. Le chercher dans le seul essai.log revenait à ne
# jamais le trouver -- et l'échec, lui, était bien réel.
if grep -qs 'ne sait pas la 3D' "$SORTIE/essai.log" "$SORTIE/wayland.log"; then
	printf '  ok    %-50s %s\n' "repli sans 3D détecté et annoncé" "écrasement"
else
	printf '  ÉCHEC %-50s %s\n' "repli sans 3D non annoncé" "la rotation sera rose"
	ECHECS=$((ECHECS + 1))
fi
N=$((N + 1))

# --- 7. la face cachée est hors d'atteinte --------------------------------
#
# Face avant à l'écran : un clic là où la face arrière a ses boutons ne doit
# rien produire. La rotation n'étant qu'une transformation, GTK n'aurait
# aucune raison d'écarter le clic si child_visible ne le faisait pas.
marque "clic sur la face cachee"
AVANT_ERREURS=$(grep -c 'CRITICAL\|WARNING \*\*' "$SORTIE/essai.log" || true)
"$POINTEUR" clic 200 1040 >/dev/null 2>&1
sleep 0.5
APRES_ERREURS=$(grep -c 'CRITICAL\|WARNING \*\*' "$SORTIE/essai.log" || true)
verifier "un clic hors face ne provoque rien" "$AVANT_ERREURS" "$APRES_ERREURS"
grim "$SORTIE/06-final.png"

echo
grep -hE "CRITICAL|WARNING \*\*|AddressSanitizer|runtime error" "$SORTIE/essai.log" \
	| sed 's/^/  journal : /'
echo "$N vérifications, $ECHECS en échec. Captures et journaux : $SORTIE"
exit "$ECHECS"
