# Claude OS

Distribution Linux légère destinée à remplacer ChromeOS **en natif** sur un
**HP Chromebook x360 14b-cb0000sf**, et construite autour de **Claude Desktop**
comme environnement de travail principal, doté de privilèges étendus sur le système.

> **État : amorçage.** Le socle technique est arrêté et documenté ; la première
> étape opérationnelle (relevé matériel) est prête à être exécutée. Rien n'a
> encore été flashé ni installé sur la machine cible.

---

## Les trois contraintes qui structurent le projet

1. **Le matériel est verrouillé par défaut.** Un Chromebook ne démarre pas un
   Linux natif sans remplacer son firmware. Cette étape est la seule du projet
   qui comporte un risque matériel réel.
2. **Les ressources sont faibles.** Machine à base de Jasper Lake, mémoire et
   stockage limités. Chaque mégaoctet au repos est un mégaoctet retiré à Claude
   Desktop, qui est une application Electron.
3. **« Pleins pouvoirs » doit rester réversible.** Un agent capable de tout
   modifier sur le système n'est utilisable au quotidien que si chaque action
   privilégiée est tracée et annulable.

---

## Décisions actées (et pourquoi)

| Décision | Choix | Justification |
|---|---|---|
| Board name cible | **`MADOO`** | Identifiant réel du HP Chromebook x360 14b-cb0 dans la base MrChromebox. C'est lui, pas le nom commercial, qui détermine le firmware applicable. |
| Firmware | **UEFI Full ROM (MrChromebox)** | `MADOO` est présent dans la base sans le drapeau `noUEFI` : le firmware UEFI complet est disponible pour cette carte. Il permet un démarrage Linux standard, sans bidouille de bootloader. |
| Distribution de base | **Debian 13 « trixie »** | Imposé par la cible : Claude Desktop pour Linux est distribué en `.deb` pour Debian 12+ / Ubuntu 22.04+. Trixie apporte en plus le noyau **6.12 LTS**, nettement meilleur que le 6.1 de Bookworm pour l'audio SOF des Chromebooks Jasper Lake. Support jusqu'en 2030. |
| Session graphique v1 | **X11 léger** | Anthropic indique que le raccourci global *Quick Entry* de Claude Desktop exige X11 **ou** le portail Wayland `GlobalShortcuts` — que les compositeurs wlroots légers n'implémentent pas de façon fiable. X11 garantit la fonctionnalité complète dès la v1. Wayland sera réévalué en v2. |
| Système de fichiers | **btrfs + compression zstd** | Gain d'espace notable sur un eMMC de faible capacité, et surtout **snapshots instantanés** — le mécanisme qui rend les privilèges étendus de Claude réversibles. |
| Mémoire | **zram (zstd)** | Indispensable si la machine est en 4 Go, une fois Electron chargé. |
| Machine cible | **`MADOO`** — N6000, 4 Go | Board confirmé sur trois sources indépendantes. 4 Go de LPDDR4x **soudée** : plafond définitif, non extensible. |
| Levée du write-protect | **Cavalier `J1`** ✅ | Résolu. La déconnexion de batterie est sans effet sur MADOO (`wpsw_cur` = `1`), mais le pontage de `J1` — paire basse sous le lecteur microSD — donne `wpsw_cur` = `0`. Information neuve : ni MrChromebox ni le forum ne l'avaient confirmée. |
| Filet de récupération | **Sauvegarde USB seule** | Pas de programmateur SPI externe. La sauvegarde du firmware devient donc le seul recours, d'où un protocole de vérification strict. |

Le détail et les sources de chaque point sont dans [`docs/`](docs/).

---

## Documentation

| Document | Contenu |
|---|---|
| [`docs/01-materiel-firmware.md`](docs/01-materiel-firmware.md) | Le matériel, le déverrouillage du firmware, les points de non-retour et la procédure de sauvegarde. **À lire avant toute manipulation de la machine.** |
| [`docs/02-architecture.md`](docs/02-architecture.md) | Le socle logiciel, le budget mémoire, et le modèle de privilèges de Claude sur le système. |
| [`docs/03-write-protect-jumper.md`](docs/03-write-protect-jumper.md) | **Résolu.** Le cavalier de write-protect de MADOO est `J1`, confirmé par mesure (`wpsw_cur` = `0`). Méthode d'identification et protocole de pontage. |
| [`docs/04-environnement-bureau.md`](docs/04-environnement-bureau.md) | La pile graphique, le rendu visuel, ce qui est volontairement absent, et les points à valider sur la machine. |
| [`docs/05-energie.md`](docs/05-energie.md) | Économie d'énergie : ce qui compte vraiment, les réglages TLP et noyau, et ce qui est délibérément écarté. |

### Installation

