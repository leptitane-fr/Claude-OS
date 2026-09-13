#!/usr/bin/python3
# =========================================================================
# Claude OS — fabrique les données des suggestions du clavier à l'écran.
#
#   python3 tools/fabrique-mots.py Lexique383.tsv fra_sentences.tsv shell/data
#
# DEUX SOURCES, DEUX FICHIERS
#
#   mots-fr.txt        QUELS mots existent, et lesquels sont fréquents.
#                      Source : Lexique 3.83 (http://www.lexique.org),
#                      CC BY-SA 4.0 — 142 000 formes fléchies du français
#                      avec leur fréquence mesurée sur livres et sous-titres.
#
#   suites-fr.txt      QUEL mot suit quel autre. Source : Tatoeba
#                      (https://tatoeba.org), CC BY 2.0 FR — 726 000 phrases
#                      françaises, langue parlée : c'est ce qu'on écrit sur
#                      une tablette.
#
# Un dictionnaire de mots isolés ne peut pas proposer « vous » après
# « comment allez ». Il fallait donc un corpus de PHRASES, et il fallait
# qu'il soit redistribuable : ces deux licences le sont, avec attribution,
# portée dans l'entête de chaque fichier produit.
#
# LE TRI EST LA STRUCTURE DE DONNÉES. Les deux fichiers sont triés et lus
# par dichotomie dans une projection en lecture seule (shell/src/mots.c) :
# aucun index à construire au démarrage, aucune copie en mémoire, et les
# pages restent partagées. Sur une machine de 4 Go, cela compte.
#
# mots-fr.txt est trié sur la forme SANS ACCENTS, et la porte en première
# colonne : « eleve » doit proposer « élève » — on ne tape pas ses accents
# quand on cherche un mot au pouce.
# =========================================================================
import collections, csv, re, sys, unicodedata

LEXIQUE, PHRASES, SORTIE = sys.argv[1], sys.argv[2], sys.argv[3]
PERMIS = set("abcdefghijklmnopqrstuvwxyzàâäçéèêëîïôöùûüÿœæñ'-")
SEUIL_MOT = 0.01        # par million : en deçà, on ne le proposera jamais
SEUIL_SUITE = 2         # une suite vue une seule fois n'apprend rien
SUITES_PAR_MOT = 30     # au-delà, ce sont des queues sans usage

def sans_accents(mot):
    # œ et æ ne se décomposent pas : ils se transcrivent.
    mot = mot.replace('œ', 'oe').replace('æ', 'ae')
    return ''.join(c for c in unicodedata.normalize('NFD', mot)
                   if unicodedata.category(c) != 'Mn')

# --- les mots, et leur fréquence -----------------------------------------
freq = collections.Counter()
with open(LEXIQUE, encoding='utf-8') as f:
    for row in csv.DictReader(f, delimiter='\t'):
        mot = row['ortho'].lower()
        poids = (float(row['freqfilms2'] or 0) + float(row['freqlivres'] or 0)) / 2
        if poids <= 0 or not mot or set(mot) - PERMIS:
            continue
        if len(mot) == 1 and mot not in ('a', 'y', 'à'):
            continue
        freq[mot] += poids

# --- les suites de mots ---------------------------------------------------
# Élisions : « l'homme » se tape « l' » puis « homme », mais « aujourd'hui »
# est un seul mot. On ne coupe donc qu'après les élisions connues.
ELISIONS = {"l'", "d'", "j'", "n'", "s'", "t'", "c'", "m'", "qu'", "jusqu'",
            "lorsqu'", "puisqu'", "quoiqu'", "presqu'", "entr'"}
MOT = re.compile(r"[a-zà-öø-ÿœæ]+(?:'[a-zà-öø-ÿœæ]+)*(?:-[a-zà-öø-ÿœæ]+)*", re.I)

def couper(jeton):
    parts, reste = [], jeton
    while True:
        i = reste.find("'")
        if i < 0:
            break
        tete = reste[:i + 1]
        if tete in ELISIONS:
            parts.append(tete)
            reste = reste[i + 1:]
        else:
            break
    if reste:
        parts.append(reste)
    return parts

suites = collections.Counter()
phrases = 0
with open(PHRASES, encoding='utf-8') as f:
    for ligne in f:
        champs = ligne.rstrip('\n').split('\t')
        if len(champs) < 3:
            continue
        phrases += 1
        mots = []
        for jeton in MOT.findall(champs[2].lower()):
            mots.extend(couper(jeton))
        precedent = '^'           # début de phrase : une « suite » comme une autre
        for m in mots:
            if set(m) - PERMIS:
                precedent = '^'
                continue
            suites[(precedent, m)] += 1
            precedent = m

# --- écriture -------------------------------------------------------------
mots = sorted(((sans_accents(m), m, round(v * 100)) for m, v in freq.items() if v >= SEUIL_MOT))
with open(f'{SORTIE}/mots-fr.txt', 'w', encoding='utf-8') as s:
    s.write("# Claude OS — mots français, pour les suggestions du clavier.\n"
            "# Source : Lexique 3.83, http://www.lexique.org — CC BY-SA 4.0.\n"
            "# Colonnes : forme sans accents, mot, fréquence en centièmes de\n"
            "# « par million de mots ». Trié sur la première : le clavier y\n"
            "# cherche par dichotomie (shell/src/mots.c).\n")
    for cle, mot, f2 in mots:
        s.write(f'{cle}\t{mot}\t{f2}\n')

par_mot = collections.defaultdict(list)
for (a, b), n in suites.items():
    if n >= SEUIL_SUITE:
        par_mot[a].append((n, b))
lignes = 0
with open(f'{SORTIE}/suites-fr.txt', 'w', encoding='utf-8') as s:
    s.write("# Claude OS — quel mot suit quel autre, pour les suggestions.\n"
            "# Source : Tatoeba, https://tatoeba.org — CC BY 2.0 FR,\n"
            f"# {phrases} phrases françaises.\n"
            "# Colonnes : mot précédent (« ^ » = début de phrase), mot, compte.\n"
            "# Trié par mot précédent, puis par compte décroissant.\n")
    for a in sorted(par_mot):
        for n, b in sorted(par_mot[a], reverse=True)[:SUITES_PAR_MOT]:
            s.write(f'{a}\t{b}\t{n}\n')
            lignes += 1
print(f'{len(mots)} mots, {lignes} suites tirées de {phrases} phrases')
