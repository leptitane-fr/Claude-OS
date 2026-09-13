#!/usr/bin/python3
# =========================================================================
# Claude OS — calcule la disposition du clavier gauche du mode console.
#
# L'AZERTY vient des machines à écrire ; un pouce seul sur un écran obéit à
# autre chose. Ce script refait le calcul qui a produit la disposition de
# shell/src/clavier-ecran.c (tables CL0..CL5 et CA0..CA5), le 12 septembre
# 2026. Il est ici pour que ce calcul soit REFAISABLE : changer une mesure,
# une fréquence ou une contrainte, et relancer.
#
# CE QU'IL LUI FAUT
#
#   Lexique383.tsv   les fréquences du français, 142 000 formes, CC BY-SA.
#                    http://www.lexique.org/databases/Lexique383/Lexique383.tsv
#   pouces.csv       la sortie de sonde-pouces.c : les appuis mesurés,
#                    tablette tenue à deux mains. FACULTATIF : sans lui, on
#                    repart de l'ellipse de la mesure du 11 septembre 2026,
#                    inscrite plus bas. Le fichier brut de cette mesure a été
#                    perdu dans un redémarrage — il était dans /tmp, faute à
#                    ne pas refaire : une mesure se verse au dépôt le jour
#                    même.
#
#   python3 shell/essais/disposition-pouce.py Lexique383.tsv pouces.csv
#
# LA MÉTHODE, en trois temps
#
#   1. Fréquences : lettres, é et apostrophe d'après Lexique, pondérées par
#      l'usage (freqfilms2 + freqlivres) ; enchaînements de deux lettres
#      dans le mot ; virgule et point mesurés sur la prose du dépôt (1,51 et
#      1,40 % des lettres) car un dictionnaire n'en contient pas.
#   2. Zone du pouce : ellipse ajustée sur les appuis (axes principaux).
#      Son centre a été REMONTÉ à y = 330 après le premier essai au doigt —
#      « la ligne du bas est trop basse » : le retour d'usage corrige la
#      sonde, où l'on tape plus bas qu'en écrivant.
#   3. Coût d'une frappe = inconfort de la place (distance à l'ellipse) +
#      trajet depuis la lettre précédente (loi de Fitts), pondérés par les
#      fréquences ; minimisé par recuit simulé.
#
# NE PAS S'ÉTONNER QUE CE SCRIPT NE REDONNE PAS LES TABLES DU DÉPÔT : elles
# viennent d'un tirage antérieur, à 1,964, et un nouveau tirage tombe à
# 1,970 avec des places différentes. C'est la nature du problème, pas un
# défaut — voir ci-dessous.
#
# CE QU'IL FAUT SAVOIR DU RÉSULTAT : l'optimum est PLAT. Quatre tirages
# donnent le même coût à 0,3 % près avec des places différentes. Ce qui est
# stable, c'est la hiérarchie — e à la meilleure place, puis s, a, i, t, n.
# Le détail se choisit donc sur d'autres critères sans rien perdre.
# =========================================================================
import collections, csv, json, math, os, random, sys

LEXIQUE = sys.argv[1] if len(sys.argv) > 1 else 'Lexique383.tsv'
APPUIS  = sys.argv[2] if len(sys.argv) > 2 else 'pouces.csv'

JEU = list("esaitnrudolpmcvébfgqhjxzyk") + ["'", "w", ",", "."]
PONCT = {',': 1.51, '.': 1.40}          # % des lettres, mesuré sur docs/*.md

# --- 1. les fréquences ----------------------------------------------------
lettres, bigrammes, fin = collections.Counter(), collections.Counter(), collections.Counter()
with open(LEXIQUE, encoding='utf-8') as f:
    for row in csv.DictReader(f, delimiter='\t'):
        poids = (float(row['freqfilms2'] or 0) + float(row['freqlivres'] or 0)) / 2
        mot = row['ortho'].lower()
        if poids <= 0 or not mot:
            continue
        for i, c in enumerate(mot):
            lettres[c] += poids
            if i:
                bigrammes[(mot[i-1], mot[i])] += poids
        fin[mot[-1]] += poids

tot = sum(lettres.values())
freq = {c: 100 * lettres[c] / tot for c in JEU}
freq.update(PONCT)
n1 = sum(freq.values())
p1 = {c: freq[c] / n1 for c in JEU}

bt = sum(bigrammes.values())
bi = collections.Counter()
for (a, b), v in bigrammes.items():
    if a in JEU and b in JEU:
        bi[(a, b)] += 0.80 * v / bt      # ~80 % des caractères suivent une lettre du même mot
ft = sum(fin.values())
for x, v in fin.items():                 # « mot, » et « mot. » : la ponctuation suit la dernière lettre
    if x in JEU:
        for signe in PONCT:
            bi[(x, signe)] += p1[signe] * v / ft

