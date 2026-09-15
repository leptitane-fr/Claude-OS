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

## 9 septembre 2026 — les lecteurs réseau

Le bureau n'avait aucun moyen d'atteindre un partage. Ni `gvfs-backends`, ni
`cifs-utils`, ni `nfs-common` : la machine ne savait pas monter un serveur.

**La décision.** Montages du noyau plutôt que gvfs, sur mesure :
`gvfs-backends` demande 43 paquets — MTP, gphoto2, iOS, codecs AV1 et HEIF —
contre 19 pour les quatre protocoles par le noyau. Mais l'argument qui a
tranché n'est pas le nombre : un montage gvfs vit dans `/run/user/1000/gvfs`
et n'existe que pour les programmes qui parlent GIO. Un montage du noyau est
un répertoire, et tout le système sait lire un répertoire.

**Le NAS a servi de banc d'essai réel.** Trouvé par balayage du sous-réseau
puis par l'OUI `00:90:a9` de sa carte — Western Digital — à l'adresse
`192.168.1.29`, nom NetBIOS `WDMYCLOUDMIRROR`. Huit partages, trois ouverts
en invité. `//192.168.1.29/Vidéos` a été monté, parcouru en lecture et en
écriture, puis démonté.

**Ce que le NAS a appris.** Il plafonne à **SMB 2.1** : `vers=3.0` répond
`-95`, « Dialect not supported », et le noyau ne redescend jamais tout seul.
C'est précisément à cela que sert le champ « Options » du panneau. Son NFS
n'annonce que v2 et v3, et ne publie **aucun export** — d'où l'impossibilité
d'éprouver le chemin NFS.

**Trois usages après libération, trouvés en cliquant.** `reconstruire()`
libère `L->lecteurs` ; `connecter_lecteur` s'en servait juste après. SIGSEGV
à chaque clic sur un lecteur non connecté. Deux jumeaux ailleurs. Aucun ne se
voyait à la compilation. Il a fallu un clic réel et `coredumpctl`.

**Deux pièges de méthode**, détaillés dans
[`docs/08`](08-lecteurs-reseau.md) : `g_file_query_exists` est synchrone et
gelait la fenêtre sur un serveur éteint ; et `/etc/default/rpcbind` ne décide
rien, parce que rpcbind est activé par socket et que c'est systemd qui ouvre
les ports.

---

## 9 septembre 2026 au soir — l'écran de connexion

Demandé : que l'écran de connexion suive le thème de la session, qu'il
accepte un code PIN lié au mot de passe, et qu'il soit utilisable au doigt.
Détail complet dans [`09`](09-code-pin.md).

### Le thème ne suivait pas, et le code censé s'en charger ne POUVAIT pas marcher

`connexion.c` avait déjà une fonction `theme_de()`. Elle n'a jamais rien
donné, et personne ne l'avait vu — **trois défaillances muettes empilées**,
l'invariant n°4 en toutes lettres :

1. elle lisait `~/.config/claude-os/shell.conf`, mais `/home/stef` est en
   `drwx------` et le greeter tourne sous `_greetd`.
   `g_key_file_load_from_file` échouait à **chaque** ouverture, sans un mot ;
2. son repli était `claude-sombre`, un thème que la machine n'utilise pas —
   le sien est `sombre` ;
3. elle écrivait `cfg->theme` **sans repasser par `theme_par_id()`**, donc
   `cfg->dark` restait à `FALSE` quoi qu'il arrive : la feuille de style
   suivait, les widgets natifs de GTK non.

Aucune des trois ne se voit à la lecture du code. Chacune se lit dans un
`ls -ld /home/stef`, dans un `grep theme= shell.conf`, ou dans la signature
de la fonction qu'on n'a pas appelée.

Corrigé de trois façons, et la troisième est la plus importante : **tout
repli est désormais écrit** dans `/var/log/claude-os-connexion.log`. Un repli
muet est ce qui a caché la panne.

### C'est `pam_gnome_keyring` qui a décidé de toute l'architecture

`/etc/pam.d/greetd` contient :

```
-auth        optional        pam_gnome_keyring.so
```

**PAM doit donc recevoir le VRAI mot de passe.** C'est lui qui ouvre le
trousseau, où sont les mots de passe des lecteurs réseau de la section
précédente. Un module PAM maison qui aurait validé le code PIN aurait
authentifié l'utilisateur en laissant le trousseau fermé : session ouverte,
partages inaccessibles, aucun message. Trois séances de diagnostic en
perspective.

Le code PIN n'est donc pas un mot de passe de rechange, c'est une **clé de
coffre**. `claude-os-coffre` garde le mot de passe scellé, le rend contre les
six chiffres, et l'écran le relaie à greetd comme s'il avait été tapé. Rien ne
change en aval.

Le coffre sert aussi le **thème** : il est déjà le seul composant capable de
lire le répertoire personnel, et lui faire publier le thème évite un second
mécanisme tout en gardant `shell.conf` comme source unique — aucun fichier
miroir à tenir synchronisé.

### Ce qui protège le coffre

La socket, et rien d'autre : `SocketUser=_greetd`, `SocketMode=0600`,
`Accept=yes`. Pas de setuid, pas de `sudoers`. Le programme **redit** la règle
par `SO_PEERCRED`, pour qu'une unité systemd remplacée par une mise à jour ne
suffise pas à l'ouvrir.

`MaxConnections=4` n'est pas une valeur décorative : **129,4 Mio de pic
mesurés par instance** dans le journal. La valeur par défaut de systemd est
64 — soit 8 Go réservés sur une machine qui en a 4.

### Argon2id, mesuré et non recopié

Meilleur de trois passes sur le N6000, `p=1` :

| t | m | durée |
|---|---|---|
| 2 | 32 Mio | 0,105 s |
| 3 | 32 Mio | 0,155 s |
| 3 | 64 Mio | 0,312 s |
| **3** | **128 Mio** | **0,631 s** ← retenu |

Aller-retour complet par la vraie socket, démarrage du processus compris :
**0,68 à 0,79 s**, confirmé ensuite par le journal en usage réel
(`Consumed 714ms CPU time`).

`p=1` et non 4 : `gcry_kdf_compute(h, NULL)` déroule les voies **en série**.
Un `p` plus grand multiplierait l'attente sans rien coûter à l'attaquant, qui
paralléliserait.

### Trois pièges payés

- **`gcry_kdf_derive()` ne sait pas faire Argon2**, malgré son nom. Elle rend
  « Invalid value », sans plus. Argon2 n'existe que dans l'API à poignée, dont
  l'ordre des quatre paramètres n'est documenté nulle part dans `gcrypt.h`.
  Cet ordre — `{taglen, t, m, p}` — a été **établi contre le vecteur de test
  Argon2id de la RFC 9106 §5.3** : les quatre ordres plausibles ont été
  essayés, un seul le reproduit octet pour octet. Une demi-heure, et zéro
  supposition.
- **`g_printerr` transcode vers l'encodage de la locale**, et un service
  systemd n'en a pas : `LANG` est vide, la locale est « C », et tous les
  accents des messages ressortaient en `?` dans `journalctl`. `fputs` sur
  `stderr`. Un journal qu'on relit mal est un journal qu'on ne relit pas.
- **Un `GtkButton` prend le focus quand on le presse.** Sans
  `set_can_focus(FALSE)` **et** `set_focus_on_click(FALSE)` sur chaque touche
  des claviers à l'écran, le premier appui au doigt vole le focus au champ et
  **coupe la frappe physique** — silencieusement, et seulement après un clic,
  donc jamais au premier essai. L'utilisateur avait demandé les deux modes de
  saisie ; l'un serait tombé sans qu'on sache pourquoi.

### Ce qui a été vu, et dans quel ordre

`--essai` deux fois, puis une vraie ouverture de session.

Au banc d'essai, écran regardé : le volet nom d'utilisateur s'ouvre **sombre
comme le bureau** — une première —, le clavier azerty avec lui. Au doigt : les
touches écrivent, **et le clavier physique écrit toujours après**. ⇧ et &#
répondent. Le piège du focus est écarté, éprouvé et non déduit.

Puis la chronologie du coffre, lue dans le journal :

```
16:15:49  @9  (greeter 12214)   état : premier écran, aucun code PIN
16:16:15  @10 (greeter 12214)   « code PIN enregistré pour "stef" »
16:16:15  @11 (greeter 12214)   la relecture de contrôle, aussitôt après
16:17:04  @12 (greeter 12922)   état : second écran, un code PIN existe
16:17:10  @13 (greeter 12922)   ouverture — 721 ms, aucune erreur
```

Deux greeters différents, deux PID. Le second a ouvert la session **au code
PIN**. Et dans cette session :

```
org.freedesktop.Secret.Collection Locked  →  false
~/.local/share/keyrings/Default_Keyring.keyring  →  écrit à 16:17
```

**Le trousseau est déverrouillé**, et son fichier a été réécrit pendant la
session ouverte au code. C'est le point qui avait dicté toute l'architecture.

