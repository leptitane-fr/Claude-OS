# 12. Le mode tablette

Écran retourné, le Chromebook devient une tablette. Cette page dit **ce qui a
été mesuré** sur MADOO, ce qui a été décidé avec l'utilisateur, et ce qui
reste une limite. Ce qui n'est pas établi y est écrit comme tel.

État au 13 septembre 2026 :

| Pièce | État |
|---|---|
| Détection du mode tablette | **Faite, vue fonctionner sur MADOO** |
| Rotation de l'écran (paysage / chevalet) | **Faite, vue fonctionner sur MADOO** |
| Tactile et stylet alignés sur l'écran tourné | **Fait** — doigt vu juste en portrait ; stylet non essayé |
| Clavier AZERTY à l'écran, plein format | **Vu fonctionner sur MADOO** — voir §12.6 |
| Mode console (deux colonnes, écran recadré) | **En service pour un essai de plusieurs jours** — voir §12.7 |
| Disposition de la colonne gauche | **Calculée** : fréquences du français + zone du pouce mesurée — §12.7 |
| Suggestions de mots, espace et majuscule automatiques | **Écrites le 13 septembre 2026** — §12.8 |
| Déplacer Claude Desktop au doigt | **Impossible sous labwc 0.8.3** — voir §12.5 |

---

## 12.1 Ce que la machine offre — mesuré

| Constat | Comment |
|---|---|
| Trois périphériques portent `SW_TABLET_MODE` : `Tablet Mode Switch` (chromeos_tbmc, ACPI GOOG0006), `cros_ec_buttons`, `Intel Virtual Switches` | `/proc/bus/input/devices` |
| Tous en `root:input 0660` ; le compte n'est pas dans `input` | `ls -l /dev/input` |
| Ils concordent à 120 ms près, sans rebond ; la bascule se fait vers 180–220° à l'aller, vers 160° au retour | deux sondes au doigt, `EVIOCGSW` et lecture des événements |
| **L'EC ne coupe NI le clavier NI le pavé tactile** : 26 frappes et 28 contacts reçus par evdev écran replié | même sonde |
| **libinput les neutralise** : rien ne s'écrit, le pointeur ne bouge pas | constaté à l'écran par l'utilisateur |
| Quatre capteurs IIO : angle du capot, accéléromètre de l'écran, de la base, gyroscope ; sysfs `*_raw` lisibles par le compte, `/dev/iio:*` en root 0600 | `/sys/bus/iio/devices` |
| L'accéléromètre de l'écran est échantillonné **en continu** à 15,6 Hz par l'EC | `sampling_frequency` |
| labwc 0.8.3 expose `zwp_virtual_keyboard_manager_v1`, `zwp_input_method_manager_v2`, `zwp_text_input_manager_v3` | sonde des globaux Wayland |

**Deux affirmations antérieures étaient fausses** et ont été corrigées :
`docs/09` et `clavier.h` disaient que labwc n'expose pas le clavier virtuel
Wayland — il l'expose ; et que l'EC coupe le clavier capot retourné — c'est
libinput qui l'écarte, l'EC le laisse passer.

## 12.2 La détection — `shell/src/tablette.c`

Dans `claude-os-dock`, pour la raison qui a mis les notifications dans la
barre : un processus à part coûterait un runtime GTK4 entier.

- Le commutateur est lu par ses **événements** : le descripteur est surveillé
  par la boucle GLib, rien ne tourne entre deux retournements.
- Un seul périphérique est ouvert : `Tablet Mode Switch`, qui ne porte que ce
  commutateur. `cros_ec_buttons` porte aussi l'alimentation et le volume ;
  `intel-vbtn` a un historique de faux « mode tablette » ailleurs.
- La permission vient de `rootfs/etc/udev/rules.d/70-claude-os-tablette.rules`
  : `uaccess`, donc une ACL pour la seule session active, sur ce seul
  périphérique. **Vérifié** : le commutateur s'ouvre, le clavier (`event0`)
  reste refusé. Le groupe `input` aurait ouvert la frappe à tout programme du
  compte.
- udev ne rejoue pas un périphérique présent : après déploiement,
  `udevadm trigger --action=change /sys/class/input/eventN`, ou un
  redémarrage.

L'état est publié sur le bus comme **action à état** de
`os.claude.shell.dock` — lisible par `org.gtk.Actions.Describe`, suivi par le
signal `Changed` :

```sh
gdbus call --session --dest os.claude.shell.dock \
  --object-path /os/claude/shell/dock \
  --method org.gtk.Actions.Describe tablette          # ((true, '', [<false>]),)
gapplication action os.claude.shell.dock tablette          # force, bascule
gapplication action os.claude.shell.dock tablette-suivre   # rend au commutateur
gapplication action os.claude.shell.dock rotation-verrou   # verrou d'orientation
```

