# 11. Le lecteur vidéo — `claude-os-video`

Un lecteur pour Claude OS, dont le fil rouge est l'économie d'énergie. Cette
page dit **ce qui a été mesuré**, dans l'ordre où ça l'a été. Ce qui n'est pas
établi y est écrit comme tel.

État au 10 septembre 2026 : **les cinq phases sont faites**, campagne
d'énergie sur batterie comprise. Le lecteur joue, se commande, porte pistes
et sous-titres, et **consomme 0,86 W de moins que Chromium sur le même
fichier** — soit environ trente-cinq minutes de film de plus par charge.

Ce qui n'a **pas** encore été fait : les gestes au doigt, que le banc ne sait
pas produire ; et le lecteur n'est pas installé sur la machine.

---

## 11.1 Ce que la machine impose

| Constat | Conséquence |
|---|---|
| Aucun lecteur ni codec installé au départ — ni mpv, ni VLC, ni ffmpeg, ni GStreamer | Le choix du moteur était entièrement ouvert |
| VA-API opérationnel (iHD 25.2.3) : H.264, HEVC 8 et 10 bits, VP9 profils 0-3, VP8, MPEG-2, JPEG | Décodage matériel pour tout ce qui compte |
| **Pas d'AV1 matériel** — Jasper Lake est Gen11 | AV1 = `dav1d` en logiciel, à annoncer honnêtement à l'écran |
| GTK 4.18.6 : `GtkGraphicsOffload` et `GdkDmabufTextureBuilder` présents | Le chemin sans copie existe nativement, sans OpenGL |
| Écran tactile Goodix multipoint + stylet, et un « Tablet Mode Switch » | L'exigence tactile est réelle |
| 3,7 Gio de RAM, SoC 6 W | Une file d'images courte, et rien de résident |

## 11.2 L'architecture retenue, et pourquoi

```
libavformat (démux)  →  libavcodec + VA-API (décodage matériel)
    →  av_hwframe_map → dmabuf  →  GdkDmabufTexture
        →  GtkGraphicsOffload  →  sous-surface Wayland  →  balayage
audio : pw_stream (PipeWire natif), qui porte l'horloge maître
```

**Exigence « aucun codec à installer », tenue :** les codecs sont *dans*
`libavcodec`. Une fois le lecteur installé, il n'y a plus jamais rien à
ajouter. Coût unique, mesuré : 40 paquets, ~98 Mio.

**Pourquoi pas libmpv.** 151 paquets tirés — X11, Vulkan, Lua, libplacebo —
contre 40, et surtout sa sortie `dmabuf-wayland` **ne s'embarque pas** dans
une fenêtre GTK. Il aurait fallu choisir entre le zéro-copie (mpv seul à
l'écran, sans nos commandes) et son API de rendu OpenGL (le GPU retravaille
chaque image). Les deux étaient contraires à la commande.

**Pourquoi pas GStreamer / `GtkVideo`.** C'est exactement le modèle « chercher
le bon paquet de codecs » que l'exigence écarte.

## 11.3 La sonde — ce qui a été vérifié avant d'écrire une ligne du lecteur

`shell/essais/sonde-offload.c`. Écrite pour trancher **une** question : une
image décodée par le matériel peut-elle arriver à l'écran sans jamais être
recopiée, sous labwc, avec GTK 4.18 ?

**Réponse : oui, et c'est vu.** `GDK_DEBUG=offload` écrit une ligne
`GdkDmabufTexture Attaching` par image. 602 images en 20 s, aucune texture
refusée, **0,83 ms de décodage par image** — le bloc matériel, pas les cœurs.

Ce que la sonde a appris au passage, et qu'aucune supposition n'aurait donné :

