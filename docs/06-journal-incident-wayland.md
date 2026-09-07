# Journal d'incident — migration Wayland / écran de connexion

Date : 2026-09-07
Branche : `claude/claude-os-chromebook-nhoec4`

Ce document consigne les faits de la séance de correction de la session
graphique. Il ne remplace pas les journaux de la machine (`journalctl`) : ils
restent la source permettant d'établir la cause technique exacte d'un échec de
démarrage.

## Contexte initial

Le projet vise une session entièrement Wayland : `greetd` lance `labwc`, qui
lance ensuite le greeter et le shell Claude OS. Les restes de la précédente
interface X11 (LightDM, Xorg, Xwayland et les configurations utilisateur) ont
été identifiés comme une source probable de conflit avec cette session.

Une revue statique de la branche a notamment relevé :

- une bascule `greetd`/LightDM fragile si le lien
  `display-manager.service` restait attaché à LightDM ;
- une configuration `~/.config/labwc` qui prime sur `/etc/xdg/labwc` et peut
  donc masquer les fichiers système mis à jour ;
- la présence de composants X11 alors que les programmes du bureau sont prévus
  pour Wayland.

Ces constats étaient des hypothèses de diagnostic ; aucune trace `journalctl`
de la machine de test n'a été collectée pendant cette séance.

## Correctif Wayland publié

