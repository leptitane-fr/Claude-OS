# Clones de Claude Desktop

*Écrit le 10 septembre 2026. Deux instances vues tourner en parallèle sur
MADOO le jour même.*

---

## Le problème

Claude Desktop ne connaît qu'un compte. Tout son état vit dans un répertoire
unique, `~/.config/Claude` : cookie de session, base locale, sessions Claude
Code, arbres de travail git. Se connecter avec un second compte écrase le
premier.

Ce qu'on veut ici n'est pas un sélecteur de compte, mais **deux instances
ouvertes en même temps** — un compte par fenêtre, chacune sur ses projets.

## Le levier

Claude Desktop est une application Electron, donc Chromium. Elle accepte
`--user-data-dir`. Le verrou d'instance vivant lui-même dans le profil, deux
profils différents donnent deux instances qui coexistent sans se voir.

Claude Code, lui, suit `CLAUDE_CONFIG_DIR` — l'application l'honore
explicitement. **Un clone = ces deux répertoires.** Rien d'autre à détourner :
pas de correctif dans l'application, pas de paquet à recompiler.

**Mesuré le 10 septembre 2026** : PID 19095 sur `~/.config/Claude` et
PID 20560 sur le profil du clone, tournant côte à côte, chacun avec son écran
de connexion.

### Une piste écartée

L'application lit aussi `CLAUDE_USER_DATA_DIR`, ce qui semblait plus direct.
C'est un piège : en build packagé, elle **efface** cette variable de son propre
environnement au démarrage, sauf jeton de débogage CDP valide. Elle est
réservée à ses essais internes. `--user-data-dir` est la voie supportée.

## Rien de l'existant n'est modifié

C'est le principe de l'outil, et ce qui le distingue d'un gestionnaire de
comptes : l'installation d'origine garde son lanceur, son profil, son entrée
de menu, son autostart. **Un clone s'ajoute à côté, il ne s'interpose nulle
part.** Retirer `claude-os-clone` ne demande donc rien à défaire.

## Ce qui est partagé, ce qui ne l'est pas

| | |
|---|---|
| **Cloisonné par clone** | session, cookies, base locale, sessions Claude Code, projets, réglages de l'application |
| **Copié à la création** | `claude_desktop_config.json` — la déclaration des serveurs MCP est de l'outillage, pas du compte. Copie et non lien : un clone doit pouvoir diverger. |
| **Partagé par lien** | `~/.claude/CLAUDE.md` — les consignes globales valent pour la machine, pas pour un compte, et deux copies divergeraient en silence |

### Ce que le cloisonnement ne couvre pas

Il sépare l'état de l'application, **pas les fichiers sur lesquels on
travaille**. Deux clones ouverts sur le même dépôt écrivent dans le même arbre
de travail git, le même `~/.local`, les mêmes fichiers de projet. C'est le
prix du fonctionnement simultané, et il est assumé : à l'usage, on donne des
projets distincts à des clones distincts.

## Le trousseau : un défaut de la machine, pas du clonage

Au premier lancement d'un clone, l'application affiche : « Votre connexion ne
sera pas enregistrée sur cet appareil. Installez et déverrouillez un trousseau
système. »

**Le journal de l'installation d'origine porte le même avertissement, à chaque
démarrage** — vérifié le 10 septembre 2026 :

```
[safeStorage] isEncryptionAvailable=false on linux at startup
(backend=basic_text) — session will not persist
```

Le clone ne fait que rendre visible un défaut déjà là. Il affiche l'infobulle
parce que c'est son premier lancement ; l'origine la garde silencieuse depuis
que `insecureStorageNoticeShown` est posé dans son `config.json`.

### La cause

Chromium choisit son magasin de secrets d'après `XDG_CURRENT_DESKTOP`. Ici
c'est `labwc`, qui ne fait partie d'aucun bureau qu'il reconnaît : il retombe
sur `basic_text`, le stockage en clair. Electron considère alors que le
chiffrement n'est pas disponible, et l'application refuse d'écrire son jeton
de session.

Le trousseau est pourtant là : `gnome-keyring` tourne avec son composant
`secrets`, `org.freedesktop.secrets` est sur le bus de session, et
`pam_gnome_keyring` le déverrouille à l'ouverture de session par greetd.
Personne ne le lui a désigné.

