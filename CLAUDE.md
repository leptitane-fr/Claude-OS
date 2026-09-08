# Claude OS — à lire avant toute intervention

Ce fichier est chargé automatiquement à l'ouverture d'une session. Il dit
**où en est le projet** et **ce qu'on ne casse jamais**. Le détail est dans
[`docs/`](docs/) ; ici, l'essentiel pour reprendre la main sans dommage.

---

## Où en est le projet — 7 septembre 2026

**Le bureau est en service.** Firmware UEFI flashé, Debian 13 installée,
`greetd` ouvre l'écran de connexion Claude OS, le mot de passe est accepté et
la session labwc s'ouvre avec le dock, la barre d'état et le lanceur.

La machine est un **HP Chromebook x360 14b-cb0000sf**, board `MADOO`,
Pentium Silver N6000, **4 Go de RAM soudée**. Compte utilisateur : `stef`.
Accès SSH actif — c'est le filet de secours de toute intervention.

Versions constatées sur la machine : labwc 0.8.3, greetd 0.10.3,
xwayland 2:24.1.6, libgtk4-layer-shell0 1.0.4, dbus-user-session 1.16.2.

### Ce qui reste ouvert

| Sujet | État |
|---|---|
| **Audio** | **EN ÉCHEC.** `sof_rt5682 jsl_rt5682_def: probe with driver sof_rt5682 failed with error -22`, précédé de `ipc tx timed out` et `failed to load DSP topology`. Le DSP démarre mais la topologie ne se charge pas. C'est le risque n°1 identifié dès `docs/01`. |
| Affichage au démarrage | L'écran restait noir jusqu'à ce qu'on touche le pavé tactile. Probablement le même conflit de terminal virtuel que l'invariant n°5 — à reconfirmer maintenant que greetd est sur le tty7. |
| Thème global | **La chaîne est vivante sur la machine** : le portail y répond `uint32 2` en clair, `gsettings` conserve la valeur, `~/.config/labwc/themerc-override` est engendré à l'ouverture. Le 8 septembre, il manquait uniquement la **recompilation** du panneau de réglages — voir l'invariant n°3. Reste à confirmer à l'écran, après `--compiler` et réouverture de session, que Chromium (option « suivre le thème du système ») et Claude Desktop suivent la bascule. |
| Rangée supérieure du clavier | Non câblée. `tools/probe-keys.sh` relève les codes, les liaisons labwc restent à écrire. |
| Reports | rclone (Drive, OneDrive), notifications, icônes sur le bureau. |
| Volume dans la Console | Le curseur est en place mais **ne commande rien tant que l'audio est en panne** : sans carte son, `wpctl` ne trouve aucune sortie et la rangée se désactive d'elle-même en le disant. |
| Luminosité dans la Console | Exige que le compte soit dans le groupe `video` — `provision.sh` l'y ajoute, mais **l'appartenance ne prend effet qu'à la session suivante**. D'ici là le curseur se désactive et l'explique. |
| Luminosité automatique | Non implémentée : elle suppose un capteur de luminosité ambiante dont la présence sur MADOO n'a pas été constatée. À vérifier avec `ls /sys/bus/iio/devices/` avant d'écrire quoi que ce soit. |

---

## Les invariants — les enfreindre casse la machine

Chacun a coûté une soirée. Ils sont vérifiés par
`install/bascule-session.sh --verifier` ; ne pas les contourner.

### 1. Ne JAMAIS purger `xwayland`, `x11-common` ni `xserver-common`

`labwc` porte `Depends: xwayland`, et `xwayland → xserver-common →
x11-common`. Nommer l'un de ces trois dans une purge **désinstalle le
compositeur**, et la machine redémarre sans bureau ni écran de connexion.

Xwayland n'est jamais exécuté — tout parle Wayland nativement — mais il doit
rester installé. `xserver-xorg-core`, lui, est sans danger : mesuré.

`provision.sh` interroge apt **avant** toute purge et l'abandonne si un
composant vital figure dans la cascade. Ne pas retirer ce garde-fou.

### 2. `/tmp/.X11-unix` doit appartenir à `root`, mode `1777`

labwc démarre Xwayland à l'ouverture et **traite son échec comme fatal**.
Xwayland refuse le répertoire s'il n'appartient ni à root ni à l'utilisateur
courant. L'écran de connexion tourne sous `_greetd` : si c'est lui qui crée
le répertoire, la **session de l'utilisateur** ne peut plus s'en servir et
meurt en une seconde, en boucle — l'écran de connexion, lui, reste parfait.

**Et wlroots CRÉE le répertoire s'il ne le trouve pas**, au nom du compte qui
tourne à ce moment-là. La règle `tmpfiles` seule ne suffit donc pas : elle
s'exécute une fois, tôt, et rien ne garantit qu'elle passe avant le premier
Xwayland. Au premier redémarrage après sa mise en place, le greeter a gagné la
course et la panne est revenue à l'identique.

