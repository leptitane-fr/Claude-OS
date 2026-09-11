# Le code PIN de l'écran de connexion

Écrit le 9 septembre 2026. Ce document dit **ce qui est en place**, **ce qui a
été mesuré**, et — section « Ce que cela coûte » — **ce que ce confort retire
à la sécurité de la machine**. Cette dernière partie n'est pas une formalité :
elle contient la seule raison qu'on pourrait avoir de revenir en arrière.

---

## Ce que ça fait

À la première ouverture, l'écran demande un nom d'utilisateur puis un mot de
passe. Une fois PAM satisfait, il propose de choisir un **code à six
chiffres**, saisi deux fois. Les ouvertures suivantes se font au code, au
doigt sur le pavé numérique ou au clavier physique — les deux marchent, tout
le temps.

Le mot de passe reste accessible depuis **tous** les volets. Le code PIN se
retire depuis l'écran lui-même (« Ne plus utiliser de code PIN »).

---

## Pourquoi le PIN déverrouille le mot de passe au lieu de le remplacer

C'est la contrainte qui a décidé de toute l'architecture, et elle tient en
une ligne de `/etc/pam.d/greetd` :

```
-auth        optional        pam_gnome_keyring.so
```

**PAM doit recevoir le vrai mot de passe.** C'est lui qui déverrouille le
trousseau de session, où sont rangés les mots de passe des lecteurs réseau
(voir [`08`](08-lecteurs-reseau.md)). Un module PAM maison qui validerait le
code PIN authentifierait l'utilisateur et laisserait le trousseau fermé : la
session s'ouvrirait, et les partages deviendraient inaccessibles sans le
moindre message. C'est le genre de panne qu'on cherche trois séances.

Le code PIN n'est donc pas un mot de passe de rechange. C'est une **clé de
coffre**, et le mot de passe est dedans. Rien ne change en aval de
`create_session` : greetd reçoit exactement ce qu'il recevrait d'une frappe
au clavier.

---

## Les pièces

| Pièce | Rôle |
|---|---|
| `/usr/bin/claude-os-coffre` | Le service privilégié. Scelle, ouvre, efface. Sert aussi le nom du thème. |
| `claude-os-coffre.socket` | `/run/claude-os/coffre.sock`, **0600 `_greetd`**, `Accept=yes`, `MaxConnections=4`. |
| `claude-os-coffre@.service` | Une instance par connexion, en root, durcie. Lit une requête, répond, meurt. |
| `/var/lib/claude-os/coffre/<user>.pin` | Le mot de passe scellé. **0600 root:root.** |
| `/var/lib/claude-os/coffre/<user>.essais` | Le compteur d'essais ratés. |
| `shell/src/connexion.c` | Les quatre volets et leur enchaînement. |
| `shell/src/clavier.c` | Le pavé numérique et le clavier azerty à l'écran. |

### Ce qui protège le coffre

**La socket, et rien d'autre.** Pas de setuid, pas de `sudoers`, aucun chemin
d'exécution depuis un compte ordinaire. Le programme **redit** la règle de son
côté par `SO_PEERCRED` : une unité systemd remplacée par une mise à jour ou
par une erreur de déploiement ne suffit pas à l'ouvrir à n'importe qui.

`MaxConnections=4` n'est pas décoratif : chaque instance alloue 128 Mio pour
Argon2id — **129,5 Mio de pic mesurés dans le journal**. Avec la valeur par
défaut de systemd, 64, un `_greetd` compromis ferait réserver 8 Go sur une
machine qui en a 4.

### Le protocole

Une requête JSON par ligne, une réponse, puis fermeture.

| Verbe | Entrée | Réponse |
|---|---|---|
| `etat` | `utilisateur` | `{"pin":true,"essais_restants":5,"theme":"sombre"}` |
| `ouvrir` | `utilisateur`, `pin` | `{"mdp":"…"}` ou `{"erreur":"pin_faux","essais_restants":3}` |
| `enroler` | `utilisateur`, `mdp`, `pin` | `{"ok":true}` |
| `oublier` | `utilisateur` | `{"ok":true}` |

