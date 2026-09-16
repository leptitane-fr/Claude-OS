# 14 — La surface d'outils partagée

*Commencé le 16 septembre 2026.*

**Si vous écrivez une application pour Claude OS, ce document et
[`shell/src/outils.h`](../shell/src/outils.h) sont tout ce qu'il vous faut.**
Le reste du dépôt explique comment le bureau est fait ; ces deux fichiers-là
disent comment s'y brancher.

---

## 14.1 La thèse

Une application de cette distribution **ne dessine pas son chrome**. Pas de
barre d'outils, pas de volet latéral, pas de rangée de boutons en haut de la
fenêtre. Elle **déclare** ce qu'elle sait faire et où l'on peut aller ; le
dock le dessine pour elle, dans la matière du bureau.

Trois gains, qu'aucune convention de style n'obtient :

- **Un seul endroit où regarder.** Les outils de l'application active sont
  toujours au même endroit de l'écran, quelle que soit l'application.
- **Une seule source de vérité.** Le menu contextuel, la barre du dock et le
  clavier lisent le même modèle. Un état à tenir d'accord de moins — la même
  discipline que le trieur unique des quatre vues de Fichiers.
- **De la place.** Une fenêtre sans barre ni volet rend environ 150 px de
  haut et 200 de large à son contenu. La dalle de MADOO fait 1920 × 1080.

**Le prix est nommé, et il est réel :** une fenêtre nue pilotée au clic
droit est admirable pour qui sait, opaque pour qui ne sait pas. C'est un
choix assumé pour cette distribution, décidé par l'utilisateur le
16 septembre 2026. Ce n'est pas une bonne pratique générale, et il ne faut
pas la présenter comme telle.

---

## 14.2 Les emplacements

Le dock a **deux faces**, et un flip les échange.

Face **bureau**, celle d'aujourd'hui :

```
      ╭───────────────────────────────────────╮
      │  ⊞ │ ▣ ▣ ▣ ▣ ▣ │ ▣ ▣                  │
      ╰───────────────────────────────────────╯
        lanceur, épinglées, ouvertes
```

Face **établi**, celle que le contrat remplit :

```
                    ┌─────────────────────┐
                    │       AUVENT        │   ← se déploie vers le haut
      ╭─────────────┴─────────────────────┴───────────────────╮
      │ ▣ ▣ ▣ │ 🏠 ★ 💾 ☁ │ Accueil › Images › 2026 │ 🔍 │ ⤺ │
      ╰───────────────────────────────────────────────────────╯
        apps      LIEUX          FIL            OUTILS  retour
```

**Quatre emplacements sont offerts aux applications**, et deux sont réservés
au bureau. Une application ne peut rien poser ailleurs, et c'est ce qui
garantit que deux applications se ressemblent.

| Emplacement | Ce qu'on y met | Comment c'est dessiné |
|---|---|---|
| `lieux` | Où l'on peut aller : dossiers, favoris, lecteurs, sources, onglets, projets. Ce qui se choisit, et où l'on revient. | Icônes avec libellé, l'entrée courante allumée |
| `fil` | Où l'on est. | Étapes séparées de chevrons, cliquables, défilantes |
| `outils` | Ce qu'on déclenche. | Boutons à icône |
| `auvent` | Ce qui demande de la place : une saisie, une liste, un réglage. | Un volet qui monte au-dessus de la pilule |

Réservés au bureau, non déclarables : **la bande des applications ouvertes**
à l'extrême gauche, et **le bouton de retour au bureau** à l'extrême droite.

### Ce qui ne va PAS dans la barre

Les actions d'édition — copier, coller, renommer, supprimer, trier, changer
de vue — **restent au menu contextuel et au clavier**.

> La barre porte la **navigation** et les **portes** ;
> le clic droit porte les **verbes**.

C'est une décision de l'utilisateur, pas une limite technique : rien
n'empêcherait d'y mettre des verbes, et c'est précisément pour cela qu'il
faut l'écrire. Une application qui remplirait sa zone `outils` de boutons
« Copier » et « Coller » ne serait pas en panne — elle serait hors sujet.

---

## 14.3 Écrire une application qui s'intègre

Six gestes. L'exemple complet et **qui compile** est l'application témoin,
dans [`shell/src/outils-diag.c`](../shell/src/outils-diag.c), section
« L'application témoin ».

### 1. Un groupe d'actions, celui que vous avez déjà

C'est le même que celui de votre menu contextuel. C'est tout l'intérêt : une
seule source de vérité.

```c
GSimpleActionGroup *actions = g_simple_action_group_new ();
g_action_map_add_action_entries (G_ACTION_MAP (actions), mes_actions,
                                 G_N_ELEMENTS (mes_actions), NULL);
```

### 2. Une action à état pour dire où l'on est

```c
{ "aller", sur_aller, "s", "'file:///home/stef'", NULL, { 0 } },
```

Dans le gestionnaire, **posez l'état après avoir navigué** :

```c
g_simple_action_set_state (a, g_variant_ref (but));
```

Le dock allume l'entrée dont la cible vaut cet état — la sémantique radio de
GMenu. **N'inventez pas d'attribut « courant »** : vous auriez deux vérités
à tenir d'accord, et elles divergeraient.

### 3. Le modèle de la barre

```c
GMenu *lieux = g_menu_new ();
GMenuItem *it = g_menu_item_new ("Images", NULL);
g_menu_item_set_action_and_target_value (it, "outils.aller",
        g_variant_new_string ("file:///home/stef/Images"));
g_menu_item_set_attribute (it, "icon", "s", "folder-pictures-symbolic");
g_menu_item_set_attribute (it, SHELL_OUTILS_A_FORME, "s", SHELL_OUTILS_LIEU);
g_menu_append_item (lieux, it);
g_object_unref (it);

GMenu *barre = g_menu_new ();
GMenuItem *sec = g_menu_item_new_section ("Personnel", G_MENU_MODEL (lieux));
g_menu_item_set_attribute (sec, SHELL_OUTILS_A_ZONE, "s", SHELL_OUTILS_ZONE_LIEUX);
g_menu_append_item (barre, sec);
g_object_unref (sec);
```

