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

### Quand le dock et la barre sont à l'écran

Depuis le 11 septembre 2026, ils **sortent de l'écran par le bas** dès qu'on
travaille dans une application, et ne reviennent qu'à la demande :

| Ce qui se passe | Effet |
|---|---|
| une fenêtre est activée — clic dessus, application qui s'ouvre, Alt-Tab | le dock et la barre descendent hors de l'écran |
| touche **Loupe** | ils remontent ; un second appui les renvoie |
| court **glisser du doigt** depuis le bord bas (5 mm suffisent) | ils remontent |
| clic **à côté** alors qu'on les a rappelés par-dessus une application | ils repartent — ce clic-là n'atteint pas l'application |
| clic sur l'icône de l'application déjà active | ils repartent |
| **plus aucune fenêtre active** — bureau vide, tout réduit | le dock revient seul : c'est le seul moyen d'aller ailleurs |
| une **notification** arrive pendant qu'ils sont partis | la barre seule remonte le temps de la bannière |

Le dock mène : lui seul suit les fenêtres, et il dit à la barre « afficher »
ou « masquer » sur le bus. La règle et ses trois états sont dans
`shell/src/visibility.h`, la mécanique dans `shell/src/dock.c` (« À l'écran
ou non »).

Quatre limites connues, toutes voulues :

- **La bande du bord prend les dix derniers pixels** de l'écran (1,6 mm)
  quand le dock est caché : un appui qui commence là ne va plus à
  l'application dessous. C'est le prix du geste — labwc 0.8.3 n'a pas de
  geste de bord, un client ne voit que les doigts posés sur ses propres
  surfaces.
- **Rappelé par-dessus une application, un clic à côté est consommé** : il
  renvoie le dock, il ne clique pas dans l'application. C'est le geste du
  panneau qu'on ferme en cliquant à côté. Si la Console est ouverte, le
  premier clic la ferme, le second renvoie le reste.
- **Dans la bande basse (86 px), un clic à côté n'est pas consommé** : il
  atteint l'application sans renvoyer le dock. C'est ce qui garde la barre
  touchable quel que soit l'ordre dans lequel le compositeur a empilé les
  deux processus.
- **Le dock et la barre sont en couche OVERLAY**, au-dessus du plein écran.
  En TOP, labwc les éteignait sous une fenêtre plein écran — mesuré au banc —
  et ni la Loupe ni le doigt n'auraient pu les rappeler pendant une vidéo.

### Le coin — ce que la machine dit d'elle en permanence

Il a remplacé la barre d'état le 15 septembre 2026, et il ne lui ressemble
pas. `shell/src/coin.c`, dans `claude-os-status`.

La barre était une pilule opaque, qu'on cliquait pour ouvrir la Console et
qui sortait de l'écran dès qu'une application passait au premier plan. Les
trois propriétés sont abandonnées :

- **Plus de surface.** Ni fond, ni bordure, ni ombre : des tracés clairs
  posés sur le fond d'écran. Ce qui reste à l'écran en permanence ne doit pas
  y occuper de place. Même jeton `@avis` et **même opacité** que les avis
  système, par le même réglage — une seule voix, sinon le bureau a deux
  blancs.
- **Plus de clic, ni de survol.** Région d'entrée vide et mode clavier
  « aucun ». C'est la contrepartie exacte de la permanence : une surface qui
  reste là pour toujours et qui capterait le pointeur poserait un rectangle
  mort définitif dans le coin du bureau. La région est **reposée à chaque
  nouvelle disposition** — par `GdkSurface::layout`, GTK 4 ne publiant plus
  `size-allocate` — parce que la taille du coin change avec l'heure et avec
  le pourcentage.
- **Plus de disparition — à une exception près.** Il ne suit plus le dock
  hors de l'écran : une information permanente qui s'absente dès qu'une
  fenêtre s'ouvre n'est pas permanente. Mais **il s'efface sous une fenêtre
  plein écran**, et cette exception a été concédée à contrecœur, à l'usage :
  une heure posée sur un film n'est plus un service, et les commandes de
  lecture de Netflix vivent exactement en bas à droite — deux tracés clairs
  l'un sur l'autre, illisibles tous les deux. Il n'y a pas d'arrangement
  possible ; l'un des deux doit partir, et ce n'est pas au film de s'effacer.

  La question posée est « quelque chose couvre-t-il l'écran », pas « qui a le
  clavier » : **toute** fenêtre plein écran non réduite compte, et pas
  seulement l'active — une vidéo qui perd le focus continue d'occuper l'écran.
  C'est `shell_toplevels_plein_ecran()`, donc
  `wlr-foreign-toplevel-management-v1`, qui **ne consulte rien** : le
  compositeur prévient quand un état change et se tait le reste du temps.
  La fenêtre est masquée, pas vidée ni déplacée — une surface layer-shell
  masquée ne coûte plus ni composition ni mélange de sa transparence, ce qui
  est précisément ce qu'on veut pendant une lecture vidéo.

  **Mesuré** sur les trois états, en relevant le contraste dans le bloc du
  coin : bureau `9–255` (présent), plein écran `249–255` (absent), retour
  `9–255`.

  **Les avis système, eux, restent.** Ils sont fugaces et ce qu'ils annoncent
  — « Batterie critique » — vaut d'interrompre un film. Le compte à rebours
  de la veille, lui, ne se déclenche pas : un lecteur vidéo tient un
  inhibiteur d'éveil.

**Ce qu'il montre, et dans quel ordre.** À gauche l'heure et la date, les
seules choses qu'on vient vraiment y lire — la date en toutes lettres, la
pilule n'étant plus là pour imposer une abréviation. Par-dessus leur droite,
en grand, **le mode d'énergie** : c'est le seul réglage du bureau dont
l'effet se produit quand on ne regarde pas, et le seul qu'on doive donc
pouvoir vérifier sans rien ouvrir. À droite, en colonne : non-lu, réseau,
Bluetooth, charge — et la fiche secteur quand elle est branchée.

