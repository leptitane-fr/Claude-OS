# Claude-OS Shell

Interface graphique sur-mesure : dock central, barre d'état, lanceur.
Inspiration ChromeOS pour les surfaces et les couleurs, dock centré façon
macOS, informations groupées en bas à droite.

## Ce qui justifie d'écrire un shell plutôt que d'habiller Xfce

Les bureaux existants sont lourds parce qu'ils sont **généraux** : ils
gèrent des configurations, des protocoles et des cas d'usage que cette
machine ne rencontrera jamais. En n'écrivant que le nécessaire, l'objectif
de 20 à 30 Mo pour l'ensemble du shell est atteignable, contre environ
120 Mo pour la session Xfce complète.

Habiller Xfce aurait coûté presque autant d'efforts pour un résultat
contraint par ses composants : son tableau de bord ne sait pas faire un
dock centré avec agrandissement au survol, et son moteur de thème date
de GTK2.

## Choix techniques

| Couche | Choix | Raison |
|---|---|---|
| Compositeur | **labwc** (Wayland, wlroots) | ~15 Mo, fenêtrage flottant, prend en charge `layer-shell` |
| Composants | **GTK4 + CSS, en C** | rendu GPU, style entièrement en CSS, déjà présent sur le système |
| Ancrage | **gtk4-layer-shell** | seul moyen d'ancrer un dock hors du flux des fenêtres |

**Pas de libadwaita** : elle imposerait son propre langage visuel, que nous
remplacerions intégralement. GTK4 nu suffit et pèse moins.

## La contrainte qui gouverne tout : l'énergie

Cette distribution existe pour l'autonomie — 3,05 W au repos mesurés. Une
interface qui réveille le GPU en permanence ruinerait ce résultat.

Règles suivies sans exception :

- **Aucune animation au repos.** Les transitions se déclenchent sur action
  et durent moins de 200 ms.
- **Aucun flou en direct.** `backdrop-filter` impose un recalcul à chaque
  image ; la translucidité se fait par alpha simple, composé une fois.
- **Une seule minuterie pour toute la barre d'état.** Alignée sur la
  seconde 0 de chaque minute, elle met à jour l'heure *et* la batterie :
  un réveil par minute, pas un par seconde ni un par élément. La batterie
  est lue directement dans `sysfs`, sans le démon UPower — un processus de
  plus en mémoire pour une valeur qui tient dans deux fichiers texte ne se
  justifie pas.
- **Les watts ne sont mesurés que panneau ouvert.** La seule minuterie
  périodique du panneau démarre à son ouverture et s'arrête à sa
  fermeture.
- **Le réseau ne consulte rien.** Il réagit aux signaux D-Bus de
  NetworkManager, donc uniquement quand l'état change réellement.
- **Aucun processus qui tourne pour rien.** Le lanceur ne réside pas en
  mémoire : il est lancé à la demande et se termine à la fermeture.

Une interface immobile doit consommer exactement zéro.

## Structure

```
shell/
  style/theme-*.css    une palette par thème — la seule source de couleurs
  style/shell.css      règles communes, en jetons uniquement
  src/dock.c           dock central                        (fait)
  src/status.c         barre d'état bas-droite             (fait)
  src/panel.c          réglages rapides, ouverts au clic   (fait)
  src/config.c         shell.conf + styles, partagés       (fait)
  src/visibility.c     bascule dock + barre au clavier     (fait)
  src/sysfs.c          lecture batterie, partagée          (fait)
  src/launcher.c       lanceur d'applications              (à venir)
  test-render.sh       banc d'essai visuel sans écran
```

## Configuration

`~/.config/claude-os/shell.conf`, absent par défaut — le shell fonctionne
sans qu'aucun fichier n'ait été écrit.

```ini
[dock]
pinned=chromium;claude-desktop;thunar;xfce4-terminal;claude-os-reglages
reserve_space=false  ; true : les fenêtres maximisées s'arrêtent au-dessus

[appearance]
font=Inter
icon_theme=Papirus
theme=light          ; ou dark

[wallpaper]
image=               ; vide : le dégradé dessiné par le shell
fill=true            ; false : image entière, dégradé sur les côtés
```

Tout cela se règle dans le panneau de réglages ; le fichier reste modifiable
à la main, et les composants le relisent aussitôt dans les deux cas.

Le dock **et** la barre d'état lisent ce fichier, par le même code
(`config.c`), et le **relisent à chaud** : chaque composant surveille
`shell.conf` et se réapplique dès qu'il change. C'est ce qui permet à un
réglage d'agir sur un shell déjà lancé sans protocole à inventer — la
configuration reste la seule source de vérité.