Notez le préfixe : les actions du modèle s'écrivent **`outils.quelquechose`**
alors que le groupe exporté les porte **sans préfixe** (`aller`). C'est sous
`outils` que le dock insère le groupe importé.

### 4. Publier

```c
o = shell_outils_publier (G_APPLICATION (app), "Fichiers",
                          G_ACTION_GROUP (actions), G_MENU_MODEL (barre),
                          sur_prise, NULL);
```

**Le modèle peut changer à tout moment ensuite** : modifiez le `GMenu`, le
dock suit. `org.gtk.Menus` signale ses propres changements. C'est ainsi que
le fil d'Ariane se met à jour à chaque navigation, sans un appel de plus.

### 5. Le repli — il n'est PAS facultatif

```c
static void
sur_prise (gboolean prise, gpointer data)
{
    gtk_widget_set_visible (mon_volet_interne, !prise);
}
```

Une application dont les outils vivent dans un autre processus **dépend de
ce processus**. Elle doit rester utilisable sans lui : lancée seule depuis
un terminal, au banc d'essai, ou le jour où le dock tombe. Gardez donc votre
volet interne, escamoté tant que le dock tient la barre.

**L'état de départ est « pas pris »**, et il ne se rappelle pas : une
application s'ouvre avec ses replis en place. Le rappel ne signale que les
changements — dock absent, aucun appel, ce qui est déjà la bonne réponse.

### 6. Retirer à la fermeture

```c
shell_outils_retirer (o);
```

---

## 14.4 Le protocole

### Le transport — rien d'inventé

`org.gtk.Menus` et `org.gtk.Actions`, les deux interfaces que GTK exporte et
importe nativement. Le dock parle déjà `org.gtk.Actions` à la barre d'état
depuis le 11 septembre 2026 : le canal est éprouvé, et une application qui
n'est pas en GTK peut les implémenter — elles sont spécifiées.

**Tout vit au même chemin**, sur le nom de bus de l'application :

```
/os/claude/shell/outils
   ├── os.claude.shell.Outils   la découverte
   ├── org.gtk.Menus            le modèle de la barre
   └── org.gtk.Actions          ce que la barre déclenche
```

Un chemin **fixe**, et non dérivé de l'identifiant de l'application : une
application non-GTK n'a pas à reproduire la règle de dérivation de
`GApplication` pour se faire entendre.

L'interface de découverte est minuscule, et c'est voulu :

```xml
<interface name='os.claude.shell.Outils'>
  <property name='Contrat' type='u' access='read'/>
  <property name='Titre'   type='s' access='read'/>
  <method name='Prise'><arg name='prise' type='b' direction='in'/></method>
</interface>
```

`Contrat` est **demandé**, jamais supposé. Les deux processus sont déployés
ensemble aujourd'hui, mais rien ne le garantit demain : une application
tierce, un clone, une version en cours d'essai. Le dock refuse ce qu'il ne
sait pas lire, et le dit.

### La présentation — qui parle le premier

**L'application se présente au dock.** Elle ne l'attend pas, et le dock ne la
cherche pas.

```
1.  L'application exporte ses trois interfaces.
2.  Elle surveille le nom « os.claude.shell.dock ».
      absent  → rien : l'état de départ est déjà « pas pris ».
      présent → elle active l'action « outils-presenter » du dock,
                avec son propre nom de bus en paramètre.
3.  Le dock lit « Contrat », importe le menu et les actions,
    puis appelle Prise(true) — ou Prise(false) s'il ne sait pas lire.
4.  Le dock disparaît → Prise retombe à false, les replis reviennent.
```

**L'autre voie a été écartée.** Le dock ne découvre une fenêtre que par
`wlr-foreign-toplevel-management-v1`, donc au moment où elle s'**active**.
Une application ne saurait alors qu'au premier clic si ses outils sont pris
en charge, et son volet de repli apparaîtrait puis disparaîtrait sous les
yeux de l'utilisateur. Se présenter à la publication ferme ce trou.

**Aucune minuterie, aucune scrutation.** Tout part d'un événement : un nom
qui apparaît sur le bus, un appel de méthode. Ici la discipline du projet
tombe particulièrement juste — l'absence du dock se **lit** sur le bus, elle
ne se déduit pas d'un délai écoulé.

**`Prise` n'est accepté que du dock.** L'application retient le propriétaire
du nom `os.claude.shell.dock` et refuse l'appel qui vient d'ailleurs. Sans
cette vérification, n'importe quel programme du bus de session pourrait
faire disparaître le volet de repli d'une application et la laisser sans
outils du tout.

### L'appariement fenêtre → barre

Par l'**`app_id`**, seul identifiant que
`wlr-foreign-toplevel-management-v1` fournisse. L'application se présente
avec son nom **bien connu** (son `application_id`), et c'est lui que le dock
rapproche de l'`app_id` de ses fenêtres.

**Conséquence assumée, et il faut la connaître :** la barre est **par
application**, pas par fenêtre. Une application à plusieurs fenêtres suit
son propre focus et réexporte le contenu de la fenêtre active ; le dock lit
toujours le même chemin et n'a pas à connaître les fenêtres une à une.

### L'auvent

En v1, **un seul contrôle : la saisie.**

```c
GMenuItem *it = g_menu_item_new ("Rechercher", "outils.chercher");
g_menu_item_set_attribute (it, SHELL_OUTILS_A_FORME,    "s", SHELL_OUTILS_AUVENT);
g_menu_item_set_attribute (it, SHELL_OUTILS_A_CONTROLE, "s", SHELL_OUTILS_SAISIE);
g_menu_item_set_attribute (it, SHELL_OUTILS_A_INVITE,   "s", "Nom du fichier…");
g_menu_item_set_attribute (it, "icon", "s", "system-search-symbolic");
```

Le bouton paraît dans la zone `outils` ; au clic, l'auvent monte avec un
champ, et chaque frappe active `outils.chercher` avec le texte. Fermer
l'auvent l'active une dernière fois avec la chaîne vide.

**C'est le dock qui dessine le contrôle, et l'application n'en voit que la
valeur.** Un vocabulaire fermé, et non un langage de description
d'interface : la cohérence visuelle est alors garantie par construction, et
une application ne **peut pas** dessiner dans une surface qui appartient à
un autre processus.

