# 7. L'accès root de Claude Desktop

L'exigence n°2 du projet est que Claude Desktop dispose de pleins pouvoirs sur
la machine. La contrainte n°3 est que ces pouvoirs restent **tracés et
annulables**. Ce document décrit ce qui a été mis en place pour tenir les deux,
et pourquoi les solutions plus simples ont été écartées.

---

## 7.1 Le problème réel

Claude Desktop lance ses commandes **sans terminal**. Ce détail décide de tout :

- `sudo apt install foo` n'a nulle part où réclamer un mot de passe. Il échoue
  aussitôt sur `sudo: no tty present and no askpass program specified`.
- Le message ne dit pas « il faut confirmer », il ressemble à un défaut de
  droits. Conclusion pratique pour l'agent : « je n'ai pas les droits », et il
  s'arrête là.

L'accès root n'était donc pas seulement *restreint*, il était **invisible**.
Rendre l'accès « natif » veut dire deux choses : qu'il fonctionne sans
manipulation, et qu'il soit **annoncé** là où l'application lit ses consignes.

---

## 7.2 Ce qui a été écarté, et pourquoi

| Piste | Pourquoi non |
|---|---|
| Lancer Claude Desktop en root | Une application Electron qui ouvre des pages web tournerait avec tous les droits. Chromium refuse d'ailleurs son bac à sable en root, et le trousseau de session ne suit pas. Ce n'est pas le pouvoir de l'application qu'on veut élever, c'est celui de ses commandes. |
| `NOPASSWD: ALL` sur le compte | Fonctionne, et ne trace rien. Aucun instantané, aucune distinction entre installer un paquet et repartitionner l'eMMC. C'est « pleins pouvoirs de casser sans retour ». |
| Le courtier `claude-osd` de `docs/02` §2.6 | C'est la bonne cible : un démon, un socket, un serveur MCP. C'est aussi trois composants à écrire, à faire tourner en permanence sur une machine de 4 Go, et à déboguer. Le guichet ci-dessous rend le même service — classement, instantané, journal, confirmation — en un script, sans démon résident. La porte MCP pourra s'y brancher plus tard sans rien changer au modèle. |

---

## 7.3 Le guichet

```
Claude Desktop  (session utilisateur, NON privilégié)
      │
      │  claude-os-root <commande…>
      ▼
claude-os-root  (classe la commande, choisit la porte)
      │
      ├── niveaux 1 et 2 ──► sudo, sans mot de passe
      │                      instantané btrfs si niveau 2, puis exécution
      │
      └── niveau 3 ────────► sudo, AVEC mot de passe
                             claude-os-askpass ouvre une fenêtre sur le
                             bureau, montre la commande, attend l'accord
```

Un seul composant est privilégié, et il est petit. Claude obtient un pouvoir
complet sur la machine, mais **par une porte instrumentée**.

### Les trois niveaux

| Niveau | Exemples | Comportement |
|---|---|---|
| **1 — lecture** | `journalctl`, `systemctl status`, `dpkg -l`, `lsblk` | Direct, journalisé, sans instantané. |
| **2 — écriture réversible** | installer un paquet, modifier une configuration, activer un service | **Instantané btrfs**, exécution, journalisation. Annulable par `claude-os rollback`. |
| **3 — irréversible ou sensible** | partitionner, flasher, `passwd`, `/etc/shadow`, `/etc/sudoers`, transfert vers une machine distante | **Confirmation humaine** dans une fenêtre, qui affiche la commande exacte. |

Le classement est fait sur la **commande entière**, pas sur le seul nom du
programme : `btrfs subvolume list` se lit, `btrfs subvolume delete` détruit.

Le principe est asymétrique, et volontairement : le niveau 1 est une liste
blanche **courte**, le niveau 3 une liste de motifs, et **tout le reste tombe
au niveau 2**. Se tromper au niveau 2 coûte un instantané inutile. Se tromper
au niveau 1 coûterait un instantané manquant, et cela ne se voit que le jour
où on le cherche.

Pour savoir où tombe une commande sans l'exécuter :

