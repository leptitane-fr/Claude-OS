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

**Le thème global et la luminosité sont confirmés sur MADOO** : la bascule
clair/sombre est suivie par toutes les applications sans qu'aucune soit
relancée, et le curseur de luminosité commande l'écran immédiatement, par
logind, sans appartenance au groupe `video` ni réouverture de session.

**L'audio fonctionne.** Le `probe failed with error -22` a disparu des
journaux, PipeWire énumère cinq sorties, et les touches de volume du clavier
la commandent — confirmé à l'oreille. Ce fut longtemps le chantier n°1 ; il
ne l'est plus.

**La rangée supérieure du clavier est câblée**, volume et plein écran
compris. **Les notifications existent** — le shell est lui-même le serveur
freedesktop. **Les barres de titre sont uniformisées**, avec trois boutons
colorés propres au projet.

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

## Séance du 8 septembre au soir — Chromium, le clavier, la Console, les notifications

Une séance longue, en sept temps, où presque chaque correctif a commencé par
une hypothèse fausse mesurée puis écartée.

### Chromium : trois pannes, trois causes distinctes

**L'interface restait en anglais** alors que le réglage était bon
(`intl.accept_languages = "fr"`). `/usr/lib/chromium/locales/` ne contenait
qu'`en-US.pak` : le paquet `chromium-l10n` n'était pas installé, Chromium
n'avait donc aucune traduction à charger.

**Les sessions se perdaient à chaque lancement.** Chromium choisit son
fournisseur de clé de chiffrement d'après `XDG_CURRENT_DESKTOP`. Notre session
s'annonce `labwc` : aucun cas reconnu, il tentait le portail XDG « Secret »
et, à son échec, retombait sur une clé codée en dur. Deux clés selon les
jours, donc deux formats dans le profil — mesuré : **81 cookies en `v11`
(clé du trousseau) et 5 en `v10` (clé codée en dur)**, les uns illisibles sous
l'autre clé.

La cause côté portail était dans `portals.conf` : le `default=gtk` routait
*toutes* les interfaces vers xdg-desktop-portal-gtk, y compris `Secret`, que
gtk n'implémente pas. Corrigé sur les deux fronts — fournisseur nommé
explicitement (`--password-store=gnome-libsecret`) et route `Secret` vers
gnome-keyring. Après quoi `prev_init_success` passe à `true`, trois clés
disponibles, **zéro échec de déchiffrement**.

**La connexion au compte Google est cassée en amont, pas chez nous.**
L'identifiant OAuth partagé que Debian livre dans `/etc/chromium.d/apikeys` a
été interrogé directement :

```
client_id=811574891467.apps.googleusercontent.com
→ HTTP 401  {"error": "deleted_client"}
```

Google a supprimé ce client. Aucun réglage local n'y changera rien. Reste, si
récupérer les données de Chrome importe : installer Google Chrome à côté, ou
exporter/importer à la main.

### La rangée supérieure du clavier

Le noyau expose la rangée en mode « vivaldi » ; sa disposition se lit dans
`/sys/devices/platform/i8042/serio0/function_row_physmap` :
`EA E9 E7 91 92 94 95 A0 AE B0`.

**Volume : la liaison existait, la commande échouait.** labwc lie déjà les
touches audio, mais à `amixer sset Master` — et cette carte n'expose aucun
contrôle « Master », le son passant de toute façon par PipeWire. Le journal de
session en portait la trace, une ligne par appui :

```
amixer: Unable to find simple control 'Master',0
```

Redirigé vers `wpctl`.

**Plein écran : la touche n'était liée à rien.** Elle émet `KEY_FULL_SCREEN`
(372), que XKB nomme `<I380>` et traduit en keysym `XF86FullScreen`, présent
dans toutes les dispositions par `* = +inet(evdev)`. La validité du nom a été
éprouvée en soumettant d'abord à labwc un keysym volontairement faux, qui
produit `unknown keybind` — les deux vrais n'en produisent aucun.

### La Console

**Elle collait à la barre d'état** : mesuré au banc, le popover finissait à
`y=1037` et la pastille de la barre commençait à `y=1038`. Zéro pixel, et
l'ombre portée retombait dessus. Relevée de 12 px, l'écart que le dock et la
barre gardent déjà avec le bord de l'écran.