### La table des attributs

| Attribut | Porté par | Valeurs |
|---|---|---|
| `x-claude-zone` | une section | `lieux`, `fil`, `outils`, `auvent` |
| `x-claude-forme` | une entrée | `lieu`, `bouton` (défaut), `etape`, `auvent` |
| `x-claude-controle` | une entrée `auvent` | `saisie` (écrit) — `liste` et `choix` sont prévus, non écrits |
| `x-claude-invite` | une saisie | le texte d'invite |
| `x-claude-astuce` | une entrée | l'infobulle |
| `x-claude-cle` | une entrée | le raccourci à **montrer** |

`x-claude-cle` ne fait que **montrer** : c'est l'application qui arme le
raccourci, le dock n'intercepte aucune touche. Un dock qui volerait des
touches au clavier d'une application serait une source de pannes
indéchiffrables.

Les constantes sont dans `outils.h`. `grep SHELL_OUTILS_` donne la liste
complète de ce qui circule entre les deux processus.

---

## 14.5 Éprouver

```sh
claude-os-outils --temoin                    # dans un terminal
claude-os-outils --dock                      # dans un second
claude-os-outils os.claude.shell.fichiers    # lire une vraie application
```

`claude-os-outils` est l'autre bout du contrat : il lit ce qu'une
application publie, sait **jouer le dock** — présentation et `Prise`
comprises —, et porte l'**application témoin** qui sert d'exemple de
référence.

Il a été écrit avant l'interface, et c'est délibéré : un contrat qui tient
entre deux processus n'est prouvé que si les deux bouts existent. Le
protocole a donc été joué en entier avant qu'un seul pixel ne soit dessiné.

Il reste utile ensuite, et c'est sa vraie raison d'être : **le jour où une
barre ne s'affiche pas, il dit lequel des deux côtés se tait.**

**Sans GTK, à dessein.** Un outil de diagnostic qui tire un runtime
graphique complet ne peut pas servir le jour où c'est le graphique qui est
en panne. Le témoin tourne sur un `GApplication` tout court — ce qui prouve
au passage qu'une application non-GTK peut porter ce contrat.

**Il dit ce qu'il sait, et seulement cela.** Les modèles D-Bus se
remplissent de façon asynchrone : une barre lue trop tôt paraît vide. Le
programme attend donc, et quand il n'a rien reçu il écrit « rien n'est
arrivé en 1500 ms » plutôt que « la barre est vide ». Deux faux négatifs ont
déjà coûté une séance à ce projet.

### Ce qui a été joué le 16 septembre 2026

Sur un bus isolé (`dbus-run-session`), le vrai dock occupant le nom sur la
session :

| Étape | Résultat |
|---|---|
| Le témoin publie, le dock est absent | aucun appel, repli en place — correct |
| Le dock paraît | le témoin se présente |
| Le dock lit la barre | zones, formes, cibles, icônes, contrôle, invite, touche : tout arrive |
| Le dock appelle `Prise(true)` | le témoin escamote son repli |
| L'état de `aller` est posé | l'entrée correspondante s'allume, **dans les deux zones** |
| Le dock est tué | le témoin revient au repli |
| Deux applications successives | le dock sert les deux sans rendre la main |

Et sur la session réelle, le vrai dock a répondu
`Unknown action "outils-presenter"` : la présentation part bien, et le dock
d'aujourd'hui ne sait simplement pas encore la recevoir. C'est l'étape 3.

### Un défaut payé le jour même

**Sans `setlocale (LC_ALL, "")`, tout accent sort en `?`.** Un programme C
reste en locale « C » tant qu'il ne demande pas celle de l'environnement ;
`g_print` convertit alors vers l'ASCII et remplace ce qu'il ne sait pas
écrire. GTK appelle `setlocale` pour ses applications, et c'est ce qui masque
le problème partout ailleurs dans ce dépôt — ici, il n'y a pas de GTK.

Le symptôme a d'abord été pris pour un bug de comparaison : le marqueur `▶`
du lieu courant et le `·` des autres devenaient tous deux `?`, donc
indiscernables. **La trace a tranché en une minute ce que la lecture du code
n'aurait pas tranché.** Mesurer, pas supposer.

---

## 14.6 Où en est le chantier

| # | Étape | État |
|---|---|---|
| 1 | Le contrat, la bibliothèque, l'outil, la doc | **fait le 16 septembre 2026** |
| 2 | Le retourneur — le flip du dock | **fait le 16 septembre 2026** — voir 14.7 |
| 3 | L'établi — la face outils, et la règle de visibilité | **fait le 16 septembre 2026** — voir 14.8 |
| 4 | L'auvent et le vocabulaire des contrôles | **fait le 16 septembre 2026** — voir 14.9 |
| 5 | Fichiers : publication, dépouillement, clavier, repli | **fait le 16 septembre 2026** — voir 14.10 |

**Les cinq étapes sont écrites.** Ce qui reste ne s'écrit pas : voir la fin
de 14.10.

### Le risque principal est labwc, pas GTK

Le flip change la **largeur** de la surface layer-shell. Or labwc 0.8.3
renvoie hors écran tout popover porté par une surface redimensionnée — un
invariant payé trois fois dans ce projet : centre de notifications, nappe du
dock, tiroirs.

La parade retenue pour l'étape 2 : **un seul redimensionnement, avant le
mouvement, popovers fermés**. La surface prend d'emblée la largeur de la
plus large des deux faces, et c'est le fond arrondi — dessiné en CSS sur un
enfant — qui s'élargit à l'intérieur. La surface ne bouge plus pendant
l'animation.

C'est pour cela que le retourneur passe avant tout le reste : s'il ne tient
pas au banc, la thèse entière se renégocie, et il vaut mieux le savoir à
l'étape 2 qu'à l'étape 5.

### Un effet de bord à connaître

Recompiler et réinstaller le shell **installera le lecteur vidéo**.
`--compiler` réinstalle tout, et `claude-os-video` attend dans
`meson.build` depuis le 10 septembre 2026. Ce n'est pas un problème, c'est
l'occasion de le voir enfin à l'écran — voir [`docs/11`](11-lecteur-video.md).

