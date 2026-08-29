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

## 1.3 Points de non-retour

C'est la seule phase du projet où une erreur coûte du matériel. Trois règles.

**1. Sauvegarder le firmware d'origine, sur un support externe.**
Le script MrChromebox propose cette sauvegarde ; elle doit être écrite sur une
clé USB, pas sur l'eMMC qui sera effacé. Sans elle, revenir à ChromeOS n'est
plus possible sans reconstruire une image firmware depuis une source tierce.

**2. Ce n'est pas « débrickable » sans matériel.**
Contrairement à une idée répandue, un flash raté du firmware complet ne se
répare pas depuis le logiciel : il faut un programmateur SPI externe (type
CH341A) et une pince SOIC-8 pour réécrire directement la puce. À prévoir *avant*
de flasher, pas après.

**3. Ne pas interrompre l'alimentation pendant le flash.**
Sur secteur, batterie rebranchée si elle avait été déconnectée pour le
write-protect — sauf si la procédure impose l'inverse, auquel cas s'assurer que
l'alimentation secteur est stable.

**Conséquence sur ChromeOS :** le flash UEFI Full ROM remplace intégralement le
firmware. ChromeOS ne démarre plus. C'est l'objectif, mais c'est définitif tant
que la sauvegarde n'est pas restaurée.

---

## 1.4 Risques logiciels identifiés

À valider en live USB **après** le flash, avant de construire l'image finale.

| Risque | Détail | Gravité |
|---|---|---|
| **Audio** | Les Chromebooks Jasper Lake utilisent SOF (Sound Open Firmware) avec un codec discret et des amplificateurs de haut-parleurs pilotés séparément. Symptôme classique : le casque fonctionne, les haut-parleurs internes restent muets faute du bon profil UCM. Le noyau 6.12 de Debian 13 et un `alsa-ucm-conf` récent améliorent nettement la situation, sans garantie. | **Élevée** — c'est le premier point à tester. |
| **Wi-Fi** | Firmware `iwlwifi` non libre requis. S'il n'est pas embarqué dans l'image d'installation, la machine démarre sans réseau. | Moyenne, mais bloquante à l'installation. |
| **Veille** | Le S0ix sur Chromebook hors ChromeOS est souvent imparfait : consommation en veille supérieure à l'origine. | Moyenne — confort. |
| **Clavier** | Rangée de touches ChromeOS non standard ; pas de touches F1–F12 physiques. Nécessite un remappage. | Faible — purement logiciel. |
| **Rotation / tactile** | Convertible : dépend de la présence d'un accéléromètre exposé via IIO. | Faible — confort. |

---

## 1.5 Sources

- Base de données des périphériques MrChromebox — `MrChromebox/scripts`, fichier
  `device-db.sh` (entrée `MADOO`, lue et vérifiée).
- Table des périphériques supportés — <https://docs.mrchromebox.tech/docs/supported-devices.html>
