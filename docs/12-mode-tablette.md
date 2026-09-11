# 12. Le mode tablette

Écran retourné, le Chromebook devient une tablette. Cette page dit **ce qui a
été mesuré** sur MADOO, ce qui a été décidé avec l'utilisateur, et ce qui
reste une limite. Ce qui n'est pas établi y est écrit comme tel.

État au 11 septembre 2026 :

| Pièce | État |
|---|---|
| Détection du mode tablette | **Faite, vue fonctionner sur MADOO** |
| Rotation de l'écran (paysage / chevalet) | **Faite, vue fonctionner sur MADOO** |
| Tactile et stylet alignés sur l'écran tourné | **Fait** — doigt vu juste en portrait ; stylet non essayé |
| Clavier AZERTY à l'écran | À faire — voir §12.6 |
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

## 12.6 Le clavier à l'écran — à faire

Choix de l'utilisateur : un clavier Claude OS, et non squeekboard ou wvkbd.
Les protocoles nécessaires existent (§12.1) : `input-method-v2` pour
apparaître quand un champ prend le focus, `virtual-keyboard-v1` pour frapper.

## 12.7 Ce qui n'est pas établi

- le stylet sur écran tourné ;
- les boutons de barre de titre au doigt, écran en chevalet ;
- la scrutation de la rotation écran éteint : elle continue, deux commandes à
  l'EC par seconde, tant que la machine est en mode tablette. Coût non mesuré.