Une réserve d'honnêteté : cette mesure ne distingue pas « déverrouillé par
`pam_gnome_keyring` » de « trousseau sans mot de passe ». L'argument qui vaut
vraiment est structurel — PAM reçoit une chaîne **identique octet pour
octet**, qu'elle vienne du clavier ou du coffre, et ne peut pas faire la
différence. Le risque n'a jamais été que PAM se comporte autrement ; il était
de **contourner PAM**, ce qu'un module PIN aurait fait et ce que nous ne
faisons pas.

### Ce que ce confort retire

À dire sans détour, et c'est écrit en toutes lettres dans
[`09`](09-code-pin.md) : six chiffres, c'est **un million de combinaisons**, et
`mmcblk1p2` est un ext4 nu — pas de `crypttab`. Qui démonte l'eMMC attaque le
coffre hors ligne : **1,8 jour sur les quatre cœurs de la machine**, quelques
heures sur du matériel récent. Sans code PIN, le même attaquant ne trouvait
que le hachage yescrypt de `/etc/shadow`, dont il ne tire rien sans
dictionnaire.

**Le code PIN abaisse la sécurité au repos et l'améliore à l'usage** — on tape
six chiffres au lieu d'exposer un mot de passe complet en public, et on le
tape moins souvent. Arbitrage assumé, réversible d'un clic depuis l'écran
lui-même. Le seul vrai remède est le chiffrement du disque.

### Cinq garde-fous, dans le code et non dans la procédure

Ce chantier touche la seule porte de la machine.

1. Le mot de passe reste atteignable depuis **tous** les volets.
2. Le code PIN est facultatif, et se retire depuis l'écran.
3. Coffre absent ou muet → volet mot de passe, avec la raison affichée.
4. **Un enrôlement raté n'empêche jamais la session de s'ouvrir** : il part au
   journal, le code est abandonné, et l'écran en repropose un à l'ouverture
   suivante. La panne se rattrape d'elle-même.
5. **Ce qui vient d'être scellé est relu aussitôt** — c'est la connexion `@11`
   ci-dessus. Un scellement faux ne se verrait sinon qu'au réveil suivant,
   sous la forme d'un « mot de passe incorrect » sur un code juste.

Le filet de sécurité a été armé avant la première vraie ouverture
(`/etc/claude-os/filet-arme`, `2026-09-09 16:14:39`).

## Ce qui n'est pas établi

Par principe, ce document distingue ce qui a été mesuré de ce qui est
plausible. N'ont **jamais** été vérifiés sur MADOO :

- la cause de l'écran noir au démarrage ;
- la présence d'un capteur de luminosité ambiante ;
- **les lecteurs réseau NFS, SFTP et WebDAV** : écrits et compilés le
  9 septembre 2026, jamais montés pour de vrai. Le NAS ne publie aucun
  export NFS et son port 22 est fermé. Seul SMB a été vu fonctionner ;
- **le mode tablette**, qui justifie pourtant le clavier azerty à l'écran :
  jamais essayé, capot retourné, clavier physique coupé par l'EC ;
- **ce qui déverrouille exactement le trousseau** à l'ouverture au code PIN.
  Il est déverrouillé — mesuré — mais `pam_gnome_keyring` et « trousseau sans
  mot de passe » n'ont pas été distingués ;
- **le coût d'une attaque du coffre par carte graphique.** Argon2id à 128 Mio
  est limité par la bande passante mémoire, ce qui gêne un GPU ; écrire un
  chiffre serait l'inventer.

Une cause plausible n'est pas une cause.

**Rayés de cette liste le 8 septembre 2026**, parce qu'ils ont été vus à
l'écran et non plus seulement au banc d'essai : le suivi du thème par
Chromium et Claude Desktop, et l'appel `SetBrightness` au vrai logind. Ce
dernier méritait la prudence : le banc d'essai n'avait ni bus système, ni
siège, ni écran rétro-éclairé, et seule la forme de l'appel y était prouvée.
La machine a tranché.

**Rayés de cette liste le 9 septembre 2026 au soir** : le suivi du thème par
l'écran de connexion — vu sombre à l'écran, alors qu'il n'avait jamais pu
l'être — et le piège du focus des claviers tactiles, éprouvé au doigt et non
déduit d'un banc d'essai qui ne sait pas cliquer.

---

## 9 septembre 2026 — la veille progressive, et une porte grande ouverte

Demandé : que l'écran s'assoupisse au lieu de rester allumé pour personne.
Le rétroéclairage est le premier poste de consommation de la machine —
**environ 1 à 2 W sur les 6,8 W mesurés à la batterie**, plus que tout le
reste réuni.

### Ce qui a été livré

`shell/src/energie.c` vit dans `claude-os-status`, comme le centre de
notifications et pour la même raison : un processus séparé aurait coûté un
runtime GTK4 complet, **~40 Mo mesurés**, sur 4 Go soudés.

Trois modes nommés — **Travail, Automatique, Nomade** — et le mode est un
*choix*, pas une déduction de la prise. La version précédente dérivait le
comportement du câble : commode et illisible, personne ne pouvait dire ce
que la machine allait faire sans regarder derrière.

Étages : préavis → atténuer → préavis → éteindre → verrou → suspendre.
`shell_energie_delais_mode()` **borne l'ordre** : un mode qui ne dort pas
ne dort pas, quelle que soit la valeur écrite dans le fichier. C'est ce qui
rend un mode indénaturable depuis l'interface.

**Aucune scrutation.** `ext_idle_notifier_v1` prévient ; entre deux
événements le module ne coûte rien. Les inhibiteurs sont gratuits : la
spécification impose au compositeur de ne pas rendre la notification
inactive tant qu'un `zwp_idle_inhibitor_v1` existe. Vérifié dans les deux
sens — aucun événement tant que Claude Desktop tenait son verrou d'éveil,
les trois étages à l'heure dès qu'il l'a relâché.

**Le compte à rebours**, `preavis.c` : un soleil-chronomètre en bas à
droite, de la largeur exacte de la pilule de la barre d'état, fond
transparent. Disque central immobile, soixante graduations à trois
longueurs qui s'éteignent une à une. Il ne prend ni le clavier ni le clic —
région d'entrée **vide** — il se voit et ne s'attrape pas.

Trois versions ont été nécessaires, et les deux premières ont été rejetées
**à l'usage, pas à la lecture** : un texte « Veille dans 8 s » appelait la
lecture et reproduisait la gêne qu'il devait corriger ; un disque de 44 px
se laissait ignorer trop bien ; douze graduations faisaient une étoile et
non un chronomètre.

### Trois défaillances muettes, encore, et toutes du même genre

L'invariant n°4 ne parle pas que des `>/dev/null`.

1. **Deux `return` silencieux** dans `shell_energie_init` : sans affichage
   Wayland ou sans siège, le module renonçait sans un mot.
2. **Aucun `wl_display_flush`** après `get_idle_notification`. L'init a lieu
   avant que la boucle GTK ne tourne ; les requêtes restaient dans la file.
   Sur un bureau au repos — la situation même qu'on veut détecter — rien ne
   la vidait. *Le module de veille ne démarrait qu'une fois qu'il se passait
   quelque chose.*
3. **`g_dbus_proxy_call` avec `NULL` comme rappel**, au motif que « personne
   ne regarde ». Le plus coûteux : le journal montrait « etage 1 atteint »
   et l'écran ne bougeait pas.

### Deux bogues de fond, dont un vieux de plusieurs semaines

**L'écriture sysfs n'avait jamais fonctionné.** `g_file_set_contents()`
écrit de façon atomique, par un temporaire créé à côté puis renommé — et
**on ne crée aucun fichier dans sysfs**. L'erreur parlait d'un
« brightness.79UMV3 » introuvable, ce qui envoyait chercher du côté des
droits, à tort. `console.c` portait le même code : le repli sysfs du
curseur de la Console n'avait jamais marché depuis son écriture. Personne
ne pouvait le voir, logind réussissant toujours en session normale.
Corrigé en `open`/`write`/`close`.

**Le module recevait ses événements par salves puis plus rien.** Ses objets
Wayland vivaient dans la file **par défaut** de la connexion de GTK, que
GDK ne s'engage pas à vider : les `idled` s'y accumulaient et n'étaient
dépilés que lorsqu'une opération GTK provoquait incidemment un aller-retour.
Ce qui a tranché : un client d'essai isolé, lancé **au même instant sur le
même compositeur**, recevait tout pendant que la barre ne recevait rien.
Deux mesures simultanées valent mieux que dix raisonnements. Le module a
désormais **sa propre connexion** et vide sa file depuis une source GLib.

### Un piège de méthode qui a coûté une matinée

```
labwc, claude-os-fond    → session-N.scope   (seat0, tty7)
claude-desktop, et tout ce qui en est lancé
                         → user@1000.service/app.slice/app-com.anthropic.Claude
```

**Claude Desktop vit hors de la session du siège.** Or logind n'accepte
`SetBrightness` que de la session active. Un composant relancé à la main
depuis un terminal de Claude Desktop se le voit refuser, et le module
*paraît* cassé alors qu'il ne l'est pas.

**Un composant qui touche à logind ne se teste QUE démarré par l'autostart
de labwc.** Vérifier la portée avant de conclure :

