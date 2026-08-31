# Construire et utiliser Claude-OS

## 1. Sonder la machine (à faire en premier)

Depuis ton live Mint, sur le Vivobook :

```bash
sudo apt install acpica-tools          # pour l'analyse ACPI du dGPU
sudo ./tools/hw-probe.sh
```

Le rapport `claude-os-hw-report.txt` détermine trois réglages de
`build/config/local.conf` qu'on ne peut pas deviner à l'avance :

| Ce que dit le rapport | Réglage à poser |
|---|---|
| `asus-nb-wmi` expose `dgpu_disable` | `DGPU_STRATEGY="mux"` |
| Le pont PCIe a des `power_resource_*` / `_PR3` présent | `DGPU_STRATEGY="pr3"` |
| Ni l'un ni l'autre | `DGPU_STRATEGY="remove"` |
| `lspci` montre « Volume Management Device » | `BLACKLIST_VMD="yes"` |

## 2. Construire

```bash
cp build/config/default.conf build/config/local.conf
$EDITOR build/config/local.conf        # au minimum : TARGET_DEVICE, USERNAME
sudo ./build/build.sh
```

Compter 20 à 40 minutes selon le débit réseau. Tout est journalisé dans
`build.log` ; suivre l'avancement depuis un autre terminal avec
`tail -f build.log`.

Le script **refuse d'écrire** si la cible n'est pas sur le bus USB, si
elle porte une partition montée, si elle héberge le système en cours, ou
si sa taille est incohérente. Il exige en plus que tu retapes le chemin
exact. Un `/dev/nvme*` est rejeté sans discussion.

Relancer le script reconstruit la clé de zéro : on itère sur la recette,
jamais sur le résultat.

## 3. Démarrer

Redémarrer, maintenir **Échap** (ou **F8** selon le firmware), choisir la
clé dans le menu. **Ne toucher à aucun réglage du BIOS** — ni Secure
Boot, ni le mode VMD/RST, ni l'ordre de démarrage.

Si la clé n'apparaît pas dans le menu, la cause la plus fréquente est
*Fast Boot*, qui abrège l'énumération USB. C'est le seul réglage BIOS
qu'il puisse être nécessaire de changer.

## 4. Vérifier après le premier démarrage

```bash
mokutil --sb-state                        # attendu : SecureBoot enabled
journalctl -b -u claude-os-dgpu-power     # état final du dGPU
vainfo                                    # accélération vidéo Intel
sudo tlp-stat -s -b                       # politique d'énergie active
sudo powertop                             # consommation réelle
```

### Lire le résultat sur le GPU

C'est la vérification qui compte le plus pour l'autonomie.

| Message du journal | Signification | Suite à donner |
|---|---|---|
| `D3cold` | Alimentation coupée. Objectif atteint. | rien |
| `D3hot` | Carte suspendue mais toujours alimentée. | passer à `DGPU_STRATEGY="remove"` |
| `D0` | Carte à pleine consommation. | vérifier `pcie_port_pm=force` dans `/proc/cmdline` |

Comparer la consommation au repos avant/après via `powertop` : l'écart
attendu entre `D0` et `D3cold` est de l'ordre de 15 W, soit un facteur
deux sur l'autonomie totale.

## 5. Ce qui n'a pas été touché

- L'ESP du disque interne : aucune écriture.
- La NVRAM du firmware : aucune entrée créée (`--no-nvram`, et l'`efivarfs`
  n'est délibérément pas monté dans le chroot pendant le build).
- L'ordre de démarrage : inchangé. Clé retirée, la machine démarre sur
  Windows exactement comme avant.

## Points restant à valider sur le matériel

Ces éléments sont écrits selon la documentation mais n'ont pas encore été
vérifiés sur la machine réelle :

- **La chaîne Secure Boot.** `build.sh` compare l'empreinte SHA-256 de
  `EFI/BOOT/BOOTX64.EFI` avec celle du shim signé Debian et avertit en cas
  d'écart. À confirmer par `mokutil --sb-state` après démarrage.
- **`/etc/chromium.d/`.** Le lanceur Chromium de Debian lit ce répertoire ;
  vérifier avec `chrome://gpu` que le décodage vidéo est bien matériel.
- **Le trousseau sous Xfce.** `gnome-keyring` doit être démarré par la
  session pour que Claude Desktop conserve sa connexion. Si l'application
  redemande une authentification à chaque démarrage, c'est le point à
  regarder en premier.
- **`i915.enable_psr=1`.** Gain réel sur batterie, mais peut provoquer un
  scintillement sur certaines dalles. Passer à `0` le cas échéant.