---

## 14.7 Le retourneur — étape 2

*Fait le 16 septembre 2026.* `shell/src/retourneur.{c,h}`, banc
`shell/essais/banc-retourneur.sh`.

Un widget à deux faces qui les échange par une rotation autour de son axe
horizontal. Pourquoi un retournement et pas un fondu : les deux faces ne sont
pas deux pages d'un même livre, c'est le **même objet vu de l'autre côté**.
Un fondu laisserait croire qu'on a remplacé le dock ; le retournement dit
qu'on l'a tourné.

### La parade à labwc, et elle tient

Le retourneur **mesure toujours au plus large des deux faces**. La surface a
donc, au repos comme en mouvement, la taille de la plus encombrante : elle ne
change pas pendant le flip. Ce qui s'élargit, c'est la pilule — le fond
arrondi, dessiné en CSS sur la face, pas sur la fenêtre.

Mesuré dans la trace Wayland, et non dans les compteurs du programme :

```
zwlr_layer_surface_v1#39.configure(3, 896, 34)
zwlr_layer_surface_v1#39.configure(4, 896, 34)
zwlr_layer_surface_v1#39.configure(5, 896, 34)
```

Trois configure, **tous au démarrage, tous à la même taille, et plus aucun
ensuite** — y compris après plusieurs retournements entre une face de cinq
boutons et une de quatorze. La surface ne bouge pas.

### Ce que le banc établit

| Question | Réponse |
|---|---|
| Images par retournement | **16 à 17** |
| Images au repos, avant et après | **0**, sur trois secondes |
| Tailles de surface distinctes | **1** |
| Départ signalé, popover fermé | une fois, avant la première image |
| Popover demandé en plein mouvement | refusé |
| Clic sur la face cachée | sans effet |

13 vérifications, 0 en échec.

### LE RENDU LOGICIEL NE SAIT PAS DESSINER LA 3D

**Et il le dit en rose vif.** Sous `GSK_RENDERER=cairo`, une face portant
`gsk_transform_perspective()` est peinte en rose — la couleur dont GSK marque
un nœud qu'il ne sait pas rendre. Découvert au banc, qui tourne en rendu
logiciel par construction.

Avec `ngl`, la même face tourne correctement : les verticales convergent, les
bords gauche et droit penchent en sens opposés et le centre reste droit —
c'est la perspective juste.

MADOO utilise `ngl`. Mais un repli logiciel reste possible — pilote en panne,
machine virtuelle, banc — et un dock qui virerait au rose à chaque bascule
serait un désastre visible. **Le retourneur détecte donc le renderer**
(`GskCairoRenderer`) et remplace la rotation par un **écrasement vertical** :
la hauteur suit le cosinus de l'angle, ce qui est la rotation dont on a
retiré la profondeur. L'objet se referme et se rouvre ; cairo sait le faire ;
et le journal dit qu'on est passé par là.

Le banc vérifie ce message. **S'il disparaît, la détection ne marche plus**,
et le dock virera au rose sur toute machine tombée en rendu logiciel.

### Trois pièges de mesure payés ce jour-là

Ils valent pour tout banc de ce dépôt :

- **La trace de `WAYLAND_DEBUG` nomme les objets `nom#id`, pas `nom@id`.** Un
  motif écrit avec `@` ne trouve rien, et le banc annonce fièrement zéro. Le
  test passait parce qu'il ne mesurait rien.
- **`gtk4-layer-shell` n'émet jamais `set_size`** dans ce montage : ancrée sur
  un seul bord, la surface prend la taille de son buffer et c'est le
  compositeur qui renvoie un `configure`. Compter les `set_size` revient à
  compter zéro quoi qu'il arrive. Le banc exige désormais que la trace
  **parle** avant de juger ce qu'elle dit.
- **`g_message` écrit sur la sortie d'erreur**, qui porte ici la trace
  Wayland. Chercher le message dans le seul journal standard, c'était ne
  jamais le trouver.

### Un instrument, et pourquoi il est dans le code de production

`CLAUDE_OS_RETOURNEUR_MS` règle la durée du mouvement. Un retournement dure
moins d'un tiers de seconde et `grim` met plus longtemps que cela à produire
une capture : sans ce réglage, **aucune image du mouvement ne peut être
photographiée**, et la justesse de la perspective ne se vérifierait que de
visu sur la machine. C'est par là qu'on a établi le rose de cairo. Hors banc,
la variable n'existe pas et la constante s'applique.

### Ce qui reste à voir sur MADOO

Le banc n'a pas d'écran, donc pas de rendu accéléré : **la rotation n'a
jamais été vue avec le renderer de la vraie machine sur le vrai écran.** Les
captures `ngl` du banc le prouvent en logiciel émulé, pas en conditions
réelles. À confirmer à l'étape 3, quand le dock portera vraiment ses deux
faces.

---

## 14.8 L'établi — étape 3

*Fait le 16 septembre 2026.* `shell/src/etabli.{c,h}`, l'état `ETABLI` de
`visibility.h`, les modifications de `dock.c`, banc
`shell/essais/banc-etabli.sh` et son témoin `etabli-essai.c`.

**C'est ici que le chantier devient visible.** Le dock porte deux faces, et
l'une d'elles est remplie par une application.

### La règle de visibilité, et ce qu'elle ajoute

Le 11 septembre : une fenêtre s'active, le dock s'en va. Cette règle valait
tant que le dock n'était qu'un lanceur — on ne lance pas une application
pendant qu'on travaille dedans.

Depuis le 16 : si l'application active a publié sa barre, **le dock reste, et
réserve sa place**. C'est la même surface, et ce n'est plus le même objet :
un lanceur s'efface, une barre d'outils reste.

**La règle ancienne n'est pas remplacée, elle est complétée** — et le banc le
vérifie dans les deux sens : une application sans barre fait toujours sortir
le dock, une application avec barre le fait rester, et l'alternance entre les
deux suit.

En `ETABLI`, **congédier ne fait rien**. C'est la différence de fond avec
`CONVOQUE` : convoqué, le dock est un invité par-dessus l'application, et le
premier clic à côté le renvoie. En établi, il *est* la barre d'outils de
cette application — le renvoyer au premier clic dans la fenêtre reviendrait à
faire disparaître les outils dès qu'on se sert de ce qu'ils servent.

