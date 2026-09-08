# 7. Journal des séances — ce qui a été fait, ce qui reste

Ce document est le **fil chronologique** du projet depuis la remise en service
du bureau. Il ne remplace pas [`docs/06`](06-journal-incident-wayland.md), qui
autopsie en détail les pannes de session graphique ; il dit ce qui a été
entrepris, dans quel ordre, ce qui a été **mesuré**, et ce qui reste ouvert.

L'essentiel pour reprendre la main est dans [`CLAUDE.md`](../CLAUDE.md) —
les invariants et la procédure d'intervention. Ici, le détail.

---

## État au 8 septembre 2026

**Le bureau est en service et harmonisé.** L'écran de connexion s'affiche, le
mot de passe est accepté, la session labwc s'ouvre avec le dock, la Console et
le lanceur. Les fenêtres portent des boutons réduire/agrandir, et le thème
sort du shell pour atteindre Chromium, Claude Desktop, le terminal et les
barres de titre.

**Deux corrections attendent leur confirmation à l'écran** : le thème global
(après recompilation) et la luminosité par logind. Les deux sont éprouvées au
banc d'essai, aucune ne l'est sur MADOO.

**L'audio reste en échec**, et c'est le chantier n°1.

---

## Séance du 7 septembre — remettre le bureau debout

La machine ne démarrait plus sur un bureau : connexion en mode texte, invite
`stef@Claude-OS:~$`, rien d'autre. Le SSH était le seul filet.

### Ce qui avait cassé

La purge de l'ancienne pile X11 nommait `xwayland` et `x11-common`. Or
`labwc` porte `Depends: xwayland`, et `xwayland → xserver-common →
x11-common` : apt a donc **désinstallé le compositeur**, et la sortie partait
dans `/dev/null`. Personne ne l'a lu.

Mesuré, pas supposé : `apt-cache show labwc` donne la dépendance,
`apt-get -s purge` donne la cascade complète.

### Ce qui a été écrit

- La purge ne nomme plus les composants vitaux, et `provision.sh` **simule**
  toute purge avant de l'exécuter : si un vital figure dans la cascade, il
  abandonne.
- `bascule-session.sh` naît de cette séance : `--verifier`, `--deployer`,
  `--essai` sur un terminal virtuel libre, `--basculer`, `--revenir`. L'étape
  qui manquait était `--essai` — montrer le vrai écran de connexion sans rien
  activer.
- Le **filet de sécurité** : quatre minutes après le démarrage, il vérifie
  qu'on peut entrer ; sinon il collecte l'état, désactive greetd et redémarre
  sur un écran utilisable. On ne peut plus être enfermé dehors.
- Le greeter et la session consignent désormais leur contexte, leur sortie
  **et leur code de retour**. C'est ce qui a rendu la suite diagnosticable.

### La seconde panne, cachée derrière la première

Écran de connexion parfait, session qui meurt en une seconde, en boucle.
`~/.local/state/claude-os/session.log` l'a dit en une ligne :
`/tmp/.X11-unix not owned by root or us`.

labwc démarre Xwayland et **traite son échec comme fatal**. Le greeter tourne
sous `_greetd` : c'est lui qui avait créé le répertoire, à son nom, et la
session de l'utilisateur ne pouvait plus s'en servir.

Détail qui a coûté un aller-retour : **wlroots crée le répertoire s'il ne le
trouve pas**. Une règle `tmpfiles` ne suffit donc pas — rien ne garantit
qu'elle passe avant le premier Xwayland, et au premier redémarrage le greeter
a gagné la course. Ce qui garantit vraiment, c'est un `ExecStartPre` sur
greetd, exécuté en root à chaque démarrage du service.

### Le dépôt rangé pour la suite

`CLAUDE.md` est créé à cette occasion : où en est le projet, les invariants,
la procédure d'intervention, où lire quoi quand ça ne marche pas. Deux
scripts devenus faux — `patch-session-wayland-ssh.sh` et
`restore-session-x11-ssh.sh` — sont retirés.

---

## Séance du 7 septembre au soir — la Console et les fenêtres

Quatre demandes, toutes traitées :

