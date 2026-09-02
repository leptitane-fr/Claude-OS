# Claude-OS

Distribution Linux sur-mesure basée sur **Debian 13 « Trixie »**, destinée à
une clé **USB-C 3.2 PNY 256 Go**, et taillée pour l'autonomie sur un ASUS
Vivobook K3605ZV (Alder Lake-H + RTX 4060, UEFI AMI K3605ZV.305).

Le système est volontairement réduit à son usage : **Chromium et Claude
Desktop**. Tout le reste n'est là que pour les faire tourner longtemps sur
batterie.

## Comment on l'installe

Aucun Linux n'est nécessaire. L'intégration continue produit une image
disque ; tu l'écris sur la clé depuis Windows avec balenaEtcher ou Rufus,
et tu démarres dessus.

→ **[docs/flasher-depuis-windows.md](docs/flasher-depuis-windows.md)**

Le premier démarrage étend tout seul la partition racine à la taille réelle
de la clé.

## Les trois contraintes fondatrices

### 1. Secure Boot reste activé

Aucune signature personnalisée, aucun MOK à enrôler : on s'appuie sur la
chaîne de confiance native de Debian.

```
Firmware AMI → shimx64.efi.signed  (signé Microsoft)
             → grubx64.efi.signed  (signé Debian)
             → vmlinuz             (signé Debian)
```

**Conséquence structurante :** aucun module noyau hors-arbre non signé.
Pas de DKMS, donc pas de pilote NVIDIA propriétaire — ce qui converge avec
l'objectif d'autonomie plutôt que de s'y opposer.

L'intégration continue **vérifie réellement cette chaîne** : elle démarre
l'image sous QEMU avec un firmware OVMF portant les clés Microsoft et
appliquant Secure Boot. Atteindre l'invite de connexion prouve que la
chaîne est valide de bout en bout ; un shim mal signé serait refusé par le
firmware et ferait échouer la construction.

### 2. Le démarrage du disque interne n'est jamais touché

La clé porte sa propre partition EFI, et GRUB s'installe sur le **chemin
amovible** `/EFI/BOOT/BOOTX64.EFI`, avec `--no-nvram` :

- aucune écriture sur la partition EFI du disque interne ;
- **aucune entrée créée dans la mémoire du firmware** ;
- ordre de démarrage inchangé, `bootmgfw.efi` intact.

Le chroot de construction n'a d'ailleurs **pas d'`efivarfs` monté** :
`grub-install` y est matériellement hors d'état d'écrire dans la NVRAM,
même s'il le voulait. `os-prober` est désactivé pour que rien n'aille lire
le disque Windows.

Le module `vmd` est blacklisté par défaut. Si le BIOS est en mode Intel
VMD/RST, le SSD Windows devient **invisible au noyau** — la garantie de
non-écriture la plus solide qu'on puisse obtenir. S'il ne l'est pas,
blacklister ce module ne change rien : le réglage est sûr dans les deux cas.
Le revers : depuis Claude-OS, tes fichiers Windows sont inaccessibles.

### 3. Le GPU NVIDIA n'est pas alimenté

Le point le plus délicat du projet, et le plus mal compris.

Blacklister `nouveau` et `nvidia` **ne suffit pas** : sans pilote pour
négocier sa mise en veille, la RTX 4060 reste en état `D0` et consomme
**10 à 20 W en continu** — le premier poste de consommation de la machine,
devant le rétroéclairage. Un blacklist seul *dégrade* donc l'autonomie par
rapport à une configuration où le pilote endort la carte.

L'objectif réel est le **D3cold** : alimentation coupée.
`claude-os-dgpu-power.service` interroge le matériel au démarrage et choisit
sa méthode, puis **dit ce qu'il a obtenu** :

| Méthode retenue | Condition détectée | Gain |
|---|---|---|
| `mux` | `asus-nb-wmi` expose `dgpu_disable` | total |
| `pr3` | le pont PCIe expose des power resources ACPI | ~15 W |
| `remove` | ni l'un ni l'autre — retrait du bus PCI | partiel |

```bash
journalctl -b -u claude-os-dgpu-power
```

## Contraintes propres au support flash

Une clé n'est pas un SSD : pas de TRIM fiable, wear-leveling sommaire,
endurance en écriture limitée. Chaque réglage sert deux objectifs à la fois
— moins d'écritures, et moins de réveils, donc moins de watts.

- journaux en RAM, `/tmp` et `/var/tmp` en tmpfs
- `zram` compressé à la place d'une partition d'échange
- `noatime`, journal ext4 commité toutes les 10 minutes
- **30 % du support laissé hors partition** : faute de TRIM, le contrôleur
  ne sait pas quels blocs sont libres et ne peut pas les recycler ; des
  secteurs jamais écrits, eux, lui restent disponibles pour son
  wear-leveling. C'est la seule réserve d'usure réellement garantie.

**Piège évité :** l'autosuspend USB de TLP peut endormir le contrôleur qui
porte la racine et **geler la machine**. La clé n'étant pas connue au moment
de fabriquer l'image, l'autosuspend part désactivé ; le premier démarrage
identifie la clé, l'exclut nommément, puis réactive l'autosuspend pour tous
les autres périphériques.

## Le dépôt

| | |
|---|---|
| `build/build.sh` | construit le système — image à flasher, ou clé directe |
| `build/packages.list` | les paquets, et pourquoi chacun est là |
| `build/config/default.conf` | tous les réglages |
| `overlay/` | la configuration appliquée au système cible |
| `tools/hw-probe.sh` | sonde matérielle en lecture seule |
| `.github/workflows/` | construction et test Secure Boot automatisés |
| `docs/` | flashage depuis Windows, utilisation, essai du shell, vérifications |

```bash
sudo ./build/build.sh --image claude-os.img   # image à flasher
sudo ./build/build.sh --device /dev/sdb       # écriture directe (hôte Linux)
```

Le mode `--device` applique cinq garde-fous indépendants avant d'écrire
(bus USB, absence de montage, non-hébergement du système courant, taille
plausible, rejet des `nvme`/`mmcblk`) et exige une ressaisie du chemin.