### Ce que l'établi dessine

```
╭──────────────────────────────────────────────────────────────────────╮
│ ▣ ▣ │ 🏠 Accueil  📄 Documents  🖼 Images  🗑 Corbeille │ Accueil › Images › 2026 │ 🔍 │ ⌂ │
╰──────────────────────────────────────────────────────────────────────╯
```

Les lieux portent leur libellé sous l'icône, et non l'icône seule : « Documents »
et « Téléchargements » partagent le même pictogramme de dossier dans la
plupart des thèmes, et une rangée d'icônes identiques ne sert à rien.

**Ce qui se reconstruit, et ce qui ne fait que s'allumer.** Le modèle change
souvent — un fil d'Ariane bouge à chaque navigation — mais l'entrée
*courante* change encore plus souvent, et elle ne vaut pas une
reconstruction : refaire la rangée changerait la largeur de la pilule, donc
la taille de la surface, donc la position des popovers de labwc. Deux chemins
distincts, donc : le modèle change ⇒ on reconstruit et le dock ferme ses
surfaces ; un état d'action change ⇒ on ne fait que poser une classe CSS.

### UN GDBusMenuModel ARRIVE PAR ÉTAGES

**Le piège de toute cette étape, et il est muet.** Le modèle racine signale
« trois sections » bien avant que ces sections aient le moindre contenu :
chacune est un modèle à elle, qui se remplit par le bus et émet **son propre**
`items-changed`.

Écouter le seul modèle racine, c'est donc reconstruire un établi de trois
sections vides — et ne plus jamais être prévenu. Au banc : trois sections
annoncées, zéro entrée posée, **et pas une plainte** ; l'établi n'avait rien
à redire de ce qu'il n'avait pas reçu.

L'établi suit donc chaque sous-modèle rencontré, et coupe tout à la
reconstruction suivante. Le compte rendu de débogage
(`établi : 4 lieux, 4 étapes, 1 outils`) existe pour cette raison : c'est la
seule façon de vérifier de l'extérieur qu'une barre est arrivée **entière**,
puisqu'on ne compte pas des widgets depuis un autre processus.

### Trois défauts que seul l'écran a montrés

- **La face ne tournait pas.** `on_etat` décidait quelle face montrer au
  moment du changement d'état — or le modèle arrive par le bus, donc *après*.
  Le journal disait « etabli » et l'écran montrait les icônes du lanceur. La
  question se repose donc aussi quand l'établi se garnit.
- **Le fil d'Ariane était écrasé à zéro.** Un `GtkScrolledWindow` demande par
  défaut la place minimale — presque rien : il sait défiler, donc il accepte
  n'importe quelle largeur, et dans une boîte il la prend. Quatre étapes
  posées, aucune visible, aucun avertissement. Il faut
  `propagate_natural_width` **et** une politique `AUTOMATIC` : sous
  `EXTERNAL`, GTK considère que le défilement est géré ailleurs et ne
  propage rien.
- **Le bouton de retour ne faisait rien.** Il appelait `cacher()` puis
  `convoquer()` ; `convoquer` relit l'état souhaité, et comme une barre était
  toujours en place, il revenait à l'établi. Le journal disait « etabli »
  dans les deux cas. Le retour **ne touche pas à la visibilité** : il force la
  face bureau, jusqu'au prochain changement de fenêtre active — on revient au
  bureau pour aller chercher autre chose, et ce qu'on y trouve est justement
  ce qui rend sa barre au dock.

### La zone d'entrée suit la face, plus la fenêtre

Depuis que le dock a deux faces, sa fenêtre est taillée au plus large des
deux : elle déborde de ce qu'on voit. Une région d'entrée calquée sur elle
avalerait les clics tombés à côté de la pilule — sur le fond d'écran, ou sur
une fenêtre en dessous — sans que rien ne l'indique. Elle se découpe donc sur
les limites réelles de la face affichée.

### Ce que le banc établit

`banc-etabli.sh`, **14 vérifications, 0 en échec**, aucun avertissement GTK :

| Question | Réponse |
|---|---|
| Application sans barre | le dock s'efface — l'ancienne règle tient |
| Application avec barre | le dock reste, en `etabli` |
| Volet de repli de l'application | escamoté, et l'application sait pourquoi |
| Zones déclarées | toutes connues, aucune plainte |
| Auvent | se signale comme non écrit, bouton inerte |
| Navigation reçue par l'application | oui, par le chemin du contrat |
| Barre arrivée entière | `4 lieux, 4 étapes, 1 outils` |
| Retour au bureau | change la face, **pas** l'état |
| Alternance entre les deux applications | suit dans les deux sens |
| Application fermée | le dock quitte l'établi |

### Ce qui reste à voir sur MADOO

Le banc n'a pas d'écran : **la rotation entre les deux faces n'a toujours pas
été vue avec le renderer de la vraie machine**, et le style de l'établi n'a
été jugé que sur des captures. Restent aussi le doigt, et la question de
savoir si 86 px réservés en permanence se supportent à l'usage sur une dalle
de 1080.

---

## 14.9 L'auvent — étape 4

*Fait le 16 septembre 2026.* `shell/src/auvent.{c,h}`, le clavier virtuel de
banc `shell/essais/frappe.c`.

Un volet qui monte au-dessus de la pilule, ouvert par un bouton de la zone
`outils`. Un contrôle écrit : la **saisie**.

```
                    ┌──────────────────────┐
                    │  Nom du fichier…     │   ← l'auvent
      ╭─────────────┴──────────────────────┴──────────────╮
      │ ▣ ▣ │ 🏠 📄 🖼 🗑 │ Accueil › Images │ 🔍 │ ⌂ │
      ╰──────────────────────────────────────────────────╯
```

Chaque frappe part vers l'application par son action ; la fermeture en envoie
une dernière, **vide** — c'est ainsi qu'une application sait qu'il faut rendre
la liste complète. Sans elle, une recherche refermée laisserait le filtre en
place, et l'utilisateur chercherait pourquoi la moitié de ses fichiers a
disparu.

