# 4. L'environnement de bureau

> Ce document décrit **ce qui tourne réellement sur la machine**. Une version
> antérieure décrivait une pile X11 — openbox, plank, tint2, picom — qui a été
> construite, essayée et abandonnée. Le pourquoi de ce retournement est en
> `docs/02`, §2.4 ; il n'est pas répété ici.

---

## 4.1 La pile

| Couche | Choix | Poids | Rôle |
|---|---|---|---|
| Compositeur | **labwc** | ~2 Mo | wlroots ; ne fait que porter le shell |
| Ancrage | **gtk4-layer-shell** | — | pose le dock hors du flux des fenêtres |
| Interface | **shell sur mesure**, C + GTK4 | ~250 Ko | dock, barre, lanceur, fichiers, réglages |
| Connexion | **LightDM** | — | ouvre directement la session |

Le compositeur ne décore rien, ne dessine ni dock ni menu : `rc.xml` tient en
cinquante lignes, dont l'essentiel est une poignée de raccourcis clavier. Tout
ce qui se voit vient du shell.

### Pourquoi un shell écrit sur mesure

Ce n'était pas le plan. Le premier essai assemblait des composants existants,
et c'est ce qui a échoué : chacun apportait ses conventions, ses fichiers de
configuration, ses limites — plank ne lisait plus son fichier de réglages
depuis la 0.11, tint2 n'hébergeait que les icônes XEmbed, picom arrondissait
les quatre coins ou aucun. Ajuster l'ensemble revenait à combattre six
programmes à la fois.

Six petits programmes écrits pour ce système précis pèsent moins lourd que
les six qu'ils remplacent, partagent une seule feuille de style, un seul
fichier de configuration, et font exactement ce qu'on leur demande.

Le détail est dans `shell/README.md`.

---

## 4.2 Ce que l'on voit

### Le dock

Centré en bas, à la façon de macOS. À gauche, un **bouton rond** qui ouvre le
lanceur ; puis les applications épinglées ; puis, après un séparateur, celles
qui sont ouvertes sans être épinglées — sans quoi une fenêtre lancée depuis un
terminal serait introuvable.

- Un **point** sous l'icône quand l'application tourne ; il prend l'accent
  quand c'est elle qui a le focus.
- **Survol** : la liste de ses fenêtres, cliquables. Le panneau ne prend pas
  le clavier — survoler le dock ne vole pas le focus à ce qu'on est en train
  d'écrire.
- **Glisser** une icône la déplace, et l'ordre est écrit aussitôt dans
  `shell.conf`. Une réorganisation qu'il faudrait penser à enregistrer serait
  une réorganisation perdue.
- **Clic droit** : nouvelle fenêtre, épingler ou retirer, fermer les fenêtres.

Le dock **ne réserve pas sa place** par défaut : une fenêtre maximisée passe
dessous. L'inverse la ferait se redimensionner à chaque appui sur la touche
Loupe — mesuré : 1920×1114 dock affiché, 1920×1200 dock masqué. Le réglage
existe pour qui préfère l'autre comportement.

### La barre d'état

En bas à droite, dans la même pilule. Réseau, batterie, heure. Au clic, un
panneau : bascules Wi-Fi et Bluetooth avec leurs listes, carte batterie avec
la **consommation instantanée en watts**, et l'accès aux Réglages.

Discipline d'énergie, parce que c'est le composant qui risque le plus de
réveiller la machine :

- **une seule** minuterie, alignée sur la minute, pour l'heure *et* la
  batterie ;
- le réseau ne consulte rien — il réagit aux signaux D-Bus de NetworkManager ;
- la minuterie des watts ne tourne **que** pendant que le panneau est ouvert ;
- les services ne sont contactés qu'à la première ouverture du panneau.

### Le lanceur

Une fenêtre centrale, en icônes par défaut. Recherche — qui ignore la casse
*et* les accents, et fouille aussi les mots-clés des fichiers `.desktop` —,
tri par nom ou catégorie, et trois présentations : icônes, liste, détails.

Clic droit pour épingler au dock ou en retirer ; glisser une application vers
le dock l'y épingle à l'endroit du dépôt.

Il se ferme au clic à côté. Sa surface couvre l'écran **sauf une bande de
100 px en bas** : c'est ce qui permet de fermer d'un clic tout en gardant le
dock atteignable pour le glisser-déposer.

