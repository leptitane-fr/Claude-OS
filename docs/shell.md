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
  style/tokens.css     couleurs, rayons, espacements — source unique
  style/shell.css      règles communes aux composants
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
pinned=chromium;claude-desktop;thunar;xfce4-terminal
reserve_space=false  ; true : les fenêtres maximisées s'arrêtent au-dessus

[appearance]
font=Inter
icon_theme=Papirus
theme=light          ; ou dark
```

Le dock **et** la barre d'état lisent ce fichier, par le même code
(`config.c`). Quand seul le dock le lisait, un `theme=dark` donnait un dock
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

## Thèmes clair et sombre

Une seule palette dans `tokens.css`, deux jeux de valeurs. Les composants
ne référencent jamais une couleur littérale : ils lisent les jetons. Le
basculement se fait en rechargeant la feuille de style, sans redémarrage.