# --- 2. la zone du pouce --------------------------------------------------
# La mesure du 11 septembre 2026 : 136 appuis du pouce gauche, ellipse
# ajustée dessus. Sert quand le fichier brut n'est pas là.
MESURE_2026_09_11 = dict(n=136, mx=145.0, th=math.radians(70), su=76.0, sv=61.0)

pts = []
for r in (csv.reader(open(APPUIS)) if os.path.exists(APPUIS) else []):
    if r[1] != 'appui':
        continue
    # les premières sondes écrivaient la virgule décimale (locale fr)
    x, y = (float(r[3] + '.' + r[4]), float(r[5] + '.' + r[6])) if len(r) > 5 else (float(r[3]), float(r[4]))
    if x < 960:
        pts.append((x, y))
n = len(pts)
if n < 20:
    m = MESURE_2026_09_11
    n, mx, th, su, sv = m['n'], m['mx'], m['th'], m['su'], m['sv']
    print(f'{APPUIS} absent ou trop court : ellipse du 11 septembre 2026')
else:
  mx = sum(x for x, _ in pts) / n
  sxx = sum((x - mx) ** 2 for x, _ in pts) / n
  my = sum(y for _, y in pts) / n
  syy = sum((y - my) ** 2 for _, y in pts) / n
  sxy = sum((x - mx) * (y - my) for x, y in pts) / n
  tr, det = sxx + syy, sxx * syy - sxy * sxy
  l1, l2 = tr / 2 + math.sqrt(tr * tr / 4 - det), tr / 2 - math.sqrt(tr * tr / 4 - det)
  th = 0.5 * math.atan2(2 * sxy, sxx - syy)
  su, sv = math.sqrt(l1), math.sqrt(l2)
cx, cy = mx, 330.0                       # centre remonté : voir l'en-tête
print(f'{n} appuis — axe {math.degrees(th):.0f}°, écarts-types {su:.0f} / {sv:.0f} px')

def inconfort(x, y):
    u = (x - cx) * math.cos(th) + (y - cy) * math.sin(th)
    v = -(x - cx) * math.sin(th) + (y - cy) * math.cos(th)
    return (u / su) ** 2 + (v / sv) ** 2

# --- 3. les places, et le recuit -----------------------------------------
# Grille 5 x 6 de la colonne gauche : touches de 52 px au pas de 56 en
# hauteur, 54 px au pas de 58,4 en largeur (300 px de colonne).
POS = [(33.2 + 58.4 * j, 190 + 56 * r) for r in range(6) for j in range(5)]
LARGEUR_TOUCHE, CONFORT = 54.4, 0.5

def fitts(a, b):
    d = math.hypot(a[0] - b[0], a[1] - b[1])
    return 0 if d < 1 else math.log2(d / LARGEUR_TOUCHE + 1)

def cout(aff):
    c = sum(p1[l] * CONFORT * inconfort(*POS[aff[l]]) for l in JEU)
    t = sum(v * fitts(POS[aff[a]], POS[aff[b]]) for (a, b), v in bi.items())
    return c + t, c, t

def recuit(graine, iters=150000):
    random.seed(graine)
    ordre = sorted(JEU, key=lambda l: -p1[l])
    places = sorted(range(len(POS)), key=lambda i: inconfort(*POS[i]))
    aff = {l: places[i] for i, l in enumerate(ordre)}     # départ : fréquence -> confort
    cur = cout(aff)[0]
    best, bestc, T = dict(aff), cur, 0.05
    for _ in range(iters):
        a, b = random.sample(JEU, 2)
        aff[a], aff[b] = aff[b], aff[a]
        nouveau = cout(aff)[0]
        if nouveau < cur or random.random() < math.exp((cur - nouveau) / T):
            cur = nouveau
        else:
            aff[a], aff[b] = aff[b], aff[a]
        if cur < bestc:
            bestc, best = cur, dict(aff)
        T *= 0.99998
    return best

meilleur, meilleur_cout = None, None
for graine in range(1, 5):
    aff = recuit(graine)
    c = cout(aff)
    print(f'graine {graine} : coût {c[0]:.3f} (confort {c[1]:.3f} + trajets {c[2]:.3f})')
    if meilleur_cout is None or c[0] < meilleur_cout:
        meilleur, meilleur_cout = aff, c[0]

inv = {v: k for k, v in meilleur.items()}
print('\ndisposition retenue :')
for r in range(6):
    print('  ' + '  '.join(inv[r * 5 + j] for j in range(5)))

# Les accents, à la même grille : par fréquence sur les places les plus
# confortables, puis la typographie française.
acc = sorted("àèêçùôîûâïëüœæÿ", key=lambda c: -lettres.get(c, 0))
suite = ["«", "»", "…", "–", "—", "“", "”", "‘", "’", "°", "ö", "ä", "ñ", "ß", "ã"]
places = sorted(range(len(POS)), key=lambda i: inconfort(*POS[i]))
g = [None] * len(POS)
for i, c in enumerate((acc + suite)[:len(POS)]):
    g[places[i]] = c
print('\naccents :')
for r in range(6):
    print('  ' + '  '.join(g[r * 5 + j] for j in range(5)))