Il n'est démarré par personne à l'ouverture de session : le bouton du dock le
lance à la première utilisation, et comme il n'admet qu'une instance, les
appels suivants ne font que le faire basculer. Rien n'est payé en mémoire tant
qu'il n'a pas servi.

### Le gestionnaire de fichiers

Classique, à la Windows : navigation, fil d'Ariane cliquable, volet des
emplacements à gauche, barre d'actions, barre d'état.

Trois vues — icônes, liste, détails — qui partagent **un seul** magasin, un
seul filtre, un seul tri et une seule sélection : changer de vue ne perd rien,
et les en-têtes de colonnes de la vue Détails règlent le tri des trois. Les
vues sont virtuelles ; un répertoire de dix mille fichiers ne coûte que dix
mille petits objets.

Copier, couper, coller, renommer, corbeille, suppression définitive, nouveau
dossier, propriétés, favoris, fichiers cachés, recherche, historique,
glisser-déposer. Les opérations tournent dans un fil séparé, avec une fenêtre
de progression qui n'apparaît qu'au bout de 400 ms.

**Rien n'est jamais écrasé** : une destination occupée décale le nom en
« (copie) ». Poser la question depuis un fil de travail demanderait de le
suspendre à chaque collision ; décaler ne perd jamais rien et se défait à la
main.

### Le fond d'écran

Une image, ou le dégradé que le shell dessine lui-même — défini par chaque
thème, en trois dégradés radiaux superposés. Aucune image à charger, aucun
octet sur le disque.

---

## 4.3 Le style

`style/shell.css` ne contient **aucune couleur littérale**. Uniquement des
jetons : `@accent`, `@surface`, `@surface-alt`, `@text`, `@border`, `@shadow`…
Chaque thème est un fichier `style/theme-<id>.css` qui définit exactement les
mêmes noms.

Quatre thèmes : **Clair**, **Sombre** (inspiration ChromeOS, accent bleu), et
**Claude clair** / **Claude sombre**, qui reprennent les couleurs de la charte
d'Anthropic — hommage, pas habillage officiel.

Ajouter un thème, c'est ajouter un fichier et une ligne dans la table de
`src/config.c`. Aucune règle de `shell.css` n'est à toucher.

Les ombres sont **doublées** : une large et diffuse pour l'élévation, une
courte et dense pour asseoir le contact. C'est ce doublement qui donne la
profondeur de ChromeOS, là où une ombre unique paraît plate. Elle est calculée
une fois par le compositeur, jamais réévaluée — rien à voir avec un flou
permanent, qui aurait coûté un rendu par image.

---

## 4.4 Le cahier des charges, point par point

| # | Demande | État |
|---|---|---|
| 1 | Esthétique ChromeOS, dock macOS centré | fait |
| 2 | Fenêtres sans cadre latéral ni inférieur | fait — décorations côté client, `decoration=client` |
| 3 | Pas de flou d'arrière-plan | fait — aucun flou nulle part |
| 4 | Ombres légères sur fenêtres, icônes, dock, barre | fait |
| 5 | Bouton du lanceur sur le dock | fait |
| 6 | Icônes réorganisables au glisser-déposer | fait, ordre enregistré aussitôt |
| 7 | Dock et barre basculés par la touche Loupe | fait (Super, et Super+Espace en repli) |
| 8 | Fenêtres maximisées **sous** le dock | fait, et réglable |
| 9 | Panneau de réglages d'affichage | fait — thème, police, icônes, fond d'écran, dock |
| 10 | Déposer icônes et dossiers sur le bureau | **non fait** — voir ci-dessous |
| 11 | Google Drive et OneDrive dans le gestionnaire | reporté, prévu |
| 12 | Écran tactile fonctionnel | fait — natif sous Wayland, sans configuration |

### Les deux points ouverts

**Point 10 — des icônes sur le bureau.** Le fond d'écran est une surface
layer-shell qui ne dessine qu'un dégradé ou une image ; il n'y a pas de
gestionnaire de bureau. Le faire demande d'ajouter au fond la gestion d'une
grille d'icônes, du `~/Bureau`, du glisser-déposer et du clic droit —
c'est-à-dire un septième programme. Reporté, pas oublié.

**Point 11 — le nuage.** Le volet du gestionnaire de fichiers est déjà découpé
en sections ; il n'y aura qu'à en déclarer une de plus, alimentée par la
configuration de rclone. Rien ne s'affichera tant que rien n'est configuré :
une entrée qui ne mène nulle part serait pire que son absence.

