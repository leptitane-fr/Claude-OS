# 5. Économie d'énergie

Exigence : optimisée et orientée économie d'énergie, **sans tomber dans
l'extrême**. La ligne suivie est donc : tout réglage qui provoque une
micro-coupure, une déconnexion ou un réveil perceptible est écarté, même s'il
rapporte quelques milliwatts.

## 5.1 Ce qui compte vraiment sur cette machine

Par ordre d'effet réel, pas par ordre de notoriété.

| Rang | Levier | Pourquoi il domine |
|---|---|---|
| 1 | **Décodage vidéo matériel** | Une vidéo décodée en logiciel occupe les quatre cœurs à plein régime. C'est de loin le premier poste de consommation sur un CPU 6 W. Voir `docs/02` §2.3 : AV1 doit être évité, VP9 et H.264 sont accélérés. |
| 2 | **Ne rien faire tourner d'inutile** | Un service résident coûte des réveils permanents. D'où l'absence d'indexeur, de télémétrie, de gestionnaire de paquets résident, et le masquage de ModemManager. |
| 3 | **Affichage** | Compression du tampon (`i915.enable_fbc=1`) et luminosité. L'écran est le second poste sur un portable. |
| 4 | **Gouverneur et politique d'énergie** | Réglé par TLP, avec `balance_power` sur batterie. |
| 5 | Veille des périphériques | Gain modeste, gêne potentielle élevée. Traité avec prudence. |

Le flou du compositeur a été retiré en partie pour cette raison : il faisait
travailler le GPU en permanence pour un effet décoratif.

## 5.2 TLP

Fichier : `rootfs/etc/tlp.d/99-claude-os.conf`.

| Réglage | Choix | Motif |
|---|---|---|
| Gouverneur | `powersave` sur secteur **et** batterie | Avec `intel_pstate`, `powersave` n'est pas un mode dégradé : c'est le mode normal, qui monte en fréquence à la demande. `performance` maintiendrait des fréquences hautes en continu sans gain perçu. |
| Politique d'énergie | `balance_performance` / `balance_power` | |
| **Boost maintenu sur batterie** | `CPU_BOOST_ON_BAT=1` | C'est lui qui rend l'interface réactive sur quatre cœurs à 6 W. Le couper économiserait peu et se sentirait beaucoup. |
| **Veille USB désactivée** | `USB_AUTOSUSPEND=0` | C'est le réglage qui fait décrocher souris, clés et casques USB. Le gain ne vaut pas la gêne. |
| Économie Wi-Fi | Sur batterie seulement | Elle ajoute quelques millisecondes de latence : invisible en navigation, gênante en visioconférence. |
| Veille du codec audio | Sur batterie seulement | Un délai trop court produit un clic audible à chaque reprise. |

## 5.3 Paramètres noyau

Fichier : `rootfs/etc/default/grub.d/99-claude-os.cfg`, appliqué par
`update-grub`.

| Paramètre | Effet |
|---|---|
| `i915.enable_fbc=1` | Compression du tampon d'affichage : le GPU relit moins souvent la mémoire. Se voit sur l'autonomie en affichage statique, c'est-à-dire l'essentiel du temps quand on lit une page. Sans effet visible sur l'image. |
| `mem_sleep_default=s2idle` | Les Chromebooks ne proposent pas la veille S3. L'indiquer évite une tentative infructueuse au premier suspend. |
| `nmi_watchdog=0` | Surveillance de débogage noyau sans objet ici, qui réveille chaque cœur périodiquement. |
| `i915.enable_psr=1` | **Commenté par défaut.** Panel Self Refresh : gain réel sur affichage statique, mais scintillement sur certaines dalles. À essayer et observer une minute sur une page fixe. |

## 5.4 Ce qui n'est délibérément pas fait

- **`powertop --auto-tune`.** Il active en bloc tous les réglages agressifs, y
  compris la veille USB. Le paquet est installé pour *mesurer*, pas pour
  décider à notre place.
- **Couper le Bluetooth au démarrage.** Il est demandé fonctionnel.
- **Brider le processeur.** Voir le boost ci-dessus.
- **Réduire la fréquence de rafraîchissement.** Gain marginal, confort dégradé.

## 5.5 Mesurer plutôt que supposer

Aucun chiffre d'autonomie n'est avancé ici : il dépend de la dalle, de l'usure
de la batterie et de l'usage réel. À faire une fois la machine installée :

```sh
tlp-stat -s -c                 # TLP actif, gouverneurs appliqués
powertop --auto-tune=false     # consommation par poste, sans rien modifier
cat /sys/class/power_supply/BAT*/power_now   # puissance instantanée, en µW
```

Protocole utile : relever la puissance instantanée au repos, écran allumé,
session ouverte sans activité. C'est ce chiffre qui permet de comparer avant et
après un changement, bien mieux qu'une estimation d'autonomie.