- Le tampon sort de VA-API en **NV12, modificateur `0x0100000000000002`**
  (tuilage Y d'Intel), **un objet, deux plans**.
- **FFmpeg exporte deux couches séparées** (`R8` puis `GR88`), là où GTK veut
  *un* fourcc et *N* plans. La traduction est obligatoire, et son absence ne
  produit pas d'erreur : elle produit une image noire.
- **GTK 4.18 ne gère pas l'espace colorimétrique des dmabufs YUV** — il le
  dit lui-même : `FIXME: Implement the proper colorstate for YUV dmabufs`.
  Le tampon est étiqueté `srgb`. **Conséquence à l'écran non encore
  jugée à l'œil** ; à confronter à une mire de couleurs en phase 2.

## 11.4 Les mesures

Banc : `tools/mesure-conso.sh`. Mire : `shell/essais/fabrique-mire.c`, 1080p30
H.264 à 5,5 Mbit/s + AAC 48 kHz, 60 s, **refabriquée à l'identique** — c'est
ce qui rend deux mesures comparables.

### Les instruments, et leurs limites

| | |
|---|---|
| `package-0` | Le SoC seul. **Seul instrument disponible sur secteur.** Il compare des chemins ; il ne dit pas l'autonomie. |
| Batterie | `current_now × voltage_now` : toute la plateforme, écran compris. **La seule mesure d'autonomie** — et elle exige de débrancher. **Moyennée sur tous les échantillons**, et non lue deux fois : le courant varie de plus d'un watt d'une seconde à l'autre, et deux instantanés à trente secondes d'écart ont d'abord rendu `dmabuf` moins gourmand que `offload` à la batterie alors qu'il l'était plus au SoC. Deux instruments qui se contredisent, donc au moins un qui ment. |
| `psys` | **Mesuré inutilisable sur MADOO** le 10 septembre 2026 : le domaine existe, mais `enabled` vaut 0 et le compteur avance de 61 mW pour une plateforme qui en consomme près de sept. Le banc le lit encore, uniquement pour dire qu'il ne compte pas. |

### En fenêtre — repère de repos : 2,92 W

| Chemin | SoC | Écart au repos | CPU |
|---|---|---|---|
| **`offload`** — VA-API → dmabuf → `GtkGraphicsOffload` | **3,24 W** | **+0,32 W** | 27 % |
| `dmabuf` — même texture, composée par GTK | 3,30 W | +0,38 W | 27 % |
| `logiciel` — décodage sur les quatre cœurs | 4,08 W | +1,16 W | 56 % |
| `copie` — matériel, puis `av_hwframe_transfer_data` | 5,45 W | **+2,53 W** | 27 % |

Répétabilité vérifiée : 3,24 / 3,24 · 5,42 / 5,47 · 4,10 / 4,07.

**Le chemin naïf coûte huit fois le chemin retenu.** Et il coûte plus cher que
le décodage logiciel intégral, ce qui est contre-intuitif : la relecture d'une
surface tuilée depuis la mémoire du GPU est lente et chère, sans pour autant
apparaître comme du temps processeur. C'est le résultat le plus utile de la
phase 0 — c'est exactement le code qu'on écrit sans y penser.

### En plein écran — une première série annulée, et pourquoi

Une série avait été prise en plein écran et donnait des chiffres plus bas :
2,23 à 2,40 W pour `offload`. On en avait conclu que le balayage direct
rapportait 0,2 W. **C'était faux, et les mesures avec.**

À 20:14:12 ce jour-là, `claude-os-verrou` s'est monté : la veille progressive
avait éteint le rétroéclairage, puis verrouillé la session. Sous
`ext-session-lock-v1`, le compositeur masque **toutes** les fenêtres. Plus un
« frame callback », plus une image composée, le GPU au repos. Les chiffres
étaient flatteurs, ce qui est la pire espèce d'erreur de mesure — et
l'explication qu'on leur avait d'abord donnée (« le plein écran masque Claude
Desktop ») était une cause plausible pour un phénomène inexistant.

`tools/mesure-conso.sh` lit désormais le rétroéclairage et cherche le verrou
**avant** de mesurer, et **refuse** de rendre un chiffre dans cet état.

## 11.5 La campagne d'énergie — sur batterie, écran allumé

Faite le 10 septembre 2026 à 22 h, machine **débranchée**, écran déverrouillé
au maximum, batterie à 88 %. C'est la seule condition où l'autonomie se
mesure : sur secteur, `current_now` reste à zéro.

**Batterie : 40,0 Wh utiles** (47,4 Wh d'origine, 15 % d'usure).

### Le témoin, et pourquoi il a fallu l'inventer