Commit : [`caa60fd`](https://github.com/leptitane-fr/Claude-OS/commit/caa60fdb31bcf1023e623e15e58fe51c98ee6a31) — `Force une session Wayland propre`.

Il a :

1. ajouté `install/patch-session-wayland-ssh.sh`, un script autonome exécutable
   par SSH ;
2. retiré `xwayland` de la liste des paquets installés pour les nouvelles
   installations ;
3. ajouté aux nettoyages de `provision.sh` les paquets LightDM/Xorg/Xwayland de
   l'ancienne pile ;
4. forcé les variables de session Wayland pour GTK, Qt et Electron ;
5. supprimé `DISPLAY` dans le lanceur de session Claude OS afin d'empêcher
   l'héritage d'une session X11 ;
6. déplacé, avec sauvegarde, les fichiers connus de l'ancienne session dans
   `/root/claude-os-wayland-backup-<horodatage>` ;
7. désactivé LightDM, activé `greetd`, puis purgé les composants X11 listés.

Le script ne lançait pas de redémarrage automatique : il demandait une
vérification depuis SSH avant `sudo reboot`.

## Résultat rapporté

Après application du correctif, le retour utilisateur a été : **« Tout a
planté. »**

Cet énoncé est le seul symptôme confirmé dans cette séance. En l'absence du
journal de démarrage, il n'est pas possible d'attribuer formellement la panne à
greetd, labwc, au greeter, à une dépendance purgée, ou à un autre composant.

La priorité a donc été le rétablissement d'un écran de connexion connu plutôt
que de poursuivre les essais Wayland sur la machine affectée.

## Retour d'urgence publié

Commit : [`d104f3e`](https://github.com/leptitane-fr/Claude-OS/commit/d104f3e1ae7dcf18b9603e5ef03e978172ec898a) — `Ajoute un retour d'urgence vers LightDM`.

Le script `install/restore-session-x11-ssh.sh` :

1. réinstalle LightDM, Xorg/Xwayland et un bureau Openbox minimal ;
2. cherche la sauvegarde Wayland la plus récente sous `/root/` et restaure les
   configurations qui y figurent ;
3. désactive `greetd`, remet `display-manager.service` sur LightDM et démarre
   `lightdm.service` ;
4. conserve la session SSH et demande un redémarrage explicite à la fin.

Commande prévue sur la machine de test :

```sh
cd ~/Claude-OS && git pull
sudo bash install/restore-session-x11-ssh.sh
sudo reboot
```

---

# La cause, établie

Date : 2026-09-07, seconde séance.
Branche : `claude/examine-project-qgnt80`.

**La purge désinstallait le compositeur.**

## La chaîne de dépendances

Le paquet `labwc` ne porte pas `xwayland` en recommandation, mais en
**dépendance ferme** :

```
labwc      Depends: … xwayland
xwayland   Depends: xserver-common
xserver-common  Depends: x11-common
```

Les deux listes de purge — celle de `provision.sh` et celle du correctif
`patch-session-wayland-ssh.sh` — nommaient `xwayland` **et** `x11-common`.
Chacun des deux noms, à lui seul, suffit à faire retirer `labwc` par apt.

La machine a donc désinstallé son propre compositeur. Au redémarrage, greetd
a démarré normalement et a lancé `labwc -C /etc/xdg/labwc-greeter` — un
binaire qui n'existait plus. Aucun écran de connexion, aucun message, et pas
de touches F sur ce Chromebook pour atteindre une console. « Tout a planté »
décrivait exactement la situation.

## Comment cela a pu passer inaperçu

La commande de purge de `provision.sh` s'écrivait :

```sh
apt-get purge -y $A_PURGER >/dev/null 2>&1 || true
```

Sortie avalée, code de retour ignoré. apt a annoncé qu'il retirait `labwc`,
personne ne l'a lu. C'est la **troisième** fois dans ce projet qu'une sortie
redirigée vers `/dev/null` coûte une soirée — après Chromium retiré par un
`autoremove` silencieux, et après l'activation de greetd qui échouait sans un
mot. Le motif est désormais traité comme un défaut en soi.

## Vérification

La cascade a été **reproduite en simulation**, `labwc` installé :

```
$ apt-get -s purge -y lightdm xserver-xorg-core x11-common xwayland openbox
Purg labwc
Purg xwayland
…
```

Avec la liste corrigée — les trois noms X11 serveur retirés — la même
simulation ne touche plus rien de vital. `xserver-xorg-core`, en revanche, a
été mesuré **sans danger** : rien de la pile Wayland n'en dépend, et il
emporte l'essentiel du serveur X en partant. Il reste donc dans la liste.

> **Portée de la mesure.** Elle a été faite sur un conteneur Ubuntu 24.04
> (labwc 0.7.1), le miroir Debian n'étant pas joignable depuis
> l'environnement de travail. La dépendance `labwc → xwayland` vient de
> l'empaquetage Debian dont Ubuntu hérite, mais elle n'a pas été relue sur
> trixie. Sur la machine, une ligne tranche :
>
> ```sh
> apt-cache show labwc | grep '^Depends'
> ```
>
> `bascule-session.sh --verifier` contrôle de toute façon la seule chose qui
> compte à l'usage : que `labwc` et `xwayland` soient là.

## Conséquence sur l'état de la machine

`restore-session-x11-ssh.sh` réinstalle LightDM, Xorg **et** `xwayland` — mais
**pas `labwc`**. Une machine passée par le retour d'urgence a donc un écran de
connexion, et toujours aucun compositeur Wayland. `provision.sh` le réinstalle,
puisque `labwc` figure dans `packages.list`.

---

# Ce qui a changé

| Quoi | Où |
|---|---|
| `xwayland` et `x11-common` retirés des listes de purge | `install/provision.sh` |
| `xwayland` déclaré explicitement, avec la raison | `install/packages.list` |
| La purge n'est plus silencieuse, et son échec est signalé | `install/provision.sh` |
| Garde-fou : apt est interrogé **avant** de purger, et la purge est abandonnée si un composant vital figure dans la cascade | `install/provision.sh` |
| Contrôle final : `provision.sh` refuse de rendre la main sans compositeur ni écran de connexion | `install/provision.sh` |
| Bascule par étapes, dont un essai du greeter sans rien activer | `install/bascule-session.sh` |
| Filet de sécurité : la machine constate seule qu'on peut entrer, et se rétablit sinon | `rootfs/usr/local/lib/claude-os/filet-session` |
| `patch-session-wayland-ssh.sh` **supprimé** | — |

Le correctif de septembre a été retiré du dépôt plutôt que corrigé. Il
faisait tout d'un bloc — déployer, purger, activer — et c'est cette forme,
autant que la liste de paquets, qui a produit la panne. `bascule-session.sh`
le remplace en séparant ce qui est réversible de ce qui ne l'est pas.

## Le filet de sécurité

C'est la réponse au vrai problème, qui n'était pas la liste de paquets mais
**l'impossibilité de se rétablir seul**. Une purge fautive sera peut-être
réintroduite un jour ; se retrouver enfermé dehors ne doit plus arriver.

Armé par `provision.sh` et par `bascule-session.sh --basculer`, il s'exécute
quatre minutes après le démarrage et se pose une seule question : **le champ
de mot de passe est-il affiché, ou une session est-elle ouverte ?**

- Oui → il se désarme et s'efface. Le risque est passé.
- Non → il écrit dans `/var/log/claude-os-echec-<date>.txt` l'état des
  binaires, des paquets et les journaux de greetd et labwc — **avant** de
  toucher à quoi que ce soit — puis désactive greetd et redémarre sur un
  écran où l'on peut entrer.

Les deux chemins ont été essayés. Sur le chemin d'échec, le rapport produit
nomme le composant manquant en clair :

```
--- le compositeur et l'écran de connexion sont-ils là ? ---
  /usr/bin/labwc : présent
  /usr/sbin/greetd : ABSENT  <<<<
--- paquets ---
  labwc        installed 0.7.1-1build1
  greetd       NON INSTALLÉ
```

C'est précisément le renseignement qui a manqué pendant une semaine.

## Ce que le greeter, lui, n'avait pas

Deux hypothèses de la première séance ont été **écartées par la mesure**, en
lançant le vrai binaire sous un labwc sans écran :

- **le bus de session D-Bus.** `claude-os-connexion` s'affiche sans lui ; il
  se contente d'un avertissement. Ce n'était pas la cause ;
- **la chaîne `greetd → labwc → autostart → greeter`.** Elle a été exécutée
  telle quelle : le greeter démarre, s'affiche, et l'horloge est en français.

Le code de l'écran de connexion n'était pas en cause, et n'a pas été modifié.

## La procédure, désormais

```sh
cd ~/Claude-OS && git pull
sudo bash install/provision.sh              # réinstalle labwc, arme le filet
sudo bash install/bascule-session.sh --verifier
sudo bash install/bascule-session.sh --essai      # regarder l'écran
sudo bash install/bascule-session.sh --basculer
sudo systemctl reboot
```

`--essai` affiche le véritable écran de connexion sur un terminal virtuel
libre pendant trente secondes, sans rien activer, sans rien purger et sans
toucher à la session en cours. C'est la validation progressive que la première
séance recommandait sans avoir les moyens de la faire.

---

# L'écran de connexion fonctionne sur la machine

Date : 2026-09-07, troisième séance.

Mesuré sur le matériel, par `bascule-session.sh --essai`, sans redémarrage et
sans bascule du gestionnaire de session :

```
  ✓ le compositeur tourne                      pid 4147
  ✓ le greeter tourne                          pid 4183
```

Le champ de mot de passe s'affiche. La chaîne `greetd → labwc → autostart →
claude-os-greeter → claude-os-connexion` est bonne de bout en bout sur MADOO.

## Ce qui a débloqué

`--deployer`, ajouté à cette séance. **`git pull` met à jour le dépôt, pas la
machine** : `rootfs/` n'est recopié vers `/` que par `provision.sh`. Trois
essais ont donc porté sur l'ancien `/usr/local/bin/claude-os-greeter` pendant
que le correctif dormait dans le dépôt, sans que rien ne le signale — le
dépôt était à jour, et c'est ce qu'on regardait.

`--deployer` fait cette recopie seule, sans réinstaller les paquets ni
recompiler le shell, et annonce la branche et le commit qu'il déploie.

## Un symptôme fabriqué par l'outil de diagnostic

L'essai laissait derrière lui un compositeur orphelin, et il faut le dire
parce que cela a coûté des allers-retours.

`pam_systemd` ouvre une session logind pour `_greetd` ; systemd déplace alors
labwc et le greeter dans un `session-NN.scope` qui leur est propre, **hors du
cgroup du service d'essai**. Arrêter le service tuait greetd et laissait ses
enfants tourner. Ce qui restait était un compositeur sans greeter, gardant le
terminal virtuel : **fond noir et pointeur de souris** — c'est-à-dire
exactement le symptôme que l'on cherchait à diagnostiquer.

Le ménage termine désormais la session logind, ce qui emporte le scope entier.
Le filtre porte sur le numéro de terminal virtuel de l'essai, pour ne jamais
toucher à un écran de connexion en service.

## Ce qui n'est pas établi

Pourquoi le greeter ne démarrait pas lors des premiers essais n'a pas été
tranché, et il ne faut pas le présenter autrement. Deux explications tiennent :
le `claude-os-greeter` réellement présent sur la machine différait de celui du
dépôt, ou un compositeur orphelin d'un essai précédent tenait déjà le terminal
virtuel. Aucune des deux n'a été prouvée.

Ce qui a changé, en revanche, c'est qu'un tel échec ne serait plus muet : le
greeter écrit maintenant son contexte, sa sortie et son code de retour dans le
premier emplacement où l'écriture réussit vraiment, et `--essai` verse son
autopsie complète dans `/var/log/claude-os-essai-<date>.txt`.

## Trois contrôles ajoutés en route

| Contrôle | Ce qu'il attrape |
|---|---|
| `ldd` sur les six binaires | Une bibliothèque manquante tue le programme avant sa première ligne : aucun processus, rien à l'écran, aucun message. Symptôme : fond noir et pointeur. |
| `libgtk4-layer-shell0` | Sans lui, aucun composant ne peut s'ancrer à l'écran. |
| Branche et commit affichés par `provision.sh` | Ce dépôt porte trois branches `claude/…` et aucune branche par défaut ; `git pull` sur la mauvaise répond « Déjà à jour ». `provision.sh` refuse en outre de tourner s'il est antérieur au correctif de la purge. |
