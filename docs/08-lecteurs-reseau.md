# Les lecteurs réseau

Écrit le 9 septembre 2026, et **vu fonctionner sur MADOO** contre un NAS
Western Digital *My Cloud Mirror* réel, à l'adresse `192.168.1.29`.

---

## Ce que c'est

Déclarer un serveur une fois, dans **Réglages › Lecteurs réseau**, puis le
connecter d'un clic depuis le volet latéral de **Fichiers**. Une fois
connecté, le partage est un **dossier ordinaire** : le terminal le voit,
Chromium le voit, ses boîtes « Enregistrer sous » le voient.

Quatre protocoles : SMB/CIFS, NFS, SFTP et WebDAV.

---

## Pourquoi des montages du noyau, et non gvfs

C'était la décision structurante, et elle a été prise sur mesure.

| | `gvfs-backends` | Montages du noyau |
|---|---|---|
| Paquets à installer | **43** | **19** (dont 3 pour SMB seul) |
| Ce que cela tire | MTP, gphoto2, iOS, codecs AV1/HEIF | les seuls assistants de montage |
| Qui voit le partage | **les applications GIO seulement** | **tout le système** |
| Privilèges au montage | aucun | niveau 2 du guichet, sans mot de passe |

Les 43 paquets de `gvfs-backends` amènent la prise en charge des appareils
photo et des téléphones, dont il n'est pas question ici. Mais ce n'est pas
l'argument principal : un montage gvfs vit dans `/run/user/1000/gvfs` et
n'existe vraiment que pour les programmes qui parlent GIO. Un montage du
noyau est un répertoire, et un répertoire n'a besoin d'être expliqué à
personne.

---

## Comment cela se range

```
~/.config/claude-os/lecteurs          la déclaration — RIEN DE SECRET
trousseau gnome-keyring               les mots de passe
/run/claude-os/reseau/<id>            le point de montage
/usr/local/bin/claude-os-lecteur      le seul à parler à mount, en root
```

Le fichier de configuration peut être lu, copié, versé dans un dépôt sans
précaution : il ne contient aucun mot de passe. Ceux-ci vivent dans le
trousseau, par `libsecret` — `gnome-keyring` tournait déjà sur cette machine
avec son composant `secrets`, la bibliothèque partagée était déjà installée,
et seul l'en-tête de compilation s'est ajouté.

Les points de montage sont sous **`/run`**, un tmpfs : un redémarrage efface
l'arborescence entière, et aucun répertoire mort ne s'accumule — ce que
`/mnt` ou `/media` ne garantissent pas.

---

## L'interface ne compose jamais de commande privilégiée

C'est la frontière de sécurité, et elle mérite d'être dite clairement.

`Fichiers` et `Réglages` ne disent que **deux choses** au guichet : un verbe
et un identifiant de lecteur.

```sh
claude-os-root claude-os-lecteur monter nas-videos
```

Tout le reste — protocole, adresse, options — est relu par
`claude-os-lecteur`, **en root**, depuis la configuration de l'utilisateur, et
validé avant d'atteindre `mount` :

- l'identifiant ne peut contenir que `[A-Za-z0-9._-]`, ce qui interdit à un
  `../` de faire sortir le point de montage de son arborescence ;
- l'adresse du serveur est refusée si elle contient autre chose qu'un nom
  d'hôte ou une adresse — **refusée, pas échappée** : refuser se vérifie,
  échapper se discute ;
- les options supplémentaires passent par une **liste blanche de clés**.
  Sans ce filtre, le champ « options » serait un canal direct vers `mount`,
  en root, depuis un simple fichier texte.

Le fichier INI est lu en `awk`, jamais évalué par le shell : une valeur qui
contiendrait `; rm -rf /` reste une chaîne de caractères.

**Le mot de passe ne passe jamais par la ligne de commande.** Il serait
visible dans `ps` par n'importe quel compte, et recopié tel quel dans
`/var/log/claude-os/actions.log`. Il arrive par un fichier d'identifiants
écrit dans `/run/user/1000` — un tmpfs privé — créé en `0600` **dès
l'ouverture** (`O_CREAT|O_NOFOLLOW`, et non un `chmod` après coup qui
laisserait une fenêtre), et effacé dès que `mount` l'a lu.

---

## Ce qui a été mesuré sur le NAS

| Constat | Valeur |
|---|---|
| Modèle | `WDMYCLOUDMIRROR`, groupe `WORKGROUP` |
| Dialecte SMB maximal | **2.1** — `vers=3.0` répond « Dialect not supported » |
| Partages annoncés | 8, dont **3 ouverts en invité** : `SauvegardePC`, `Movies`, `Vidéos` |
| NFS | v2 et v3 seulement, **aucun export publié** |
| SSH | port 22 fermé |

