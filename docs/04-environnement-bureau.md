# 4. L'environnement de bureau

Cahier des charges : usage 100 % web, Wi-Fi et Bluetooth fonctionnels, interface
sur-mesure inspirée de ChromeOS avec dock central façon macOS et barre d'état en
bas à droite, aucune fonction superflue, et trois applications — Chromium, un
bloc-notes, Claude Desktop.

## 4.1 Une remarque sur la référence visuelle

« Dock centré façon macOS » et « barre d'état en bas à droite façon ChromeOS »
ne s'opposent pas : c'est exactement la disposition de l'étagère de ChromeOS,
qui centre ses icônes d'applications et aligne à droite sa zone d'état. La
cible est donc cohérente, et c'est celle qui est mise en œuvre.

## 4.2 La pile retenue

| Rôle | Choix | Empreinte | Pourquoi |
|---|---|---|---|
| Serveur d'affichage | **Xorg** | ~70 Mo | Imposé : le raccourci *Quick Entry* de Claude Desktop exige X11 ou le portail Wayland `GlobalShortcuts`, absent des compositeurs légers. |
| Gestionnaire de fenêtres | **Openbox** | ~10 Mo | Rôle volontairement effacé : place les fenêtres, gère les raccourcis. Sans décorations ni bureaux multiples. |
| Dock + barre d'état | **tint2 ×2** | ~24 Mo | Deux instances : c'est la seule façon d'obtenir un dock réellement centré **et** une barre ancrée à droite, tint2 disposant ses éléments de gauche à droite dans un même panneau. |
| Compositeur | **picom** | ~25 Mo | C'est lui qui produit l'aspect : coins arrondis, translucidité, flou, ombres. |
| Notifications | **dunst** | ~8 Mo | |
| Session | **LightDM** + greeter GTK | ~40 Mo | |

Total au repos visé : **~300 Mo**, services système compris. Le budget de
`docs/02` (session graphique sous 250 Mo) est tenu.

### Pourquoi pas un shell écrit sur mesure

Une interface entièrement développée — Electron ou GTK — était l'autre voie.
Écartée : un second processus Electron coûterait 200 à 400 Mo sur une machine
qui n'en a que 4 Go, dont Claude Desktop consomme déjà une bonne part. tint2
est configurable au pixel près, ce qui suffit largement à obtenir le rendu
voulu pour un vingtième du coût mémoire.

## 4.3 Le rendu

Codes visuels communs aux deux panneaux, pour l'unité :

| Élément | Valeur |
|---|---|
| Fond des panneaux | `#202124` à 90 % d'opacité |
| Bordure | `#ffffff` à 10 % |
| Rayon des coins | 18 px (panneaux), 12 px (pastilles) |
| Accent au survol | `#8ab4f8` (bleu ChromeOS) à 18 % |
| Texte principal | `#e8eaed` |
| Texte secondaire | `#9aa0a6` |
| Police | Inter |
| Hauteur des panneaux | 60 px, icônes 40 px |

Fond d'écran : dégradé indigo profond avec deux halos radiaux, généré par
script donc reproductible et versionnable, jamais téléchargé.

Le flou (`dual_kawase`, force 4) n'est appliqué qu'aux fenêtres translucides,
c'est-à-dire aux deux panneaux seulement. Un flou plein écran serait hors
budget sur un GPU 32 EU à 6 W. **C'est le premier réglage à sacrifier** si
l'affichage saccade : `blur-method = "none"` dans `picom.conf`.

## 4.4 Ce qui est volontairement absent

- **Cowork.** La documentation Anthropic est explicite : sous Linux, Cowork
  lance ses tâches dans une machine virtuelle QEMU/KVM, installée via les
  paquets *recommandés*. Notre installation utilise `--no-install-recommends`,
  ce qui l'écarte — et c'est délibéré : une VM à côté d'une application
  Electron sur 4 Go n'est pas exploitable, et QEMU + OVMF pèsent plusieurs
  centaines de Mo sur un eMMC déjà petit. Chat et Claude Code restent
  disponibles.
- **Gestionnaire de fichiers, terminal graphique, suite bureautique, lecteur
  multimédia, indexeur, imprimante.** Hors du cahier des charges.
- **Bureaux multiples, décorations de fenêtres, menu démarrer.** Sans objet
  pour un usage où une application occupe l'écran.
- **ModemManager**, masqué : pas de modem cellulaire sur cette machine.

## 4.5 Points à valider sur la machine

Aucun de ces points ne peut être tranché avant installation.

| À vérifier | Commande | Enjeu |
|---|---|---|
| **Audio** | `aplay -l` puis `wpctl status` | **Risque n°1.** Symptôme classique sur ces Chromebooks : casque fonctionnel, haut-parleurs internes muets, faute de profil UCM. |
| Wi-Fi | `nmcli device wifi list` | `firmware-iwlwifi` doit être présent dès l'installation, sans quoi pas de réseau. |
| Bluetooth | `bluetoothctl show` | |
| Décodage matériel | `vainfo` et `chrome://gpu` | « Video Decode » doit indiquer *Hardware accelerated*. Décide de la fluidité et de l'autonomie. |
| Empreinte réelle | `free -h` au repos | Cible : moins de 400 Mo. |

### AV1

Jasper Lake ne décode pas l'AV1 en matériel (cf. `docs/02` §2.3). YouTube en
sert par défaut aux clients qui l'annoncent : le flux retomberait alors en
décodage logiciel sur quatre cœurs à 6 W.

Chromium n'expose pas d'option en ligne de commande fiable pour refuser l'AV1.
La mitigation praticable est une extension du type *enhanced-h264ify*, qui
masque le support AV1/VP9 auprès du site et force la négociation vers un codec
accéléré. **À valider en mesurant** : lecture d'une vidéo 1080p, puis
`chrome://media-internals` pour confirmer le codec réellement utilisé, et
observation de la charge CPU.

## 4.6 Sources

- Claude Desktop sous Linux (dépôt apt, empreinte de clé, limites de la bêta,
  prérequis Cowork) — <https://code.claude.com/docs/en/desktop-linux>
