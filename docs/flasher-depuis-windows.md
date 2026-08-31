# Écrire Claude-OS sur la clé depuis Windows

Aucun Linux n'est nécessaire. Tu télécharges une image, tu l'écris sur la
clé, tu démarres dessus.

## 1. Télécharger

Depuis l'onglet **Releases** du dépôt :

- `claude-os.img.xz` — l'image compressée, environ **840 Mo**
- `claude-os.img.xz.sha256` — son empreinte

Une fois écrite, elle occupe 11 Gio sur la clé, puis la partition racine
s'étend au premier démarrage jusqu'à environ 170 Go.

## 2. Vérifier l'empreinte

Dans PowerShell, depuis le dossier de téléchargement :

```powershell
Get-FileHash claude-os.img.xz -Algorithm SHA256 | Format-List
Get-Content claude-os.img.xz.sha256
```

Les deux valeurs doivent être identiques. Si elles diffèrent, le
téléchargement est corrompu — recommence, n'écris pas l'image.

## 3. Écrire sur la clé

> **Cette opération efface intégralement la clé**, et une erreur de lecteur
> efface le mauvais disque. Vérifie la lettre et la taille avant de valider.
> Débranche tout autre périphérique USB de stockage pendant l'opération.

### balenaEtcher — le plus simple

[balena.io/etcher](https://etcher.balena.io/) — gratuit, lit le `.xz`
directement, et **relit la clé après écriture pour vérifier**. C'est cette
vérification qui en fait le meilleur choix : une clé USB qui écrit mal en
silence est une panne pénible à diagnostiquer.

1. *Flash from file* → `claude-os.img.xz`
2. *Select target* → la clé PNY de 256 Go (vérifie la taille)
3. *Flash!*

### Raspberry Pi Imager — alternative

[raspberrypi.com/software](https://www.raspberrypi.com/software/) — gère
aussi le `.img.xz` nativement. Choisir *Use custom image*.

### Rufus — si tu y tiens

Rufus ne décompresse pas toujours le `.xz` : extraire d'abord
`claude-os.img` avec [7-Zip](https://www.7-zip.org/), puis choisir le mode
**« Image DD »** (pas « ISO »). Rufus ne vérifie pas l'écriture.

## 4. Ce que Windows va afficher après l'écriture

Trois réactions normales, à ne pas confondre avec une erreur :

- **« Vous devez formater le disque avant de pouvoir l'utiliser »** →
  **Annuler**. Windows ne sait pas lire l'ext4 : la partition est saine, il
  ne la comprend pas.
- **Une petite partition d'environ 512 Mo apparaît** → c'est la partition
  de démarrage EFI, en FAT32. Ne rien y écrire.
- **La clé semble faire beaucoup moins que 256 Go** → normal. L'image fait
  11 Gio ; la partition racine s'étendra d'elle-même au premier démarrage.

## 5. Démarrer

1. Laisser la clé branchée, redémarrer.
2. Dès l'apparition du logo ASUS, marteler **Échap** (ou **F8**).
3. Choisir la clé dans le menu de démarrage.

**Ne changer aucun réglage du BIOS** — ni Secure Boot, ni le mode de
stockage, ni l'ordre de démarrage. L'image est signée pour démarrer avec
Secure Boot actif, et rien n'a été écrit dans la mémoire du firmware :
clé retirée, ton PC démarre sur Windows exactement comme avant.

### Si la clé n'apparaît pas dans le menu

La cause quasi systématique est **Fast Boot**, qui abrège l'énumération USB
au démarrage. C'est le seul réglage du BIOS qu'il puisse être nécessaire de
toucher. Le port compte aussi : essayer un port USB-A plutôt que l'USB-C,
certains firmwares énumérant les deux différemment.

## 6. Première connexion

| | |
|---|---|
| Identifiant | celui défini dans `build/config/default.conf` (`stef` par défaut) |
| Mot de passe | `claude` |

Le mot de passe **doit être changé immédiatement** — le système l'impose,
puisqu'une image publiée ne peut contenir aucun secret.

Le premier démarrage étend la partition racine et identifie la clé pour la
protéger de la mise en veille USB : compter une minute de plus, une seule
fois.

## 7. Vérifier que tout fonctionne

```bash
mokutil --sb-state                        # attendu : SecureBoot enabled
df -h /                                   # racine étendue (~170 Gio)
journalctl -b -u claude-os-dgpu-power     # état du GPU NVIDIA
journalctl -b -u claude-os-firstboot      # extension et exclusion USB
vainfo                                    # accélération vidéo Intel
sudo powertop                             # consommation réelle
```

La ligne qui compte pour l'autonomie est celle du GPU : `D3cold` signifie
alimentation coupée. Voir `docs/utilisation.md` pour l'interpréter et
corriger si le résultat est `D3hot` ou `D0`.