Le glyphe du mode est **superposé** et non rangé à côté : `GtkOverlay` ne
mesure pas son enfant superposé, le glyphe occupe donc le vide au-dessus de
la date sans coûter un pixel de largeur.

**Trois glyphes de mode, et ce sont les nôtres.** C'était la famille
`power-profile` de GNOME : trois cadrans que seule l'inclinaison d'une
aiguille distinguait. Cela suffisait dans une rangée de Console où les trois
se voient côte à côte sous leur nom. Le coin n'en montre qu'**un**, en grand,
sans libellé : il faut alors que chacun se reconnaisse seul. Trois objets
différents, donc — **le cadran pour Travail, la balance pour Automatique, la
feuille pour Nomade**. C'est la famille d'Adwaita, celle que le bureau
portait avant d'avoir son propre jeu d'icônes, redessinée dans la grammaire
du projet. Livrés avec le shell et préfixés `claude-os-`, comme la cloche :
qui bascule sur Adwaita garde ses trois modes.

**Un badge « AUTO » avait été essayé**, puis écarté : dire la chose par un
mot est l'aveu qu'on n'a pas trouvé l'image, et quatre lettres deviennent
illisibles dès qu'on descend à la taille de la Console. La balance dit
« l'équilibre » sans être lue.

**Le glyphe est plus effacé que l'heure** — 0,62 — et c'est ce qui les
sépare. Les deux se chevauchent et partagent la même encre ; à opacité
égale, le glyphe se lisait comme un trait de plus dans les chiffres. Reculé
d'un cran, il passe derrière l'heure sans qu'on ait eu à l'écarter : le
recouvrement reste, la confusion part. L'opacité se **multiplie** avec celle
du réglage des avis système, qui continue de commander l'ensemble.

**RIEN NE BOUGE, ET C'EST MESURÉ.** Le coin est ancré à droite : sa largeur
suit son contenu, donc son bord gauche recule ou avance à chaque changement.
Trois choses bougeaient, et les trois sont fermées :

- **L'heure.** En chasses proportionnelles, « 11:11 » est plus étroit que
  « 10:00 ». La boîte heure/date prend la plus large de ses deux lignes, et
  selon la minute c'était l'heure ou la date qui l'emportait — le bloc entier
  sautait **une fois par minute**. Chiffres tabulaires
  (`font-feature-settings: "tnum"`), et la question est close.
- **La date.** « mardi 15 septembre » et « mercredi 1 octobre » n'ont pas la
  même longueur. La boîte reçoit donc une largeur **fixe, mesurée** : la plus
  large date de l'année, obtenue en formatant 28 jours consécutifs — les sept
  jours de la semaine — sur douze mois, soit 336 mesures d'une chaîne courte,
  une fois. Mesurée et non écrite en dur : elle dépend de la police, qui se
  règle.
- **La fiche secteur**, qui apparaissait et disparaissait de la colonne. Elle
  est désormais posée à **opacité zéro** plutôt que masquée : un widget
  masqué ne reçoit plus d'allocation, et la batterie glissait de vingt pixels
  à chaque branchement. La place est réservée en permanence ; elle se remplit
  ou reste vide.

Le pourcentage a la même cure : quatre caractères réservés — « 100 % » est le
plus large — et les mêmes chiffres tabulaires.

**Ce qui est éteint s'efface, il ne disparaît pas.** Une icône qui s'en va
fait sauter la colonne d'un cran, et l'œil croit qu'autre chose a changé.

**La cloche n'est qu'un témoin.** Elle dit qu'il reste du non-lu ; elle
n'ouvre plus rien, puisque rien ici ne s'ouvre.

**Le Bluetooth est la seule chose que le coin lit lui-même.** Le réseau vient
de `status.c`, qui tient un proxy NetworkManager permanent ; la charge vient
de `batterie.c`. Le Bluetooth n'avait qu'un observateur — la tuile de la
Console — et `panel.c` ne cherche l'adaptateur qu'au premier affichage du
panneau, à dessein. Un témoin permanent demande une source permanente : le
coin ouvre donc son propre proxy sur `Powered`.

### Les tiroirs des bords latéraux

`shell/src/tiroir.c`. **Un volet par bord**, tiré de son bord, venant
affleurer le cadre — coins arrondis du seul côté intérieur, aucune marge du
côté du bord. C'est ce qui les fait lire comme des tiroirs qu'on sort, et non
comme des cartes posées près des bords.

- **À gauche, les widgets à venir**, sur toute la hauteur. Le volet est vide,
  et il le dit : une place réservée qu'on voit est une promesse, une place
  absente est un oubli.
- **À droite, la Console**, **centrée verticalement** et de sa seule hauteur —
  étirée, elle laisserait la rangée d'alimentation flotter au bas d'un grand
  vide. Sa hauteur (≈ 460 px sur 1080) la tient loin du coin ; le centrage
  n'a donc pas besoin d'une marge basse pour l'éviter.

Les deux volets ont **la même largeur**, celle de la Console (`.qs
{ min-width: 296px }`), pour que les deux bords se répondent.

**Les widgets ont quitté le bord droit le 15 septembre 2026**, où ils
partageaient une colonne avec la Console : deux choses sans rapport empilées
au même bord se lisaient comme une seule, et la Console, poussée en bas par un
volet vide, n'était centrée sur rien.

**Deux façons de les ouvrir, et elles ne se valent pas.**

