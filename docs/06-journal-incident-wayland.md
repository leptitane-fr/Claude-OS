# 6. Incident de la session graphique — post-mortem

**Du 7 septembre 2026. Résolu.** L'écran de connexion Claude OS s'affiche,
le mot de passe est accepté, la session labwc s'ouvre avec le dock, la barre
d'état et le lanceur.

Ce document remplace le journal tenu au fil des séances. Il est réécrit à
froid, une fois les causes établies, parce qu'un empilement chronologique de
cinq séances de tâtonnement ne se relit pas.

---

## Résumé

Deux pannes distinctes, et un défaut de méthode qui a coûté plus que les deux
réunies.

| # | Symptôme | Cause établie | Correctif |
|---|---|---|---|
| 1 | Écran noir au démarrage, aucune connexion possible, SSH seul recours | La purge de l'ancienne pile X11 nommait `xwayland` et `x11-common`. `labwc` porte `Depends: xwayland` : apt a désinstallé **le compositeur**. greetd démarrait sur un binaire absent. | Les deux noms retirés des listes de purge ; `xwayland` déclaré dans `packages.list` ; garde-fou qui interroge apt avant de purger. |
| 2 | Écran de connexion parfait, mot de passe accepté, mais la session meurt en une seconde — en boucle | `/tmp/.X11-unix` appartenait à `_greetd` (uid 102), qui l'avait créé. labwc démarre Xwayland à l'ouverture et **traite son échec comme fatal** ; Xwayland refuse le répertoire s'il n'appartient ni à root ni à l'utilisateur courant. | Règle `tmpfiles` : `d /tmp/.X11-unix 1777 root root -`, appliquée aussi sur-le-champ par `provision.sh` et `--deployer`. |
| 3 | Trois séances de diagnostic sur des correctifs absents de la machine | `git pull` met à jour le dépôt, pas `/`. `rootfs/` n'est déployé que par `provision.sh`. | `bascule-session.sh --deployer`, qui annonce la branche et le commit déployés. |

C'est **le même paquet `xwayland`** qui a causé les deux pannes : en le
purgeant on désinstallait labwc, en le gardant on héritait d'un répertoire
mal possédé. Il n'est jamais exécuté par le bureau — Chromium, Claude Desktop
et le shell parlent Wayland nativement. Il aura coûté deux soirées.

---

## Chronologie

**Séance 1 — la panne.** Un correctif de migration Wayland est appliqué d'un
bloc : déployer, purger, activer. Retour utilisateur : « Tout a planté. »
Aucun journal n'est collecté avant le retour d'urgence vers LightDM, et la
cause reste inconnue une semaine. Le script fautif
(`patch-session-wayland-ssh.sh`) et son chemin de retour
(`restore-session-x11-ssh.sh`) sont publiés.

