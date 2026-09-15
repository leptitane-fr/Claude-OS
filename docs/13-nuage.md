# Les lecteurs nuage

Écrit le 15 septembre 2026, et **vu fonctionner sur MADOO** contre un compte
Google Drive réel : monté, parcouru, écrit, démonté.

---

## Ce que c'est

Un compte connecté une fois, puis un **dossier ordinaire** dans le volet
latéral de **Fichiers**, section « Nuage ». Le terminal le voit, Chromium le
voit, ses boîtes « Enregistrer sous » le voient.

Deux fournisseurs prévus : **Google Drive** et **Microsoft OneDrive**. Au
15 septembre 2026, **seul Drive est en service** — voir « Ce qui n'est pas
établi ».

---

## Pourquoi rclone

Le report était inscrit depuis le 8 septembre (`docs/02`, `docs/04` point 11).
Mesuré avant de s'engager :

| | `gvfs-backends` | `rclone` |
|---|---|---|
| Paquets à installer | 43 | **1** |
| Dépendances | MTP, gphoto2, iOS, codecs | **`libc6` seule** |
| Poids installé | — | 61 Mo |
| Qui voit le partage | les applications GIO | **tout le système** |
| Privilèges au montage | aucun | **aucun** |

Le même raisonnement que pour les lecteurs réseau (`docs/08`), avec une
conclusion plus nette encore : rclone ne dépend de rien.

---

## Les identifiants OAuth — et le précédent qui obligeait à vérifier

Ce projet a déjà perdu la synchronisation Google de Chromium parce que
**Google a supprimé l'identifiant OAuth que Debian livrait** : interrogé, il
répond `deleted_client` (`README.md`, `docs/07`). Rien ne garantissait que
celui de rclone ait mieux vieilli. Il a donc été interrogé, avant tout
engagement :

| Identifiant livré par Debian | Réponse au 15 septembre 2026 |
|---|---|
| Google `202264815644.apps.googleusercontent.com` | **vivant** — écran de consentement d'une application **vérifiée**, sans avertissement « application non validée » |
| Microsoft `b15665d9-eda6-4092-8539-0eec376afd59` | **vivant** — page de connexion normale, aucun `AADSTS` |

Le `+dfsg` du paquet Debian **n'a pas retiré ces clés** : elles sont dans le
binaire, vérifiées par `strings`.

Ce que rclone demande à Google, lu dans la redirection réelle :

```
scope=https://www.googleapis.com/auth/drive    accès complet aux fichiers
access_type=offline                            jeton de rafraîchissement durable
```

**Le rafraîchissement a été éprouvé**, et non supposé : l'expiration a été
antidatée à 2020 dans la configuration, et rclone a renouvelé le jeton seul
puis réinscrit une échéance neuve. L'accès ne s'éteindra pas dans l'heure.

### Pourquoi pas une application « Claude OS »

Ce serait mieux : un seul écran de connexion par compte, au nom de la
distribution, avec des scopes couvrant aussi le courrier, l'agenda et les
contacts — de quoi servir les fonctionnalités à venir sans reposer d'invite.

