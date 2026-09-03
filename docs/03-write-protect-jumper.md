# 3. Le cavalier de write-protect sur MADOO

> ## ✅ RÉSOLU — `J1` est le cavalier de write-protect
>
> **Mesuré sur la machine**, cavalier `J1` ponté, machine redémarrée :
>
> ```
> $ sudo crossystem wpsw_cur
> 0
> ```
>
> `J1` est la **paire basse** de trous métallisés traversants, située sous le
> lecteur microSD, à gauche de la diode `D25`, au-dessus de `D58`, avec
> `Q1018` à sa droite. Son marquage `J1` est sérigraphié immédiatement à
> droite de la paire.
>
> **C'est une information neuve.** Le fil chrultrabook consacré à cette carte
> désignait `J1` comme candidat mais n'a jamais vérifié `wpsw_cur` ; MrChromebox
> note encore `jumper?` pour MADOO. La vérification manquante est faite.
>
> Le pont doit rester en place pendant la sauvegarde **et** pendant le flash :
> l'état du write-protect est relu à chaque démarrage.

---

## 3.0 Historique de l'identification

> **Contexte.** `sudo crossystem wpsw_cur` renvoie `1` batterie débranchée :
> MADOO ne lie pas le write-protect à la ligne batterie (cf. `docs/01` §1.2).
> Restent le câble SuzyQ ou le cavalier. Cette page documente la voie cavalier.
>
> **L'emplacement du cavalier n'est pas publié pour MADOO.** MrChromebox note
> `jumper?`, avec un point d'interrogation et sans photo — contrairement à
> DRAWCIA, BOTEN et CRET. Ce document sert à l'identifier par comparaison.

---

## 3.1 Ce que l'on cherche

Trois cartes Dedede de référence, photographiées par MrChromebox, permettent de
dégager la constante et les variables.

| Carte | Fabricant | Marquage | Emplacement |
|---|---|---|---|
| **DRAWCIA** | HP | **`H_WP`** | Immédiatement **à gauche de la puce SPI** |
| BOTEN | Lenovo | *(aucun visible)* | Paire dans une rangée de points de test, près du bord |
| CRET | Dell | `H31` *(+ vis « WP SCREW » en alternative)* | Près du bord, à distance de la puce SPI |

**Ce qui est constant — la forme :**

- deux pastilles **rondes**, dorées ou cuivrées, **non peuplées** (aucun
  composant soudé dessus) ;
- **rapprochées**, de l'ordre du millimètre ;
- souvent alignées dans une rangée de points de test similaires.

**Ce qui varie — l'emplacement et le marquage.** Il n'existe pas de position
universelle : chaque fabricant place ce cavalier où il veut.

**Notre meilleur analogue est DRAWCIA** : même fabricant (HP), même plateforme
(Jasper Lake / Dedede), même génération (2021). Il y a une chance sérieuse que
MADOO reprenne la même convention de sérigraphie `H_WP` et un emplacement
voisin de la puce SPI.