- **Au doigt** : un glissé depuis le bord vers l'intérieur de l'écran, seuil
  de 32 px. Le geste de tous les tiroirs latéraux. **Le sens compte** : un
  glissé qui s'éloigne de l'écran n'ouvre rien.
- **Au pointeur** : le curseur **posé** contre le bord et tenu là **une
  seconde**. Pas un clic, pas une entrée : une attente. Les bords latéraux
  sont l'endroit où finit tout mouvement de souris un peu vif, et un tiroir
  qui s'ouvrirait au contact s'ouvrirait surtout par accident. La minuterie
  est réarmée à **chaque mouvement dans la bande** : tant que le curseur
  bouge, le compte repart de zéro.

**Tout clic ailleurs les referme**, par la nappe — une surface transparente
qui couvre l'écran tant qu'un tiroir est ouvert. Même mécanisme que le dock
rappelé par-dessus une application, et pour la même raison : labwc ne signale
rien quand on revient à la fenêtre déjà active.

**Les deux côtés s'ignorent, mais partagent LA fenêtre.** Chacun a sa
lisière, sa minuterie et son révélateur ; une seule fenêtre plein écran porte
la nappe, parce que deux nappes superposées se seraient disputé le clic
extérieur et que l'une des deux aurait fermé le mauvais tiroir. La fenêtre est
montrée dès qu'un côté s'ouvre, masquée quand le dernier est rentré — et
seulement alors : la minuterie de retrait relit l'état des deux.

**Conséquence à connaître : tant qu'un volet est dehors, l'autre bord
n'ouvre rien.** Les lisières sont créées avant la fenêtre du tiroir, donc
sous elle ; se poser contre le bord opposé ne fait que toucher la nappe. C'est
vérifié au banc (`shell/essais/banc-tiroirs.sh`, étape 7) et c'est voulu : le
geste qui suit l'ouverture d'un tiroir est presque toujours de le refermer.

**La fenêtre du tiroir est plein écran dès sa création**, et c'est un
`GtkRevealer` qui bouge. Redimensionner une surface layer-shell qui porte un
popover ouvert fait partir ce popover hors de l'écran sous labwc 0.8.3 —
règle payée le 11 septembre 2026 — et la Console ouvre des popovers.

**La nappe et les volets sont séparés par un `GtkOverlay`**, pas par un test
dans un gestionnaire de clic. Les deux révélateurs y sont posés en
superposition, la nappe en enfant principal. Un geste posé sur le conteneur aurait attrapé
les deux ; avec une superposition, GTK désigne le widget le plus haut sous le
pointeur — le volet s'il y en a un, la nappe sinon. La distinction n'est pas
codée, elle est structurelle.

**La Console n'est plus un popover.** `panel_new()` rend le contenu, et la
relecture périodique suit `map` et `unmap` plutôt que `show` et `closed` :
ces deux signaux disent exactement « la Console est à l'écran », ce que
l'ouverture d'un popover ne garantissait pas. La rangée d'alimentation reçoit
un **rappel de fermeture** au lieu d'un widget — elle dit « ferme-toi » et ne
sait pas à quoi elle parle.

**Ce que cette bascule a coûté, et qui est assumé :** le centre de
notifications n'a plus d'entrée. La cloche de la barre l'ouvrait ; le témoin
du coin ne s'ouvre pas. L'historique attend qu'un widget du **volet de
gauche** lui redonne une porte. **La bannière, elle, continue d'annoncer ce qui arrive** —
elle s'accroche au coin comme elle s'accrochait à la pilule, et c'était la
moitié qu'on ne pouvait pas perdre.

### Le centre de notifications

La cloche l'ouvre, au-dessus de la Console si elle est ouverte — les deux
coexistent. **Un clic à côté le ferme** : une surface transparente, la
« nappe », recueille ce clic, et laisse passer ceux qui visent la barre
elle-même. La cloche, allumée à l'accent, signale du non-lu.

### Les avis système

Une surface unique, **au centre de l'écran et juste au-dessus du dock**, pour
tout ce que le système a à dire en passant. Elle montre aujourd'hui deux
choses : le **compte à rebours** avant que l'écran ne baisse, et de **courts
messages** — « En charge » au branchement, « Batterie faible » à un seuil.
`shell/src/avis.c`, dans `claude-os-status`.

**C'est le lieu qui fait l'avis, pas le module qui l'émet.** Un signal
périphérique ne vaut que si l'œil sait d'avance où le trouver : deux coins
d'écran différents pour deux messages du même genre obligeraient à chercher,
ce qui est exactement le contraire du service rendu. Tout ce qui passe par là
partage donc la même place, la même couleur, la même opacité — réglable dans
le panneau Énergie — et la même absence de prise.

**Ni clic, ni survol, ni focus.** Mode clavier « aucun », pour qu'une frappe
en cours ne soit jamais interceptée, et **région d'entrée vide**, pour que la
surface ne pose pas un rectangle mort par-dessus le bureau. Elle se voit et ne
s'attrape pas. La région est reposée à chaque affichage : la fenêtre ne
changeait jamais de taille tant qu'elle ne montrait qu'un cadran, elle passe
maintenant d'un disque de 168 px à une ligne de texte et retour.

**Blanche.** La teinte vient du jeton `@avis`, blanc dans les quatre thèmes :
un avis n'appartient à aucune application et ne colore rien, il éclaire. Un
accent l'aurait rattaché au thème, un gris l'aurait fait passer pour éteint.
**La contrepartie est réelle et doit être connue** : posée sur une fenêtre
claire — et le centre de l'écran en porte souvent une, ce que le coin bas
droit évitait — une trace blanche se lit mal. Le remède disponible est
l'opacité, qui se règle ; une ombre portée relèverait la lisibilité mais
contredirait le choix, documenté et tenu, de n'avoir ni ombre ni fond.