**Séance 2 — la cause n°1.** La cascade de dépendances est reproduite en
simulation, `labwc` installé. Le correctif de la purge est écrit, avec un
garde-fou, un filet de sécurité et une bascule par étapes. Deux hypothèses de
la séance 1 sont **écartées par la mesure** : le bus de session D-Bus (le
greeter s'affiche sans lui) et la chaîne `greetd → labwc → autostart →
greeter` (exécutée telle quelle sous un labwc sans écran, elle fonctionne).

**Séance 3 — les correctifs n'arrivaient pas.** La machine tournait sur une
autre branche ; `git pull` répondait « Déjà à jour » en toute honnêteté.
L'ancien `provision.sh` est relancé et désinstalle labwc une troisième fois.
`provision.sh` annonce désormais sa branche et son commit, et refuse de
tourner s'il est antérieur au correctif.

**Séance 4 — l'écran de connexion fonctionne.** `--deployer` déploie enfin le
greeter corrigé. `--essai` mesure sur le matériel, sans redémarrage :
`✓ le compositeur tourne (pid 4147)`, `✓ le greeter tourne (pid 4183)`.

**Séance 5 — la cause n°2.** La bascule est faite, la machine redémarre,
l'écran de connexion apparaît, le mot de passe est accepté — et la session
boucle. Le journal de session, ajouté au tour précédent, désigne la cause au
premier essai.

---

## Cause n°1 — la purge désinstallait le compositeur

```
labwc           Depends: … xwayland
xwayland        Depends: xserver-common
xserver-common  Depends: x11-common
```

Les deux listes de purge nommaient `xwayland` **et** `x11-common`. Chacun des
deux, à lui seul, suffit à faire retirer `labwc` par apt. Reproduit :

```
$ apt-get -s purge -y lightdm xserver-xorg-core x11-common xwayland openbox
Purg labwc
Purg xwayland
```

`xserver-xorg-core`, en revanche, est **sans danger** : rien de la pile
Wayland n'en dépend, et il emporte l'essentiel du serveur X en partant. Il
reste dans la liste.

> **Portée de la mesure.** Faite sur un conteneur Ubuntu 24.04 (labwc 0.7.1),
> le miroir Debian n'étant pas joignable depuis l'environnement de travail.
> Confirmé depuis sur la machine : labwc 0.8.3 de trixie porte la même
> dépendance. `--verifier` contrôle de toute façon la seule chose qui compte :
> que `labwc` et `xwayland` soient là.

**Comment cela a pu passer inaperçu.** La commande s'écrivait :

```sh
apt-get purge -y $A_PURGER >/dev/null 2>&1 || true
```

apt a annoncé qu'il retirait `labwc`, personne ne l'a lu. Troisième fois dans
ce projet qu'une sortie envoyée dans `/dev/null` coûte une soirée — après
Chromium emporté par un `autoremove` silencieux, et après une activation de
greetd qui échouait sans un mot.

**Une conséquence à connaître.** `restore-session-x11-ssh.sh` réinstallait
LightDM, Xorg et `xwayland` — mais **pas `labwc`**. Une machine passée par le
retour d'urgence avait donc un écran de connexion et toujours aucun
compositeur Wayland. Ce script a été retiré du dépôt : il réinstallait
plusieurs centaines de mégaoctets d'une pile X11 que l'architecture rejette,
et `--revenir` plus le filet couvrent désormais le besoin.

---

## Cause n°2 — `/tmp/.X11-unix`

Le journal de session, mot pour mot :

```
[ERROR] [xwayland/sockets.c:100] /tmp/.X11-unix not owned by root or us
[ERROR] [xwayland/sockets.c:217] No display available in the first 33
[ERROR] [../src/xwayland.c:1117] cannot create xwayland server
=== labwc s'est arrêté — code de retour 1 ===
```

labwc est compilé avec Xwayland — Debian le lui impose — et le démarre à
l'ouverture. Xwayland refuse de créer sa socket si `/tmp/.X11-unix`
n'appartient ni à root ni à l'utilisateur courant, et **labwc traite cet
échec comme fatal**.

L'écran de connexion tourne sous `_greetd` (uid 102). C'est lui qui créait le
répertoire en premier, et il en devenait propriétaire. La session de `stef`,
ouverte ensuite, ne pouvait plus s'en servir.

**L'asymétrie est ce qui rendait la panne déroutante.** Le greeter
fonctionnait parfaitement — il possédait le répertoire. La session seule
échouait, et greetd la remplaçait aussitôt par un nouvel écran de connexion,
d'où la boucle.

Trois hypothèses ont été écartées avant celle-ci, et toutes annoncées comme
des suspects, jamais comme des causes :

- `~/.config/labwc`, qui prime sur `/etc/xdg/labwc` pour la session mais pas
  pour le greeter (lancé avec `-C`) — **absent** sur la machine ;
- les actions de `rc.xml` — `Execute`, `Exit`, `ToggleFullscreen`, toutes
  connues de labwc 0.8.4, vérifié dans la source ;
- le siège et la carte graphique — `seat0`, VT 1, i915 chargé,
  `[MASTER] drm:card0`, `/dev/dri` peuplé.

Le correctif est `rootfs/etc/tmpfiles.d/claude-os-x11.conf` :

```
d /tmp/.X11-unix 1777 root root -
```

Créé par root avec le bit collant, le répertoire sert aux deux comptes.
`/etc/tmpfiles.d` prime sur `/usr/lib/tmpfiles.d` : la règle tient même si le
paquet qui la fournissait d'ordinaire a été retiré — dans ce projet, ce n'est
pas une hypothèse d'école. `tmpfiles` ne s'exécutant qu'au démarrage,
`provision.sh` et `--deployer` appliquent aussi la correction sur-le-champ.

---

## Ce qui a fait perdre le plus de temps

Ni l'une ni l'autre des deux causes. **Le fait que les pannes soient muettes.**

`claude-os-greeter` essayait d'ouvrir `/var/log/claude-os-connexion.log` et
continuait sans redirection en cas d'échec. Sous greetd il tourne en
`_greetd`, qui n'écrit pas dans `/var/log` : la redirection échouait donc
*toujours* en service réel. Le seul cas où ce journal servait était
précisément celui où il ne s'écrivait pas.

`claude-os-session` ne conservait rien du tout. Lancée par greetd, elle écrit
sur le terminal virtuel ; greetd réaffiche l'écran de connexion par-dessus en
une seconde. L'utilisateur voyait « de nombreuses lignes rouges » défiler,
illisibles — c'était la sortie d'erreur de wlroots, qui colore ses erreurs en
rouge — et elle n'atteignait ni le journal systemd ni aucun fichier.

Les deux consignent maintenant leur contexte, leur sortie complète et leur
**code de retour**. La cause n°2 a été identifiée au premier essai qui a
suivi.

### Trois faux négatifs, trouvés avant livraison

Un outil de diagnostic qui ment coûte plus qu'il ne rapporte : il fait
chercher au mauvais endroit, avec autorité.

- `pgrep -x` compare au nom court du processus, **tronqué à quinze
  caractères** par le noyau. `claude-os-status` en fait seize,
  `claude-os-connexion` dix-neuf : `diag-session.sh` les déclarait absents
  alors qu'ils tournaient.
- Chercher `pam_systemd` dans le seul `/etc/pam.d/greetd-greeter` le
  déclarait absent : Debian y écrit `@include login`, et il est tiré trois
  niveaux plus loin par `common-session`. Le test suit désormais les
  inclusions.
- `ls répertoire | tr` rend **toujours** 0 — le code de retour est celui de
  `tr`. Le `|| echo ABSENT` accroché à ce tuyau ne se déclenchait jamais, et
  un répertoire manquant s'affichait comme une ligne vide.

### L'essai fabriquait le symptôme qu'il cherchait

`pam_systemd` ouvre une session logind pour `_greetd` ; systemd déplace alors
labwc et le greeter dans un `session-NN.scope` qui leur est propre, **hors du
cgroup du service d'essai**. `systemctl stop` tuait greetd et laissait ses
enfants tourner.

Ce qui restait était un compositeur sans greeter, gardant le terminal
virtuel : **fond noir et pointeur de souris** — c'est-à-dire exactement la
panne que l'on cherchait. Le ménage termine désormais la session logind, ce
qui emporte le scope entier.

---

## Ce qui n'est pas établi

Pourquoi le greeter ne démarrait pas lors des tout premiers essais de la
séance 4 n'a **jamais été tranché**. Deux explications tiennent — le
`claude-os-greeter` réellement installé différait de celui du dépôt, ou un
compositeur orphelin d'un essai précédent tenait déjà le terminal virtuel —
et aucune n'a été prouvée.

C'est écrit ici plutôt que résolu en choisissant la plus flatteuse. Ce qui a
changé, c'est qu'un tel échec ne serait plus muet.

---

## Ce qui a changé

| Quoi | Où |
|---|---|
| `xwayland` et `x11-common` retirés des purges ; `xwayland` déclaré avec sa raison | `install/provision.sh`, `install/packages.list` |
| Garde-fou : apt est interrogé **avant** de purger, la purge est abandonnée si un composant vital figure dans la cascade | `install/provision.sh` |
| La purge n'est plus silencieuse ; `provision.sh` refuse de rendre la main sans compositeur ni greetd | `install/provision.sh` |
| Branche et commit affichés ; refus de tourner si antérieur au correctif de la purge | `install/provision.sh` |
| `/tmp/.X11-unix` à root, au démarrage et sur-le-champ | `rootfs/etc/tmpfiles.d/claude-os-x11.conf` |
| `libpam-systemd` et `polkitd` déclarés — deux recommandations que `--no-install-recommends` écartait | `install/packages.list` |
| Bascule par étapes : `--verifier`, `--deployer`, `--essai`, `--basculer`, `--revenir` | `install/bascule-session.sh` |
| Filet de sécurité : la machine constate seule qu'on peut entrer, écrit pourquoi sinon, et se rétablit | `rootfs/usr/local/lib/claude-os/filet-session` |
| Greeter et session consignent contexte, sortie et code de retour | `rootfs/usr/local/bin/claude-os-{greeter,session}` |
| Diagnostic du cas « pas d'écran de connexion » et de la session qui boucle | `tools/diag-connexion.sh` |
| `patch-session-wayland-ssh.sh` et `restore-session-x11-ssh.sh` **supprimés** | — |

Les deux scripts de la séance 1 ont été retirés plutôt que corrigés. Le
premier faisait tout d'un bloc, et c'est cette forme autant que sa liste de
paquets qui a produit la panne. Le second réinstallait une pile X11 que
l'architecture rejette, sans remettre le compositeur. `bascule-session.sh`
les remplace en séparant ce qui est réversible de ce qui ne l'est pas.
