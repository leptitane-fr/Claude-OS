# Maquette de l'étagère

`etagere.html` est un rendu fidèle du dock et de la barre d'état, aux
dimensions, couleurs et rayons réels des fichiers de `rootfs/`.

Il sert à trancher l'esthétique **avant** que la machine soit installée, et
produit directement les lignes de configuration correspondantes : on règle à
l'œil, on récupère des valeurs exactes à reporter dans `dock.tint2rc`,
`status.tint2rc` et `picom.conf`.

Publié : <https://claude.ai/code/artifact/474091f0-3648-4b3f-aa1a-5bf076fe3e47>

Limites assumées : le flou du navigateur n'est pas l'algorithme de picom, et
les icônes définitives viendront du thème Papirus et du paquet
`claude-desktop`. Les dimensions, elles, sont exactes.
