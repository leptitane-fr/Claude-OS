# Construire et vérifier Claude-OS

Pour simplement installer le système sur la clé, voir
**[flasher-depuis-windows.md](flasher-depuis-windows.md)**. Ce document
s'adresse à la construction et au diagnostic.

## Construire

### Par l'intégration continue (la voie normale)

Onglet **Actions** → *Construire l'image Claude-OS* → *Run workflow*.
Cocher *Publier* pour créer une release, sinon l'image reste disponible
comme artefact pendant 14 jours.

Le workflow construit l'image, **la démarre sous QEMU avec un firmware
OVMF appliquant Secure Boot**, puis la compresse. Si la chaîne de
démarrage est invalide, la construction échoue au lieu de publier une
image qui ne démarrerait pas.

### En local, sur un hôte Linux

```bash
sudo apt install debootstrap gdisk dosfstools e2fsprogs mtools
sudo ./build/build.sh --image claude-os.img     # image à flasher
sudo ./build/build.sh --device /dev/sdb         # écriture directe sur clé
```

Compter 20 à 40 minutes. Tout est journalisé dans `build.log` ; suivre
depuis un autre terminal avec `tail -f build.log`.

Pour personnaliser : `cp build/config/default.conf build/config/local.conf`
et éditer. `local.conf` est lu après `default.conf` et n'est pas versionné.

Le mode `--device` refuse d'écrire si la cible n'est pas sur le bus USB, si
elle porte une partition montée, si elle héberge le système en cours, ou si
sa taille est incohérente ; un `/dev/nvme*` est rejeté sans discussion, et
il faut retaper le chemin exact.

## Vérifier après le premier démarrage

```bash
mokutil --sb-state                        # attendu : SecureBoot enabled
df -h /                                   # racine étendue (~170 Gio)
journalctl -b -u claude-os-firstboot      # extension + exclusion USB
journalctl -b -u claude-os-dgpu-power     # état du GPU NVIDIA
vainfo                                    # accélération vidéo Intel
sudo tlp-stat -s -b                       # politique d'énergie
sudo powertop                             # consommation réelle
```

### Le GPU : la vérification qui compte

C'est de loin le premier levier d'autonomie. Le service choisit sa méthode
seul, puis annonce le résultat obtenu :

| État final | Signification | Suite à donner |
|---|---|---|
| `D3cold` | Alimentation coupée. Objectif atteint. | rien |
| `D3hot` | Carte suspendue mais toujours alimentée. | forcer `DGPU_STRATEGY="remove"` dans `/etc/default/claude-os-dgpu`, puis redémarrer |
| `D0` | Carte à pleine consommation. | vérifier que `pcie_port_pm=force` figure bien dans `/proc/cmdline` |

L'écart attendu entre `D0` et `D3cold` est de l'ordre de 15 W — un facteur
deux sur l'autonomie totale. Mesurer au repos avec `powertop` pour comparer.

## La sonde matérielle

`tools/hw-probe.sh` n'est plus nécessaire à la construction : l'image
s'adapte seule au matériel. Elle reste utile pour comprendre *pourquoi* le
GPU ne descend pas en `D3cold`.

```bash
sudo apt install acpica-tools
sudo ./tools/hw-probe.sh
```

Strictement en lecture seule : ne monte rien, n'écrit que son rapport,
masque les numéros de série. La section 4 détaille l'état du dGPU, la
présence d'un MUX `asus-nb-wmi` et celle de `_PR3` dans la DSDT.

## Ce qui n'est jamais touché

- La partition EFI du disque interne : aucune écriture.
- La mémoire du firmware : aucune entrée créée. `grub-install` reçoit
  `--no-nvram`, et le chroot de construction n'a pas d'`efivarfs` monté —
  il en est donc matériellement incapable.
- L'ordre de démarrage : inchangé. Clé retirée, la machine démarre sur
  Windows exactement comme avant.

## Points restant à valider sur le matériel réel

Écrits d'après la documentation, vérifiés en CI pour ce qui peut l'être,
mais pas encore éprouvés sur le Vivobook :

- **La méthode retenue pour le GPU.** Le choix automatique est testé en
  logique, pas contre une RTX 4060 réelle. C'est le premier point à
  regarder après le premier démarrage.
- **`/etc/chromium.d/`.** Le lanceur Chromium de Debian lit ce répertoire ;
  confirmer via `chrome://gpu` que le décodage vidéo est bien matériel.
