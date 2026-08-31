#!/usr/bin/env bash
# Claude-OS : sonde matérielle en LECTURE SEULE.
#
# Ce script ne monte rien, ne modifie aucune partition, n'ecrit nulle part
# ailleurs que dans son propre rapport. Il masque les numeros de serie.
#
# Usage :  sudo ./tools/hw-probe.sh
# Sortie :  ./claude-os-hw-report.txt   (a me renvoyer tel quel)

set -uo pipefail

OUT="${1:-$PWD/claude-os-hw-report.txt}"
: >"$OUT"

say()  { printf '\n\n===== %s =====\n' "$*" >>"$OUT"; }
note() { printf '%s\n' "$*" >>"$OUT"; }
run()  {
  printf '\n--- $ %s\n' "$*" >>"$OUT"
  if command -v "${1%% *}" >/dev/null 2>&1 || type "${1%% *}" >/dev/null 2>&1; then
    eval "$*" >>"$OUT" 2>&1 || note "(code de retour $?)"
  else
    note "(commande absente : ${1%% *})"
  fi
}

[ "$(id -u)" -eq 0 ] || { echo "A lancer en root : sudo $0" >&2; exit 1; }

note "Claude-OS hw-probe  --  $(date -u '+%F %T UTC')"
note "Noyau du live : $(uname -r)  /  $(uname -m)"

# --------------------------------------------------------------------------
say "1. FIRMWARE / SECURE BOOT"
run "mokutil --sb-state"
run "bootctl status 2>/dev/null | head -30"
note ""
note "Variables UEFI presentes (noms seulement, pas de contenu) :"
ls /sys/firmware/efi/efivars/ 2>/dev/null | sed 's/-[0-9a-f-]\{36\}$//' | sort -u | tr '\n' ' ' >>"$OUT"
note ""
note "SetupMode / SecureBoot (1 = actif) :"
for v in SecureBoot SetupMode; do
  f=$(ls /sys/firmware/efi/efivars/${v}-* 2>/dev/null | head -1)
  [ -n "$f" ] && note "  $v = $(od -An -t u1 -j4 -N1 "$f" 2>/dev/null | tr -d ' ')"
done
run "efibootmgr -v | sed 's/[0-9A-F]\{16,\}/<HEX>/g'"

# --------------------------------------------------------------------------
say "2. MACHINE / BIOS"
run "dmidecode -t bios -t system -t baseboard 2>/dev/null | grep -viE 'serial|uuid|asset'"
run "cat /sys/class/dmi/id/product_name /sys/class/dmi/id/bios_version /sys/class/dmi/id/bios_date"

# --------------------------------------------------------------------------
say "3. CPU"
run "lscpu | grep -viE 'serial'"
run "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver"
run "cat /sys/devices/system/cpu/intel_pstate/status"
run "cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_available_preferences"
note ""
note "Cœurs P/E (type 0x40=E, 0x20=P sur hybride Intel) :"
run "grep -H . /sys/devices/system/cpu/cpu*/topology/core_type 2>/dev/null | head -30"
run "cat /sys/firmware/acpi/platform_profile_choices 2>/dev/null"