Comparer une lecture en plein écran au bureau au repos ne mesure pas la
lecture : le plein écran **masque** Claude Desktop et ses 25 % de processeur,
et l'on mesure surtout ce qu'on a caché. Le repère est donc `--fige` : la
même fenêtre, le même plein écran, la même occultation — mais une seule
image, et aucun décodage.

### Ce que coûte le chemin d'affichage (sonde, sans son)

| Chemin | SoC | Batterie | CPU | Coût de la lecture |
|---|---|---|---|---|
| **témoin** — une image figée | 1,71 W | **5,99 W** | 3,6 % | — |
| **`offload`** — dmabuf, sans copie | 2,04 W | **6,44 W** | 7,0 % | **+0,45 W** |
| `dmabuf` — composé par GTK | 2,09 W | 6,52 W | 8,2 % | +0,53 W |
| `logiciel` — quatre cœurs | 2,85 W | 7,68 W | 34,5 % | +1,69 W |
| `copie` — matériel puis recopie | 3,24 W | 8,44 W | 22,6 % | **+2,45 W** |

**Lire une vidéo 1080p coûte 0,45 W à la plateforme par le chemin retenu.**
Le décodage logiciel en coûte 3,8 fois plus, le chemin naïf 5,4 fois plus.

### Contre Chromium — trois paires alternées

Le seul autre moyen de regarder cette vidéo sur cette machine. La
comparaison est honnête : **Chromium décode aussi en matériel** — sa ligne de
commande porte `VaapiVideoDecodeLinuxGL` — et sa lecture a été **vérifiée**,
flux PipeWire à l'état `running`, avant que le chiffre ne soit retenu. Les
deux jouent le son.

**Les essais sont alternés, et ce n'est pas un détail :** la tension d'une
batterie baisse à mesure qu'elle se décharge, donc deux blocs successifs
avantagent le premier. Chaque paire est mesurée dans la foulée.

| Paire | `claude-os-video` | Chromium | écart |
|---|---|---|---|
| 1 | 6,98 W | 7,39 W | 0,41 W |
| 2 | 6,81 W | 7,68 W | 0,87 W |
| 3 | 6,90 W | 8,22 W | 1,32 W |
| **moyenne** | **6,90 W** ± 0,09 | **7,76 W** | **0,86 W** |

**Les trois paires vont dans le même sens, sur les deux instruments.** Le
lecteur est en outre remarquablement stable — 6,81 à 6,98 W — là où Chromium
dérive vers le haut d'une mesure à l'autre.

### Ce que cela donne en heures de film

| | puissance | autonomie sur 40 Wh |
|---|---|---|
| écran allumé, sans vidéo | 5,99 W | 6,7 h |
| **`claude-os-video`** | **6,90 W** | **5,8 h** |
| Chromium, même fichier | 7,76 W | 5,2 h |
| bureau au repos, Claude Desktop visible | 7,72 W | 5,2 h |

**Environ trente-cinq minutes de film de plus par charge**, à contenu
identique et décodage matériel des deux côtés. Et un résultat qui n'était pas
cherché : **regarder un film coûte moins cher que laisser le bureau
affiché** — le plein écran masque l'application Electron qui tournait
derrière.

### Ce qui reste non établi

- Une seule vidéo, une seule définition, un seul codec. H.264 1080p30 à
  5,5 Mbit/s n'est pas tout le monde.
- Trois paires. C'est assez pour un sens, pas pour une décimale.
- Le rétroéclairage était **au maximum** : c'est le premier poste de la
  machine, et il écrase tout le reste dans le chiffre de la batterie. Les
  écarts mesurés sont donc des écarts **malgré** lui.

## 11.6 Le noyau de lecture — phase 1

`shell/src/video-moteur.c`, `video-audio.c`, `video-image.c`, `video.c`.

**Ce qui commande le tempo :** l'audio donne l'heure, le compositeur donne le
rythme d'affichage par le *frame clock* de GTK, et le fil de décodage
travaille trois images d'avance puis s'endort sur une condition. **Aucun
minuteur périodique dans tout le lecteur** — le seul du programme est celui
du parcours automatique de banc, `--scenario`.

Bénéfice obtenu sans une ligne de code : fenêtre masquée, le compositeur
cesse d'appeler, GTK cesse de battre, plus personne ne dépile d'image, et le
décodage vidéo s'arrête de lui-même. Une heuristique de visibilité aurait pu
se tromper ; celle-ci ne le peut pas.

