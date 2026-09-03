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
| Serveur d'affichage | **Xorg** | ~70 Mo | Imposé : *Quick Entry* exige X11 ou un portail Wayland absent des compositeurs légers. |
| Gestionnaire de fenêtres | **Openbox** | ~10 Mo | Thème sur mesure : barre de titre seule, aucun cadre. |
| Dock | **plank** | ~35 Mo | Glisser-déposer des icônes, zoom au survol, ombres d'icônes. |
| Barre d'état | **tint2** | ~12 Mo | Zone de notification, batterie, horloge. Ancrée à droite. |
| Bureau et fichiers | **PCManFM** | ~25 Mo | Icônes déposées sur le bureau, fond d'écran, gestionnaire de fichiers. |
| Lanceur | **rofi** | 0 au repos | Non résident : ne consomme que le temps de son affichage. |
| Compositeur | **picom** | ~25 Mo | Coins arrondis et ombres. Sans flou. |
| Notifications | **dunst** | ~8 Mo | |
| Session | **LightDM** | ~40 Mo | |

Total au repos visé : **~330 Mo**. Le budget de 400 Mo tient.

### Le dock : pourquoi plank et non tint2

Le premier jet utilisait tint2 pour le dock comme pour la barre d'état. **tint2
ne sait pas réorganiser ses lanceurs au glisser-déposer** : leur ordre est figé
dans le fichier de configuration. plank le fait nativement, comme le dock de
macOS, et enregistre lui-même le nouvel ordre. Il apporte au passage le zoom au
survol et les ombres d'icônes.

Coût : ~23 Mo de plus que tint2. tint2 reste pour la barre d'état, où ses
lanceurs ne servaient pas.

### Pourquoi pas un shell écrit sur mesure

Un second processus Electron coûterait 200 à 400 Mo sur une machine qui n'en a
que 4, dont Claude Desktop consomme déjà une bonne part. L'assemblage retenu
obtient le même résultat pour un dixième de ce coût.

## 4.3 Le rendu

Codes visuels communs aux deux panneaux, pour l'unité :

| Élément | Valeur |
|---|---|
| Fond des panneaux | `#202124` à 90 % d'opacité |
| Bordure | `#ffffff` à 10 % |
| Rayon des coins | 18 px (panneaux), 12 px (pastilles), 0 côté compositeur |
| Accent au survol | `#8ab4f8` (bleu ChromeOS) à 18 % |
| Texte principal | `#e8eaed` |
| Texte secondaire | `#9aa0a6` |
| Police | Inter |
| Hauteur des panneaux | 60 px, icônes 40 px |

Fond d'écran : dégradé indigo profond avec deux halos radiaux, généré par
script donc reproductible et versionnable, jamais téléchargé.

### Les fenêtres : un empilement, pas un cadre

Une fenêtre n'est pas un cadre mais trois bandes superposées — barre de titre
et ses trois boutons, barre d'outils, contenu — dont **seuls les deux coins
hauts sont adoucis**. Aucun contour : c'est l'ombre, et elle seule, qui détache
la fenêtre du fond.

**Cet arrondi ne peut pas venir du compositeur.** picom arrondit les quatre
coins ou aucun : `corner-radius`, `corner-radius-rules` et
`rounded-corners-exclude` agissent tous sur la fenêtre entière, aucune option
ne vise un coin en particulier. Un arrondi global produirait deux coins bas
arrondis, contraires à l'intention.

`corner-radius` est donc à **0**, et l'arrondi supérieur est laissé à ceux qui
savent le dessiner :

| Élément | Coins hauts arrondis ? | Par quoi |
|---|---|---|
| Chromium | Oui | Son propre cadre |
| Claude Desktop | Oui | Son propre cadre (Electron) |
| Dock | Oui | `TopRoundness` du thème plank |
| Barre d'état | Oui | `rounded` de tint2 |
| Bloc-notes, gestionnaire de fichiers, boîtes de dialogue | **Non** | Décorées par Openbox, qui ne sait pas arrondir |

Autrement dit : les deux applications réellement utilisées au quotidien ont
l'aspect voulu, les utilitaires restent à coins droits. Le curseur « Coins
hauts des fenêtres » du panneau de réglages permet de trancher autrement — à
une valeur non nulle, tous les coins sont arrondis, y compris ceux du bas.

