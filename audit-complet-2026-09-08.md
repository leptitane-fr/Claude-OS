# Audit complet — Claude OS

**Machine auditée :** `Claude-OS` — HP Chromebook x360 14b-cb0000sf, board `MADOO`, Pentium Silver N6000, 3,7 Gio de RAM utilisable
**Firmware :** MrChromebox-2606.1 (UEFI), Secure Boot désactivé
**Système :** Debian GNU/Linux 13.6 « trixie », noyau 6.12.107+deb13-amd64
**Dépôt :** `/home/stef/Claude-OS`, distant `github.com/leptitane-fr/Claude-OS`
**Branche sur la machine :** `claude/examine-project-qgnt80` (commit `2ce21dc`)
**Date de l'audit :** 8 septembre 2026
**Portée :** disque entier, en lecture. Seule exception, à la demande de l'utilisateur :
le test de la porte `claude-os-root` (§9) a installé le paquet `tree` (59 ko) pour
éprouver le niveau 2. Rien d'autre n'a été modifié.

---

## 1. Résumé exécutif

Le projet est **d'une qualité d'ingénierie nettement au-dessus de la moyenne** sur ce
qu'il a effectivement traité : les scripts sont commentés au *pourquoi*, les pannes ont
été instrumentées plutôt que devinées, le shell en C passe par D-Bus et `argv` plutôt
que par des appels shell concaténés — je n'ai trouvé **aucune injection de commande**
dans les 10 121 lignes de C. Le déploiement par étapes, le filet de sécurité et la
journalisation du greeter et de la session sont de bonnes réponses à de vrais incidents.

Trois problèmes dominent néanmoins, et deux d'entre eux touchent le cœur de ce que le
projet dit être.

1. **Le déploiement a rendu `stef` propriétaire de `/`, `/etc`, `/usr` et de
   55 fichiers système, tous inscriptibles par lui.** Parmi eux : une unité systemd
   exécutée en root, le script qu'elle lance, un fichier de règles udev et la
   configuration noyau de GRUB. N'importe quel processus tournant sous `stef` — Claude
   Desktop, Chromium, un agent — obtient root **sans mot de passe et sans passer par
   `sudo`**. C'est une régression silencieuse, produite par `cp -a rootfs/. /`, et
   qu'aucun des contrôles du projet ne voit.

2. **Le modèle de privilèges décrit dans `docs/02` n'existe pas sur la machine.** Pas de
   courtier `claude-osd`, pas de `/etc/claude-os/policy.d/`, pas de `claude-os rollback`,
   pas de journal d'audit — et **pas de btrfs non plus** : la racine est en **ext4**,
   donc aucun snapshot n'est possible. La troisième contrainte structurante du projet
   (« *pleins pouvoirs* doit rester réversible ») n'est aujourd'hui satisfaite **ni pour
   la réversibilité, ni pour la traçabilité**. Le `README` la présente pourtant comme une
   « décision actée ».

3. **L'invariant n°4 — « aucune sortie de commande n'est envoyée dans `/dev/null` » — est
   enfreint par le projet lui-même, précisément aux endroits où il a coûté le plus cher.**
   Trois exemples mesurés : `meson install >/dev/null`, la création du profil Wi-Fi
   NetworkManager (`>/dev/null 2>&1 || true`) exécutée **juste avant** de neutraliser
   `ifupdown`, et les commandes de repli du filet de sécurité lui-même.

Sur le chantier n°1 (l'audio), **la cause est établie** : le firmware MrChromebox ne
publie pas la table ACPI **NHLT**, sans laquelle SOF ne peut rien configurer. Voir §6.

Une **porte instrumentée `claude-os-root` a été installée le 8 septembre à 12 h 12**,
après le corps de cet audit. Elle répond correctement à **C2** sur la traçabilité, mais
**C1 la désarme entièrement** : `/usr/local/bin` étant inscriptible par `stef`, le
guichet lui-même est remplaçable sans mot de passe. Voir §9.

**Verdict global :** système fonctionnel et bien documenté, **posture de sécurité
faible et en contradiction avec ses propres objectifs affichés**. **C1 est bloquant** :
tant qu'il tient, aucun mécanisme d'autorisation posé par-dessus ne peut valoir, y
compris celui installé depuis. Il doit être corrigé avant tout autre travail.

---

## 2. Méthode

Tout ce qui suit a été **mesuré sur la machine**, jamais déduit de la documentation. Les
commandes de vérification sont données avec chaque constat pour que le résultat soit
reproductible. Là où je n'ai pas pu mesurer (journal noyau inaccessible au compte
`stef`), je le dis explicitement plutôt que de conclure.

Les fichiers du dépôt ont été comparés un à un à leur homologue déployé :
**`rootfs/` et `/` sont identiques** — aucun écart de contenu. L'écart est ailleurs :
dans les propriétaires, et dans ce que `rootfs/` ne contient pas.

---

## 3. Inventaire mesuré

| Poste | Valeur constatée |
|---|---|
| Disque | eMMC 58,2 Go — `p1` EFI 976 Mo, `p2` **ext4** 54,3 Go, `p3` swap 3 Go |
| Occupation | 5,5 Go / 54 Go (11 %) — `/usr` 3,2 Go, `/var` 1,3 Go, `/home` 976 Mo |
| Mémoire | 3,7 Gio ; zram0 1,9 Gio (zstd, prio 100) + swap disque 3 Go (prio −2) ✅ |
| Paquets | 871 |
| Chiffrement | **aucun** (pas de LUKS) ; TPM 2.0 présent (`/dev/tpm0`) mais inutilisé |
| Secure Boot | désactivé |
| Session | greetd 0.10.3 sur **tty7** ✅, labwc, `display-manager → greetd.service` |
| Shell Claude OS | 7 binaires dans `/usr/bin`, root:root, datés du 8 sept. 10:12 ✅ |
| Réseau | NetworkManager ; SSH en écoute sur `0.0.0.0:22` ; **aucun pare-feu** |
| Mises à jour | 0 en attente ✅ ; **pas d'`unattended-upgrades`** |
| Microcode | `intel-microcode 3.20251111.1~deb13u1` ✅ |
| Audio | **aucune carte son** — voir §6 |
| LSM actifs | `lockdown, capability, landlock, yama, apparmor, …` — **aucun profil AppArmor** |
| Services en échec | 0 ✅ |

Empreinte mémoire réelle du bureau au repos (PSS, hors Claude Desktop) :

| Processus | PSS |
|---|---|
| `claude-os-dock` | 57,2 Mo |
| `claude-os-status` | 57,0 Mo |
| `claude-os-fond` | 40,3 Mo |
| **Sous-total shell** | **154,5 Mo** |
| `labwc` | 46,8 Mo |
| `xdg-desktop-portal` (×2) | 24,8 Mo |

---

## 4. Constats

### 🔴 CRITIQUE

---

#### C1 — `stef` est propriétaire de `/`, `/etc`, `/usr` et peut obtenir root sans mot de passe

**Constat**