Le montage de `//192.168.1.29/Vidéos` a été fait, parcouru en lecture ET en
écriture, et démonté. Les accents traversent correctement (`Séries`), et le
contenu appartient bien à `stef` grâce à `uid=`/`gid=`.

**`vers=2.1` n'est pas un défaut, c'est une nécessité pour ce NAS.** Le
noyau tente les dialectes récents d'abord et échoue en `-95` sans jamais
redescendre. C'est à cela que sert le champ « Options » du panneau.

---

## Trois pièges payés

### 1. `g_file_query_exists` est synchrone

`naviguer()` vérifiait l'existence du dossier avant d'y aller. Sur un dossier
local cela ne coûte rien, et c'est passé inaperçu pendant des mois. Sur un
lecteur réseau dont le serveur est éteint, l'appel ne rend la main qu'au bout
du délai TCP — **fenêtre entièrement gelée, boutons compris**, c'est-à-dire
exactement au moment où l'on veut revenir en arrière.

Le contrôle est conservé, mais posé en asynchrone, et une demande plus
récente annule celle qui attendait.

### 2. Trois usages après libération, trouvés par un clic réel

`reconstruire()` relit le fichier : il **libère** `L->lecteurs` et le
remplace. Or `connecter_lecteur()` recevait un `Lecteur*` pointant dans ce
tableau, appelait `reconstruire()` pour afficher le sablier, puis se servait
du pointeur. `SIGSEGV` à chaque clic sur un lecteur non connecté.

Deux jumeaux ailleurs : dans `on_lecteur_fini`, qui cherchait l'entrée
*avant* de reconstruire, et dans `on_mdp_valide`, où `gtk_window_destroy`
détruit le bouton, donc sa fermeture, donc la structure dont on lisait encore
les champs.

**Aucun de ces trois n'apparaît à la compilation, ni à la lecture.** Les
trois ont été trouvés en cliquant pour de vrai, avec `coredumpctl` pour dire
où. La règle qui en sort, écrite dans le code : *dans `fichiers-lieux.c`,
tout ce qui vient de `L->lecteurs` meurt au prochain `reconstruire()`.*

### 3. `/etc/default/rpcbind` ne décide rien

`nfs-common` tire `rpcbind`, qui écoutait sur `0.0.0.0:111` et `[::]:111` en
permanence — l'exposition même que ce projet a retirée pour SSH le
8 septembre.

Poser `OPTIONS="-h 127.0.0.1"` dans `/etc/default/rpcbind` **n'y change
rien** : rpcbind est activé par socket, et c'est *systemd* qui ouvre les
ports, avant que le démon ne démarre. Mesuré : après modification et
redémarrage du service, `ss` montrait toujours `0.0.0.0:111`.

La correction est un `.socket.d/` qui **remet la liste à zéro** — une ligne
`ListenStream=` vide — avant de redéclarer les ports en boucle locale. Sans
cette remise à zéro, les nouvelles directives s'ajouteraient aux anciennes et
le port resterait ouvert, en silence.

Vérifié depuis l'adresse réseau de la machine : `192.168.1.30:111` répond
« connexion refusée », `127.0.0.1:111` répond.

Ce que cela coûte : le verrouillage NFSv3, qui demanderait au serveur de
rappeler le client. `claude-os-lecteur` impose donc `nolock` par défaut sur
NFS. Un portable en Wi-Fi n'a pas l'usage des verrous NFS ; le prix serait de
rouvrir le port.

---

## Ce qui n'est PAS établi

Par principe — le même que celui de [`docs/07`](07-journal-des-seances.md).

- **NFS, SFTP et WebDAV n'ont jamais été montés pour de vrai.** Le code est
  écrit et compilé, mais le NAS ne publie aucun export NFS, son port 22 est
  fermé, et aucun serveur WebDAV n'était joignable. Seul **SMB** a été vu
  fonctionner de bout en bout. Tant qu'un montage n'a pas eu lieu, ces trois
  chemins sont *plausibles*, pas *prouvés*.
- **La connexion automatique à l'ouverture de session** a été essayée en
  lançant `claude-os-lecteurs-auto` à la main — elle a bien connecté le
  lecteur. Elle n'a **pas** encore été vue s'exécuter depuis l'autostart de
  labwc lors d'une vraie ouverture de session.
- **Le bouton « Parcourir… »** du panneau de réglages n'a pas été cliqué à
  l'écran. Son analyseur a été corrigé sur la sortie réelle de
  `smbclient -L -g` du NAS (`Disk|Vidéos|`), mais le clic n'a pas eu lieu.
- **`mount.davfs` sans SUID** : la dérogation `dpkg-statoverride` est posée
  et le bit est retiré, mais aucun montage WebDAV n'a été tenté pour vérifier
  qu'il passe malgré tout par le guichet.

Une cause plausible n'est pas une cause, et un chemin de code compilé n'est
pas un chemin de code éprouvé.
