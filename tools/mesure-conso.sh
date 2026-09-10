#!/usr/bin/env bash
#
# Claude OS — Banc de mesure de consommation
#
# Objectif : donner un chiffre comparable, et refuser d'en donner un quand la
# machine n'est pas en état d'en produire. Le lecteur vidéo « claude-os-video »
# se réclame d'un record d'économie d'énergie ; ce script est ce qui permet de
# l'établir ou de le démentir. Il ne modifie rien : lectures seules.
#
# Usage :
#   bash tools/mesure-conso.sh -d 30 "repos, bureau seul"
#   bash tools/mesure-conso.sh -d 60 --contre "chromium --app=file:///.../mire.mp4"
#   bash tools/mesure-conso.sh --tableau            # relit les relevés passés
#
# Le banc REFUSE de mesurer écran éteint ou verrouillé : voir plus bas.
#
# Les relevés s'accumulent dans ~/.local/state/claude-os/conso.tsv, pour que
# deux mesures faites à deux jours d'écart restent comparables.
#
# ------------------------------------------------------------------- méthode
#
# TROIS INSTRUMENTS, ET ILS NE DISENT PAS LA MÊME CHOSE :
#
#   package-0  le SoC seul (cœurs + GPU + uncore). C'est lui qui bouge quand
#              on change un chemin de décodage. C'est celui qui EXPLIQUE, et
#              c'est le seul instrument disponible sur secteur.
#   batterie   la vérité de terrain : toute la plateforme, écran compris.
#              C'est elle qui répond à « la batterie tient-elle plus
#              longtemps », et elle seule. Indisponible sur secteur — la
#              batterie de MADOO annonce alors current_now = 0.
#   psys       MESURÉ INUTILISABLE SUR MADOO, le 10 septembre 2026. Le domaine
#              existe, mais « enabled » vaut 0 et le compteur avance de 61 mW
#              pour une plateforme qui en consomme près de sept. Le script le
#              lit encore, uniquement pour DIRE qu'il ne compte pas : un zéro
#              affiché sans avertissement se serait retrouvé dans une
#              conclusion.
#
# Conséquence de méthode, et elle est contraignante : AUCUN record d'autonomie
# ne peut être établi sur secteur. Les comparaisons de chemins de décodage se
# font au package-0, les chiffres d'autonomie se font CÂBLE DÉBRANCHÉ.
#
# RAPL est en lecture root (0400 depuis les attaques par canal auxiliaire).
# Un « claude-os-root cat » par échantillon coûterait un fork toutes les deux
# secondes DANS la mesure elle-même. L'échantillonneur privilégié est donc
# lancé UNE fois pour toute la durée, et écrit sa trace dans un fichier.
#
# Les compteurs energy_uj débordent — 262 J sur cette machine, soit moins de
# quarante secondes à 7 W. Le débordement est traité, pas ignoré : sans cela
# toute mesure de plus d'une minute serait fausse d'un multiple entier.

set -uo pipefail

# awk suit la locale : sans cela le tableau se remplit de virgules
# decimales, et la colonne cesse d'etre calculable.
export LC_ALL=C

DUREE=30
INTERVALLE=2
LIBELLE=""
COMMANDE=""
QUAND_MEME=""
ETAT="${XDG_STATE_HOME:-$HOME/.local/state}/claude-os"
TABLEAU="$ETAT/conso.tsv"

usage() { sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
	case "$1" in
		-d|--duree)   DUREE="${2:?durée en secondes}"; shift 2 ;;
		-i|--intervalle) INTERVALLE="${2:?intervalle en secondes}"; shift 2 ;;
		--contre)     COMMANDE="${2:?commande à mesurer}"; shift 2 ;;
		--tableau)    [ -s "$TABLEAU" ] && column -t -s $'\t' "$TABLEAU" \
		                  || echo "Aucun relevé dans $TABLEAU" ; exit 0 ;;
		--quand-meme) QUAND_MEME=1; shift ;;
		-h|--help)    usage; exit 0 ;;
		-*)           echo "Option inconnue : $1" >&2; exit 2 ;;
		*)            LIBELLE="$1"; shift ;;
	esac
