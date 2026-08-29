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

Le détail et les sources de chaque point sont dans [`docs/`](docs/).

---

## Documentation

| Document | Contenu |
|---|---|
| [`docs/01-materiel-firmware.md`](docs/01-materiel-firmware.md) | Le matériel, le déverrouillage du firmware, les points de non-retour et la procédure de sauvegarde. **À lire avant toute manipulation de la machine.** |
| [`docs/02-architecture.md`](docs/02-architecture.md) | Le socle logiciel, le budget mémoire, et le modèle de privilèges de Claude sur le système. |

---

## Étape 1 — Relever le matériel (à faire maintenant)

Toute la suite (choix du noyau, pilotes, firmwares à embarquer) dépend de faits
que seule la machine cible peut donner. Le script de relevé doit être lancé
**depuis ChromeOS, avant tout effacement** : c'est le seul moment où le HWID,
l'état du write-protect et la version du CR50 sont lisibles.

Le dépôt étant **privé**, `curl` ne peut pas récupérer le script sans jeton.
La voie fiable passe par le navigateur, déjà authentifié sur GitHub :

1. Sur le Chromebook, ouvrir le fichier [`tools/probe-hardware.sh`](tools/probe-hardware.sh)
   dans GitHub, cliquer sur **Raw**, puis enregistrer la page
   (Ctrl+S) sous le nom `probe.sh`.
2. Passer en mode développeur si ce n'est pas déjà fait
   (⚠️ **cela efface les données locales de la machine**).
3. Ouvrir `crosh` avec Ctrl+Alt+T, puis taper `shell` :

```sh
cd /home/chronos/user/MyFiles/Downloads
sudo bash probe.sh
```

Le script est en **lecture seule** : il ne modifie rien. Il produit un rapport
Markdown (`hardware-report-chromeos-<date>.md`) à me transmettre.

Il répond notamment à :

- le board name est-il bien `MADOO` ? (sinon la cible firmware change) ;
- 4 Go ou 8 Go de RAM ? (détermine l'agressivité du budget mémoire) ;
- quelle chaîne audio exacte ? (**principal risque de non-fonctionnement sous Linux**) ;
- quel contrôleur Wi-Fi, donc quel firmware embarquer pour avoir du réseau
  dès la première installation.

## Étapes suivantes

2. Analyse du relevé et gel des spécifications réelles.
3. Sauvegarde du firmware d'origine, déverrouillage du write-protect, flash UEFI.
4. Validation matérielle sous live USB Debian 13 (audio, Wi-Fi, tactile, veille).
5. Construction de l'image Claude OS reproductible.
6. Intégration de Claude Desktop et du courtier de privilèges.