- **Le trousseau sous Xfce.** `gnome-keyring` doit être démarré par la
  session pour que Claude Desktop conserve sa connexion. Si l'application
  redemande une authentification à chaque démarrage, c'est le point à
  regarder en premier.
- **`i915.enable_psr=1`.** Gain réel sur batterie, mais peut provoquer un
  scintillement sur certaines dalles. Passer à `0` le cas échéant dans
  `/etc/default/grub`, puis `sudo update-grub`.
- **L'énumération USB au démarrage.** L'initramfs conserve `MODULES=most`
  pour que la clé démarre sur un contrôleur quelconque, mais un délai
  d'énumération trop long resterait à corriger par `rootdelay=`.

## Ce qui a été vérifié, et comment

Résultats obtenus sur une image construite de zéro, démarrée sous QEMU avec
un firmware OVMF portant les clés Microsoft et **appliquant réellement**
Secure Boot. Ce n'est pas une simulation : c'est la même vérification
cryptographique que celle du firmware du portable.

| Point vérifié | Méthode | Résultat |
|---|---|---|
| Chaîne shim → GRUB → noyau | démarrage sous OVMF Secure Boot | invite de connexion atteinte en 80–90 s, aucune violation |
| Démarrage depuis un périphérique USB | clé présentée en `usb-storage` à QEMU | `BdsDxe: starting … USB HARDDRIVE` |
| Racine désignée par UUID | inspection de `grub.cfg` + démarrage | `root=UUID=…`, contrôle bloquant dans le build |
| Vérification du système de fichiers | sortie série | `claudeos-root: clean, 48982/688128 files` |
| Extension de la partition | image de 11 Gio écrite sur un support de 40 Gio | racine portée de 10,5 à 27,5 Gio (70 %) |
| Réparation du GPT | `sgdisk -v` avant / après | « 1 problème » → « No problems found » |
| Réserve d'usure préservée | `sgdisk -v` après extension | 12 Gio laissés hors partition sur 40 |
| Outils présents | auto-contrôle en fin de build | `fsck.ext4`, `resize2fs`, `sgdisk`, `chromium`, `tlp` |
| Services activés | auto-contrôle en fin de build | dGPU, premier démarrage, TLP, NetworkManager, lightdm |
| Taille de l'image | `xz -9` après `fstrim` | **836 Mio** compressés (limite GitHub : 2048) |

Transposé à une clé de 256 Go : racine d'environ 170 Go, et près de 76 Go
laissés hors partition comme réserve de wear-leveling.

### Ce que ces tests ne disent pas

Aucun d'eux ne s'est exécuté sur le Vivobook. En particulier, **rien ici ne
prédit le comportement de la RTX 4060** : QEMU n'a pas de GPU discret, donc
`claude-os-dgpu-power` n'a rien eu à éteindre. C'est la première chose à
vérifier après le premier démarrage réel, et la seule qui décide de
l'autonomie.

### Une limite du test QEMU, découverte sur matériel réel

Le premier démarrage sur le Vivobook s'est arrêté sur l'interpréteur GRUB
(« Minimal BASH-like line editing is supported »), alors que la même image
atteignait l'invite de connexion sous QEMU.

Le `grubx64.efi` signé par Debian embarque un préfixe codé en dur,
`/EFI/debian`, et y cherche sa configuration. En installation amovible,
`grub-install` ne la dépose que dans `/EFI/BOOT` : GRUB ne la trouve alors
qu'en retombant sur `$cmdpath`, le répertoire d'où il a été chargé. Ce
repli fonctionne sous OVMF, mais pas sur le firmware AMI de l'ASUS, qui
n'expose pas le chemin de périphérique de la même manière.

La configuration est désormais déposée **aux deux emplacements**, et le
build vérifie que le préfixe réellement embarqué dans le binaire est servi.

La leçon dépasse ce cas : **un démarrage réussi sous QEMU ne prouve pas un
démarrage réussi sur une machine donnée.** OVMF valide la chaîne
cryptographique Secure Boot — ce qui reste précieux — mais pas les
particularités d'un firmware constructeur.

En cas de rechute, on démarre manuellement depuis l'invite `grub>` :

```
search --no-floppy --file --set=root /boot/grub/grub.cfg
configfile ($root)/boot/grub/grub.cfg
```
