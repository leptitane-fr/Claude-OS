#!/usr/bin/env bash
#
# Claude-OS -- construction du systeme sur la cle USB.
#
# Deux modes :
#
#   sudo ./build/build.sh --image claude-os.img
#       Produit un fichier image, a ecrire ensuite sur la cle depuis
#       n'importe quel systeme (balenaEtcher, Rufus, dd). C'est le mode
#       utilise par l'integration continue, et le seul necessaire si l'on
#       ne dispose pas d'un hote Linux.
#
#   sudo ./build/build.sh --device /dev/sdb
#       Ecrit directement sur une cle branchee, depuis un hote Linux.
#
# Le script repart toujours d'un partitionnement neuf : relancer reconstruit
# le systeme a l'identique. C'est voulu -- on itere sur la recette, pas sur
# le resultat.

set -Eeuo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MNT="/mnt/claude-os-build"
LOGFILE="${REPO_DIR}/build.log"
MODE=""            # image | device
IMAGE_PATH=""
LOOP_DEV=""

# ---------------------------------------------------------------------------
# Journalisation et garde-fous
# ---------------------------------------------------------------------------
c_red=$'\033[1;31m'; c_grn=$'\033[1;32m'; c_yel=$'\033[1;33m'
c_blu=$'\033[1;34m'; c_off=$'\033[0m'

step() { printf '\n%s==>%s %s\n' "$c_blu" "$c_off" "$*" | tee -a "$LOGFILE"; }
info() { printf '    %s\n' "$*" | tee -a "$LOGFILE"; }
warn() { printf '%s /!\\ %s%s\n' "$c_yel" "$*" "$c_off" | tee -a "$LOGFILE"; }
ok()   { printf '%s  ok%s %s\n' "$c_grn" "$c_off" "$*" | tee -a "$LOGFILE"; }
die()  { printf '\n%sECHEC :%s %s\n' "$c_red" "$c_off" "$*" | tee -a "$LOGFILE" >&2; exit 1; }

cleanup() {
    set +e
    if mountpoint -q "$MNT" 2>/dev/null; then
        step "Demontage"
        for m in dev/pts dev proc sys run ''; do
            umount -l "$MNT/$m" 2>/dev/null
        done
        sync
    fi
    if [ -n "$LOOP_DEV" ] && losetup "$LOOP_DEV" >/dev/null 2>&1; then
        losetup -d "$LOOP_DEV" 2>/dev/null && ok "peripherique loop detache"
    fi
    [ -n "$MODE" ] && ok "support libere"
}
trap cleanup EXIT
trap 'die "interrompu ligne $LINENO"' ERR

# ---------------------------------------------------------------------------
# Chargement de la configuration
# ---------------------------------------------------------------------------
: >"$LOGFILE"
[ "$(id -u)" -eq 0 ] || die "a lancer en root : sudo $0"

usage() {
    cat >&2 <<USAGEEOF
Usage :
    sudo $0 --image <fichier.img>   construit une image a flasher
    sudo $0 --device /dev/sdX       ecrit directement sur une cle USB
USAGEEOF
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --image)  MODE="image";  IMAGE_PATH="${2:-}"; shift 2 || usage ;;
        --device) MODE="device"; TARGET_DEVICE_ARG="${2:-}"; shift 2 || usage ;;
        -h|--help) usage ;;
        *) echo "argument inconnu : $1" >&2; usage ;;
    esac
done
[ -n "$MODE" ] || usage

# shellcheck source=config/default.conf
. "${REPO_DIR}/build/config/default.conf"
if [ -r "${REPO_DIR}/build/config/local.conf" ]; then
    . "${REPO_DIR}/build/config/local.conf"
    info "surcharge locale chargee depuis build/config/local.conf"
fi

step "Verification des outils requis"
missing=()
for t in debootstrap sgdisk mkfs.ext4 mkfs.vfat mcopy blkid partprobe chroot losetup; do
    command -v "$t" >/dev/null 2>&1 || missing+=("$t")
