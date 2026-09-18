# La carte son

Écrit le 18 septembre 2026, et **vu fonctionner sur MADOO** : haut-parleurs et
microphone interne, éprouvés l'un par l'autre, puis confirmés par l'utilisateur
— il a dicté sa réponse au micro réparé.

---

## Ce que c'est

Un contrôleur **Intel Jasper Lake HD Audio** (`00:1f.3`), piloté par la pile
**SOF** (`sof-audio-pci-intel-icl`), avec un codec **RT5682** pour le casque et
un amplificateur **MAX98360A** pour les haut-parleurs. Carte ALSA
`sofrt5682`, nom long `HP-Madoo-rev4`.

Deux pannes distinctes ont été traitées le 18 septembre :

| Panne | Cause | État |
|---|---|---|
| Plus aucun son, par intermittence | Le DSP répond trop tard au démarrage, la carte échoue à s'instancier **en entier** | **Rattrapée** par un service |
| Le micro n'a jamais marché | Source par défaut sur un jack vide, et deux canaux morts qui écrasaient la voix | **Réparé** |

---

## Le matériel est sain, et c'est mesuré

La méthode qui a tranché : une **boucle acoustique**. Un bip de 440 Hz joué sur
les haut-parleurs, enregistré au même instant par le micro interne.

```
niveau capté, par tranche de 0,5 s (dBFS)
  canal 0 :  -72,5  -84,3  -83,8  -44,2  -42,9  -43,2  -43,3  -43,6  -44,2  -63,4
  canal 1 :  -72,8  -75,7  -80,3  -40,2  -38,8  -39,0  -39,0  -39,3  -39,9  -59,4
                                  └──────── le bip, 3 secondes ────────┘
```

Le niveau monte de trente décibels pendant exactement la durée du bip, puis
retombe. Un seul essai prouve alors **les deux** chaînes : la sortie émet, le
micro entend. Aucune des deux pannes n'était matérielle.

---

## Panne 1 — la carte disparaît un démarrage sur vingt

### Ce que fait le noyau

```
sof-audio-pci-intel-icl 0000:00:1f.3: ipc tx timed out for 0x30100000
sof-audio-pci-intel-icl 0000:00:1f.3: fw_state: SOF_FW_BOOT_COMPLETE (7)
sof-audio-pci-intel-icl 0000:00:1f.3: Failed to setup widget PIPELINE.12.DMIC1.IN
sof-audio-pci-intel-icl 0000:00:1f.3: error: tplg component load failed -110
sof_rt5682 jsl_rt5682_def: probe with driver sof_rt5682 failed with error -22
```

Le firmware DSP démarre (`SOF_FW_BOOT_COMPLETE`), puis une commande expire, et
le chargement de la topologie échoue. La carte n'est alors **pas instanciée du
tout** : `/proc/asound/cards` répond « no soundcards ». Pas de micro muet, pas
de haut-parleur muet — rien.

### Ce n'est pas une régression, c'est une course

Le journal conserve plus de cent démarrages. Balayés un par un, en ne comptant
que ceux où le DSP a été détecté :

| Démarrages | Résultat |
|---|---|
| 95 | carte instanciée |
| **5** | `failed to instantiate card` |

**Cinq échecs sur cent.** L'IPC qui expire n'est d'ailleurs pas toujours le
même — `0x30100000` sur le widget DMIC, `0x50010000` sur un autre démarrage :
c'est le DSP qui est lent à répondre, pas un widget fautif en particulier.

C'est ce qui rendait la panne si trompeuse. Le 8 septembre, la carte est
revenue après des installations de paquets, et le journal en a conclu une
réparation — voir « Le faux diagnostic » plus bas. Elle serait revenue de la
même façon sans rien installer, dix-neuf fois sur vingt.

### Le rattrapage

Recharger la pile une fois, à froid, suffit — vérifié en direct, carte revenue
en six secondes :

```sh
claude-os-root modprobe -r snd_sof_pci_intel_icl
claude-os-root modprobe    snd_sof_pci_intel_icl
```

C'est ce que fait `/usr/local/sbin/claude-os-rattrapage-audio`, lancé au
démarrage par `claude-os-rattrapage-audio.service`. Il **ne fait rien** quand
la carte est là, et n'agit que sur son absence. Éprouvé dans les deux cas :
carte présente (« rien à faire »), et carte retirée à la main puis restaurée.

Il écrit ce qu'il fait sous l'étiquette `claude-os-audio` :

```sh
journalctl -t claude-os-audio
```

**Ce service traite le symptôme, pas la cause.** Le jour où l'on saura
pourquoi le DSP traîne, il deviendra inutile.

