# Le shell de Claude OS

Dock, barre d'état, lanceur d'applications, gestionnaire de fichiers,
panneau de réglages et fond d'écran. Six petits programmes en C, écrits sur
GTK4 et gtk4-layer-shell, portés par le compositeur **labwc**.

## Origine

Ce code vient de la branche `claude/custom-linux-usb-debian-ceoegh`, au
commit `ebcc811`, où il avait été écrit pour une autre machine. Il a d'abord
été essayé ici sous forme de correctifs appliqués à la compilation, le temps
de vérifier qu'il convenait ; il est désormais versé dans ce dépôt, les cinq
correctifs fondus dedans, et c'est cette copie qui fait foi.

Ce qui a été ajouté ou corrigé au passage :

| Quoi | Où |
|---|---|
| Rapprochement des identifiants d'application, qui confondait Claude et les Réglages | `src/toplevels.c` |
| Lanceur d'applications | `src/launcher.c` |
| Bouton du lanceur, menu au clic droit, dépôts venus du lanceur | `src/dock.c` |
| Réglages déplacés du dock vers le panneau de la barre d'état | `src/panel.c`, `src/config.c` |
| Gestionnaire de fichiers | `src/fichiers*.c` |

## Les programmes

| Binaire | Rôle | Résident |
|---|---|---|
| `claude-os-fond` | fond d'écran, couche la plus basse | oui |
| `claude-os-dock` | dock centré en bas | oui |
| `claude-os-status` | barre d'état en bas à droite, et son panneau | oui |
| `claude-os-lanceur` | liste des applications | à la demande, puis masqué |
| `claude-os-fichiers` | gestionnaire de fichiers | non |
| `claude-os-reglages` | apparence, fond d'écran, dock | non |
| `claude-os-connexion` | écran de connexion, sous greetd | avant la session |

Les trois résidents sont des processus **séparés** : un défaut dans la barre
d'état n'emporte pas le dock, et chacun se relance seul sans fermer la
session.

## Compilation

```sh
meson setup build --prefix=/usr
ninja -C build
sudo meson install -C build
```

Il faut `libgtk-4-dev`, `libgtk4-layer-shell-dev`, `libwayland-dev`,
`libwayland-bin` (pour `wayland-scanner`), `libjson-glib-dev`, `meson`,
`ninja-build` et `pkgconf`. `install/provision.sh` s'en charge.

`json-glib` ne sert qu'à l'écran de connexion, qui parle à greetd en JSON.
Écrire un analyseur à la main pour un protocole dont une erreur d'analyse
empêche de se connecter n'est pas un pari raisonnable.

Le protocole `wlr-foreign-toplevel-management-v1` — celui qui dit quelles
fenêtres sont ouvertes — n'est empaqueté nulle part dans Debian trixie. Son
XML est donc versé dans `protocols/` et le code client engendré à la
compilation.

## Configuration

Tout passe par `~/.config/claude-os/shell.conf`, que chaque composant
surveille : l'écrire suffit à changer le thème ou l'ordre des icônes sur un
dock déjà lancé. Aucun protocole à inventer, une seule source de vérité.

```ini
[dock]
pinned=chromium;claude-desktop;mousepad;claude-os-fichiers
reserve_space=false

[appearance]
theme=claude-sombre        ; clair, sombre, claude-clair, claude-sombre
icon_theme=Papirus
font=                      ; vide = la police du thème

[wallpaper]
image=                     ; vide = le dégradé dessiné par le shell
fill=true
```

Ce fichier appartient à l'utilisateur : le dock y écrit l'ordre des icônes au
glisser-déposer, le lanceur et le clic droit y ajoutent et retirent des
applications. `provision.sh` ne l'écrit qu'à la première installation.

## Le style

`style/shell.css` ne connaît **aucune couleur littérale**, seulement des
jetons : `@accent`, `@surface`, `@text`… Chaque thème est un fichier
`style/theme-<id>.css` qui définit exactement les mêmes noms. Ajouter un
thème, c'est ajouter ce fichier et une ligne dans la table de `src/config.c` :
aucune règle de `shell.css` n'est à toucher.

## Banc d'essai visuel

`test-render.sh` démarre labwc sans écran (backend headless de wlroots, rendu
logiciel), lance un composant, capture le rendu et arrête tout. Il permet de
juger l'apparence réelle sans matériel graphique :

```sh
./shell/test-render.sh ./build/claude-os-dock rendu.png
```
