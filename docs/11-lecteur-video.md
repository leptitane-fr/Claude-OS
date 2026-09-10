# 11. Le lecteur vidéo — `claude-os-video`

Un lecteur pour Claude OS, dont le fil rouge est l'économie d'énergie. Cette
page dit **ce qui a été mesuré**, dans l'ordre où ça l'a été. Ce qui n'est pas
établi y est écrit comme tel.

État au 10 septembre 2026 : **phase 0 terminée**, architecture validée par la
mesure. Le lecteur lui-même n'est pas encore écrit.

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
| Batterie | `current_now × voltage_now` : toute la plateforme, écran compris. **La seule mesure d'autonomie** — et elle exige de débrancher. |
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

### En plein écran — la comparaison propre

**Un piège a été évité ici.** La première mesure en plein écran donnait
2,40 W, soit *moins que le repos*. Rien de miraculeux : le plein écran masque
Claude Desktop, qui consommait 23 % de processeur. La série a donc été refaite
entièrement en plein écran, où l'occultation est identique pour les quatre
chemins et les écarts redeviennent lisibles.

| Chemin, en plein écran | SoC |
|---|---|
| **`offload`** | **2,40 / 2,23 W** |
| `dmabuf` | 2,54 W |
| `logiciel` | 2,85 W |
| `copie` | 3,64 W |

L'écart `offload` / `dmabuf` — environ 0,2 W, près de 9 % du SoC — est ce que
rapporte le fait de ne plus composer du tout : le compositeur donne le tampon
au balayage.

**Ce qui n'est PAS établi :** aucun chiffre d'autonomie. Toutes ces mesures
ont été faites sur secteur, où la batterie ne mesure rien. Il faudra une
campagne câble débranché avant d'écrire le mot « record » ailleurs que dans
une intention.

## 11.5 Les huit règles d'énergie du lecteur

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

## 11.6 Les instruments, et comment s'en servir

```sh
bash shell/essais/construire.sh                 # les deux programmes d'essai
./shell/essais/build/fabrique-mire mire.mp4 60  # h264 (défaut) | hevc | vp9

# Le chemin sans copie est-il pris ? Chercher « Attaching » :
GDK_DEBUG=offload ./shell/essais/build/sonde-offload mire.mp4 --mode=offload

bash tools/mesure-conso.sh -d 30 --contre "…" "libellé"
bash tools/mesure-conso.sh --tableau            # tous les relevés passés
```

Les mires vivent dans `~/.local/share/claude-os/mires/` et ne sont pas dans le
dépôt : elles se refabriquent.

## 11.7 Ce qui reste à faire

| Phase | Objet | État |
|---|---|---|
| 0 | Banc de mesure, sonde, choix d'architecture | **fait, mesuré** |
| 1 | Noyau de lecture : démux, décodage, audio PipeWire, synchro | à écrire |
| 2 | L'interface : vidéo sans bordure, capsule, glissière au survol, tactile | à écrire |
| 3 | Pistes, sous-titres, MIME, intégration au bureau | à écrire |
| 4 | Campagne d'énergie **sur batterie**, réglages, conclusions | à faire |

Points ouverts, à ne pas oublier :

- **L'espace colorimétrique YUV de GTK** (§11.3) — non jugé à l'œil.
- **AV1** n'a pas encore été mesuré sur cette machine ; il sera le pire cas.
- **La reprise après suspension est cassée** sur MADOO (voir `CLAUDE.md`) : un
  plantage au réveil ne devra pas être imputé au lecteur.