Deux détails qui ne s'improvisent pas. La surveillance déclare
`G_FILE_MONITOR_WATCH_MOVES` : un enregistrement atomique remplace le fichier
par un autre, et sans cet indicateur la surveillance suivrait l'ancien inode
sans plus jamais rien voir. Et les événements sont regroupés sur 120 ms, un
seul enregistrement en produisant trois — création du temporaire,
déplacement, attributs.

L'écriture relit le fichier avant de le modifier clé par clé : commentaires
et réglages inconnus de cette version survivent. Réécrire de zéro les
perdrait au premier enregistrement. Quand seul le dock le lisait, un `theme=dark` donnait un dock
sombre et une barre claire côte à côte.

Le thème d'icônes n'est pas un détail : **Adwaita a abandonné les noms
hérités** (`web-browser`, `utilities-terminal`, `system-file-manager`) que
la plupart des fichiers `.desktop` déclarent encore, et affiche un
pictogramme générique à leur place. Constaté à l'écran, pas supposé.
Papirus les conserve.

## Réglages rapides : tout au clic, rien au repos

La barre d'état ne montre que l'heure, l'état de la connexion et le niveau
de batterie. Le reste — bascules Wi-Fi et Bluetooth, consommation
instantanée — n'apparaît qu'au clic sur la barre, dans un panneau qui se
referme aussitôt. Une information permanente qu'on ne consulte pas est du
bruit, et une valeur rafraîchie en permanence est de l'énergie perdue.

C'est aussi ce qui rend la mesure de consommation acceptable : **la
minuterie des watts ne tourne que pendant que le panneau est ouvert.**
Elle démarre sur le signal `show` du popover et s'arrête sur `closed`.

### La consommation en watts

Deux conventions coexistent dans `sysfs` selon le pilote ACPI :

| Fichiers | Unité | Calcul |
|---|---|---|
| `power_now` | µW | ÷ 10⁶ |
| `current_now` × `voltage_now` | µA × µV | ÷ 10¹² |

Ce Vivobook expose la seconde. Mesure de référence relevée sur la machine :
0,247 A × 12,363 V = **3,05 W** au repos, écran allumé. Le signe de
`current_now` n'étant pas normalisé entre pilotes, on prend la valeur
absolue et c'est `status` qui donne le sens.

### Choisir un réseau, connecter un appareil

Chaque bascule porte un **chevron** : le corps allume et éteint, le chevron
ouvre la liste. Les pages détaillées vivent dans le **même popover**, dans un
`GtkStack` — ouvrir une fenêtre séparée pour choisir un réseau ferait perdre
le fil.

La page Wi-Fi liste les réseaux visibles, dédoublonnés par SSID en gardant la
borne la plus forte. Un clic active le profil enregistré (`ActivateConnection`
avec `"/"` comme connexion : NetworkManager choisit lui-même le profil qui
convient au couple carte + borne) ; pour un réseau protégé, un champ de mot
de passe se déplie en même temps, prêt si le profil n'existait pas.

Rien ne tourne quand une page est fermée : le balayage Wi-Fi part à
l'ouverture, la découverte Bluetooth s'arrête à la fermeture — c'est le poste
de consommation le plus lourd de ces pages.

Le code est écrit contre les **fichiers d'introspection** de NetworkManager
1.52, récupérés chez freedesktop : noms de méthodes, signatures et propriétés
en sont copiés plutôt que cités de mémoire.

### Aucun appel D-Bus bloquant, jamais

C'est la leçon la plus chère de ce composant. `g_dbus_proxy_new_for_bus_sync`
**n'accepte aucun délai** et attend les vingt-cinq secondes réglementaires
quand le service tarde à répondre. Mesuré au banc d'essai contre un faux
NetworkManager muet : la barre d'état ne s'affichait pas du tout pendant ce
temps, alors même que sa fenêtre avait déjà été présentée.

Trois appels de ce type se trouvaient sur le chemin de démarrage — l'icône
réseau de la barre, la bascule Wi-Fi, la bascule Bluetooth — plus une
recherche d'adaptateur BlueZ sans borne. Tous sont désormais asynchrones ou
bornés à quelques secondes, et la lecture des réseaux l'est de bout en bout :
service → périphériques → carte → bornes, en comptant les réponses en attente
à chaque étage.

Les propriétés se lisent par `GetAll` et non une par une : vingt bornes
visibles font la différence entre vingt allers-retours et cent.

### Les bascules ne mentent pas