**Côté Microsoft, c'est fermé pour un compte personnel.** Mesuré le
15 septembre 2026 : le portail Entra affiche, à la place du formulaire, une
erreur bloquante — « la possibilité de créer des applications hors d'un
répertoire a été déconseillée ». Les trois portes proposées ont chacune un
prix : créer un annuaire (Microsoft réclame un nom d'entreprise), s'inscrire
à Azure (vérification par carte bancaire), ou le programme développeur M365
(locataire de test qui expire — **à écarter** : fonder l'accès permanent d'un
système d'exploitation sur un locataire temporaire serait fragile).

**Côté Google, l'obstacle est différent** : les scopes restreints (Drive
complet, Gmail) exigent une vérification Google. Sans elle, l'application
reste en mode « Test » et, d'après la documentation Google, les jetons de
rafraîchissement expirent au bout de 7 jours. *Non mesuré ici* — le vérifier
demanderait d'attendre une semaine.

D'où le choix retenu avec l'utilisateur : **l'identifiant de rclone pour
Drive**, qui est vérifié par Google et dont les jetons durent. L'architecture
garde l'identifiant remplaçable par compte : le jour où une application
propre existe, c'est une ligne de configuration.

**Ce qui est écarté par principe** : se servir de l'application rclone pour
lire le courrier de l'utilisateur. Techniquement peut-être possible, mais ce
serait emprunter l'identité d'un tiers auprès de Google et de Microsoft.

---

## Comment cela se range

```
~/.config/claude-os/nuage             la déclaration — RIEN DE SECRET
~/.config/rclone/rclone.conf          les jetons, CHIFFRÉ
trousseau gnome-keyring               la phrase qui ouvre ce fichier
/run/user/1000/claude-os/nuage/<id>   le point de montage
/usr/local/bin/claude-os-nuage        monte et démonte — SANS privilège
/usr/bin/claude-os-nuage-phrase       rend la phrase au seul rclone
```

### Un jeton de rafraîchissement vaut un mot de passe

Il rouvre le compte indéfiniment, sans mot de passe et sans second facteur.
La règle du dépôt s'applique donc telle quelle — « le fichier de
configuration ne contient rien de secret » — et rclone garde ses jetons dans
**sa** configuration, qu'on chiffre.

La phrase vit au trousseau, et **ne passe ni par la ligne de commande**
(`ps` la montrerait à tout compte de la machine) **ni par l'environnement**
(`/proc/<pid>/environ`) : rclone exécute `claude-os-nuage-phrase` et lit sa
sortie. C'est le raisonnement déjà tenu pour le mot de passe des montages
CIFS, appliqué au même problème.

Mesures : le déchiffrement coûte **0,33 s** au total, dont 0,19 s pour la
seule lecture du trousseau en Python — d'où le choix du C, qui la ramène à
**0,020 s**. Sans la phrase, rclone refuse **explicitement** (« unable to
decrypt configuration ») : pas d'échec muet.

### Aucun privilège, et c'est la vraie différence avec `docs/08`

Monter du CIFS ou du NFS est une opération du noyau, donc réservée à root :
`claude-os-lecteur` passe par le guichet. rclone monte par **FUSE**, sous le
compte de l'utilisateur, dans un répertoire qui lui appartient déjà.
`claude-os-nuage` **refuse de tourner en root**, et le vérifie à sa première
ligne. Demander des droits dont on n'a pas l'usage est la meilleure façon de
s'habituer à les demander.

D'où le point de montage sous `$XDG_RUNTIME_DIR` et non sous `/run`.

---

## Ce qui a été mesuré

| Constat | Valeur |
|---|---|
| Compte | Google Drive, 5 Tio, 2,24 Gio utilisés |
| Racine | 492 entrées |
| Lecture, écriture, suppression | éprouvées sur le montage réel |
| Google Docs natifs | 290 exposés en `.docx`, contenu téléchargé (22 659 octets vérifiés) |
| Mémoire par montage | **53 à 55 Mo** |
| Connexion | **1 à 2 s** d'ordinaire ; **5 à 35 s** à la première après une pause |

**`--vfs-cache-mode writes` n'est pas un réglage de confort.** Sans lui, un
fichier ouvert en écriture non séquentielle est refusé — et c'est ainsi
qu'écrivent la moitié des applications, dont les boîtes « Enregistrer sous ».
Le nuage cesserait d'être un dossier ordinaire, ce qui est tout l'objet.

**`--drive-export-formats` non plus.** Sans lui, les Google Docs natifs
apparaissent dans la liste et n'ont aucun contenu téléchargeable.

**La lenteur de la première connexion n'est pas de notre fait** : le montage
direct a été chronométré à 18,6 s puis 1,6 s et 1,6 s, tandis que le montage
par `claude-os-nuage-auto` tombait à 1,1 s deux fois de suite. C'est rclone
qui est lent à rouvrir — jeton à rafraîchir, cache de répertoires vide — puis
rapide. Le montage se faisant en arrière-plan à l'ouverture de session, cela
ne retarde rien.

---

## Le quota partagé de rclone — la limite de l'identifiant par défaut

**Mesuré le 15 septembre 2026, après le premier usage réel.** Des listages de
dossiers de vingt entrées prenaient 17, 34 puis 52 secondes, sans rapport avec
leur taille ni avec le réseau, et *le même dossier* passait de 34,56 s à
1,36 s d'un essai à l'autre. Le journal détaillé donne la cause :

```
Error 403: Quota exceeded for quota metric 'Queries' and limit
'Requests per minute' of service 'drive.googleapis.com'
for consumer 'project_number:202264815644'
```

`202264815644` est **le projet de rclone**. Le quota Google est attaché à
l'application, donc **partagé par tous les utilisateurs de rclone dans le
monde** qui se servent de l'identifiant livré. Saturé, il fait attendre le
pacer en doublant à chaque tentative — 1,5 s, 2 s, 4,6 s…

C'est la limite, non mesurée au moment du choix, de la recommandation
« identifiant rclone pour Drive » retenue plus haut. Elle ne la condamne pas :
l'accès fonctionne, les jetons durent, et rien n'est à créer. Mais la
navigation reste à la merci de ce que font les autres.

**Deux remèdes, de portée inégale.**

*Palliatif, en place :* lisser le débit (`--tpslimit 10`) pour ne pas
déclencher la limite par rafales, et surtout ne plus relister sans cesse
(ci-dessous).

*Vrai remède, à faire :* un identifiant OAuth propre au compte, créé dans la
console Google Cloud — gratuite et **sans carte bancaire**, contrairement à
Azure. Le compte reçoit alors son propre quota. Attention au piège : laissée
en mode « Test », l'application voit ses jetons de rafraîchissement expirer au
bout de 7 jours ; il faut la publier en « Production », ce qui affiche un
écran « application non validée » mais rend les jetons durables.

---

## Le cache de répertoires — une faute et sa correction

`--dir-cache-time` valait **30 s**, et c'était une faute de réglage. Passé ce
délai, revenir dans le dossier parent qu'on venait de quitter le faisait
re-interroger entièrement. Symptôme rapporté par l'utilisateur dès le premier
usage : « un retour prend autant de temps que l'aller, rien n'est conservé ».
Et chaque relistage inutile rapprochait un peu plus du quota ci-dessus.

Un cache long ne périme rien, parce que `--poll-interval` interroge le service
pour les changements distants et invalide ce qu'il faut. On ne choisit donc
pas entre fraîcheur et vitesse.

Mesures avant / après, même machine, même compte :

| | avant (30 s) | après (1000 h + poll 1 min) |
|---|---|---|
| Retour au dossier parent | aussi long que l'aller | **0,01 s** |
| Même dossier, 105 s plus tard | relisté depuis le réseau | **0,01 s** |
| Fichier déposé par un autre appareil | — | **vu en 50 s**, sans démontage |

---

## Les aperçus ne se font plus sur un lecteur distant

Le premier usage réel a montré une lenteur que `ls` ne pouvait pas révéler :
le gestionnaire de fichiers fabrique des vignettes, donc il TÉLÉCHARGE.

Le garde-fou existant était un seuil de taille — 64 Mo — et son commentaire
disait déjà l'intention : « lire un fichier entier sur un lecteur réseau pour
en tirer 128 pixels n'a pas de sens ». Mais un seuil de taille ne servait pas
cette intention, parce que **ce qui coûte cher sur un lecteur distant n'est
pas la taille d'un fichier, c'est le nombre de fichiers**.

Mesure du 15 septembre 2026 sur la racine du Drive : 627 fichiers, dont
**173 éligibles à une vignette, pour 71 Mo à télécharger**. Fichier médian à
0,2 Mo — un seuil plus bas n'aurait donc rien changé au nombre. Ces
téléchargements partent en rafale, et c'est précisément ce qui fait saturer
le quota de l'API Google dont dépend toute la navigation.

Comparaison isolée des deux binaires — bus et `HOME` séparés, cache de
vignettes vierge, même dossier, même durée :

| | trafic en 35 s | vignettes |
|---|---|---|
| Avant | **24,5 Mo**, et le téléchargement continuait | 40 |
| Après | **2,1 Mo** | 0 |

Les dossiers **locaux gardent leurs aperçus** : la règle ne vise que les
chemins sous les racines de montage du nuage et du réseau, que `nuage.h` et
`reseau.h` exposent désormais — une seule source de vérité, plutôt que des
préfixes recopiés.

**Ce qui a été rectifié en cours de route :** il avait été affirmé que les
71 Mo étaient rechargés à chaque visite. C'est faux — le cache de vignettes
sur disque (spécification freedesktop, `~/.cache/thumbnails`) fait que le
prix n'est payé qu'une fois par fichier. Il est payé à la **première** visite,
celle où l'on attend, ce qui suffit à justifier la règle ; mais le chiffre
était inexact et l'utilisateur avait choisi sur sa foi.

**Et un piège de méthode, déjà consigné et repris quand même :**
`GtkApplication` est mono-instance. Le premier essai lançait le binaire neuf
alors qu'une instance installée tournait : celle-ci a reçu l'ouverture, le
binaire d'essai est sorti aussitôt, et **c'est l'ancien code qui a été
mesuré**. Le remède : `dbus-run-session` et un `HOME` séparé, qui isolent
l'essai sans toucher à la session de l'utilisateur.

---

## Quatre pièges payés

### 1. `--daemon` coûte une demi-minute pour rien

34 s contre 5 s, pour le même montage. Le parent y attend un signal de
disponibilité qu'il met longtemps à voir, alors que le montage est utilisable
depuis longtemps. `claude-os-nuage` détache donc lui-même et attend que le
montage **apparaisse** — la seule chose qui l'intéresse, et la seule qui se
constate.

### 2. rclone réclame sa phrase au clavier, et attend indéfiniment

Par défaut, rclone qui n'obtient pas sa phrase la demande sur l'entrée
standard. Lancé par l'autostart, il n'en a pas : ni montage, ni erreur, ni
ligne au journal. Un montage qui ne rendait jamais la main — la panne muette
de l'invariant n°4. **`--ask-password=false`** le fait écrire sa raison et
sortir.

### 3. Le nettoyage détruisait ce qu'il attendait

« Un montage qui échoue ne doit rien laisser derrière lui » : le `rmdir` de la
branche d'échec supprimait le point de montage **sous les pieds d'un rclone
encore en train de démarrer**, qui mourait alors sur `mountpoint does not
exist`. Une panne fabriquée par son remède. Le répertoire n'est plus
supprimé : il vit sous `$XDG_RUNTIME_DIR`, donc il part à la fermeture de
session, et l'état d'un lecteur se lit dans `findmnt`, jamais dans la
présence d'un répertoire.

### 4. La détection d'échec lisait le passé

`tail -5` sur un journal **cumulatif** attrapait l'erreur de la tentative
précédente et déclarait perdue une tentative qui démarrait à peine — un
montage « refusé en 0 s » sur une erreur vieille d'une minute. La position du
journal est désormais relevée **avant** le lancement, et seule la portion
ajoutée est lue.

### Et un piège de méthode, payé deux fois dans la même séance

`pkill -f <motif>` **et** `pgrep -f <motif> | kill` se prennent eux-mêmes pour
cible quand le motif figure dans la ligne de commande du script appelant. Le
dépôt le documentait déjà pour `pkill` (`docs/12`) ; cela vaut aussi pour
`pgrep` suivi d'un `kill`. Deux shells tués en séance. Le remède : un motif
qui ne peut pas se reconnaître, `"rclone .*mount [g]drive:"`.

---

## Les accents perdus — la cause était plus générale qu'on ne croyait

`docs/09` impute les « ? » du journal à l'absence de locale sous systemd. La
cause est plus large, et a été revue le 15 septembre 2026 : `fr_FR.utf8` est
bien générée et `LANG=fr_FR.UTF-8` est bien posé dans la session labwc, et
les accents se perdaient quand même.

**Un programme C n'hérite pas de la locale tout seul** : il faut
`setlocale()`, et seul `gtk_init()` l'appelle. Un programme **GIO pur** reste
donc en locale « C », et `g_print`/`g_printerr`, qui transcodent vers elle,
remplacent tous les accents par des « ? ». `fprintf` écrit les octets tels
quels.

**Conséquence à traiter, hors de ce chantier :**
`shell/src/lecteurs-auto.c` porte le même défaut (deux `g_print`) — les
messages de connexion des lecteurs réseau perdent leurs accents dans
`shell.log` depuis septembre.

---

## Ce qui n'est PAS établi

Par principe — le même que celui de `docs/07` et `docs/08`.

- **OneDrive n'a jamais été monté.** Le code le prévoit et le chemin est
  écrit, mais aucun compte Microsoft n'a pu être connecté : la création d'une
  application Azure est fermée aux comptes personnels (ci-dessus), et
  l'utilisateur a choisi de reporter. Tant qu'un montage n'a pas eu lieu, ce
  chemin est *plausible*, pas *prouvé*.
- **Le montage à l'ouverture de session n'a pas été vu depuis une vraie
  ouverture de session.** Il a été éprouvé en simulant l'environnement de
  l'autostart (`env -i`, PATH nu, bus de session) — connecté en 34 s,
  accents corrects — mais pas encore après une fermeture et une réouverture
  réelles. Le trousseau, notamment, est déverrouillé par PAM : rien ne
  garantit encore qu'il ait fini quand l'autostart démarre.
- **Le clic n'a pas été fait à l'écran.** La section « Nuage » a été vue dans
  le volet, avec Google Drive et son bouton de déconnexion, et le programme a
  tourné sous AddressSanitizer sans un signalement. Mais naviguer, se
  déconnecter et se reconnecter *au doigt* reste à faire.
- **Aucun panneau de réglages.** Les lecteurs se déclarent à la main dans
  `~/.config/claude-os/nuage` ; il n'y a ni « Ajouter un compte » ni écran de
  connexion intégré. La connexion OAuth a été faite en ligne de commande.
- **Les icônes n'existent pas.** `claude-os-nuage-drive-symbolic` et son
  équivalent OneDrive sont demandés par le code et absents du thème : GTK
  descend silencieusement sur les replis. `tools/fabrique-icones.py` reste à
  compléter — c'est exactement le piège que `CLAUDE.md` décrit.

Une cause plausible n'est pas une cause, et un chemin de code compilé n'est
pas un chemin de code éprouvé.
