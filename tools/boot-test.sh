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
LOG="$WORK/boot.log"; : >"$LOG"

KVM=()
[ -w /dev/kvm ] && KVM=(-enable-kvm -cpu host)
echo "acceleration : ${KVM[*]:-aucune (TCG -- comptez plusieurs minutes)}"
echo "duree maximale : ${TIMEOUT}s"

# La cle est presentee comme un peripherique USB, comme sur la machine
# reelle : cela exerce le meme chemin d'enumeration au demarrage.
qemu-system-x86_64 "${KVM[@]}" \
    -m 2048 -smp 2 -machine q35 \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,unit=1,file="$WORK/vars.fd" \
    -drive id=usbdisk,file="$IMG",format=raw,if=none \
    -device qemu-xhci -device usb-storage,drive=usbdisk \
    -serial file:"$LOG" -display none -daemonize -pidfile "$WORK/qemu.pid" \
    >/dev/null 2>&1 || { echo "qemu n'a pas demarre" >&2; exit 1; }

QPID="$(cat "$WORK/qemu.pid")"
stop_qemu() { kill "$QPID" 2>/dev/null; }
trap 'stop_qemu; rm -rf "$WORK"' EXIT

# On s'arrete des qu'un etat terminal est atteint, au lieu d'attendre la
# duree maximale : un demarrage reussi n'a plus rien a dire apres l'invite
# de connexion, et laisser tourner coute vingt minutes par execution.
elapsed=0; outcome="timeout"
while [ "$elapsed" -lt "$TIMEOUT" ]; do
    kill -0 "$QPID" 2>/dev/null || { outcome="qemu-arrete"; break; }
    if grep -qi 'Security Violation\|access denied\|Image failed to load' "$LOG"; then
        outcome="violation"; break
    fi
    if grep -qi 'Dropping to a shell\|Kernel panic\|Cannot open root' "$LOG"; then
        outcome="panne-amorcage"; break
    fi
    if grep -q 'login:' "$LOG"; then
        outcome="succes"; break
    fi
    sleep 5; elapsed=$(( elapsed + 5 ))
done
stop_qemu

echo
echo "================= SORTIE SERIE ================="
sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' "$LOG" | tail -30

echo
echo "================= VERDICT ================="
fail=0
case "$outcome" in
    succes)
        echo "  OK    : invite de connexion atteinte sous Secure Boot applique (${elapsed}s)" ;;
    violation)
        echo "  ECHEC : le firmware a refuse un binaire -- chaine Secure Boot invalide"; fail=1 ;;
    panne-amorcage)
        echo "  ECHEC : le noyau demarre mais n'atteint pas la racine"
        grep -iE 'ALERT|Cannot open root|panic' "$LOG" | head -3 | sed 's/^/          /'; fail=1 ;;
    qemu-arrete)
        echo "  ECHEC : qemu s'est arrete avant l'invite de connexion"; fail=1 ;;
    timeout)
        echo "  ECHEC : duree maximale de ${TIMEOUT}s depassee sans invite de connexion"; fail=1 ;;
esac

grep -q 'fsck.*error\|fsck exited with status' "$LOG" \
    && echo "  ALERTE: fsck a echoue -- e2fsprogs ou dosfstools manquant ?"

cp "$LOG" ./boot-test.log 2>/dev/null && echo "  journal complet : ./boot-test.log"
exit "$fail"