```sh
claude-os-root --expliquer apt-get install tree
```

---

## 7.4 Comment le niveau 3 est réellement imposé

C'est le point qui mérite d'être compris, parce que c'est lui qui fait la
différence entre une politique et une convention.

Le même programme est atteignable par **deux chemins** :

```
/usr/local/bin/claude-os-root       →  NOPASSWD dans /etc/sudoers.d
/usr/local/bin/claude-os-sensible   →  lien vers le même fichier, ABSENT de
                                       la règle NOPASSWD
```

`sudo` traite un lien symbolique comme une commande distincte : il ne le suit
pas jusqu'à sa cible. Le second chemin retombe donc sur la règle générale, qui
exige le mot de passe.

Quand `claude-os-root` classe une commande au niveau 3, il se relance
**par le second chemin**. Ce n'est donc pas le programme qui s'interdit le
niveau 3 par bonne volonté : c'est `sudo` qui le lui refuse. Contourner
supposerait de modifier `/etc/sudoers.d` — qui est lui-même de niveau 3.

Un garde-fou complète le dispositif : si le niveau 3 est atteint par la porte
sans mot de passe (`sudo claude-os-root parted …` tapé à la main), le
programme refuse et le consigne.

La règle sudo, dans l'ordre — **et l'ordre compte, sudoers retient la dernière
règle qui correspond** :

```
%claude-os-root ALL=(ALL:ALL) ALL                        # avec mot de passe
%claude-os-root ALL=(root) NOPASSWD: /usr/local/bin/claude-os-root
```

La première ligne n'est pas une facilité : sur une Debian installée avec un
mot de passe root, le premier compte n'est **pas** dans le groupe `sudo`.
Sans elle, le niveau 3 serait impossible même en connaissant le mot de passe.

---

## 7.5 La fenêtre de confirmation

`sudo` sait déléguer la saisie du mot de passe à un programme externe quand il
n'y a pas de terminal — c'est exactement notre cas. Deux précisions apprises à
l'usage :

1. Ce réglage se déclare dans **`/etc/sudo.conf`** (`Path askpass …`), **pas**
   dans `sudoers`. L'écrire dans un fichier `sudoers` le fait rejeter en
   entier : `unknown defaults entry "askpass"`.
2. `sudo` ne l'utilise pas de lui-même : il faut **`sudo -A`**. C'est le
   guichet qui l'ajoute, ce qui est une raison de plus de passer par lui plutôt
   que par `sudo` directement.

`claude-os-askpass` ouvre alors une fenêtre `foot` sur la session, affiche la
commande — pas le chemin du guichet, la commande réelle — et attend. Fenêtre
fermée ou saisie vide valent refus.

`timestamp_timeout=0` : l'autorisation n'est **pas** mise en cache. Chaque
action de niveau 3 est approuvée pour elle-même. Une approbation ne doit pas
en couvrir quinze autres pendant le quart d'heure qui suit.

---

## 7.6 Réversibilité

Avant chaque écriture de niveau 2, un instantané btrfs en lecture seule est
pris, et son identifiant part **dans le journal, sur la même ligne que la
commande**. C'est ce lien qui permet, plus tard, de dire « annule ça » et de
savoir quoi restaurer.

```sh
claude-os journal          # ce qui a été fait, avec quel instantané
claude-os instantanes      # les instantanés disponibles, du plus récent
claude-os rollback <id>    # restaure, puis redémarre
```

`rollback` ne recopie pas des fichiers. Il monte la racine réelle du système
de fichiers (`subvolid=5`), met `@` de côté sous `@-remplace-<date>`, et
recrée `@` à partir de l'instantané. **L'ancien `@` n'est jamais détruit** : si
le retour arrière est lui-même une erreur, il est encore là. `/home` est un
sous-volume distinct et n'est pas touché.

Deux dépôts possibles, dans cet ordre de préférence :

- `/.snapshots`, monté depuis le sous-volume `@snapshots`. C'est le bon
  endroit : il est **hors de `@`**, donc un retour arrière ne l'emporte pas.
  Le correctif crée ce sous-volume et l'entrée `fstab` correspondante si la
  racine est en btrfs et que la disposition est bien celle du projet ;