> Photos de référence (à consulter directement chez MrChromebox, non
> redistribuées ici) :
> [DRAWCIA](https://docs.mrchromebox.tech/images/wp/Drawcia_wp.jpg) ·
> [BOTEN](https://docs.mrchromebox.tech/images/wp/Boten_wp.jpg) ·
> [CRET](https://docs.mrchromebox.tech/images/wp/Cret_jumper.jpg)

---

## 3.1 bis Candidats identifiés sur la carte

### Ce que dit la communauté

Un fil du forum chrultrabook porte exactement sur cette question :
[« Identifying WP Jumper on HP MADOO board »](https://forum.chrultrabook.com/t/identifying-wp-jumper-on-hp-madoo-board/6520).

Enseignements, à prendre pour ce qu'ils valent :

- Le cavalier **n'a jamais été identifié avec certitude**. L'auteur du fil
  écrit lui-même qu'il n'est « pas sûr à 100 % ».
- Le candidat retenu est **`J1`, près du lecteur microSD**.
- **Aucun marquage `H_WP`** n'existe sur cette carte, contrairement à DRAWCIA.
- Ponter `J1` a permis d'exécuter `ccd open` alors que la machine ne répondait
  plus — donc `J1` fait *quelque chose*, sans qu'on sache quoi exactement.
- **`wpsw_cur` n'a jamais été vérifié** après pontage. C'est précisément la
  vérification qui manque, et qu'il nous revient de faire.

### ⚠️ Incident documenté dans ce fil

L'auteur a tenté `gsctool -a -o` (ouverture CCD) : **la machine s'est éteinte
en cours d'opération et n'a plus démarré**. Elle n'a été récupérée que grâce à
un câble SuzyQ.

**Conséquence directe pour nous : ne pas exécuter `gsctool -a -o`.** Sans
SuzyQ ni programmateur SPI, un tel incident serait terminal. Cette commande
est écartée tant que le câble n'est pas en notre possession.

### Repérage sur notre exemplaire

Les photos de la carte confirment la zone décrite par le forum. Sur la partie
de carte mère située **juste sous le lecteur microSD**, deux paires de trous
métallisés traversants sont alignées verticalement, chacune dans un cadre
sérigraphié blanc :

| Repère | Position | Voisinage immédiat |
|---|---|---|
| **`J2`** | Paire du **haut** | Sous le repère `J9`, à gauche du bloc `CR1`/`CR6`/`CR7` |
| **`J1`** | Paire du **bas** | À gauche de la diode `D25`, au-dessus de `D58`, marquage `Q1018` à droite |

Le marquage `J1` est sérigraphié immédiatement à droite de la paire basse,
`J2` immédiatement à droite de la paire haute. Repère de situation
supplémentaire : le grand marquage `M2x2.5` et un QR code de carte se trouvent
à gauche des deux paires.

Ce sont des **trous traversants**, pas des pastilles de surface : un fil
peut y être passé, ce qui rend le pontage temporaire plus simple et plus sûr
qu'une soudure.

**`J1` est le candidat n° 1** (celui du forum), **`J2` le candidat n° 2**.

---

## 3.2 Méthode d'identification

### Étape 1 — Trouver la puce SPI, pas le cavalier

Le cavalier se cherche **à partir de** la puce de mémoire flash, qui est bien
plus facile à repérer :

- boîtier **SOIC-8** (8 pattes, 4 de chaque côté), environ **6 × 5 mm** ;
- marquage du type `winbond 25Q128JVSM`, `GD25...` ou `MX25...` ;
- fréquemment entourée d'un **cadre blanc sérigraphié**.

> Sur DRAWCIA la puce est une **Winbond 25Q128** — 128 Mbit, soit **16 Mio**.
> C'est la taille de dump attendue sur ces cartes, et donc la valeur que doit
> confirmer `tools/verify-firmware-backup.sh`.

### Étape 2 — Balayer les alentours

Chercher une paire de pastilles rondes non peuplées, en priorité **à gauche de
la puce SPI** (position DRAWCIA), puis en élargissant. Lire la sérigraphie
juste au-dessus ou à côté : tout marquage contenant **`WP`** — `H_WP`,
`WP`, `FW_WP`, `SPI_WP` — est un candidat sérieux.

### Étape 3 — Photographier avant de toucher

Trois clichés, nets, en lumière rasante :

1. vue d'ensemble de la carte mère, pour la mise en situation ;
2. gros plan sur la puce SPI **et ses alentours immédiats** ;
3. macro sur toute paire de pastilles candidate, **sérigraphie lisible**.

**Ne rien ponter avant identification confirmée.**

---

## 3.3 Protocole de pontage

### Règle : réversible d'abord, définitif ensuite

Ne jamais souder en première intention. Un pont temporaire se retire ; une
soudure sur les mauvaises pastilles, non.

Par ordre de préférence :

1. **Fil ou pince** maintenu manuellement pendant l'allumage ;
2. **Crayon graphite** frotté sur les deux pastilles — résistif, non
   destructif, effaçable ;
3. **Soudure**, seulement une fois le bon emplacement prouvé.

### Le WP est verrouillé au démarrage

Point qui fait échouer beaucoup de tentatives : l'état du write-protect est
**lu au démarrage**. Ponter machine allumée ne change rien. Le pont doit être
en place **avant** la mise sous tension, et le rester.

### Séquence

```
1. Machine éteinte, alimentation débranchée, batterie débranchée
2. Mettre en place le pont sur les pastilles candidates
3. Rebrancher la batterie
4. Rebrancher le secteur, démarrer
5. Ctrl+Alt+F2, login chronos
6. sudo crossystem wpsw_cur
```

| Résultat | Interprétation |
|---|---|
| **`0`** | Trouvé. Le write-protect est levé, on passe à la sauvegarde firmware. |
| `1` | Mauvaise paire, ou pont ne faisant pas contact. Éteindre, retirer le pont, reprendre à l'étape 2 avec un autre candidat. |

Un `1` persistant n'abîme rien : c'est un essai infructueux, pas une erreur.

---

## 3.4 Ce qu'il ne faut pas faire

- **Ponter au hasard.** Deux pastilles rapprochées ne sont pas forcément un
  cavalier WP : ce peut être une ligne d'alimentation, un bus, une entrée de
  charge. Les shunter peut détruire la carte. Sans marquage lisible ni
  correspondance avec une photo de référence, on ne ponte pas.
- **Souder avant d'avoir validé** par un pont temporaire.
- **Travailler sous tension.** Batterie et secteur débranchés pendant toute
  manipulation sur la carte.
- **Insister.** Si aucun candidat ne donne `0`, la voie cavalier est un échec
  pour cette carte : on repart sur le câble SuzyQ, qui reste la seule méthode
  officiellement documentée pour MADOO.
- **Exécuter `gsctool -a -o`.** Écarté tant que nous n'avons pas de SuzyQ :
  un incident documenté sur cette carte exacte a rendu la machine non
  démarrable en cours d'opération, et seul un SuzyQ a permis de la récupérer.

---

## 3.5 Sources

- Photos de référence et méthodes — <https://docs.mrchromebox.tech/docs/firmware/wp/disabling.html>
- Table des périphériques (colonne WP Method) — <https://docs.mrchromebox.tech/docs/supported-devices.html>
- Protection en écriture du firmware — <https://chromium.googlesource.com/chromiumos/docs/+/master/write_protection.md>