Le forçage sert au banc d'essai, et à qui voudrait le clavier à l'écran capot
ouvert.

## 12.3 La rotation — `shell/src/rotation.c`

**Seulement en mode tablette, et seulement paysage ↔ chevalet (180°).**

Les portraits ont été écrits et éprouvés — sens juste, doigt juste — puis
**fermés à la demande de l'utilisateur** : la dalle de MADOO se lit mal en
portrait et il ne s'en sert jamais. Le 180° sert à la position chevalet, pour
les vidéos. Une seule constante (`PERMISE[]`) les rouvrirait.

- **Une scrutation, la seule du projet, et bornée** : deux lectures par
  seconde de `in_accel_{x,y}_raw`, en mode tablette seulement. L'EC
  échantillonne ce capteur en permanence pour l'angle du capot ; le lire ne
  réveille aucun matériel. Capot ouvert, aucun minuteur n'existe.
- Repère mesuré : 1 g ≈ 16384 brut, +y vers le haut de l'écran, +z vers
  l'utilisateur. Le sens de x était supposé ; les portraits l'ont confirmé.
- Seuil de 0,4 g dans le plan de l'écran (tablette à plat : on ne tourne
  pas), hystérésis de 20°, deux lectures concordantes.
- **Temps de pose de 1,5 s en entrant en mode tablette.** Vu au premier
  essai : l'écran passait en 180° pendant le geste de retournement. Ce n'est
  pas le capteur qui se trompe — base à plat, l'écran passe réellement sous
  la charnière entre 180 et 360°, bord haut vers le sol, et le commutateur
  bascule vers 200°, avant la fin du geste.
- Appliquée par `wlr-randr`, un processus par rotation ; sa sortie est lue.
  Retour en paysage en quittant le mode tablette, et au démarrage du dock —
  un dock tombé écran tourné ne laisse pas l'écran de biais.

**Le tactile et le stylet suivent** parce que `rc.xml` les associe à `eDP-1`
(`<touch mapToOutput>`, `<tablet mapToOutput>`). Sans cela, wlroots rapporte
le doigt à l'écran non tourné.

## 12.4 Les fenêtres : laissées telles quelles

En portrait, les fenêtres flottantes débordaient de l'écran : labwc recadre
une fenêtre agrandie, pas une fenêtre flottante. Un module qui agrandissait
toutes les fenêtres en mode tablette, à la ChromeOS, a été écrit et éprouvé,
puis **retiré** : le portrait fermé, les fenêtres tiennent dans l'écran, et
l'utilisateur préfère garder la main sur chacune.

## 12.5 Une limite de labwc : déplacer au doigt une fenêtre qui se décore elle-même

| Fenêtre | Au doigt |
|---|---|
| Barre de titre dessinée par labwc (applications du shell, Chromium…) : boutons | **Marchent** |
| Même barre : glisser pour déplacer | **Marche** |
| Claude Desktop : ses propres boutons fermer/agrandir/réduire | **Marchent** — ce sont des boutons de l'application |
| Claude Desktop : glisser son en-tête pour déplacer | **Ne marche pas** |

La cause est lue dans la source de labwc 0.8.3 (`src/xdg.c`,
`handle_request_move`) : quand une application demande à être déplacée, labwc
n'accepte que si la fenêtre est celle où un **bouton de souris** est enfoncé
(`seat.pressed.view`). Un toucher sur une surface d'application ne pose
jamais cet état (`src/input/touch.c`), et la demande est ignorée sans un mot.
La branche de développement de labwc a la même condition, déplacée dans
`interactive_begin()`.

Rien à faire du côté du shell. Les remèdes seraient un correctif de labwc, ou
imposer la barre de labwc à Claude Desktop (deux barres empilées). Décision du
11 septembre 2026 : **documenter et passer au clavier.**

Les boutons de barre de titre ont d'abord paru ne pas répondre, écran en
portrait. Revus capot ouvert, ils répondent. **Non établi** : leur réponse au
doigt écran en chevalet (180°).

## 12.6 Le clavier à l'écran — `clavier-ecran.c`, `saisie.c`

Choix de l'utilisateur : un clavier Claude OS, et non squeekboard ou wvkbd.
**Vu fonctionner sur MADOO le 11 septembre 2026**, première version :
apparition au focus d'un champ, disparition à sa perte, renvoi par ⌄,
rappel par l'icône du dock, accents, Maj, Maj+↵ dans Claude Desktop, ⌫ tenu.

**Deux protocoles, chacun pour ce qu'il fait bien** (§12.1) :