**UNE OMBRE PORTÉE SOUS CHAQUE ÉLÉMENT, ET C'EST CE QUI REND LE BLANC
LISIBLE.** Le coin écrit en blanc sur ce qui se trouve dessous. Sur un fond
d'écran cela va de soi ; sur **une page web blanche en plein écran**, le
blanc écrit sur du blanc et le coin disparaît.

**Deux ombres par élément**, sur le modèle de celles du dock : une courte et
dense décalée d'un pixel vers le bas, qui donne le contour, et une large sans
décalage, qui pose le halo. L'une sans l'autre donne soit un liseré dur, soit
un flou qui ne détache rien. `text-shadow` pour les libellés,
**`-gtk-icon-shadow`** pour les icônes — GTK 4 a renommé la propriété, et
l'ancien nom `icon-shadow` est refusé au chargement de la feuille sans que
rien d'autre ne s'arrête ; les deux ont été soumis au parseur avant d'écrire
la règle.

**UN VIGNETTAGE AVAIT ÉTÉ ESSAYÉ D'ABORD, en deux temps, et écarté.** C'est
la piste qui vient naturellement, et elle a deux défauts qu'il faut connaître
avant de la reprendre :

- **Court, il se lit comme une tache.** Le premier essai tenait dans 500 px
  et culminait à 0,55 d'opacité. Il faisait son travail — 4,7:1 mesurés sous
  l'heure sur une page blanche — mais son bord se voyait : une pastille grise
  posée dans le coin.
- **Long, il occupe l'écran.** Pour s'éteindre sans qu'on voie où, il lui
  fallait près de **800 px de course** en diagonale et cinq paliers. La
  transition devenait invisible, mais le voile couvrait alors un quart de
  l'écran et tirait l'œil — et à une densité assez basse pour ne plus gêner
  (0,30), il ne donnait plus que 1,6:1 sous l'heure.

**Les deux exigences ne se cumulent pas pour un voile de région** : assez
dense pour porter du blanc sur du blanc, il se voit. Une ombre portée, elle,
obtient le même détachement **sur quelques pixels** et ne prend aucune place
— mesuré, le gris de la page reste à 255 dès 200 px du coin. C'est ce que
font les sous-titres, et pour la même raison : elle suit le glyphe au lieu
d'assombrir la région.

Le thème dit **de quelle couleur** l'ombre est faite — `@ombre-encre`, noir
dans les quatre, par nécessité et non par goût, puisque l'encre du coin est
blanche partout. Le **combien** vient de `shell.conf`, groupe `[appearance]`,
relu à chaud comme tout le reste :

| clé | défaut | ce qu'elle fait |
|---|---|---|
| `ombre_opacite` | 62 | l'opacité des deux ombres, en pourcent |
| `ombre_flou` | 5 | le rayon du halo, en pixels |
| `ombre_contour` | 2 | le rayon de l'ombre courte |
| `ombre_decalage` | 1 | de combien l'ombre courte descend |

On écrit, on enregistre, le coin change sous les yeux — sans recompiler.
**Ce n'est pas dans le panneau de réglages, et c'est délibéré** : on y règle
des habitudes, pas des détails de dessin. « Rayon de diffusion de l'ombre »
n'est pas un choix d'utilisateur, c'est un choix qu'on fait une fois, à
l'œil, sur son propre fond d'écran — après quoi il devient le défaut et
personne n'y revient.

**LE GLYPHE DU MODE EST DERRIÈRE L'HEURE, et l'ombre l'a prouvé.** Il était
l'enfant *superposé* de la `GtkOverlay`, donc dessiné par-dessus le bloc
heure/date. Tant qu'il n'était qu'une forme claire en retrait, cela ne se
voyait pas ; dès qu'il a porté une ombre, cette ombre est tombée sur les
chiffres et les a salis. Un filigrane se met derrière — c'est la définition
d'un filigrane. Le glyphe est donc l'enfant principal, le bloc heure/date lui
est superposé, et `gtk_overlay_set_measure_overlay()` rend à ce dernier le
soin de dicter la taille.

**Deux pièges payés au passage, tous deux muets :**

- **`window.shell { background: transparent }` gagnait sur `.coin`** par
  spécificité, du temps du vignettage : le dégradé n'était jamais peint, et
  rien ne le disait — 255 mesurés sous le texte là où on attendait 115.
- **Le padding et le dégradé posés sur le nœud `window` ont fait DISPARAÎTRE
  le coin.** La surface layer-shell gardait la taille du contenu seul, GTK
  plaçait l'enfant hors d'elle, et il n'en restait que le fond. Un coin
  entièrement vide, sans une ligne de journal. Ce qui décore le coin vit donc
  sur la **rangée**, jamais sur la fenêtre.

**Position fixe, et elle ne suit pas le dock.** Le dock sort de l'écran dès
qu'une application passe au premier plan ; l'avis, lui, ne bouge pas. Un
signal qui monterait et descendrait selon ce qui est au premier plan
demanderait à l'œil de le chercher.

**Un cadran et non un nombre**, pour le décompte. La première version
affichait « Veille dans 8 s » : un texte appelle la lecture, l'œil quitte le
paragraphe pour déchiffrer trois mots — l'interruption même qu'on voulait
éviter. Un disque qui se vide se perçoit sans se lire. Soixante graduations,
trois longueurs, celles d'un cadran horloger : avec dix secondes de préavis un
trait s'éteint toutes les 167 ms, ce n'est plus une disparition mais un
balayage. Les messages, eux, sont du texte parce qu'ils n'ont **pas d'échelle
à montrer** : « En charge » n'a pas de fraction.

