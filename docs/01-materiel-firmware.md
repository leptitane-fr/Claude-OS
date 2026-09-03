# 1. Matériel et firmware

## 1.1 La machine

**HP Chromebook x360 14b-cb0000sf** — convertible 14", SKU française.

### Le piège du nom commercial

« HP Chromebook x360 14b » recouvre **cinq plateformes matérielles
incompatibles entre elles**. Le suffixe de deux lettres est le seul
discriminant, et il détermine entièrement le firmware applicable :

| Modèle | Board name | Plateforme |
|---|---|---|
| 14b-**ca0** | `BLOOGUARD` | Intel Gemini Lake |
| 14b-**cb0** ← **la nôtre** | **`MADOO`** | **Intel Jasper Lake** |
| 14b-**cc0** / **cd0** | `JOXER` | Intel Alder Lake-N |
| 14b-**ce0** | `KALADIN` | Intel Alder Lake-N |
| 14b-**na0** | `BERKNIP` | AMD (famille Zork) |

Se tromper de deux lettres, c'est flasher un firmware conçu pour une autre
puce. C'est le risque le plus bête et le plus coûteux du projet.

### Trois confirmations indépendantes

**1. Base de données MrChromebox** (`device-db.sh`, référence pour la
compatibilité firmware) :

```
["MADOO*"]="HP Chromebook x360 14b-cb0|JSL|||"
```

Format déclaré en tête du fichier : `[HWID]="description|CPU|override|flags|"`.

| Champ | Valeur | Conséquence |
|---|---|---|
| Board name | `MADOO` | L'identifiant qui compte pour le firmware. |
| Plateforme | `JSL` — Jasper Lake | Famille ChromeOS « dedede ». |
| Override | *(vide)* | Firmware propre à la carte. |
| Flags | *(vide)* | **Pas de drapeau `noUEFI` → UEFI Full ROM disponible.** |

**2. Index officiel des images de récupération ChromeOS** publié par Google
(`dl.google.com/dl/edgedl/chromeos/recovery/recovery.json`) :

```json
{
  "hwidmatch": "^MADOO.*",
  "model":     "Chromebook x360 14b-cb0",
  "file":      "chromeos_16733.57.0_dedede_recovery_..."
}
```

Point notable : le motif est `^MADOO.*`, **sans suffixe à quatre lettres**,
contrairement à la quasi-totalité des autres entrées (`^DRAWCIA-CFUL.*`,
`^JOXER-MEZV.*`…). Autrement dit, **tous** les `14b-cb0` sont des MADOO : il
n'existe pas de sous-variante régionale susceptible d'échapper à la règle. Le
SKU `sf` est couvert. Le nom de fichier confirme par ailleurs la famille
`dedede`, donc Jasper Lake.

**3. Cohérence du processeur.** Le Pentium Silver **N6000** relevé sur la
machine est un Jasper Lake (4 cœurs / 4 threads, 1,1–3,3 GHz, UHD Graphics
32 EU, 6 W, LPDDR4x). Il ne peut pas équiper un BLOOGUARD (Gemini Lake) ni un
JOXER (Alder Lake-N). La cohérence CPU ↔ board ↔ famille est totale.

### Vérifier sur la machine, sans mode développeur

Les confirmations ci-dessus portent sur le *modèle*. Il reste à vérifier
l'exemplaire. Les deux méthodes suivantes **n'exigent ni mode développeur ni
effacement** — à faire dès maintenant, depuis ChromeOS :

1. **Contrôle en dix secondes** — ouvrir `chrome://version`. La ligne
   `Platform` se termine par le board name en minuscules :

   ```
   Platform   16733.57.0 (Official Build) stable-channel madoo
                                                        ^^^^^
   ```

2. **Contrôle formel** — ouvrir `chrome://system`, chercher le champ `hwid`
   (ou la section `crossystem`). Il donne le HWID complet, qui doit commencer
   par `MADOO` :

   ```
   hwid    MADOO A6B-C7D-E8F
   ```

