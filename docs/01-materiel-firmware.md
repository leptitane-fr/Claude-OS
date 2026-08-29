# 1. Matériel et firmware

## 1.1 La machine

**HP Chromebook x360 14b-cb0000sf** — convertible 14", SKU française.

Identification dans la base de données officielle des scripts MrChromebox
(`device-db.sh`, source de vérité pour la compatibilité firmware) :

```
["MADOO*"]="HP Chromebook x360 14b-cb0|JSL|||"
```

Décodage, selon le format déclaré en tête de ce fichier
(`[HWID]="deviceDescription|CPU|device override|flags|"`) :

| Champ | Valeur | Conséquence |
|---|---|---|
| Board name | `MADOO` | C'est l'identifiant qui compte. Le nom commercial ne sert à rien pour le firmware. |
| Plateforme | `JSL` — Intel **Jasper Lake** | Famille ChromeOS « Dedede ». |
| Device override | *(vide)* | Firmware propre à la carte, pas d'emprunt à une autre. |
| Flags | *(vide)* | **Absence du drapeau `noUEFI` → le firmware UEFI Full ROM est disponible.** |

C'est le point qui rend le projet viable : `MADOO` peut recevoir un firmware UEFI
complet et démarrer un Linux standard, sans `RW_LEGACY` ni contournement du
bootloader ChromeOS.

> **À confirmer par le relevé.** Le HWID réel de *cette* machine doit être lu
> avec `crossystem hwid` avant toute manipulation. Si le board n'est pas `MADOO`,
> toute la cible firmware change. Voir `tools/probe-hardware.sh`.

### Spécifications attendues (à valider, non vérifiées à ce stade)

Les variantes `14b-cb0xxx` circulent en Celeron N4500, N5100 ou Pentium Silver
N6000, avec 4 ou 8 Go de LPDDR4x et 64 ou 128 Go d'eMMC. **La configuration
exacte de cette machine n'est pas connue** et conditionne directement le budget
mémoire du chapitre 2. Le relevé tranche.

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