done
[ ${#missing[@]} -eq 0 ] || die "outils manquants : ${missing[*]}
    Installer avec :  apt install debootstrap gdisk dosfstools e2fsprogs mtools"
ok "tous les outils sont presents"

# ---------------------------------------------------------------------------
# VALIDATION DE LA CIBLE
#
# La section la plus importante du script. Une erreur ici detruit le disque
# Windows -- exactement ce que le projet s'interdit. Chaque garde-fou ci-dessous
# est une raison independante de refuser d'ecrire.
# ---------------------------------------------------------------------------
step "Preparation du support cible"

if [ "$MODE" = "image" ]; then
    # --- Mode image ---------------------------------------------------------
    # Rien de destructif ici : on fabrique un fichier creux et on l'expose via
    # un peripherique loop. Le reste du script ne fait aucune difference entre
    # un loop et une vraie cle.
    [ -n "$IMAGE_PATH" ] || die "--image exige un chemin de fichier"
    [ -e "$IMAGE_PATH" ] && die "$IMAGE_PATH existe deja -- le supprimer ou choisir un autre nom"

    MIN_MB=$(( SIZE_ESP + 4096 ))
    [ "$IMAGE_SIZE_MB" -ge "$MIN_MB" ] \
        || die "IMAGE_SIZE_MB=$IMAGE_SIZE_MB trop petit ; ${MIN_MB} MiB minimum"

    info "creation d'une image creuse de ${IMAGE_SIZE_MB} MiB"
    truncate -s "${IMAGE_SIZE_MB}M" "$IMAGE_PATH"
    LOOP_DEV="$(losetup -f --show -P "$IMAGE_PATH")" \
        || die "losetup a echoue -- le conteneur autorise-t-il les peripheriques loop ?"
    TARGET_DEVICE="$LOOP_DEV"
    DEV_BYTES=$(( IMAGE_SIZE_MB * 1048576 ))

    # La cle reelle est inconnue a la construction : impossible de renseigner
    # l'exclusion d'autosuspend USB. On la desactive, et claude-os-firstboot
    # la reactivera une fois la cle identifiee.
    USB_DENYLIST=""
    USB_AUTOSUSPEND=0
    ok "image exposee via $LOOP_DEV"

else
    # --- Mode peripherique --------------------------------------------------
    # Section la plus sensible du script. Une erreur ici detruit le disque
    # Windows -- exactement ce que le projet s'interdit. Chaque garde-fou
    # ci-dessous est une raison independante de refuser d'ecrire.
    TARGET_DEVICE="${TARGET_DEVICE_ARG:-$TARGET_DEVICE}"
    if [ -z "$TARGET_DEVICE" ]; then
        echo; lsblk -o NAME,SIZE,TYPE,TRAN,RM,MODEL,MOUNTPOINTS; echo
        read -rp "Peripherique cible (ex. /dev/sdb) : " TARGET_DEVICE
    fi

    [ -b "$TARGET_DEVICE" ] || die "$TARGET_DEVICE n'est pas un peripherique bloc"
    case "$TARGET_DEVICE" in
        *[0-9]) die "$TARGET_DEVICE designe une partition. Indiquer le disque entier (ex. /dev/sdb)" ;;
        /dev/nvme*|/dev/mmcblk*) die "$TARGET_DEVICE est un disque interne. Refus categorique." ;;
    esac

    DEV_NAME="$(basename "$TARGET_DEVICE")"

    # Garde-fou 1 : le peripherique doit etre annonce comme USB par udev.
    eval "$(udevadm info --query=property --name="$TARGET_DEVICE" \
            | grep -E '^(ID_BUS|ID_VENDOR_ID|ID_MODEL_ID|ID_MODEL)=' | sed 's/^/UD_/')"
    [ "${UD_ID_BUS:-}" = "usb" ] \
        || die "$TARGET_DEVICE n'est pas sur le bus USB (ID_BUS=${UD_ID_BUS:-inconnu}). Refus."

    # Garde-fou 2 : le noyau doit le declarer amovible.
    [ "$(cat "/sys/block/$DEV_NAME/removable" 2>/dev/null)" = "1" ] \
        || warn "le noyau ne declare pas $TARGET_DEVICE amovible (certains SSD USB non plus)"

    # Garde-fou 3 : il ne doit porter aucun systeme de fichiers monte.
    if lsblk -nro MOUNTPOINT "$TARGET_DEVICE" | grep -q .; then
        lsblk -o NAME,SIZE,MOUNTPOINTS "$TARGET_DEVICE"
        die "$TARGET_DEVICE porte des partitions montees. Les demonter d'abord."
    fi

    # Garde-fou 4 : il ne doit surtout pas heberger le systeme en cours.
    ROOT_SRC="$(findmnt -no SOURCE / 2>/dev/null || true)"
    ROOT_DISK="$(lsblk -nro PKNAME "$ROOT_SRC" 2>/dev/null | head -1 || true)"
    [ -n "$ROOT_DISK" ] && [ "$ROOT_DISK" = "$DEV_NAME" ] \
        && die "$TARGET_DEVICE heberge le systeme en cours d'execution. Refus."

    # Garde-fou 5 : taille plausible pour une cle.
    DEV_BYTES="$(blockdev --getsize64 "$TARGET_DEVICE")"
    DEV_GB=$(( DEV_BYTES / 1000000000 ))
    [ "$DEV_GB" -ge 32 ]   || die "$TARGET_DEVICE ne fait que ${DEV_GB} Go ; il en faut au moins 32."
    [ "$DEV_GB" -le 2000 ] || die "$TARGET_DEVICE fait ${DEV_GB} Go -- trop gros pour une cle. Verifier la cible."

    echo
    printf '%s' "$c_yel"
    cat <<BANNER
  +-------------------------------------------------------------+
  |  TOUTES LES DONNEES DE CE PERIPHERIQUE SERONT DETRUITES      |
  +-------------------------------------------------------------+
BANNER
    printf '%s' "$c_off"
    printf '    Peripherique : %s\n' "$TARGET_DEVICE"
    printf '    Modele       : %s\n' "${UD_ID_MODEL:-inconnu}"
    printf '    Identifiant  : %s:%s\n' "${UD_ID_VENDOR_ID:-????}" "${UD_ID_MODEL_ID:-????}"
    printf '    Taille       : %s Go\n' "$DEV_GB"
    echo
    read -rp "    Retaper le chemin exact du peripherique pour confirmer : " confirm
    [ "$confirm" = "$TARGET_DEVICE" ] || die "confirmation non conforme -- rien n'a ete ecrit"

    # Ici la cle est connue : on peut exclure precisement son controleur de
    # l'autosuspend USB, qui gelerait la racine du systeme.
    USB_DENYLIST="${UD_ID_VENDOR_ID:-}:${UD_ID_MODEL_ID:-}"
    [ "$USB_DENYLIST" = ":" ] && USB_DENYLIST=""
    USB_AUTOSUSPEND=1
    ok "cible validee ; exclusion TLP d'autosuspend : ${USB_DENYLIST:-<aucune>}"
fi

# ---------------------------------------------------------------------------
# Mot de passe du compte
# ---------------------------------------------------------------------------
step "Compte utilisateur"
FORCE_PW_CHANGE=0
if [ -n "${CLAUDE_OS_PASSWORD:-}" ]; then
    PW1="$CLAUDE_OS_PASSWORD"
    FORCE_PW_CHANGE=1
    info "mot de passe pris dans CLAUDE_OS_PASSWORD ; changement impose a la 1re connexion"
elif [ -t 0 ]; then
    while :; do
        read -rsp "    Mot de passe pour '$USERNAME' : " PW1; echo
        read -rsp "    Confirmer                     : " PW2; echo
        [ -n "$PW1" ] && [ "$PW1" = "$PW2" ] && break
        warn "mots de passe vides ou differents"
    done
else
    # Construction non interactive : l'image est publique, le mot de passe
    # initial n'a donc aucune valeur de secret. Il doit imperativement etre
    # change a la premiere connexion, ce que chage impose plus bas.
    PW1="claude"
    FORCE_PW_CHANGE=1
    warn "construction non interactive : mot de passe initial 'claude'"
    warn "il DOIT etre change a la premiere connexion (impose par chage)"
fi