- `input-method-v2` ne sert que de SIGNAL : un champ prend le focus, le
  perd, attend des chiffres. Mesuré dans le journal : clavier montré 2 à
  56 ms après la prise de focus, masqué 150 ms après sa perte (délai voulu,
  pour qu'un passage de champ en champ ne fasse pas clignoter).
- `virtual-keyboard-v1` sert à FRAPPER : de vrais événements clavier, que
  toute application comprend. La disposition XKB est **fabriquée** au
  démarrage — une touche par caractère, sans modificateur, « é » et « É »
  chacun la sienne — et **vérifiée par xkbcommon** avant d'être envoyée :
  wlroots n'en dirait rien si elle était refusée, les touches ne feraient
  rien. Seul Maj existe vraiment, pour Maj+Entrée.

Les XML des deux protocoles sont ceux de wlroots 0.18.2 (la version de
labwc), versés dans `shell/protocols/`.

**Claude Desktop et Chromium ne signalent pas leurs champs** : Chromium ne
parle `text-input-v3` que sur option. Le clavier n'y apparaît donc pas seul ;
l'icône « clavier » que le dock porte en mode tablette le fait venir, et il
y écrit normalement. Les options `--enable-wayland-ime` n'ont pas été
essayées.

Le clavier est une surface layer-shell OVERLAY en bas de l'écran, en zone
réservée (les fenêtres agrandies raccourcissent), qui ne prend jamais le
focus clavier (`KEYBOARD_MODE_NONE`).

## 12.7 Le mode console — deux claviers aux bords, et l'écran recadré

Dessiné par l'utilisateur les 11 et 12 septembre 2026, à la façon d'une
console portable : deux claviers collés aux bords gauche et droit, sur toute
la hauteur, et **l'écran recadré entre eux** — labwc rétrécit de lui-même
les fenêtres agrandies, puisque chaque colonne réserve sa largeur.

**Le mode RESTE en place** tant qu'il est choisi et que la tablette est
retournée : il ne va pas et vient au gré des champs, sans quoi toutes les
fenêtres se recadreraient à chaque fois. ⌄ le range jusqu'au champ suivant,
⇆ repasse au plein format ; le choix est gardé dans
`~/.local/state/claude-os/clavier.ini`.

**À gauche la frappe, à droite les bascules** — répartition voulue par
l'utilisateur, gaucher : le pouce gauche ne fait qu'écrire, le droit choisit
ce qui s'écrit (accents, chiffres, symboles), et porte l'espace, ⇧, ⌫, ↵ et
les flèches. Une bascule s'utilise de deux façons : **appui bref** (la
couche s'affiche ; accents et symboles reviennent aux lettres après une
frappe, double appui pour verrouiller ; les chiffres tiennent) ou **appui
tenu** (la couche dure le temps qu'on la tient). Les deux se distinguent au
relâcher, selon qu'une touche a été frappée pendant.

### Les largeurs et la bande sont MESURÉES, pas choisies

`shell/essais/sonde-pouces.c` couvre l'écran d'une surface qui absorbe les
touchers et enregistre les appuis. L'utilisateur tient la tablette et tape
du pouce là où c'est confortable. Mesure du 11 septembre 2026, 245 appuis :

| | Pouce gauche | Pouce droit |
|---|---|---|
| Portée depuis le bord, médiane | 136 px (22 mm) | 164 px (26 mm) |
| 95 % des appuis en deçà de | **262 px** | 320 px |
| Le plus loin | **305 px** | 398 px |
| Hauteur atteinte | y ≈ 180–535 | y ≈ 200–550 |

D'où **300 px (48 mm) par colonne** : la portée du pouce le plus court —
règle de l'utilisateur, pour ne jamais étirer le plus limité. Six appuis
tombaient exactement sur le bord droit, en bas (y ≈ 990) : c'est la main qui
tient, pas un pouce qui tape. **Aucune touche ne va là.**

**Le fichier brut de cette mesure a été perdu** dans un redémarrage — il
était dans `/tmp`. Les paramètres qui en sont tirés sont inscrits dans
`shell/essais/disposition-pouce.py`. Une mesure se verse au dépôt le jour
même.

### La disposition de gauche est calculée, pas héritée

L'AZERTY vient des machines à écrire de 1870 ; il n'a pas été pensé pour la
fréquence des lettres, et encore moins pour un pouce. La disposition de la
colonne gauche a donc été calculée — démarche de BÉPO pour les fréquences,
de Metropolis (Zhai, 2000) pour le pointeur unique :

- **fréquences réelles du français**, Lexique 3.83 pondéré par l'usage :
  e 14,7 %, s 8,4, a 8,1, i 7,2, t 7,0, n 6,8… **é 1,70 %, plus fréquent que
  f, b, g, h, q ou j** — d'où sa place sur le calque des lettres ;
  apostrophe 1,01 % ; enchaînements dominants es, ai, en, le, ou, re, de ;
- **virgule et point** mesurés sur la prose du dépôt (1,51 et 1,40 % des
  lettres) : un dictionnaire n'en contient pas ;
- **zone du pouce** : ellipse ajustée sur les appuis, axe à 70° — l'arc du
  coin haut-gauche au coin bas-droit que décrit l'utilisateur ;
- **coût d'une frappe** = inconfort de la place + trajet depuis la lettre
  précédente (loi de Fitts), pondérés par les fréquences ; recuit simulé.

**39 % de coût en moins** que l'AZERTY replié en 5 × 6, qui mettait n dans
le coin du repli. Deux corrections sont venues du doigt, et non du calcul :
la rangée du bas descendait trop (« mon pouce peine à y descendre »), d'où
des touches de 52 px et une grille remontée à y = 190…470 ; et le centre de
confort a été remonté de 352 à 330, car on tape plus bas pendant une sonde
qu'en écrivant vraiment.

**L'optimum est PLAT** : quatre tirages donnent le même coût à 0,3 % près
avec des places différentes. Ce qui est stable est la hiérarchie — e à la
meilleure place, puis s, a, i, t, n. Le détail peut donc se choisir sur
d'autres critères (mémorisation, repères) sans rien coûter.

Le calcul est refaisable : `shell/essais/disposition-pouce.py`.

**En service depuis le 13 septembre 2026**, pour un essai de plusieurs jours
avant d'ajuster.

## 12.8 Les suggestions de mots — `mots.c`

Demandées par l'utilisateur : « n'importe quel clavier tactile de taille
réduite serait laborieux à l'utilisation sans cette fonction ». Elles
rendent aussi ce qu'on a retiré du calque gauche — l'apostrophe, k, w, les
accents rares : « aujourd » propose « aujourd'hui ».

- **Le dictionnaire** : Lexique 3.83 (CC BY-SA), 119 688 formes du français
  avec leur fréquence d'usage, préparées par `tools/fabrique-mots.py` et
  installées en `mots-fr.txt` (1,5 Mo). Le fichier est trié : `mots.c` y
  cherche un préfixe **par dichotomie dans une projection en lecture seule**
  — rien n'est recopié, les pages restent partagées. Sur 4 Go soudés, cela
  compte.
- **Il apprend** : tout mot choisi est retenu avec son compte dans
  `~/.local/state/claude-os/mots-appris.ini`, et remonte dans la liste. Un
  mot absent du dictionnaire finit par être proposé. L'écriture est différée
  de 20 s : taper cinquante mots ne fait pas cinquante écritures sur l'eMMC.
- **Le mot en cours est celui qu'on a TAPÉ**, pas celui qui est à l'écran :
  le clavier se souvient de ce qu'il a envoyé depuis la dernière espace. Un
  doigt posé ailleurs dans le texte le trompe jusqu'au mot suivant. C'est la
  limite assumée ; elle tomberait si les applications transmettaient leur
  texte alentour (`surrounding_text`, que Chromium n'offre pas sans option).
- **Choisir un mot** efface ce qui a été tapé (autant de ⌫) et écrit le mot
  entier, puis une espace. C'est la seule façon qui marche partout.

**Trois automatismes**, demandés avec la fonction :

| Geste | Ce qui se passe |
|---|---|
| Mot choisi | une **espace** est posée |
| Ponctuation juste après | elle **remplace** cette espace, et une espace la suit : « mot . » devient « mot. » |
| Après `.`, `!`, `?`, une entrée, ou à l'entrée dans un champ | la **majuscule s'arme** seule (sauf mot de passe et champ numérique) |
| Espace tapée après une espace automatique | **ignorée** — la double espace serait une faute à corriger |

## 12.9 Ce qui n'est pas établi

- le stylet sur écran tourné ;
- **la vitesse de frappe réelle du mode console**, et le temps qu'il faut
  pour s'habituer à une disposition qui ne ressemble à rien de connu. C'est
  l'objet de l'essai en cours ;
- **les suggestions à l'usage** : leur utilité réelle, la place de la rangée
  (au-dessus de la zone que le pouce atteint, faute de mieux dans la
  colonne), et si l'apprentissage remonte les bons mots ;
- **l'accent à l'aveugle** : « eleve » ne propose pas « élève », la
  recherche étant exacte. À reprendre si cela gêne ;
- les boutons de barre de titre au doigt, écran en chevalet ;
- la scrutation de la rotation écran éteint : elle continue, deux commandes à
  l'EC par seconde, tant que la machine est en mode tablette. Coût non mesuré.
