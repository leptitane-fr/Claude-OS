# Essayer le shell sur une Claude-OS déjà installée

Comment voir le dock et la barre d'état tourner sur la vraie machine, **sans
rien changer au démarrage** et en gardant Xfce disponible à tout instant.

## Le principe, et pourquoi le retour en arrière est garanti

Rien de ce qui suit ne touche à l'amorçage, à LightDM, ni à la session par
défaut. Le shell est installé **à côté** :

| Ce qui est modifié | Où | Comment l'annuler |
|---|---|---|
| Binaires du shell | `/usr/local/` | `sudo ninja -C build uninstall` |
| Script de bascule | `/usr/local/bin/claude-os-shell-basculer` | `rm` |
| Configuration labwc | `~/.config/labwc/` | `rm -rf` |
| Configuration du shell | `~/.config/claude-os/` | `rm -rf` |
| Option Wayland de Chromium | `/etc/chromium.d/zz-claude-os-wayland` | `rm` |
| Paquets de compilation | apt | `apt-get autoremove --purge` |

`/usr` n'est pas touché : tout passe par `/usr/local`, que dpkg n'utilise
jamais. **Un redémarrage ramène Xfce quoi qu'il arrive**, puisque rien dans
la chaîne de démarrage n'a été modifié.

Et surtout : Xfce **continue de tourner** sur son terminal virtuel pendant
l'essai. Le shell démarre sur un autre. Un `Ctrl+Alt+F<n>` fait l'aller-retour
entre les deux, sans rien fermer.

## Avant de commencer

Dans un terminal Xfce, noter le numéro du terminal virtuel courant — c'est
par là qu'on reviendra :

```sh
echo "Xfce est sur le terminal virtuel $XDG_VTNR"
```

## 1. Les paquets

Deux groupes : ce que le shell exige pour tourner, et ce qu'il faut pour le
compiler. L'image du 1er septembre est antérieure à l'arrivée de labwc dans
le projet, elle n'a donc encore rien du premier groupe.

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends \
    labwc libgtk4-layer-shell0 papirus-icon-theme fonts-inter libglib2.0-bin \
    xwayland unzip \
    build-essential meson ninja-build pkgconf \
    libgtk-4-dev libgtk4-layer-shell-dev
```

Le paquet de la bibliothèque d'ancrage s'appelle `libgtk4-layer-shell0`.
`gtk4-layer-shell` tout court est le nom du paquet **source** : il apparaît
dans l'index de Debian, mais ne s'installe pas.

`libglib2.0-bin` n'est pas décoratif : il fournit `gapplication`, sans lequel
le raccourci de masquage ne ferait rien.

## 2. Récupérer les sources

Le dépôt est privé : `git clone` demanderait des identifiants. Le plus simple
est de passer par Chromium, déjà connecté à GitHub — le bouton vert **Code →
Download ZIP** sur la page du dépôt donne exactement la bonne branche, qui
est aussi la branche par défaut.

```sh
cd ~
unzip -q "$(ls -t ~/Téléchargements/Claude-OS-*.zip ~/Downloads/Claude-OS-*.zip \
            ~/Claude-OS-*.zip 2>/dev/null | head -1)"
cd Claude-OS-*/shell
```

## 3. Compiler et installer

```sh
meson setup build --prefix=/usr/local
ninja -C build
sudo meson install -C build
```

`--prefix=/usr/local` n'est pas un détail de goût : c'est ce qui garantit
qu'aucun fichier du système géré par dpkg n'est écrasé.

## 4. Configuration

Tout est dans le dossier personnel, sauf le script de bascule qui doit être
à un chemin fixe puisque le raccourci clavier l'appelle par son nom.

```sh
mkdir -p ~/.config/labwc ~/.config/claude-os

cp ../overlay/etc/xdg/labwc/rc.xml     ~/.config/labwc/
cp ../overlay/etc/xdg/labwc/autostart  ~/.config/labwc/
sed 's|@KEYMAP@|fr|' ../overlay/etc/xdg/labwc/environment > ~/.config/labwc/environment

sudo install -m755 ../overlay/usr/local/bin/claude-os-shell-basculer /usr/local/bin/
```

Les applications épinglées : n'y mettre que ce qui est réellement installé,
sinon le dock affiche un pictogramme générique qui ne lance rien.
`reserve_space=true` ferait s'arrêter les fenêtres maximisées au-dessus du
dock, au prix d'un redimensionnement à chaque affichage de celui-ci.

```sh
cat > ~/.config/claude-os/shell.conf <<'FIN'
[dock]
pinned=chromium;thunar;xfce4-terminal;claude-os-reglages
reserve_space=false

[appearance]
font=
icon_theme=Papirus
theme=claude-sombre
FIN
```

Enfin, Chromium. Il tente X11 par défaut ; sous le shell il ne trouverait
aucun serveur X et refuserait de démarrer. Un fichier séparé plutôt qu'une
modification de celui du système : une seule chose à supprimer pour revenir
en arrière.

```sh
echo 'CHROMIUM_FLAGS="$CHROMIUM_FLAGS --ozone-platform-hint=auto"' \
  | sudo tee /etc/chromium.d/zz-claude-os-wayland
