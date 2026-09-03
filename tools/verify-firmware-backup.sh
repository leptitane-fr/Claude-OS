#!/usr/bin/env bash
#
# Claude OS - Vérification d'une sauvegarde de firmware d'origine
#
# CONTEXTE
# Sans programmateur SPI externe, la sauvegarde du firmware d'origine est le
# SEUL moyen de revenir en arrière. Une sauvegarde corrompue ne se découvre
# qu'au moment où l'on en a besoin, c'est-à-dire trop tard. Ce script la
# valide immédiatement après sa création.
#
# MÉTHODE
# La règle est de lire la puce DEUX FOIS et de comparer les deux dumps : une
# lecture SPI peut échouer silencieusement et produire un fichier de la bonne
# taille mais au contenu faux. Deux lectures identiques rendent ce scénario
# très improbable.
#
# USAGE
#   bash verify-firmware-backup.sh dump1.rom            # contrôles sur un dump
#   bash verify-firmware-backup.sh dump1.rom dump2.rom  # + comparaison (recommandé)
#
# CODES DE RETOUR
#   0  sauvegarde exploitable
#   1  réserves : lire les avertissements avant de flasher
#   2  sauvegarde inutilisable : NE PAS FLASHER
#
# Ce script est en lecture seule sur les dumps analysés.

set -uo pipefail

VERSION="0.2.0"
RC=0

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
grn()   { printf '\033[32m%s\033[0m\n' "$*"; }
ylw()   { printf '\033[33m%s\033[0m\n' "$*"; }
hdr()   { printf '\n\033[1m%s\033[0m\n' "$*"; }

ok()    { grn "  [ok]    $*"; }
warn()  { ylw "  [!]     $*"; [ "$RC" -lt 1 ] && RC=1; return 0; }
fail()  { red "  [ÉCHEC] $*"; RC=2; return 0; }
info()  { printf '  %s\n' "$*"; }

usage() { sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; }

[ $# -ge 1 ] || { usage; exit 2; }
case "$1" in -h|--help) usage; exit 0 ;; esac

D1="$1"
D2="${2:-}"

# Somme de contrôle, avec repli si sha256sum est absent (shell ChromeOS minimal)
checksum() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | awk '{print $1}'
	elif command -v openssl >/dev/null 2>&1; then
		openssl dgst -sha256 "$1" | awk '{print $NF}'
	else
		echo "[indisponible : ni sha256sum ni openssl]"
	fi
}

filesize() { wc -c < "$1" | tr -d ' '; }

# --------------------------------------------------------- contrôles de base

check_readable() {
	local f="$1"
	if [ ! -f "$f" ]; then fail "$f : fichier introuvable"; return 1; fi
	if [ ! -r "$f" ]; then fail "$f : illisible"; return 1; fi
	return 0
}

# La taille doit correspondre à une puce SPI réelle. Une taille arbitraire
# signale une lecture tronquée.
check_size() {
	local f="$1" sz mib
	sz="$(filesize "$f")"
	mib=$(( sz / 1048576 ))
	info "taille : $sz octets (~${mib} Mio)"
	case "$sz" in
		4194304|8388608|16777216|33554432)
			ok "taille cohérente avec une puce SPI (${mib} Mio)" ;;
		*)
			fail "taille non conforme : une puce SPI fait 4, 8, 16 ou 32 Mio. Lecture probablement tronquée." ;;
	esac
}

# Une lecture ratée renvoie typiquement un fichier entièrement à 0x00 ou 0xFF.
check_not_blank() {
	local f="$1" sz nonzero nonff pct
	sz="$(filesize "$f")"
	[ "$sz" -gt 0 ] || { fail "fichier vide"; return; }

	nonzero="$(tr -d '\000' < "$f" | wc -c | tr -d ' ')"
	nonff="$(tr -d '\377' < "$f" | wc -c | tr -d ' ')"

	if [ "$nonzero" -eq 0 ]; then
		fail "dump entièrement à 0x00 : la lecture de la puce a échoué"
		return
	fi
	if [ "$nonff" -eq 0 ]; then
		fail "dump entièrement à 0xFF : puce non lue ou vierge"
		return
	fi

	pct=$(( nonzero * 100 / sz ))
	info "octets non nuls : ${pct} % du fichier"
	if [ "$pct" -lt 5 ]; then
		fail "moins de 5 % de contenu non nul : dump quasi vide, lecture invalide"
	elif [ "$pct" -lt 20 ]; then
		warn "contenu non nul faible (${pct} %) — inhabituel, à confronter au second dump"
	else
		ok "le dump contient des données réelles"
	fi
}

# __FMAP__ est la signature de la table de partitionnement du firmware.
# Sa présence prouve qu'on a lu un vrai firmware ChromeOS/coreboot.
check_fmap() {
	local f="$1" off
	off="$(LC_ALL=C grep -abo --binary-files=text -m1 -- '__FMAP__' "$f" 2>/dev/null | head -1 | cut -d: -f1)"
	if [ -n "$off" ]; then
		ok "signature __FMAP__ trouvée à l'offset $off"
	else
		fail "signature __FMAP__ absente : ce fichier n'est pas un firmware ChromeOS valide"
	fi
}