# ---------------------------------------------------------------------------
# Partitionnement
# ---------------------------------------------------------------------------
step "Partitionnement"
wipefs -a "$TARGET_DEVICE" >/dev/null 2>&1 || true
sgdisk --zap-all "$TARGET_DEVICE" >/dev/null 2>&1 || true

# Deux partitions seulement : ESP puis racine. Un /home distinct serait coince
# entre la racine et la fin du support, donc impossible a agrandir au premier
# demarrage -- or c'est precisement ce qu'il faut pouvoir faire quand une
# image de 11 Gio arrive sur une cle de 256 Go.
if [ "$MODE" = "image" ]; then
    # La racine occupe tout le reste de l'image ; l'extension a la taille
    # reelle du support a lieu au premier demarrage.
    ROOT_END=0
else
    SECTOR_SZ=$(blockdev --getss "$TARGET_DEVICE")
    TOTAL_SECT=$(( DEV_BYTES / SECTOR_SZ ))
    ROOT_END=$(( TOTAL_SECT * FILL_PERCENT / 100 ))
fi

sgdisk \
    -n 1:0:+${SIZE_ESP}M   -t 1:ef00 -c 1:"CLAUDEOS-ESP"  \
    -n 2:0:${ROOT_END}     -t 2:8304 -c 2:"claudeos-root" \
    "$TARGET_DEVICE" >/dev/null

partprobe "$TARGET_DEVICE" >/dev/null 2>&1 || true
partx -u "$TARGET_DEVICE" >/dev/null 2>&1 || true
command -v udevadm >/dev/null 2>&1 && udevadm settle
sleep 2

# Les noms de partition varient : sdb1 pour une cle, loop0p1 pour un loop,
# nvme0n1p1 ailleurs. On les resout plutot que de les deviner.
mapfile -t PARTS < <(lsblk -nro NAME "$TARGET_DEVICE" | tail -n +2)
[ "${#PARTS[@]}" -ge 2 ] || die "partitionnement incomplet : ${#PARTS[@]} partition(s) detectee(s)"
P_ESP="/dev/${PARTS[0]}"; P_ROOT="/dev/${PARTS[1]}"
ok "ESP=$P_ESP  racine=$P_ROOT"

if [ "$MODE" = "device" ]; then
    info "espace laisse hors partition (reserve d'usure) : $(( (DEV_BYTES/1048576) * (100-FILL_PERCENT) / 100 / 1024 )) Gio"
fi

step "Formatage"

# La partition EFI est fabriquee comme un FICHIER image, peuple avec mtools,
# puis recopie tel quel dans la partition. Elle n'est jamais montee.
#
# Ce detour evite d'exiger la prise en charge du FAT par le noyau de l'hote :
# beaucoup de conteneurs de construction en sont depourvus, et rien ne
# justifie que le build en depende.
ESP_WORK="$(mktemp -d)"
ESP_IMG="$ESP_WORK/esp.img"
mkfs.vfat -F32 -n CLAUDE-EFI -C "$ESP_IMG" $(( SIZE_ESP * 1024 )) >/dev/null

# Desactiver le journal ext4 menagerait le flash, mais un arrachage de cle
# sans journal corrompt le systeme de fichiers. On garde le journal et on
# espace ses ecritures via commit=600 dans fstab.
mkfs.ext4 -q -F -L claudeos-root -m 1 "$P_ROOT"
ok "systemes de fichiers crees"

UUID_ESP="$(blkid -s UUID -o value "$ESP_IMG")"
UUID_ROOT="$(blkid -s UUID -o value "$P_ROOT")"
[ -n "$UUID_ROOT" ] && [ -n "$UUID_ESP" ] || die "UUID illisibles apres formatage"
info "UUID racine $UUID_ROOT / ESP $UUID_ESP"

step "Montage sur $MNT"
mkdir -p "$MNT"
mount -o noatime "$P_ROOT" "$MNT"
# Simple repertoire pendant la construction ; il deviendra le point de
# montage de l'ESP une fois le systeme demarre.
mkdir -p "$MNT/boot/efi"
ok "monte"

# ---------------------------------------------------------------------------
# Systeme de base
# ---------------------------------------------------------------------------
step "debootstrap ($SUITE) -- comptez 5 a 15 minutes"
debootstrap \
    --arch=amd64 \
    --variant=minbase \
    --components="$(echo "$COMPONENTS" | tr ' ' ',')" \
    --include=apt-transport-https,ca-certificates \
    "$SUITE" "$MNT" "$MIRROR" >>"$LOGFILE" 2>&1 \
    || die "debootstrap a echoue -- voir $LOGFILE"
ok "systeme de base installe"

step "Preparation du chroot"
mount --bind /dev     "$MNT/dev"
mount --bind /dev/pts "$MNT/dev/pts"
mount -t proc  proc   "$MNT/proc"
mount -t sysfs sysfs  "$MNT/sys"
mount -t tmpfs tmpfs  "$MNT/run"
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

# Autorite de certification supplementaire, si l'on construit derriere un
# proxy TLS interceptant : sans elle, la recuperation de la cle de signature
# de Claude Desktop echoue a la verification du certificat.
if [ -n "${EXTRA_CA_CERT:-}" ] && [ -r "$EXTRA_CA_CERT" ]; then
    mkdir -p "$MNT/usr/local/share/ca-certificates"
    cp "$EXTRA_CA_CERT" "$MNT/usr/local/share/ca-certificates/build-proxy.crt"
    info "autorite de certification supplementaire installee dans le chroot"
fi

# Les variables TLS de l'environnement hote designent des chemins qui
# n'existent pas dans le chroot : heritees telles quelles, elles font echouer
# curl avec "error setting certificate file" avant meme toute connexion. On
# les neutralise pour que le chroot utilise son propre magasin de certificats.
# Execute une commande DANS le systeme cible.
#
# L'environnement de l'hote n'a pas cours dans le chroot : ses chemins n'y
# existent pas. Les variables TLS sont neutralisees (elles designent des
# fichiers de certificats propres a l'hote), et HOME/USER/PATH sont poses
# explicitement plutot qu'herites.
#
# HOME merite une mention : lance par `sudo -E`, le script herite du HOME de
# l'appelant -- /home/runner sur un runner GitHub. Ce repertoire n'existe pas
# dans le chroot, et gpg, qui veut y creer son trousseau, echoue alors avec
# "Fatal: can't create directory". Tout outil touchant a $HOME est concerne.
in_chroot() {
    chroot "$MNT" /usr/bin/env \
        -u CURL_CA_BUNDLE -u SSL_CERT_FILE -u SSL_CERT_DIR \
        -u REQUESTS_CA_BUNDLE -u NODE_EXTRA_CA_CERTS -u AWS_CA_BUNDLE \
        -u NIX_SSL_CERT_FILE -u HTTPLIB2_CA_CERTS -u CLOUDSDK_CORE_CUSTOM_CA_CERTS_FILE \
        HOME=/root USER=root LOGNAME=root \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        DEBIAN_FRONTEND=noninteractive LC_ALL=C "$@"
}

