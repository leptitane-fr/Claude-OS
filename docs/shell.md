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

Le thème d'icônes n'est pas un détail : **Adwaita a abandonné les noms
hérités** (`web-browser`, `utilities-terminal`, `system-file-manager`) que
la plupart des fichiers `.desktop` déclarent encore, et affiche un
pictogramme générique à leur place. Constaté à l'écran, pas supposé.
Papirus les conserve.

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