---

## Panne 2 — le microphone

Deux causes se cumulaient. Toutes deux découlent de la même origine : le
firmware MrChromebox **n'expose pas de table ACPI NHLT**.

```
sof-audio-pci-intel-icl 0000:00:1f.3: NHLT table not found
sof-audio-pci-intel-icl 0000:00:1f.3: DMICs detected in NHLT tables: 0
```

Rien ne décrit donc les microphones au système, et la topologie générique en
déclare d'office quatre.

### Cause 1 — la source par défaut était un jack vide

PipeWire prenait « Headset » comme source par défaut, c'est-à-dire l'entrée du
**jack casque**. Sans casque branché, une application y lit du zéro numérique
parfait :

```
source par défaut (Headset) canal 0 : -999,0 dBFS
source par défaut (Headset) canal 1 : -999,0 dBFS
```

C'est ce que recevaient Chromium et tout le reste.

### Cause 2 — deux canaux morts écrasaient la voix

Le micro interne sort sur `hw:0,5` en **quatre canaux**, alors que la machine
n'a que **deux capsules**. Mesuré au repos :

| Canal | RMS | Crête | Ce que c'est |
|---|---|---|---|
| 0 (FL) | 0,000164 | 0,000856 | micro réel, plancher à −75 dBFS |
| 1 (FR) | 0,000240 | 0,001292 | micro réel, plancher à −72 dBFS |
| 2 | 0,465313 | 0,480871 | **valeur figée, −6,6 dBFS** |
| 3 | 0,465313 | 0,480871 | **valeur figée, −6,6 dBFS** |

Sur les canaux 2 et 3, RMS et crête sont presque égales : la ligne ne bouge
pas. Ce n'est pas de l'audio, c'est une entrée jamais câblée restée à un
niveau fixe.

PipeWire mélangeait les quatre canaux vers de la stéréo. Une constante à
−6,6 dBFS contre une voix à −42 dBFS : la voix disparaissait sous elle. C'est
ce que l'on entendait — un micro qui « ne marche pas ».

### Le correctif

`/etc/wireplumber/wireplumber.conf.d/51-claude-os-micro-interne.conf` marque
les deux canaux morts comme non attribués, et relève la priorité du micro :

```
audio.position   = [ FL, FR, UNK, UNK ]
priority.session = 1900
```

`UNK` les exclut du mélange, et seules les vraies capsules subsistent.

**La règle est en `/etc`, pas dans le répertoire de l'utilisateur**, et c'est
délibéré : le nombre de capsules est une propriété de la machine, pas une
préférence. La priorité règle la cause 1 sans dépendre d'un état utilisateur —
« Headset » sortait à **1728** contre **1664** au micro interne, d'où le choix
du jack. Vérifié en effaçant `~/.local/state/wireplumber/default-nodes` : le
micro interne est choisi quand même, et capte le bip.

**Rien d'autre n'a été nécessaire.** `api.alsa.disable-mmap` et
`api.alsa.period-size`, essayés d'abord, ont été retirés après vérification :
la position des canaux suffit seule. Le périphérique s'ouvre en
`MMAP_INTERLEAVED`, `S32_LE`, 4 canaux, 48 kHz.

---

## Le contournement d'ACP, et à quoi il sert vraiment

`/etc/wireplumber/wireplumber.conf.d/99-claude-os-audio.conf`, posé le
8 septembre, coupe ACP pour cette carte (`api.alsa.use-acp = false`) faute de
profil UCM. Il reste **nécessaire** : c'est lui qui fait créer un nœud par PCM
matériel, donc le nœud `…capture.5.0` que vise le correctif du micro.

Il porte aussi la conséquence à connaître : **pas de bascule automatique** au
branchement d'un casque, ni pour la sortie ni pour son micro. Il faudra
choisir à la main, tant qu'aucun profil UCM n'existe.

---

## Le faux diagnostic — « réparé le 8 septembre » était faux

`CLAUDE.md` et `docs/07` affirmaient que le `probe failed with error -22`
**avait disparu des journaux**. Il n'a jamais disparu : il revient cinq fois
sur cent démarrages, et il était là au démarrage du 17 septembre — d'où les
douze heures sans son qui ont ouvert cette séance.

Ce qui a été réellement acquis le 8 septembre est le contournement d'ACP
ci-dessus : sans lui, même une carte correctement instanciée ne sortait aucun
son. C'est utile, et ce n'était pas la panne.

**La leçon est celle que ce dépôt a déjà payée deux fois** (voir la reprise
après suspension, `docs/05`) : un symptôme intermittent que l'on observe une
fois guéri n'est pas guéri. Il faut compter, pas constater.

