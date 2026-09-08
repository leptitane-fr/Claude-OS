## Accès root sur Claude OS

Cette machine tourne sous Claude OS. Tu y disposes d'un accès root, par une
porte instrumentée. Elle est déjà en place : rien à installer, rien à demander.

### Comment l'utiliser

```sh
claude-os-root apt-get install -y tree
claude-os-root -c 'apt-get update && apt-get upgrade -y'      # avec un shell
claude-os-root --pourquoi "audio muet" journalctl -b -p err
```

Préfixe par `claude-os-root`, pas par `sudo` : `sudo` seul n'a pas de terminal
où réclamer un mot de passe et échouera sur « no tty present ».

### Les trois niveaux

| Niveau | Ce que c'est | Ce qui se passe |
|---|---|---|
| 1 — lecture | journaux, état des services, inventaire | exécution directe |
| 2 — écriture réversible | paquets, configuration, services | **instantané btrfs**, puis exécution |
| 3 — sensible | partitions, firmware, identifiants, secrets, réseau distant | une **fenêtre s'ouvre sur le bureau**, montre la commande, et attend le mot de passe |

Le guichet classe seul, et tu n'as rien à faire de particulier pour le
niveau 3 : la même commande suffit, la fenêtre s'ouvre toute seule. Pour
savoir d'avance où tombe une commande, sans rien exécuter :

```sh
claude-os-root --expliquer <commande>
```

### Ce qu'il ne faut pas faire

Ne cherche pas à contourner le niveau 3 : ni `sudo` direct, ni `su`, ni
découpage de la commande en morceaux plus discrets, ni modification de
`/etc/sudoers.d` ou de `/etc/claude-os/politique.conf` — qui sont eux-mêmes
de niveau 3. Si l'utilisateur n'est pas devant sa machine, la fenêtre ne peut
pas s'ouvrir : dis-lui quelle commande tu veux lancer et pourquoi, et
attends-le.

### Revenir en arrière

```sh
claude-os journal          # ce qui a été fait, et avec quel instantané
claude-os instantanes      # les instantanés disponibles
claude-os rollback <id>    # restaure, puis redémarre (demande confirmation)
claude-os etat             # ce qui est en place, et ce qui manque
```

### Deux habitudes utiles

- `--pourquoi "…"` sur les actions de niveau 2 : le motif part dans le journal
  à côté de la commande. C'est ce qui rend une trace relisible dans six mois.
- Après une opération lourde, cite l'identifiant de l'instantané dans ta
  réponse. L'utilisateur saura quoi annuler sans avoir à chercher.