| Demande | Ce qui a été fait |
|---|---|
| Un bouton éteindre / redémarrer / veille | Rangée d'alimentation dans la Console, par logind, avec **armement à deux clics** qui retombe seul au bout de quatre secondes |
| Beaucoup de fenêtres sans boutons réduire/agrandir | `rc.xml` passe en `decoration=server` : labwc pose lui-même une barre de titre à celles qui n'en dessinent pas |
| Le terminal ne suit pas le thème | `gnome-terminal` remplacé par **xfce4-terminal**, en GTK 3, qui suit le thème GTK. `foot` reste, uniquement pour le secours au clavier |
| Faire de la barre d'état une vraie Console | Redessinée en une page dense : volume, luminosité, tuiles Wi-Fi/Bluetooth, batterie, réglages, alimentation |

Deux constats mesurés avant d'écrire, plutôt que promis :

- **Le volume** ne commandera rien tant que l'audio est en panne : sans carte
  son, `wpctl` ne trouve aucune sortie. La rangée se désactive en le disant.
- **La luminosité automatique** supposerait un capteur de luminosité ambiante
  dont la présence sur MADOO n'est pas constatée. Non implémentée.

---

## Séance du 8 septembre — le thème, le déploiement, la luminosité

### Ce qui a bloqué le démarrage, encore

Deux causes de plus, chacune établie par mesure :

- **`greetd` occupait le mauvais terminal virtuel.** L'unité Debian porte
  `Conflicts=getty@tty7.service` : elle n'écarte le getty **que du tty7**.
  `config.toml` disait `vt = 1`, où `getty.target` démarre un getty. Les deux
  se disputaient l'écran, et labwc échouait sur `Atomic commit failed: busy`.
  D'où l'intermittence — un soir le bureau, le lendemain une invite texte.
- **`--essai` fabriquait le symptôme qu'il cherchait** : il s'installait sur
  le terminal virtuel 2, où logind engendre un getty. Déplacé au-delà de
  `NAutoVTs`, sur le 8.

### Le thème, qui ne sortait pas du shell

Le bureau était sombre, les fenêtres claires. La feuille de style du shell
n'est lue que par le shell.

Toutes les autres applications lisent **la même chose, et une seule** :
`color-scheme` de `org.freedesktop.appearance`, publié par le portail XDG.

```
gsettings org.gnome.desktop.interface color-scheme
  └─> xdg-desktop-portal-gtk
       └─> org.freedesktop.appearance/color-scheme  (+ SettingChanged)
            └─> Chromium, Claude Desktop, GTK 4
```

Trois pièges, chacun mesuré :

1. **`xdg-desktop-portal-gtk` déclare `UseIn=gnome`.** Sous labwc il n'était
   jamais choisi : l'interface `Settings` n'existait pas sur le bus, et
   `ReadOne` répondait « No such interface ». Corrigé par
   `/etc/xdg-desktop-portal/portals.conf`.
2. **`gsettings set` rend 0 sans conserver la valeur** quand dconf ou le bus
   manquent. `claude-os-theme` relit donc ce qu'il vient d'écrire.
3. **`labwc --reconfigure` ne marche que depuis un enfant de labwc** : il
   prend le destinataire dans `LABWC_PID`. Le script envoie SIGHUP lui-même.

Et un quatrième fait, celui qui rend le mécanisme possible : **labwc résout
ses fichiers de configuration un par un, pas par répertoire.** Un
`themerc-override` chez l'utilisateur ne masque donc pas le `rc.xml` système
— vérifié en posant les deux et en constatant que la barre de titre
apparaissait *et* prenait la couleur demandée.

Résultat éprouvé au banc d'essai : les quatre thèmes donnent la bonne valeur
de portail, `SettingChanged` part à chaque bascule, et la barre de titre
d'une fenêtre **déjà ouverte** passe de `#2a2a27` à `#f0eee6` — vérifié pixel
par pixel sur deux captures.

### Le correctif qui n'est jamais arrivé sur la machine

Tout ce qui précède a été poussé, tiré, déployé — et **rien n'a changé** au
redémarrage.

Le journal l'a dit : un seul appel à `claude-os-theme`, celui de l'autostart.
Le panneau de réglages avait été ouvert deux fois ensuite sans rien écrire.
`/usr/bin/claude-os-reglages` datait d'une heure plus tôt.

`propager_theme()` est du **C**. `--deployer` fait `cp -a rootfs/. /` et ne
compile rien. Le correctif était donc à moitié sur la machine, à moitié resté
dans le dépôt — et le vérificateur passait tous ses contrôles au vert.