**Les curseurs ne suivaient pas les touches** : son et luminosité n'étaient
relus qu'à l'ouverture. Ni `wpctl` ni `brightnessctl` n'émettent de signal
qu'on puisse écouter ; une minuterie de 400 ms prend donc le relais entre
« show » et « closed », et nulle part ailleurs. Ce que cela coûte, mesuré :
**40 appels à `wpctl` en 1,71 s, soit 35 ms de processeur chacun**, environ
9 % d'un cœur pendant les quelques secondes où le panneau est ouvert.

**Les pages Wi-Fi et Bluetooth escamotaient la Console.** Elles se déplient
désormais à sa gauche, dans un révélateur, la Console restant entière à côté.

**La Console glissait de 73 px à l'ouverture du Bluetooth.** Instrumenté
plutôt que deviné : à taille de surface strictement identique (768x491),
`popup_x` valait -545 sur la page Wi-Fi et -618 sur le Bluetooth. Ni la liste
ni les noms d'appareils n'étaient en cause — deux fausses pistes mesurées et
écartées. C'était **le libellé du message « liste vide »** : un GtkLabel qui
s'enroule réclame la largeur de son texte déroulé, 369 px, et il vit sur la
page cachée d'une pile homogène en largeur. GTK positionnant le popover sur
la largeur naturelle, cette page invisible déplaçait tout.

**La liste n'indiquait jamais le réseau connecté.** Un même SSID est souvent
porté par plusieurs bornes ; on n'en garde que la plus forte, mais le drapeau
« actif » était posé sur celle-là. Mesuré sur le réseau de la maison : **le
SSID connecté est vu par quatre bornes, et celle à laquelle on est réellement
associé est à 56 de force quand deux autres sont à 60**. Le drapeau se calcule
désormais par OU logique sur toutes les bornes du SSID.

### Le centre de notifications

La machine n'en avait **aucun** : ni dunst, ni mako, ni notification-daemon, et
`NameHasOwner org.freedesktop.Notifications` répondait `false`. Chromium et
Claude Desktop émettaient dans le vide, sans erreur visible — une notification
perdue ne se plaint pas.

`shell/src/notifications.c` **est** le serveur, pas seulement l'affichage : il
prend le nom sur le bus de session et implémente les quatre méthodes de la
spécification freedesktop plus ses deux signaux. Sont honorés `replaces_id`,
l'urgence critique qui n'expire pas, `image-data` pour la favicon jointe par
Chromium, et l'action « default » qui rend la carte cliquable. `body-markup`
est volontairement **absent** des capacités annoncées : les libellés sont
rendus en texte brut.

Le centre et la bannière sont accrochés au même bouton que la Console — leur
bord droit est donc hérité, pas recalculé. Seule la verticale est calculée,
depuis la hauteur que la Console annonce. La formule étant unique, les deux
ordres d'ouverture donnent des géométries **identiques au pixel**.

Trois pièges, tous mesurés avant d'être corrigés :

- **Un popover s'ouvre vers le bas.** Né au ras de l'écran, GTK le retournait,
  et ce retournement annulait le décalage : offset -12 donnait `popup_y=-391`,
  offset -387 donnait `-390`. Avec `GTK_POS_TOP` explicite : -391 puis -766.
- **`gtk_icon_theme_add_search_path()` ignore un répertoire sans
  `index.theme`.** L'icône était installée, bien formée, et introuvable.
- **Un commentaire XML placé avant `<svg>`** repousse la balise hors de la
  fenêtre où gdk-pixbuf reconnaît le format : « Format d'image non reconnu »
  sur un fichier parfaitement valide.

Le « clic à côté » a demandé un détour : deux saisies du pointeur ne
coexistent pas, et le centre doit rester ouvert en même temps que la Console.
C'est donc la fenêtre de la barre qui s'étend à tout l'écran, transparente, le
temps de recueillir le clic.

### Les barres de titre

L'uniformité était déjà presque acquise — vérifié, pas supposé : nos
applications GTK4 portent la barre de labwc, n'ayant pas de `GtkHeaderBar`, et
Claude Desktop aussi. Ce qui manquait, c'étaient les boutons.

**Les images de boutons ne se surchargent pas** : labwc les cherche dans le
répertoire du thème, et `themerc-override` ne porte que des propriétés. D'où
un thème à nous, désigné par `<theme><name>` dans `rc.xml`.

Des pastilles colorées à coins arrondis, pas des disques : le disque aurait
été un emprunt direct à macOS, le rectangle arrondi est le vocabulaire de tout
le reste du bureau. Le glyphe est creusé et **visible en permanence** — macOS
ne le montre qu'au survol, ce qui n'existe pas au doigt.

