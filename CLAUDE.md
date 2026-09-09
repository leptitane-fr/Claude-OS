# Claude OS — à lire avant toute intervention

Ce fichier est chargé automatiquement à l'ouverture d'une session. Il dit
**où en est le projet** et **ce qu'on ne casse jamais**. Le détail est dans
[`docs/`](docs/) ; ici, l'essentiel pour reprendre la main sans dommage.

---

## Où en est le projet — 9 septembre 2026

**L'écran de connexion a été repris le 9 septembre 2026** : il suit enfin le
thème de la session, accepte un code PIN à six chiffres, et porte deux
claviers à l'écran pour le mode tablette. Compilé, déployé, coffre éprouvé
par sa socket — **mais jamais encore ouvert au code PIN sur le vrai écran**.
Voir la section dédiée plus bas et [`docs/09`](docs/09-code-pin.md).

**Le bureau est en service et harmonisé.** Firmware UEFI flashé, Debian 13
installée, `greetd` ouvre l'écran de connexion Claude OS, le mot de passe est
accepté et la session labwc s'ouvre avec le dock, la Console et le lanceur.
Les fenêtres portent des boutons réduire/agrandir, et le thème sort du shell
pour atteindre Chromium, Claude Desktop, le terminal et les barres de titre.

La machine est un **HP Chromebook x360 14b-cb0000sf**, board `MADOO`,
Pentium Silver N6000, **4 Go de RAM soudée**. Compte utilisateur : `stef`.

**SSH n'est plus lancé au démarrage** (8 septembre 2026). Il écoutait sur
`0.0.0.0:22` en permanence pour un usage occasionnel. Le filet de secours est
désormais une **entrée du menu de démarrage** — « Claude OS — dépannage réseau
(SSH activé) » — qui ajoute `systemd.wants=ssh.service` à la ligne de commande
noyau. C'est un effet en mémoire, dans `/run` : le démarrage suivant repart
sans SSH, sans rien à défaire. Générateur : `/etc/grub.d/11_claude-os-ssh`,
source dans `rootfs/`.

**`--deployer` ne suffit pas pour ce fichier** : il copie `rootfs/` vers `/`,
il ne régénère pas le menu. Après tout déploiement qui touche
`etc/grub.d/` ou `etc/default/grub.d/`, il faut `sudo update-grub` — sans
quoi le menu reste celui d'avant, en silence. **Cette entrée n'a pas encore été démarrée pour de
vrai** — la valider une fois pendant que le bureau fonctionne, pas le jour où
il faudra s'en servir.

Versions constatées sur la machine : labwc 0.8.3, greetd 0.10.3,
xwayland 2:24.1.6, libgtk4-layer-shell0 1.0.4, dbus-user-session 1.16.2.

Le fil chronologique complet — ce qui a été fait, dans quel ordre, et ce qui a
été mesuré — est dans
[`docs/07`](docs/07-journal-des-seances.md).

### Confirmé à l'écran le 8 septembre 2026

Deux corrections écrites ce jour-là ont été **vues fonctionner sur MADOO**,
pas seulement au banc d'essai :

| Confirmé | Ce qui a été constaté |
|---|---|
| **Thème global** | La bascule clair/sombre des Réglages est suivie par **toutes** les applications — Chromium, Claude Desktop, le terminal, les barres de titre — sans qu'aucune soit relancée. |
| **Luminosité** | Le curseur de la Console commande l'écran immédiatement, par logind, sans appartenance au groupe `video` ni réouverture de session. |

### La veille progressive de l'écran

Écrite le 9 septembre 2026, et **vue fonctionner sur MADOO** : l'écran
descend à 30 % après le délai, et remonte dès qu'on touche le pavé tactile.

Le rétroéclairage est le premier poste de consommation de la machine —
environ 1 à 2 W sur les 6,8 W mesurés. C'est le plus gros levier
d'autonomie restant, et il est entièrement logiciel.

