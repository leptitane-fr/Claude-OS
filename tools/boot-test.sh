#!/usr/bin/env bash
# Claude-OS -- test de demarrage sous Secure Boot applique.
#
# Demarre une image dans QEMU avec un firmware OVMF portant les cles
# Microsoft et appliquant Secure Boot. Ce n'est pas une simulation : c'est
# la meme verification cryptographique que celle du firmware du portable.
# Si le shim n'etait pas correctement signe, le firmware refuserait de le
# lancer et le test echouerait.
#
# Usage :  ./tools/boot-test.sh claude-os.img [duree_max_secondes]

set -uo pipefail

IMG="${1:-}"
TIMEOUT="${2:-900}"
[ -f "$IMG" ] || { echo "Usage : $0 <image.img> [duree_max]" >&2; exit 1; }

OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.secboot.fd
OVMF_VARS=/usr/share/OVMF/OVMF_VARS_4M.ms.fd   # cles Microsoft enrolees
for f in "$OVMF_CODE" "$OVMF_VARS"; do
    [ -r "$f" ] || { echo "firmware absent : $f (apt install ovmf)" >&2; exit 1; }
done

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
cp "$OVMF_VARS" "$WORK/vars.fd"; chmod +w "$WORK/vars.fd"
LOG="$WORK/boot.log"

KVM=()
[ -w /dev/kvm ] && KVM=(-enable-kvm -cpu host)
echo "acceleration : ${KVM[*]:-aucune (TCG -- comptez plusieurs minutes)}"
echo "duree maximale : ${TIMEOUT}s"

# La cle est presentee comme un peripherique USB, comme sur la machine reelle :
# cela exerce le meme chemin d'enumeration au demarrage.
timeout "$TIMEOUT" qemu-system-x86_64 "${KVM[@]}" \
    -m 2048 -smp 2 -machine q35 \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,unit=1,file="$WORK/vars.fd" \
    -drive id=usbdisk,file="$IMG",format=raw,if=none \
    -device qemu-xhci -device usb-storage,drive=usbdisk \
    -serial mon:stdio -display none -nographic 2>&1 | tee "$LOG"

echo
echo "================= VERDICT ================="
fail=0

if grep -qi 'Security Violation\|access denied\|Image failed to load' "$LOG"; then
    echo "  ECHEC : le firmware a refuse un binaire -- chaine Secure Boot invalide"
    fail=1
fi
if grep -q 'Secure boot enabled' "$LOG"; then
    echo "  OK    : le noyau confirme Secure Boot actif"
elif grep -qi 'secure boot' "$LOG"; then
    grep -i 'secure boot' "$LOG" | head -2 | sed 's/^/  info  : /'
fi
if grep -q 'login:' "$LOG"; then
    echo "  OK    : invite de connexion atteinte sous Secure Boot applique"
else
    echo "  ECHEC : invite de connexion jamais atteinte"
    fail=1
fi
grep -q 'claude-os-dgpu' "$LOG" && echo "  info  : le service dGPU s'est execute"
grep -q 'claude-os-firstboot' "$LOG" && echo "  info  : le service de premier demarrage s'est execute"

cp "$LOG" ./boot-test.log 2>/dev/null && echo "  journal complet : ./boot-test.log"
exit "$fail"