### EXCLUSIVE, et c'est une mesure, pas un goût

Le dock n'avait jamais pris le clavier. Une saisie change cela, et seulement
le temps qu'elle est ouverte.

`ON_DEMAND` paraissait le choix poli : le champ reçoit les touches parce
qu'on a cliqué dedans, et l'application les reprend d'un clic chez elle.
**Mesuré au banc : labwc 0.8.3 n'accorde le focus clavier d'une surface
`ON_DEMAND` qu'après un clic dedans.** Le volet montait, le champ portait son
contour bleu de focus GTK, et la frappe partait à l'application. Il aurait
fallu cliquer une seconde fois dans le champ — après avoir cliqué le bouton
qui l'ouvre.

`EXCLUSIVE` donne le focus à l'instant même : vérifié sur le même banc, la
frappe arrive sans un clic.

**La contrepartie est réelle** — l'application ne reçoit plus une touche tant
que le volet est là — et c'est pourquoi le dock **tend sa nappe** en même
temps. Trois portes de sortie, donc, et il en faut trois : Échap, un clic
n'importe où ailleurs, et le passage à une autre application.

### Un bug que seule la nappe a révélé

Le retourneur **centrait sa face verticalement**. Cela valait tant que la
fenêtre avait la taille de son contenu — mais le dock tend sa fenêtre à tout
l'écran quand il est convoqué, et maintenant quand l'auvent est ouvert. La
pilule, centrée dans 1080 px, **partait au milieu de l'écran**.

Vu sur deux captures d'auvent où le dock avait tout simplement disparu du
bas. Aucun test d'état ne l'aurait dit : le journal annonçait le bon état, et
la pilule était introuvable. La face suit désormais son propre `valign`, et
le journal dit où elle atterrit (`pilule : 516,996 887x84 dans 1920x1080`) —
le banc vérifie que `y + hauteur` touche le bas sur **toutes** les
allocations plein écran.

### Deux fermetures, et les confondre boucle

`dock_fermer_popovers()` ne touche qu'aux surfaces GTK — c'est ce qu'il faut
quand l'établi change de taille, **y compris quand c'est l'auvent qui vient
de s'ouvrir** : y fermer l'auvent le refermerait dans la foulée.
`dock_fermer_surfaces()` ferme l'auvent en plus, avant un retournement ou un
départ.

### `frappe` — le clavier virtuel du banc

**Aucun banc de ce dépôt ne savait taper.** On pouvait donc éprouver que le
volet s'ouvrait, et rien de ce qu'il sert à faire.

`shell/essais/frappe.c` donne au labwc sans écran un clavier, par
`virtual-keyboard-unstable-v1` — celui-là même que le clavier à l'écran du
mode tablette utilise. Il **fabrique une keymap pour ce qu'on tape** plutôt
que d'utiliser la disposition du système : chercher le keycode d'un caractère
dans un AZERTY demande de connaître ses niveaux, ses groupes et ses touches
mortes, et le banc taperait alors autre chose selon la machine.

```sh
bash shell/essais/construire.sh frappe
frappe "mire"
frappe --touche Escape
```

Il resservira : l'écran de connexion, le code PIN, le clavier à l'écran.

### Une commande pour actionner un outil

`gapplication action os.claude.shell.dock outil 0` actionne le premier outil
de l'établi. Le banc n'a pas d'yeux — il ne sait pas où le compositeur a posé
un bouton, et un clic à coordonnées devinées éprouverait surtout notre
capacité à deviner. C'est aussi le point d'accroche d'un raccourci clavier,
le jour où l'on voudra ouvrir la recherche du dock sans quitter le clavier.

### Ce que le banc établit

`banc-etabli.sh`, **20 vérifications, 0 en échec** :

| Question | Réponse |
|---|---|
| L'auvent s'ouvre | oui, et le dock passe le clavier en `exclusif` |
| La frappe arrive à l'application | `mire`, caractère par caractère |
| Échap referme | oui, et envoie la valeur vide |
| Le clavier est rendu | oui, retour à `none` |
| Un clic à côté referme | oui, sans quitter l'établi |
| La pilule, nappe tendue | collée au bas sur 13 allocations plein écran |

---

## 14.10 Fichiers — étape 5

*Fait le 16 septembre 2026.* Le premier vrai client de la surface d'outils —
et le seul banc qui éprouve une application **écrite avant le contrat**.
C'est la différence qui compte : le témoin avait été conçu pour lui, Fichiers
a trois semaines de plus.

### Ce que Fichiers publie

- **Les lieux**, tels que son volet les tient déjà : dossiers personnels,
  favoris, périphériques, lecteurs réseau, nuage. Le modèle est rempli dans
  la **même passe** que les widgets du volet — deux parcours du même contenu
  finiraient par diverger, et c'est exactement ce que le contrat cherche à
  éviter.
- **Le fil d'Ariane**, rempli par `maj_fil()` à chaque navigation, à côté des
  boutons qu'elle construisait déjà.
- **La recherche**, en auvent de saisie.

Ce qui n'a pas d'adresse ne figure pas dans le modèle : un volume non monté,
un lecteur réseau non connecté. On ne peut pas « y aller » d'un clic depuis
le dock, qui ne saurait pas quoi monter — **le volet, lui, sait le faire**, et
c'est une raison de plus de le garder en repli.

### Le même groupe d'actions, inséré deux fois

Sous `fichiers` pour la fenêtre — boutons, menu contextuel, raccourcis — et
sous `outils` parce que c'est sous ce nom que le contrat veut voir les
actions dans le modèle publié. **Un seul groupe, deux façons de l'appeler** :
dupliquer les actions donnerait deux comportements à tenir d'accord.

Le terme de recherche vit dans le champ interne, et lui seul : l'auvent écrit
dedans plutôt que de tenir sa propre copie. Le filtre lit ce champ, le champ
de repli **est** ce champ, et il n'y a jamais deux termes à accorder.

### Le clavier, complété

Une fenêtre nue doit être entièrement pilotable sans souris. Cinq raccourcis
manquaient tant qu'il y avait des boutons pour les remplacer :