### Ce que le banc constate

Mire 1080p30 H.264, 40 s, banc sans écran :

| | |
|---|---|
| images affichées | 1196, soit 29,9 par seconde |
| images sautées | **2**, toutes deux au démarrage |
| passées par le chemin sans copie | **1196 sur 1196** |
| écart de synchronisation | **7,5 ms** en moyenne, 43 ms au pire |
| horloge d'affichage | 89 Hz |

Le parcours automatique — lecture, pause, reprise, saut à 40 s, saut arrière
de 10 s, retour au début — passe : la position gèle en pause sans dériver, et
chaque saut atterrit à moins de 40 ms de sa cible.

### Deux pièges payés, tous deux muets

**L'horloge audio était un escalier.** `pw_stream_get_time_n` ne rend qu'un
instantané, actualisé une fois par cycle du graphe — 42,7 ms avec le quantum
long choisi ici. Sans extrapoler l'âge de cet instantané, l'horloge avance par
marches de 42,7 ms, ce qui couvre 1,28 image à 30 im/s : **une fois sur
quatre, deux images deviennent dues au même battement et l'une est sautée.**
Résultat mesuré avant correction : 23 images par seconde au lieu de 30,
6,7 sauts par seconde, et un écart de synchronisation de 16 ms. Après
correction : 29,9 images par seconde, 2 sauts en 40 secondes, 7,5 ms d'écart.
Le symptôme — une saccade parfaitement régulière — désignait le décodeur, qui
n'y était pour rien. L'en-tête `stream.h` de PipeWire documente
l'extrapolation ; il fallait la lire jusqu'au bout.

**Un saut ne tombe que sur une image-clé.** Avec un groupe d'images d'une
seconde, viser 32,9 s faisait commencer à 32,0 s. Invisible sur un bouton
« −10 s », très visible en tirant une glissière au doigt. Le moteur décode
donc de l'ancrage jusqu'à la cible en jetant ce qui précède — au pire un
groupe d'images, 25 ms.

**Et un troisième, de méthode :** pendant une heure, le lecteur a paru mort —
une seule image, aucun battement — alors qu'il fonctionnait. C'est l'écran
qui était éteint puis verrouillé. La sonde de la phase 0, elle, continuait à
décoder dans le vide, parce qu'elle était cadencée par un minuteur : la
comparaison des deux comportements est ce qui a fini par désigner le
compositeur. **Un lecteur qui s'arrête quand personne ne regarde est un
lecteur qui marche.**

## 11.7 L'interface — phase 2

```
+-------------------------------+
|            vidéo              |  sans aucune bordure : ni cadre, ni marge,
|                               |  ni coin arrondi, ni ombre
+-------------------------------+
|        (espace vide)          |  <- au survol, la glissière apparaît ICI
|     ( o====|--------- )       |
+-------------------------------+
|     ( capsule des commandes ) |
+-------------------------------+
```

L'espace vide n'est pas une marge : c'est une **zone sensible**. La glissière
y apparaît en fondu **par-dessus** le vide — une `GtkOverlay`, jamais une
boîte : toute autre disposition ferait sauter la vidéo de quelques pixels à
chaque passage du pointeur.

**Au doigt, le survol n'existe pas.** Un toucher n'importe où révèle la
glissière, et c'est seulement une fois les commandes visibles qu'un appui sur
l'image met en pause. Sans cette règle, toucher l'écran pour voir où l'on en
est arrêterait le film. Les commandes se retirent seules après quatre
secondes — le seul minuteur du lecteur, à un coup.

Commandes : position et durée, ±10 s, lecture/pause, sourdine, volume, plein
écran. Clavier : espace, flèches, Origine, M, F, F11, Échap, Q. Cibles de
44 px, 52 px pour la lecture.

Trois détails qui ne se voient qu'à l'usage, et qui sont tous des règles
d'énergie appliquées :

- l'étiquette de position n'est réécrite que quand la **seconde** change —
  sinon elle serait reconstruite quatre-vingts fois par seconde pour afficher
  le même texte, et chaque réécriture réveille le compositeur ;