```sh
grep -oE 'session-[0-9]+\.scope|app-com[^/]*\.scope' /proc/$(pgrep -x claude-os-statu)/cgroup
```

*(Depuis la correction de l'écriture sysfs, la luminosité ne dépend plus de
la portée — mais le piège vaut pour tout ce qui parle à logind.)*

---

## 9 septembre 2026 — le verrou d'écran, et une élévation vers root

### La porte grande ouverte, trouvée par hasard

En déployant le service PAM du verrou, un réflexe sur les droits :

```
/etc/pam.d/claude-os-verrou    stef:stef 664
```

`cp -a` conserve la propriété de la **source** — celle du dépôt, donc celle
de l'utilisateur. **Tous** les fichiers déployés depuis que `rootfs/`
existe étaient à `stef:stef`, dont :

| Fichier | Tourne en |
|---|---|
| `claude-os-coffre@.service` | **root** |
| `claude-os-filet.service` | **root** |
| `greetd.service.d/…` (`ExecStartPre`) | **root** |
| `filet-session` | lancé par root |
| `claude-os-greeter` | lancé par `_greetd` |

N'importe quel programme tournant sous le compte de l'utilisateur pouvait
réécrire l'`ExecStart=` d'un service root, déclencher le service, et
**obtenir root**. Sans exploit, avec un éditeur de texte.

Et cela vidait le coffre de sa protection. `docs/09` dit « la socket, et
elle seule » — mais l'unité qui **définit** cette socket était réinscriptible
par le compte qu'elle tenait à l'écart. Le contrôle `SO_PEERCRED` redit
dans `coffre.c` ne servait à rien davantage : le binaire vérifie son
appelant, pas qui a écrit l'unité qui l'a lancé.

`--deployer` reprend maintenant chaque chemin livré : `root:root`, 755 pour
les répertoires et exécutables, 644 sinon.

**Leçon générale :** un mécanisme de protection ne vaut que si ce qui le
définit est hors de portée de ce qu'il protège.

### Le coffre, ouvert à la session mais pas en grand

Le verrou tourne sous le compte de l'utilisateur et doit joindre le coffre.
Trois règles plutôt qu'une porte :

- `appelant_admis()` laisse entrer un compte ordinaire — et ne fait que ça ;
- `utilisateur_permis()` lui interdit de nommer quelqu'un d'autre que
  lui-même ;
- **moindre privilège sur les verbes** : `etat` et `ouvrir` seulement.
  Sceller et effacer restent au greeter.

Socket en groupe `users` et non un groupe neuf : le compte y appartient
**déjà**, alors qu'un groupe créé pour l'occasion aurait exigé une
réouverture de session — le piège du groupe `video` documenté dans
`console.c`.

*Ce que cela affaiblit :* un programme tournant déjà sous ce compte peut
tenter un PIN. Le compteur le borne à cinq, après quoi le coffre est effacé
et l'on retombe sur le mot de passe. Un tel programme pouvait de toute façon
enregistrer la frappe.

### Le verrou

`shell/src/verrou.c`, 712 lignes, **sans GTK**. Le protocole
`ext-session-lock-v1` demande une surface d'un type que GTK ne sait pas
produire, et aucune bibliothèque équivalente à `gtk4-layer-shell` n'existe
pour le verrouillage dans Debian trixie — vérifié. D'où `wayland-client`,
cairo, pango, xkbcommon et PAM à nu.

**Pourquoi ce protocole plutôt qu'une surface « par-dessus tout ».** Une
surface layer-shell en couche OVERLAY aurait permis de réutiliser l'écran de
connexion tel quel, clavier tactile compris. Mais elle disparaît avec le
processus : un plantage, et l'écran se déverrouille seul. `ext-session-lock`
fait l'inverse — si le verrou meurt, le compositeur **garde** l'écran
bloqué. Plus sûr, et c'est aussi le danger.

D'où **`--essai=N`**, qui verrouille pour de vrai puis rend la main tout
seul. Le vrai chemin de code est exercé sans pari. À lancer avant de
brancher le verrou, jamais après.

**Ce qui prouve que la chaîne marche** : aucun des deux journaux d'essai ne
contient « fin de l'essai ». Le minuteur n'a pas eu à servir — l'utilisateur
a déverrouillé lui-même au code PIN, et le compteur du coffre est repassé de
4 à 5, ce qui ne se produit que sur un succès.

Deux défauts vus **à l'écran** : un trait oblique en travers du panneau
(`pango_cairo_show_layout` laisse un point courant, `cairo_arc` s'y
raccorde — `cairo_new_sub_path()` avant chaque arc), et les accents perdus
dans le journal, le même piège `g_printerr`/locale que `docs/09` documente
pour le coffre, repayé faute de l'avoir lu jusqu'au bout.

### Éprouvé de bout en bout

Délais raccourcis, chaque étage à la seconde armée, luminosité
échantillonnée en parallèle :

```
20:02:44  étage 0  préavis atténuation   (armé à 10 s)
20:02:49  étage 1  atténuation           (15 s)  → 30 %
20:02:54  étage 2  préavis extinction    (20 s)
20:02:59  étage 3  extinction            (25 s)  →  0 %
20:03:04  étage 4  verrou d'écran lancé  (30 s)
```

---

## 10 septembre 2026 — la visionneuse d'images

Une application de plus dans le shell : `claude-os-images`, écrite en C sur
GTK4 comme Fichiers, dans `shell/src/images.c` et `shell/src/images-vue.c`.
Elle ouvre une image et parcourt tout son dossier. Les images s'ouvraient
jusque-là dans Google Chrome, seul programme de la machine à déclarer leurs
types ; la visionneuse est désormais l'application par défaut, par
`~/.config/mimeapps.list`.

### Ce qui a été livré

| Geste | Effet |
|---|---|
| balayage d'un doigt, glisser à la souris | image suivante ou précédente ; l'image suit le doigt, sa voisine entre par le bord, élastique au bout du dossier |
| ← →, Page préc./suiv., Espace, Début, Fin | même chose au clavier, en phase de capture pour passer avant les boutons |
| molette, deux doigts horizontaux sur le pavé | même chose ; un cran de molette = une image |
| pincer, tourner à deux doigts | zoom autour des doigts, rotation calée au quart de tour au lever |
| double appui, doigt ou souris | plein écran, et retour |
| F11, `f`, bouton, touche du Chromebook | plein écran — la touche est gardée par labwc, la visionneuse suit donc la propriété `fullscreened` |

En plus : zoom et taille réelle, rotation d'affichage (le fichier n'est jamais
réécrit), orientation EXIF appliquée, GIF animés, « Afficher dans Fichiers »,
liste suivie en direct si le dossier change, tri naturel identique à celui de
Fichiers. Aucune couleur propre : les jetons du thème, relus à chaud quand
`shell.conf` change — vu dans les quatre thèmes, et à la bascule.

### Décoder à la taille de l'écran — et pas exactement

Mesuré sur MADOO, photo de 4032 × 3024 :

```
pleine résolution            85 ms    35 Mo
réduite à 1920 px           112 ms     8 Mo
réduite à la moitié          50 ms     9 Mo
```

La première version réduisait à 1920 px, la largeur de l'écran. **C'était plus
lent que ne rien réduire du tout.** libjpeg ne sait réduire que par 2, 4 ou 8
pendant le décodage ; demander 1920 lui fait produire 2016, puis gdk-pixbuf
rééchantillonne le reste, et ce rééchantillonnage coûte plus que tout le
décodage. On réduit désormais par la plus petite puissance de deux qui couvre
encore l'écran. La pleine résolution n'est décodée qu'au zoom, pour l'image
courante seule. Mémoire mesurée : 78 Mo à l'ouverture, 121 Mo après une
rafale de navigation dans tout le dossier.

### Le piège du jour : les groupes de gestes GTK

Pincer et tourner ont été écrits, compilés, éprouvés au banc — et **ne
marchaient pas au doigt**. Rien d'anormal à l'écran : l'image ne réagissait
simplement pas.

Un journal pris sur la machine l'a dit en une ligne. Version de diagnostic
avec une trace par geste, plus `WAYLAND_DEBUG=client` :

```
wl_touch#42.down(…, 1, 455.13, 540.94)     ← le second doigt
DIAG on_deux_debut                          pincer démarre
DIAG on_deux_debut                          tourner démarre
DIAG on_deux_fin                            … et s'arrête
DIAG on_deux_fin
DIAG on_tenue_fin                           le glisser à un doigt aussi
```

Les deux doigts arrivaient bien — labwc annonce le tactile, `capabilities(7)`,
et GTK s'y abonne. Mais tous les gestes de la toile avaient été mis dans **un
seul groupe**, et dans un groupe GTK, ce qu'un geste refuse, tous le
refusent. La tenue et l'appui ne suivent qu'un doigt et refusent le second :
ils l'arrachaient au pincement dans l'image même où il se posait. Deux
groupes désormais — pincer + tourner, tenue + appui — et le journal suivant
compte 1 574 mises à jour de chaque.