Si l'un de ces deux affichages ne commence pas par `madoo` / `MADOO`,
**s'arrêter** : toute la cible firmware du projet est à revoir.

Le script `tools/probe-hardware.sh` effectue automatiquement cette
comparaison et rend un verdict explicite (`[OK]`, `[ALERTE]`), mais il
suppose le mode développeur — d'où l'intérêt des deux contrôles navigateur
en amont.

### Spécifications

| Élément | Valeur | Statut |
|---|---|---|
| Board name | `MADOO` | Confirmé sur trois sources ; **à valider sur l'exemplaire** via `chrome://version`. |
| Plateforme | Jasper Lake (famille `dedede`) | Confirmé. |
| Processeur | Pentium Silver **N6000** — 4C/4T, 1,1–3,3 GHz, 6 W | Relevé utilisateur. |
| Graphiques | Intel UHD (Jasper Lake), 32 EU | Déduit du CPU. |
| Mémoire | **4 Go** LPDDR4x, soudée | Relevé utilisateur. **Contrainte dimensionnante n°1.** |
| Stockage | eMMC, capacité à confirmer | À relever. |

> La mémoire n'est pas extensible : elle est soudée. Les 4 Go sont donc un
> plafond définitif, et non un point de départ améliorable.
---

## 1.2 Le verrou : write-protect

Sur Jasper Lake / Dedede, il **n'y a pas de vis de write-protect** : la
protection est pilotée par la puce **CR50**. Sur la plupart des plateformes
CR50, le CR50 aligne l'état du WP sur la ligne de détection de la batterie —
**débrancher le connecteur de batterie de la carte mère lève donc la
protection**.

Attention : ce n'est pas universel. Sur certaines plateformes plus récentes, le
WP ne se lève pas par la batterie mais par un cavalier non peuplé à ponter.
D'où la règle : **ne jamais supposer, toujours vérifier.**

### Vérification (impérative avant toute écriture)

```sh
sudo crossystem wpsw_cur
```

**`sudo` est indispensable.** `wpsw_cur` lit l'état d'une broche GPIO, ce qui
exige root. Sans `sudo`, la commande échoue ainsi — et cette erreur ne dit
rien de l'état du write-protect :

```
Unable to open /dev/gpiochip0
open: Permission denied
```

Relevé plus complet, également utile pour la suite :

```sh
for k in wpsw_cur mainfw_type devsw_boot cros_debug; do
  printf '%-14s %s\n' "$k" "$(sudo crossystem $k)"
done
```

| Valeur | Signification |
|---|---|
| `0` | Write-protect **levé** — le flash est possible. |
| `1` | Write-protect **toujours actif** — la déconnexion de batterie n'a pas suffi sur cette carte. Ne rien flasher ; il faut la voie CCD (SuzyQ) ou le cavalier. |

### Accès au terminal : VT2, pas crosh

Depuis **ChromeOS R117**, le script MrChromebox ne fonctionne plus depuis le
shell de `crosh`. Il faut un vrai terminal virtuel :

- **Ctrl + Alt + F2** (la touche `→` de la rangée du haut) pour basculer sur VT2 ;
- se connecter en `chronos` (aucun mot de passe en mode développeur) ;
- **Ctrl + Alt + F1** pour revenir à l'interface graphique.

Le script se lance en utilisateur normal, pas en root — il appelle `sudo`
lui-même :

```sh
cd; curl -LOf https://mrchromebox.tech/firmware-util.sh && sudo bash firmware-util.sh
```

### Après le flash

Séquence de remontage imposée par la procédure : éteindre la machine,
**débrancher l'alimentation externe**, puis rebrancher délicatement le
connecteur de batterie. La batterie reste débranchée pendant toute la durée du
flash.

---

## 1.3 Le risque propre à cette configuration

Batterie débranchée, la machine n'a **aucune réserve d'énergie** : une coupure
secteur, une prise mal enfoncée ou une multiprise à interrupteur coupe le
courant instantanément. Si cela survient pendant l'écriture de la puce, la
machine est brickée — et, sans programmateur SPI externe (arbitrage acté en
§1.4), irrécupérable.