cat > "$MNT/etc/apt/sources.list" <<APTEOF
deb $MIRROR $SUITE $COMPONENTS
deb $MIRROR ${SUITE}-updates $COMPONENTS
deb http://security.debian.org/debian-security ${SUITE}-security $COMPONENTS
deb $MIRROR ${SUITE}-backports $COMPONENTS
APTEOF

# Les backports sont marques NotAutomatic par Debian : rien n'en provient
# sans un -t explicite. Les ajouter aux sources ne modifie donc pas le
# systeme, cela rend seulement le depot disponible.

# Empeche les demons de demarrer pendant l'installation dans le chroot.
cat > "$MNT/usr/sbin/policy-rc.d" <<'PRCEOF'
#!/bin/sh
exit 101
PRCEOF
chmod +x "$MNT/usr/sbin/policy-rc.d"

in_chroot apt-get update -qq
ok "chroot pret"

# ---------------------------------------------------------------------------
# Paquets
# ---------------------------------------------------------------------------
step "Installation des paquets"
mapfile -t PKGS < <(grep -vE '^\s*#|^\s*$' "${REPO_DIR}/build/packages.list" | sed 's/\s*#.*//' | tr -d ' ')
info "${#PKGS[@]} paquets a installer -- suivre la progression : tail -f $LOGFILE"

# Le trousseau et l'applet reseau ont besoin de leurs recommandes pour
# fonctionner ; le reste est installe sans, conformement a apt.conf.d.
# Verifier d'abord que tous les noms existent. apt --simulate ne telecharge
# rien et repond en une seconde ; sans ce controle, un nom errone se
# manifeste apres le debootstrap, et la reprise ci-dessous le reessaie trois
# fois pour rien puisque l'erreur est deterministe.
if ! in_chroot apt-get install -s --no-install-recommends "${PKGS[@]}" >>"$LOGFILE" 2>&1; then
    warn "des paquets de build/packages.list sont introuvables :"
    grep -oE 'Unable to locate package [^ ]+|Package .* has no installation candidate' "$LOGFILE" \
        | sort -u | sed 's/^/      /'
    die "corriger build/packages.list avant de relancer"
fi
ok "les ${#PKGS[@]} noms de paquets existent"

# Les miroirs Debian renvoient occasionnellement une erreur passagere, et
# perdre une construction entiere pour cela n'a pas de sens. On reessaie,
# en journalisant chaque tentative pour qu'un echec reel reste lisible.
apt_ok=0
for attempt in 1 2 3; do
    if in_chroot apt-get install -y --no-install-recommends "${PKGS[@]}" >>"$LOGFILE" 2>&1; then
        apt_ok=1; break
    fi
    warn "tentative $attempt echouee"
    tail -5 "$LOGFILE" | sed 's/^/      /'
    # Un paquet introuvable ou un conflit de dependances ne se resoudra pas
    # en reessayant : seuls les incidents reseau le meritent.
    if tail -40 "$LOGFILE" | grep -qE 'Unable to locate package|has no installation candidate|Unmet dependencies'; then
        die "erreur deterministe, inutile de reessayer -- voir $LOGFILE"
    fi
    [ "$attempt" -lt 3 ] || break
    in_chroot apt-get update -qq >>"$LOGFILE" 2>&1 || true
    sleep $(( attempt * 10 ))
done
[ "$apt_ok" -eq 1 ] || die "installation des paquets echouee apres 3 tentatives -- voir $LOGFILE"

# ---------------------------------------------------------------------------
# Noyau
#
# Installe a part pour pouvoir choisir sa provenance sans embarquer les deux.
# Le materiel recent en depend directement : la carte Wi-Fi MediaTek MT7902
# du Vivobook n'est reconnue qu'a partir de Linux 7.1, qui l'a integree au
# pilote mt7921e. Sur le noyau 6.12 de stable, elle reste non reclamee.
# ---------------------------------------------------------------------------
if [ "$ENABLE_BACKPORTS_KERNEL" = "yes" ]; then
    step "Noyau depuis ${SUITE}-backports"
    # Les firmwares suivent le noyau : celui du MT7902 n'existe que dans les
    # versions recentes de linux-firmware.
    if in_chroot apt-get install -y --no-install-recommends \
            -t "${SUITE}-backports" \
            linux-image-amd64 \
            firmware-mediatek firmware-iwlwifi firmware-realtek firmware-atheros \
            firmware-intel-graphics firmware-intel-misc firmware-intel-sound \
            firmware-sof-signed >>"$LOGFILE" 2>&1; then
        KVER="$(in_chroot dpkg-query -Wf '${Depends}' linux-image-amd64 2>/dev/null \
                | grep -oE 'linux-image-[0-9][^ ,]*' | head -1)"
        ok "noyau installe : ${KVER:-inconnu}"
    else
        warn "noyau de backports indisponible -- repli sur celui de stable"
        in_chroot apt-get install -y --no-install-recommends linux-image-amd64 >>"$LOGFILE" 2>&1 \
            || die "aucun noyau installable -- voir $LOGFILE"
    fi
else
    step "Noyau depuis $SUITE (stable)"
    in_chroot apt-get install -y --no-install-recommends linux-image-amd64 >>"$LOGFILE" 2>&1 \
        || die "installation du noyau echouee -- voir $LOGFILE"
    warn "le Wi-Fi MediaTek MT7902 ne fonctionnera pas sur ce noyau"
fi