Un clic ne bascule pas l'affichage : il écrit la propriété
(`WirelessEnabled` chez NetworkManager, `Powered` sur l'adaptateur BlueZ)
et l'affichage suit le **signal renvoyé par le service**. Si polkit refuse
ou si un interrupteur matériel bloque la radio, la bascule reste
visiblement dans son état réel.

`WirelessHardwareEnabled` est lu en plus : quand il est faux, aucun
logiciel ne peut rallumer la radio, et la bascule est grisée plutôt que
simplement « désactivé ». Services absents : les deux bascules affichent
« Indisponible » et la carte batterie « Aucune batterie détectée ».

### Deux pièges rencontrés, tous deux mesurés

**Adwaita peint les boutons avec un `background-image`**, qui recouvre tout
`background-color` posé dessous. Sans `background: none` en préambule, les
deux pastilles restaient blanches quoi qu'on écrive ensuite.

**`g_variant_iter_loop` libère lui-même les valeurs qu'il a posées**, au
début de l'itération suivante. Un `g_autoptr` sur l'une d'elles les
libérerait une seconde fois — la recherche de l'adaptateur BlueZ utilise
donc des pointeurs nus, et ne libère à la main qu'en cas de sortie
anticipée.

## Masquage : ce que fait le compositeur, ce que fait le shell

Le partage est net, et il a été tranché par l'essai sur la machine.

**Le plein écran ne masque rien, et c'est le comportement voulu.** La fenêtre
occupe tout l'écran — une surface layer-shell ne réserve rien face à une
fenêtre plein écran, seules les fenêtres maximisées s'arrêtent au-dessus du
dock, mesuré à 1920×1200 alors que le dock en réserve 86 — et le dock reste
affiché par-dessus.

Choix pris après essai sur la machine : on garde l'heure et la batterie sous
les yeux pendant une vidéo ou une visio, et la touche Windows dégage l'écran
quand on le veut vraiment.

J'avais annoncé l'inverse, en lisant dans le code de labwc 0.8.3 une
désactivation de la couche `TOP` sous une fenêtre plein écran
(`desktop_update_top_layer_visibility`). Sur la machine, cela ne se produit
pas. Lire le code d'un compositeur dit ce qu'il contient, pas ce qu'il fait
dans une situation donnée.

**La bascule manuelle appartient au shell.** Le compositeur n'a aucun moyen
de masquer une surface layer-shell à la demande. Chaque composant publie
donc une action `basculer` sur le bus de session sous son identifiant
d'application, et `/usr/local/bin/claude-os-shell-basculer` les appelle
toutes les deux :

```sh
gapplication action os.claude.shell.dock   basculer
gapplication action os.claude.shell.status basculer
```

Pas de démon d'orchestration, pas de socket à nous, pas de chasse au numéro
de processus. Les deux appels sont indépendants : si un composant n'est pas
lancé, l'autre bascule quand même.

`g_application_hold()` est indispensable côté composant — sans lui, masquer
la seule fenêtre ferait sortir GApplication de sa boucle et le dock
disparaîtrait pour de bon au lieu de se cacher.

### La touche Windows : deux liaisons, et pourquoi

`rc.xml` fournit `Super_L` et `Super+Espace`. La première **fonctionne sur la
machine**, vérifié à l'usage.

Le doute venait d'une mesure locale, clavier virtuel à l'appui : labwc ne
déclenche `<keybind key="Super_L">` que si la touche arrive comme **touche
ordinaire**, pas comme **modificateur seul**. Sur un vrai clavier l'appui
produit les deux, et la liaison se déclenche — ce qu'aucun clavier virtuel ne
pouvait montrer. `Super+Espace` reste comme repli si la touche seule venait à
gêner d'autres raccourcis `Super`.

L'apparition est instantanée, sans animation : déplacer une surface
layer-shell demanderait un réveil par image pour quelques dixièmes de
seconde de mouvement.

## Zones réservées : le piège de l'empilement

**Ni le dock ni la barre ne réservent d'espace.** Le shell passe par-dessus
les fenêtres ; afficher ou masquer une surface ne doit jamais remettre en
page ce qu'il y a dessous.

Le dock réservait d'abord 86 px, pour que les fenêtres maximisées s'arrêtent
au-dessus de lui. Conséquence signalée à l'usage, puis reproduite ici :

| | fenêtre maximisée |
|---|---|
| dock affiché, `reserve_space=true` | 1920 × **1114** |
| dock masqué | 1920 × 1200 |
| dock affiché, défaut actuel | 1920 × 1200 |

La fenêtre se redimensionnait donc à chaque appui sur la touche Windows, et
la bande de fond d'écran laissée entre elle et le bas de l'écran se voyait
autour de la pilule. `reserve_space=true` dans `shell.conf` rétablit
l'ancien comportement pour qui préfère que rien ne passe sous le dock.

Les fenêtres **plein écran** n'ont jamais été concernées : leur géométrie se
calcule sur la résolution de l'écran et non sur la zone utile (labwc,
`view_apply_fullscreen_geometry`), mesuré à 1920 × 1200 dans les deux cas.
C'est bien d'une fenêtre maximisée qu'il s'agissait.