### Reporté sans regret

Les **boutons de barre de titre stylisés** « coup de crayon » : les fenêtres
portent des décorations côté client, dessinées par chaque application. Les
imposer voudrait dire les reprendre au compositeur, donc redessiner soi-même
la barre de titre de Chromium et de Claude Desktop.

---

## 4.5 L'écran de connexion, et un piège qui a coûté une soirée

LightDM affiche une barre en haut de l'écran de connexion — c'est le panneau
du greeter, pas un morceau du bureau : il n'existe qu'avant l'ouverture de
session. À droite, un bouton donne le choix de la session.

**Ce menu l'emporte sur tout le reste.** LightDM retient la dernière session
choisie, dans `~/.dmrc` et dans AccountsService, et la relance à la connexion
suivante — quoi que dise `user-session=` du siège.

C'est ce qui s'est passé à la bascule vers Wayland. La préférence enregistrée
valait `lightdm-xsession`, la session X générique de Debian, qui exécute
`~/.xsession` ; l'ancienne installation y mettait le lancement du bureau. En
supprimant ce fichier sans corriger la préférence, on a laissé LightDM
relancer une session vide.

Et Debian ne s'arrête pas là : privé de `~/.xsession`, son `/etc/X11/Xsession`
descend sa liste de replis et finit par lancer `x-terminal-emulator`. D'où le
symptôme exact — fond noir, curseur, et **une fenêtre de terminal apparue
toute seule**, qui n'avait aucun rapport avec le shell.

`provision.sh` écrit désormais la préférence aux deux endroits. Et
`validate-install.sh` vérifie quelle session tourne réellement : le contrôle
qui aurait tranché en une ligne.

Trois entrées restent proposées dans le menu :

| Entrée | Ce qu'elle lance |
|---|---|
| **Claude OS** | `claude-os-session`, qui garantit un bus D-Bus puis lance labwc |
| **labwc** | labwc directement — le même bureau, sans ce filet. Repli utile |
| **Default Xsession** | une session X vide. À ne pas choisir |

---

## 4.6 Ce qui est volontairement absent

| Absent | Pourquoi |
|---|---|
| Effets de bureau, animations de fenêtres | chaque image rendue est de l'énergie |
| Flou d'arrière-plan | demandé absent, et coûteux : un rendu par image |
| Indexeur de fichiers | écrit sur l'eMMC en permanence pour une recherche rare |
| Gestionnaire de paquets graphique | `apt` suffit, et c'est 80 Mo de moins |
| Client de courrier, suite bureautique | usage 100 % web |
| Serveur X | Wayland de bout en bout ; Xwayland n'est qu'un filet |
| Démon de notifications | parti avec X11 — à remettre, voir `docs/02` §2.7 |

---

## 4.7 Ce qui reste à valider sur la machine

| Quoi | Comment | Enjeu |
|---|---|---|
| **Audio** | `speaker-test -c2 -twav`, casque **et** haut-parleurs | risque n°1 : casque bon, haut-parleurs muets est le symptôme classique |
| Décodage vidéo | `vainfo`, puis `chrome://gpu` | autonomie en lecture vidéo |
| Rangée supérieure | `bash tools/probe-keys.sh` | luminosité et volume ne sont pas câblés |
| Consommation | `powertop`, panneau de la barre d'état | cible : ~4 W au repos, écran allumé |

### AV1

**Jasper Lake ne décode pas l'AV1 en matériel** — cette capacité arrive avec
Tiger Lake. YouTube sert de l'AV1 par défaut à un navigateur qui l'annonce :
le décodage retombe alors sur le processeur, et une vidéo 1080p peut y passer
la moitié des quatre cœurs.

VP9 et H.264, eux, sont accélérés. La mitigation est côté navigateur —
extension forçant VP9, ou `chrome://flags` — et se règle une fois pour toutes.

---

## 4.8 Sources

- Documentation Anthropic, Claude Desktop pour Linux — *Quick Entry* et
  portails.
- `labwc(1)`, `labwc-config(5)` — configuration du compositeur.
- Protocole `wlr-foreign-toplevel-management-unstable-v1` — suivi des fenêtres.
- Protocole `wlr-layer-shell-unstable-v1` — ancrage des surfaces de bureau.
- Documentation GTK4 : `GtkColumnView`, `GtkListItemFactory`, `GtkCssProvider`.
- Spécification freedesktop : entrées `.desktop`, catégories de menu,
  corbeille.
