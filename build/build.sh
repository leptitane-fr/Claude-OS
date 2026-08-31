#!/usr/bin/env bash
#
# Claude-OS -- construction du systeme sur la cle USB.
#
# A lancer depuis un Linux live (Mint, Debian live...) en root :
#     sudo ./build/build.sh
#
# Le script est idempotent au sens ou il repart toujours d'un partitionnement
# neuf : relancer reconstruit la cle a l'identique. C'est voulu -- on itere
# sur la recette, pas sur le resultat.

set -Eeuo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MNT="/mnt/claude-os-build"
LOGFILE="${REPO_DIR}/build.log"

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
        for m in dev/pts dev proc sys/firmware/efi/efivars sys run boot/efi home ''; do
            umount -l "$MNT/$m" 2>/dev/null
        done
        sync
        ok "cle demontee -- extraction sans risque"
    fi
}
trap cleanup EXIT
trap 'die "interrompu ligne $LINENO"' ERR

# ---------------------------------------------------------------------------
# Chargement de la configuration
# ---------------------------------------------------------------------------
: >"$LOGFILE"
[ "$(id -u)" -eq 0 ] || die "a lancer en root : sudo $0"

# shellcheck source=config/default.conf
. "${REPO_DIR}/build/config/default.conf"
if [ -r "${REPO_DIR}/build/config/local.conf" ]; then
    . "${REPO_DIR}/build/config/local.conf"
    info "surcharge locale chargee depuis build/config/local.conf"
fi

step "Verification des outils requis"
missing=()
for t in debootstrap sgdisk mkfs.ext4 mkfs.vfat blkid udevadm partprobe chroot; do
    command -v "$t" >/dev/null 2>&1 || missing+=("$t")
