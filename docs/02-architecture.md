# 2. Architecture logicielle

## 2.1 Base : Debian 13 « trixie »

Ce choix n'est pas une préférence, il est **imposé par l'exigence n°2 du projet**
(intégrer Claude Desktop) :

Claude Desktop pour Linux est officiellement distribué depuis le 30 juin 2026,
**en bêta**, sous forme de paquet `.deb` via un dépôt apt Anthropic, pour
**Debian 12+ et Ubuntu 22.04+**, en `x86_64` et `arm64`. Fedora, RHEL et les
distributions non-Debian ne sont pas encore prises en charge.

Partir d'une base Arch, Alpine ou Void obligerait à repackager l'application à
chaque mise à jour, pour un gain de légèreté marginal face à une application
Electron. **Une base Debian minimale est déjà très légère** : le poids vient de
l'environnement de bureau, pas de la distribution.

Trixie plutôt que Bookworm, pour deux raisons concrètes :

- **noyau 6.12 LTS** au lieu de 6.1 — écart déterminant pour la prise en charge
  audio SOF des Chromebooks Jasper Lake (cf. `docs/01`, risque n°1) ;
- support assuré jusqu'en **2030**.

Construction depuis `debootstrap --variant=minbase`, sans `task-desktop` : on
ajoute, on ne retire pas.

---

## 2.2 Budget mémoire

L'hypothèse dimensionnante est **4 Go** (à confirmer par le relevé ; si la
machine est en 8 Go, ce budget devient confortable au lieu d'être serré).

| Poste | Cible | Remarque |
|---|---|---|
| Noyau + firmware + `tmpfs` | ~250 Mo | |
| Session graphique complète (serveur X, WM, barre, portails) | **< 250 Mo** | Ce plafond exclut GNOME et KDE. |
| Services système | < 100 Mo | Pas d'indexeur de fichiers, pas de télémétrie, pas de gestionnaire de paquets résident. |
| **Disponible pour Claude Desktop + navigateur** | **~3 Go** | |

Mesures de compensation :

- **zram** avec compression `zstd`, dimensionné à ~50 % de la RAM. Sur un eMMC
  lent, comprimer en mémoire est nettement préférable à un swap sur disque.
- Pas de swap sur eMMC en usage normal : lent, et usure inutile de la mémoire flash.
- `systemd-oomd` réglé pour préserver la session en cas de pression mémoire.

---

## 2.3 Session graphique : X11 en v1

**Contre-intuitif, et assumé.** Wayland serait le choix moderne, mais la
documentation d'Anthropic est explicite : le raccourci global *Quick Entry* de
Claude Desktop exige **X11**, ou bien la prise en charge du portail Wayland
`GlobalShortcuts`. Or ce portail n'est pas implémenté de façon fiable par les
compositeurs wlroots légers (ceux qui, précisément, tiendraient dans le budget
mémoire ci-dessus) : il l'est surtout par GNOME et KDE, hors budget.

Choisir Wayland en v1 reviendrait donc à sacrifier une fonctionnalité de
l'application qui est la raison d'être du système, pour un gain théorique.

X11 apporte en prime, sur ce matériel précis : une pile Electron mieux éprouvée,
et un outillage mature pour la rotation d'écran et l'étalonnage du tactile sur
un convertible.

**Wayland est un objectif de v2**, à rouvrir dès qu'un compositeur léger
implémente `GlobalShortcuts` correctement.

Composants visés — gestionnaire de fenêtres léger, barre d'état minimale,
`greetd` comme gestionnaire de session, `xdg-desktop-portal-gtk` pour les
dialogues de fichiers dont Electron a besoin. Le détail sera arrêté après
validation matérielle.

---

## 2.4 Stockage

Disposition sur eMMC (capacité à confirmer, 64 Go supposés) :