# Le noyau doit etre signe, sinon Secure Boot refusera de le charger.
in_chroot sh -c 'ls /boot/vmlinuz-* >/dev/null 2>&1' \
    || die "aucun noyau dans /boot apres installation"
in_chroot dpkg -l linux-image-amd64 shim-signed grub-efi-amd64-signed >/dev/null \
    || die "paquets d'amorcage absents -- Secure Boot serait impossible"
[ -n "${EXTRA_CA_CERT:-}" ] && in_chroot update-ca-certificates >/dev/null 2>&1
ok "paquets installes"

if [ "$ENABLE_COWORK" = "yes" ]; then
    step "Dependances Cowork (QEMU/KVM)"
    warn "une VM QEMU sur cle flash : ecritures intensives et forte ponction batterie"
    in_chroot apt-get install -y qemu-system-x86 ovmf virtiofsd >/dev/null
    ok "QEMU installe"
fi

# ---------------------------------------------------------------------------
# Application de la surcouche de configuration
# ---------------------------------------------------------------------------
step "Application de overlay/"
cp -a "${REPO_DIR}/overlay/." "$MNT/"

# Substitution des valeurs decouvertes a l'execution.
DGPU_BDF="$(for d in /sys/bus/pci/devices/*/; do
    [ "$(cat "$d/vendor" 2>/dev/null)" = "0x10de" ] || continue
    case "$(cat "$d/class" 2>/dev/null)" in 0x0300*|0x0302*) basename "$d"; break ;; esac
done)"
[ -n "$DGPU_BDF" ] && info "dGPU detecte sur la machine de build : $DGPU_BDF" \
                   || info "aucun dGPU detecte ici (build sur une autre machine ?)"

# Le noyau n'accepte qu'un seul parametre module_blacklist= : les modules
# doivent former une liste unique separee par des virgules. En passer deux
# ferait silencieusement ignorer l'un des deux.
BLACKLIST_MODS=""
CMDLINE_DGPU=""
if [ "$DGPU_STRATEGY" != "none" ]; then
    BLACKLIST_MODS="nouveau,nvidia,nvidia_drm,nvidia_modeset,nvidiafb"
    # Autorise la gestion d'energie du pont PCIe portant le dGPU, condition
    # necessaire a la descente en D3cold.
    CMDLINE_DGPU="pcie_port_pm=force"
fi
if [ "$BLACKLIST_VMD" = "yes" ]; then
    BLACKLIST_MODS="${BLACKLIST_MODS:+$BLACKLIST_MODS,}vmd"
fi
if [ -n "$BLACKLIST_MODS" ]; then
    CMDLINE_DGPU="module_blacklist=$BLACKLIST_MODS${CMDLINE_DGPU:+ $CMDLINE_DGPU}"
fi
info "modules blacklistes au noyau : ${BLACKLIST_MODS:-<aucun>}"

grep -rlZ '@[A-Z_]*@' "$MNT/etc" "$MNT/usr/local" 2>/dev/null | while IFS= read -r -d '' f; do
    sed -i \
        -e "s|@UUID_ROOT@|$UUID_ROOT|g" \
        -e "s|@UUID_ESP@|$UUID_ESP|g" \
        -e "s|@USB_DENYLIST@|$USB_DENYLIST|g" \
        -e "s|@USB_AUTOSUSPEND@|$USB_AUTOSUSPEND|g" \
        -e "s|@DGPU_BDF@|$DGPU_BDF|g" \
        -e "s|@CMDLINE_DGPU@|$CMDLINE_DGPU|g" \
        "$f"
done

echo "DGPU_STRATEGY=\"$DGPU_STRATEGY\"" > "$MNT/etc/default/claude-os-dgpu"
echo "FILL_PERCENT=$FILL_PERCENT" > "$MNT/etc/default/claude-os-firstboot"

if [ "$BLACKLIST_VMD" = "yes" ]; then
    cat > "$MNT/etc/modprobe.d/claude-os-vmd.conf" <<'VMDEOF'
# Rend le SSD interne (Windows) invisible au noyau : sans le module vmd,
# Linux ne voit tout simplement pas le controleur Intel RST qui le porte.
# Une garantie materielle de non-ecriture, plus solide qu'une discipline
# de montage.
blacklist vmd
VMDEOF
    warn "module vmd blackliste : le SSD interne sera invisible sous Claude-OS"
fi
ok "surcouche appliquee"

# ---------------------------------------------------------------------------
# Configuration systeme
# ---------------------------------------------------------------------------
step "Configuration du systeme"
echo "$HOSTNAME" > "$MNT/etc/hostname"
cat > "$MNT/etc/hosts" <<HOSTEOF
127.0.0.1   localhost
127.0.1.1   $HOSTNAME
::1         localhost ip6-localhost ip6-loopback
HOSTEOF

in_chroot ln -sf "/usr/share/zoneinfo/$TIMEZONE" /etc/localtime
echo "$LOCALE UTF-8" > "$MNT/etc/locale.gen"
in_chroot locale-gen >/dev/null
echo "LANG=$LOCALE" > "$MNT/etc/default/locale"
sed -i "s/^XKBLAYOUT=.*/XKBLAYOUT=\"$KEYMAP\"/" "$MNT/etc/default/keyboard" 2>/dev/null || \
    printf 'XKBLAYOUT="%s"\nXKBMODEL="pc105"\n' "$KEYMAP" > "$MNT/etc/default/keyboard"

# Pas de partition de swap : sans cela, l'initramfs attend un peripherique de
# reprise inexistant et rallonge le demarrage de 30 secondes.
echo "RESUME=none" > "$MNT/etc/initramfs-tools/conf.d/resume"

# MODULES=most est le defaut Debian et doit le rester : la cle doit pouvoir
# demarrer sur une machine dont on ne connait pas le controleur USB.
grep -q '^MODULES=most' "$MNT/etc/initramfs-tools/initramfs.conf" \
    || warn "MODULES n'est pas a 'most' -- risque de non-amorcage sur une autre machine"

# systemd-journal : sans ce groupe, journalctl -u <service> repond
# « No journal files were opened due to insufficient permissions », ce qui
# rend le diagnostic des services de Claude-OS impossible sans sudo.
in_chroot useradd -m -s /bin/bash -G sudo,audio,video,netdev,plugdev,systemd-journal \
    -c "$USER_FULLNAME" "$USERNAME"