| Touche | Effet |
|---|---|
| `Menu`, `Maj+F10` | le menu contextuel — **la porte d'entrée de tous les verbes** |
| `Ctrl+F` | la recherche, dans la fenêtre ou dans l'auvent selon qui tient la barre |
| `Ctrl+1…4` | les quatre vues |

`Ctrl+F` a demandé un ajout au contrat : `shell_outils_auvent()`.
L'application arme ses propres raccourcis — le dock n'intercepte aucune
touche — mais elle ne sait pas dessiner l'auvent. Elle **demande** au dock de
l'ouvrir sur une action donnée, et le contrat s'occupe du reste.

### UNE SECTION PEUT EN CONTENIR D'AUTRES

Le contrat a dû céder sur un point, et c'est Fichiers qui l'a montré.

`GMenuModel` est récursif par nature, et une application a de bonnes raisons
de s'en servir : Fichiers compose sa barre à partir du modèle que son volet
tient à jour tout seul, et ce modèle a **ses propres sections** —
Emplacements, Favoris, Périphériques. L'établi ne descendait pas dedans :
chaque sous-section était traitée comme une entrée ordinaire, sans libellé ni
action. Résultat mesuré : **zéro lieu et deux boutons vides**.

La zone se transmet désormais de parent en enfant, sauf si l'enfant déclare
la sienne. Une application peut grouper sans répéter la zone sur chaque
morceau — et une bibliothèque qui produit un modèle de lieux n'a pas à savoir
dans quelle zone on la posera.

### Le `.desktop` prend le nom de l'app_id

`claude-os-fichiers.desktop` est devenu `os.claude.shell.fichiers.desktop`.
C'était une coquetterie tant que seule l'icône du dock en dépendait ; c'est
un défaut depuis que **la barre** en dépend aussi. La visionneuse d'images
avait déjà pris cette règle le 10 septembre.

`provision.sh` purge désormais les **deux** noms : une installation
antérieure garde l'ancien, et deux entrées « Fichiers » apparaîtraient au
lanceur.

### Ce que le banc établit

`banc-fichiers.sh`, **9 vérifications, 0 en échec** :

| Question | Réponse |
|---|---|
| Sans dock | la fenêtre garde son chrome, et reste utilisable |
| Le dock arrive | il prend la barre, reste à l'écran, la fenêtre se dépouille |
| Les lieux | ceux du volet, dans la barre |
| Navigation depuis le dock | le dossier change, le fil suit |
| `Ctrl+F` | ouvre l'auvent du dock |
| La frappe | **`etat : 2 éléments`** sur six — tout le chemin, d'un bout à l'autre |
| Échap | referme |
| Le dock repart | le chrome revient |

La ligne qui compte est celle du filtre : le dock a reçu la frappe, l'a
passée à l'action de l'application, qui a refiltré sa liste. **Aucun des
maillons ne peut être éprouvé isolément.**

Les trois bancs ensemble : **42 vérifications, 0 en échec**.

### Ce qui reste, et ce n'est plus du code

Le chantier est écrit. Ce qui manque ne s'écrit pas, ça se regarde :

- **la rotation avec le renderer de la vraie machine**, sur le vrai écran ;
- **le style**, jugé jusqu'ici sur des captures d'un labwc sans écran ;
- **le doigt**, qu'aucun banc ne sait produire ;
- et la question d'usage : **86 px réservés en permanence** sur une dalle de
  1080, est-ce que cela se supporte ? C'est le choix qui a été fait, et il ne
  se juge qu'en s'en servant.

---

## 14.11 Ce que l'écran a corrigé

*16 septembre 2026, après la première mise en service sur MADOO.* Deux
défauts d'usage, **qu'aucun banc ne pouvait voir** : le banc lit des états, il
ne regarde pas.

### Les deux animations se chevauchaient

Dock caché, une application à barre passe devant : on voyait la pilule monter
**sous sa forme de lanceur**, puis basculer — et les deux mouvements se
recouvrant, la bascule paraissait précipitée.

La cause est un ordre d'opérations. `on_etat()` montrait la glissière, *puis*
demandait la face : la fenêtre devenue visible, le retourneur se trouvait
mappé et son animation partait en même temps que la montée.

**Hors de l'écran, on ne tourne pas : on est déjà tourné.** Le retourneur
pose désormais sa face sans l'animer tant qu'il n'est pas mappé, et le dock
la demande **avant** de monter. La face outils *arrive* en place au lieu de
se retourner une fois arrivée. Le mouvement ne vaut plus que pour un
retournement qu'on voit — d'une face à l'autre, dock à l'écran.

### Un aller sans retour n'est pas une bascule

Le bouton de retour au bureau n'avait pas de pendant : une fois revenu au
lanceur, il fallait passer à une autre application et revenir pour retrouver
les outils de la fenêtre devant.

La face bureau porte maintenant, **à la même extrémité**, un bouton
« Outils de la fenêtre ». Il n'existe que quand il y a quelque chose à y
retrouver : une barre posée, et garnie. Les deux boutons sont au même endroit
et font l'aller et le retour du même geste. Sur le bus :
`gapplication action os.claude.shell.dock outils`.

### Le journal dit la face, et pas seulement l'état

L'état de visibilité ne suffit pas : on reste en `ETABLI` tout en montrant le
lanceur, quand le bouton de retour a été pressé. Rien ne distinguait de
l'extérieur les deux moitiés de la bascule — le banc ne pouvait donc pas
éprouver le bouton qui la fait. `face : bureau` / `face : etabli` comble ce
trou, et les deux sens sont désormais vérifiés.

Les trois bancs : **44 vérifications, 0 en échec**.

### Ce qui reste, et n'est pas de ce chantier

Un défaut de Fichiers, repéré à l'usage et **hors sujet ici** : deux fenêtres
ouvertes sont la même instance, et les boutons de navigation, le volet et
l'affichage s'appliquent tous à la dernière ouverte. C'est un défaut
antérieur à la surface d'outils — l'utilisateur le traite séparément.

Il éclaire tout de même une limite déjà écrite en 14.4 : **la barre est par
application, pas par fenêtre.** Le jour où Fichiers aura de vraies fenêtres
indépendantes, il devra suivre son propre focus et réexporter — le dock, lui,
n'a rien à changer.