---

## Quatre pièges payés

### 1. `arecord` prouvait le contraire de ce que vivait l'utilisateur

`arecord -D hw:0,5` captait parfaitement pendant que toute application recevait
du silence. La différence n'était ni dans le matériel ni dans les mixeurs :
`arecord` demandait les quatre canaux et l'on regardait les deux premiers,
là où PipeWire les mélangeait tous les quatre.

**Tester un micro en ALSA direct ne dit rien de ce que reçoivent les
applications.** Mesurer ce que l'utilisateur vit, pas ce qui est commode à
mesurer — c'est la même faute que celle notée dans `docs/13`.

### 2. `pw-record --target` peut écrire du silence sans ouvrir la carte

En ciblant un nœud par `--target`, le fichier se remplissait de silence
pendant que `/proc/asound/card0/pcm5c/sub0/hw_params` disait `closed` : le
matériel n'était jamais ouvert. Sans `--target`, source par défaut réglée, la
liaison se fait et le PCM s'ouvre.

**Vérifier `hw_params` avant de conclure quoi que ce soit d'un enregistrement
vide.** Un flux de zéros ne prouve pas que le micro est muet ; il peut prouver
que personne ne lui a parlé.

### 3. Les noms de PCM contiennent une étoile, et le vérificateur s'y est pris

`wpctl status` marque le périphérique par défaut d'une `*` en début de ligne.
Mais les noms eux-mêmes en portent une : **« Headset (\*) »**, « DMIC (\*) ».
Un `grep '\*'` attrape donc **toutes** les lignes, et `head -1` rend la
première — le Headset. `tools/validate-install.sh` a ainsi annoncé « micro par
défaut : le JACK » alors que le micro interne était bien choisi.

Le motif juste vise l'étoile **suivie du numéro de nœud** :

```sh
grep -E '\*[[:space:]]+[0-9]+\.'
```

Et l'on se limite à la section `Audio` : la caméra a sa propre `Sources:`, avec
sa propre étoile.

### 4. Un fichier de configuration écrasé par une autre séance

Le journal du guichet montre `/etc/modprobe.d/99-claude-os-audio.conf` écrit à
12 h 47 le 8 septembre pour rendre `dmic_num=0` permanent, puis **réécrit
entièrement à 19 h 01** par une autre séance, pour un tout autre motif — la
mise à l'écart des pilotes concurrents. Le `dmic_num=0` a disparu là, sans que
personne le sache.

Il valait mieux qu'il disparaisse : il aurait supprimé le micro pour toujours.
Mais deux séances ont écrit dans le même fichier le même jour sans se voir.
**Un fichier par sujet, et un nom qui dit le sujet.**

---

## Ce qui n'est PAS établi

- **Aucun profil UCM n'existe pour `sof-rt5682`** — ni dans Debian, ni en
  amont. C'est la racine de tout ce qui précède. L'écrire serait le vrai
  remède : bascule casque automatique, noms de ports lisibles, ACP réactivable.
- **Pourquoi le DSP traîne un démarrage sur vingt** n'est pas élucidé. Le
  service rattrape, il n'explique pas.
- **`DMIC16kHz` (`hw:0,6`) n'a pas été éprouvé.** Il expose sans doute les
  mêmes quatre canaux et souffre du même défaut ; la règle ne le corrige pas.
- **Le micro du casque n'a pas été essayé** — aucun casque n'a été branché.
- **La sortie casque non plus**, pour la même raison.
- **Le firmware `intel/sof/community/sof-jsl.ri`** est celui que le pilote
  choisit ; il diffère de `intel-signed/sof-jsl.ri` (empreintes distinctes).
  Ne pas y toucher sans raison : c'est celui qu'attend un Chromebook reflashé,
  et il fonctionne quatre-vingt-quinze fois sur cent.

---

## Où lire quoi

| Quoi | Où |
|---|---|
| Ce que fait le noyau au démarrage | `claude-os-root dmesg \| grep -i sof` |
| Si la carte existe | `cat /proc/asound/cards` |
| Ce que le rattrapage a décidé | `journalctl -t claude-os-audio` |
| Ce que voient les applications | `wpctl status` |
| Si le matériel est vraiment ouvert | `cat /proc/asound/card0/pcm5c/sub0/hw_params` |

Les quatre fichiers qui portent tout cela sont versés au dépôt sous
[`rootfs/`](../rootfs/) — les deux règles WirePlumber, le script de rattrapage
et son unité — et `install/provision.sh` active le service.
