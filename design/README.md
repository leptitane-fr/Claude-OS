# Maquette de l'étagère

`etagere.html` rend le dock, la barre d'état **et les fenêtres** aux dimensions,
couleurs et rayons réels des fichiers de `rootfs/`.

Il sert à trancher l'esthétique avant que la machine soit installée, et produit
les lignes de configuration correspondantes : on règle à l'œil, on récupère des
valeurs exactes à reporter dans `picom.conf`, `status.tint2rc` et les réglages
de plank.

Publié : <https://claude.ai/code/artifact/474091f0-3648-4b3f-aa1a-5bf076fe3e47>

## Ce que chaque vue démontre

| Vue | Point du cahier des charges |
|---|---|
| **Bureau** | Icônes et dossiers déposés sur le bureau (point 10). |
| **Bloc-notes** | Fenêtre sans cadre latéral ni inférieur : barre de titre avec ses trois boutons, puis barre de menu (point 2). |
| **Chromium maximisé** | La fenêtre occupe tout l'écran et passe **sous** le dock et la barre d'état (point 8). Son cadre est dessiné par l'application, pas par Openbox. |
| **Touche Loupe** | Masque et réaffiche l'ensemble dock + barre d'état (point 7). |

## Limites assumées

Le zoom de plank agrandit les icônes voisines en cascade ; la maquette ne bouge
que celle survolée. Les icônes définitives viendront du thème Papirus et du
paquet `claude-desktop`. Les dimensions, couleurs et rayons, eux, sont exacts.
