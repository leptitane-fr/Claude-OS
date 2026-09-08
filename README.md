# Claude OS

Distribution Linux légère destinée à remplacer ChromeOS **en natif** sur un
**HP Chromebook x360 14b-cb0000sf**, et construite autour de **Claude Desktop**
comme environnement de travail principal, doté de privilèges étendus sur le système.

> **État : en service.** Le firmware est flashé, Debian 13 installée, et le
> bureau tourne sur la machine — `greetd` ouvre l'écran de connexion Claude OS,
> le mot de passe est accepté, la session labwc s'ouvre avec le dock, la
> Console et le lanceur. Les fenêtres portent des boutons réduire/agrandir, et
> le thème sort du shell pour atteindre Chromium, Claude Desktop, le terminal
> et les barres de titre.
>
> Quatre pannes de session graphique ont été traversées et résolues les 7 et
> 8 septembre 2026 ; leur post-mortem est dans
> [`docs/06`](docs/06-journal-incident-wayland.md), le fil chronologique du
> projet dans [`docs/07`](docs/07-journal-des-seances.md), et les invariants
> qui en découlent dans [`CLAUDE.md`](CLAUDE.md) — **à lire avant toute
> intervention**.
>
> Restent ouverts : l'**audio**, qui échoue au chargement de la topologie DSP,
> l'affichage qui n'apparaît qu'au premier contact du pavé tactile, et les
> touches de la rangée supérieure à câbler.

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
| Session graphique | **Wayland — labwc** | X11 avait été retenu pour *Quick Entry* de Claude Desktop. La pile X11 a été construite, installée, et a échoué à l'usage : dock insensible au clic, tactile non fonctionnel, barre d'état absente. Le choix a été retourné. Coût assumé : Quick Entry n'est pas disponible. Voir `docs/02` §2.4. |
| Interface | **Shell sur mesure, C + GTK4** | Six petits programmes — dock, barre d'état, lanceur, gestionnaire de fichiers, réglages, fond d'écran — pèsent moins que les six composants existants qu'ils remplacent, partagent une feuille de style et un fichier de configuration, et font exactement ce qu'on leur demande. |
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
| [`CLAUDE.md`](CLAUDE.md) | **À lire en premier.** Où en est le projet, les sept invariants qu'on ne casse jamais, la procédure d'intervention sur la session graphique, et où lire quoi quand ça ne marche pas. Chargé automatiquement par Claude Code. |
| [`docs/01-materiel-firmware.md`](docs/01-materiel-firmware.md) | Le matériel, le déverrouillage du firmware, les points de non-retour et la procédure de sauvegarde. **À lire avant toute manipulation de la machine.** |
| [`docs/02-architecture.md`](docs/02-architecture.md) | Le socle logiciel, le budget mémoire, et le modèle de privilèges de Claude sur le système. |
| [`docs/03-write-protect-jumper.md`](docs/03-write-protect-jumper.md) | **Résolu.** Le cavalier de write-protect de MADOO est `J1`, confirmé par mesure (`wpsw_cur` = `0`). Méthode d'identification et protocole de pontage. |
| [`docs/04-environnement-bureau.md`](docs/04-environnement-bureau.md) | La pile graphique, le rendu visuel, ce qui est volontairement absent, et les points à valider sur la machine. |
| [`docs/05-energie.md`](docs/05-energie.md) | Économie d'énergie : ce qui compte vraiment, les réglages TLP et noyau, et ce qui est délibérément écarté. |
| [`docs/06-journal-incident-wayland.md`](docs/06-journal-incident-wayland.md) | **Résolu.** Post-mortem des pannes de session : la purge qui désinstallait le compositeur, `/tmp/.X11-unix` possédé par `_greetd`, le terminal virtuel disputé, et le correctif qui n'était pas sur la machine. Ce qui a fait perdre du temps, et ce qui n'est pas établi. |
| [`docs/07-journal-des-seances.md`](docs/07-journal-des-seances.md) | **Le fil du projet.** Ce qui a été fait séance par séance, ce qui a été mesuré, ce qui reste à faire — avec la marche à suivre proposée pour l'audio. |

### Installation

| Fichier | Rôle |
|---|---|
| [`install/provision.sh`](install/provision.sh) | Transforme une Debian 13 minimale en Claude OS. Idempotent, `--dry-run` disponible. |
| [`install/packages.list`](install/packages.list) | Les paquets, chacun justifié en commentaire. |
| [`install/bascule-session.sh`](install/bascule-session.sh) | Met l'écran de connexion en service **par étapes** : `--verifier`, `--deployer`, `--compiler`, `--essai` sur un terminal virtuel libre, `--basculer`, `--revenir`. Seul `--basculer` change le gestionnaire de session. |
| [`shell/`](shell/) | Le code du bureau : dock, barre d'état, lanceur, gestionnaire de fichiers, réglages, fond d'écran. Compilé sur la machine par `provision.sh`. |
| [`rootfs/`](rootfs/) | Les fichiers déployés tels quels : configuration de labwc, session Wayland, lanceur Claude, fond d'écran. |

### Outils