**Le diamètre ne se mesure plus.** Le cadran valait la largeur de la pilule de
la barre d'état — 172 px relevés le 9 septembre 2026 — parce qu'il partageait
son bord droit et qu'un écart de quelques pixels s'y serait vu. Au centre de
l'écran il n'y a plus de bord à partager : faire dépendre un diamètre de la
largeur de l'heure affichée était devenu une coïncidence entretenue pour rien.
168 px, fixes.

**Une icône devant, et ce n'est pas un ornement.** C'est elle qu'on reconnaît
de loin, avant même d'avoir lu : « En charge » et « Sur batterie » se
distinguent d'un coup d'œil par la fiche ou la pile, jamais par la longueur du
mot. La fiche — `ac-adapter-symbolic` — a été dessinée pour l'occasion dans le
thème de la distribution ; l'icône de « Sur batterie » est celle du **niveau
réel**, pas une pile générique, parce qu'au moment où l'on débranche ce qu'on
veut savoir est précisément combien il reste. Les noms en `-symbolic` sont
recolorés par GTK : l'icône prend donc exactement la couleur et l'opacité du
texte, à condition que `color` soit posé sur la **ligne** et non sur
l'étiquette — sur l'étiquette seule, les deux auraient divergé au premier
réglage d'opacité.

**Ce n'est pas le centre de notifications, et les deux ne se remplacent pas.**
Un avis est fugace et ne laisse aucune trace ; ce qui doit se retrouver plus
tard passe par `org.freedesktop.Notifications`, la cloche et l'historique.
`batterie.c` fait les deux aux seuils — l'avis est l'écho immédiat, la
notification est l'archive. Aux deux bascules de la prise il ne fait que
l'avis : brancher ou débrancher est un geste qu'on vient de faire, on veut la
confirmation tout de suite et aucune raison de la retrouver dans la cloche une
heure plus tard.

**Et l'avis doit répondre au geste, pas au minuteur.** « En charge »
n'apparaissait qu'à la lecture suivante de la batterie — jusqu'à cinq minutes
après le branchement, l'intervalle sur secteur. `batterie.h` explique
longuement pourquoi la **charge** se scrute : mesuré, cette machine n'émet
aucun événement quand le pourcentage change. **Mais cela ne valait que pour le
pourcentage.** Brancher est un événement matériel, et le noyau l'annonce : le
pilote de l'adaptateur appelle `power_supply_changed()`, qui émet un uevent
sur la classe `power_supply`. Le shell s'y abonne par un socket **netlink**,
groupe 1 — celui des uevents du noyau, déclaré `NL_CFG_F_NONROOT_RECV`, donc
ouvert à un processus ordinaire : **ni privilège, ni libudev**. La rafale
— cinq messages, un par alimentation — est regroupée, et une seule lecture
suit. **Mesuré le 15 septembre 2026 dans `claude-os-status` :** événement à
09:26:54, une lecture, une seule. La scrutation reste en filet : si le socket
ne s'ouvrait pas, on retombe sur le comportement d'avant — plus lent, jamais
muet, et le journal le dit.

**Le compte à rebours l'emporte** sur un message : les deux se disputeraient
la même surface, et le décompte est le seul des deux qui ait une échéance. Le
message écarté est écrit au journal, et sa notification part quand même.

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

Deux calques peuvent se poser **par-dessus** le thème, et ne redéfinissent que
des jetons : la couleur de contraste et le verre. Les deux sections qui
suivent les décrivent ; la mécanique est la même, et elle tient à une
propriété de GTK vérifiée à la mesure — une couleur nommée se résout en
parcourant les fournisseurs de la plus haute priorité vers la plus basse,
quel que soit celui où la règle qui l'utilise est écrite.

Les ombres sont **doublées** : une large et diffuse pour l'élévation, une
courte et dense pour asseoir le contact. C'est ce doublement qui donne la
profondeur de ChromeOS, là où une ombre unique paraît plate. Elle est calculée
une fois par le compositeur, jamais réévaluée — rien à voir avec un flou
permanent, qui aurait coûté un rendu par image.

### La couleur de contraste

Un thème décide de tout à la fois : les surfaces, le texte, les ombres **et**
l'accent. On voulait pouvoir garder les surfaces d'un thème en changeant ce
qui les souligne — le bouton activé, la sélection, le point sous une
application ouverte, la ligne en surbrillance d'un menu.

Le mécanisme est **le même que celui des thèmes, d'un cran au-dessus** :
`style/accent-<id>.css` ne définit que quatre jetons — `@accent`,
`@accent-hover`, `@accent-press`, `@on-accent` — et le shell le charge comme
un fournisseur CSS de priorité supérieure à celle du thème. GTK résout une
couleur nommée en parcourant les fournisseurs du plus prioritaire au moins
prioritaire, quel que soit celui où la règle qui l'utilise est écrite :
`shell.css` n'a donc pas une ligne à changer, et décocher la couleur rend la
main au thème d'elle-même. **Vérifié dans les deux sens**, y compris après
vidage du calque.

Huit couleurs, et **elles ne sont pas choisies à l'œil** : chacune est la
teinte la plus vive de sa famille qui tienne encore **4,5:1 sous du texte
blanc**, seuil du texte de petit corps. C'est ce seuil qui a fixé la valeur,
pas l'inverse — un réglage qui s'appelle « couleur de contraste » et qui
rendrait les libellés illisibles serait une plaisanterie. Le ratio mesuré est
écrit en tête de chaque fichier, avec celui du survol et celui contre la
surface d'un thème sombre.

Une seule valeur par couleur sert les quatre thèmes. Une variante claire et
une variante sombre auraient mieux rendu sur les thèmes sombres, au prix de
seize fichiers à tenir d'accord — et la contrepartie mesurée est faible :
3,6:1 contre la surface sombre, au-dessus du 3:1 demandé à un élément
d'interface.