| Partition | Taille | Format | Rôle |
|---|---|---|---|
| ESP | 512 Mo | FAT32 | Démarrage UEFI |
| `/` | reste | **btrfs**, `compress=zstd:1` | Sous-volumes `@`, `@home`, `@snapshots`, `@var-log` |

btrfs n'est pas un choix esthétique, il porte deux fonctions du projet :

1. **La compression `zstd:1`** récupère de l'espace sur un disque de faible
   capacité, et accélère même les lectures sur un eMMC lent (moins d'octets à lire).
2. **Les snapshots instantanés** sont le mécanisme qui rend réversibles les
   privilèges étendus de Claude — voir ci-dessous. Sans eux, « pleins pouvoirs »
   signifie « pleins pouvoirs de casser sans retour ».

Options de montage : `noatime`, `ssd`, `discard=async`.

---

## 2.5 Claude Desktop et le modèle de privilèges

### Ce que l'application fournit

Installation depuis le dépôt apt officiel d'Anthropic (et non par téléchargement
manuel du `.deb`, qui ne reçoit pas les mises à jour automatiques). Chat, Claude
Code et Claude Cowork sont disponibles dans une seule fenêtre.

**Deux limites à connaître, propres à la bêta Linux :** *Computer Use* n'est pas
encore disponible, et la dictée non plus.

### Conséquence directe sur l'exigence « pleins pouvoirs »

Puisque *Computer Use* n'est pas disponible sous Linux, l'accès de Claude au
système **ne passera pas par le contrôle de l'écran**, mais par une voie
programmatique — ce qui est de toute façon la bonne approche : plus rapide, plus
fiable, traçable, et scriptable.

### Architecture proposée

```
Claude Desktop  (session utilisateur, non privilégié)
      │  stdio
      ▼
claude-os-mcp   (serveur MCP, non privilégié)
      │  socket Unix  /run/claude-os/broker.sock
      ▼
claude-osd      (courtier, root, service systemd)
      │
      ├── vérifie la politique   /etc/claude-os/policy.d/*.toml
      ├── crée un snapshot btrfs avant toute écriture
      ├── journalise l'appel (journald + journal append-only)
      └── exécute
```

Le courtier est le seul composant privilégié, et il est petit : c'est lui, et
non l'agent, qui décide. Claude obtient un pouvoir complet sur la machine, mais
**par une porte instrumentée**.

### Trois niveaux d'autorisation

| Niveau | Exemples | Comportement |
|---|---|---|
| **Lecture** | état des services, journaux, inventaire des paquets, configuration | Direct, sans confirmation. |
| **Écriture réversible** | installer un paquet, modifier une configuration, activer un service | **Snapshot automatique**, exécution, journalisation. Annulable par `claude-os rollback`. |
| **Irréversible ou sensible** | partitionner, flasher, toucher aux secrets, aux identifiants ou au réseau distant | **Confirmation humaine explicite** obligatoire. |

Le troisième niveau n'est pas une bride sur les capacités de l'agent : c'est ce
qui permet d'accorder les deux premiers **sans réserve**. Une action annulable
peut être automatique ; une action définitive mérite une seconde paire d'yeux.

### Réversibilité

- Snapshot btrfs avant chaque opération du niveau 2, horodaté et corrélé à
  l'entrée de journal correspondante.
- `claude-os rollback <id>` restaure l'état antérieur.
- Rotation automatique des snapshots pour ne pas saturer l'eMMC.
- Journal d'audit lisible : *qui* a demandé quoi, *quand*, *quel* snapshot
  correspond.

---

## 2.6 Ce qui reste à trancher

- Gestionnaire de fenêtres exact (après mesure réelle de l'empreinte mémoire sur
  la machine, pas sur la base de suppositions).
- Outil de construction d'image reproductible.
- Remappage de la rangée de touches ChromeOS.
- Politique de mise à jour : Debian stable strict, ou backports ciblés pour le
  noyau si l'audio l'exige.