Ce risque est inhérent à la procédure : le WP ne peut pas être levé autrement
sur cette carte. Il se réduit, il ne s'élimine pas :

- alimentation branchée **directement au mur**, pas sur une multiprise commutée ;
- câble et connecteur vérifiés, machine immobile pendant l'écriture ;
- ne rien flasher tant que la sauvegarde n'est pas faite **et vérifiée**.

## 1.4 Décisions arrêtées

Deux arbitrages ont été tranchés en amont ; ils conditionnent la procédure.

### Levée du write-protect : décidée après le relevé

Le choix entre **déconnexion de la batterie** et **câble SuzyQ** est reporté
après l'exécution de `tools/probe-hardware.sh`. Motif : la procédure CCD exacte
dépend de la version du CR50 embarquée, que le relevé donne (`gsctool -a -f`),
et l'état courant du write-protect (`crossystem wpsw_cur`) peut déjà être
différent de celui supposé. Décider avant de savoir n'apporterait rien.

Le relevé fournit les trois entrées de la décision : version du CR50, état CCD,
état du write-protect matériel.

### Récupération : sauvegarde USB, sans programmateur externe

**Aucun programmateur SPI externe ne sera acquis.** Conséquence directe et
structurante : la sauvegarde du firmware d'origine devient **le seul filet de
sécurité du projet**. Un flash interrompu ou une sauvegarde corrompue ne serait
pas rattrapable.

Cela ne rend pas l'opération déraisonnable — les flashs réussissent dans
l'immense majorité des cas — mais cela déplace toute l'exigence sur la qualité
de la sauvegarde. D'où le protocole ci-dessous, qui n'est pas optionnel.

---

## 1.5 Protocole de sauvegarde

Une sauvegarde de firmware peut avoir la bonne taille et un contenu faux : une
lecture SPI échoue parfois silencieusement. Le protocole retenu :

1. **Lire la puce deux fois**, dans deux fichiers distincts. Le script
   MrChromebox propose la sauvegarde ; la relancer une seconde fois.
2. **Vérifier et comparer** avec l'outil du dépôt :

   ```sh
   bash tools/verify-firmware-backup.sh dump1.rom dump2.rom
   ```

   Il contrôle la taille (une puce SPI fait 4, 8, 16 ou 32 Mio — toute autre
   taille signale une troncature), l'absence de dump vide (0x00 ou 0xFF
   intégral, symptôme d'une lecture ratée), la présence de la signature
   `__FMAP__`, celle des neuf régions attendues (`GBB`, `RO_SECTION`,
   `WP_RO`…), puis compare les deux lectures octet à octet.

   Codes de retour : `0` exploitable, `1` réserves, `2` **ne pas flasher**.

   **Depuis VT2, le dépôt est privé donc `tools/verify-firmware-backup.sh`
   n'est pas téléchargeable.** Contrôle équivalent à coller directement dans
   le terminal, une fois placé dans le dossier des sauvegardes :

   ```sh
   for f in *.rom; do
     printf '%-28s %10s octets  ' "$f" "$(stat -c%s "$f")"
     LC_ALL=C grep -aq __FMAP__ "$f" && printf 'FMAP:ok  ' || printf 'FMAP:ABSENT  '
     printf 'sha256:%s\n' "$(sha256sum "$f" | cut -c1-16)"
   done
   cmp bak1.rom bak2.rom && echo "IDENTIQUES — sauvegarde fiable" \
                         || echo "DIFFERENTES — relire la puce"
   ```

   Attendu : deux fichiers de taille identique et conforme (4/8/16/32 Mio),
   `FMAP:ok` sur les deux, et `IDENTIQUES`.

3. **Copier le `.rom` sur deux supports distincts** — la clé USB de travail
   n'est pas une sauvegarde à elle seule.