La barre d'état, elle, déclare `exclusive_zone = -1` : elle **ignore** les
zones réservées par les autres surfaces. Avec la valeur 0, elle se posait
au-dessus des 86 px que le dock réservait alors, et flottait plus haut que
lui — visible immédiatement sur une capture.

Les composants sont des exécutables **séparés**. Un défaut dans le lanceur
ne doit pas emporter le dock, et chacun peut être relancé isolément.

## Les thèmes

Quatre : **Clair**, **Sombre**, **Claude clair**, **Claude sombre**.

### Un fichier par thème, et rien d'autre à toucher

`shell.css` ne connaît que des **noms de jetons** — `@surface`, `@text`,
`@accent`, `@border`… — et jamais une couleur littérale. Chaque thème est un
fichier qui définit exactement les mêmes noms. **Ajouter un thème, c'est
ajouter un fichier et une ligne dans la table `shell_themes()`** : aucune
règle n'est à modifier.

C'est ce que la première version ne permettait pas. Les couleurs y étaient
écrites deux fois — `@l-surface` et `@d-surface` — et chaque règle avait son
doublon `window.dark`. Il y en avait 39. Passer à quatre thèmes aurait voulu
dire quatre écritures de chaque règle ; le refactor en a supprimé 39 et
raccourci la feuille de 431 à 331 lignes.

Le basculement recharge le seul fichier de jetons : GTK re-résout les
couleurs nommées des règles quand le fournisseur qui les définit change.
Vérifié dans les deux sens — le dock suit le thème sans que `shell.css` soit
relu.

Chaque thème fixe aussi **sa police** et **son dégradé de fond d'écran** :
ce sont les deux choses les plus dépendantes de la palette. Un réglage de
police explicite dans `shell.conf` prend le pas ; vide, c'est le thème qui
décide.

La police est déclarée dans la **table des thèmes** (`config.c`) et non dans
le fichier CSS, pour qu'elle ait une source unique : le panneau de réglages
doit pouvoir la nommer et vérifier qu'elle est installée, ce qu'il ne saurait
pas faire en lisant une règle CSS. Quand la famille demandée est absente, il
le dit — sans deviner le nom du paquet apt, parce que « Lato » donne bien
`fonts-lato` mais « DejaVu Sans » ne donne pas `fonts-dejavu-sans`.

### Les thèmes Claude

Les couleurs sont **relevées** dans la feuille de style de marque
d'Anthropic (`anthropic.com`, `ant-brand.shared.css`), pas citées de
mémoire :

| Nom d'origine | Valeur | Usage ici |
|---|---|---|
| `clay` | `#d97757` | accent |
| `accent` | `#c6613f` | accent enfoncé |
| `ivory-light` | `#faf9f5` | surface (clair) |
| `ivory-medium` | `#f0eee6` | surface secondaire |
| `oat` | `#e3dacc` | surface enfoncée |
| `slate-dark` | `#141413` | texte (clair), fond (sombre) |
| `slate-light` | `#5e5d59` | texte secondaire |
| `slate-medium` | `#3d3d3a` | surface enfoncée (sombre) |
| `cloud-medium` | `#b0aea5` | texte secondaire (sombre) |
| `olive`, `kraft`, `cactus` | | états, halos du fond |

Trois honnêtetés à garder en tête.

**Les surfaces du thème sombre sont construites, pas relevées.** La feuille
de marque publie `slate-dark` et `slate-medium`, mais aucune des surfaces
intermédiaires d'une interface sombre. `#1f1f1d` et `#2a2a27` sont
interpolés entre les deux pour que le dock se détache du fond.

**La palette de marque n'a pas de rouge.** Une alerte doit rester lisible
comme telle : le `danger` est construit dans la même famille chaude.

**Les fontes sont propriétaires.** Le site déclare « Anthropic Sans »,
« Anthropic Serif » et « Tiempos Text » : rien de tout cela ne peut être
embarqué. **Lato** leur sert de substitut libre — un sans humaniste, plus
chaud qu'Inter, qui tient le petit corps. Comparé à l'écran avant d'être
retenu, pas choisi sur description.

C'est un **hommage, pas un habillage officiel** : ces thèmes ne sont pas des
ressources d'Anthropic et ne prétendent pas l'être.
