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

Le partage est net, et il a été tranché par la lecture du code de labwc
plutôt qu'à l'estime.

**Le plein écran appartient au compositeur.** labwc 0.8.3 — la version de
Debian trixie — désactive lui-même toute la couche `TOP` du layer-shell dès
qu'une fenêtre plein écran n'a aucune fenêtre au-dessus d'elle
(`src/desktop.c`, `desktop_update_top_layer_visibility`). Le dock et la
barre disparaissent donc sans une ligne de notre part, et selon une règle
meilleure que celle qu'on aurait écrite : elle raisonne sur l'empilement
réel, pas seulement sur le focus.

Une première version suivait les fenêtres par
`wlr-foreign-toplevel-management-v1` pour en déduire le plein écran. Elle
fonctionnait — les événements arrivaient, les états étaient suivis — mais
elle dupliquait dans le shell une politique qui appartient au compositeur,
au prix d'un protocole embarqué dans le dépôt. Elle a été retirée.

Ce n'est pas une supposition : labwc **0.7.1** (celui du conteneur de
développement) ne transmet jamais l'état `fullscreen` sur ce protocole —
seul `activated` circule, mesuré en journalisant le tableau d'états brut.
labwc **0.8.3** appelle bien `wlr_foreign_toplevel_handle_v1_set_fullscreen`
(`src/foreign-toplevel/wlr-foreign.c`). C'est cette différence de version
qui a mené à lire le code plutôt qu'à conclure de l'essai.

**À vérifier au premier démarrage** : passer Chromium en plein écran doit
faire disparaître dock et barre. Sinon, le suivi des fenêtres devra revenir
dans `visibility.c`.

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

Mesuré ici, clavier virtuel à l'appui : labwc déclenche `<keybind
key="Super_L">` quand la touche arrive comme **touche ordinaire**, mais pas
quand elle arrive comme **modificateur seul**. Le comportement d'un clavier
réel — où l'appui produit les deux à la fois — n'a pas pu être reproduit
sans matériel.

`rc.xml` fournit donc les deux liaisons, `Super_L` et `Super+Espace`. Si la
touche Windows seule se déclenche en trop pendant d'autres raccourcis Super,
supprimer la première ligne suffit.

L'apparition est instantanée, sans animation : déplacer une surface
layer-shell demanderait un réveil par image pour quelques dixièmes de
seconde de mouvement.

## Zones réservées : le piège de l'empilement

Le dock réserve 86 px, ce qui empêche les fenêtres maximisées de passer
dessous. La barre d'état, elle, déclare `exclusive_zone = -1` : elle
**ignore** les zones réservées par les autres surfaces.

Avec la valeur 0, elle se posait au-dessus des 86 px du dock et flottait
plus haut que lui — visible immédiatement sur une capture. Avec -1 elle
s'ancre au vrai bord de l'écran et partage la ligne de base du dock. Elle
ne réserve rien pour elle-même, sinon la hauteur utile serait amputée deux
fois.

Les composants sont des exécutables **séparés**. Un défaut dans le lanceur
ne doit pas emporter le dock, et chacun peut être relancé isolément.

## Thèmes clair et sombre

Une seule palette dans `tokens.css`, deux jeux de valeurs. Les composants
ne référencent jamais une couleur littérale : ils lisent les jetons. Le
basculement se fait en rechargeant la feuille de style, sans redémarrage.