done
[ ${#missing[@]} -eq 0 ] || die "outils manquants : ${missing[*]}
    Installer avec :  apt install debootstrap gdisk dosfstools e2fsprogs"
ok "tous les outils sont presents"

# ---------------------------------------------------------------------------
# VALIDATION DE LA CIBLE
#
# La section la plus importante du script. Une erreur ici detruit le disque
# Windows -- exactement ce que le projet s'interdit. Chaque garde-fou ci-dessous
# est une raison independante de refuser d'ecrire.
# ---------------------------------------------------------------------------
step "Validation du peripherique cible"

if [ -z "${TARGET_DEVICE}" ]; then
    echo
    lsblk -o NAME,SIZE,TYPE,TRAN,RM,MODEL,MOUNTPOINTS
    echo
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
[ "$DEV_GB" -ge 32 ] || die "$TARGET_DEVICE ne fait que ${DEV_GB} Go ; il en faut au moins 32."
[ "$DEV_GB" -le 2000 ] || die "$TARGET_DEVICE fait ${DEV_GB} Go -- trop gros pour une cle. Verifier la cible."

MIN_MB=$(( SIZE_ESP + SIZE_ROOT + SIZE_HOME + 64 ))
[ $(( DEV_BYTES / 1048576 )) -ge "$MIN_MB" ] \
    || die "cle trop petite : ${MIN_MB} MiB requis par la configuration"

echo
printf '%s' "$c_yel"
cat <<BANNER
  ┌─────────────────────────────────────────────────────────────┐
  │  TOUTES LES DONNEES DE CE PERIPHERIQUE SERONT DETRUITES      │
  └─────────────────────────────────────────────────────────────┘
BANNER
printf '%s' "$c_off"
printf '    Peripherique : %s\n' "$TARGET_DEVICE"
printf '    Modele       : %s\n' "${UD_ID_MODEL:-inconnu}"
printf '    Identifiant  : %s:%s\n' "${UD_ID_VENDOR_ID:-????}" "${UD_ID_MODEL_ID:-????}"
printf '    Taille       : %s Go\n' "$DEV_GB"
echo
read -rp "    Retaper le chemin exact du peripherique pour confirmer : " confirm
[ "$confirm" = "$TARGET_DEVICE" ] || die "confirmation non conforme -- rien n'a ete ecrit"

USB_DENYLIST="${UD_ID_VENDOR_ID:-}:${UD_ID_MODEL_ID:-}"
[ "$USB_DENYLIST" = ":" ] && USB_DENYLIST=""
ok "cible validee ; exclusion TLP d'autosuspend : ${USB_DENYLIST:-<aucune>}"

# ---------------------------------------------------------------------------
# Mot de passe du compte
# ---------------------------------------------------------------------------
step "Compte utilisateur"
while :; do
    read -rsp "    Mot de passe pour '$USERNAME' : " PW1; echo
    read -rsp "    Confirmer                     : " PW2; echo
    [ -n "$PW1" ] && [ "$PW1" = "$PW2" ] && break
    warn "mots de passe vides ou differents"
done

# ---------------------------------------------------------------------------
# Partitionnement
# ---------------------------------------------------------------------------
step "Partitionnement de $TARGET_DEVICE"
wipefs -a "$TARGET_DEVICE" >/dev/null
sgdisk --zap-all "$TARGET_DEVICE" >/dev/null

sgdisk \
    -n 1:0:+${SIZE_ESP}M   -t 1:ef00 -c 1:"CLAUDEOS-ESP"  \
    -n 2:0:+${SIZE_ROOT}M  -t 2:8304 -c 2:"CLAUDEOS-ROOT" \
    -n 3:0:+${SIZE_HOME}M  -t 3:8302 -c 3:"CLAUDEOS-HOME" \
    "$TARGET_DEVICE" >/dev/null

partprobe "$TARGET_DEVICE"; udevadm settle; sleep 2

# Les cles USB n'utilisent pas le suffixe p ; les lecteurs NVMe/MMC si. On
# resout les noms plutot que de les supposer.
mapfile -t PARTS < <(lsblk -nro NAME "$TARGET_DEVICE" | tail -n +2)
[ "${#PARTS[@]}" -ge 3 ] || die "partitionnement incomplet : ${#PARTS[@]} partitions detectees"
P_ESP="/dev/${PARTS[0]}"; P_ROOT="/dev/${PARTS[1]}"; P_HOME="/dev/${PARTS[2]}"
ok "ESP=$P_ESP  root=$P_ROOT  home=$P_HOME"
info "espace non alloue laisse au wear-leveling du controleur : $(( (DEV_BYTES/1048576 - MIN_MB) / 1024 )) GiB"

step "Formatage"
mkfs.vfat -F32 -n CLAUDEOS-EFI "$P_ESP" >/dev/null
# -O ^has_journal serait tentant pour le flash, mais un arrachage de cle sans
# journal corrompt le systeme de fichiers. On garde le journal et on espace
# ses ecritures via commit=600 dans fstab.
mkfs.ext4 -q -F -L claudeos-root -m 1 "$P_ROOT"
mkfs.ext4 -q -F -L claudeos-home -m 0 "$P_HOME"
ok "systemes de fichiers crees"

UUID_ESP="$(blkid -s UUID -o value "$P_ESP")"
UUID_ROOT="$(blkid -s UUID -o value "$P_ROOT")"
UUID_HOME="$(blkid -s UUID -o value "$P_HOME")"

# ---------------------------------------------------------------------------
# Montage
# ---------------------------------------------------------------------------
step "Montage sur $MNT"
mkdir -p "$MNT"
mount -o noatime "$P_ROOT" "$MNT"
mkdir -p "$MNT/home" "$MNT/boot/efi"
mount -o noatime "$P_HOME" "$MNT/home"
mount -o umask=0077 "$P_ESP" "$MNT/boot/efi"
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

in_chroot() { chroot "$MNT" /usr/bin/env DEBIAN_FRONTEND=noninteractive LC_ALL=C "$@"; }

cat > "$MNT/etc/apt/sources.list" <<APTEOF
deb $MIRROR $SUITE $COMPONENTS
deb $MIRROR ${SUITE}-updates $COMPONENTS
deb http://security.debian.org/debian-security ${SUITE}-security $COMPONENTS
APTEOF

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
in_chroot apt-get install -y --no-install-recommends "${PKGS[@]}" >>"$LOGFILE" 2>&1 \
    || die "installation des paquets echouee -- voir $LOGFILE"
in_chroot dpkg -l linux-image-amd64 shim-signed grub-efi-amd64-signed >/dev/null \
    || die "paquets d'amorcage absents -- Secure Boot serait impossible"
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
        -e "s|@UUID_HOME@|$UUID_HOME|g" \
        -e "s|@UUID_ESP@|$UUID_ESP|g" \
        -e "s|@USB_DENYLIST@|$USB_DENYLIST|g" \
        -e "s|@DGPU_BDF@|$DGPU_BDF|g" \
        -e "s|@CMDLINE_DGPU@|$CMDLINE_DGPU|g" \
        "$f"
done

echo "DGPU_STRATEGY=\"$DGPU_STRATEGY\"" > "$MNT/etc/default/claude-os-dgpu"

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

in_chroot useradd -m -s /bin/bash -G sudo,audio,video,netdev,plugdev \
    -c "$USER_FULLNAME" "$USERNAME"
echo "$USERNAME:$PW1" | in_chroot chpasswd
in_chroot passwd -l root >/dev/null   # pas de connexion root directe ; sudo suffit
unset PW1 PW2
ok "compte '$USERNAME' cree"

step "Activation des services"
in_chroot systemctl enable lightdm NetworkManager tlp thermald \
    claude-os-dgpu-power.service >/dev/null 2>&1 || true
# TLP et power-profiles-daemon se marchent dessus ; TLP l'emporte.
in_chroot systemctl mask power-profiles-daemon.service >/dev/null 2>&1 || true
in_chroot systemctl mask systemd-rfkill.service systemd-rfkill.socket >/dev/null 2>&1 || true
ok "services actives"

# ---------------------------------------------------------------------------
# Claude Desktop -- depot APT officiel d'Anthropic
# ---------------------------------------------------------------------------
step "Claude Desktop"
CLAUDE_KEY_FPR="31DDDE24DDFAB679F42D7BD2BAA929FF1A7ECACE"
if in_chroot curl -fsSLo /usr/share/keyrings/claude-desktop-archive-keyring.asc \
        https://downloads.claude.ai/claude-desktop/key.asc 2>/dev/null; then
    fpr="$(in_chroot gpg --show-keys --with-colons \
            /usr/share/keyrings/claude-desktop-archive-keyring.asc 2>/dev/null \
          | awk -F: '/^fpr:/{print $10; exit}')"
    if [ "$fpr" = "$CLAUDE_KEY_FPR" ]; then
        echo "deb [arch=amd64 signed-by=/usr/share/keyrings/claude-desktop-archive-keyring.asc] https://downloads.claude.ai/claude-desktop/apt/stable stable main" \
            > "$MNT/etc/apt/sources.list.d/claude-desktop.list"
        in_chroot apt-get update -qq
        if in_chroot apt-get install -y --no-install-recommends claude-desktop >/dev/null 2>&1; then
            ok "Claude Desktop installe depuis le depot officiel"
        else
            warn "installation echouee -- depot enregistre, reessayer apres le premier demarrage"
        fi
    else
        rm -f "$MNT/usr/share/keyrings/claude-desktop-archive-keyring.asc"
        warn "empreinte de cle inattendue ($fpr) -- depot NON enregistre"
        warn "attendue : $CLAUDE_KEY_FPR"
    fi
else
    warn "cle de signature inaccessible (reseau ?) -- Claude Desktop a installer manuellement"
fi

# ---------------------------------------------------------------------------
# Amorcage : Secure Boot, chemin amovible, aucune ecriture NVRAM
# ---------------------------------------------------------------------------
step "Installation de GRUB (Secure Boot, cible amovible)"

# Sans efivarfs, grub-install ne peut pas toucher a la NVRAM meme s'il le
# voulait. Ceinture et bretelles, en complement de --no-nvram.
in_chroot debconf-set-selections <<'DEBEOF'
grub2/force_efi_extra_removable boolean true
grub-efi/install_devices_empty   boolean true
DEBEOF

in_chroot grub-install \
    --target=x86_64-efi \
    --efi-directory=/boot/efi \
    --boot-directory=/boot \
    --removable \
    --no-nvram \
    --uefi-secure-boot \
    --recheck >>"$LOGFILE" 2>&1 || die "grub-install a echoue -- voir $LOGFILE"

in_chroot update-grub >>"$LOGFILE" 2>&1 || die "update-grub a echoue -- voir $LOGFILE"
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

for f in EFI/BOOT/grubx64.efi EFI/debian/grub.cfg; do
    [ -e "$MNT/boot/efi/$f" ] && ok "present : $f" \
                              || warn "absent : $f -- GRUB pourrait ne pas trouver sa configuration"
done

# Aucune entree NVRAM ne doit avoir ete creee.
if command -v efibootmgr >/dev/null 2>&1 && efibootmgr 2>/dev/null | grep -qi 'claude\|debian'; then
    warn "une entree NVRAM 'debian' existe -- verifier qu'elle preexistait a ce build"
else
    ok "aucune entree NVRAM creee : l'amorcage de Windows est intact"
fi

echo
info "Contenu de l'ESP :"
find "$MNT/boot/efi" -type f | sed "s|$MNT/boot/efi|      |" | sort

# ---------------------------------------------------------------------------
# Finalisation
# ---------------------------------------------------------------------------
step "Finalisation"
rm -f "$MNT/usr/sbin/policy-rc.d"
in_chroot apt-get clean
rm -f "$MNT/etc/resolv.conf"
in_chroot ln -sf /run/systemd/resolve/stub-resolv.conf /etc/resolv.conf 2>/dev/null || true
# Identifiant machine regenere au premier demarrage.
: > "$MNT/etc/machine-id"
sync

echo
printf '%s' "$c_grn"
cat <<DONEEOF
  ┌─────────────────────────────────────────────────────────────┐
  │  Claude-OS construit                                        │
  └─────────────────────────────────────────────────────────────┘
DONEEOF
printf '%s' "$c_off"
cat <<NEXTEOF
    Pour demarrer : redemarrer, maintenir Echap (ou F8) et choisir la cle
    dans le menu du firmware. Ne rien changer dans le BIOS.

    Au premier demarrage :
      1. verifier l'etat du dGPU :   journalctl -b -u claude-os-dgpu-power
      2. verifier Secure Boot :      mokutil --sb-state
      3. verifier l'acceleration :   vainfo
      4. mesurer la consommation :   powertop

    Journal complet : $LOGFILE
NEXTEOF