done

[ -n "$LIBELLE" ] || LIBELLE="${COMMANDE:-sans libellé}"
mkdir -p "$ETAT"

# ------------------------------------------------------- état de la machine
#
# Une mesure faite dans de mauvaises conditions est pire qu'une mesure
# absente : on la cite. Ces avertissements ne sont donc pas décoratifs.

SECTEUR=$(cat /sys/class/power_supply/AC/online 2>/dev/null || echo "?")
PSYS_OK=$(claude-os-root cat /sys/class/powercap/intel-rapl:1/enabled 2>/dev/null | tr -d ' \n')
BAT_STATUT=$(cat /sys/class/power_supply/BAT0/status 2>/dev/null || echo "?")

echo "== Banc de mesure Claude OS =="
echo "Libellé      : $LIBELLE"
echo "Durée        : ${DUREE} s, un échantillon toutes les ${INTERVALLE} s"
if [ "$SECTEUR" = "1" ]; then
	echo "Alimentation : SECTEUR — la batterie ne mesure rien."
	echo "  Le package-0 compare des chemins de décodage ; il ne dit PAS"
	echo "  l'autonomie. Pour un chiffre d'autonomie : débrancher."
else
	echo "Alimentation : BATTERIE ($BAT_STATUT) — la plateforme entière est mesurée."
fi
[ "$PSYS_OK" = "1" ] || echo "  psys : compteur désactivé sur cette machine, ignoré (voir l'en-tête)."

# ------------------------------------------------- l'écran est-il allumé ?
#
# CE CONTRÔLE A COÛTÉ UNE SÉRIE ENTIÈRE, le 10 septembre 2026.
#
# Cinq mesures de « lecture vidéo en plein écran » ont été prises alors que la
# veille progressive avait éteint le rétroéclairage, puis que le verrou de
# session s'était monté. Sous ext-session-lock-v1 le compositeur masque toutes
# les fenêtres : plus un « frame callback », plus une image composée, et le
# GPU au repos. Les chiffres étaient plus bas — donc flatteurs — et
# l'explication qu'on leur a d'abord donnée (« le plein écran masque Claude
# Desktop ») était fausse. Rien n'était affiché du tout.
#
# Une mesure d'affichage écran éteint ne mesure pas ce qu'on croit. Le banc
# refuse donc de la prendre, et --quand-meme reste possible pour qui mesure
# justement l'écran éteint.