# --------------------------------------------------------------------------
say "4. GPU — LE POINT CRITIQUE"
run "lspci -nnk | grep -iEA3 'vga|3d|display'"
note ""
note ">>> Etat d'alimentation et capacite D3cold du dGPU NVIDIA :"
for d in /sys/bus/pci/devices/*/; do
  ven=$(cat "$d/vendor" 2>/dev/null)
  cls=$(cat "$d/class"  2>/dev/null)
  case "$ven:$cls" in
    0x10de:0x030*|0x10de:0x0302*)
      bdf=$(basename "$d")
      note ""
      note "  dGPU trouve : $bdf"
      note "    power_state    = $(cat "$d/power_state" 2>/dev/null)"
      note "    power/control  = $(cat "$d/power/control" 2>/dev/null)"
      note "    d3cold_allowed = $(cat "$d/d3cold_allowed" 2>/dev/null)"
      note "    pilote charge  = $(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null || echo aucun)"
      note "    chemin ACPI    = $(cat "$d/firmware_node/path" 2>/dev/null)"
      note "    real_power_state = $(cat "$d/firmware_node/real_power_state" 2>/dev/null)"
      # Le pont PCIe parent porte les power resources _PR3
      par=$(basename "$(dirname "$(readlink -f "$d")")")
      note ""
      note "  Pont PCIe parent : $par"
      note "    chemin ACPI    = $(cat "/sys/bus/pci/devices/$par/firmware_node/path" 2>/dev/null)"
      note "    power/control  = $(cat "/sys/bus/pci/devices/$par/power/control" 2>/dev/null)"
      note "    >>> power resources exposees (la presence de _PR3 = D3cold possible) :"
      ls "/sys/bus/pci/devices/$par/firmware_node/" 2>/dev/null \
        | grep -i 'power_resource' | sed 's/^/        /' >>"$OUT" \
        || note "        (aucune -- D3cold via ACPI probablement indisponible)"
      ;;
  esac
done
note ""
note ">>> Recherche de _PR3 dans la DSDT (methode de reference) :"
if command -v acpidump >/dev/null 2>&1 && command -v iasl >/dev/null 2>&1; then
  tmp=$(mktemp -d); ( cd "$tmp" && acpidump -b >/dev/null 2>&1 && iasl -d dsdt.dat >/dev/null 2>&1 \
    && { grep -c '_PR3' dsdt.dsl | sed 's/^/    occurrences de _PR3 : /'; \
         grep -B2 -A8 'Name (_PR3' dsdt.dsl | head -40; } ) >>"$OUT" 2>&1
  rm -rf "$tmp"
else
  note "    (acpidump/iasl absents -- installer : apt install acpica-tools)"
fi
note ""
note ">>> MUX materiel (Vivobook : bascule dGPU cote firmware) :"
run "ls -l /sys/devices/platform/asus-nb-wmi/ 2>/dev/null"
run "cat /sys/devices/platform/asus-nb-wmi/gpu_mux_mode 2>/dev/null"
run "cat /sys/devices/platform/asus-nb-wmi/dgpu_disable 2>/dev/null"

# --------------------------------------------------------------------------
say "5. STOCKAGE (aucun montage effectue)"
run "lsblk -o NAME,SIZE,TYPE,FSTYPE,PARTTYPENAME,TRAN,ROTA,MODEL"
note ""
note ">>> Intel VMD / RST actif ? (si present : le SSD interne peut etre rendu invisible)"
run "lspci -nn | grep -i 'volume management'"
run "lspci -nn | grep -iE 'sata|nvme|raid'"

# --------------------------------------------------------------------------
say "6. CLE USB CIBLE (pour l'exclusion d'autosuspend USB)"
note "Reperer ci-dessous la PNY 256 Go : son ID vendor:product ira dans la denylist TLP."
note "Sans cette exclusion, l'autosuspend USB peut geler le systeme de fichiers racine."
run "lsusb"
run "lsusb -t"

# --------------------------------------------------------------------------
say "7. RESEAU / AUDIO / FIRMWARES"
run "lspci -nnk | grep -iEA3 'network|ethernet|audio'"
note ""
note ">>> Firmwares reclames par le noyau (indique les paquets non-free necessaires) :"
run "dmesg | grep -iE 'firmware' | grep -iE 'fail|missing|direct load|not found' | sort -u | head -40"

# --------------------------------------------------------------------------
say "8. ENERGIE — MESURE DE REFERENCE"
run "cat /sys/class/power_supply/BAT*/power_now 2>/dev/null"
run "grep -H . /sys/class/power_supply/BAT*/{energy_full_design,energy_full,cycle_count,status} 2>/dev/null"
note ""
note ">>> Etats de veille supportes (les portables recents n'ont souvent que s2idle) :"
run "cat /sys/power/mem_sleep"
run "cat /sys/power/state"
note ""
note ">>> Residence dans les C-states du package (plus c'est haut, mieux c'est) :"
run "grep -H . /sys/devices/system/cpu/cpuidle/low_power_idle_*_residency_us 2>/dev/null"
run "turbostat --quiet --show PkgWatt,CorWatt,GFXWatt,Pkg%pc2,Pkg%pc6,Pkg%pc8,Pkg%pc10 sleep 5 2>&1 | head -20"

# --------------------------------------------------------------------------
say "9. ACPI — ERREURS EVENTUELLES"
run "dmesg | grep -iE 'acpi.*(error|exception|bug|warn)' | sort -u | head -30"

# --------------------------------------------------------------------------
sed -i -E 's/\b[A-Z0-9]{10,}\b/<REDACTED>/g; ' "$OUT" 2>/dev/null
printf '\n\n===== FIN DU RAPPORT =====\n' >>"$OUT"
echo "Rapport ecrit dans : $OUT"
echo "Relis-le rapidement avant de l'envoyer, puis transmets-le."