echo "$USERNAME:$PW1" | in_chroot chpasswd
in_chroot passwd -l root >/dev/null   # pas de connexion root directe ; sudo suffit
if [ "$FORCE_PW_CHANGE" = "1" ]; then
    in_chroot chage -d 0 "$USERNAME"   # expire le mot de passe : changement force
fi
unset PW1 PW2
ok "compte '$USERNAME' cree"

step "Activation des services"
in_chroot systemctl enable lightdm NetworkManager tlp thermald \
    claude-os-dgpu-power.service claude-os-firstboot.service >/dev/null 2>&1 || true
# TLP et power-profiles-daemon se marchent dessus ; TLP l'emporte.
in_chroot systemctl mask power-profiles-daemon.service >/dev/null 2>&1 || true
in_chroot systemctl mask systemd-rfkill.service systemd-rfkill.socket >/dev/null 2>&1 || true
ok "services actives"

# ---------------------------------------------------------------------------
# Claude Desktop -- depot APT officiel d'Anthropic
# ---------------------------------------------------------------------------
step "Claude Desktop"
CLAUDE_KEY_FPR="31DDDE24DDFAB679F42D7BD2BAA929FF1A7ECACE"
CLAUDE_KEYRING="/usr/share/keyrings/claude-desktop-archive-keyring.asc"

# Composant optionnel : son echec ne doit pas interrompre une construction
# par ailleurs saine. Le systeme reste parfaitement utilisable, et
# l'application peut etre installee apres le premier demarrage.
#
# Aucune erreur n'est envoyee vers /dev/null : c'est precisement ce qui a
# rendu un echec precedent indiagnosticable. Tout part dans le journal.
install_claude_desktop() {
    if ! in_chroot curl -fsSLo "$CLAUDE_KEYRING" \
            https://downloads.claude.ai/claude-desktop/key.asc >>"$LOGFILE" 2>&1; then
        warn "cle de signature inaccessible (reseau ?)"
        return 1
    fi

    local fpr
    fpr="$(in_chroot gpg --show-keys --with-colons "$CLAUDE_KEYRING" 2>>"$LOGFILE" \
           | awk -F: '$1=="fpr" && !seen {print $10; seen=1}')" || true

    if [ "$fpr" != "$CLAUDE_KEY_FPR" ]; then
        rm -f "$MNT$CLAUDE_KEYRING"
        warn "empreinte de cle inattendue : ${fpr:-<illisible, voir le journal>}"
        warn "attendue : $CLAUDE_KEY_FPR"
        return 1
    fi
    ok "empreinte de la cle Anthropic verifiee"

    echo "deb [arch=amd64 signed-by=$CLAUDE_KEYRING] https://downloads.claude.ai/claude-desktop/apt/stable stable main" \
        > "$MNT/etc/apt/sources.list.d/claude-desktop.list"

    in_chroot apt-get update -qq >>"$LOGFILE" 2>&1 \
        || { warn "depot Claude Desktop injoignable"; return 1; }
    in_chroot apt-get install -y --no-install-recommends claude-desktop >>"$LOGFILE" 2>&1 \
        || { warn "installation de claude-desktop echouee"; return 1; }

    ok "Claude Desktop installe depuis le depot officiel"
    return 0
}

if install_claude_desktop; then
    CLAUDE_DESKTOP_OK=1
else
    CLAUDE_DESKTOP_OK=0
    warn "Claude Desktop absent de cette image ; l'installer apres le premier"
    warn "demarrage :  sudo apt update && sudo apt install claude-desktop"
fi

# ---------------------------------------------------------------------------
# Amorcage : Secure Boot, chemin amovible, aucune ecriture NVRAM
# ---------------------------------------------------------------------------
step "Installation de GRUB (Secure Boot, cible amovible)"

# Sans efivarfs, grub-install ne peut pas toucher a la NVRAM meme s'il le
# voulait. Ceinture et bretelles, en complement de --no-nvram.
in_chroot debconf-set-selections <<'DEBEOF'
grub-efi-amd64 grub2/force_efi_extra_removable boolean true
grub-efi-amd64 grub-efi/install_devices_empty boolean true
DEBEOF

# --force est indispensable ici, et sans danger.
#
# grub-install verifie que le repertoire EFI reside sur un systeme de
# fichiers FAT, et s'arrete net sinon. Pendant la construction, /boot/efi
# n'est qu'un repertoire sur la racine ext4 : ses fichiers sont transferes
# juste apres dans une vraie image FAT, elle-meme ecrite dans la partition
# EFI. --force ramene ce controle a un simple avertissement.
#
# L'avertissement decrit donc exactement la situation qu'on a organisee.
# La partition EFI finale est bien du FAT ; la verification de la chaine
# signee, plus bas, controle le resultat reel plutot que l'intention.
in_chroot grub-install \
    --target=x86_64-efi \
    --efi-directory=/boot/efi \
    --boot-directory=/boot \
    --removable \
    --no-nvram \
    --uefi-secure-boot \
    --force \
    --recheck >>"$LOGFILE" 2>&1 || die "grub-install a echoue -- voir $LOGFILE"

# grub-mkconfig n'ecrit root=UUID= que si /dev/disk/by-uuid/<uuid> existe
# reellement ; sinon il retombe SILENCIEUSEMENT sur le nom de peripherique
# de construction (/dev/loop0p2, /dev/sdb2...). L'image demarrerait alors
# uniquement sur une machine ou la cle porte ce meme nom -- autant dire
# nulle part. Ces liens sont poses par udev, absent de bien des conteneurs
# de construction : on cree donc le lien attendu avant de generer la config.
BYUUID="$MNT/dev/disk/by-uuid"
mkdir -p "$BYUUID"
ln -sf "$P_ROOT" "$BYUUID/$UUID_ROOT"

in_chroot update-grub >>"$LOGFILE" 2>&1 || die "update-grub a echoue -- voir $LOGFILE"

rm -f "$BYUUID/$UUID_ROOT"