```

## 5. Lancer l'essai

`Ctrl+Alt+F3` amène un terminal virtuel libre. S'y connecter, puis :

```sh
dbus-run-session labwc >~/essai-shell.log 2>&1
```

`dbus-run-session` garantit un bus de session, sans lequel `gapplication` —
et donc le raccourci de masquage — n'aurait personne à qui parler.

### Ce qui doit fonctionner

| | |
|---|---|
| Dock centré en bas, barre d'état en bas à droite | au démarrage |
| Clic sur la barre | panneau Wi-Fi / Bluetooth / batterie en watts |
| `Super` (ou `Super+Espace`) | masque et affiche dock + barre |
| `Super+Entrée` | terminal |
| `Super+B` | Chromium |
| Survol d'une icône d'application ouverte | liste de ses fenêtres, cliquables |
| Glisser une icône épinglée | réorganise le dock, ordre enregistré |
| Icône « Réglages » du dock | thème, police, icônes, fond d'écran |
| Quatre thèmes | Clair, Sombre, Claude clair, Claude sombre |
| `Super+Maj+Q` | quitter la session |

`Super+Entrée` et `Super+B` sont des filets de sécurité : tant que le shell
n'a pas de lanceur, ce sont les seuls moyens d'ouvrir quelque chose si le
dock refuse de démarrer.

### Rester joignable

Le partage de connexion est géré par NetworkManager, un service **système** :
il n'est pas affecté par le changement de session et reste actif dans les
deux. Chromium fonctionne donc dans la session d'essai, ce qui permet le
copier-coller de commandes entre le navigateur et le terminal — les deux
partagent le presse-papier du compositeur.

Le presse-papier n'est en revanche **pas** partagé entre la session d'essai
et Xfce : ce sont deux compositeurs distincts.

## 6. Revenir en arrière

**Immédiatement, sans rien casser** : `Ctrl+Alt+F<n>` avec le numéro relevé
au début. Xfce est toujours là, avec ses fenêtres ouvertes. La session
d'essai continue de tourner de son côté ; `Super+Maj+Q` la ferme quand on y
retourne, ou `pkill labwc` depuis Xfce.

**Si rien ne s'affiche du tout** : le compositeur a rendu la main au terminal
virtuel. Le journal dit pourquoi.

```sh
tail -40 ~/essai-shell.log
```

**Désinstallation complète :**

```sh
cd ~/Claude-OS-*/shell
sudo ninja -C build uninstall
sudo rm -f /usr/local/bin/claude-os-shell-basculer /etc/chromium.d/zz-claude-os-wayland
rm -rf ~/.config/labwc ~/.config/claude-os
sudo apt-get autoremove --purge build-essential meson ninja-build \
    libgtk-4-dev libgtk4-layer-shell-dev
```

## Mettre à jour après un correctif

Retélécharger le ZIP, puis, depuis le nouveau dossier :

```sh
cd ~/Claude-OS-*/shell
meson setup build --prefix=/usr/local
ninja -C build && sudo meson install -C build
cp ../overlay/etc/xdg/labwc/rc.xml    ~/.config/labwc/
cp ../overlay/etc/xdg/labwc/autostart ~/.config/labwc/
```

Recopier `autostart` n'est pas facultatif quand de nouveaux composants
apparaissent : sans cela le fond d'écran ne serait pas lancé.

Ajouter aussi le panneau de réglages aux icônes épinglées, faute de quoi
rien ne permettrait de l'ouvrir :

```sh
sed -i 's/^pinned=.*/pinned=chromium;thunar;xfce4-terminal;claude-os-reglages/' \
    ~/.config/claude-os/shell.conf
```

Les thèmes Claude ont besoin de Lato, et une police vide laisse le thème
décider de la sienne :

```sh
sudo apt-get install --no-install-recommends fonts-lato
sed -i 's/^font=.*/font=/' ~/.config/claude-os/shell.conf
```

Dans la session d'essai, `Super+Maj+Q` puis relancer `dbus-run-session labwc`
suffit à reprendre les nouveaux binaires.

## Ce qui n'existe pas encore

- **Aucun lanceur d'applications** : seules les icônes épinglées, les
  applications déjà ouvertes et les deux raccourcis de secours ouvrent
  quelque chose.
- **Aucune notification, aucun réglage de volume ni de luminosité.**

## Ce que l'essai sur la machine a tranché

1. **La touche Windows seule fonctionne.** Le doute venait de ce que labwc,
   en local, ne réagit qu'à un `Super_L` arrivant comme touche ordinaire et
   pas comme modificateur seul ; sur un vrai clavier, les deux arrivent et la
   liaison se déclenche.
2. **Le plein écran ne masque rien, et c'est devenu le comportement voulu.**
   La fenêtre occupe bien tout l'écran — une surface layer-shell ne réserve
   rien face à une fenêtre plein écran — et le dock reste par-dessus. On
   garde l'heure et la batterie sous les yeux pendant une vidéo, et la touche
   Windows dégage l'écran quand on le veut vraiment.

   J'avais annoncé l'inverse, en lisant dans le code de labwc 0.8.3 une
   désactivation de la couche haute sous une fenêtre plein écran. Sur la
   machine, cela ne se produit pas : lire le code d'un compositeur dit ce
   qu'il contient, pas ce qu'il fait dans une situation donnée.
