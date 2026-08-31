# Claude-OS

Distribution Linux sur-mesure basée sur **Debian**, installée sur une clé
**USB-C 3.2 PNY 256 Go**, pensée pour l'autonomie sur un ASUS Vivobook
K3605ZV (Alder Lake-H + RTX 4060 Laptop, UEFI AMI K3605ZV.305).

> **État du projet : cadrage.** La sonde matérielle est le seul livrable
> pour l'instant ; l'architecture de build est arrêtée une fois son
> rapport analysé.

## Les trois contraintes fondatrices

### 1. Secure Boot reste activé

Aucune signature personnalisée, aucun MOK à enrôler. On s'appuie sur la
chaîne de confiance native de Debian :

```
Firmware AMI → shimx64.efi.signed  (signé Microsoft)
             → grubx64.efi.signed  (signé Debian)
             → vmlinuz             (signé Debian)
```

**Conséquence structurante :** aucun module noyau hors-arbre non signé.
Pas de DKMS, donc pas de pilote NVIDIA propriétaire — ce qui converge
avec l'objectif d'autonomie plutôt que de s'y opposer.

### 2. Le démarrage du disque interne n'est jamais touché

La clé porte sa propre ESP, et GRUB s'installe sur le **chemin amovible**
`/EFI/BOOT/BOOTX64.EFI`, avec `--no-nvram` :

- aucune écriture sur l'ESP du disque interne ;
- **aucune entrée créée dans la NVRAM** du firmware ;
- ordre de démarrage inchangé, `bootmgfw.efi` intact.

Le démarrage se fait par le menu du firmware (Échap / F8). Clé
débranchée, la machine est strictement dans son état d'origine.

C'est volontairement plus strict qu'une installation Debian standard,
qui elle écrit dans la NVRAM via `efibootmgr`.

**Piste de renforcement** (à confirmer par la sonde) : si le BIOS est en
mode **Intel VMD/RST**, blacklister le module `vmd` rend le SSD Windows
*matériellement invisible* au noyau. Une garantie bien plus forte qu'une
simple discipline de montage.

### 3. Le GPU NVIDIA ne doit pas être alimenté

Le point le plus délicat du projet, et le plus mal compris.

Blacklister `nouveau` et `nvidia` **ne suffit pas** : sans pilote pour
négocier sa mise en veille, la RTX 4060 reste en état `D0` et consomme
**10 à 20 W en continu**. C'est le premier poste de consommation de la
machine, devant le rétroéclairage. Un simple blacklist *dégrade*
l'autonomie par rapport à une configuration où le pilote endort la carte.

Objectif réel : amener le GPU en **D3cold**, par ordre de préférence.

| # | Méthode | Condition | Gain |
|---|---------|-----------|------|
| 1 | MUX / `dgpu_disable` firmware | interface `asus-nb-wmi` exposée | total |
| 2 | Runtime D3cold via `_PR3` ACPI | power resources sur le pont PCIe | ~15 W |
| 3 | `pcie_port_pm=force` + runtime PM | ASPM stable sur la plateforme | partiel |
| 4 | Blacklist seul | — | **négatif** |

La sonde tranche entre ces méthodes. `bbswitch` est écarté d'emblée :
cassé depuis Turing, et de toute façon incompatible Secure Boot.

## Contraintes propres au support USB

Une clé flash n'est pas un SSD : pas de TRIM fiable, wear-leveling
sommaire, endurance en écriture limitée. La discipline d'écriture rejoint
l'objectif d'autonomie — moins d'E/S, moins de réveils, moins de watts.

- `noatime`, intervalle de commit allongé
- `zram` plutôt qu'une partition d'échange
- journald borné (volatile ou plafonné)
- `/tmp` et `/var/tmp` en tmpfs

**Piège à ne pas manquer :** l'autosuspend USB de TLP peut endormir le
contrôleur qui porte la racine du système et **geler la machine**. La clé
doit être explicitement exclue via son ID `vendor:product` — d'où la
section 6 de la sonde.

## Sonde matérielle

```bash
sudo ./tools/hw-probe.sh
```

Strictement en lecture seule : ne monte rien, n'écrit que son rapport,
masque les numéros de série. Produit `claude-os-hw-report.txt`.

Elle répond aux questions qui conditionnent l'architecture : présence
d'un MUX, disponibilité de `_PR3`, mode VMD/RST, ID de la clé PNY,
firmwares non-free requis, états de veille supportés, et une mesure de
consommation de référence.