### Le correctif

`--password-store=gnome-libsecret`, passé au lancement de chaque clone.
**Mesuré** : l'avertissement disparaît du journal, la session est chiffrée par
le trousseau.

La session web, elle, tenait déjà sans cela — les cookies survivent au
redémarrage même en `basic_text`. Ce qui ne tenait pas, ce sont les jetons
OAuth de l'application (`[oauth-v2] safeStorage not available, tokens will not
persist`).

### L'origine aussi

Le défaut n'ayant rien à voir avec le clonage, le correctif a été porté au
lanceur `claude-os-claude` lui-même, sur décision du 10 septembre 2026. Le
lanceur n'écrase pas le choix d'un appelant qui désigne déjà un magasin —
`claude-os-clone` garde donc le sien.

Trois chemins ouvraient Claude, et deux contournaient le lanceur. Les trois
passent désormais par lui :

| Chemin | Avant | Après |
|---|---|---|
| Entrée de menu | déjà le lanceur | inchangé |
| Démarrage de session | binaire nu, `--startup` | lanceur, `--startup` conservé |
| Liens `claude://` | `com.anthropic.Claude.desktop`, binaire nu | `claude-desktop.desktop`, qui porte `MimeType` |

**Conséquence à connaître** : changer de magasin de secrets peut demander une
reconnexion, une fois, au prochain lancement de l'origine. Les cookies écrits
en `basic_text` restent lisibles, mais rien ne le garantit pour les secrets
applicatifs.

## Un piège, trouvé au banc d'essai

La première version détectait les instances en découpant `/proc/<pid>/cmdline`
sur les NUL, la structure normale d'un cmdline. **Elle ne trouvait aucun
drapeau** : l'application réécrit son titre de processus au démarrage et les
séparateurs disparaissent — toute la ligne devient un seul champ.

Le défaut était silencieux et trompeur : faute de trouver `--user-data-dir`,
le code concluait « pas de profil, donc l'installation d'origine », et
attribuait chaque clone à l'origine. `liste` annonçait « arrêté » pour un
clone bien vivant. La détection découpe désormais à l'espace.

## Usage

```sh
claude-os-clone creer boulot "Travail"   # profil vierge
claude-os-clone lancer boulot            # s'ouvre à côté du reste
claude-os-clone liste                    # l'origine, les clones, qui tourne
claude-os-clone arreter boulot           # SIGTERM, fermeture propre
claude-os-clone oublier boulot           # déconnexion : réauthentification
claude-os-clone supprimer boulot
claude-os-clone integrer                 # régénère les entrées de menu
```

Chaque clone reçoit son entrée dans le menu du bureau : « Claude — Travail ».

Les identifiants restent dans le profil du clone : son ouverture suivante est
automatique, sans saisie. `oublier` est là pour la déconnexion franche.

Relancer un clone déjà ouvert ne le duplique pas — Electron passe la main à
l'instance en place, qui remonte sa fenêtre. C'est ce qu'on attend d'un second
clic sur l'entrée de menu.

L'arrêt se fait par `SIGTERM`, que l'application intercepte pour fermer
proprement. `SIGKILL` n'arrive qu'après quarante secondes d'attente.

## Emplacements

| | |
|---|---|
| Outil | `/usr/local/bin/claude-os-clone` |
| Profils | `~/.local/share/claude-os/clones/<nom>/{donnees,cli}` |
| Entrées de menu | `~/.local/share/applications/claude-os-clone-*.desktop` |

Source dans `rootfs/usr/local/bin/`.

## Ce qui reste à confirmer

- **Une connexion réelle sur un second compte.** Le clone a été vu s'ouvrir
  sur son écran de connexion ; personne ne s'y est encore authentifié, donc le
  retour automatique au lancement suivant n'est pas établi.
- **Le coût en mémoire.** 4 Go soudés, et une instance Electron en occupe déjà
  le quart. Deux instances tiennent-elles à l'usage réel — avec des sessions
  Claude Code ouvertes des deux côtés — reste à mesurer.
- **Le groupement dans le dock.** Tous les clones partagent le même `WM_CLASS`
  (`com.anthropic.Claude`) : les fenêtres se rangeront sous une seule icône.