RETRO=$(cat /sys/class/backlight/*/brightness 2>/dev/null | head -1)
# « pgrep -x » tronque a 15 caracteres et ne trouverait jamais
# « claude-os-verrou » ; les crochets evitent que pgrep ne se voie lui-meme.
VERROU=$(pgrep -f '[c]laude-os-verrou' >/dev/null && echo oui || echo non)
PROBLEME=""
[ "${RETRO:-1}" = "0" ] && PROBLEME="le rétroéclairage est éteint"
[ "$VERROU" = "oui" ]   && PROBLEME="${PROBLEME:+$PROBLEME, et }l'écran est verrouillé"

if [ -n "$PROBLEME" ]; then
	echo "Écran        : $PROBLEME"
	if [ -z "$QUAND_MEME" ]; then
		echo
		echo "REFUS DE MESURER : rien n'est composé dans cet état, et le chiffre" >&2
		echo "obtenu serait plus bas que la réalité — donc trompeur." >&2
		echo "Déverrouiller, ou relancer avec --quand-meme si c'est justement" >&2
		echo "l'écran éteint que l'on veut mesurer." >&2
		exit 3
	fi
	echo "  --quand-meme : mesure prise malgré tout, le relevé le portera."
	LIBELLE="$LIBELLE [écran éteint ou verrouillé]"
fi

CHARGE=$(cut -d' ' -f1 /proc/loadavg)
echo "Charge avant : $CHARGE"
case "$CHARGE" in
	0.[0-4]*|0) ;;
	*) echo "  ATTENTION : la machine n'est pas au repos avant la mesure." ;;
esac
echo

# ------------------------------------------------- échantillonneur privilégié
#
# Un seul processus root pour toute la durée. Il écrit une ligne par
# échantillon : horodatage, puis les compteurs bruts. Aucun calcul en root.

TRACE=$(mktemp /tmp/claude-os-conso.XXXXXX)
trap 'rm -f "$TRACE"' EXIT

N=$(( DUREE / INTERVALLE ))
[ "$N" -ge 2 ] || { echo "Durée trop courte : il faut au moins deux échantillons." >&2; exit 2; }

RAPL_PSYS=/sys/class/powercap/intel-rapl:1/energy_uj
RAPL_PKG=/sys/class/powercap/intel-rapl:0/energy_uj
RAPL_CORE=/sys/class/powercap/intel-rapl:0:0/energy_uj
RAPL_UNCORE=/sys/class/powercap/intel-rapl:0:1/energy_uj

# Le « sleep » est dans la boucle root ; c'est ce qui évite un fork par
# échantillon. La boucle ne fait que lire et écrire sur sa sortie standard.
ECHANTILLONNEUR='
for i in $(seq '"$N"'); do
    printf "%s\t%s\t%s\t%s\t%s\n" \
        "$(date +%s.%N)" \
        "$(cat '"$RAPL_PSYS"' 2>/dev/null || echo -1)" \
        "$(cat '"$RAPL_PKG"' 2>/dev/null || echo -1)" \
        "$(cat '"$RAPL_CORE"' 2>/dev/null || echo -1)" \
        "$(cat '"$RAPL_UNCORE"' 2>/dev/null || echo -1)"
    sleep '"$INTERVALLE"'
done'

# Repères non privilégiés, pris avant et après : ils content ce que RAPL ne
# dit pas — combien de travail a été fait pour cette énergie.
lire_cpu()  { awk '/^cpu /{ total=0; for(i=2;i<=NF;i++) total+=$i; print total, $5 }' /proc/stat; }
lire_irq()  { awk 'NR>1 { for(i=2;i<=5;i++) t+=$i } END { print t+0 }' /proc/interrupts; }
lire_bat()  {
	local i v
	i=$(cat /sys/class/power_supply/BAT0/current_now 2>/dev/null || echo 0)
	v=$(cat /sys/class/power_supply/BAT0/voltage_now 2>/dev/null || echo 0)
	awk -v i="$i" -v v="$v" 'BEGIN{ printf "%.3f", (i/1000000)*(v/1000000) }'
}

CPU_AV=$(lire_cpu); IRQ_AV=$(lire_irq); BAT_AV=$(lire_bat)
GPU_MHZ_AV=$(cat /sys/class/drm/card0/gt_act_freq_mhz 2>/dev/null || echo -1)

claude-os-root -c "$ECHANTILLONNEUR" > "$TRACE" &
PID_MESURE=$!

# La commande à mesurer tourne DANS la fenêtre de mesure, pas à côté.
if [ -n "$COMMANDE" ]; then
	echo "Lancement de : $COMMANDE"
	setsid bash -c "$COMMANDE" &
	PID_SUJET=$!
	trap 'kill -- -"$PID_SUJET" 2>/dev/null; rm -f "$TRACE"' EXIT
fi

echo "Mesure en cours…"
wait "$PID_MESURE"
CODE=$?

if [ -n "$COMMANDE" ]; then
	kill -- -"$PID_SUJET" 2>/dev/null
	trap 'rm -f "$TRACE"' EXIT
fi

CPU_AP=$(lire_cpu); IRQ_AP=$(lire_irq); BAT_AP=$(lire_bat)
GPU_MHZ_AP=$(cat /sys/class/drm/card0/gt_act_freq_mhz 2>/dev/null || echo -1)

# INVARIANT N°4 : une commande qui échoue doit parler.
if [ "$CODE" -ne 0 ]; then
	echo "L'échantillonneur privilégié a rendu $CODE — mesure abandonnée." >&2
	exit 1
fi
if [ "$(grep -c '' "$TRACE")" -lt 2 ]; then
	echo "Moins de deux échantillons recueillis — rien à calculer." >&2
	echo "Trace brute :" >&2; cat "$TRACE" >&2
	exit 1
fi
if grep -q -- "-1" "$TRACE"; then
	echo "ATTENTION : au moins un compteur RAPL est resté illisible." >&2
fi

# ----------------------------------------------------------------- calculs
#
# Puissance = énergie consommée / temps écoulé, entre le premier et le dernier
# échantillon. Le débordement du compteur s'ajoute d'un tour complet.

RESULTAT=$(awk -v plage=262143328850 '
	NR == 1 { t0=$1; p0=$2; k0=$3; c0=$4; u0=$5; tp=$1; pp=$2; kp=$3; cp=$4; up=$5; next }
	{
		# Un compteur qui recule a deborde : on lui rend son tour.
		if ($2 < pp) deb_p += plage
		if ($3 < kp) deb_k += plage
		if ($4 < cp) deb_c += plage
		if ($5 < up) deb_u += plage
		tp=$1; pp=$2; kp=$3; cp=$4; up=$5
		tn=$1; pn=$2; kn=$3; cn=$4; un=$5
	}
	END {
		dt = tn - t0
		if (dt <= 0) { print "ERREUR"; exit }
		printf "%.2f %.2f %.2f %.2f %.1f",
			(pn + deb_p - p0) / 1000000 / dt,
			(kn + deb_k - k0) / 1000000 / dt,
			(cn + deb_c - c0) / 1000000 / dt,
			(un + deb_u - u0) / 1000000 / dt,
			dt
	}' "$TRACE")

[ "$RESULTAT" = "ERREUR" ] && { echo "Horodatages incohérents." >&2; exit 1; }
read -r W_PSYS W_PKG W_CORE W_UNCORE DT <<< "$RESULTAT"

OCCUPATION=$(awk -v a="$CPU_AV" -v b="$CPU_AP" 'BEGIN {
	split(a, x, " "); split(b, y, " ")
	dt = y[1]-x[1]; di = y[2]-x[2]
	if (dt <= 0) { print "?" } else { printf "%.1f", 100*(1-di/dt) }
}')
IRQ_S=$(awk -v a="$IRQ_AV" -v b="$IRQ_AP" -v d="$DT" 'BEGIN { printf "%.0f", (b-a)/d }')
BAT_MOY=$(awk -v a="$BAT_AV" -v b="$BAT_AP" 'BEGIN { printf "%.2f", (a+b)/2 }')

echo
echo "== Résultat =="
if [ "$PSYS_OK" = "1" ]; then
	printf "  Plateforme (psys)   %7s W\n" "$W_PSYS"
fi
printf "  SoC (package-0)     %7s W   <- ce qui compare les chemins\n" "$W_PKG"
printf "     dont cœurs       %7s W\n" "$W_CORE"
printf "     dont uncore/GPU  %7s W\n" "$W_UNCORE"
if [ "$SECTEUR" = "1" ]; then
	printf "  Batterie                  —     (sur secteur, non mesurable)\n"
else
	printf "  Batterie            %7s W   <- LE chiffre d'autonomie\n" "$BAT_MOY"
fi
printf "  Occupation CPU      %7s %%   sur 4 cœurs\n" "$OCCUPATION"
printf "  Interruptions       %7s /s   <- le coût du reveil, invisible en watts moyens\n" "$IRQ_S"
printf "  Fréquence GPU       %7s -> %s MHz\n" "$GPU_MHZ_AV" "$GPU_MHZ_AP"
echo

[ -s "$TABLEAU" ] || printf "date\tlibellé\tdurée\tpkg_W\tcore_W\tuncore_W\tbat_W\tcpu_%%\tirq_s\n" > "$TABLEAU"
[ "$SECTEUR" = "1" ] && BAT_COL="secteur" || BAT_COL="$BAT_MOY"
printf "%s\t%s\t%.0f\t%s\t%s\t%s\t%s\t%s\t%s\n" \
	"$(date +%Y-%m-%dT%H:%M)" "$LIBELLE" "$DT" \
	"$W_PKG" "$W_CORE" "$W_UNCORE" "$BAT_COL" "$OCCUPATION" "$IRQ_S" >> "$TABLEAU"
echo "Relevé ajouté à $TABLEAU  (--tableau pour tout relire)"