---

## 14.12 Le contrat 2 — l'établi ne porte que des boutons

*16 septembre 2026, après usage.* Trois constats, dont le troisième a changé
le contrat.

### Les applications ouvertes restent à droite

Sur la face bureau elles sont à droite ; l'établi les mettait à gauche. Au
retournement, elles traversaient la pilule et l'œil devait les rattraper.

L'ordre suit maintenant celui du dock des deux côtés :

```
lieux · outils │ applications ouvertes │ retour
```

Le bouton de retour passe **après** les applications ouvertes, tout à droite —
comme le bouton « outils » est la dernière chose du dock. Rien de ce qui est
commun aux deux faces ne bouge plus.

### Réseau et Nuage manquaient aux lieux

Le modèle couvrait les emplacements, les favoris et les périphériques. Les
deux dernières sections du volet passent par `entree_lecteur()` et
`entree_nuage()`, pas par `ajouter_chemin()` — personne ne les avait suivies.

Elles n'y figurent que **connectées** : un lecteur déclaré mais éteint n'a pas
de chemin où aller. Le volet, lui, sait le monter — c'est une des choses que
la barre ne saura jamais faire, et une raison de plus de le garder en repli.

### LE FIL D'ARIANE QUITTE LA RANGÉE — contrat 2

Une suite de mots séparés de chevrons au milieu d'une rangée de boutons
cassait le rythme : **l'œil ne savait plus ce qui se clique et ce qui se
lit.**

La zone `fil` et la forme `etape` ont été **retirées du contrat**, qui passe
en **version 2**. L'établi ne porte que des boutons, et c'est une règle.

À la place :

- **un bouton « Chemin »** dans la zone `outils`, qui déploie un auvent de
  type **`liste`** — les étapes, une par ligne, verticales, cliquables ;
- **le dossier courant dans le titre de la fenêtre.** labwc dessine la barre
  de titre de nos fenêtres : elle est toujours là, y compris quand la fenêtre
  est nue. Le dossier vient **en premier** — une barre de titre se tronque par
  la droite, et c'est le dossier qu'on cherche, pas le nom de l'application.

**Ce qui se lit va ailleurs ; ce qui s'atteint devient un bouton.** C'est la
règle générale que cette correction a dégagée, et elle vaut pour les
applications à venir.

### Le contrôle « liste »

Le second contrôle de l'auvent, et il ne marche pas comme le premier :

| | produit | qui agit |
|---|---|---|
| `saisie` | une valeur, frappe par frappe | l'action de l'entrée d'auvent |
| `liste` | rien | **chaque ligne**, avec sa propre action et sa cible |

Une liste est un menu, et le contrat la décrit comme tel : un **sous-menu**
(`G_MENU_LINK_SUBMENU`) attaché à l'entrée d'auvent. Le groupe d'actions n'est
pas cherché — l'auvent descend de l'établi, GTK remonte l'arbre pour résoudre
`outils.aller`, et les lignes agissent comme les boutons de la rangée.

Choisir une ligne referme le volet : on a obtenu ce pour quoi on l'avait
ouvert, et il tient le clavier.

**Une liste ne reçoit jamais la valeur vide de fermeture.** Elle n'a rien
produit ; la lui envoyer déclencherait une action qu'on n'a pas demandée.

### LE DOCK LIT ENFIN LA VERSION QU'IL PROMETTAIT DE VÉRIFIER

`outils.h` le disait depuis le premier jour : « le dock DEMANDE, et refuse ce
qu'il ne sait pas lire ». **Il ne le faisait pas.** La propriété `Contrat`
existait, personne ne la lisait, et la promesse était vide.

Le passage à la version 2 l'a rendue nécessaire : une application écrite pour
le contrat 1 déclare une zone `fil` que le dock ne connaît plus, et son
contenu tomberait dans les outils en boutons de texte — exactement le défaut
qu'on venait de corriger.

La version est demandée **une fois, à la présentation** : une application ne
change pas de contrat en cours de route. Refusée, elle reçoit `Prise(false)`
et garde son chrome — le repli fait son office, et le journal dit pourquoi.

### Deux pièges du banc, tous deux des erreurs de mesure

- **Une course, prise pour une panne.** Le premier essai du bouton de chemin
  donnait « liste vide » : le sous-menu venait du bus et n'était pas encore
  arrivé. Le modèle monte par étages, ici comme partout — quatre lignes
  finissent par venir, une salve après l'autre. Ce n'était pas le chemin qui
  manquait, c'était le banc qui n'attendait pas.
- **Le témoin a deux outils depuis le contrat 2.** Le banc ouvrait `outil 0`
  en croyant ouvrir la loupe, et s'étonnait qu'aucune frappe n'arrive. C'est
  le chemin qu'il ouvrait.

Les trois bancs : **45 vérifications, 0 en échec**.

---

## 14.13 Les décisions, et pourquoi

| Décision | Raison |
|---|---|
| `org.gtk.Menus` / `org.gtk.Actions`, pas de protocole à nous | GTK les parle des deux côtés, elles sont spécifiées, le canal est déjà éprouvé entre le dock et la barre d'état |
| Un chemin D-Bus fixe | une application non-GTK n'a pas à deviner la règle de dérivation de `GApplication` |
| L'application se présente | sinon elle ne saurait qu'au premier clic, et son repli clignoterait |
| La barre par application, pas par fenêtre | l'`app_id` est le seul identifiant que le compositeur donne |
| Le dock dessine les contrôles | la cohérence visuelle par construction ; et un processus ne dessine pas dans la surface d'un autre |
| Un vocabulaire fermé, pas un langage d'interface | ce qui n'est pas prévu se refuse et se dit, plutôt que d'afficher un trou |
| L'état de l'action dit le lieu courant | la sémantique radio de GMenu ; un attribut « courant » serait une seconde vérité |
| Le repli est obligatoire | une application ne peut pas dépendre d'un autre processus pour rester utilisable |
| `Prise` n'est accepté que du dock | sinon n'importe quel programme du bus pourrait priver une application de ses outils |
| La barre porte la navigation, le clic droit les verbes | décision de l'utilisateur, 16 septembre 2026 |