- la glissière n'est mise à jour que si elle est **visible**, et jamais dans
  les 300 ms qui suivent un geste, sans quoi le curseur saute sous le doigt ;
- les largeurs de temps sont fixées en caractères : sinon la capsule change
  de taille au passage de 9 à 10 secondes et les boutons se décalent.

**Le plein écran se lit sur la propriété `fullscreened`**, pas sur le bouton :
labwc garde pour lui la touche du Chromebook. Même leçon que la visionneuse.

### Comment cela a été jugé, écran verrouillé

`banc-video.sh --capture=fichier.png` capture l'écran du compositeur
imbriqué par `grim`, et `--revele` force la glissière visible faute de
pointeur dans un banc sans écran. C'est par là que la mise en page a été vue.

Vérifié au passage, et ce n'était pas acquis : **l'ajout des widgets n'a rien
coûté au chemin sans copie** — 451 images sur 451 encore confiées au
compositeur. Un `GtkGraphicsOffload` cesse d'être pris dès que son contenu est
rogné ou recouvert ; la capsule est en dessous, pas au-dessus.

### Ce que l'usage a corrigé, le 10 septembre au soir

Six retours, tous constatés sur la machine, aucun visible au banc.

| Constat | Ce qui a été fait |
|---|---|
| La fenêtre a une bordure et une barre de titre | `set_decorated(FALSE)` et fond **transparent** : il ne reste que l'image, la capsule et la glissière. La fenêtre se déplace en tirant la capsule — un `GtkWindowHandle` — ou à l'Alt-glisser de labwc |
| Le double appui met en pause au lieu du plein écran | L'appui simple est **retardé** du temps du double clic (borné à 300 ms) ; un second appui l'annule et prend le plein écran. Le retour visuel, lui, reste immédiat |
| En plein écran la capsule reste affichée | Elle s'efface en entrant, revient au mouvement de pointeur ou à l'appui, et se retire seule |
| Les boutons ±10 s ne servent à rien | Remplacés par **vidéo précédente / suivante**, sur le dossier courant. Les flèches du clavier font toujours le saut |
| Le sélecteur s'ouvre **derrière** la fenêtre | Il était créé avant que la fenêtre ne soit affichée. Ouvert au `map`, modal et transitoire |
| Le sélecteur ne montre pas les lecteurs réseau | Ils sont montés sous `/run/claude-os/reseau/`, que `g_unix_mount_guess_should_display()` ne retient pas. Ajoutés à la main, lus dans `/proc/mounts` |

**Le plein écran ne remet pas les commandes dans la pile.** Elles flottent
toujours dans une `GtkOverlay` ; ce qui change est la marge basse de l'image
— la hauteur des commandes en fenêtre, zéro en plein écran. Les remettre
dans le flux ferait sauter l'image de cent pixels à chaque mouvement de
souris.

**`GtkFileDialog` ne sait pas ajouter un raccourci de dossier.** L'API
recommandée depuis GTK 4.10 n'expose que le dossier initial ; avec elle, les
lecteurs réseau resteraient hors d'atteinte. On garde donc
`GtkFileChooserDialog`, déprécié mais capable, et on le dit dans le code
plutôt que de le subir.

**Et un piège de méthode :** `gtk_window_present()` affiche la fenêtre
*pendant* l'appel. S'abonner à `map` juste après, c'est s'abonner à un
signal déjà passé — la boîte d'ouverture n'arrivait jamais, et l'application
restait sur une fenêtre vide. Trouvé à la capture d'écran, pas au
raisonnement.

## 11.8 Pistes, sous-titres, reprise — phase 3

Un menu dans la capsule liste les **pistes audio** et les **sous-titres**,
nommés par leur langue ou leur titre — « Piste 2 » à défaut. Changer de piste
rouvre le décodeur puis **se recale sur la position courante** : sans ce
recalage, le nouveau décodeur repart là où le démux se trouve, quelques
secondes plus loin que ce qu'on regarde.

Les sous-titres **ne s'activent pas d'office**, et le menu ne propose une
section que s'il y a vraiment un choix à faire.

**Mesuré, et ce n'était pas acquis :** l'étiquette de sous-titre posée sur
l'image **ne coûte rien** au chemin sans copie — 296 images sur 296 encore
confiées au compositeur, sous-titres affichés. Elle reste **cachée**, et non
vide, tant qu'il n'y a rien à dire : un widget vide mais visible aurait suffi
à faire renoncer GTK.