# Verification imperative : une racine designee par nom de peripherique
# produit une image qui ne demarre pas, et le defaut est invisible jusqu'au
# premier essai. On echoue ici plutot que de livrer cette image.
if grep -q "root=UUID=$UUID_ROOT" "$MNT/boot/grub/grub.cfg"; then
    ok "la racine est designee par UUID dans grub.cfg"
else
    warn "grub.cfg designe la racine ainsi :"
    grep -oE 'root=[^ ]+' "$MNT/boot/grub/grub.cfg" | sort -u | sed 's/^/      /'
    die "la racine n'est pas designee par UUID : l'image ne demarrerait que sur cette machine"
fi
in_chroot update-initramfs -u -k all >>"$LOGFILE" 2>&1 \
    || die "generation de l'initramfs echouee -- voir $LOGFILE"

# --- Verification de la chaine Secure Boot ---------------------------------
# grub-install ne signale pas toujours une chaine incomplete. On verifie que
# BOOTX64.EFI est bien le shim signe Microsoft, et non un GRUB nu que le
# firmware refuserait de lancer Secure Boot active.
step "Verification de la chaine d'amorcage"
BOOTX64="$MNT/boot/efi/EFI/BOOT/BOOTX64.EFI"
SHIM_SRC="$MNT/usr/lib/shim/shimx64.efi.signed"
[ -f "$SHIM_SRC" ] || SHIM_SRC="$MNT/usr/lib/shim/shimx64.efi.signed.latest"

[ -f "$BOOTX64" ] || die "EFI/BOOT/BOOTX64.EFI absent : la cle ne demarrera pas"

if [ -f "$SHIM_SRC" ] \
   && [ "$(sha256sum <"$BOOTX64" | cut -d' ' -f1)" = "$(sha256sum <"$SHIM_SRC" | cut -d' ' -f1)" ]; then
    ok "BOOTX64.EFI est bien le shim signe Microsoft"
else
    warn "BOOTX64.EFI ne correspond pas au shim signe attendu"
    warn "le demarrage echouera probablement avec Secure Boot actif"
fi

[ -e "$MNT/boot/efi/EFI/BOOT/grubx64.efi" ] \
    && ok "present : EFI/BOOT/grubx64.efi" \
    || die "grubx64.efi absent : shim n'aura rien a charger"

# LE POINT QUI DECIDE DU DEMARRAGE SUR MATERIEL REEL.
#
# Le grubx64.efi signe par Debian embarque un prefixe code en dur,
# /EFI/debian, et y cherche sa configuration. En installation amovible,
# grub-install ne depose celle-ci que dans /EFI/BOOT : GRUB ne la trouve
# alors qu'en retombant sur $cmdpath, le repertoire d'ou il a ete charge.
#
# Ce repli fonctionne sous OVMF mais PAS sur tous les firmwares : un AMI de
# portable ASUS n'expose pas le chemin de peripherique de la meme maniere,
# et GRUB tombe dans son interpreteur avec "Minimal BASH-like line editing".
# Le test QEMU ne peut pas detecter cette difference.
#
# On depose donc la configuration AUX DEUX emplacements : le prefixe
# embarque est satisfait directement, sans dependre d'aucun repli.
[ -e "$MNT/boot/efi/EFI/BOOT/grub.cfg" ] \
    || die "aucun grub.cfg sur l'ESP : GRUB ne trouverait pas sa configuration"
ok "present : EFI/BOOT/grub.cfg (chemin amovible)"

mkdir -p "$MNT/boot/efi/EFI/debian"
cp "$MNT/boot/efi/EFI/BOOT/grub.cfg" "$MNT/boot/efi/EFI/debian/grub.cfg"
ok "present : EFI/debian/grub.cfg (prefixe embarque dans le GRUB signe)"

# Verification que le prefixe attendu par le binaire est bien celui qu'on
# vient de servir : si Debian le changeait, ce controle le signalerait.
GRUB_PREFIX="$(strings -a "$MNT/boot/efi/EFI/BOOT/grubx64.efi" 2>/dev/null \
               | grep -m1 '^/EFI/' || true)"
if [ -n "$GRUB_PREFIX" ]; then
    if [ -e "$MNT/boot/efi$GRUB_PREFIX/grub.cfg" ]; then
        ok "prefixe embarque $GRUB_PREFIX : configuration en place"
    else
        die "le GRUB signe attend sa configuration dans $GRUB_PREFIX, absente de l'ESP"
    fi
fi

# Le grub.cfg de l'ESP n'est qu'un relais : il doit designer la racine par
# UUID pour que la cle demarre quelle que soit la machine.
if grep -q "$UUID_ROOT" "$MNT/boot/efi/EFI/BOOT/grub.cfg" 2>/dev/null \
   && grep -q "$UUID_ROOT" "$MNT/boot/efi/EFI/debian/grub.cfg" 2>/dev/null; then
    ok "les deux relais grub.cfg designent la racine par UUID"
else
    warn "le relais grub.cfg ne mentionne pas l'UUID de la racine -- a verifier"
    sed 's/^/      /' "$MNT/boot/efi/EFI/BOOT/grub.cfg" 2>/dev/null | head -10
fi

# Aucune entree NVRAM ne doit avoir ete creee. Le chroot n'a pas d'efivarfs
# monte : grub-install y etait materiellement hors d'etat d'ecrire.
if [ "$MODE" = "device" ]; then
    if command -v efibootmgr >/dev/null 2>&1 && efibootmgr 2>/dev/null | grep -qi 'claude'; then
        warn "une entree NVRAM 'claude' existe -- inattendu, a verifier"
    else
        ok "aucune entree NVRAM creee : l'amorcage de Windows est intact"
    fi
else
    ok "mode image : aucun acces a la NVRAM par construction"
fi

echo
info "Contenu de l'ESP :"
find "$MNT/boot/efi" -type f | sed "s|$MNT/boot/efi|      |" | sort