`claude-os-theme` lit le même fichier pour teindre les menus de labwc. Il n'en
recopie pas la table : il compose le nom du fichier depuis `shell.conf` et
s'arrête s'il n'existe pas. **Le fichier fait foi des deux côtés.**

### La transparence

Décochée par défaut, et **pas par prudence d'affichage**. Une surface opaque
est annoncée comme telle au compositeur, qui peut la poser sans rien
mélanger ; une surface translucide l'oblige à fondre ce qu'il y a dessous, à
chaque image et sur toute la hauteur de la pile. C'est du remplissage GPU,
donc des watts, sur une machine qui en consomme 6,8 au repos — et l'énergie
est le fil rouge de ce projet.

`style/verre.css` ne contient **aucune règle**, et c'est tout son intérêt :

```css
@define-color surface      @verre;
@define-color surface-alt  @verre-alt;
@define-color surface-sunk @verre-sunk;
```

Le dock, la barre d'état, la Console, le lanceur, les fenêtres du système et
leurs survols ne peignent jamais une couleur : ils peignent l'un de ces trois
jetons. Les renommer suffit donc à rendre le bureau entier translucide, sans
qu'on ait à tenir ici la liste des classes qui portent un fond — une liste qui
aurait vieilli à la première fenêtre ajoutée, en silence.

Les trois valeurs de verre appartiennent **au thème**, qui seul sait de quelle
couleur il dilue. **Trois et non une, et l'opacité croît avec
l'enfoncement** : le survol d'une icône se peint par-dessus le fond du dock ;
plus clairsemé que lui, il se lirait comme un trou creusé dans la surface au
lieu d'une réaction au doigt.

**Il n'y a pas de flou derrière ce verre, et il ne peut pas y en avoir** :
labwc ne sait pas flouter ce qui est sous une fenêtre. Les opacités — 0,74 à
0,95 — sont donc choisies pour rester lisibles sur un fond d'écran chargé, là
où un flou aurait permis d'aller beaucoup plus loin.

Le réglage s'arrête aux fenêtres que le shell peint lui-même. Les barres de
titre, les menus et l'affichage à l'écran sont dessinés par labwc, dont le
`themerc` ne prend que des couleurs opaques : il n'existe aucun moyen de lui
demander une surface translucide. Le panneau le dit.

### Le thème d'icônes de la distribution

`rootfs/usr/share/icons/Claude-OS`, engendré par
[`tools/fabrique-icones.py`](../tools/fabrique-icones.py). **C'est le
générateur qui est la source** ; corriger un SVG installé serait perdu à
l'exécution suivante.

Ce qui fait qu'un jeu d'icônes paraît dessiné plutôt qu'assemblé, ce n'est pas
le talent de chaque pictogramme : c'est qu'ils partagent tous la même
épaisseur de trait, le même rayon d'angle, la même marge. Ces constantes sont
en tête du fichier, et les changer redessine les cent quinze icônes d'un coup.
L'épaisseur — 1,5 sur une grille de 16 — est celle de la cloche des
notifications, seule icône que le projet possédait avant : un jeu plus gras
aurait été plus net au pixel près, mais c'est la cloche qu'on voit à côté de
l'heure toute la journée.

**Tout y est une surface pleine, jamais un contour.** GTK recolore une icône
`-symbolic` en imposant `fill` ; un trait au sens SVG — `stroke` — ne serait
pas recoloré et resterait noir sur un thème sombre. « Trait » veut donc dire
« rectangle long », et « cercle vide » veut dire « anneau à deux
sous-chemins ». Ce que GTK recolore exactement — `rect`, `circle`, `path`,
`polygon`, y compris dans un groupe transformé — a été **mesuré** : une icône
d'essai rendue en rouge, puis les pixels relus.

Les icônes en couleur reprennent **exactement** le glyphe des symboliques,
agrandi par une transformation : le dossier du dock et celui de la barre
latérale sont le même dessin, et non deux interprétations du même objet.

`Inherits=Papirus`. Le thème couvre ce que Claude OS **affiche** — les
pictogrammes que le shell demande à GTK, les dossiers de la barre latérale de
Fichiers, les types de fichiers courants, les applications du bureau. Il ne
couvre pas les milliers d'icônes du reste du monde et ne prétend pas le
faire : un thème qui aurait voulu tout redessiner aurait surtout affiché des
carrés barrés. Chromium et Claude Desktop gardent **délibérément** leur propre
icône — redessiner la marque de quelqu'un d'autre n'est pas une question de
style.

Le générateur **contrôle sa propre couverture** : il relève les noms d'icônes
cités dans `shell/src/*.c` et dit lesquels il ne dessine pas. Ce contrôle
existe parce que l'erreur est muette — une icône absente ne provoque rien, GTK
descend dans Papirus et affiche autre chose. C'est ainsi qu'on a trouvé le
défaut du 14 septembre 2026 : `status.c` ne demande pas un nom écrit en clair,
il **compose** `battery-level-%d%s-symbolic` depuis la charge arrondie à la
dizaine. Le thème ne dessinait que le cran 100 ; sur une machine à 67 %, la
barre d'état affichait la batterie de Papirus au milieu de nos icônes. Les
vingt-deux crans sont désormais engendrés, et le contrôle sait que ces
noms-là ne peuvent pas être trouvés par un `grep`.

### Le thème ne s'arrête pas au shell

La feuille de style habille les six programmes du shell. Elle ne dit rien à
Chromium, à Claude Desktop, au terminal, aux dialogues GTK ni aux barres de
titre que labwc dessine depuis que `rc.xml` demande `decoration server`. On
avait donc un bureau sombre entouré de fenêtres claires — constaté à l'écran
le 8 septembre 2026, photo à l'appui.