**HEVC 10 bits vérifié.** C'est le seul format qui produit du **P010** au lieu
du NV12, donc le seul qui éprouve cette branche de la traduction de
disposition dmabuf — jamais exécutée avant. Mire 10 bits fabriquée pour
l'occasion : décodage matériel, `fourcc P010`, 206 images sur 206 sans copie,
image juste en capture.

**Reprise à la position quittée**, avec trois garde-fous : rien sous deux
minutes de film, rien sous trente secondes de lecture, rien dans la dernière
minute. Et la reprise **se montre** — les commandes apparaissent quelques
secondes, la glissière dit où l'on est. Un lecteur qui repart au milieu sans
rien dire donne l'impression de s'être trompé de fichier.

### Trois pièges payés sur les sous-titres, tous muets

- **FFmpeg rend les sous-titres texte en ASS sous deux formes** : l'ancienne
  avec `Dialogue:` en tête, l'actuelle sans. Neuf virgules avant le texte dans
  un cas, **huit** dans l'autre. Ne traiter que la première affiche
  `0,0,Default,,0,0,0,,seconde 3` à l'écran — le décodage est parfait, seule
  la lecture du format est fausse.
- **`end_display_time` est nul en Matroska** : la durée est dans le paquet.
  Tomber sur le repli de trois secondes faisait se chevaucher trois
  répliques, ce qui ressemble à un défaut de synchronisation.
- **La mire écrivait ses sous-titres en bloc à la fin.** Ils se retrouvaient
  physiquement en fin de fichier ; un lecteur séquentiel ne les rencontre
  qu'après la vidéo entière. Le décodeur s'ouvrait, la piste était annoncée,
  et l'écran restait vide sans la moindre erreur.

### Ce que les détecteurs ont dit, et ce qu'ils n'ont pas pu dire

`CLAUDE_OS_SANITIZE=adresse` (ou `fils`) devant `construire.sh`.

**AddressSanitizer : rien**, sur un fichier joué en entier, sous-titres
affichés, avec le parcours pause/reprise/sauts.

**ThreadSanitizer est inutilisable tel quel sur ce code, et c'est une limite
de l'outil, pas un résultat.** `GMutex` de GLib est bâti sur des futex que
ThreadSanitizer ne sait pas voir : **tout** accès pourtant protégé par un
verrou lui apparaît comme une course. Ses centaines d'avertissements ne
distinguent donc pas le vrai du faux, et les premiers rapports portaient sur
`gdbus` et `malloc` à l'intérieur de GLib.

Sa lecture a tout de même servi : elle a poussé à relire les accès partagés
un par un, et **trois vraies fautes** en sont sorties, qu'aucun essai
n'aurait révélées :

- `video_moteur_sous_titre()` rendait **le pointeur interne** du texte
  courant. Le fil de décodage le libère sur un saut : entre le retour de la
  fonction et l'affichage, la chaîne pouvait disparaître. Un usage après
  libération qui ne se produit qu'en sautant pile au changement de réplique
  — donc jamais pendant les essais, et un jour chez l'utilisateur. Rend
  désormais une copie, et seulement quand le texte change.
- La **position et l'horloge** étaient lues sans le verrou alors que le fil
  de décodage les récrit à chaque saut.
- **La liste des pistes** était bâtie sans le verrou pendant que le fil
  pouvait remplacer la piste active : deux pistes cochées, ou aucune.

Et un quatrième défaut, trouvé lui en laissant simplement un fichier aller
**jusqu'au bout** : c'est `video_moteur_image_due()` qui constate la fin et
appelle le rappel de fin, lequel peut fermer la fenêtre — donc détruire les
widgets et le moteur — pendant que le battement continue de s'en servir.
`gtk_label_set_text: assertion GTK_IS_LABEL failed`, juste après le bilan.

## 11.9 Les huit règles d'énergie du lecteur

1. **Zéro scrutation** — pas un minuteur périodique. L'horloge de l'interface
   est l'image présentée.
2. **En pause, plus un seul réveil.**
3. **Fenêtre occultée : décodage vidéo suspendu.** Wayland cesse d'envoyer les
   *frame callbacks* ; c'est gratuit et exact.