- `/.instantanes`, un simple répertoire dans `@`. Repli fonctionnel, mais les
  instantanés y sont emportés par un rollback.

Toutes les écritures de niveau 2 ne méritent pas un instantané : démarrer un
service, monter un volume, changer la luminosité ne laissent rien sur le
disque. Elles restent journalisées, sans instantané — sinon la rotation
chasserait les instantanés qui comptent au profit de copies inutiles.

**Si la racine n'est pas en btrfs**, le guichet exécute quand même et le dit,
en clair, à chaque action. Le journal reste tenu, mais plus rien n'est
annulable. La politique `instantanes=toujours` inverse ce choix : pas
d'instantané, pas d'exécution.

---

## 7.7 Le journal

Une ligne par action privilégiée, en champs séparés par des tabulations :

```
date  niveau  porte  instantané  code  demandeur  motif  commande
```

Doublé dans `journald` (`journalctl -t claude-os-root`), qui est l'exemplaire
difficile à retoucher. Le fichier `/var/log/claude-os/actions.log` est celui
qu'on lit ; il appartient au groupe `claude-os-root` en lecture seule, tourne
tous les mois et est conservé douze mois.

`docs/02` §2.6 annonçait un journal *append-only* (`chattr +a`). Ce n'est pas
ce qui a été fait : un fichier en `+a` ne peut pas tourner, et un journal qui
ne tourne pas finit par remplir un eMMC de 64 Go. L'inviolabilité est portée
par `journald`, la lisibilité par le fichier.

`--pourquoi "…"` ajoute un motif à côté de la commande. C'est ce qui rend une
trace relisible dans six mois.

---

## 7.8 Ce que Claude en sait

Rien de tout cela ne sert si l'application l'ignore. Le correctif écrit un
bloc délimité dans `~/.claude/CLAUDE.md` du compte du bureau — le contenu
personnel du fichier est préservé, seul le bloc est remplacé à chaque mise à
jour. Il annonce le guichet, les trois niveaux, et la conduite à tenir devant
un niveau 3.

C'est ce qui fait la différence entre un accès *disponible* et un accès
*natif*.

---

## 7.9 Mise en place et retrait

```sh
sudo bash install/patch-acces-root-claude.sh --dry-run   # montre tout, ne fait rien
sudo bash install/patch-acces-root-claude.sh
sudo bash install/patch-acces-root-claude.sh --retirer   # défait
```

`provision.sh` appelle le même script : il n'y a pas deux exemplaires du code.

Le script est idempotent, sauvegarde tout ce qu'il touche sous
`/root/claude-os-root-backup-<date>`, et **vérifie la règle sudo avant de
l'installer** puis la configuration complète après — un fichier invalide dans
`/etc/sudoers.d` casse `sudo` pour tout le monde, y compris pour la commande
qui servirait à le réparer.

L'appartenance au groupe `claude-os-root` ne prend effet **qu'à la prochaine
ouverture de session**. C'est la seule étape manuelle.

Le retrait conserve volontairement les instantanés, le journal et
`/etc/claude-os/politique.conf` : défaire un mécanisme de traçabilité ne doit
pas effacer les traces de ce qu'il a enregistré.

### Contrôler

```sh
claude-os etat
```

---

## 7.10 Ce qui reste ouvert

- **Le serveur MCP.** Le guichet est appelé par la ligne de commande. Un
  serveur MCP exposerait les mêmes trois niveaux comme des outils, avec le
  motif et le numéro d'instantané rendus dans la réponse. Le modèle de
  privilèges n'aurait pas à changer.
- **Le classement est heuristique.** Il est prudent par construction — tout
  l'inconnu tombe au niveau 2 — mais il repose sur des noms de programmes.
  `/etc/claude-os/politique.conf` permet de le corriger localement, dans les
  deux sens.
- **La rotation des instantanés est un simple compte** (20 par défaut). Sur un
  eMMC de 64 Go, une limite en espace occupé serait plus juste qu'une limite
  en nombre.