Ce qui garantit vraiment le répertoire est un `ExecStartPre` sur greetd —
exécuté en root juste avant lui, à chaque démarrage du service :

- `rootfs/etc/systemd/system/greetd.service.d/10-claude-os-x11-unix.conf`
- `rootfs/etc/tmpfiles.d/claude-os-x11.conf` (couvre les démarrages sans greetd)

**Leçon générale :** une correction qui dépend d'un ordonnancement qu'on n'a
pas vérifié n'est pas une correction, c'est un pari.

### 3. `git pull` ne déploie rien, et `--deployer` ne compile rien

Deux moitiés, et il a fallu se faire prendre par chacune.

**`rootfs/` n'arrive sur la machine que par `provision.sh` ou
`--deployer`.** Trois séances de diagnostic ont porté sur des correctifs
présents dans le dépôt et absents de `/`.

**`shell/` n'arrive sur la machine que par une compilation.** `--deployer`
fait exactement `cp -a rootfs/. /` : il ne transporte pas une ligne de C, ni
une feuille de style — celles-ci sont posées par `meson install`, pas par une
copie. Le 8 septembre 2026, la propagation du thème écrite dans
`shell/src/settings.c` a été poussée, tirée, déployée… et la machine a gardé
un `claude-os-reglages` daté d'une heure plus tôt. Le bureau changeait de
thème, rien d'autre ne suivait, `--verifier` disait que tout allait bien, et
l'on a cherché la panne dans le portail XDG — qui, lui, fonctionnait.

**La règle :**

| Ce que touche le correctif | Ce qu'il faut lancer |
|---|---|
| `rootfs/` | `--deployer` |
| `shell/` (C, en-têtes, `style/`, `data/`, `meson.build`) | `--compiler` |
| les deux | les deux |

`--verifier` **et** `--deployer` comparent maintenant la date des sources à
celle du binaire installé le plus ancien, et refusent de se dire satisfaits
quand le dépôt est en avance. `--compiler` recompile et réinstalle, puis se
soumet au même contrôle.

**Leçon générale :** un déploiement qui se déclare satisfait alors qu'il
laisse la moitié du correctif dans le dépôt est pire qu'un déploiement qui
échoue — on lui fait confiance, et on cherche ailleurs.

### 4. Aucune sortie de commande n'est envoyée dans `/dev/null`

Ce projet a perdu Chromium, puis le compositeur, puis une bascule de
gestionnaire de session, à cause de trois `>/dev/null 2>&1` sur des
commandes qui échouaient en silence. Une commande qui peut échouer doit
parler, et son code de retour doit être lu.

### 5. Le terminal virtuel de greetd doit être celui que l'unité protège

`greetd.service`, livré par Debian, porte `Conflicts=getty@tty7.service` : il
n'écarte le getty **que du tty7**. Configurer `vt = 1` dans
`/etc/greetd/config.toml` revenait à occuper un terminal non protégé, où
`getty.target` démarre un getty. Les deux se disputaient l'écran ; le perdant
n'affichait rien et labwc échouait sur `Atomic commit failed: busy`.

L'intermittence venait de là : selon le démarrage, la course avait un
gagnant différent — un soir le bureau s'ouvrait, le lendemain on tombait sur
une invite texte.

`config.toml` dit donc `vt = 7`, et `--verifier` compare les deux valeurs.
Bénéfice de côté : le getty du **tty1 reste disponible**, console de secours
permanente sur une machine sans touches F.

### 6. Ce dépôt n'a pas de branche par défaut

Trois branches `claude/…` coexistent. Un `git pull` sur la mauvaise répond
« Déjà à jour » sans rien changer. `provision.sh` affiche désormais sa
branche et son commit, et **refuse de tourner** s'il est antérieur au
correctif de la purge. La branche de référence est
`claude/examine-project-qgnt80`.

### 7. Le thème sort du shell par le portail XDG, et par lui seul

Chromium, Claude Desktop, le terminal, les dialogues GTK et les barres de
titre ne lisent pas la feuille de style du shell. Ils lisent tous la même
chose : `color-scheme` de `org.freedesktop.appearance`, publié par le portail.

La chaîne, mesurée de bout en bout :

```
gsettings org.gnome.desktop.interface color-scheme
  └─> xdg-desktop-portal-gtk
       └─> org.freedesktop.appearance/color-scheme  (+ signal SettingChanged)
            └─> Chromium, Claude Desktop, GTK 4
```

`claude-os-theme` l'alimente, écrit les réglages GTK, engendre
`~/.config/labwc/themerc-override` et envoie SIGHUP au compositeur. Le panneau
de réglages l'appelle à chaque changement, l'autostart une fois à l'ouverture.

Trois pièges, chacun payé :

- **`xdg-desktop-portal-gtk` déclare `UseIn=gnome`.** Sous labwc il n'est
  jamais choisi, l'interface `Settings` n'existe pas sur le bus, et rien ne
  suit le thème. C'est `/etc/xdg-desktop-portal/portals.conf` qui le désigne.
  **Retirer ce fichier suffit à tout casser, sans le moindre message.**