4. File d'images courte — 3,7 Gio soudés.
5. Les commandes ne réinvalident jamais la surface vidéo.
6. Inhibiteur d'inactivité **en lecture seulement**, relâché en pause.
7. Rien de résident : pas de démon, pas de vignettes, pas d'indexation.
8. **Aucun chiffre annoncé sans mesure.**

## 11.10 Les instruments, et comment s'en servir

```sh
bash shell/essais/construire.sh                 # les deux programmes d'essai
./shell/essais/build/fabrique-mire mire.mp4 60  # h264 (défaut) | hevc | hevc10 | vp9
# Une sortie en .mkv ajoute une piste de sous-titres, une ligne par seconde.

# Le chemin sans copie est-il pris ? Chercher « Attaching » :
GDK_DEBUG=offload ./shell/essais/build/sonde-offload mire.mp4 --mode=offload

# Le lecteur, sur un compositeur sans écran — utilisable écran éteint ou
# verrouillé, ce qui est précisément quand tout le reste devient trompeur :
bash shell/essais/banc-video.sh mire.mp4 40
bash shell/essais/banc-video.sh mire.mp4 30 --scenario   # pause, sauts, reprise

bash shell/essais/banc-video.sh mire.mp4 10 --capture=/tmp/vu.png --revele

bash tools/mesure-conso.sh -d 30 --contre "…" "libellé"
bash tools/mesure-conso.sh --tableau            # tous les relevés passés
```

Les mires vivent dans `~/.local/share/claude-os/mires/` et ne sont pas dans le
dépôt : elles se refabriquent.

## 11.11 Ce qui reste à faire

| Phase | Objet | État |
|---|---|---|
| 0 | Banc de mesure, sonde, choix d'architecture | **fait, mesuré** |
| 1 | Noyau de lecture : démux, décodage, audio PipeWire, synchro | **fait, éprouvé au banc sans écran** |
| 2 | L'interface : vidéo sans bordure, capsule, glissière au survol, tactile | **faite, vue en capture** |
| 3 | Pistes audio, sous-titres texte, reprise à la position | **fait, vu en capture** |
| 4 | Campagne d'énergie **sur batterie** | **faite, 10 septembre 2026 au soir** |

Points ouverts, à ne pas oublier :

- **RIEN N'A ÉTÉ VU SUR LE VRAI ÉCRAN**, ni entendu. Tout a été jugé sur des
  captures du compositeur sans écran. Restent à confirmer sur MADOO : les
  couleurs, la fluidité perçue, le son, et surtout **les gestes au doigt** —
  que le banc ne peut pas produire.
- **LE LECTEUR N'EST PAS INSTALLÉ.** Il est dans `meson.build` et compile
  sans un avertissement, mais `--compiler` réinstalle *tout* le shell et
  l'autre instance travaille dans le même dépôt. À lancer quand vous serez
  devant la machine :

  ```sh
  sudo bash install/bascule-session.sh --compiler
  ```
- **L'espace colorimétrique YUV de GTK** (§11.3) — non jugé à l'œil, et c'est
  la première chose à regarder quand l'écran sera disponible.
- **Les gestes au doigt** n'ont toujours pas été éprouvés : le banc ne sait
  pas les produire.
- **La vitesse de lecture n'est pas faite, et c'est délibéré.** La faire
  correctement demande de conserver la hauteur du son — donc `atempo` de
  libavfilter, donc une dépendance de plus. La faire en changeant le taux
  d'échantillonnage coûterait une ligne et donnerait des voix de dessin
  animé. À trancher avant de l'écrire.
- **Les sous-titres graphiques** (PGS, VobSub) sont détectés et annoncés au
  journal, pas affichés : ce sont des images, et cela demande un autre
  chemin.
- **VP9 et HEVC 8 bits** n'ont pas été éprouvés faute de temps d'encodage ;
  ils empruntent le même chemin NV12 que H.264, déjà vérifié.
- **AV1** n'a pas encore été mesuré sur cette machine ; il sera le pire cas.
- **La reprise après suspension est cassée** sur MADOO (voir `CLAUDE.md`) : un
  plantage au réveil ne devra pas être imputé au lecteur.