| Fichier | Rôle |
|---|---|
| [`install/provision.sh`](install/provision.sh) | Transforme une Debian 13 minimale en Claude OS. Idempotent, `--dry-run` disponible. |
| [`install/packages.list`](install/packages.list) | Les 34 paquets, chacun justifié en commentaire. |
| [`rootfs/`](rootfs/) | Les fichiers déployés tels quels : configurations tint2, picom, openbox, session, fond d'écran. |

### Outils

| Outil | Rôle |
|---|---|
| [`tools/probe-hardware.sh`](tools/probe-hardware.sh) | Relevé matériel en lecture seule, 13 sections. À lancer depuis ChromeOS **avant** tout effacement. |
| [`tools/verify-firmware-backup.sh`](tools/verify-firmware-backup.sh) | Valide une sauvegarde de firmware avant de flasher : taille, dump vide, signature `__FMAP__`, régions, et comparaison de deux lectures. Retourne `2` si la sauvegarde est inutilisable. |
| [`tools/validate-install.sh`](tools/validate-install.sh) | Passe en revue l'installation poste par poste — Wi-Fi, Bluetooth, **audio**, VA-API, énergie, session, empreinte mémoire — et rend un verdict. À lancer après `provision.sh`. |
| [`tools/probe-keys.sh`](tools/probe-keys.sh) | Relève ce qu'émettent réellement la rangée supérieure et la touche Loupe du clavier Chromebook, pour en déduire les liaisons Openbox. |

---

## Étape 0 — Vérifier le board (2 minutes, sans rien casser)

« HP Chromebook x360 14b » recouvre **cinq plateformes matérielles
différentes** ; seul le suffixe de deux lettres les distingue. Flasher le
firmware d'un `14b-ca0` (Gemini Lake) sur un `14b-cb0` (Jasper Lake) est le
risque le plus bête du projet.

Ce contrôle **n'exige ni mode développeur ni effacement**. Depuis ChromeOS :

1. Ouvrir `chrome://version` → la ligne `Platform` doit se terminer par
   **`madoo`**.
2. Pour confirmation, ouvrir `chrome://system` → le champ `hwid` doit
   commencer par **`MADOO`**.

Si ce n'est pas le cas, s'arrêter et me le dire : toute la cible firmware
change. Détails et sources dans
[`docs/01`](docs/01-materiel-firmware.md#vérifier-sur-la-machine-sans-mode-développeur).

---

## Étape 1 — Relever le matériel

Le reste (noyau, pilotes, firmwares à embarquer) dépend de faits que seule la
machine peut donner. Le relevé doit être lancé **depuis ChromeOS, avant tout
effacement** : c'est le seul moment où le HWID, l'état du write-protect et la
version du CR50 sont lisibles.

Le dépôt étant **privé**, `curl` ne peut pas récupérer le script sans jeton.
La voie fiable passe par le navigateur, déjà authentifié sur GitHub :

1. Ouvrir [`tools/probe-hardware.sh`](tools/probe-hardware.sh) dans GitHub,
   cliquer sur **Raw**, enregistrer (Ctrl+S) sous le nom `probe.sh`.
2. Passer en mode développeur si ce n'est pas déjà fait
   (⚠️ **cela efface les données locales de la machine**).
3. Ouvrir `crosh` avec Ctrl+Alt+T, puis taper `shell` :

```sh
cd /home/chronos/user/MyFiles/Downloads
sudo bash probe.sh
```

Le script est en **lecture seule**. Il compare automatiquement le matériel
aux attentes du projet et rend un verdict :

```
[OK]     board = MADOO, conforme à l'attendu
         HWID complet : MADOO A6B-C7D-E8F
[OK]     CPU = Intel(R) Pentium(R) Silver N6000 @ 1.10GHz
[OK]     RAM = 4 Go
```

Puis il produit un rapport Markdown à me transmettre, qui répond aux
questions encore ouvertes : capacité exacte de l'eMMC, **chaîne audio**
(principal risque de non-fonctionnement sous Linux), contrôleur Wi-Fi et
firmware associé, état du write-protect et version du CR50.

## Étapes suivantes

Le firmware UEFI est flashé, le cavalier retiré, la machine remontée.

1. **Terminer l'installateur Debian 13.** Le choix décisif est l'écran de
   sélection des logiciels : **tout décocher sauf « Utilitaires usuels du
   système »**. « Environnement de bureau Debian » et « GNOME » y sont cochés
   par défaut — les laisser installerait plusieurs gigaoctets dont Claude OS
   n'a que faire.
2. **Récupérer le dépôt** sur la machine, puis lancer
   `sudo bash install/provision.sh`.
3. **Valider** avec `bash tools/validate-install.sh` — l'audio en premier.
4. **Relever les touches** avec `bash tools/probe-keys.sh`, pour en tirer les
   liaisons Openbox définitives.

Puis les reports : rclone pour Drive et OneDrive, réglage fin du tactile, et
les couleurs des boutons si une voie apparaît.
