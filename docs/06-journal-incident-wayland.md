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

## État et suite recommandée

La branche contient maintenant à la fois le correctif Wayland et son chemin de
retour. Le correctif Wayland ne doit pas être réessayé sans collecter d'abord
les éléments suivants après l'échec :

```sh
journalctl -b -u greetd --no-pager
journalctl -b _COMM=labwc --no-pager
journalctl -b --priority=err..alert --no-pager
cat /var/log/claude-os-connexion.log 2>/dev/null
```

La prochaine itération devra tester le greeter et la session labwc sans purge
de LightDM/Xorg, valider leur démarrage, puis seulement retirer les composants
X11. Cette validation progressive évite qu'un incident de connexion masque sa
propre cause.