Toutes ces applications lisent la même chose, et une seule : `color-scheme`
dans l'espace `org.freedesktop.appearance`, publié par le **portail XDG**.
C'est là, et nulle part ailleurs, que Chromium va chercher de quoi honorer son
option « suivre le thème du système ».

`/usr/local/bin/claude-os-theme` est le seul programme qui écrit tout cela :

| Il écrit | Qui le lit |
|---|---|
| `gsettings org.gnome.desktop.interface color-scheme` | `xdg-desktop-portal-gtk`, qui le republie en `org.freedesktop.appearance` |
| `~/.config/gtk-3.0/settings.ini` et `gtk-4.0/` | GTK 3 et GTK 4, à chaud, sans redémarrage |
| `~/.config/labwc/themerc-override` | labwc, au SIGHUP qui suit |

Il prend ses couleurs **dans la feuille de style du thème courant**, pas dans
une table à lui : `style/theme-<id>.css` reste la source unique — et
`style/accent-<id>.css` par-dessus, quand une couleur de contraste est
choisie, exactement comme le shell la superpose. Il en déduit
même « clair ou sombre » par la luminosité de `@surface`, plutôt que par le
nom du thème — un thème nommé « nuit » fonctionnerait sans qu'on y touche.

Le panneau de réglages l'appelle à chaque changement, l'autostart une fois à
l'ouverture de session pour que Chromium démarre déjà de la bonne couleur au
lieu d'apparaître clair puis de basculer.

**Mesuré, dans une session labwc réelle avec le portail :** le portail répond
`uint32 1` sur les deux thèmes sombres et `uint32 2` sur les deux clairs ; le
signal `SettingChanged ('org.freedesktop.appearance', 'color-scheme', …)` part
à chaque bascule — c'est lui qui fait suivre les fenêtres **déjà ouvertes** ;
et la barre de titre d'une fenêtre laissée en place passe de `#2a2a27` à
`#f0eee6` sans qu'elle soit relancée, vérifié pixel par pixel sur deux
captures.

Trois écueils y sont enterrés, chacun décrit en tête du script : `UseIn=gnome`
dans `gtk.portal` (voir juste dessous), `gsettings set` qui rend 0 sans
conserver, et `labwc --reconfigure` qui exige `LABWC_PID`.

### `portals.conf`, sans quoi rien de tout cela n'existe

`xdg-desktop-portal-gtk` sait publier `color-scheme`, mais son fichier
`gtk.portal` déclare `UseIn=gnome`. Notre session s'annonce `labwc` : le
portail frontal ne retenait donc aucun fournisseur, l'interface `Settings`
n'apparaissait pas sur le bus, et l'appel répondait « No such interface ».

`/etc/xdg-desktop-portal/portals.conf` le désigne explicitement :

```
[preferred]
default=gtk
```

Quatre lignes dont dépend toute l'harmonie du bureau. Les retirer ne provoque
aucune erreur : les fenêtres redeviennent simplement claires.

---

## 4.4 Le cahier des charges, point par point

| # | Demande | État |
|---|---|---|
| 1 | Esthétique ChromeOS, dock macOS centré | fait |
| 2 | Fenêtres sans cadre latéral ni inférieur | revu — voir « Les décorations ont changé de camp » ci-dessous |
| 3 | Pas de flou d'arrière-plan | fait — aucun flou nulle part |
| 4 | Ombres légères sur fenêtres, icônes, dock, barre | fait |
| 5 | Bouton du lanceur sur le dock | fait |
| 6 | Icônes réorganisables au glisser-déposer | fait, ordre enregistré aussitôt |
| 7 | Dock et barre basculés par la touche Loupe | fait (Super, et Super+Espace en repli) |
| 8 | Fenêtres maximisées **sous** le dock | fait, et réglable |
| 9 | Panneau de réglages d'affichage | fait — thème, police, icônes, fond d'écran, dock |
| 10 | Déposer icônes et dossiers sur le bureau | **non fait** — voir ci-dessous |
| 11 | Google Drive et OneDrive dans le gestionnaire | **Drive fait** le 15 septembre 2026 — section « Nuage » du volet, voir [`docs/13`](13-nuage.md). OneDrive écrit, jamais monté. |
| 12 | Écran tactile fonctionnel | fait — natif sous Wayland, sans configuration |

### Les deux points ouverts

**Point 10 — des icônes sur le bureau.** Le fond d'écran est une surface
layer-shell qui ne dessine qu'un dégradé ou une image ; il n'y a pas de
gestionnaire de bureau. Le faire demande d'ajouter au fond la gestion d'une
grille d'icônes, du `~/Bureau`, du glisser-déposer et du clic droit —
c'est-à-dire un septième programme. Reporté, pas oublié.

**Point 11 — le nuage.** *Fait pour Google Drive le 15 septembre 2026.* La
section « Nuage » a été déclarée comme prévu, et rien ne s'affiche tant
qu'aucun compte n'est connecté — une entrée qui ne mène nulle part serait
pire que son absence. Reste OneDrive, dont la connexion bute sur Microsoft et
non sur ce code, et qui n'a **jamais été monté**. Détail dans
[`docs/13`](13-nuage.md).

### Les décorations ont changé de camp

Le cahier des charges demandait des fenêtres sans cadre, et `rc.xml` disait
donc `decoration=client` : chaque application dessinait la sienne. Sur la
machine, le résultat n'était pas celui qu'on attendait — **beaucoup de
fenêtres n'avaient aucun bouton réduire ni agrandir**, parce que toutes les
applications ne dessinent pas de barre de titre quand le compositeur leur
laisse la main. Le mousepad, les dialogues GTK, plusieurs fenêtres de Chromium
étaient simplement impossibles à réduire à la souris.