# Les noms de régions confirment qu'il s'agit bien du firmware d'origine
# complet, et pas d'un fragment.
check_regions() {
	local f="$1" found=0 missing="" r
	local regions="GBB RO_SECTION RW_SECTION_A RW_SECTION_B SI_DESC WP_RO COREBOOT RO_VPD RW_VPD"
	local present=""
	for r in $regions; do
		if LC_ALL=C grep -aq --binary-files=text -- "$r" "$f" 2>/dev/null; then
			present="$present $r"
			found=$(( found + 1 ))
		else
			missing="$missing $r"
		fi
	done
	info "régions détectées ($found/9) :$present"
	[ -n "$missing" ] && info "absentes :$missing"

	if [ "$found" -ge 7 ]; then
		ok "structure du firmware conforme"
	elif [ "$found" -ge 4 ]; then
		warn "seulement $found régions sur 9 — dump peut-être partiel"
	else
		fail "structure non reconnue ($found régions) : ce n'est pas un firmware complet"
	fi
}

# ------------------------------------------------------------------ exécution

echo "Claude OS — vérification de sauvegarde firmware v${VERSION}"

hdr "Dump 1 : $D1"
if check_readable "$D1"; then
	check_size      "$D1"
	check_not_blank "$D1"
	check_fmap      "$D1"
	check_regions   "$D1"
	SUM1="$(checksum "$D1")"
	info "sha256 : $SUM1"
else
	exit 2
fi

if [ -n "$D2" ]; then
	hdr "Dump 2 : $D2"
	if check_readable "$D2"; then
		check_size      "$D2"
		check_not_blank "$D2"
		check_fmap      "$D2"
		check_regions   "$D2"
		SUM2="$(checksum "$D2")"
		info "sha256 : $SUM2"
	fi

	hdr "Comparaison des deux lectures"
	if cmp -s "$D1" "$D2"; then
		ok "les deux lectures sont identiques — sauvegarde fiable"
	else
		# Toute différence n'est pas une corruption : le firmware écrit en
		# fonctionnement dans certaines régions (journal d'événements RW_ELOG,
		# VPD, NVRAM). Deux lectures espacées peuvent donc différer légèrement
		# sans que le dump soit mauvais. On distingue par le volume.
		ndiff=""; capped=""
		ndiff="$(cmp -l "$D1" "$D2" 2>/dev/null | head -200001 | wc -l | tr -d ' ')"
		[ "$ndiff" -gt 200000 ] && capped=" (au moins)"
		info "octets différents :$capped $ndiff"
		info "première différence : $(cmp "$D1" "$D2" 2>&1 | head -1)"

		if [ "$ndiff" -le 16384 ]; then
			warn "différences localisées ($ndiff octets)."
			info "Compatible avec une région écrite en fonctionnement"
			info "(journal d'événements, VPD, NVRAM) plutôt qu'avec une lecture"
			info "corrompue. La sauvegarde est probablement exploitable, mais"
			info "une troisième lecture permettrait de trancher."
		else
			fail "les deux lectures divergent massivement ($ndiff octets)."
			info "Ce volume n'est pas explicable par les régions d'exécution :"
			info "au moins une lecture est corrompue. Relire la puce."
		fi
	fi
else
	hdr "Comparaison"
	warn "un seul dump fourni : la lecture n'est pas confirmée."
	info "Relire la puce une seconde fois et relancer avec les deux fichiers."
fi

# ------------------------------------------------------------------- manifeste

hdr "Manifeste"
MANIFEST="firmware-backup-manifest.txt"
{
	echo "# Claude OS — manifeste de sauvegarde firmware"
	echo "# NE PAS committer le fichier .rom lui-même : il contient le VPD,"
	echo "# donc le numéro de série de la machine et son adresse MAC."
	echo "date        : $(date -Is 2>/dev/null || date)"
	echo "machine     : $(uname -n 2>/dev/null)"
	echo "dump1       : $(basename "$D1")"
	echo "taille1     : $(filesize "$D1")"
	echo "sha256_1    : ${SUM1:-?}"
	if [ -n "$D2" ]; then
		echo "dump2       : $(basename "$D2")"
		echo "taille2     : $(filesize "$D2")"
		echo "sha256_2    : ${SUM2:-?}"
		echo "identiques  : $(cmp -s "$D1" "$D2" && echo oui || echo NON)"
	fi
	echo "verdict     : $( [ "$RC" -eq 0 ] && echo EXPLOITABLE || { [ "$RC" -eq 1 ] && echo 'AVEC RESERVES' || echo INUTILISABLE; } )"
} > "$MANIFEST"
info "écrit : $MANIFEST (à committer — contrairement au .rom)"

# --------------------------------------------------------------------- verdict

hdr "Verdict"
case "$RC" in
	0) grn "  Sauvegarde exploitable. Copier le .rom sur DEUX supports distincts avant de flasher." ;;
	1) ylw "  Réserves. Lire les avertissements ci-dessus avant de poursuivre." ;;
	2) red "  NE PAS FLASHER. Sans programmateur SPI externe, cette sauvegarde ne permettrait pas de revenir en arrière." ;;
esac
echo
exit "$RC"