**Le coffre ne vérifie aucun mot de passe.** `enroler` scelle ce qu'on lui
donne. C'est délibéré : y mettre PAM recréerait le programme privilégié que
`connexion.c` s'est donné pour règle de ne pas être. L'écran n'appelle
`enroler` qu'après un `success` de greetd, donc après que PAM a validé.

### Le format du fichier scellé

```
[coffre]
version=1
kdf=argon2id
m=131072
t=3
p=1
sel=<base64, 16 o>
nonce=<base64, 12 o>
scelle=<base64, mot de passe chiffré + tag GCM de 16 o>
```

Argon2id (clé de 32 o) puis **AES-256-GCM**. Les paramètres sont **relus du
fichier** et non pris dans les constantes, pour qu'un coffre scellé avant un
changement de réglage continue de s'ouvrir.

Ce qui est authentifié sans être chiffré (l'AAD) :

```
claude-os-coffre-v1|<compte>|argon2id|m=<m>|t=<t>|p=<p>
```

Le compte, pour qu'un coffre recopié d'un utilisateur à l'autre ne s'ouvre
pas. Les paramètres, pour qu'on ne puisse pas **abaisser le coût d'Argon2
dans le fichier** — `m=8`, `t=1` — et rendre l'attaque hors ligne mille fois
plus rapide. Modifier l'un ou l'autre casse le tag, et le coffre refuse.

---

## Les mesures

### Argon2id sur MADOO — Pentium Silver N6000 à 1,10 GHz

Meilleur de trois passes, `p=1`, clé de 32 octets :

| t | m | durée |
|---|---|---|
| 2 | 32 Mio | 0,105 s |
| 3 | 32 Mio | 0,155 s |
| 2 | 64 Mio | 0,215 s |
| 3 | 64 Mio | 0,312 s |
| 4 | 64 Mio | 0,409 s |
| **3** | **128 Mio** | **0,631 s** ← retenu |

**Retenu : m=131072 Kio, t=3, p=1.** Aller-retour complet mesuré par la vraie
socket, sous `_greetd`, démarrage du processus et JSON compris : **0,75 à
0,79 s**.

`p=1` et non 4 : `gcry_kdf_compute(h, NULL)` déroule les voies **en série**.
Un `p` plus grand multiplierait l'attente sans rien coûter de plus à
l'attaquant, qui, lui, paralléliserait.

### Le piège libgcrypt, à ne pas repayer

`gcry_kdf_derive()` — la fonction évidente, celle dont le nom dit qu'elle
dérive une clé — **ne sait pas faire Argon2**. Elle rend « Invalid value »
sans plus. Argon2 n'existe que dans l'API à poignée
(`gcry_kdf_open`/`compute`/`final`), et l'ordre de ses quatre paramètres
n'est documenté nulle part dans `gcrypt.h`.

Cet ordre — **`{taglen, t, m, p}`** — a été **établi et non supposé** : les
quatre ordres plausibles ont été essayés contre le vecteur de test Argon2id
de la **RFC 9106 §5.3**, et un seul le reproduit octet pour octet.

---

## Ce que cela coûte

**À lire avant de se féliciter du reste.**

Un code à six chiffres n'a qu'**un million de combinaisons**. Le compteur
d'essais — cinq, puis le coffre s'efface — rend l'attaque *en ligne*
impossible. Il ne peut rien contre l'attaque *hors ligne*.

Or **le disque de cette machine n'est pas chiffré** : `mmcblk1p2` est un ext4
nu, il n'y a pas de `/etc/crypttab`. Qui démonte l'eMMC obtient
`/var/lib/claude-os/coffre/stef.pin` et peut l'attaquer à loisir.

Le coût de cette attaque, **calculé à partir de la mesure ci-dessus** :

| Moyens | Durée pour épuiser 10⁶ codes |
|---|---|
| Un cœur de N6000 | 10⁶ × 0,63 s ≈ **7,3 jours** |
| Les 4 cœurs | ≈ **1,8 jour** |
| Un processeur de bureau récent, 16 cœurs | de l'ordre de quelques heures |

**Une carte graphique irait plus vite, et ce facteur-là n'a PAS été mesuré.**
Argon2id à 128 Mio est limité par la bande passante mémoire, ce qui est
précisément ce qui gêne un GPU — c'est la raison d'être de ce paramètre — mais
écrire un chiffre ici serait inventer. Le tenir pour « quelques heures » est
l'hypothèse prudente.

**À comparer avec ce qu'on avait avant.** Sans code PIN, le même attaquant ne
trouvait que le hachage yescrypt de `/etc/shadow` : sans dictionnaire qui
contienne le mot de passe, il n'en tire rien. **Le code PIN abaisse donc la
sécurité de la machine au repos.** Il l'améliore à l'usage — on tape six
chiffres au lieu d'exposer un mot de passe complet dans un lieu public, et on
le tape moins souvent.

C'est un arbitrage, il est assumé, et il se défait d'un clic — « Ne plus
utiliser de code PIN ». **Le seul vrai remède est le chiffrement du disque**,
et c'est un chantier à lui seul.

---

## Ce qui garantit qu'on ne s'enferme pas dehors

Cinq garde-fous, tous dans le code, aucun dans la procédure :

1. **Le mot de passe reste atteignable depuis tous les volets.** Aucun chemin
   ne mène à un écran qui n'accepterait que six chiffres.
2. **Le code PIN est facultatif** : « Plus tard » laisse l'écran tel qu'il
   était, « Ne plus utiliser de code PIN » y revient.
3. **Coffre absent, muet ou en panne = volet mot de passe**, avec la raison
   écrite dans `/var/log/claude-os-connexion.log`. Le délai est de 15 s.
4. **L'enrôlement ne peut pas faire échouer l'ouverture de session.** Un
   coffre en panne au moment de sceller laisse partir la session sans code
   PIN, et le journal dit pourquoi. Aucun PIN n'étant alors enregistré,
   l'écran en repropose un à l'ouverture suivante : **la panne se rattrape
   d'elle-même.**
5. **Ce qui vient d'être scellé est relu aussitôt.** Un scellement
   silencieusement faux ne se verrait qu'au réveil suivant, sous la forme
   d'un « mot de passe incorrect » sur un code juste — impossible à rattacher
   à sa cause des heures plus tard. Le contrôle coûte 0,7 s, une fois, pendant
   que l'utilisateur est encore devant l'écran ; s'il échoue, le code est
   retiré plutôt que laissé inutilisable.

---

## Le clavier à l'écran

La machine est un convertible à écran tactile Goodix (`GDIX0000:00 27C6:0E88`)
et porte un `Tablet Mode Switch`. **Capot retourné, le clavier physique ne
répond plus** : jusqu'ici, dans cette position, ouvrir une session était tout
simplement impossible.

> **Corrigé le 11 septembre 2026** (voir [`docs/12`](12-mode-tablette.md)).
> Ce paragraphe disait « l'EC coupe le clavier ». Mesuré : l'EC le laisse
> passer — les frappes arrivent à evdev écran replié — et c'est **libinput**
> qui les écarte en voyant le commutateur. L'effet est le même ; la cause
> compte le jour où l'on chercherait pourquoi une frappe passe.

Deux claviers, dans `shell/src/clavier.c` : un pavé numérique pour le code, un
azerty complet — deux couches, lettres et symboles — pour le nom et le mot de
passe. Azerty parce que `/etc/xdg/labwc-greeter/environment` pose déjà
`XKB_DEFAULT_LAYOUT=fr` : les deux saisies doivent montrer les mêmes lettres
aux mêmes endroits.

**Pas de clavier virtuel Wayland.** squeekboard et wvkbd passent par
`zwp_virtual_keyboard_v1`, et demanderaient un second processus — donc une
seconde surface — **avant authentification**. Ce sont des boutons GTK dans la
même fenêtre.

> **Corrigé le 11 septembre 2026.** Ce paragraphe ajoutait que labwc
> n'expose pas `zwp_virtual_keyboard_v1`. C'est faux : une sonde des globaux
> Wayland le trouve dans labwc 0.8.3, avec `zwp_input_method_manager_v2` et
> `zwp_text_input_manager_v3`. Le choix reste bon pour l'écran de connexion —
> la raison du second processus avant authentification suffit —, mais la
> session, elle, peut avoir un vrai clavier virtuel : voir `docs/12`.

### Le piège qui tue les deux claviers, et il tient en deux lignes

Un `GtkButton` prend le focus quand on le presse. Sur cet écran, le focus
appartient au champ de saisie : c'est lui qui reçoit la frappe **physique**.
Sans précaution, le premier appui sur une touche à l'écran le lui vole, et la
frappe physique cesse de fonctionner — silencieusement, et seulement après un
clic, donc **jamais au premier essai**.

D'où, sur **toutes** les touches des deux claviers :

```c
gtk_widget_set_can_focus (b, FALSE);
gtk_widget_set_focus_on_click (b, FALSE);
```

Il faut les deux. La première seule laisse encore GTK déplacer le focus au
clic dans certains conteneurs.

---

## Le thème — la panne qui était là depuis le début

`connexion.c` avait déjà du code pour lire le thème du bureau. **Il ne pouvait
pas fonctionner**, et personne ne l'avait vu :

- `theme_de()` lisait `~/.config/claude-os/shell.conf`, mais `/home/stef` est
  en `drwx------` et le greeter tourne sous `_greetd`.
  `g_key_file_load_from_file` échouait à chaque ouverture, **sans un mot**, et
  le repli s'appliquait toujours ;
- ce repli était `claude-sombre`, un thème que la machine n'utilise pas — le
  sien est `sombre` ;
- `cfg->theme` était écrasé **sans repasser par `theme_par_id()`**, donc
  `cfg->dark` restait à `FALSE` quoi qu'il arrive, et les widgets natifs de
  GTK se dessinaient clairs sur fond sombre.

Trois défaillances muettes empilées : l'invariant n°4 en toutes lettres.

Corrigé de trois façons. Le coffre lit `shell.conf` **en root** et le sert au
greeter — `shell.conf` reste la source unique de vérité, aucun fichier miroir
à tenir. `shell_config_set_theme()` pose `theme` **et** `dark` ensemble : un
champ écrit à la main est un invariant qu'on oublie, une fonction non. Et
**tout repli est désormais écrit** dans `/var/log/claude-os-connexion.log`.

Le coffre ne renvoie qu'un identifiant pris dans une **liste blanche** de six.
C'est cela — et non le chemin — qui garantit qu'un root lisant un fichier du
répertoire personnel ne peut rien faire remonter d'arbitraire vers `_greetd`.

---

## Éprouver, et dépanner

### Le coffre seul, sans écran de connexion

Il s'active par socket : lancé à la main, il refuse et le dit. Pour lui
parler comme le fait le greeter :

```sh
sudo runuser -u _greetd -- python3 - <<'PY'
import json, socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(20)
s.connect("/run/claude-os/coffre.sock")
s.sendall(json.dumps({"verbe":"etat","utilisateur":"stef"}).encode() + b"\n")
print(s.recv(65536).decode())
PY
```

### L'écran, sans toucher à la machine

```sh
claude-os-connexion --apercu --volet=pin --theme=clair
```

**Ctrl-Q pour en sortir, et il faut le savoir avant de le lancer.** L'aperçu
s'ancre sur les quatre bords en couche OVERLAY et prend le clavier en mode
EXCLUSIVE, comme le vrai écran : lancé depuis une session ouverte, il
recouvre le bureau et capte chaque touche. Ni Alt-Tab ni Alt-F4 n'en sortent.
Ctrl-Q n'existe **qu'en aperçu** — le vrai écran de connexion n'a aucun
raccourci de sortie, et n'en aura jamais.

`--volet=` vaut `nom`, `mdp`, `pin` ou `choix` ; `--theme=` prend les quatre
thèmes. Le coffre n'est **pas** sollicité en aperçu : rien n'est écrit dans
`/var/lib`. Le code d'aperçu est `123456`.

Le banc d'essai headless produit une capture sans matériel graphique :

```sh
./shell/test-render.sh "claude-os-connexion --apercu --volet=choix" rendu.png 1366 768
```

### Le vrai écran, sur un terminal virtuel libre

```sh
sudo bash install/bascule-session.sh --essai
```

### Retirer un code PIN sans passer par l'écran

```sh
sudo rm -f /var/lib/claude-os/coffre/stef.pin /var/lib/claude-os/coffre/stef.essais
```

L'écran repassera de lui-même au nom + mot de passe.

### Où lire quand ça ne va pas

| Symptôme | Où regarder |
|---|---|
| Le code PIN n'est pas proposé, ou le thème est faux | `journalctl -u 'claude-os-coffre@*' -b` |
| L'écran reste sur le mot de passe sans raison visible | `/var/log/claude-os-connexion.log` |
| La socket n'existe pas | `systemctl status claude-os-coffre.socket` |

---

## Confirmé à l'écran le 9 septembre 2026

`--essai` sur le terminal virtuel 8, écran regardé pendant les trente
secondes. Ce qui a été **vu**, et non déduit :

- le volet **nom d'utilisateur** s'ouvre — comportement attendu, aucun code
  PIN n'étant enregistré ;
- **le fond et la carte sont sombres**, comme le bureau. C'est la première
  fois : jusqu'ici l'écran restait sur son repli quoi qu'il arrive ;
- **le clavier azerty s'affiche**, ses cinq rangées complètes ;
- le champ porte « Nom d'utilisateur » et le bouton « Suivant ».

Puis, lors d'un second essai de 90 secondes, **au doigt sur l'écran** :

- les touches du clavier azerty **écrivent dans le champ** ;
- **le clavier physique écrit toujours après**, ce qui établit que le piège
  du focus est évité — c'est le seul point que le banc d'essai headless ne
  pouvait pas atteindre, faute de savoir cliquer ;
- **⇧** passe les lettres en majuscules et s'allume, **&#** bascule sur la
  couche des symboles.

Et ce que le journal établit de son côté :

```
=== 2026-09-09 16:04:48 — lancement de l'écran de connexion ===
compte du processus : _greetd (uid 102)
```
```
16:04:48  Started claude-os-coffre@7-10558-102.service (PID 10558/UID 102)
```

**Le greeter a joint le coffre depuis `_greetd`, sur le vrai écran.** Le PID
10558 est celui du greeter ; c'est lui qui a ouvert la connexion. Aucun
« coffre injoignable » dans `/var/log/claude-os-connexion.log` : la lecture du
thème a donc réussi par ce chemin-là, celui qui échouait en silence depuis le
début.

Le `code de retour 1` et le « Lost connection to Wayland compositor » de fin
de journal sont normaux : c'est l'essai qui arrête le compositeur au bout des
trente secondes.

---

## Ce qui n'est PAS établi

Par honnêteté, et parce que ce dépôt s'est déjà trompé en annonçant des
causes avec assurance :

- **Aucune session n'a encore été ouverte, ni au mot de passe ni au code
  PIN, avec ce greeter.** L'écran s'affiche et joint le coffre — c'est
  confirmé ci-dessus — mais le chemin qui va d'un mot de passe accepté à
  l'enrôlement puis au démarrage de la session n'a jamais été parcouru en
  entier. C'est la vérification qui reste, et elle demande une vraie
  ouverture de session.
- **Le trousseau n'a pas été vérifié après une ouverture au code PIN.** C'est
  pourtant le point qui a dicté toute l'architecture. Il doit être constaté
  pour de vrai — un lecteur réseau connecté après une ouverture au code —
  et non déduit.
- **Le mode tablette n'a pas été essayé.** C'est le cas d'usage qui justifie
  le clavier azerty à l'écran ; il n'a jamais été mis à l'épreuve.
- **Le coût d'une attaque par carte graphique n'a pas été mesuré.** Voir
  « Ce que cela coûte ».
