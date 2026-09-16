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
| `mem_sleep_default=deep` | **Veille S3, et non s2idle.** Le firmware MrChromebox annonce `ACPI: PM: (supports S0 S3 S4 S5)` — il offre le S3, contrairement au firmware ChromeOS d'origine. Mesure du 16 septembre 2026 : s2idle ne se réveille **jamais** sur cette machine (98 tentatives, 98 gels, puis l'EC qui réinitialise le processeur au bout de 64 s) ; en S3, 5 réveils sur 5. Ne pas revenir à s2idle sans remesurer. |
| `nmi_watchdog=0` | Surveillance de débogage noyau sans objet ici, qui réveille chaque cœur périodiquement. |
| `i915.enable_psr=1` | **Commenté par défaut.** Panel Self Refresh : gain réel sur affichage statique, mais scintillement sur certaines dalles. À essayer et observer une minute sur une page fixe. |

## 5.4 Ce qui n'est délibérément pas fait

- **`powertop --auto-tune`.** Il active en bloc tous les réglages agressifs, y
  compris la veille USB. Le paquet est installé pour *mesurer*, pas pour
  décider à notre place.
- **Couper le Bluetooth au démarrage.** Il est demandé fonctionnel.
- **Brider le processeur.** Voir le boost ci-dessus.
- **Réduire la fréquence de rafraîchissement.** Gain marginal, confort dégradé.

## 5.5 La fin de la charge : prévenir, puis se mettre à l'abri

**Ajouté le 14 septembre 2026, parce que rien ne surveillait la batterie.**
Ni upower, ni démon d'énergie, et le seuil ACPI `alarm` laissé à zéro : le
shell ne lisait la charge que la Console ouverte, donc quand l'utilisateur
regardait déjà. Une machine qui ne sait pas qu'elle va manquer de courant ne
peut ni prévenir ni se mettre à l'abri — et celle-ci en est morte une
centaine de fois dans la seule matinée du 14 (voir `docs/07`).

### Trois seuils, et seul le dernier agit

Réglables dans le panneau Énergie, parce que ce sont des habitudes de travail
et non des constantes physiques : qui reste près d'une prise veut qu'on le
laisse tranquille, qui travaille en déplacement veut être prévenu tôt.

| Seuil | Défaut | Ce qui se passe |
|---|---|---|
| Prévenir | 20 % | Un avis qui disparaît de lui-même |
| Insister | 10 % | Un avis qui reste à l'écran |
| Se mettre à l'abri | 5 % | L'action choisie : hiberner, suspendre, éteindre, ou rien |

Le dernier seuil **demande d'abord à logind s'il sait faire**, et se rabat
sur une extinction propre sinon. Une hibernation impossible rend une erreur
que personne ne lit, et la machine meurt quand même : le pire des deux
mondes, puisqu'on se croyait à l'abri.

### Pourquoi ce module SCRUTE, alors que la règle l'interdit

`energie.c` tient la règle « aucune scrutation » grâce à
`ext-idle-notify-v1` : le compositeur prévient. Pour la batterie, **personne
ne prévient**, et ce n'est pas une supposition — deux voies ont été essayées
le 14 septembre 2026, les deux muettes :

1. **Les uevents du noyau.** Sept minutes d'écoute
   (`udevadm monitor --subsystem-match=power_supply`) pendant une charge
   active : **neuf changements de pourcentage, zéro événement.**
2. **Le seuil matériel.** `/sys/class/power_supply/BAT0/alarm` armé
   au-dessus de la charge courante, donc franchi d'emblée : aucun
   événement, et `capacity_level` immobile sur « Normal ».

Ces deux essais ayant eu lieu **en charge**, ils ont été refaits **en
décharge** le même jour, le pilote pouvant se comporter autrement : quinze
minutes, **quatre changements de pourcentage** (88 → 84 %), **zéro
événement**. La question est close — cette machine ne prévient pas.

Alors on scrute **le moins possible** : l'intervalle se calcule depuis le
temps restant avant le prochain seuil (`charge_now / current_now` — cette
batterie rapporte en charge, pas en énergie : elle n'a ni `energy_now` ni
`power_now`), et l'on se réveille au quart de ce temps. Loin du seuil la
machine dort, près du seuil elle regarde souvent. Sur secteur, une seule
chose peut arriver — qu'on débranche — et elle n'est pas urgente :
intervalle long et fixe.

## 5.6 Le capot, et la veille profonde

### Le capot appartient au panneau, plus à /etc

Il était réglé par `HandleLidSwitch=suspend` dans `logind.conf` : un réglage
système, le même pour les trois modes d'énergie, qu'on ne pouvait pas changer
sans élévation de privilèges. Depuis le 14 septembre 2026, le shell pose un
inhibiteur `handle-lid-switch` en **block** et décide lui-même, comme GNOME
et KDE le font.

**Contrepartie assumée** — la même que pour les notifications et la veille :
si la barre d'état tombe, l'inhibiteur tombe avec elle et logind reprend la
main, donc l'ancien comportement. Le capot ne devient jamais inerte, il
redevient ce qu'il était. C'est aussi pourquoi l'inhibiteur n'est posé
qu'APRÈS avoir réussi à lire le commutateur : prendre la main sans savoir
lire le capot laisserait la machine allumée, repliée, dans un sac.