`shell/src/energie.c` vit dans `claude-os-status`, comme le centre de
notifications et pour la même raison : un processus séparé aurait coûté un
runtime GTK4 complet, ~40 Mo mesurés, sur 4 Go soudés. La barre est déjà
résidente, déjà cliente Wayland, et tient déjà la connexion logind.

Deux profils, choisis sur la prise, forçables d'un clic dans la Console
(rangée « Veille de l'écran ») :

| | Secteur — « Normal » | Batterie — « Économe » |
|---|---|---|
| Atténuer à 30 % | 3 min | 1 min |
| Éteindre | 10 min | 3 min |
| Suspendre | jamais | 10 min, **verrouillé** |

**Aucune scrutation.** `ext_idle_notifier_v1` prévient ; entre deux
événements le module ne coûte rien. Les trois signaux qui affinent la
décision — charge CPU, son en lecture, source d'alimentation — sont lus une
seule fois, au moment où un étage va se déclencher. La source est
réévaluée sur `resumed`, donc à chaque interaction : ni scrutation ni
UPower.

**Les inhibiteurs sont gratuits.** La spécification impose au compositeur
de ne pas rendre la notification inactive tant qu'un
`zwp_idle_inhibitor_v1` existe sur une surface visible. Un lecteur vidéo
n'est jamais interrompu, sans une ligne de code. Vérifié dans les deux
sens : aucun événement tant que Claude Desktop tenait son verrou d'éveil,
les trois étages à l'heure dès qu'il l'a relâché.

**TROIS DÉFAILLANCES MUETTES ont coûté une séance entière**, toutes du même
genre, toutes corrigées. L'invariant n°4 ne parle pas que des `>/dev/null` :
il vaut aussi pour un `return` anticipé et pour un appel D-Bus asynchrone.

- Deux `return` silencieux dans `shell_energie_init` : sans affichage
  Wayland ou sans siège, le module renonçait sans un mot.
- **Aucun `wl_display_flush`** après `get_idle_notification`. L'init a lieu
  avant que la boucle GTK ne tourne ; les requêtes restaient dans la file.
  Sur un bureau au repos — la situation même qu'on veut détecter — rien ne
  la vidait. *Le module de veille ne démarrait qu'une fois qu'il se passait
  quelque chose.*
- `g_dbus_proxy_call` avec `NULL` comme rappel, au motif que « personne ne
  regarde ». Le plus coûteux des trois.

**Et un piège de méthode, à retenir absolument :**

```
labwc, claude-os-fond   → session-N.scope   (seat0, tty7)
claude-desktop, et tout ce qui en est lancé
                        → user@1000.service/app.slice/app-com.anthropic.Claude
```

**Claude Desktop vit hors de la session du siège.** Or `logind` n'accepte
`SetBrightness` que de la session active. Un composant relancé à la main
depuis un terminal de Claude Desktop se le voit refuser — et le module
paraît cassé alors qu'il ne l'est pas. **Un composant qui touche à logind
ne se teste QUE démarré par l'autostart de labwc**, c'est-à-dire après une
réouverture de session. Vérifier la portée avant de conclure :

```sh
grep -oE 'session-[0-9]+\.scope|app-com[^/]*\.scope' /proc/$(pgrep -x claude-os-statu)/cgroup
```

### Le panneau Réglages, en volets

Restructuré le 9 septembre 2026. Il n'était dessiné que pour l'interface :
une colonne de cartes dans un défilement, sans navigation. Il porte
désormais une barre latérale et une `GtkStack`.

**Ajouter une section, c'est ajouter une fonction et une ligne de table** —
`VOLETS[]` dans `settings.c`. Ni bouton à câbler, ni page à nommer deux
fois. Chaque fabrique renvoie le *contenu* du volet ; `on_activate`
enveloppe dans le défilement, pour que toutes les sections défilent pareil.

`claude-os-reglages --volet=energie` ouvre directement une section.
Proposer un réglage puis obliger à le chercher dans une liste est une façon
sûre de le rendre introuvable.

Le volet **Énergie** n'expose que des durées et un niveau, jamais l'ordre
des étages : atténuer → éteindre → suspendre est ce que le module sait
faire, et l'ouvrir inviterait à fabriquer des combinaisons sans
signification. Listes de durées et non champs libres — « 90 » saisi dans
une case ne dit pas s'il s'agit de secondes ou de minutes.

### Les lecteurs réseau

Écrits le 9 septembre 2026, et **vus fonctionner sur MADOO** contre le NAS
`WDMYCLOUDMIRROR` du réseau local : partage monté, parcouru en lecture et en
écriture, démonté.

Déclarés dans **Réglages › Lecteurs réseau**, connectés d'un clic dans le
volet latéral de **Fichiers**. Une fois connecté, le partage est un dossier
ordinaire — le terminal, Chromium et ses boîtes « Enregistrer sous » le
voient aussi. Détail complet dans [`docs/08`](docs/08-lecteurs-reseau.md).

**Montages du noyau, et non gvfs.** Mesuré : `gvfs-backends` demande
43 paquets — MTP, gphoto2, iOS, codecs AV1 — contre 19 pour les quatre
protocoles par le noyau, et surtout un montage gvfs n'existe que pour les
applications GIO.

**L'interface ne compose JAMAIS de commande privilégiée.** Elle dit un verbe
et un identifiant — `claude-os-lecteur monter nas-videos` — et tout le reste
est relu en root depuis la configuration, puis validé : identifiant restreint
à `[A-Za-z0-9._-]`, adresse refusée si elle contient autre chose qu'un hôte,
options filtrées par liste blanche, fichier INI lu en `awk` et jamais évalué.
**Le mot de passe ne passe jamais par la ligne de commande** — il serait dans
`ps` et dans `actions.log` ; il transite par un fichier `0600` dans
`/run/user/1000`, effacé aussitôt lu. Les mots de passe sont dans le
trousseau ; `~/.config/claude-os/lecteurs` ne contient rien de secret.

**TROIS USAGES APRÈS LIBÉRATION, tous trouvés en cliquant pour de vrai.**
`reconstruire()` relit le fichier, donc **libère** `L->lecteurs` : tout
pointeur qui en vient meurt là. `connecter_lecteur` s'en servait juste après
— `SIGSEGV` à chaque clic sur un lecteur non connecté. Deux jumeaux dans
`on_lecteur_fini` et `on_mdp_valide`. Aucun des trois ne se voit à la
compilation ni à la lecture ; `coredumpctl` a dit où.

**Et deux pièges de méthode :**

- **`g_file_query_exists` est synchrone.** Il dormait dans `naviguer()`
  depuis toujours, invisible sur un dossier local. Sur un serveur éteint, il
  gèle la fenêtre entière pendant le délai TCP — au moment précis où l'on
  veut cliquer « précédent ». Rendu asynchrone.
- **`/etc/default/rpcbind` ne décide rien.** `nfs-common` tire `rpcbind`, qui
  écoutait sur `0.0.0.0:111` — l'exposition retirée pour SSH la veille. Les
  `OPTIONS` de ce fichier n'y peuvent rien : rpcbind est activé par socket,
  et c'est systemd qui ouvre les ports. Il faut un `.socket.d/` dont la
  première ligne `ListenStream=` **vide remet la liste à zéro**, sinon les
  nouvelles adresses s'ajoutent aux publiques. Vérifié : `192.168.1.30:111`
  refuse, `127.0.0.1:111` répond. Contrepartie : `nolock` par défaut sur NFS.

**Seul SMB a été monté pour de vrai.** NFS, SFTP et WebDAV sont écrits et
compilés, jamais éprouvés — le NAS ne publie aucun export NFS et son port 22
est fermé. `docs/08` dit précisément ce qui reste non établi.

### L'écran de connexion — thème, code PIN, clavier tactile

Écrit le 9 septembre 2026. Détail complet dans
[`docs/09`](docs/09-code-pin.md).

**Vu à l'écran le 9 septembre 2026**, par `--essai` : le volet nom
d'utilisateur s'affiche, **sombre comme le bureau** — une première —, le
clavier azerty avec lui, et le journal montre le greeter joignant le coffre
depuis `_greetd` (`claude-os-coffre@7-10558-102`, PID du greeter). **Le piège
du focus est écarté, éprouvé au doigt** : les touches à l'écran écrivent, et
le clavier physique écrit toujours après. ⇧ et &# marchent. **Mais
aucune session n'a encore été ouverte avec ce greeter** : le chemin qui va
d'un mot de passe accepté à l'enrôlement puis au démarrage reste à parcourir
en entier.

**Le thème ne suivait pas, et le code censé s'en charger ne POUVAIT pas
marcher.** Trois défaillances muettes empilées, l'invariant n°4 en toutes
lettres : `theme_de()` lisait `~/.config/claude-os/shell.conf` alors que
`/home/stef` est en 0700 et que le greeter tourne sous `_greetd` ; le repli
était `claude-sombre`, un thème que la machine n'utilise pas ; et `cfg->theme`
était écrasé sans repasser par `theme_par_id()`, donc `cfg->dark` restait
faux et les widgets natifs se dessinaient clairs sur fond sombre.

Désormais : le coffre lit `shell.conf` en root et le sert au greeter —
`shell.conf` reste la source unique de vérité, aucun fichier miroir à tenir —
`shell_config_set_theme()` pose `theme` et `dark` ensemble, et **tout repli
est écrit** dans `/var/log/claude-os-connexion.log`.

**Le code PIN déverrouille le mot de passe, il ne le remplace pas.** C'est
`pam_gnome_keyring.so`, dans `/etc/pam.d/greetd`, qui l'impose : PAM doit
recevoir le VRAI mot de passe, sinon le trousseau reste fermé et les lecteurs
réseau deviennent inaccessibles sans le moindre message. Un module PAM maison
était donc exclu d'emblée.

`claude-os-coffre` garde le mot de passe scellé par **Argon2id
(128 Mio, t=3, p=1 — 0,63 s mesurées sur MADOO) puis AES-256-GCM**. Cinq
essais faux et le coffre s'efface.

**Ce qui le protège est la socket, et rien d'autre** : `SocketUser=_greetd`,
`SocketMode=0600`, `Accept=yes`. Pas de setuid, pas de sudoers. Le programme
redit la règle par `SO_PEERCRED`, pour qu'une unité systemd remplacée ne
suffise pas. `MaxConnections=4` n'est pas décoratif : 129,5 Mio de pic mesurés
par instance, et la valeur par défaut de systemd est 64.

**Ce que cela coûte, et il faut le savoir :** un code à six chiffres n'a qu'un
million de combinaisons et le disque n'est pas chiffré. Qui démonte l'eMMC
peut attaquer le coffre hors ligne — 1,8 jour sur les quatre cœurs de la
machine, quelques heures sur du matériel récent — là où `/etc/shadow` ne lui
donnerait rien. **Le PIN abaisse la sécurité au repos et l'améliore à
l'usage.** Arbitrage assumé, réversible d'un clic. `docs/09` fait le calcul.

**Trois pièges payés :**

- **`gcry_kdf_derive()` ne sait pas faire Argon2**, malgré son nom : « Invalid
  value », sans plus. Il faut l'API à poignée, dont l'ordre des paramètres
  n'est documenté nulle part — `{taglen, t, m, p}`, **établi contre le
  vecteur de test de la RFC 9106 §5.3**, pas supposé.
- **`g_printerr` transcode vers la locale**, et un service systemd n'en a pas :
  tous les accents du journal ressortaient en « ? ». `fputs` sur `stderr`.
- **Un `GtkButton` vole le focus au clic.** Sans
  `set_can_focus(FALSE)` ET `set_focus_on_click(FALSE)` sur chaque touche, le
  premier appui à l'écran coupe la frappe physique — silencieusement, et
  seulement après un clic, donc jamais au premier essai.

**Ne jamais retirer les sorties de secours.** Le mot de passe reste
atteignable depuis tous les volets, le PIN se retire depuis l'écran, et un
coffre muet fait retomber sur le mot de passe avec la raison affichée. Un
enrôlement raté n'empêche jamais la session de s'ouvrir : il part au journal,
le PIN est abandonné, et l'écran en repropose un à l'ouverture suivante.

Aperçu sans rien toucher :
`claude-os-connexion --apercu --volet=pin --theme=clair` (`nom`, `mdp`,
`pin`, `choix`). **Ctrl-Q pour en sortir** — l'aperçu prend l'écran entier et
le clavier en exclusif, comme le vrai écran ; ni Alt-Tab ni Alt-F4 n'en
sortent. Ce raccourci n'existe qu'en aperçu.

### Le centre de notifications

Écrit le 8 septembre 2026. La machine n'avait **aucun** démon : ni dunst, ni
mako, ni notification-daemon, et `NameHasOwner org.freedesktop.Notifications`
répondait `false`. Chromium et Claude Desktop émettaient donc dans le vide,
sans erreur visible.

`shell/src/notifications.c` **est** le serveur, pas seulement l'affichage : il
prend le nom sur le bus de session et implémente les quatre méthodes de la
spécification freedesktop plus ses deux signaux. Toute application capable de
notifier passe par là, sans rien à configurer de son côté.

Il vit dans `claude-os-status` parce que la cloche est dans la barre et que le
centre doit s'aligner sur la Console : un processus séparé aurait demandé un
protocole pour transporter une hauteur en pixels. Contrepartie assumée — si la
barre d'état tombe, les notifications tombent avec elle.

Trois pièges payés, tous documentés dans le code :

- un **popover s'ouvre vers le bas** par défaut ; né au ras de l'écran, GTK le
  retournait, et ce retournement annulait le décalage vertical. `GTK_POS_TOP`
  est obligatoire ici.
- `gtk_icon_theme_add_search_path()` **ignore un répertoire sans
  `index.theme`** : l'icône était installée, bien formée, et introuvable.
- un **commentaire XML placé avant `<svg>`** repousse la balise hors de la
  fenêtre de détection de format de gdk-pixbuf : « Format d'image non
  reconnu » sur un fichier parfaitement valide.

### Les barres de titre

Uniformisées le 8 septembre 2026. Presque toutes les fenêtres portaient déjà
la barre de labwc — nos applications GTK4 (elles n'ont pas de `GtkHeaderBar`,
GTK ne dessine donc rien et le compositeur décore), Claude Desktop, les
dialogues GTK. Seuls les **boutons** restaient ceux de labwc : des masques XBM
1 bit, fins, monochromes et visiblement crénelés.

Les images de boutons **ne se surchargent pas** : labwc les cherche dans le
répertoire du thème. `themerc-override` ne pouvait donc rien y faire. D'où un
thème à nous, `rootfs/usr/share/themes/Claude-OS/labwc/`, désigné par
`<theme><name>` dans `rc.xml`. Le `themerc-override` continue de primer pour
les couleurs, que `claude-os-theme` réécrit à chaque bascule clair/sombre.

Deux limites mesurées :

- **`titlebar.height` n'existe plus** en labwc 0.8.3 : il répond
  « no longer supported » dans le journal. La hauteur se règle par
  `window.titlebar.padding.height`.
- **`labwc --reconfigure` ne recharge pas les images de boutons.** Les
  couleurs suivent, les pastilles non : elles n'apparaissent qu'à la
  **prochaine ouverture de session**. Ne pas conclure que le thème est cassé.

**Chromium** dessinait son propre cadre — onglets dans la barre de titre, un
seul `×`, ni réduire ni agrandir. `browser.custom_chrome_frame` est passé à
`false` dans son profil pour qu'il prenne la barre du système. C'est un
réglage de goût, réversible d'un clic droit sur la bande d'onglets
(« Utiliser la barre de titre et les bordures du système »), au prix d'une
barre de 34 px au-dessus des onglets.

### Ce qui reste ouvert

| Sujet | État |
|---|---|
| **Audio** | **Réparé le 8 septembre 2026**, cette ligne ne décrit plus la machine. Le `probe failed with error -22` a disparu des journaux, PipeWire énumère cinq sorties dont « Jasper Lake HD Audio » par défaut, et les touches de volume la commandent — confirmé à l'oreille. L'historique de la panne reste dans `docs/07`. |
| Affichage au démarrage | L'écran restait noir jusqu'à ce qu'on touche le pavé tactile. Probablement le conflit de terminal virtuel de l'invariant n°5 — **à reconfirmer** maintenant que greetd est sur le tty7, et à ne pas déclarer résolu sans l'avoir revu. |
| Luminosité automatique | **Impossible par capteur — mesuré le 8 septembre 2026.** Aucun capteur de luminosité ambiante sur MADOO : `/sys/bus/iio/devices/` n'expose que deux accéléromètres, un gyroscope et un angle d'écran. Question close. **L'asservissement à l'inactivité, lui, est FAIT et VU FONCTIONNER** le 9 septembre — voir la veille progressive ci-dessus. |
| **Reprise après suspension** | **CASSÉE, et c'est le chantier le plus important qui reste.** Le 9 septembre 2026, onze suspensions consécutives déclenchées par la fermeture du capot n'ont jamais repris : le journal s'arrête net sur `PM: suspend entry (s2idle)` et la machine réapparaît avec un nouvel identifiant de démarrage. Fermer le capot coûte donc une session. Une seule reprise a réussi, le matin du 9. Piste **non vérifiée** : `rtw88` a un historique de problèmes de reprise en s2idle, et le journal montre NetworkManager libérant `wlp1s0` juste avant. Tant que ce point n'est pas réglé, `energie.suspendre_permis` reste à `false`. |
| **Clavier tactile du verrou** | **ABSENT — premier chantier.** En mode tablette il faut le clavier physique pour déverrouiller, ce qui est exactement la situation où l'on n'en a pas. `clavier.c` existe mais est en GTK4, et le verrou est en Wayland brut : soit redessiner le pavé en cairo avec `wl_touch`, soit revoir le choix de protocole. |
| **Veille profonde** | Irréalisable en l'état : swap réel 3,0 Gio pour 3,7 Gio de RAM, et `resume=` absent de la ligne de commande. Hiberner perdrait la session. Décision à prendre — agrandir le swap, ou renoncer au nom. |
| Réglages : durées brutes | Le panneau montre ce qui est écrit, pas ce qui est appliqué après bornage par `shell_energie_delais_mode()`. La Console, elle, dit vrai. |
| `console.c` non converti | Le rétroéclairage y est encore soudé au widget du curseur, en double de `retroeclairage.c`. |
| Capot par mode | Le verrou s'ancre sur l'extinction ; le capot reste géré par logind, donc identique pour les trois modes. |
| Reports | rclone (Drive, OneDrive), icônes sur le bureau. |

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

**Faux positif connu : un `git rebase` déclenche « PÉRIMÉ » à tort.** Un
rebase réécrit les fichiers du répertoire de travail, donc leur date, sans
changer une ligne de leur contenu. Le 8 septembre 2026, un rebase à 17:47 a
fait déclarer périmés six fichiers compilés à 17:12 — le code installé était
pourtant le bon. Avant de conclure, comparer le `git reflog` aux dates : si
un rebase tombe entre la compilation et le contrôle, l'alerte ne dit rien.
Recompiler reste sans danger, mais ne cherchez pas une régression qui
n'existe pas.

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
| Code PIN non proposé, ou thème faux à la connexion | `journalctl -u 'claude-os-coffre@*' -b` |
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