**Le banc d'essai ne pouvait pas le voir.** Faute d'écran tactile simulable,
les gestes y étaient éprouvés par une souris virtuelle
(`zwlr_virtual_pointer_v1`, que labwc accepte) — un seul contact, donc
jamais de second doigt à refuser. La géométrie du geste à deux doigts, elle,
avait été vérifiée en appelant ses fonctions depuis un programme d'essai,
et elle était juste. Ce qui était faux était en amont, là où aucun outil du
banc n'arrive.

**Leçon :** un geste multi-touch ne se valide qu'au doigt. Le banc dit si la
géométrie est juste, pas si les doigts arrivent jusqu'à elle.

### Deux autres constats

- **Un double appui au doigt qui zoome**, comme sur un téléphone, a été
  essayé puis retiré à l'usage : sur cette machine, le plein écran s'est
  révélé plus naturel.
- **Le dock ne reconnaît pas la fenêtre de Fichiers.** Il rapproche fenêtres
  et icônes par l'app_id ; celui de Fichiers est `os.claude.shell.fichiers`,
  son `.desktop` s'appelle `claude-os-fichiers.desktop`, et les deux ne se
  correspondent pas. La visionneuse évite le défaut en nommant son fichier
  `os.claude.shell.images.desktop` — vu dans le dock, icône et point
  « ouverte » justes. Fichiers n'a pas été touché.

### Éprouvé

Au banc, sous AddressSanitizer et UBSan : parcours complet, balayages,
molette, pavé, flèches, zoom et pleine résolution, dossier, fichier illisible,
rafale de navigation, fermeture en plein décodage — aucun rapport. **Au doigt
sur MADOO** : balayage, double appui, pincement et rotation.

---

## 11 septembre 2026 — le dock qui sort de l'écran

Demande : le dock et la barre sortent de l'écran par le bas dès qu'on
travaille dans une application — clic sur une fenêtre, lancement — et
reviennent par la touche Loupe ou par un court glisser du doigt depuis le
bord bas. Jusque-là, la Loupe les masquait et les rendait, rien de plus.

### Ce qui a été livré

- `visibility.c` réécrit en **automate à trois états** — caché, bureau,
  convoqué —, qui ne vit plus que dans le dock. La barre reçoit « afficher »
  ou « masquer » sur le bus : deux bascules indépendantes, une par processus,
  finissaient par se contredire.
- `glissiere.c` : un conteneur qui translate son enfant vers le bas, puis
  retire la fenêtre. 220 ms, cubique.
- Une **bande de 10 px** au ras du bord, toujours affichée, qui guette le
  doigt ; seuil de 32 px, soit 5 mm sur cette dalle (310 mm pour 1920 px
  d'après l'EDID — et c'est **1920×1080**, pas 1920×1200 comme l'écrivent
  quelques commentaires plus anciens).
- La **nappe** : rappelé par-dessus une application, le dock tend sa fenêtre
  à tout l'écran pour recevoir le clic à côté.
- `claude-os-shell-basculer` ne s'adresse plus qu'au dock, se rabat sur la
  barre s'il ne répond pas, et ne jette plus rien dans `/dev/null`.