```
drwxrwxr-x stef:stef /
drwxrwxr-x stef:stef /etc
drwxrwxr-x stef:stef /usr
drwxrwxr-x stef:stef /etc/systemd/system
-rw-rw-r-- stef:stef /etc/systemd/system/claude-os-filet.service
-rwxrwxr-x stef:stef /usr/local/lib/claude-os/filet-session
-rw-rw-r-- stef:stef /etc/udev/rules.d/90-claude-os-retroeclairage.rules
-rw-rw-r-- stef:stef /etc/default/grub.d/99-claude-os.cfg
-rwxrwxr-x stef:stef /usr/local/bin/claude-os-greeter
```

**55 fichiers et répertoires système** appartiennent à `stef` et sont inscriptibles par
lui (`find / -xdev -user stef` hors `/home`, `/proc`, `/sys`, `/tmp`, `/run`).

**Cause racine — deux mécanismes qui se combinent**

1. `install/bascule-session.sh:496` et `install/provision.sh:420` font
   `cp -a "$DEPOT/rootfs/." /`. **`cp -a` préserve le propriétaire et le mode de la
   source**, et applique aussi ces attributs aux répertoires de destination existants.
   Le dépôt étant cloné sous `stef`, chaque fichier déployé arrive en `stef:stef` — et
   `/`, `/etc`, `/usr`, `/etc/systemd/system` sont chownés au passage.
2. L'umask effectif est **`0002`** (groupes privés d'utilisateur, `USERGROUPS_ENAB yes`),
   ce qui donne des modes `664`/`775` : les fichiers ne sont pas seulement possédés par
   `stef`, ils lui sont **inscriptibles**.

