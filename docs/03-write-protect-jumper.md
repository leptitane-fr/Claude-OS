# 3. Identifier le cavalier de write-protect sur MADOO

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

---

## 3.5 Sources

- Photos de référence et méthodes — <https://docs.mrchromebox.tech/docs/firmware/wp/disabling.html>
- Table des périphériques (colonne WP Method) — <https://docs.mrchromebox.tech/docs/supported-devices.html>
- Protection en écriture du firmware — <https://chromium.googlesource.com/chromiumos/docs/+/master/write_protection.md>