- **`gsettings set` rend 0 sans conserver la valeur** quand dconf ou le bus
  manquent. Le script relit donc ce qu'il vient d'écrire.
- **`labwc --reconfigure` ne marche que depuis un enfant de labwc** : il prend
  le destinataire dans `LABWC_PID`, que labwc ne pose que dans l'environnement
  des programmes qu'il lance. Appelé par SSH ou par `provision.sh`, il répond
  « LABWC_PID not set ». Le script envoie donc SIGHUP lui-même.

`~/.config/labwc` **n'est plus une anomalie** : la résolution de labwc se fait
fichier par fichier, pas par répertoire — mesuré. Un `themerc-override` y est
normal et attendu ; ce sont les homonymes de `/etc/xdg/labwc` (`rc.xml`,
`autostart`, `environment`, `menu.xml`) qui masquent et cassent la session.

---

## Intervenir sur la session graphique

Par étapes, et **jamais d'un bloc** — c'est la forme « tout d'un coup » qui a
produit la panne de septembre autant que son contenu.

```sh
cd ~/Claude-OS && git pull
sudo bash install/bascule-session.sh --verifier   # ne change rien
sudo bash install/bascule-session.sh --deployer   # recopie rootfs/ vers /
sudo bash install/bascule-session.sh --compiler   # recompile shell/ — voir n°3
sudo bash install/bascule-session.sh --essai      # ← REGARDER L'ÉCRAN
sudo bash install/bascule-session.sh --basculer   # arme le filet, puis bascule
sudo systemctl reboot
```

`--essai` affiche le véritable écran de connexion sur un terminal virtuel
libre pendant trente secondes, puis rend l'affichage — sans rien activer,
sans rien purger, sans toucher à la session en cours. En cas d'échec il verse
son autopsie dans `/var/log/claude-os-essai-<date>.txt`.

`--revenir` défait la bascule.

### Après un déploiement qui touche au thème

Le portail choisit son fournisseur **au démarrage** : `portals.conf` déployé
pendant une session ouverte ne sert à rien tant qu'il n'est pas relancé. Sous
le compte de l'utilisateur, pas en root :

```sh
systemctl --user restart xdg-desktop-portal xdg-desktop-portal-gtk
claude-os-theme          # repose color-scheme, GTK, themerc, et signale labwc
```

Sans cela, `--verifier` dit que tout est en place et les fenêtres restent
claires.

### Le filet de sécurité

Armé par `provision.sh` et par `--basculer`. Quatre minutes après le
démarrage, il vérifie que l'écran de connexion est affiché ou qu'une session
est ouverte. Sinon il écrit `/var/log/claude-os-echec-<date>.txt` — état des
binaires, des paquets, journaux de greetd et labwc — **avant** de désactiver
greetd et de redémarrer sur un écran où l'on peut entrer. Il se désarme seul
dès que la session a fait ses preuves.

Il garantit qu'on ne peut plus être enfermé dehors. Il ne couvre pas le cas
« écran de connexion présent mais session qui boucle » : c'est voulu.

---

## Où lire quoi quand ça ne marche pas

| Symptôme | Fichier |
|---|---|
| Pas d'écran de connexion, console texte, écran noir | `sudo bash tools/diag-connexion.sh` |
| Session ouverte mais bureau anormal | `bash tools/diag-session.sh` |
| L'écran de connexion meurt | `/var/log/claude-os-connexion.log` |
| La session meurt | `~/.local/state/claude-os/session.log` (et `.1`) |
| Le bureau démarre mal | `~/.local/state/claude-os/shell.log` |
| Le filet est intervenu | `/var/log/claude-os-filet.log` |

Le greeter et la session consignent leur contexte, leur sortie complète et
leur **code de retour**. Un journal vide alors qu'une tentative a eu lieu
signifie que le programme n'a pas été lancé du tout — pas qu'il s'est tu.

---

## Méthode

Ce projet s'est trompé plusieurs fois en annonçant des causes avec assurance.
Trois règles en sont sorties :

1. **Mesurer, pas supposer.** Une dépendance se lit dans `apt-cache show`,
   un comportement de labwc dans sa source, une panne dans son journal. Les
   décisions d'architecture prises sans machine sont des hypothèses.
2. **Écrire ce qui n'est pas établi comme tel.** `docs/06` dit explicitement
   ce qui n'a jamais été prouvé. Une cause plausible n'est pas une cause.
3. **Un outil de diagnostic qui ment coûte plus qu'il ne rapporte.** Deux
   faux négatifs ont été trouvés et corrigés avant livraison ; l'essai lui-même
   fabriquait un temps le symptôme qu'il cherchait.

Le code et les commentaires sont **en français**, et les commentaires
expliquent *pourquoi*, pas *quoi*. Les messages de commit sont en français,
détaillés, et disent ce qui a été mesuré.