Ce n'est pas un accident local : la procédure d'installation du `README` (cloner en tant
qu'utilisateur, puis `sudo bash install/provision.sh`) **reproduit le problème à
l'identique** sur toute machine neuve.

**Impact**

Quatre chemins d'escalade immédiats, tous sans mot de passe :

| Chemin | Mécanisme |
|---|---|
| `/etc/systemd/system/claude-os-filet.service` + `/usr/local/lib/claude-os/filet-session` | l'unité s'exécute **en root** ; les deux fichiers sont inscriptibles par `stef` |
| `/etc/udev/rules.d/` | les règles `RUN+=` s'exécutent en root depuis `systemd-udevd` |
| `/etc/default/grub.d/99-claude-os.cfg` | contrôle de la ligne de commande noyau au prochain `update-grub` |
| `/usr/local/bin/claude-os-greeter` | exécuté sous `_greetd` — franchissement `stef` → `_greetd` |

Objection anticipée : « `stef` est déjà dans le groupe `sudo` ». C'est vrai, mais `sudo`
**exige un mot de passe** (vérifié : `sudo -n` refuse). La frontière franchie ici est
celle-là. Concrètement, sur une machine dont le propos est de faire tourner un agent aux
privilèges étendus, cela signifie qu'**une application Electron compromise, une extension
Chromium ou un agent qui déraille obtient root en écrivant un fichier**, sans
interaction humaine et sans laisser de trace dans le journal `sudo`.

**Aggravant n°1 — C1 désarme la porte `claude-os-root`.** Le guichet installé le
8 septembre à 12 h 12 est bien en `root:root 0755`, mais il vit dans
`/usr/local/bin`, qui est `drwxrwxr-x stef:stef`. **Le droit d'écriture sur un
répertoire autorise à renommer et à supprimer ce qu'il contient, quel qu'en soit le
propriétaire** — il n'y a pas de bit collant ici. `stef` peut donc substituer son
propre programme au guichet, sur le chemin exact que `sudoers` autorise en `NOPASSWD`.
La confirmation humaine du niveau 3 devient contournable **sans jamais toucher au
guichet lui-même ni à `/etc/sudoers.d`**. Le même raisonnement vaut pour `/etc`, qui est
`stef:stef` : ses sous-répertoires `claude-os/` et `sudoers.d/` sont en `root:root`,
mais **le répertoire qui les contient ne l'est pas**.

*Vérifié en lecture seule, non exploité — `CLAUDE.md` interdit de contourner le
niveau 3, et les bits de permission suffisent à établir le fait.*

**Aggravant n°2** : `install/bascule-session.sh --verifier` contrôle les propriétaires **d'un
seul chemin**, `/tmp/.X11-unix` (ligne 334). Il ne vérifie jamais les propriétaires ni les
modes de ce qu'il vient lui-même de déployer. La régression est donc invisible à l'outil
censé la détecter.

**Correctif proposé**

Remplacer la copie préservant les attributs par une copie qui les impose. Dans
`provision.sh` et `bascule-session.sh` :

```sh
# Remplacer :  cp -a "$DEPOT/rootfs/." /
# Par :
cp -a --no-preserve=ownership "$DEPOT/rootfs/." /
find "$DEPOT/rootfs" -mindepth 1 -printf '/%P\0' \
  | xargs -0 chown root:root
find "$DEPOT/rootfs" -mindepth 1 -type d -printf '/%P\0' | xargs -0 chmod 755
find "$DEPOT/rootfs" -mindepth 1 -type f -printf '/%P\0' | xargs -0 chmod 644
# puis les exécutables, comme aujourd'hui (lignes 421-423 de provision.sh)
```

Poser `umask 022` en tête des deux scripts.

Réparation de la machine actuelle :

```sh
sudo chown root:root / /etc /usr
sudo find / -xdev \( -path /home -o -path /tmp -o -path /run -o -path /proc -o -path /sys \) -prune \
     -o -user stef -exec chown root:root {} +
sudo chmod 755 / /etc /usr
sudo find /etc /usr/local /usr/share/claude-os -perm -g+w -exec chmod g-w {} +
```

Ajouter enfin à `--verifier` un contrôle explicite :

```sh
MAUVAIS="$(find /etc /usr/local /usr/share/claude-os ! -user root -print -quit)"
[ -z "$MAUVAIS" ] || ko "propriétaire" "des fichiers système n'appartiennent pas à root ($MAUVAIS)"
```

---

#### C2 — Le modèle de privilèges et la réversibilité n'existent pas

**Constat**

`docs/02-architecture.md` §« Architecture proposée » décrit un courtier `claude-osd` en
root, un serveur MCP `claude-os-mcp`, une politique `/etc/claude-os/policy.d/*.toml`,
trois niveaux d'autorisation, un snapshot btrfs avant chaque écriture, `claude-os
rollback <id>` et un journal d'audit corrélé.

Sur la machine :

```
$ command -v claude-osd claude-os-mcp claude-os   → aucun
$ ls /etc/claude-os/policy.d                      → n'existe pas
$ findmnt -no FSTYPE /                            → ext4
$ grep -rn btrfs install/ tools/                  → aucune occurrence
```

`/etc/claude-os/` ne contient que `utilisateur` (5 octets). **Aucune ligne de code du
dépôt ne crée de sous-volume btrfs, de snapshot ou de journal d'audit.**

**Impact**

La contrainte n°3 du `README` — « *Un agent capable de tout modifier sur le système n'est
utilisable au quotidien que si chaque action privilégiée est tracée et annulable* » —
n'est satisfaite sur **aucun de ses deux termes**. Aujourd'hui, une action privilégiée
lancée depuis Claude Desktop n'est ni annulable, ni distinguable dans un journal.
Combiné à **C1**, elle n'a même pas besoin de demander.

Ce n'est pas seulement une fonctionnalité manquante : le `README` la présente dans le
tableau « **Décisions actées** », au même rang que le firmware et la distribution, qui,
eux, sont réellement en place. Un lecteur — humain ou agent — en conclut légitimement
que le filet existe.

**Correctif proposé**

Deux voies, à trancher explicitement :

- **Voie honnête, immédiate (0 ligne de code).** Déplacer btrfs, le courtier et le
  rollback du tableau « Décisions actées » vers une section « Non implémenté », et le
  dire dans `CLAUDE.md`. C'est le minimum, et cela devrait être fait aujourd'hui.
- **Voie technique.** btrfs ne se rétrofite pas sur une racine ext4 sans réinstallation.
  Une réversibilité utile est cependant atteignable sans changer de système de fichiers :
  `etckeeper` (dépôt git sur `/etc`, commit automatique à chaque transaction apt) donne
  la traçabilité et l'annulation sur la partie qui compte le plus, en un paquet et
  quelques minutes. C'est la marche la plus rentable avant d'écrire `claude-osd`.

---

### 🟠 MAJEUR

---

#### M1 — AppArmor a été désinstallé

```
$ dpkg -s apparmor
Status: deinstall ok config-files
$ cat /sys/kernel/security/lsm
lockdown,capability,landlock,yama,apparmor,tomoyo,bpf,ipe,ima,evm
```

Le noyau compte AppArmor parmi ses LSM, mais **le paquet a été retiré** et aucun profil
n'est chargé. Debian 13 l'installe par défaut et fournit des profils pour, entre autres,
les aides au bac à sable de Chromium et les utilitaires réseau.

Le mot `apparmor` n'apparaît nulle part dans `install/packages.list` ni dans les listes
de purge du dépôt : sa disparition est très probablement un **effet collatéral de la
purge ou de l'`autoremove`** qui a déjà emporté Chromium puis labwc les 7 et 8 septembre
(`docs/06`). Le garde-fou `VITAUX=` de `provision.sh` protège le compositeur ; il ne
protège pas les paquets de sécurité.

**Correctif :** `sudo apt-get install apparmor apparmor-profiles` puis redémarrer, et
ajouter `apparmor` à `VITAUX=` dans `provision.sh`.

---

#### M2 — Portable non chiffré, Secure Boot désactivé, write-protect firmware levé

- Aucun conteneur LUKS : `lsblk -o FSTYPE` donne `vfat / ext4 / swap` en clair.
- Secure Boot désactivé (conséquence assumée du firmware MrChromebox).
- Le cavalier `J1` a été ponté pour lever le write-protect (`docs/03`), et **rien dans la
  documentation n'indique de le retirer après le flash**. La seule mention de retrait
  (ligne 193) concerne le cas d'un mauvais candidat.
- TPM 2.0 présent (`/dev/tpm0`, `/dev/tpmrm0`) et totalement inutilisé.

**Impact.** Sur une machine transportable qui héberge les jetons de session de Claude
Desktop (`~/.config/Claude/Cookies`, `buddy-tokens.json`), le trousseau GNOME et
l'historique complet du projet, une perte physique donne **l'intégralité des données en
clair** à qui retire l'eMMC ou démarre sur une clé. Le write-protect resté levé ajoute
la possibilité de réécrire le firmware — c'est-à-dire de rendre la compromission
persistante et invisible.

**Correctif.** Le chiffrement complet exige une réinstallation ; à décider. À court
terme et sans réinstaller : retirer le cavalier `J1` et le documenter comme étape
obligatoire de clôture, et chiffrer au minimum `/home` (`ecryptfs`, ou un conteneur LUKS
sur fichier pour `~/.config/Claude`). La partition de swap (`p3`) doit aussi être
chiffrée ou supprimée : la zram suffit à 4 Go et le swap disque en clair peut contenir
des fragments de mémoire de Claude Desktop.

---

#### M3 — SSH exposé par mot de passe, sans clé ni pare-feu — et c'est le filet de secours

```
$ ss -tulpn → tcp LISTEN 0.0.0.0:22, [::]:22
$ ls ~/.ssh → known_hosts uniquement — aucune clé autorisée
$ nft list ruleset → vide ; ufw absent
$ grep X11Forwarding /etc/ssh/sshd_config → yes
```

`PasswordAuthentication` n'est pas configuré, donc **actif** (défaut Debian). Aucune
`authorized_keys` n'existe : l'accès distant se fait donc **au mot de passe uniquement**,
sur toutes les interfaces, sans limitation de débit (`fail2ban` absent) et sans pare-feu.

`CLAUDE.md` désigne cet accès comme « *le filet de secours de toute intervention* ». Un
filet de secours qui est en même temps la surface d'attaque la plus exposée de la machine
mérite d'être durci en priorité. Sur un portable qui se connecte à des réseaux Wi-Fi
tiers, l'exposition est réelle et permanente.

`X11Forwarding yes` est par ailleurs en contradiction directe avec l'architecture du
projet, où Xwayland est installé mais **jamais exécuté** : c'est de la surface d'attaque
pour une fonction qui ne sert pas.

**Correctif** — `/etc/ssh/sshd_config.d/10-claude-os.conf`, à verser dans `rootfs/` :

```
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin no
X11Forwarding no
AllowUsers stef
```

À faire **après** avoir déposé une clé publique et vérifié la connexion dans une seconde
session — sans quoi on se ferme la porte qu'on voulait garder ouverte. Ajouter un
pare-feu minimal (`nftables` : entrant `drop` sauf 22 depuis le réseau local, sortant
libre).

---

#### M4 — Aucune mise à jour de sécurité automatique

`unattended-upgrades` n'est pas installé, `/var/lib/apt/periodic/` est vide, aucun timer
d'`apt` n'est actif. Le système est aujourd'hui à jour (`apt-get -s upgrade` → 0), ce qui
tient à ce que quelqu'un a lancé `apt` le 8 septembre à 08 h 10 — pas à un mécanisme.

Sur une machine décrite comme « en service » au quotidien, avec 871 paquets, Chromium et
une application Electron exposée au web, l'écart se creuse en quelques semaines.

**Correctif :** ajouter `unattended-upgrades` à `packages.list` et activer
`50unattended-upgrades` sur `${distro_id}:${distro_codename}-security`. Attention : la
configuration APT du projet (`99claude-os-minimal`) met `Install-Recommends "false"`, il
faut donc nommer explicitement le paquet.

---

#### M5 — Cinq fichiers système ne sont pas dans `rootfs/`, donc pas déployables par `--deployer`

`provision.sh` écrit **en heredoc**, dans le corps du script, cinq fichiers de
configuration système :

| Fichier | Ligne | Ce qu'il porte |
|---|---|---|
| `/etc/apt/apt.conf.d/99claude-os-minimal` | 124 | politique APT (Recommends off) |
| **`/etc/greetd/config.toml`** | **517** | **`vt = 7` — l'invariant n°5** |
| `/etc/systemd/zram-generator.conf` | 725 | dimensionnement zram |
| `/etc/sysctl.d/99-claude-os.conf` | 734 | `vm.swappiness = 180`, etc. |
| `/etc/chromium.d/99-claude-os` | 752 | VA-API, ozone Wayland |

**Impact.** C'est l'invariant n°3 qui se reproduit ailleurs. Le `README` et `CLAUDE.md`
présentent `--deployer` comme la commande qui « recopie `rootfs/` vers `/` » et
`--compiler` comme celle qui installe le C. Or **aucune des deux ne touche ces cinq
fichiers**. Modifier `vt = 7` dans le dépôt et lancer la séquence documentée
`--deployer` + `--compiler` laisse la machine sur l'ancienne valeur, `--verifier` compare
la configuration réelle à l'unité systemd et dit que tout va bien — et l'on cherche
ailleurs. C'est exactement le scénario décrit dans l'invariant n°3, avec un autre
support.

Effet secondaire : ces fichiers ne sont ni relisibles ni diffables en tant que fichiers.
`/etc/greetd/config.toml`, qui porte 25 lignes de justification de l'invariant n°5, n'est
révisable qu'en lisant un heredoc au milieu d'un script de 924 lignes.

**Correctif.** Verser les cinq dans `rootfs/` (`rootfs/etc/greetd/config.toml`, etc.) et
remplacer les heredocs par la copie unique. Le cas de `/etc/greetd/config.toml` est le
plus urgent des cinq.

---

#### M6 — L'invariant n°4 est enfreint par le projet, aux endroits les plus coûteux

L'invariant dit : « *Ce projet a perdu Chromium, puis le compositeur, puis une bascule de
gestionnaire de session, à cause de trois `>/dev/null 2>&1` sur des commandes qui
échouaient en silence.* »

Trois violations subsistent, et ce sont précisément celles dont l'échec silencieux fait
le plus de dégâts.

**a) `provision.sh:412` — l'installation du shell**

```sh
run "meson install -C '$BUILD_DIR' >/dev/null" || die "l'installation du shell a échoué."
```

`|| die` rattrape le code de retour, mais **tout ce que `meson install` a à dire sur ce
qu'il a installé, et où, part dans `/dev/null`**. C'est le sujet même de l'invariant n°3
— « le 8 septembre, la machine a gardé un `claude-os-reglages` daté d'une heure plus
tôt ». La seule sortie qui aurait montré l'anomalie est celle qui est supprimée.
(`meson setup`, ligne 408, a le même problème.)

**b) `provision.sh:237` — la migration Wi-Fi**

```sh
run "nmcli connection add type wifi con-name '$SSID' ssid '$SSID' \
     wifi-sec.key-mgmt wpa-psk wifi-sec.psk '$PSK' connection.autoconnect yes \
     >/dev/null 2>&1 || true"
# puis, inconditionnellement :
run "cp -a '$IFACES' '$IFACES.avant-claude-os'"
run "awk '…commente la strophe wl…'"
```

La commande qui **crée** le profil réseau est muette *et* neutralisée par `|| true`.
Les deux lignes suivantes **désactivent `ifupdown` sans condition**. Si `nmcli` échoue —
SSID contenant un caractère spécial, NetworkManager pas encore prêt, profil homonyme
existant — la machine se retrouve **sans réseau du tout**, et le script affiche
« réseau « X » repris depuis ifupdown ». Sur une machine dont le seul accès de secours
est SSH (M3), c'est un enfermement dehors.

**c) `filet-session:130-146` — le filet de sécurité lui-même**

```sh
systemctl disable greetd.service >/dev/null 2>&1 || true
systemctl enable lightdm.service >/dev/null 2>&1 || true
systemctl set-default multi-user.target >/dev/null 2>&1 || true
```

Ce sont les trois commandes qui **exécutent** le repli. Si l'une échoue, le filet
journalise « greetd désactivé — la machine redémarre sur une console texte », puis
redémarre sur exactement la panne dont il devait sortir — et le rapport d'autopsie ne
contiendra pas la raison, puisqu'il est écrit avant.

**Correctif :** dans les trois cas, capturer la sortie plutôt que la supprimer :

```sh
SORTIE="$(systemctl disable greetd.service 2>&1)" \
  || dire "ÉCHEC disable greetd : $SORTIE"
```

Pour le Wi-Fi, conditionner la neutralisation d'`ifupdown` à la réussite effective de
`nmcli`, vérifiée par une relecture (`nmcli -g name connection show`), dans l'esprit du
« on relit ce qu'on vient d'écrire » de `claude-os-theme`.

---

#### M7 — `--verifier` ne contrôle jamais ce qu'il a déployé

Le contrôle de propriétaire porte sur un seul chemin (`/tmp/.X11-unix`,
`bascule-session.sh:334`). Rien ne vérifie :

- les propriétaires et modes des fichiers issus de `rootfs/` (→ **C1** invisible) ;
- l'existence et le contenu des cinq fichiers hors `rootfs/` (→ **M5** invisible) ;
- la présence des paquets de sécurité (→ **M1** invisible).

La comparaison de dates ajoutée après l'incident du 8 septembre est une bonne chose, mais
elle ne répond qu'à la question « le dépôt est-il en avance ? ». La question « ce qui est
sur la machine est-il *correct* ? » n'est pas posée.

---

### 🟡 MOYEN

---

#### Mo1 — Les binaires du shell sont compilés sans durcissement

```
claude-os-connexion   _FORTIFY_SOURCE : 0 symbole *_chk   stack protector : absent   RELRO : partiel (lazy)
claude-os-dock        idem
… les 7 binaires sont identiques
```

`meson.build` demande `warning_level=2`, `optimization=s`, `b_lto=true` — mais aucune
option de durcissement. PIE et ASLR sont bien là (héritage du gcc Debian), le reste
manque.

Cela porte sur 10 121 lignes de C qui analysent du JSON venant d'une socket
(`connexion.c`), lisent `sysfs`, et consomment des données D-Bus de NetworkManager et
BlueZ. `claude-os-connexion` tourne en outre sous `_greetd`, **avant toute
authentification**, et manipule le mot de passe de l'utilisateur.

Rien n'indique un défaut exploitable — la revue n'a trouvé ni `sprintf`, ni `strcpy`, ni
buffer de pile de taille fixe, et `connexion.c:183` plafonne explicitement la taille des
messages greetd. Il s'agit de défense en profondeur, à coût nul.

**Correctif** — dans `meson.build` :

```meson
add_project_arguments(
  '-D_FORTIFY_SOURCE=2', '-fstack-protector-strong', '-fstack-clash-protection',
  language : 'c')
add_project_link_arguments('-Wl,-z,relro', '-Wl,-z,now', language : 'c')
```

---

#### Mo2 — Le journal du greeter est écrasé à chaque tentative

`claude-os-greeter` choisit son journal ainsi :

```sh
if : > "$candidat"; then JOURNAL="$candidat"; break; fi
```

`: >` **tronque** le fichier. Or greetd relance le greeter à chaque échec de session.
Dans le scénario de boucle décrit dans `docs/06` — « la session meurt en une seconde, en
boucle » — la première tentative, celle qui porte la cause, est **immédiatement écrasée
par la deuxième**. Le fichier lu aujourd'hui ne contient qu'une seule tentative
(906 octets), ce qui confirme le comportement.

`claude-os-session`, lui, fait mieux : il conserve la tentative précédente en `.1`, avec
un commentaire qui explique exactement pourquoi (« *la panne qui intéresse est souvent
l'avant-dernière* »). La même logique manque au greeter.

Point secondaire : le dernier repli est `/tmp/claude-os-connexion.log`, chemin prévisible
dans un répertoire en 1777. `fs.protected_symlinks = 1` neutralise l'attaque classique,
donc le risque est faible — mais `"${XDG_RUNTIME_DIR}"` suffit et évite la question.

**Correctif :** appliquer la rotation `.1` du script de session au script du greeter.

---

#### Mo3 — Le compte `stef` ne peut pas lire le journal, alors que les outils de diagnostic en dépendent

```
$ id -nG stef → stef cdrom floppy sudo audio dip video plugdev users netdev bluetooth
                (ni adm, ni systemd-journal)
$ cat /proc/sys/kernel/dmesg_restrict → 1
$ journalctl -k -b → (vide)
```

`README` et `CLAUDE.md` documentent `bash tools/diag-session.sh` **sans `sudo`**. Or
`diag-session.sh:143` fait `journalctl -b … 2>/dev/null` : sous `stef`, cette section
**rend systématiquement vide**, sans dire pourquoi — le `2>/dev/null` supprime le message
qui l'aurait expliqué. Un outil de diagnostic qui rend une section vide alors que la
donnée existe est, selon les termes du projet lui-même, « *un outil qui ment* ».

C'est aussi ce qui m'a empêché de lire directement les messages noyau de l'audio (§6).

**Correctif :** `sudo usermod -aG adm stef` (accès en lecture au journal système, sans
privilège d'écriture), et retirer le `2>/dev/null` de la ligne 143.

---

#### Mo4 — La branche par défaut du dépôt distant est 21 commits en retard

```
origin/HEAD → origin/claude/claude-os-chromebook-nhoec4
git log --oneline origin/claude/claude-os-chromebook-nhoec4..HEAD → 21 commits
git diff --stat → 36 fichiers, 4 529 insertions, 504 suppressions
```

La branche vers laquelle pointe GitHub **ne contient ni la Console, ni le thème global,
ni la luminosité par logind, ni `tools/diag-connexion.sh`, ni le correctif
`/tmp/.X11-unix` par `ExecStartPre`**. La branche réellement en service est
`claude/examine-project-qgnt80`, ce que `CLAUDE.md` dit à l'invariant n°6 — mais
l'invariant se contente de décrire le piège (« *un `git pull` sur la mauvaise branche
répond « Déjà à jour » sans rien changer* ») sans le désamorcer.

Un `git clone` frais du dépôt donne aujourd'hui la version périmée, et `provision.sh`
l'accepte : son garde-fou ne teste que la présence de `VITAUX=`, présent dans les deux
branches.

**Correctif :** définir `claude/examine-project-qgnt80` comme branche par défaut sur
GitHub (ou fusionner vers une `main`), puis supprimer l'invariant n°6 devenu sans objet.
C'est un réglage d'interface, pas du code, et il supprime une classe entière d'incidents.

---

#### Mo5 — Le journal systemd n'est pas plafonné, sur un eMMC

`/etc/systemd/journald.conf` ne contient que `[Journal]`, sans réglage. Le journal
persistant pèse déjà **825 Mo** ; le défaut systemd autorise 10 % du système de fichiers,
soit **~5,4 Go**.

Le projet compte les mégaoctets (« *chaque mégaoctet au repos est un mégaoctet retiré à
Claude Desktop* ») et documente la faiblesse des IOPS de l'eMMC. 5 Go de journaux et
l'écriture continue associée y contreviennent, et l'usure de l'eMMC n'est pas
récupérable.

**Correctif** — à verser dans `rootfs/etc/systemd/journald.conf.d/99-claude-os.conf` :

```ini
[Journal]
SystemMaxUse=200M
SystemMaxFileSize=20M
MaxRetentionSec=1month
```

---

#### Mo6 — L'argument mémoire du shell sur mesure n'est pas vérifié

Le `README` justifie l'écriture de six programmes en C par : « *ils pèsent moins que les
six composants existants qu'ils remplacent* ». La mesure PSS donne **154,5 Mo pour trois
processus** (dock 57,2 · barre 57,0 · fond 40,3), sur une machine de 4 Go.

C'est le coût normal de GTK 4 — chaque processus porte son propre contexte GTK, son
rendu Vulkan/Cairo et ses thèmes d'icônes — et non un défaut de codage. Mais l'ordre de
grandeur est comparable, voire supérieur, à ce que consommeraient `tint2` + `dunst` +
`hsetroot`, et **la comparaison n'a jamais été faite**. Sur un projet dont la règle
n°1 est « mesurer, pas supposer », c'est une affirmation d'architecture qui n'a pas été
soumise à sa propre méthode.

Deux pistes si l'empreinte devient gênante : fusionner `claude-os-fond` dans
`claude-os-status` (le fond d'écran est statique et n'a pas besoin de son processus), et
poser `GTK_A11Y=none` — voir Mi5.

**Correctif minimal :** mesurer, puis corriger la phrase du `README` ou l'étayer.

---

#### Mo7 — `run()` évalue ses arguments, et le PSK Wi-Fi y transite

```sh
run() { if [ "$DRY" -eq 1 ]; then printf …; else eval "$@"; fi }
…
run "nmcli connection add … wifi-sec.psk '$PSK' …"
```

Deux conséquences :

1. Un `'` dans le SSID ou le mot de passe Wi-Fi **s'échappe de la chaîne** et le reste est
   interprété par un shell root. Le vecteur est étroit (il faut contrôler le contenu de
   `/etc/network/interfaces`), mais le motif est à proscrire dans un script qui tourne en
   root.
2. Le PSK est passé **en argument de ligne de commande** : il est visible dans `ps` par
   tout utilisateur pendant l'exécution.

**Correctif :** passer par un tableau plutôt qu'`eval`, et fournir le secret par
`nmcli --ask` ou un fichier temporaire en 0600 plutôt que par `argv`.

---

#### Mo8 — `claude-os-theme` écrase les réglages GTK de l'utilisateur

```sh
cat > "$CFG_DIR/gtk-$v/settings.ini" <<EOF
```

Le fichier est **remplacé intégralement** à chaque changement de thème et à chaque
ouverture de session, pour GTK 3 et GTK 4. Toute clé posée par l'utilisateur ou par une
application (`gtk-xft-antialias`, `gtk-decoration-layout`, réglages d'accessibilité…)
disparaît silencieusement.

Le script prend pourtant soin, quinze lignes plus bas, de **ne pas** écraser
`terminalrc` (« *pour ne pas écraser ses propres couleurs* ») et écrit dans
`themerc-override` un en-tête « ne pas modifier à la main ». Le soin n'a pas été étendu à
`settings.ini`.

**Correctif :** écrire l'en-tête d'avertissement dans `settings.ini` aussi, et ne
réécrire que les cinq clés gérées (fusion) plutôt que le fichier entier.

---

#### Mo9 — Le repli LightDM du filet de sécurité n'existe plus

`filet-session` prévoit deux issues : réactiver LightDM, ou basculer sur
`multi-user.target`. Or **LightDM n'est plus installé** (`systemctl is-enabled lightdm`
→ `not-found` ; `dpkg -l lightdm` → rien). La première branche est morte : le filet
retombera toujours sur la console texte.

Ce n'est pas grave en soi — la console texte remplit la fonction, et le code le prévoit
proprement. Mais `README` et `CLAUDE.md` décrivent encore le repli comme pouvant rendre
un écran graphique, et le rapport de `tools/audit-systeme.sh` daté du 8 septembre
affichait encore « lightdm installé / activé ». La documentation décrit un état qui n'est
plus.

**Correctif :** documenter que le repli est désormais la console texte, uniquement.

---

#### Mo10 — Réglages noyau permissifs sur une machine qui héberge des jetons

```
kernel.yama.ptrace_scope = 0     (aucun fichier sysctl.d ne le fixe)
kernel.kptr_restrict     = 0
```

`ptrace_scope = 0` autorise tout processus tournant sous `stef` à s'attacher à tout autre
processus de `stef` — dont Claude Desktop et Chromium — et à en lire la mémoire : jetons
de session, contenu des conversations, mot de passe déchiffré du trousseau. C'est le
défaut Debian, mais le profil d'usage de cette machine justifie de s'en écarter.

**Correctif** — à ajouter à `rootfs/etc/sysctl.d/99-claude-os.conf` (qui doit lui-même
rejoindre `rootfs/`, cf. M5) :

```ini
kernel.yama.ptrace_scope = 1
kernel.kptr_restrict = 1
```

---

### 🔵 MINEUR

| # | Constat | Correctif |
|---|---|---|
| Mi1 | `/etc/fstab` monte `/` sans `noatime` sur un eMMC dont le projet documente la lenteur | ajouter `noatime` aux options de la racine |
| Mi2 | Incohérence terminal : `rc.xml:67` lie Super+Entrée à `foot`, que `packages.list:75` déclare « *secours au clavier — jamais le terminal courant* », alors que `config.c:10` épingle `xfce4-terminal` | choisir, et aligner les trois |
| Mi3 | `audit-systeme.txt`, `diag-connexion.txt`, `diag-session.txt` sont non suivis **et non ignorés** à la racine du dépôt (`.gitignore` ne couvre que `*.log`). Aucun secret trouvé dedans, mais ils contiennent l'état détaillé de la machine | ajouter `/audit-*.txt`, `/diag-*.txt` au `.gitignore` |
| Mi4 | Les fichiers créés par les heredocs de `provision.sh` sont en `664 root:root` (umask 002). Le groupe est `root`, donc sans conséquence pratique — mais c'est le même mécanisme que **C1** | `umask 022` en tête de `provision.sh` |
| Mi5 | Chaque composant du shell émet un avertissement GTK « *Unable to acquire the address of the accessibility bus* » à chaque démarrage, qui pollue `shell.log` et `claude-os-connexion.log` — précisément les journaux sur lesquels repose le diagnostic | poser `GTK_A11Y=none` dans `/etc/xdg/labwc/environment`, ou installer `at-spi2-core` si l'accessibilité est souhaitée |
| Mi6 | `srbds: Vulnerable: No microcode` | non corrigeable — Intel n'a pas publié de microcode SRBDS pour Tremont. À connaître, rien à faire |
| Mi7 | `~/.config/claude-os/shell.conf` pointe le fond d'écran vers `~/Downloads/Gemini_Generated_Image_.jpeg` | déplacer l'image hors de `Downloads`, qui est un répertoire de passage |

---

## 5. Ce qui est bien fait

Il serait malhonnête de ne lister que les défauts. Les points suivants sont au-dessus de
ce qu'on voit habituellement, et méritent d'être préservés lors des correctifs :

- **Aucune injection de commande dans 10 121 lignes de C.** `wifi.c` et `bluetooth.c`
  parlent directement à NetworkManager et BlueZ **par D-Bus** au lieu d'appeler `nmcli`
  et `bluetoothctl` — c'est le choix coûteux et le bon. Les rares sous-processus
  (`wpctl`, `claude-os-theme`) passent par `g_subprocess_newv` avec un `argv` explicite.
  Le seul `g_spawn_command_line_async` (`dock.c:576`) porte une constante littérale.
- **Le mot de passe ne transite jamais par un shell** : `connexion.c` le sérialise en
  JSON via `json-glib` et l'envoie sur la socket greetd. La taille des messages entrants
  est plafonnée (ligne 183) avec un commentaire qui explique pourquoi.
- **Les secrets sont correctement protégés** : `~/.claude.json`, `~/.config/Claude/*` et
  `/etc/NetworkManager/system-connections/*` sont tous en `0600`. Aucun secret trouvé
  dans `.bash_history` ni dans les fichiers de diagnostic.
- **La journalisation du greeter et de la session** est la bonne réponse à la panne
  qu'elle vise : contexte, sortie complète **et code de retour**, avec l'explication du
  « pas de `exec` » à chaque fois.
- **Le déploiement par étapes** (`--verifier` / `--deployer` / `--compiler` / `--essai` /
  `--basculer` / `--revenir`), et surtout `--essai` sur un terminal virtuel libre, qui
  permet de voir le vrai écran de connexion sans rien engager.
- **Le nettoyage du compositeur orphelin** dans `--essai` (`bascule-session.sh:740-760`)
  : avoir identifié que `pam_systemd` déplace le greeter dans un scope hors du cgroup du
  service, et que l'outil fabriquait donc le symptôme qu'il cherchait, est un diagnostic
  de bonne facture.
- **La résolution des faux négatifs dans `postmortem_essai`** : suivre les `@include` PAM
  plutôt que `grep` le seul fichier, avec le commentaire qui dit pourquoi le raccourci
  aurait menti.
- **`claude-os-theme` relit ce qu'il vient d'écrire** (`gsettings set` rend 0 sans
  conserver), et déduit clair/sombre de la luminosité mesurée de la couleur de fond
  plutôt que du nom du thème.
- **La densité et la qualité des commentaires** : ils expliquent le *pourquoi*, citent la
  mesure et datent l'observation. `docs/06` distingue explicitement ce qui est prouvé de
  ce qui est plausible. C'est rare, et c'est ce qui a rendu cet audit possible en
  quelques heures.

---

## 6. Chantier n°1 — l'audio : **cause établie**

> **Mise à jour du 8 septembre, 12 h 20.** La première version de cette section
> proposait deux hypothèses, faute d'accès au journal noyau. L'accès root par le
> guichet `claude-os-root` (§9) a permis de lire `dmesg` : **la première est
> confirmée, la seconde est fausse.** Elle est conservée ci-dessous, barrée, parce
> qu'écarter une piste vaut la peine d'être écrit.

### La cause

```
sof-audio-pci-intel-icl 0000:00:1f.3: NHLT table not found
sof-audio-pci-intel-icl 0000:00:1f.3: BT link detected in NHLT tables: 0x0
sof-audio-pci-intel-icl 0000:00:1f.3: DMICs detected in NHLT tables: 0
…
sof-audio-pci-intel-icl 0000:00:1f.3: ipc tx timed out for 0x30100000 (msg/reply size: 48/0)
sof-audio-pci-intel-icl 0000:00:1f.3: error: host status 0x80000000 dsp status 0x00000000
```

**`NHLT table not found`.** La *Non-HD-Audio Link Table* est la table ACPI qui décrit
au pilote SOF les liens SSP et DMIC — horloges, format, numéro de port. Elle est absente
des tables ACPI publiées par le firmware :

```
$ ls /sys/firmware/acpi/tables/
APIC BGRT DBG2 DMAR DSDT FACP FACS HPET LPIT MCFG SSDT TPM2
```

La chaîne complète, telle que le journal la donne :

1. Le DSP est détecté et **le firmware se charge correctement** (`Firmware info: version
   2:2:0-57864`).
2. **La topologie se charge aussi** (`Topology: ABI 3:23:0`) — ce n'est donc pas un
   fichier manquant.
3. Faute de NHLT, SOF construit une configuration de liens **vide** : `BT link … 0x0`,
   `DMICs … 0`.
4. Il envoie alors au DSP une IPC que celui-ci ne peut pas honorer → `ipc tx timed out`,
   `dsp status 0x00000000` : le DSP ne répond simplement jamais.
5. `sof_rt5682` échoue au `probe` avec `-22` (`EINVAL`).

Les trois messages rapportés dans `CLAUDE.md` sont donc les **symptômes successifs d'une
seule cause**, en amont de tous : le firmware ne publie pas NHLT.

**C'est un problème de firmware, pas de Debian.** Sous ChromeOS, coreboot publiait cette
table ; MrChromebox-2606.1 ne le fait pas pour `MADOO`. Aucun réglage côté noyau, aucun
paquet, aucune topologie ne peut y suppléer.

### ~~Piste n°2 — firmware signé Intel plutôt que communauté~~ — **écartée**

~~Le lien `/lib/firmware/intel/sof/sof-jsl.ri → intel-signed/sof-jsl.ri` suggérait que le
DSP refusait un binaire mal signé.~~ Le journal montre que **le noyau choisit déjà le bon
chemin** :

```
Firmware file:  intel/sof/community/sof-jsl.ri
Topology file:  intel/sof-tplg/sof-jsl-rt5682-mx98360a.tplg
```

La correspondance DMI fonctionne malgré le remplacement du firmware, et le binaire
communautaire est accepté par le DSP. **Ne pas perdre de temps sur cette piste** : le
symlink `intel-signed` visible dans l'arborescence est le défaut du paquet Debian, que le
pilote surcharge à l'exécution. C'était une inférence raisonnable à partir de
l'arborescence seule, et elle était fausse.

### Ce qu'il reste à faire

Deux voies, par ordre de coût :

1. **Vérifier s'il existe un firmware MrChromebox plus récent pour `MADOO`.** La machine
   tourne sur `2606.1`. Si une version ultérieure rétablit NHLT, c'est un flash et c'est
   fini. À faire d'abord — mais **le write-protect (`J1`) doit être reponté pour flasher**,
   et retiré après (cf. **M2**).

2. **Injecter la table NHLT par surcharge ACPI dans l'initramfs.** Voie documentée et
   fonctionnelle, mais laborieuse : il faut extraire la NHLT de `MADOO` depuis une image
   de récupération ChromeOS pour `madoo`, puis la livrer au noyau :

   ```sh
   # squelette — la table doit d'abord être extraite d'une image ChromeOS madoo
   mkdir -p /tmp/acpi/kernel/firmware/acpi
   cp nhlt.aml /tmp/acpi/kernel/firmware/acpi/
   (cd /tmp/acpi && find . | cpio -H newc --create > /boot/acpi-nhlt-madoo.img)
   # puis ajouter acpi-nhlt-madoo.img comme initrd supplémentaire dans GRUB
   ```

   À tester **avec le filet armé** : une surcharge ACPI mal formée peut empêcher le
   démarrage.

3. Si aucune des deux ne passe : le matériel restera muet sous ce firmware. Le HDMI et le
   Bluetooth A2DP resteraient des sorties possibles, `snd_hda_codec_hdmi` étant chargé.

**Ce qui n'est PAS en cause, et qu'il ne faut plus explorer :** le paquet
`firmware-sof-signed` (à jour, 2025.01-1), le fichier de topologie (présent et chargé), la
signature du firmware (communautaire, acceptée), les modules `snd_sof_*` (les 20 sont
chargés), et le codec `rt5682` (détecté sur `i2c-10EC5682:00`).

---

## 7. Écarts documentation ↔ machine

Récapitulatif de ce que la documentation affirme et que la machine contredit. Ce tableau
est le plus important du rapport pour un lecteur qui découvre le projet : c'est ce qui
peut l'induire en erreur.

| Affirmé | Où | Constaté |
|---|---|---|
| « Système de fichiers : **btrfs + compression zstd** » — Décisions actées | `README.md:50` | **ext4**, aucun sous-volume, aucun snapshot |
| Courtier `claude-osd`, `policy.d`, `claude-os rollback`, journal d'audit | `docs/02` §2.6 | **rien de tout cela n'existe** |
| « chaque action privilégiée est tracée et annulable » — contrainte n°3 | `README.md` | ni tracée, ni annulable ; et **C1** la rend inutile |
| `--deployer` « recopie `rootfs/` vers `/` » | `README`, `CLAUDE.md` | exact, mais **5 fichiers système** ne sont pas dans `rootfs/` (**M5**) |
| « Aucune sortie de commande n'est envoyée dans `/dev/null` » — invariant n°4 | `CLAUDE.md` | enfreint dans `provision.sh` et dans le filet lui-même (**M6**) |
| Repli du filet vers LightDM | `filet-session`, `docs/06` | LightDM **non installé** — repli console uniquement |
| Branche de référence `claude/examine-project-qgnt80` | `CLAUDE.md` invariant n°6 | exact, mais `origin/HEAD` pointe ailleurs, 21 commits en retard |
| `bash tools/diag-session.sh` (sans sudo) | `README`, `CLAUDE.md` | rend des sections vides : `stef` ne lit pas le journal (**Mo3**) |
| `foot` = « secours au clavier, jamais le terminal courant » | `packages.list:75` | `rc.xml:67` lie Super+Entrée à `foot` |

Rien de tout cela n'empêche la machine de fonctionner. Mais `CLAUDE.md` est chargé
automatiquement à l'ouverture de chaque session : ces écarts sont, littéralement, ce
qu'un agent lit comme vrai avant d'intervenir.

---

## 8. Plan de remédiation

### Aujourd'hui — 30 minutes, sans risque

1. **C1** — rendre `/`, `/etc`, `/usr` et les 55 fichiers à `root`, puis corriger
   `cp -a` dans les deux scripts. *C'est le correctif le plus rentable du lot.*
2. **Mo3** — `sudo usermod -aG adm stef`. Débloque le diagnostic audio et les outils de
   diagnostic du projet.
3. **M1** — réinstaller AppArmor, et l'ajouter à `VITAUX=`.
4. **Mo4** — changer la branche par défaut sur GitHub.
5. **C2 (volet documentaire)** — déplacer btrfs et le courtier de « Décisions actées »
   vers « Non implémenté », dans `README.md` et `CLAUDE.md`.

### Cette semaine

6. **M6** — les trois `>/dev/null` : `meson install`, `nmcli`, et le filet. Conditionner
   la neutralisation d'`ifupdown` à la réussite vérifiée de `nmcli`.
7. **M3** — clé SSH, `PasswordAuthentication no`, `X11Forwarding no`, pare-feu nftables.
   **Dans cet ordre, et en gardant une seconde session ouverte.**
8. **M4** — `unattended-upgrades`.
9. **M5** — verser les cinq fichiers dans `rootfs/`, en commençant par
   `/etc/greetd/config.toml`.
10. **M7** — ajouter à `--verifier` : propriétaires, modes, présence des cinq fichiers,
    présence d'AppArmor.
11. **Mo5** — plafonner journald.

### Ensuite

12. **Audio** (§6) — piste 2 d'abord, piste 1 ensuite.
13. **Mo1** — durcissement de compilation dans `meson.build`.
14. **Mo2** — rotation `.1` du journal du greeter.
15. **Mo10, Mo8, Mo7, Mi1–Mi7**.
16. **C2 (volet technique)** — `etckeeper` comme premier pas vers une réversibilité
    réelle, en attendant `claude-osd`.
17. **M2** — décider du chiffrement, et retirer le cavalier `J1`.

---

## 9. Test de la porte instrumentée `claude-os-root`

> Ajouté le 8 septembre à 12 h 20, à la demande de l'utilisateur. Le guichet a été
> installé à 12 h 12, **après** le corps de cet audit — qui constatait son absence (**C2**).

### Ce qui a été testé, et ce qui marche

| Test | Résultat |
|---|---|
| `--expliquer` sur 4 commandes | classement correct : lecture → 1, `apt-get install` → 2, `fdisk` et `cat /etc/shadow` → 3 |
| Niveau 1 (`id`, `journalctl -k`) | exécuté en `uid=0`, sans mot de passe ✅ |
| Niveau 2 (`apt-get install -y tree`, avec `--pourquoi`) | exécuté, motif journalisé ✅ |
| Journal `/var/log/claude-os/actions.log` | 7 lignes, format tabulé : date, niveau, porte, instantané, code, demandeur, motif, commande ✅ |
| `claude-os journal` | rendu lisible, motif entre guillemets ✅ |
| `claude-os etat` | signale de lui-même l'absence de btrfs ✅ |
| Niveau 3 | **non testé** — ouvrirait une fenêtre sur le bureau, et rien n'indique que l'utilisateur soit devant la machine |

**Ce qui est bien fait.** Le guichet **ne ment pas sur ses limites** : `claude-os etat`
affiche spontanément « *racine en ext2/ext3 et non btrfs : AUCUN instantané possible* » et
« *les actions de niveau 2 s'exécuteront, mais ne seront pas annulables* », et
`--expliquer` répète l'avertissement sur chaque commande concernée. `politique.conf`
prévoit même `instantanes=toujours` pour le jour où btrfs sera là. C'est exactement la
posture que **C2** reprochait au `README` de ne pas tenir : le mécanisme dit ce qu'il ne
peut pas faire, plutôt que de le laisser croire.

### Le défaut, et il est structurel

**La porte est posée sur un mur que `stef` peut déplacer.**

```
drwxrwxr-x stef:stef  /usr/local/bin      ← inscriptible par stef
-rwxr-xr-x root:root  /usr/local/bin/claude-os-root
drwxrwxr-x stef:stef  /etc                ← inscriptible par stef
drwxr-xr-x root:root  /etc/sudoers.d
drwxr-xr-x root:root  /etc/claude-os
```

Les fichiers sensibles sont correctement en `root:root`, et `--expliquer` classe bien
leur modification en niveau 3. Mais **le droit d'écriture porte sur le répertoire, pas
sur le fichier** : renommer ou supprimer une entrée ne demande que le droit d'écriture
sur le répertoire qui la contient. Sans bit collant, `stef` peut donc écarter le guichet
et mettre autre chose à sa place — sur le chemin même que `sudoers` autorise sans mot de
passe.

Conséquence : **le niveau 3 n'est pas une frontière tant que C1 tient.** Ce n'est pas un
défaut du guichet, qui est bien écrit ; c'est C1 qui vide de sa substance tout mécanisme
d'autorisation posé par-dessus. C'est ce qui fait passer C1 de « critique » à
« bloquant ».

### Remarques mineures

- `cat /etc/claude-os/politique.conf` est classé **niveau 3**, alors que le fichier est
  en `0644` et que `stef` le lit directement sans le guichet. La classification en
  lecture est donc sans effet — sans danger, mais elle donne une fausse impression de
  protection. Seule l'**écriture** a besoin d'être en niveau 3.
- Le classement du niveau 2 est un **défaut permissif** : « *écriture présumée : ni
  lecture reconnue, ni commande sensible* ». Une commande destructrice non reconnue
  (`dd`, `mkfs`, `shred`, `rm -rf`) tombe en niveau 2 si elle n'est pas nommée
  explicitement. À vérifier commande par commande avec `--expliquer`, et à compléter par
  `niveau3_supplementaire` dans `politique.conf`.
- `claude-os-root chown root:root /etc` est classé **niveau 2** : le correctif de C1 est
  donc applicable par la porte elle-même, sans confirmation.

### Correctif

Le correctif de C1 (§4) suffit. Ajouter, pour que la régression ne revienne pas :

```sh
# à ajouter au contrôle de --verifier
for d in / /etc /usr /usr/local /usr/local/bin; do
    [ "$(stat -c '%U:%G' "$d")" = "root:root" ] \
        || ko "propriétaire" "$d appartient à $(stat -c '%U' "$d") — le guichet est contournable"
done
```

---

## 10. Annexe — commandes de vérification

```sh
# C1 — fichiers système appartenant à stef
find / -xdev \( -path /home -o -path /proc -o -path /sys -o -path /tmp -o -path /run \) \
     -prune -o -user stef -printf '%M %u:%g %p\n'

# C2 — le modèle de privilèges
findmnt -no FSTYPE /              # ext4
command -v claude-osd claude-os-mcp claude-os
ls /etc/claude-os/

# M1 — AppArmor
dpkg -s apparmor | grep Status

# M3 — exposition réseau
ss -tulpn ; nft list ruleset ; ls ~/.ssh

# M5 — fichiers hors rootfs/
grep -nE 'cat > /(etc|usr|var)' install/provision.sh

# Mo1 — durcissement des binaires
for b in /usr/bin/claude-os-*; do
  printf '%-28s chk=%s stk=%s\n' "$(basename $b)" \
    "$(nm -D $b | grep -c '_chk@')" "$(nm -D $b | grep -c '__stack_chk_fail')"
done

# §6 — audio
ls /sys/firmware/acpi/tables/ | grep -i nhlt      # attendu : rien
ls -la /lib/firmware/intel/sof/sof-jsl.ri         # → intel-signed/
ls /lib/firmware/intel/sof/community/sof-jsl.ri   # la variante attendue
cat /proc/asound/cards
sudo dmesg | grep -i nhlt        # « NHLT table not found »

# §9 — la porte, et ce qui la désarme
claude-os etat
claude-os journal
claude-os-root --expliquer <commande>
stat -c '%U:%G %a' / /etc /usr /usr/local /usr/local/bin
```

---

*Audit réalisé le 8 septembre 2026 sur la machine `Claude-OS`, contre le
commit `2ce21dc` de la branche `claude/examine-project-qgnt80`. Complété à 12 h 20 par le
test de la porte `claude-os-root` (§9) et la résolution du diagnostic audio (§6).*