step "Ecriture de la partition EFI"
( cd "$MNT/boot/efi" && mcopy -s -i "$ESP_IMG" ./* :: ) \
    || die "mcopy a echoue -- impossible de peupler la partition EFI"
dd if="$ESP_IMG" of="$P_ESP" bs=1M conv=fsync status=none \
    || die "ecriture de la partition EFI impossible"

# Relecture depuis la partition elle-meme : on verifie ce qui a reellement
# ete ecrit, pas ce qu'on croit avoir ecrit.
if mdir -i "$P_ESP" -/ :: >/dev/null 2>&1; then
    ok "partition EFI ecrite et relue"
    mdir -i "$P_ESP" -/ :: 2>/dev/null | grep -iE 'BOOTX64|grubx64|grub.cfg' | sed 's/^/      /' || true
else
    die "la partition EFI est illisible apres ecriture"
fi

# Les fichiers ne servent plus sur la racine : /boot/efi redevient un point
# de montage vide, comme le declare fstab.
rm -rf "${MNT:?}/boot/efi"/*
rm -rf "$ESP_WORK"

# ---------------------------------------------------------------------------
# Finalisation
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Auto-controle du systeme construit
#
# debootstrap --variant=minbase produit un systeme tres reduit, et
# --no-install-recommends n'y ajoute rien d'implicite. Un outil peut donc
# manquer sans qu'aucune etape n'echoue : le defaut n'apparait qu'au premier
# demarrage sur la machine cible, quand il est trop tard. On verifie ici la
# presence effective de ce dont le systeme depend au demarrage.
# ---------------------------------------------------------------------------
step "Auto-controle du systeme construit"
selftest_fail=0

[ "${CLAUDE_DESKTOP_OK:-0}" = "1" ] && SELFTEST_BINS="claude-desktop" || SELFTEST_BINS=""
for b in fsck.ext4 resize2fs sgdisk partx udevadm chromium tlp $SELFTEST_BINS; do
    if in_chroot sh -c "command -v $b" >/dev/null 2>&1; then
        ok "present : $b"
    else
        warn "ABSENT : $b"
        selftest_fail=1
    fi
done

# resize2fs merite une mention a part : sans lui, le premier demarrage
# agrandit la partition mais pas le systeme de fichiers, et l'utilisateur
# perd la quasi-totalite de sa cle sans le moindre message d'erreur.
in_chroot sh -c "command -v resize2fs" >/dev/null 2>&1 \
    || warn "sans resize2fs, la racine restera a sa taille d'image"

if [ -L "$MNT/etc/resolv.conf" ] && [ ! -e "$MNT/etc/resolv.conf" ]; then
    warn "ABSENT : la cible de /etc/resolv.conf (lien mort : pas de DNS)"
    selftest_fail=1
else
    ok "/etc/resolv.conf : laisse a NetworkManager"
fi

[ -L "$MNT/etc/systemd/system/display-manager.service" ] \
    && ok "session graphique : $(basename "$(readlink "$MNT/etc/systemd/system/display-manager.service")")" \
    || { warn "aucun gestionnaire de session actif"; selftest_fail=1; }

for svc in claude-os-dgpu-power claude-os-firstboot tlp NetworkManager; do
    if find "$MNT/etc/systemd/system" -name "${svc}.service" -path '*.wants*' \
         | grep -q .; then
        ok "active : $svc"
    else
        warn "NON ACTIVE : $svc"
        selftest_fail=1
    fi
done

[ "$selftest_fail" -eq 0 ] \
    && ok "auto-controle complet" \
    || die "auto-controle en echec -- l'image ne fonctionnerait pas correctement"

step "Finalisation"
rm -f "$MNT/usr/sbin/policy-rc.d"
in_chroot apt-get clean
# /etc/resolv.conf : le laisser a NetworkManager, qui l'ecrit lui-meme.
#
# Il pointait auparavant vers /run/systemd/resolve/stub-resolv.conf, alors
# que systemd-resolved n'est pas installe : un lien mort, donc aucune
# resolution DNS. La liaison reseau montait correctement et tout nom de
# domaine restait introuvable -- panne d'autant plus deroutante.
rm -f "$MNT/etc/resolv.conf"
# Identifiant machine regenere au premier demarrage.
: > "$MNT/etc/machine-id"
sync

# Liberer les blocs des fichiers supprimes.
#
# apt-get clean efface les .deb telecharges, mais leurs blocs continuent de
# porter les anciennes donnees : environ 1 Gio de contenu aleatoire qui
# gonfle l'image et resiste a la compression. fstrim les rend a nouveau
# nuls, ce qui reduit l'image d'autant et ameliore nettement le taux de
# compression.
#
# Sur une vraie cle, l'operation a un second interet : elle indique au
# controleur quels blocs sont libres, information qu'il n'a autrement pas.
if fstrim -v "$MNT" >>"$LOGFILE" 2>&1; then
    ok "blocs inutilises liberes : $(grep -o '[0-9.]* [KMG]iB' "$LOGFILE" | tail -1)"
else
    info "fstrim indisponible sur ce support (sans consequence sur le fonctionnement)"
fi
sync

echo
printf '%s' "$c_grn"
cat <<DONEEOF
  +-------------------------------------------------------------+
  |  Claude-OS construit                                        |
  +-------------------------------------------------------------+
DONEEOF
printf '%s' "$c_off"

if [ "$MODE" = "image" ]; then
    cleanup; trap - EXIT
    IMG_SIZE="$(du -h "$IMAGE_PATH" | cut -f1)"
    cat <<IMGEOF
    Image : $IMAGE_PATH ($IMG_SIZE)

    L'ecrire ensuite sur la cle depuis n'importe quel systeme :
      Windows : balenaEtcher, ou Rufus en mode "Image DD"
      Linux   : dd if=... of=/dev/sdX bs=4M status=progress conv=fsync

    La partition racine s'adaptera d'elle-meme a la taille de la cle
    au premier demarrage.
IMGEOF
else
    cat <<DEVEOF
    Pour demarrer : redemarrer, maintenir Echap (ou F8) et choisir la cle
    dans le menu du firmware. Ne rien changer dans le BIOS.
DEVEOF
fi

cat <<NEXTEOF

    Au premier demarrage :
      1. etat du dGPU        journalctl -b -u claude-os-dgpu-power
      2. Secure Boot         mokutil --sb-state
      3. taille de la racine df -h /
      4. acceleration video  vainfo
      5. consommation reelle sudo powertop

    Journal complet : $LOGFILE
NEXTEOF
