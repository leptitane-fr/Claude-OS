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

Un Chromebook refuse d'écrire dans sa mémoire flash de démarrage tant que le
**write-protect matériel** est actif. Il faut le lever pour flasher un firmware
UEFI.

Sur Jasper Lake / Dedede, il **n'y a pas de vis de write-protect** : la
protection est pilotée par la puce de sécurité **CR50** (Google Security Chip).
La documentation MrChromebox indique pour les modèles HP de cette génération une
levée par `CR50 (SuzyQ, battery)`, soit deux voies :

- **Déconnexion de la batterie** — ouvrir le châssis, débrancher le connecteur de
  batterie, travailler sur secteur. Voie la plus courante, sans matériel spécifique.
- **Câble SuzyQ (CCD)** — câble USB-C de débogage permettant d'ouvrir le *Closed
  Case Debugging* du CR50 sans démontage. Nécessite d'acheter ou fabriquer le câble.

> Les commandes exactes (`gsctool`, ouverture CCD, présence physique par
> pressions répétées sur le bouton d'alimentation) **ne doivent pas être
> recopiées de mémoire** : elles varient selon la version du CR50 embarquée. Il
> faut les prendre dans la documentation MrChromebox au moment de l'opération,
> après avoir relevé la version du CR50 (`gsctool -a -f`, inclus dans le relevé).

Prérequis dans tous les cas : **mode développeur activé** sur ChromeOS.
L'activation du mode développeur **efface les données locales** de la machine.

---

## 1.3 Décisions arrêtées

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

## 1.4 Protocole de sauvegarde

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

3. **Copier le `.rom` sur deux supports distincts** — la clé USB de travail
   n'est pas une sauvegarde à elle seule.
4. **Committer le manifeste**, pas le firmware. Le script produit
   `firmware-backup-manifest.txt` (tailles et sommes SHA-256) : c'est lui qui
   va dans le dépôt.

> ⚠️ **Ne jamais committer ni publier le fichier `.rom`.** Un dump de firmware
> ChromeOS contient les régions VPD, donc le **numéro de série** de la machine
> et son **adresse MAC**. `.gitignore` exclut déjà `*.rom` par précaution.

---

## 1.5 Règles pendant le flash

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
---

## 1.6 Risques logiciels identifiés

À valider en live USB **après** le flash, avant de construire l'image finale.

| Risque | Détail | Gravité |
|---|---|---|
| **Audio** | Les Chromebooks Jasper Lake utilisent SOF (Sound Open Firmware) avec un codec discret et des amplificateurs de haut-parleurs pilotés séparément. Symptôme classique : le casque fonctionne, les haut-parleurs internes restent muets faute du bon profil UCM. Le noyau 6.12 de Debian 13 et un `alsa-ucm-conf` récent améliorent nettement la situation, sans garantie. | **Élevée** — c'est le premier point à tester. |
| **Wi-Fi** | Firmware `iwlwifi` non libre requis. S'il n'est pas embarqué dans l'image d'installation, la machine démarre sans réseau. | Moyenne, mais bloquante à l'installation. |
| **Veille** | Le S0ix sur Chromebook hors ChromeOS est souvent imparfait : consommation en veille supérieure à l'origine. | Moyenne — confort. |
| **Clavier** | Rangée de touches ChromeOS non standard ; pas de touches F1–F12 physiques. Nécessite un remappage. | Faible — purement logiciel. |
| **Rotation / tactile** | Convertible : dépend de la présence d'un accéléromètre exposé via IIO. | Faible — confort. |

---

## 1.7 Sources

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