### Un capot par mode, depuis le 16 septembre 2026

Reprendre la main à logind n'avait levé que la moitié du reproche. Le réglage
restait **unique** : le même pour les trois modes, comme dans `/etc`. En
« Travail » — où la table des modes pose `veille_ordi = FALSE`, où aucune
inactivité ne peut endormir la machine, et dont le résumé promet que
« l'ordinateur ne dort jamais » — rabattre l'écran la suspendait quand même.
Une compilation lancée capot fermé mourait donc dans le mode fait pour la
laisser finir.

Trois clés désormais, une par mode, réglées dans la carte du mode qu'elles
concernent et non plus dans une carte à part :

| Mode | Clé | Défaut | Pourquoi |
|---|---|---|---|
| Travail | `travail_capot` | `verrouiller` | Le mode promet que la machine ne dort pas. Le capot ne fait donc qu'éteindre et verrouiller l'écran ; téléchargements et compilations vont à leur terme. |
| Automatique | `automatique_capot` | `suspendre-hiberner` | Réveil immédiat si l'on revient vite, session sauvée sur le disque si l'on ne revient pas. |
| Nomade | `nomade_capot` | `hiberner` | Le seul état dont la consommation est nulle. |

**Les fichiers écrits avant sont repris.** L'ancienne clé unique
`capot_action` reste *lue* : elle s'applique alors aux deux modes qui
autorisent la veille — c'est en pensant à eux qu'on l'avait choisie — mais
**pas** à « Travail », où la reporter reconduirait le défaut qu'on corrige.
Elle n'est plus écrite : deux vérités dans le même fichier, et c'est la plus
ancienne qui gagnerait à la relecture suivante.

Changer de mode change donc maintenant ce que fait le capot, **à chaud** :
`shell_config_watch` prévient, `shell_capot_reconfigurer` relit. Vérifié le
16 septembre 2026, dans les trois sens — Travail → `verrouiller`,
Automatique → `suspendre-hiberner`, Nomade → `hiberner`.

### La veille profonde MARCHE — ce document disait l'inverse

`docs/07` et `CLAUDE.md` l'ont longtemps déclarée irréalisable, sur la foi
d'un `resume=` absent de la ligne de commande du noyau. C'était mal lu :
**Debian passe par l'initramfs.**

| Ce qu'on croyait | Ce que la machine dit (14 septembre 2026) |
|---|---|
| `resume=` absent → hiberner perdrait la session | `/etc/initramfs-tools/conf.d/resume` porte le bon UUID, `/sys/power/resume` vaut `179:3` |
| hibernation non disponible | `/sys/power/state` contient `disk`, le firmware annonce `S0 S3 S4 S5` |
| à vérifier | `PM: Image not found (code -22)` à chaque démarrage : le chemin de reprise **s'exécute déjà**, il ne trouve rien |
| — | logind répond `CanHibernate` = **yes** |

**Éprouvée le 14 septembre 2026** par la fermeture du capot : la session est
partie sur le disque et revenue intacte.

Reste un point non mesuré : le swap disque fait 3,0 Gio pour 3,8 Gio de RAM,
et le zram de 1,9 Gio **ne compte pas** — ses pages sont en mémoire, donc
dans l'image. L'image a tenu ce jour-là ; la marge sur une machine chargée
n'est pas connue.

### L'ordinateur ne s'endort que sur la batterie

**Décision de l'utilisateur, le 14 septembre 2026**, une fois la veille
profonde éprouvée. La veille progressive garde ses deux premiers étages —
l'écran s'atténue, puis s'éteint — mais **le troisième reste fermé** :
l'ordinateur ne s'endort pas sur l'inactivité. Il s'endort quand la batterie
atteint son seuil de mise à l'abri, et par hibernation.

Le raisonnement : **l'inactivité de l'utilisateur ne dit rien de l'activité
de la machine.** Une compilation, un téléchargement, un transfert vers le NAS
continuent pendant qu'on va faire autre chose ; les interrompre au bout de
quinze minutes serait une nuisance pour un gain nul quand la prise est au
mur. La charge qui s'épuise, elle, est une vraie échéance — et c'est
celle-là qui endort la machine.

En conséquence, `energie.suspendre_permis` reste à `false`, et les durées
« Veille de l'ordinateur après » des trois modes restent enregistrées sans
effet. **Ce n'est pas un oubli** : c'est ce réglage-ci qui les ferme.

### Le verrouillage avant sommeil

Le premier essai d'hibernation a ramené la session **déverrouillée**. Depuis,
le verrou est posé **avant** que la machine ne parte, et non au réveil : au
réveil, l'écran se rallume sur ce qui était affiché. Un inhibiteur `sleep` en
mode `delay` donne le temps de le faire — logind attend, au plus
`InhibitDelayMaxSec`, et le shell ne lui prend que 400 ms.

C'est ancré dans `energie.c` et non dans `capot.c` : le capot n'est qu'une
des façons de s'endormir, et logind émet `PrepareForSleep` pour toutes.

Ce n'est **pas** le réglage « Demander le code PIN au réveil », qui décide du
sursis après l'extinction de l'écran, machine restée là sous les yeux de son
propriétaire. Dormir est autre chose : on ferme, on emporte.

## 5.7 Mesurer plutôt que supposer

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