| Outil | Rôle |
|---|---|
| [`tools/probe-hardware.sh`](tools/probe-hardware.sh) | Relevé matériel en lecture seule, 13 sections. À lancer depuis ChromeOS **avant** tout effacement. |
| [`tools/verify-firmware-backup.sh`](tools/verify-firmware-backup.sh) | Valide une sauvegarde de firmware avant de flasher : taille, dump vide, signature `__FMAP__`, régions, et comparaison de deux lectures. Retourne `2` si la sauvegarde est inutilisable. |
| [`tools/validate-install.sh`](tools/validate-install.sh) | Passe en revue l'installation poste par poste — Wi-Fi, Bluetooth, **audio**, VA-API, énergie, session, empreinte mémoire — et rend un verdict. À lancer après `provision.sh`. |
| [`tools/diag-connexion.sh`](tools/diag-connexion.sh) | **Quand il n'y a pas d'écran de connexion** — console texte, écran noir, ou repli du filet. Lit d'abord ce que le filet a collecté avant de redémarrer : c'est la seule trace du démarrage qui a échoué. |
| [`tools/diag-session.sh`](tools/diag-session.sh) | Le pendant du précédent, quand la session graphique est ouverte mais que le bureau se comporte mal. |
| [`tools/probe-keys.sh`](tools/probe-keys.sh) | Relève, sous Wayland, ce qu'émettent réellement la rangée supérieure et la touche Loupe du clavier Chromebook, pour en déduire les liaisons labwc. |

---

## Intervenir sur la session graphique

Le bureau est en service. Toute intervention qui touche à la session ou à
l'écran de connexion se fait **par étapes**, jamais d'un bloc — c'est cette
forme, autant que le contenu, qui a produit les pannes de septembre.

```sh
cd ~/Claude-OS && git pull
sudo bash install/bascule-session.sh --verifier   # ne change rien
sudo bash install/bascule-session.sh --deployer   # recopie rootfs/ vers /
sudo bash install/bascule-session.sh --compiler   # recompile shell/
sudo bash install/bascule-session.sh --essai      # ← REGARDER L'ÉCRAN
sudo bash install/bascule-session.sh --basculer   # arme le filet, puis bascule
sudo systemctl reboot
```

**`git pull` ne déploie rien, et `--deployer` ne compile rien.** `rootfs/`
n'est recopié vers `/` que par `--deployer` ou `provision.sh` ; `shell/`
n'arrive sur la machine que par une compilation, donc par `--compiler` ou
`provision.sh`. Quatre séances de diagnostic ont porté sur des correctifs
présents dans le dépôt et absents de la machine. `--verifier` et `--deployer`
comparent désormais la date des sources à celle du binaire installé, et
refusent de se dire satisfaits quand le dépôt est en avance.

`--essai` affiche le véritable écran de connexion sur un terminal virtuel
libre pendant trente secondes, puis rend l'affichage — sans rien activer,
sans rien purger, sans toucher à la session en cours. En cas d'échec il verse
son autopsie dans `/var/log/claude-os-essai-<date>.txt`.

Au redémarrage, le **filet de sécurité** vérifie quatre minutes après le
démarrage qu'on peut bien entrer. Sinon il écrit pourquoi dans
`/var/log/claude-os-echec-<date>.txt` et repart sur un écran où l'on se
connecte — sans seconde machine. Il se désarme seul une fois la session
éprouvée.

Les invariants à ne jamais enfreindre sont dans [`CLAUDE.md`](CLAUDE.md).

### Mettre à jour le bureau

```sh
cd ~/Claude-OS && git pull
sudo bash install/provision.sh
```

`provision.sh` recompile le shell et le réinstalle. Il **ne touche pas** à
`~/.config/claude-os/shell.conf` : l'ordre des icônes, le thème et les
applications épinglées sont à vous.

## Ce qui reste à faire

Le détail, avec la marche à suivre proposée pour chaque point, est dans
[`docs/07`](docs/07-journal-des-seances.md).

**À confirmer à l'écran** — écrit et éprouvé au banc d'essai, jamais vu sur
MADOO : le **thème global** (toutes les fenêtres suivent la bascule
clair/sombre sans être relancées) et la **luminosité par logind** (le curseur
commande l'écran sans rouvrir de session). Les deux demandent `--deployer`,
`--compiler`, puis une réouverture de session.

**Chantiers ouverts :**

1. **L'audio.** En échec : le DSP démarre mais la topologie ne se charge pas
   (`sof_rt5682 … probe failed -22`). C'est le risque n°1 identifié dès
   `docs/01`, et le premier chantier. `bash tools/validate-install.sh` en
   rend compte.
2. **L'affichage au démarrage**, qui n'apparaît qu'au premier contact du
   pavé tactile. Peut-être réglé par le passage de greetd au tty7 —
   **à reconfirmer**, et à ne pas déclarer résolu sans l'avoir revu.
3. **Les touches de la rangée supérieure**, à relever avec
   `bash tools/probe-keys.sh` puis à câbler dans `rc.xml`.
4. **Les reports** : rclone pour Drive et OneDrive, les notifications, les
   icônes sur le bureau.

---

## Annexe — si l'on repart de zéro

Ces deux étapes sont **faites** sur la machine actuelle : le board est
confirmé `MADOO`, le firmware UEFI est flashé et le matériel relevé. Elles
sont conservées pour qui referait l'opération sur une seconde machine.

### Vérifier le board (2 minutes, sans rien casser)

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

### Relever le matériel

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
