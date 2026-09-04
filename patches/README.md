# Correctifs du shell

Le shell graphique (dock, barre d'état, réglages) vit dans une autre branche du
dépôt : `claude/custom-linux-usb-debian-ceoegh`. Ce dépôt-ci ne la réécrit pas.

Les corrections que nous lui apportons sont donc conservées ici sous forme de
correctifs, et `tools/essayer-shell.sh` les applique à l'arbre de travail juste
avant la compilation — dans l'ordre alphabétique du nom de fichier.

## Ce que fait le script

Pour chaque `*.patch`, dans l'ordre :

| Situation | Comportement |
|---|---|
| Le correctif s'applique | il est appliqué, le nom est affiché |
| Il est déjà présent dans la branche | il est ignoré (la correction a été reprise en amont) |
| Il ne s'applique plus | **l'installation s'arrête** |

Le dernier cas est volontairement brutal. Un shell compilé en silence sans son
correctif est pire qu'un échec : on croit avoir corrigé, le défaut est toujours
là, et rien ne le dit.

## Destination

Ces correctifs sont un intermédiaire, pas une fin. Chacun devrait finir intégré
à la branche du shell ; le correctif disparaît alors d'ici, ou reste sans effet
(le script le détectera comme « déjà en amont »).

## Correctifs présents

### `0001-association-app-id.patch`

`shell_app_id_matches()` rapprochait deux identifiants dès que l'un préfixait
l'autre et qu'un séparateur suivait. La règle avait été écrite pour Chromium,
qui s'annonce `chromium` ou `chromium-browser` selon les versions.

Elle concluait aussi que `claude` et `claude-os-reglages` désignent la même
application. Sur la machine : Claude Desktop lancé, le dock marquait les
Réglages comme actifs, affichait « Claude » dans leur infobulle, et le clic
ramenait la fenêtre de Claude au lieu d'ouvrir les réglages.

Le correctif remplace la règle sur le séparateur par une liste fermée de
suffixes (`-browser`, `-desktop`, `-esr`, `-bin`, `-stable`, `-nightly`) : le
reste de l'identifiant doit être *exactement* l'un d'eux. Le cas Chromium est
conservé, `-os-reglages` est écarté.

### `0002-lanceur-applications.patch`

Ajoute `claude-os-lanceur` : une fenêtre centrale qui liste les applications
installées, avec recherche, tri et trois présentations (icônes, liste,
détails). Nouveau fichier `shell/src/launcher.c`, plus l'entrée `meson.build`,
les règles CSS et un raccourci `Super+A` dans `rc.xml`.

Le dock gagne trois choses au passage :

- un **bouton rond à gauche** qui ouvre le lanceur ;
- un **menu au clic droit** sur chaque icône — nouvelle fenêtre, épingler ou
  retirer, fermer les fenêtres ouvertes ;
- l'acceptation des **dépôts venus du lanceur** : glisser une application sur
  une icône l'épingle à cet endroit, sur le fond du dock l'ajoute à la fin.

`shell_toplevel_close()` est ajouté à `toplevels.c` pour l'entrée « Fermer ».

Le lanceur n'est démarré par personne au lancement de la session : le bouton
du dock le lance à la première utilisation, et comme il n'admet qu'une
instance, les appels suivants se contentent de le faire basculer.

### `0003-reglages-vers-barre-etat.patch`

Sort « Réglages » du dock et le place dans le panneau des réglages rapides,
sous la carte batterie — au même endroit que le Wi-Fi et la batterie, comme
sur ChromeOS. `claude-os-reglages` disparaît des applications épinglées par
défaut ; le bouton du lanceur garantit qu'aucune application ne devient
inatteignable.

### `0004-lanceur-clic-exterieur.patch`

Le lanceur se ferme désormais au clic à côté. Sa surface s'ancre aux quatre
bords de l'écran, avec une marge basse de 100 px : elle couvre tout, sauf la
bande du dock, qui reste cliquable — et reste donc une cible de glisser-
déposer. Le voile est entièrement transparent, rien ne change à l'écran ; il
n'existe que pour recevoir les clics.

C'est le compromis qui remplace le premier : le lanceur de ChromeOS occupe
tout l'écran, mais un voile intégral aurait capté aussi les clics destinés au
dock, et la cible du dépôt serait devenue inatteignable.

### `0005-gestionnaire-fichiers.patch`

Ajoute `claude-os-fichiers` : une fenêtre ordinaire, à la Windows —
navigation, fil d'Ariane, volet des emplacements, barre d'actions, barre
d'état — en quatre fichiers.

| Fichier | Rôle |
|---|---|
| `fichiers.h` / `fichiers-item.c` | l'élément (un `GObject`) et la lecture d'un répertoire, asynchrone et par lots |
| `fichiers-ops.c` | copier, déplacer, corbeille, supprimer — dans un fil séparé |
| `fichiers-lieux.c` | le volet de gauche : dossiers personnels, favoris, périphériques |
| `fichiers.c` | la fenêtre : barres, trois vues, menus, raccourcis |

Trois vues — icônes, liste, détails — qui partagent **un seul** magasin, un
seul filtre, un seul tri et une seule sélection. Changer de vue ne perd donc
rien, et le tri se règle en cliquant les en-têtes de la vue Détails, dont le
trieur est celui de tout le monde. Les trois sont virtuelles : un répertoire
de dix mille fichiers ne coûte que dix mille petits objets.

`config.c` remplace `thunar` par `claude-os-fichiers` dans les applications
épinglées par défaut.

Deux points à savoir :

- **Rien n'est jamais écrasé.** Quand la destination existe, le nom est
  décalé : « notes (copie).txt ». Windows pose la question ; la poser depuis
  un fil de travail demanderait de le suspendre à chaque collision.
- **La corbeille pointe sur un vrai dossier**, `~/.local/share/Trash/files`,
  parce que l'adresse `trash:///` demanderait gvfs. On y voit ce qui a été
  jeté, sous son nom d'origine ; restaurer un fichier à sa place d'origine
  n'est en revanche pas possible de là.
