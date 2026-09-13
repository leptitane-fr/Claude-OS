#!/usr/bin/python3
# =========================================================================
# Claude OS — calcule la disposition ORGANIQUE du clavier gauche (console).
#
#   python3 shell/essais/disposition-pouce.py Lexique383.tsv [pouces.csv]
#
# L'AZERTY vient des machines à écrire ; un pouce seul sur un écran obéit à
# autre chose. Ce script produit les tables de shell/src/clavier-ecran.c.
# Démarche de BÉPO pour les fréquences, de Metropolis (Zhai, 2000) pour le
# pointeur unique, et de KALQ (2013) pour la frappe au pouce sur tablette.
#
# TROISIÈME VERSION, 13 septembre 2026, après essai au doigt. L'utilisateur
# a demandé « un design plus organique, avec les lettres les moins utilisées
# plus petites que les autres, et non nécessairement un 5x5 bien aligné »,
# et « une diagonale haut-gauche bas-droit à la trajectoire légèrement
# arrondie, pour suivre le mouvement du pouce ». Il a aussi rappelé que
# l'absence de lettres (k et w avaient été relégués) est problématique.
#
# CE QUI EN DÉCOULE
#
#   - 27 touches : les 26 lettres et é, plus la barre d'espace ;
#   - des rangées qui PAVENT la colonne, sans trou : aucun appui perdu
#     entre deux touches, ce qu'une grille à espacement ne garantit pas ;
#   - dans une rangée, la touche est d'autant plus LARGE qu'elle est proche
#     de l'arc du pouce ; les rangées sont d'autant plus HAUTES qu'elles
#     sont au cœur de la zone atteinte ;
#   - l'arc lui-même : la droite d'inertie du nuage d'appuis (70°, du coin
#     haut-gauche au coin bas-droit), légèrement bombée.
#
# Et l'optimisation n'a plus besoin qu'on lui dise de mettre les lettres
# fréquentes sur les grandes touches : la loi de Fitts le lui dit, puisque
# la largeur de la cible entre dans le temps d'atteinte.
#
# Les mesures qui fondent la géométrie (136 appuis du pouce gauche, sonde
# du 11 septembre 2026) sont dans MESURE ci-dessous ; le fichier brut a été
# perdu dans un redémarrage, il était dans /tmp.
# =========================================================================
import collections, csv, json, math, os, random, sys

LEXIQUE = sys.argv[1] if len(sys.argv) > 1 else 'Lexique383.tsv'

# --- la colonne, et la zone du pouce --------------------------------------
LARGEUR = 288            # 300 px de colonne moins 2 x 6 de marge
HAUT = 176               # au-dessus, le pouce n'atteint plus
MESURE = dict(cx=145.0, cy=330.0, angle=70.0, su=76.0, sv=61.0)
BOMBE = 14               # l'arc n'est pas droit : il se bombe de 14 px

# Cinq rangées de lettres, puis la barre d'espace. Le nombre de touches par
# rangée et la hauteur des rangées sont choisis ici ; tout le reste se
# calcule. 5+6+6+5+5 = 27 touches, et 52+56+60+56+56 = 280 px — la même
# hauteur que les cinq rangées des autres calques, pour que la barre
# d'espace tombe au même endroit quand on bascule.
RANGEES = [(5, 52), (6, 56), (6, 60), (5, 56), (5, 56)]
ESPACE_H = 56

def arc_x(y):
    """L'abscisse du cœur de l'arc à cette hauteur."""
    dy = y - MESURE['cy']
    return (MESURE['cx'] + dy / math.tan(math.radians(MESURE['angle']))
            + BOMBE * max(0.0, 1 - (dy / 150.0) ** 2))

def inconfort(x, y):
    th = math.radians(MESURE['angle'])
    u = (x - MESURE['cx']) * math.cos(th) + (y - MESURE['cy']) * math.sin(th)
    v = -(x - MESURE['cx']) * math.sin(th) + (y - MESURE['cy']) * math.cos(th)
    return (u / MESURE['su']) ** 2 + (v / MESURE['sv']) ** 2

def geometrie():
    """Les cellules : elles pavent la colonne, sans trou ni recouvrement."""
    cellules, y = [], HAUT
    for n, h in RANGEES:
        centre = arc_x(y + h / 2)
        largeurs = [LARGEUR / n] * n
        for _ in range(30):                     # point fixe : largeur <-> position
            x, centres = 0, []
            for l in largeurs:
                centres.append(x + l / 2)
                x += l
            poids = [1 + 0.55 * math.exp(-((c - centre) / 95.0) ** 2) for c in centres]
            total = sum(poids)
            largeurs = [LARGEUR * p / total for p in poids]
        x = 0
        for l in largeurs:
            cellules.append(dict(x=round(x), y=y, l=round(l), h=h))
            x += l
        cellules[-1]['l'] = LARGEUR - cellules[-1]['x']   # le reste, au pixel près
        y += h
    return cellules, y

# --- les fréquences -------------------------------------------------------
JEU = list("esaitnrudolpmcvébfgqhjxzykw")
lettres, bg, fin, deb = (collections.Counter() for _ in range(4))
with open(LEXIQUE, encoding='utf-8') as f:
    for row in csv.DictReader(f, delimiter='\t'):
        w = (float(row['freqfilms2'] or 0) + float(row['freqlivres'] or 0)) / 2
        m = row['ortho'].lower()
        if w <= 0 or not m:
            continue
        for i, c in enumerate(m):
            lettres[c] += w
            if i:
                bg[(m[i-1], m[i])] += w
        fin[m[-1]] += w
        deb[m[0]] += w

