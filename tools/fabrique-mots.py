#!/usr/bin/python3
# =========================================================================
# Claude OS — fabrique la liste de mots des suggestions du clavier.
#
#   python3 tools/fabrique-mots.py Lexique383.tsv shell/data/mots-fr.txt
#
# La source est Lexique 3.83 (http://www.lexique.org), sous licence
# CC BY-SA 4.0 : 142 000 formes fléchies du français avec leur fréquence
# d'usage, mesurée sur des livres et des sous-titres de films. On n'en garde
# que ce dont le clavier a besoin — la forme et sa fréquence — et l'entête
# du fichier produit porte l'attribution, que la licence exige.
#
# Ce qui est écarté : les formes rares (moins de 0,01 par million, qui ne
# devraient jamais être proposées), celles d'une seule lettre hormis « a »
# et « y », et tout ce qui contient autre chose que des lettres, une
# apostrophe ou un trait d'union — le clavier ne sait pas les écrire.
#
# Le fichier est trié par mot, en octets : le clavier y cherche un préfixe
# par dichotomie, sans rien charger d'autre en mémoire que le fichier lui-
# même, projeté en lecture seule.
# =========================================================================
import collections, csv, sys, unicodedata

LEX, SORTIE = sys.argv[1], sys.argv[2]
PERMIS = set("abcdefghijklmnopqrstuvwxyzàâäçéèêëîïôöùûüÿœæñ'-")
SEUIL = 0.01

freq = collections.Counter()
with open(LEX, encoding='utf-8') as f:
    for row in csv.DictReader(f, delimiter='\t'):
        mot = row['ortho'].lower()
        f2 = (float(row['freqfilms2'] or 0) + float(row['freqlivres'] or 0)) / 2
        if f2 <= 0 or not mot or set(mot) - PERMIS:
            continue
        if len(mot) == 1 and mot not in ('a', 'y', 'à'):
            continue
        freq[mot] += f2

mots = sorted((m for m, v in freq.items() if v >= SEUIL))
with open(SORTIE, 'w', encoding='utf-8') as s:
    s.write("# Claude OS — mots français et fréquence d'usage, pour les\n"
            "# suggestions du clavier à l'écran.\n"
            "# Source : Lexique 3.83, http://www.lexique.org — CC BY-SA 4.0.\n"
            "# Fréquence en centièmes de « par million de mots ».\n"
            "# Trié par mot ; recherche par dichotomie (shell/src/mots.c).\n")
    for m in mots:
        s.write(f'{m}\t{round(freq[m] * 100)}\n')
print(f'{len(mots)} mots écrits dans {SORTIE}')