**Le flou a été retiré** : coûteux sur un GPU 32 EU à 6 W, et sans apport réel
une fois les ombres en place. Ce sont elles qui font la lisibilité, en
détachant chaque surface de ce qu'il y a derrière. Le panneau de réglages
permet de le réactiver pour juger sur pièce.

## 4.4 Le cahier des charges, point par point

| # | Demande | État | Comment |
|---|---|---|---|
| 1 | Esthétique validée | ✅ | Palette, typographie et disposition figées. |
| 2 | Fenêtres sans cadre, barre de titre seule | ✅ | Thème Openbox `ClaudeOS` : `border.width: 0` supprime le contour, `window.handle.width: 0` la poignée du bas. Restent la barre de titre et ses trois boutons. |
| 3 | Pas de flou | ✅ | `blur-method = "none"`. Réactivable depuis le panneau de réglages. |
| 4 | Ombrage léger | ✅ | picom pour les fenêtres et les panneaux (rayon 15, opacité 30 %), `IconShadowSize=2` pour les icônes du dock. |
| 5 | Bouton lanceur sur le dock | ✅ | Première icône du dock, ou `Super + Espace`. rofi, thémé aux mêmes codes. |
| 6 | Icônes réorganisables au glisser-déposer | ✅ | Natif dans plank. C'est ce point qui a fait abandonner tint2 pour le dock. |
| 7 | Touche Loupe masque l'étagère | ⚠️ | `xcape` convertit un appui bref sur Super **seul** en F13, lié à `claude-os-toggle-shelf`. **À vérifier sur la machine** que la touche Loupe émet bien `Super_L`. |
| 8 | Fenêtres maximisées sous l'étagère | ✅ | Aucun *strut* réservé, marge Openbox à zéro, panneaux au-dessus. |
| 9 | Panneau de réglages d'affichage | ✅ | `claude-os-settings` : ombres, flou, fondus, arrondis, dock, barre d'état. Écrit dans `~/.config/claude-os/`, jamais dans `/usr`, et « Tout réinitialiser » revient à l'origine. |
| 10 | Icônes et dossiers sur le bureau | ✅ | `pcmanfm --desktop`. |
| 11 | Google Drive et OneDrive | 📋 | Plus tard. Voir ci-dessous. |
| 12 | Écran tactile | 📋 | Plus tard. Voir ci-dessous. |

### Deux réserves sur ce tableau

**Point 2 et les applications qui dessinent leur propre cadre.** Chromium et
Claude Desktop (Electron) tracent eux-mêmes leur barre supérieure. Leur
appliquer en plus une barre de titre Openbox ferait double emploi : elles sont
donc déclarées sans décoration, et c'est leur propre barre — qui porte déjà les
trois boutons — qui s'affiche. Le thème Openbox vaut pour tout le reste :
bloc-notes, gestionnaire de fichiers, boîtes de dialogue.

**Point 4 et les icônes de la barre d'état.** plank ombre ses icônes, tint2
non : il n'expose aucun réglage d'ombre par icône. Les trois icônes de la zone
de notification n'auront donc que l'ombre du panneau qui les porte. L'écart
sera peu visible à 20 px, mais il existe.

## 4.5 Les deux demandes reportées

### Google Drive et OneDrive dans le gestionnaire de fichiers

La voie retenue est **rclone**, qui gère les deux services et se monte comme un
dossier ordinaire — donc visible dans PCManFM sans qu'il ait à en savoir quoi
que ce soit. L'alternative, GNOME Online Accounts avec gvfs, tirerait une bonne
partie de GNOME : hors de question ici.

Montage par unités systemd utilisateur, avec cache limité pour ménager l'eMMC.
C'est effectivement pertinent vu la capacité du disque : les fichiers restent
en ligne et ne descendent qu'à la demande.

### Écran tactile

`xserver-xorg-input-libinput` est déjà installé et gère le tactile sans
configuration dans la majorité des cas. Le travail réel consistera à vérifier
la détection, l'étalonnage, et le défilement tactile dans Chromium. La rotation
automatique dépend d'un accéléromètre exposé via IIO — c'est le relevé matériel
qui le dira.

Coût attendu : faible. À traiter après la validation matérielle.

## 4.6 Ce qui est volontairement absent

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

## 4.7 Points à valider sur la machine

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

## 4.8 Sources

- Claude Desktop sous Linux (dépôt apt, empreinte de clé, limites de la bêta,
  prérequis Cowork) — <https://code.claude.com/docs/en/desktop-linux>