`rc.xml` dit maintenant `decoration=server` : labwc pose lui-même une barre de
titre à celles qui n'en dessinent pas, et les autres gardent la leur. Les
boutons sont revenus partout.

Le prix de ce choix était une barre de titre dessinée avec le thème par défaut
de labwc, qui est clair — des bandeaux blancs sur un bureau sombre. C'est ce
que `themerc-override` corrige, et depuis `claude-os-theme` il suit le thème
courant plutôt que d'être figé (voir §4.3).

### Reporté sans regret

Les **boutons de barre de titre stylisés** « coup de crayon ». La barre est
maintenant dessinée par labwc, donc atteignable ; mais `themerc` ne sait
colorer que des boutons, pas en changer le dessin. Il faudrait fournir des
images à labwc, thème par thème. Le gain est esthétique et le coût réel :
reporté.

---

## 4.5 L'écran de connexion

Un seul compte, un seul champ. Le nom d'usage, le mot de passe, l'heure dans
le coin — et le même dégradé de fond que le bureau, lu dans le thème de
l'utilisateur.

| Couche | Rôle |
|---|---|
| **greetd** | ouvre la session. Ne dessine rien |
| **labwc**, configuration nue | porte l'écran. Aucun raccourci clavier |
| **`claude-os-connexion`** | le champ de mot de passe |

### L'authentification n'est pas dans notre code

`claude-os-connexion` ne touche jamais à PAM, ne lit jamais `/etc/shadow`, et
ne tourne pas en root — il tourne sous `_greetd`. Il **relaie** : greetd pose
les questions de PAM, le programme les affiche, renvoie les réponses, greetd
décide. C'est délibéré : un écran de connexion qui ferait lui-même
l'authentification serait un programme privilégié de plus à auditer.

Le protocole est `greetd-ipc(7)` — une longueur sur quatre octets, puis du
JSON, sur la socket nommée par `GREETD_SOCK`.

### La configuration de labwc y est nue, et c'est le but

`/etc/xdg/labwc-greeter/rc.xml` ne définit **aucun** raccourci — pas même les
raccourcis intégrés de labwc, qu'il faut demander explicitement. Tout
raccourci ici serait une porte ouverte *avant* authentification. C'est
pourquoi cette configuration est séparée de celle de la session, qui offre au
contraire un terminal de secours au clavier : les deux besoins sont opposés,
ils ne peuvent pas partager un fichier.

### Ce que cela a permis de retirer

Le greeter de LightDM ne savait démarrer que sur un serveur X. C'était son
**dernier usage** sur cette machine. Voir §4.6.

---

## 4.6 Le serveur X : ce qui en avait besoin

La question mérite une réponse nette, parce qu'elle a servi d'argument à deux
décisions successives.

| Ce qui pourrait en avoir besoin | Verdict |
|---|---|
| La session et le bureau | **Non.** labwc et le shell sont Wayland natifs |
| Chromium | **Non.** `--ozone-platform-hint=auto` |
| Claude Desktop | **Non.** `--ozone-platform=wayland` |
| Le bloc-notes, le terminal | **Non.** GTK3 et foot parlent Wayland |
| L'écran tactile | **Non.** libinput passe par le compositeur |
| L'écran de connexion de LightDM | **Oui** — et c'était le seul |

En remplaçant LightDM par greetd, plus rien ne réclame de serveur X.

**Xwayland reste installé**, et c'est un choix : il ne démarre que si un
programme X11 se lance, et ne coûte rien tant qu'aucun ne le fait. C'est le
filet pour le jour où une application ne saurait pas parler Wayland.

Ce qui n'est **pas** rendu possible pour autant : *Quick Entry* de Claude
Desktop, qui demande X11 **ou** le portail `GlobalShortcuts`. labwc n'implémente
pas ce portail, et lancer Claude Desktop sous Xwayland ne suffirait pas — le
raccourci doit être global, donc connu du compositeur. Cette fonction reste
indisponible, comme annoncé en `docs/02` §2.4.

### Ce qui n'est pas purgé tout de suite

LightDM et le serveur X restent **installés mais désactivés** le temps que
greetd fasse ses preuves. Un écran de connexion qui refuse de s'afficher
enferme dehors, et ce Chromebook n'a pas de touches F pour changer de terminal
virtuel : il ne resterait que SSH. Le retour en arrière tient en une commande,
et la purge est le dernier geste.

---

## 4.7 Ce qui est volontairement absent

| Absent | Pourquoi |
|---|---|
| Effets de bureau, animations de fenêtres | chaque image rendue est de l'énergie |
| Flou d'arrière-plan | demandé absent, et coûteux : un rendu par image |
| Indexeur de fichiers | écrit sur l'eMMC en permanence pour une recherche rare |
| Gestionnaire de paquets graphique | `apt` suffit, et c'est 80 Mo de moins |
| Client de courrier, suite bureautique | usage 100 % web |
| Serveur X | plus rien n'en a besoin depuis greetd — voir §4.6 |
| Démon de notifications | parti avec X11 — à remettre, voir `docs/02` §2.7 |

---

## 4.8 Ce qui reste à valider sur la machine

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

## 4.9 Sources

- Documentation Anthropic, Claude Desktop pour Linux — *Quick Entry* et
  portails.
- `labwc(1)`, `labwc-config(5)` — configuration du compositeur.
- Protocole `wlr-foreign-toplevel-management-unstable-v1` — suivi des fenêtres.
- Protocole `wlr-layer-shell-unstable-v1` — ancrage des surfaces de bureau.
- Documentation GTK4 : `GtkColumnView`, `GtkListItemFactory`, `GtkCssProvider`.
- Spécification freedesktop : entrées `.desktop`, catégories de menu,
  corbeille.