Corrigé par un mode `--compiler` et un **contrôle de péremption** qui compare
la date des sources à celle du binaire installé le plus ancien. Il tourne dans
`--verifier`, à la fin de `--deployer` — là où l'on a été trompé — et à la fin
de `--compiler`, qui se soumet à son propre verdict. Éprouvé dans les deux
sens : il crie quand il faut, il se tait quand il faut.

L'invariant n°3 porte désormais ses deux moitiés : *« `git pull` ne déploie
rien, et `--deployer` ne compile rien. »*

### La luminosité, et un choix qu'il fallait défaire

Le curseur restait verrouillé et renvoyait à `provision.sh` — que
l'utilisateur venait de lancer.

La règle udev ouvre le fichier `brightness` au groupe `video`, et
`provision.sh` y ajoute le compte. Le défaut n'avait pas été pesé : **une
appartenance à un groupe ne prend effet qu'à la session suivante.**

La Console appelle maintenant **logind** d'abord — `SetBrightness` sur le bus
système — qui ne demande aucun groupe et écrit tout de suite. La signature
`(ssu)` est lue dans le binaire de `systemd-logind`, où elle suit
`SetBrightness` comme `uus` suit `PauseDevice`. L'appel est éprouvé contre un
logind de paille dressé sur un bus privé. Le groupe `video` et la règle udev
restent en filet.

---

## Ce qui reste à faire

| # | Sujet | État | Prochain geste |
|---|---|---|---|
| 1 | **Audio** | En échec | Voir ci-dessous |
| 2 | Thème global | Écrit, à confirmer à l'écran | `--deployer`, `--compiler`, rouvrir la session |
| 3 | Luminosité par logind | Écrite, à confirmer à l'écran | idem |
| 4 | Affichage au démarrage | Non diagnostiqué | Reconfirmer maintenant que greetd est sur le tty7 |
| 5 | Rangée supérieure du clavier | Non câblée | `bash tools/probe-keys.sh`, puis les liaisons dans `rc.xml` |
| 6 | rclone (Drive, OneDrive) | Reporté | Une section de plus dans le volet du gestionnaire de fichiers |
| 7 | Notifications | Reporté | — |
| 8 | Icônes sur le bureau | Reporté | Demande un septième programme — voir `docs/04` §4.4 |
| 9 | Luminosité automatique | Non implémentée | `ls /sys/bus/iio/devices/` **avant** d'écrire quoi que ce soit |

### 1. L'audio — le chantier n°1

```
sof_rt5682 jsl_rt5682_def: probe with driver sof_rt5682 failed with error -22
```

précédé de `ipc tx timed out` et `failed to load DSP topology`. Le DSP
démarre, la topologie ne se charge pas. C'était le risque n°1 identifié dès
[`docs/01`](01-materiel-firmware.md), et il s'est réalisé.

Ce qui n'a **pas** encore été tenté, et qui devrait l'être dans cet ordre :

1. Vérifier quel fichier de topologie le pilote réclame et s'il est présent —
   `dmesg | grep -i topology`, puis `ls /lib/firmware/intel/sof-tplg/`.
2. Comparer la version de `firmware-sof-signed` de Debian 13 avec celle que
   réclame le noyau 6.12.
3. Regarder si le paramètre de démarrage `snd_intel_dspcfg.dsp_driver=` change
   quelque chose : `1` force le pilote hérité, `3` force SOF.
4. Ne pas conclure avant d'avoir lu le journal du noyau **complet** au
   démarrage, pas seulement les lignes en erreur.

Tant que l'audio est en panne, la rangée de volume de la Console reste
désactivée et le dit — c'est voulu.

### 4. L'affichage au démarrage

L'écran restait noir jusqu'à ce qu'on touche le pavé tactile. C'était
peut-être le conflit de terminal virtuel de l'invariant n°5, maintenant
corrigé. **À reconfirmer**, et à ne pas déclarer résolu sans l'avoir revu.

---

## Ce qui n'est pas établi

Par principe, ce document distingue ce qui a été mesuré de ce qui est
plausible. N'ont **jamais** été vérifiés sur MADOO :

- l'appel `SetBrightness` au **vrai** logind — le banc d'essai n'a ni bus
  système, ni siège, ni écran rétro-éclairé ;
- que Chromium et Claude Desktop suivent effectivement la bascule de thème à
  l'écran — la chaîne est prouvée jusqu'au portail, pas au-delà ;
- la cause de l'écran noir au démarrage ;
- la présence d'un capteur de luminosité ambiante.

Une cause plausible n'est pas une cause.