tot = sum(lettres.values())
mots = sum(fin.values())
p_esp = 1.0 / (tot / mots)                      # une espace par mot
p1 = {c: lettres[c] / tot for c in JEU}
n1 = sum(p1.values()) + p_esp
p1 = {c: v / n1 for c, v in p1.items()}
P_ESP = p_esp / n1
bt = sum(bg.values())
bi = collections.Counter()
for (a, b), v in bg.items():
    if a in JEU and b in JEU:
        bi[(a, b)] += 0.80 * v / bt
for x, v in fin.items():
    if x in JEU:
        bi[(x, '␣')] += P_ESP * v / mots
for x, v in deb.items():
    if x in JEU:
        bi[('␣', x)] += P_ESP * v / mots

# --- le coût --------------------------------------------------------------
CONFORT = 0.5
cellules, bas = geometrie()
ESPACE = dict(x=0, y=bas, l=LARGEUR, h=ESPACE_H)
assert len(cellules) == len(JEU), f'{len(cellules)} cellules pour {len(JEU)} lettres'

def centre(c):
    return c['x'] + c['l'] / 2, c['y'] + c['h'] / 2

def cible(c):
    """La taille utile de la cible : la moyenne géométrique de ses côtés."""
    return math.sqrt(c['l'] * c['h'])

def fitts(a, b):
    (xa, ya), (xb, yb) = centre(a), centre(b)
    d = math.hypot(xa - xb, ya - yb)
    return 0 if d < 1 else math.log2(d / cible(b) + 1)

# Tout ce qui ne dépend pas de l'affectation est calculé UNE fois : le
# confort de chaque cellule, et le trajet de chaque cellule à chaque autre.
# Sans cela, le recuit passe son temps à refaire les mêmes racines carrées.
CELL = cellules + [ESPACE]
ESP = len(cellules)
CONF = [CONFORT * inconfort(*centre(c)) for c in CELL]
TRAJET = [[fitts(a, b) for b in CELL] for a in CELL]
# Les enchaînements, par lettre : le recuit n'a besoin que de ceux qui
# touchent les deux lettres échangées.
VOISINS = collections.defaultdict(list)
BI = []
for (a, b), v in bi.items():
    ia = ESP if a == '␣' else None
    ib = ESP if b == '␣' else None
    BI.append((a, b, v))
for i, (a, b, v) in enumerate(BI):
    if a != '␣':
        VOISINS[a].append(i)
    if b != '␣' and b != a:
        VOISINS[b].append(i)

def ou(aff, c):
    return ESP if c == '␣' else aff[c]

def cout(aff):
    c = sum(p1[l] * CONF[aff[l]] for l in JEU) + P_ESP * CONF[ESP]
    t = sum(v * TRAJET[ou(aff, a)][ou(aff, b)] for a, b, v in BI)
    return c + t, c, t

def cout_partiel(aff, lettres_touchees):
    """Ce que coûtent les seuls termes qui changent quand on déplace ces
    lettres : le recuit ne recalcule que cela."""
    vus = set()
    for l in lettres_touchees:
        vus.update(VOISINS[l])
    c = sum(p1[l] * CONF[aff[l]] for l in lettres_touchees)
    t = sum(BI[i][2] * TRAJET[ou(aff, BI[i][0])][ou(aff, BI[i][1])] for i in vus)
    return c + t

def recuit(graine, iters=200000):
    random.seed(graine)
    ordre = sorted(JEU, key=lambda l: -p1[l])
    places = sorted(range(len(cellules)),
                    key=lambda i: inconfort(*centre(cellules[i])) - cible(cellules[i]) / 40)
    aff = {l: places[i] for i, l in enumerate(ordre)}
    cur = cout(aff)[0]
    best, bestc, T = dict(aff), cur, 0.05
    for _ in range(iters):
        a, b = random.sample(JEU, 2)
        avant = cout_partiel(aff, (a, b))
        aff[a], aff[b] = aff[b], aff[a]
        delta = cout_partiel(aff, (a, b)) - avant
        if delta < 0 or random.random() < math.exp(-delta / T):
            cur += delta
            if cur < bestc:
                bestc, best = cur, dict(aff)
        else:
            aff[a], aff[b] = aff[b], aff[a]
        T *= 0.99998
    return best

meilleur, mc = None, None
for graine in range(1, 4):
    aff = recuit(graine)
    c = cout(aff)
    print(f'graine {graine} : coût {c[0]:.3f} (confort {c[1]:.3f} + trajets {c[2]:.3f})')
    if mc is None or c[0] < mc:
        meilleur, mc = aff, c[0]

inv = {v: k for k, v in meilleur.items()}
print(f'\n{len(cellules)} touches, de {min(cible(c) for c in cellules):.0f} '
      f'à {max(cible(c) for c in cellules):.0f} px de côté utile ; '
      f'espace {ESPACE["l"]}x{ESPACE["h"]} à y={ESPACE["y"]}')
print('\n/* Table pour clavier-ecran.c */')
for i, c in enumerate(cellules):
    print(f'    {{ "{inv[i]}", {c["x"]:3d}, {c["y"]:3d}, {c["l"]:3d}, {c["h"]:3d} }},')
print(f'    /* espace */ {{ NULL, {ESPACE["x"]:3d}, {ESPACE["y"]:3d}, '
      f'{ESPACE["l"]:3d}, {ESPACE["h"]:3d} }},')
json.dump({'cellules': [dict(c, lettre=inv[i]) for i, c in enumerate(cellules)],
           'espace': ESPACE}, open('/tmp/disposition-organique.json', 'w'), ensure_ascii=False)