- La barre remonte le temps d'une bannière. Et une omission réparée au
  passage : la nappe du **centre de notifications** n'était jamais déployée
  (seul l'appel à `FALSE` existait depuis le 8 septembre), donc le clic à
  côté ne fermait pas le centre.

### Trois constats qui ont fixé la conception

**labwc ne dit rien quand on revient à la fenêtre déjà active.** Le clavier
qui passe à une surface layer-shell ne désactive pas la fenêtre
(`focus_change_notify`, seat.c de labwc 0.8.3). Recliquer ensuite l'application
ne produit donc aucun événement foreign-toplevel. D'où la nappe — et son
prix : le clic à côté est consommé.

**labwc éteint la couche TOP sous le plein écran.** Lu dans
`desktop_update_top_layer_visibility`, puis **vu au banc** : un dock en TOP
disparaît sous `foot --fullscreen`. Le commentaire de l'ancien
`visibility.h`, qui affirmait le contraire « constaté sur la machine », était
donc faux ou décrivait autre chose. Dock, barre et bande passent en OVERLAY.

**Une fenêtre GTK entièrement transparente et vide ne reçoit aucun appui.**
La bande, écrite avec la classe `shell` des autres surfaces, ne voyait
passer aucun geste — et labwc ouvrait son menu racine à la place. Bissection
au banc : fond `transparent`, rien ; fond à 1 %, l'appui et tout le glisser
arrivent. Encore une défaillance muette. La règle est écrite dans `dock.c`,
pas dans la feuille de style, pour qu'aucune harmonisation ne la défasse.

### Le banc

Un pointeur virtuel, `shell/essais/pointeur.c` (protocole
`wlr-virtual-pointer`), permet désormais de cliquer et de glisser pour de vrai
dans le labwc sans écran. `shell/essais/banc-dock.sh` joue tout le parcours
et vérifie l'état du dock après chaque geste — douze étapes, rejouables à
chaque retouche.

Un faux échec en est sorti, à retenir : **un clic sur le fond ouvre le menu
racine de labwc**, et le clic suivant sert à le refermer. Un scénario qui
clique sur le bureau puis ailleurs mesure le menu, pas le shell.

### Éprouvé

Au banc, sous AddressSanitizer et UBSan, trois cycles complets — Loupe en
rafale, survol, Console, bannière, clic à côté, glisser, icône de
l'application active, fermeture de toutes les fenêtres — sans un rapport.
`banc-dock.sh` : 12 étapes sur 12.
Plein écran : la Loupe et le glisser font paraître dock et barre par-dessus.
Au repos, **aucun commit** de surface du dock ni de la barre pendant trois
secondes ; une quinzaine par mouvement.

**Pas encore vu sur MADOO**, et c'est là que tout reste à juger : le glisser
**au doigt** — le banc n'a qu'un pointeur —, la hauteur de la bande face à un
doigt venu du cadre, et l'impression que laisse le mouvement.

---

## 11 septembre 2026, suite — la barre, le centre, la Console

Dix demandes : la date au-dessus de l'heure, une cloche à la hauteur de la
pilule, toute la pilule cliquable ; le centre de notifications qui sautait à
gauche, et qui devait se fermer au clic à côté ; des icônes pour les trois
modes, reprises dans la barre ; Wi-Fi et Bluetooth refaits en bouton plus
interrupteur ; la roue crantée retirée ; l'alimentation en icônes, avec
verrouiller et fermer la session en plus. Toutes livrées — voir `docs/04`
§4.2 pour ce que cela donne à l'écran.

### Le saut du centre : causé par la correction de la veille

La nappe du centre avait été « réparée » le matin même en ajoutant l'appel
qui la déployait — une ligne. Déployer, c'était étirer la fenêtre de la barre
à tout l'écran. Trace Wayland au banc : la surface passe de 216×42 à
1920×1080, GTK redemande la position du popover (`anchor_rect(1740, 1038)`),
et labwc 0.8.3 répond `configure(-151, …)` — il calcule depuis l'ancienne
origine de la surface. Le centre, et la Console si elle était ouverte,
partaient à gauche, en partie hors de l'écran.

**Règle, qui vaut pour tout le shell : on ne redimensionne jamais une
surface qui porte un popover ouvert.** La nappe est désormais sa propre
fenêtre, plein écran, dont la zone d'entrée laisse la barre dehors. Le dock,
qui s'étire lui aussi pour sa nappe, ferme ses listes au survol avant.

### Trois autres constats

- **Sous GTK 4, `gtk_widget_set_size_request` englobe les marges CSS.** La
  cloche demandait la hauteur de la pilule ; ses marges (10 px à droite,
  12 en bas) étaient prises dessus, et elle se peignait en 30×28. Les deux
  hauteurs ne coïncidaient jusque-là que par hasard. Les marges sont passées
  sur un socle, et la hauteur mesurée est la hauteur peinte
  (`gtk_widget_compute_bounds`, 44 px), pas `get_height` (42, sans bordure).
- **Retirer une fenêtre pendant un appui fâche GTK.** La nappe se retirait
  dès l'appui ; le relâchement ne lui parvenait jamais, et le journal disait
  « Broken accounting of active state ». Elle se retire à la fin du geste.
- **Papirus dessine les trois profils d'énergie comme trois cadrans** dont
  seule l'aiguille change. À 18 px on ne les distingue pas. Les icônes des
  modes sont prises chez Adwaita — compteur, balance, feuille —, recolorées
  comme toute icône symbolique.

Et une fragilité relevée au banc, **hors de ce chantier** :
`claude-os-verrou` demande un clavier sans vérifier que le siège en a un. Le
siège du banc n'en a pas ; le compositeur coupe la connexion pour erreur de
protocole. Sur MADOO le clavier existe toujours, mais un verrou qui meurt
une fois l'écran verrouillé laisserait la session inaccessible.

### Éprouvé

Au banc, pointeur virtuel à l'appui : les quatre bords de la pilule ouvrent
la Console ; le centre s'ouvre à droite et y reste, se ferme au clic à côté,
coexiste avec la Console ; le bouton Wi-Fi ouvre et referme son volet ; le
Bluetooth éteint est grisé ; Éteindre s'arme puis se désarme ; Verrouiller
lance bien `claude-os-verrou` ; l'icône de la barre suit un changement de
mode écrit dans `shell.conf`. Thème sombre vérifié. AddressSanitizer et
UBSan : aucun rapport. `banc-dock.sh` : 12 sur 12.

**Au banc, jamais les interrupteurs ni l'alimentation hors aperçu** : le bus
système y est le vrai, ils piloteraient le vrai Wi-Fi et la vraie machine.
Fermer la session n'a donc pas été éprouvé ; sur MADOO, `LABWC_PID` est bien
posé dans l'environnement de la barre et désigne labwc.

---

## 11 au 14 septembre 2026 — le mode tablette, de bout en bout

Quatre séances, une demande : « détection automatique du mode tablette,
déploiement d'un clavier AZERTY lorsque nécessaire, et optimisation de
l'interface pour la captation des mouvements et touchers tactiles ». Le
détail est dans [`docs/12`](12-mode-tablette.md) ; voici le fil, et ce
qu'il a coûté.

### Ce qui a été mesuré avant d'écrire

- **labwc 0.8.3 expose `virtual_keyboard_v1`, `input_method_v2` et
  `text_input_v3`.** `docs/09` et `clavier.h` affirmaient le contraire ;
  c'était faux, et cela fermait la porte au seul clavier qui vaille.
- **L'EC ne coupe pas le clavier ni le pavé tactile** capot retourné : les
  frappes arrivent à evdev, c'est **libinput** qui les écarte. L'effet est
  le même, la cause n'est pas celle qu'on croyait.
- **Le commutateur `Tablet Mode Switch`** bascule vers 180-220°, sans
  rebond, et les trois périphériques qui le portent concordent à 120 ms.
- **La portée des pouces**, tablette tenue à deux mains : 95 % des appuis
  du pouce gauche en deçà de 262 px du bord, du droit 320 px. C'est le
  plus court qui a fixé la largeur des colonnes : 300 px.

### Ce qui a été livré

Détection (`tablette.c`), rotation paysage ↔ chevalet (`rotation.c`,
portraits fermés à la demande de l'utilisateur), clavier à l'écran
(`clavier-ecran.c`, `saisie.c`) en deux formes — plein format calqué sur
le clavier physique, et **mode console** : deux colonnes collées aux bords,
écran recadré entre elles, frappe à gauche, bascules et fonctions à droite.
Suggestions de mots (`mots.c`) avec dictionnaire Lexique 3, suites de mots
Tatoeba, apprentissage, espace et majuscule automatiques.

**La disposition de la colonne gauche est calculée, pas héritée** :
fréquences du français, zone du pouce mesurée, loi de Fitts, recuit simulé
(`shell/essais/disposition-pouce.py`). Trois versions successives, chacune
née d'un essai au doigt : grille 5 × 6, puis 5 × 5 avec barre d'espace,
puis **organique** — touches de tailles différentes le long de l'arc du
pouce, les rares petites, les fréquentes larges, et une nuance de couleur
par fréquence.

### Trois pièges payés, et ils se ressemblent

1. **Les CODES de touches comptent.** La première disposition XKB donnait
   un code par caractère, à la suite. « ; : ! » avaient reçu les codes
   evdev de `KEY_LINEFEED`, `KEY_UP` et `KEY_LEFT` : dans Claude Desktop,
   ils déplaçaient le curseur au lieu d'écrire, parce que Chromium déduit
   du code la touche physique. ⌫ et ↵ occupaient `KEY_ESC` et ses voisines,
   et marchaient par chance. Désormais : commandes sur leurs vrais codes,
   caractères sur les seules positions de touches ordinaires, à quatre
   niveaux.
2. **La virgule décimale, deux fois.** `%.1f` dans la sonde des pouces a
   cassé son propre CSV ; `%.3f` dans `preavis.c` cassait la règle CSS du
   cadran depuis des jours — « Expected ')' at end of alpha() » dans
   `shell.log`, que personne n'avait lu. `g_ascii_formatd` pour ce qui est
   écrit à une machine, la virgule pour ce qui est lu par un humain.
3. **Une mesure se verse au dépôt le jour même.** Le fichier brut des
   appuis de pouce, laissé dans `/tmp`, a été effacé par un redémarrage.
   Les paramètres qui en dérivent sont désormais inscrits dans le script.

### La zone morte tactile — et la leçon de méthode

Le 13 au soir, l'utilisateur signale des zones où le doigt ne prend pas.
Deux sondes le confirment : un rectangle muet, **x de 0 à ~170 px, y de
~375 à 1080** — 28 × 114 mm le long du bord gauche. Trois voies
concordantes : carte de couverture, appuis délibérés jamais reçus, et un
appui rapporté à y = 68 alors qu'il avait eu lieu à mi-hauteur.

**J'ai conclu deux fois, dans deux directions opposées, sur un repère
supposé.** Ayant vu des contacts « à droite » alors que l'utilisateur
tapait à gauche, j'ai supposé l'axe X inversé, annoncé que le défaut était
logiciel, puis l'inverse. Quatre appuis dans les coins ont tranché : le
repère est **direct**, il n'y a aucun miroir. Une mesure interprétée dans
un repère non établi ne vaut rien — et l'établir coûtait quarante
secondes. `tools/diag-tactile.py` commence donc par la commande `coins`.

**Le lendemain matin, la zone avait disparu.** La dalle répondait partout.
Le défaut est donc INTERMITTENT, et c'est ce qui a sauvé le clavier : on
s'apprêtait à inscrire la zone en dur dans la disposition, ce qui aurait
été une faute durable pour une panne passagère. Rien n'a été modifié dans
le clavier. Si elle revient : relancer `tools/diag-tactile.py`, la mesurer
à nouveau, et voir si elle est au même endroit.

---

## 14 septembre 2026 — la batterie, le capot, et un diagnostic faux depuis cinq jours

### Ce qui a été démenti

Depuis le 9 septembre, ce dépôt affirmait que **la reprise après
suspension était cassée** : onze suspensions qui « n'avaient jamais
repris », le journal s'arrêtant net sur `PM: suspend entry (s2idle)`.
C'était faux, et l'utilisateur l'a dit d'emblée : « ce sont des pannes
de batterie non alertées ». Le journal lui a donné raison, ligne à ligne :

```
07:23:20  Lid closed.
07:23:42  PM: suspend entry (s2idle)
07:24:00  Lid opened.  ->  PM: suspend exit
```

**La reprise fonctionne.** Si le journal s'arrêtait sur `suspend entry`,
c'est que la machine MOURAIT en veille, faute de courant.

Et la preuve en grand, le matin même : **101 démarrages enregistrés**,
dont une centaine entre 05:39 et 07:23, par cycles réguliers de
64 secondes — démarrer, vivre 28 secondes, se rendormir capot fermé,
mourir. Une machine à plat qui n'arrivait pas à se recharger parce
qu'elle se rendormait à chaque fois qu'elle revenait.

**LA LEÇON DE MÉTHODE, et c'est la même qu'au 13 septembre avec la zone
morte tactile :** un symptôme qui a deux causes possibles n'en désigne
aucune. Un journal tronqué ne dit pas pourquoi il est tronqué, et
l'identifiant de démarrage neuf — le seul indice retenu à l'époque —
est exactement le même qu'on meure de faim ou qu'on échoue à reprendre.
Le diagnostic a tenu cinq jours et fermé un étage entier de la veille.

### Ce que la machine ne savait pas faire

**Rien ne surveillait la charge.** Ni upower, ni démon d'énergie, et le
seuil ACPI `alarm` à zéro. Le shell ne lisait la batterie que lorsque la
Console était ouverte — c'est-à-dire quand l'utilisateur regardait déjà.
Une machine qui ne sait pas qu'elle va manquer de courant ne peut ni
prévenir, ni se mettre à l'abri.

**Et la veille profonde était à portée**, contrairement à ce que ce
document affirmait. `resume=` est bien absent de la ligne de commande,
mais Debian passe par l'initramfs : `RESUME=UUID=…` y était,
`/sys/power/resume` valait `179:3`, le firmware annonçait `S0 S3 S4 S5`,
et `PM: Image not found (code -22)` à chaque démarrage prouvait que le
chemin de reprise s'exécutait déjà, sans rien trouver. logind répondait
`CanHibernate` = yes avant qu'on ait rien tenté.

**L'utilisateur a hiberné par le capot le jour même. La session est
revenue intacte.**

### Ce qui a été livré

Trois modules, et une règle udev :

- **`batterie.c`** — trois seuils réglables dans le panneau Énergie :
  prévenir, insister, se mettre à l'abri. Les deux premiers parlent, le
  dernier agit et demande d'abord à logind s'il sait faire.
- **`capot.c`** — le capot repris à logind par un inhibiteur
  `handle-lid-switch` en `block`, six actions dont « Suspendre, puis
  hiberner ». Le commutateur est « Lid Switch » (ACPI PNP0C0D), qui ne
  porte QUE `SW_LID` — le même raisonnement que pour « Tablet Mode
  Switch », d'où `70-claude-os-capot.rules` et son `uaccess`.
- **`energie.c`** — le verrouillage systématique avant sommeil, ancré
  là et non dans le capot : le capot n'est qu'une des façons de
  s'endormir, et logind émet `PrepareForSleep` pour toutes.
- **`logind.c`** — la mécanique des inhibiteurs en un seul endroit, le
  jour où un second module en a voulu un.

### Il faut scruter, et c'est mesuré

La règle du projet est « aucune scrutation ». `batterie.c` ne peut pas
la tenir, et ce n'est pas une facilité : **deux voies sans scrutation ont
été essayées, les deux muettes.** Sept minutes d'uevents `power_supply`
pendant une charge active — neuf changements de pourcentage, **zéro
événement** ; et le seuil matériel `alarm`, armé au-dessus de la charge
courante donc franchi d'emblée, sans plus d'effet ni changement de
`capacity_level`.

L'intervalle de lecture se calcule donc depuis le temps restant avant le
prochain seuil, plutôt que d'être fixe : loin du seuil la machine dort,
près du seuil elle regarde souvent.

### Trois fautes, et ce qui les a trouvées

1. **Un `g_free` en trop**, glissé dans `shell_config_load` par un
   remplacement de texte trop peu spécifique — il avait frappé deux
   endroits au lieu d'un. Toute configuration contenant
   `batterie_abri_action` faisait tomber la barre d'état sur
   `free(): double free`. **AddressSanitizer l'a nommé en trente
   secondes** : libéré `config.c:273`, relibéré `config.c:326`, douze
   octets — la taille d'« automatique ».

2. **Deux champs inversés** dans la table des actions du capot :
   `{id, nom, resume, methode}` déclaré contre `{id, nom, methode,
   resume}` écrit. Le panneau affichait « Suspend » en guise de phrase
   explicative. **C'est la capture d'écran qui l'a trouvé**, pas la
   relecture du code.

3. **Des notifications sans accents** — « Pensez a brancher », « Batterie
   tres faible ». La règle du dépôt veut des commentaires sans accents ;
   je l'avais appliquée à ce que l'utilisateur lit. **Vue sur une vraie
   notification**, là encore.

Les deux dernières ont la même morale : **regarder l'écran trouve ce que
relire le code ne trouve pas.** Elles rejoignent le `%.3f` de `preavis.c`,
qui cassait le cadran depuis des jours dans un `shell.log` que personne
ne lisait.

---

## 14 septembre 2026, suite — l'apparence se règle, et le bureau a ses icônes

Trois demandes d'esthétique, toutes **vues à l'écran sur MADOO**.

### La transparence, en trois lignes

`style/verre.css` ne contient aucune règle :

```css
@define-color surface      @verre;
@define-color surface-alt  @verre-alt;
@define-color surface-sunk @verre-sunk;
```

Le dock, la barre, la Console, le lanceur et les fenêtres du système ne
peignent jamais une couleur : ils peignent l'un de ces trois jetons. Les
renommer suffit donc, et il n'y a **aucune liste de classes à tenir à jour** —
une liste qui aurait vieilli à la première fenêtre ajoutée, en silence.

**Trois valeurs de verre et non une, l'opacité croissant avec
l'enfoncement.** Le survol d'une icône se peint par-dessus le fond du dock ;
plus clairsemé que lui, il se lirait comme un trou creusé dans la surface.

Décochée par défaut, et **pas par prudence d'affichage** : une surface
translucide interdit au compositeur de la poser sans mélange, et se paie en
remplissage GPU donc en watts.

### La couleur de contraste

Même mécanique, d'un cran au-dessus : `style/accent-<id>.css` ne redéfinit
que quatre jetons, chargé comme un fournisseur CSS de priorité supérieure.

**Le mécanisme a été vérifié avant d'être écrit** — un programme d'essai a
confirmé qu'un fournisseur plus prioritaire redéfinit une couleur nommée pour
des règles écrites ailleurs, et que le vider rend la main au thème.

**Les huit teintes ne sont pas choisies à l'œil** : chacune est la plus vive
de sa famille qui tienne encore **4,5:1 sous du texte blanc**. C'est ce seuil
qui a fixé la valeur, pas l'inverse — un réglage nommé « couleur de
contraste » qui rendrait les libellés illisibles serait une plaisanterie.

### Le thème d'icônes de la distribution

[`tools/fabrique-icones.py`](../tools/fabrique-icones.py) engendre 116
pictogrammes. **C'est le générateur qui est la source** ; corriger un SVG
installé serait perdu à l'exécution suivante.

Ce qui fait qu'un jeu paraît dessiné plutôt qu'assemblé n'est pas le talent
de chaque pictogramme, c'est qu'ils partagent l'épaisseur de trait, le rayon
d'angle et la marge. Ces constantes sont en tête du fichier.

**Tout y est une surface pleine, jamais un contour.** GTK recolore une icône
`-symbolic` en imposant `fill` ; un `stroke` resterait noir sur un thème
sombre. Ce que GTK recolore exactement — `rect`, `circle`, `path`, `polygon`,
y compris dans un groupe transformé — a été **mesuré** : une icône d'essai
rendue en rouge, puis les pixels relus.

**LE CONTRÔLE DE COUVERTURE A TROUVÉ UN VRAI DÉFAUT.** Le générateur relève
les noms cités dans `shell/src/*.c` et dit lesquels il ne dessine pas.
`status.c` ne demande pas un nom écrit en clair : il **compose**
`battery-level-%d%s-symbolic` depuis la charge arrondie à la dizaine. Le
thème ne dessinait que le cran 100 ; sur une machine à 67 %, la barre d'état
servait la batterie de Papirus au milieu de nos icônes. Les vingt-deux crans
sont désormais engendrés — et le contrôle sait que ces noms-là ne peuvent pas
être trouvés par un `grep`.

---

## 15 septembre 2026 — les avis, le coin, le tiroir : la barre d'état disparaît

La plus grosse refonte de l'interface depuis la mise en service. Elle s'est
faite en cinq temps, chacun sorti de l'usage réel.

### 1. Les avis système ont une surface à eux

`shell/src/preavis.c` est devenu `shell/src/avis.c`. Ce n'était plus le seul
compte à rebours de la veille : c'est **l'endroit unique** où le système
affiche ce qu'il a à dire en passant. Au centre, au-dessus du dock, en blanc,
sans clic ni survol ni focus.

**C'est le lieu qui fait l'avis, pas le module qui l'émet.** Un signal
périphérique ne vaut que si l'œil sait d'avance où le trouver.

**L'avis n'est pas la notification, et les deux coexistent.** L'avis est
fugace ; ce qui doit se retrouver plus tard passe par la cloche.
`batterie.c` fait les deux aux seuils, et **l'avis seul** aux bascules de la
prise — on ne va pas chercher dans l'historique la confirmation d'un geste
qu'on vient de faire.

Le diamètre ne se mesure plus sur la pilule de la barre : au centre de
l'écran il n'y a plus de bord à partager, et faire dépendre un diamètre de la
largeur de l'heure affichée était devenu une coïncidence entretenue pour rien.

### 2. « En charge » arrivait jusqu'à cinq minutes trop tard

Rapporté à l'usage. La cause était nette une fois posée : `batterie.c` ne
découvrait le branchement qu'à sa lecture suivante, et l'intervalle sur
secteur est de 300 s.

`batterie.h` dit, mesures à l'appui, que cette machine n'émet **aucun**
événement quand le pourcentage change — quinze minutes d'écoute en décharge,
quatre changements, zéro événement. **Mais cela ne valait que pour le
pourcentage.** Brancher est un événement matériel, et le noyau l'annonce.

Vérifié avant d'écrire une ligne : socket **netlink**, groupe 1 — celui des
uevents du noyau, déclaré `NL_CFG_F_NONROOT_RECV`, donc **abonnable sans
privilège et sans libudev**. Cinq messages arrivent d'un coup, un par
alimentation, avec `POWER_SUPPLY_ONLINE` dans la charge utile.

Puis vérifié dans le vrai `claude-os-status` : événement à 09:26:54, **une
seule** lecture — la rafale est regroupée sur 250 ms. La scrutation reste en
filet, et le journal dit lequel des deux chemins a parlé.

**Un second retard s'est révélé ensuite** : l'avis arrivait à l'heure, mais la
fiche du coin tenait de la minuterie d'une minute de `status.c` — deux témoins
de la même chose, dont l'un mentait pendant jusqu'à soixante secondes.
`shell_batterie_sur_lecture()` prévient désormais à chaque lecture, d'où
qu'elle vienne.

### 3. Le coin remplace la barre d'état, le tiroir remplace le clic

Sur maquette fournie par l'utilisateur. La pilule opaque du bas-droite laisse
la place à deux choses :

**Le coin** (`coin.c`) : des tracés clairs posés sur le fond d'écran, sans
fond ni bordure ni ombre, **permanents** et **insensibles au clic comme au
survol**. Heure, date en toutes lettres, mode d'énergie en grand par-dessus la
droite de l'heure, et en colonne non-lu, réseau, Bluetooth, charge, fiche
secteur.

**Le tiroir** (`tiroir.c`) : deux volets tirés du bord droit — widgets à venir
en haut, Console en bas. Glissé du doigt, ou **pointeur posé une seconde**
contre le bord : une attente et non un contact, le bord droit étant l'endroit
où finit tout mouvement un peu vif.

Quatre points de conception qui ont demandé du soin :

- **La Console n'est plus un popover.** `panel_new()` rend le contenu, et la
  relecture périodique suit `map`/`unmap` plutôt que `show`/`closed` : ces
  deux signaux disent exactement « la Console est à l'écran », ce que
  l'ouverture d'un popover ne garantissait pas.
- **La fenêtre du tiroir est plein écran dès sa création**, et c'est le
  revealer qui bouge. Redimensionner une surface layer-shell qui porte un
  popover ouvert l'envoie hors de l'écran sous labwc 0.8.3 — règle du
  11 septembre — et la Console en ouvre.
- **La nappe et les volets sont séparés par un `GtkOverlay`**, pas par un test
  dans un gestionnaire de clic : GTK désigne le widget le plus haut, la
  distinction est structurelle.
- **Le centre de notifications n'a plus d'entrée**, par choix explicite de
  l'utilisateur — le volet haut reste vide en attendant un widget. La
  **bannière**, elle, reste : elle s'accroche au coin. Perdre l'historique est
  un choix ; perdre l'annonce à l'arrivée en aurait été un autre.

**Les trois modes d'énergie ont dû changer de glyphes.** Le coin n'en montre
qu'un, en grand et sans libellé : trois cadrans que seule l'inclinaison d'une
aiguille distinguait ne pouvaient plus faire l'affaire. Un badge « AUTO » a
été essayé puis écarté — dire la chose par un mot est l'aveu qu'on n'a pas
trouvé l'image, et quatre lettres deviennent illisibles à la taille de la
Console. C'est finalement **la famille d'Adwaita** qui a été reprise, cadran,
balance et feuille, redessinée dans la grammaire du projet : celle que le
bureau portait avant d'avoir son propre jeu, et que l'utilisateur est venu
rechercher.

### 4. Rien ne doit bouger, et c'est mesuré

Le coin est ancré à droite : toute largeur qui change déplace son bord
gauche. Trois causes, trouvées à l'usage :

- **L'heure.** En chasses proportionnelles, « 11:11 » est plus étroit que
  « 10:00 ». La boîte heure/date prend la plus large de ses deux lignes — et
  selon la minute c'était l'heure ou la date qui l'emportait. Le bloc sautait
  **une fois par minute**. Chiffres tabulaires.
- **La date.** Largeur fixe, **mesurée** : 28 jours consécutifs — les sept
  jours de la semaine — sur douze mois, 336 formatages au démarrage, on garde
  le plus large. Mesurée et non écrite en dur : elle dépend de la police.
- **La fiche secteur**, qui était masquée sur batterie. Un widget masqué ne
  reçoit plus d'allocation : la batterie glissait de vingt pixels à chaque
  branchement. Elle est posée à **opacité zéro**, sa place réservée.

**Vérifié** : deux captures à 14:24 et 14:25, bornes du bloc relevées au
pixel — `x de 1711 à 1905` les deux fois, **0 px de déplacement**.

### 5. Le blanc sur du blanc — trois tentatives, et la bonne en dernier

Le coin écrit en blanc. Sur une page web blanche en plein écran, il
disparaissait.

**Premier essai, un vignettage court** (500 px, 0,55 d'opacité). Il faisait
son travail — 4,7:1 mesurés sous l'heure — et se lisait comme **une tache
grise** dans le coin : son bord se voyait.

**Deuxième essai, un vignettage long** (800 px de course, cinq paliers, 0,30).
La transition devenait invisible, mais le voile occupait **un quart de
l'écran** et tirait l'œil ; et à cette densité il ne donnait plus que 1,6:1.

**LES DEUX EXIGENCES NE SE CUMULENT PAS POUR UN VOILE DE RÉGION** : assez
dense pour porter du blanc sur du blanc, il se voit.

**Troisième essai, proposé par l'utilisateur et retenu : une ombre portée
sous chaque élément.** Elle obtient le même détachement sur quelques pixels
et ne prend aucune place — mesuré, le gris de la page reste à 255 dès 200 px
du coin. C'est ce que font les sous-titres, et pour la même raison : elle
suit le glyphe au lieu d'assombrir la région.

Deux ombres par élément, sur le modèle de celles du dock : une courte et
dense décalée d'un pixel, qui donne le contour, et une large sans décalage,
qui pose le halo.

**L'ombre a révélé un défaut vieux de la refonte** : le glyphe du mode était
l'enfant *superposé* de la `GtkOverlay`, donc dessiné **par-dessus** l'heure.
Tant qu'il n'était qu'une forme claire en retrait, cela ne se voyait pas ;
dès qu'il a porté une ombre, celle-ci est tombée sur les chiffres. Un
filigrane se met derrière — c'est la définition d'un filigrane.

Les quatre nombres de l'ombre vivent dans `shell.conf`, relus à chaud, pour
que le réglage se trouve à l'œil sans recompiler. **Pas dans le panneau** :
on y règle des habitudes, pas des détails de dessin.

### 6. Et il s'efface sous une fenêtre plein écran

Concédé à contrecœur, et sorti de l'usage : une heure posée sur un film n'est
plus un service, et **les commandes de lecture de Netflix vivent exactement en
bas à droite** — deux tracés clairs l'un sur l'autre, illisibles tous les
deux. Il n'y a pas d'arrangement ; l'un des deux doit partir, et ce n'est pas
au film de s'effacer.

`wlr-foreign-toplevel-management-v1`, qui ne consulte rien. **Toute** fenêtre
plein écran non réduite compte, pas seulement l'active : la question est
« quelque chose couvre-t-il l'écran », pas « qui a le clavier ».

**Mesuré** sur les trois états, contraste relevé dans le bloc du coin :
bureau `9–255`, plein écran `249–255`, retour `9–255`.

### Quatre pièges payés, tous muets

- **`window.shell { background: transparent }` gagne sur `.coin`** par
  spécificité. Du temps du vignettage, le dégradé n'était jamais peint et rien
  ne le disait — 255 mesurés sous le texte là où on attendait 115.
- **Padding et fond posés sur le nœud `window` font DISPARAÎTRE le coin.** La
  surface layer-shell garde la taille du contenu seul pendant que GTK place
  l'enfant hors d'elle. Un coin entièrement vide, sans une ligne de journal.
- **GTK 4 refuse `icon-shadow`**, le nom de GTK 3 : « No property named
  icon-shadow » au chargement de la feuille, sans que rien d'autre ne
  s'arrête. C'est `-gtk-icon-shadow`. Les deux ont été soumis au parseur
  avant que la règle soit écrite.
- **GTK 4 ne publie plus `size-allocate`** sur les widgets : la région
  d'entrée du coin se repose sur `GdkSurface::layout`.

**Et une faute de méthode, de mon fait.** Un `--compiler` lancé depuis
`shell/` avec un chemin relatif n'a rien fait, n'a rien affiché, et mon
`grep -c '✗'` a rendu 0 sur une sortie vide : j'ai cru l'installation faite
alors qu'elle n'avait pas eu lieu, et c'est la capture d'écran qui l'a
montré. **L'invariant n°4 en toutes lettres** — compter les lignes d'une
sortie n'est pas la lire.

---

## 15 septembre 2026, suite — les widgets passent à gauche, la Console se centre

Une demande d'une ligne, et la bonne : le volet des widgets à venir quitte le
bord droit pour **un tiroir à lui, au bord gauche** ; la Console reste à
droite, **centrée verticalement**.

**Pourquoi c'est mieux que la colonne d'avant.** Les deux volets empilés au
même bord se lisaient comme une seule chose en deux morceaux, alors qu'ils
n'ont rien à voir l'un avec l'autre. Et la Console, poussée vers le bas par un
volet vide qui prenait toute la hauteur restante, n'était centrée sur rien :
elle flottait entre un vide en haut et le coin en bas.

**Ce qui a changé dans le code.** `tiroir.c` ne connaît plus un tiroir mais un
**côté** : une structure `Cote` porte la lisière, le révélateur, la minuterie,
l'état, et **le signe du glissé** qui l'ouvre — `+1` vers la droite depuis le
bord gauche, `-1` vers la gauche depuis le bord droit. Deux instances, `G` et
`D`, et tout le reste est commun. L'API suit : `shell_tiroir_console_*`,
`shell_tiroir_widgets_*`, et `shell_tiroir_fermer()` qui **ferme les deux** —
c'est ce que demande la rangée d'alimentation avant d'éteindre, l'écran rendu,
pas un volet précis rentré.

**Une seule fenêtre pour les deux côtés, et c'est le point de conception.**
Deux nappes plein écran superposées se seraient disputé le clic extérieur, et
l'une des deux aurait fermé le mauvais tiroir. La fenêtre est donc partagée :
montrée dès qu'un côté s'ouvre, masquée quand le dernier est rentré — la
minuterie de retrait relit l'état des deux avant de masquer, sans quoi fermer
un volet escamoterait l'autre.

**Conséquence assumée, et vérifiée au banc : tant qu'un volet est dehors,
l'autre bord n'ouvre rien.** Les lisières sont créées avant la fenêtre du
tiroir, donc sous elle ; se poser contre le bord opposé ne touche que la
nappe. Le geste qui suit l'ouverture d'un tiroir est presque toujours de le
refermer.

**Le centrage de la Console ne demande pas de marge basse.** L'ancienne
colonne s'arrêtait 180 px avant le bas pour ne pas passer devant le coin. Le
volet droit fait maintenant **sa seule hauteur** — 459 px mesurés sur 1080,
centre à 540 exactement — et s'arrête à 769 px : le coin, qui commence vers
940, reste dégagé. Il faudrait 340 px de contenu en plus pour que la question
se pose.

**Un banc pour les deux tiroirs.** `shell/essais/banc-tiroirs.sh`, sur le
modèle de `banc-dock.sh` : labwc sans écran, bus jetable, pointeur virtuel, et
huit étapes lues **dans le journal** — pointeur posé à gauche, clic à côté,
glissé depuis le bord, pointeur posé à droite, bord opposé sans effet. Une
capture montre un volet sorti ; elle ne dit pas lequel des deux côtés l'a
décidé. D'où les deux lignes `g_debug ("tiroir %s : %s")`, sur le modèle de
« visibilité : » dans le dock.

**Au passage, le pointeur virtuel du banc ne compilait plus.** `pointeur.c`
appelle `nanosleep()`, qui n'est pas dans le C11 nu ; le banc a été aligné sur
`-std=c11` après coup, et depuis, `gcc` refusait la déclaration implicite.
Personne ne s'en était aperçu parce que le binaire déjà construit traînait
dans `essais/build/`. Un `#define _POSIX_C_SOURCE 200809L` en tête, et la
remarque de `construire.sh` — « un banc qui ne compile pas comme la cible ne
prouve rien » — vaut aussi pour le banc lui-même.

---

## 15 septembre 2026, suite — le doigt manquait la lisière, et un doigt virtuel l'a montré

Le volet de gauche ne s'ouvrait pas au glissé du doigt. Signalé d'usage, le
jour même de sa mise en service.

**Trois fausses pistes, toutes de ma main, et toutes du même genre : le banc
se sabotait lui-même.**

1. J'ai cru la lisière gauche **sourde** : au pointeur virtuel, elle ne
   répondait pas. Elle répondait très bien — le volet DROIT était resté
   ouvert, et sa nappe plein écran couvre le bord gauche. C'est le
   comportement documenté la veille, retourné contre son auteur.
2. J'ai cru l'avoir confirmé en relançant la barre : le pointeur était resté
   garé contre le bord droit, le volet droit s'est rouvert tout seul à la
   première image, et j'ai rejoué le même piège.
3. J'ai enfin cru le bord droit sourd à son tour : mes essais refermaient en
   **cliquant sur le fond d'écran**, ce qui ouvre le menu racine de labwc, qui
   capte tout ce qui suit. `essais/pointeur.c` le dit en tête, en toutes
   lettres, depuis le 11 septembre. Il a fallu le relire.

**Puis la mesure, et elle a été nette.** Un `uinput` monté en écran tactile
(`shell/essais/doigt.py`) fabrique un contact qui traverse libinput et le
`touch.c` de labwc — le vrai chemin du doigt, celui que le pointeur virtuel
n'emprunte pas. Balayage du point de premier contact :

| premier contact | volet gauche | | premier contact | volet droit |
|---|---|---|---|---|
| 0, 2, 6, 9 px | ouvre | | 1919, 1915, 1910 | ouvre |
| 11, 14, 20, 30 px | **rien** | | 1908, 1900, 1890 | **rien** |

La lisière faisait **10 px**, et elle n'ouvrait que si le contact y tombait.
Symétrique, donc : rien de propre au bord gauche. Ce qui diffère, c'est le
geste — le bord **bas** se prend perpendiculairement, en butant contre le
châssis, les bords latéraux se prennent en biais, et la dalle rapporte la pose
une trame après le contact : un doigt qui entre vite a déjà parcouru dix à
vingt pixels quand sa position arrive.

**Le correctif : 24 px pour le doigt, 10 px pour la pose du pointeur.** La
lisière s'élargit à 24 px — le contact peut atterrir n'importe où dedans —,
mais l'ouverture au pointeur immobilisé reste bornée à 10 px du bord
(`POSE_PX`). Une souris posée sur la bordure d'une fenêtre ne doit pas faire
sortir un volet au bout d'une seconde ; le doigt est imprécis, le pointeur ne
l'est pas. Vérifié sur la session : au doigt 0→23 px ouvre, 26 px non ; au
pointeur 4 et 9 px ouvrent, 14 et 20 px non — et les mêmes chiffres, en
miroir, à droite.

**Ce que cela coûte, et c'est dit :** les applications ne reçoivent plus ni
contact ni clic dans les 24 premiers pixels de gauche et de droite. 3,9 mm sur
cette dalle. Une ligne à changer pour le rendre.

**Et un piège de plus, payé sur le doigt virtuel lui-même :** `struct.pack`
écrit « l » sur **quatre** octets en mode standard, si bien que mes
`input_event` faisaient 16 octets là où le noyau en attend 24. Le noyau
refusait par `EINVAL`, sans un mot de plus. C'est « q » qu'il faut.

**Le banc gagne une étape** — pointeur posé à 16 px, qui ne doit rien
ouvrir — et passe neuf étapes sur neuf.

---

## Ce qui reste à faire — au 10 septembre 2026

Par ordre d'importance.

> **AU 14 SEPTEMBRE 2026, DEUX DE CES POINTS SONT CADUCS.** Le point 2
> (« la reprise après suspension est cassée ») reposait sur un diagnostic
> faux : elle fonctionne, et ce qu'on lui imputait était des morts par
> batterie vide. Le point 3 (« la veille profonde n'est pas réalisable »)
> l'était aussi : elle a été éprouvée le 14 septembre. Voir la séance de
> ce jour. Ils sont laissés ici tels quels — une liste qu'on récrit après
> coup ne montre plus comment on s'est trompé.

1. **Le clavier tactile du verrou.** Il n'y en a pas. En mode tablette,
   capot replié, il faut le clavier physique pour déverrouiller — ce qui
   est exactement la situation où l'on n'en a pas. `clavier.c` existe et
   sert l'écran de connexion, mais il est en GTK4 : le verrou étant en
   Wayland brut, il faudra soit redessiner le pavé en cairo avec
   `wl_touch`/`wl_pointer`, soit revoir le choix de protocole. **C'est le
   premier chantier.**

2. **La reprise après suspension est cassée.** Onze suspensions
   consécutives sans reprise le 9 septembre : le journal s'arrête sur
   `PM: suspend entry (s2idle)` et la machine réapparaît avec un nouvel
   identifiant de démarrage. Fermer le capot coûte une session. Tant que ce
   n'est pas réglé, `energie.suspendre_permis` reste à `false` et l'étage
   « suspendre » est écrit mais fermé. Piste **non vérifiée** : `rtw88` a un
   historique de problèmes de reprise en s2idle.

3. **La veille profonde** — l'hibernation — n'est pas réalisable en l'état :
   swap réel de 3,0 Gio pour 3,7 Gio de RAM, et **`resume=` absent** de la
   ligne de commande noyau. Hiberner aujourd'hui perdrait la session.
   Décision à prendre : agrandir le swap (fichier de 5 Gio sur les 45 libres,
   simple et sans risque), ou renoncer et appeler « veille profonde » une
   suspension normale avec le Wi-Fi coupé.

4. **Le panneau Réglages affiche les durées brutes**, pas celles réellement
   appliquées après bornage par `shell_energie_delais_mode()`. Un fichier
   incohérent — `attenuer 3 min, eteindre 1 min`, trouvé dans le vrai
   `shell.conf` — est corrigé silencieusement par le module, mais le panneau
   montre encore la valeur écrite. La Console, elle, dit vrai.

5. **`console.c` n'est pas converti à `retroeclairage.c`.** Le pilotage du
   rétroéclairage y est encore soudé au widget du curseur : deux
   implémentations qui écrivent tour à tour dans le même fichier finiront
   par se contredire.

6. **Le verrou ne se déclenche pas à la fermeture du capot.** Il s'ancre sur
   l'extinction de l'écran. Le capot reste géré par logind, donc identique
   pour les trois modes — alors que la spécification demandée distingue
   Travail (rester éveillé, avec un bip) des deux autres.

7. **Reports** : rclone (Drive, OneDrive), icônes sur le bureau, luminosité
   par inactivité déjà faite mais sans capteur de luminosité ambiante —
   celui-ci est **absent de MADOO**, mesuré, la question est close.

8. **Fichiers et le dock.** L'app_id `os.claude.shell.fichiers` ne
   correspond pas à `claude-os-fichiers.desktop` : l'icône épinglée ne
   montre jamais Fichiers comme ouvert, et une seconde entrée apparaît. Même
   défaut probable pour les Réglages. Voir la séance du 10 septembre.