Deux limites de labwc, mesurées : **`titlebar.height` n'existe plus** en 0.8.3
(« no longer supported » dans le journal), et **`--reconfigure` ne recharge
pas les images de boutons** — elles n'apparaissent qu'à la session suivante.

**Chromium était la seule vraie exception** : il dessinait son propre cadre,
avec un seul `×` et ni réduire ni agrandir. `browser.custom_chrome_frame`
passe à `false`. Réglage de goût, réversible d'un clic droit sur la bande
d'onglets, au prix d'une barre de 34 px au-dessus des onglets.

---

## Ce qui reste à faire

| # | Sujet | État | Prochain geste |
|---|---|---|---|
| 1 | Affichage au démarrage | Non diagnostiqué | Reconfirmer maintenant que greetd est sur le tty7 |
| 2 | rclone (Drive, OneDrive) | Reporté | Une section de plus dans le volet du gestionnaire de fichiers |
| 3 | Icônes sur le bureau | Reporté | Demande un septième programme — voir `docs/04` §4.4 |
| 4 | Luminosité automatique | Non implémentée | `ls /sys/bus/iio/devices/` **avant** d'écrire quoi que ce soit |
| 5 | Synchronisation Google dans Chromium | **Impossible** | Rien à faire localement : l'identifiant OAuth de Debian a été supprimé par Google. Passer par Google Chrome, ou exporter/importer à la main. |

**Rayés le 8 septembre 2026 :** l'audio, la rangée supérieure du clavier, et
les notifications. Les deux premiers ont été confirmés à l'usage par
l'utilisateur ; le troisième a été éprouvé de bout en bout sur la session
réelle, du bus D-Bus à la bannière.

### L'audio, pour mémoire — réparé le 8 septembre 2026

```
sof_rt5682 jsl_rt5682_def: probe with driver sof_rt5682 failed with error -22
```

précédé de `ipc tx timed out` et `failed to load DSP topology`. Le DSP
démarre, la topologie ne se charge pas. C'était le risque n°1 identifié dès
[`docs/01`](01-materiel-firmware.md), et il s'est réalisé.

Voilà ce que disait ce journal tant que la panne durait. Elle a été levée le
8 septembre 2026 par l'installation d'`alsa-ucm-conf` et une configuration
WirePlumber contournant l'ACP faute de profil UCM `sof-rt5682`. Le détail est
dans le journal du guichet, `/var/log/claude-os/actions.log`.

La marche à suivre d'alors est conservée ci-dessous : elle resservira si la
carte retombe en panne à une mise à jour de noyau.

Ce qui n'avait **pas** été tenté, dans l'ordre où il fallait le faire :

1. Vérifier quel fichier de topologie le pilote réclame et s'il est présent —
   `dmesg | grep -i topology`, puis `ls /lib/firmware/intel/sof-tplg/`.
2. Comparer la version de `firmware-sof-signed` de Debian 13 avec celle que
   réclame le noyau 6.12.
3. Regarder si le paramètre de démarrage `snd_intel_dspcfg.dsp_driver=` change
   quelque chose : `1` force le pilote hérité, `3` force SOF.
4. Ne pas conclure avant d'avoir lu le journal du noyau **complet** au
   démarrage, pas seulement les lignes en erreur.

La Console désactive d'elle-même sa rangée de volume quand `wpctl` ne trouve
aucune sortie, et le dit plutôt que d'afficher un curseur qui ne commande
rien — c'est voulu, et cela reste vrai si la panne revient.

### L'affichage au démarrage

L'écran restait noir jusqu'à ce qu'on touche le pavé tactile. C'était
peut-être le conflit de terminal virtuel de l'invariant n°5, maintenant
corrigé. **À reconfirmer**, et à ne pas déclarer résolu sans l'avoir revu.

---

## Ce qui n'est pas établi

Par principe, ce document distingue ce qui a été mesuré de ce qui est
plausible. N'ont **jamais** été vérifiés sur MADOO :

- la cause de l'écran noir au démarrage ;
- la présence d'un capteur de luminosité ambiante.

Une cause plausible n'est pas une cause.

**Rayés de cette liste le 8 septembre 2026**, parce qu'ils ont été vus à
l'écran et non plus seulement au banc d'essai : le suivi du thème par
Chromium et Claude Desktop, et l'appel `SetBrightness` au vrai logind. Ce
dernier méritait la prudence : le banc d'essai n'avait ni bus système, ni
siège, ni écran rétro-éclairé, et seule la forme de l'appel y était prouvée.
La machine a tranché.