4. **Committer le manifeste**, pas le firmware. Le script produit
   `firmware-backup-manifest.txt` (tailles et sommes SHA-256) : c'est lui qui
   va dans le dépôt.

> ⚠️ **Ne jamais committer ni publier le fichier `.rom`.** Un dump de firmware
> ChromeOS contient les régions VPD, donc le **numéro de série** de la machine
> et son **adresse MAC**. `.gitignore` exclut déjà `*.rom` par précaution.

---

## 1.6 Règles pendant le flash

**1. Ne pas interrompre l'alimentation.**
Sur secteur, batterie rebranchée si elle avait été déconnectée pour le
write-protect — sauf si la procédure impose l'inverse, auquel cas s'assurer que
l'alimentation secteur est stable.

**2. Le flash est définitif pour ChromeOS.**
Le firmware UEFI Full ROM remplace intégralement le firmware d'origine :
ChromeOS ne démarre plus. C'est l'objectif, mais il n'y a pas de retour sans
restaurer la sauvegarde.

**3. Ne pas flasher si la vérification renvoie 2.**
Sans programmateur externe, ce serait un pari sans issue de secours.

**4. Avoir la clé Debian prête AVANT de flasher.**
Le flash supprime ChromeOS. Sans support d'installation sous la main, la
machine se retrouve à démarrer sur rien. La clé Debian 13 se prépare pendant
que la sauvegarde se fait, pas après.

### Liste de contrôle avant l'écriture

Les quatre points doivent être vrais simultanément :

- [ ] `crossystem wpsw_cur` renvoie **`0`**
- [ ] Deux sauvegardes du firmware faites, **vérifiées identiques**, copiées
      hors de la machine
- [ ] Clé USB d'installation **Debian 13 prête et testée**
- [ ] Alimentation branchée au mur, machine immobile
---

## 1.7 Risques logiciels identifiés

À valider en live USB **après** le flash, avant de construire l'image finale.

| Risque | Détail | Gravité |
|---|---|---|
| **Audio** | Les Chromebooks Jasper Lake utilisent SOF (Sound Open Firmware) avec un codec discret et des amplificateurs de haut-parleurs pilotés séparément. Symptôme classique : le casque fonctionne, les haut-parleurs internes restent muets faute du bon profil UCM. Le noyau 6.12 de Debian 13 et un `alsa-ucm-conf` récent améliorent nettement la situation, sans garantie. | **Élevée** — c'est le premier point à tester. |
| **Wi-Fi** | Firmware `iwlwifi` non libre requis. S'il n'est pas embarqué dans l'image d'installation, la machine démarre sans réseau. | Moyenne, mais bloquante à l'installation. |
| **Veille** | Le S0ix sur Chromebook hors ChromeOS est souvent imparfait : consommation en veille supérieure à l'origine. | Moyenne — confort. |
| **Clavier** | Rangée de touches ChromeOS non standard ; pas de touches F1–F12 physiques. Nécessite un remappage. | Faible — purement logiciel. |
| **Rotation / tactile** | Convertible : dépend de la présence d'un accéléromètre exposé via IIO. | Faible — confort. |

---

## 1.8 Sources

- Base de données des périphériques MrChromebox — `MrChromebox/scripts`, fichier
  `device-db.sh` (entrée `MADOO`, lue et vérifiée).
- Table des périphériques supportés — <https://docs.mrchromebox.tech/docs/supported-devices.html>
- Index officiel des images de récupération ChromeOS, publié par Google —
  <https://dl.google.com/dl/edgedl/chromeos/recovery/recovery.json>
  (713 entrées ; `^MADOO.*` → « Chromebook x360 14b-cb0 », famille `dedede`).
- Fiche processeur Intel Pentium Silver N6000 —
  <https://www.intel.com/content/www/us/en/products/sku/212330/intel-pentium-silver-n6000-processor-4m-cache-up-to-3-30-ghz/specifications.html>
- Capacités de décodage vidéo Jasper Lake (absence d'AV1) — Intel EDS Vol. 1,
  « Hardware Accelerated Video Decode and Encode ».
