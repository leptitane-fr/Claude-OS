#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Claude OS — fabrique le thème d'icônes de la distribution.

    python3 tools/fabrique-icones.py            # écrit rootfs/usr/share/icons/Claude-OS
    python3 tools/fabrique-icones.py --planche   # + une planche de contact en PNG

POURQUOI UN GÉNÉRATEUR ET PAS QUATRE-VINGT-DIX FICHIERS ÉCRITS À LA MAIN

Ce qui fait qu'un jeu d'icônes paraît DESSINÉ plutôt qu'assemblé, ce n'est
pas le talent de chaque pictogramme : c'est qu'ils partagent tous la même
épaisseur de trait, le même rayon d'angle, la même marge, le même diamètre
de disque. Ces cinq constantes sont en tête du fichier. Les changer, c'est
redessiner les quatre-vingt-dix icônes d'un coup, sans qu'aucune ne reste en
arrière — ce qu'un répertoire de SVG écrits un par un ne permet jamais.

Le fichier est donc la SOURCE, et rootfs/usr/share/icons/Claude-OS le
produit. Corriger une icône se fait ici ; corriger le SVG installé serait
perdu à la prochaine exécution.

CE QUE GTK FAIT DE CES FICHIERS, ET LES DEUX PIÈGES QUI EN DÉCOULENT

Une icône dont le nom finit par « -symbolic » est RECOLORÉE par GTK : il
enveloppe le SVG dans une feuille de style qui impose « fill » à la couleur
du texte courant. Deux conséquences, vérifiées à la mesure sur cette machine
(rendu d'une icône d'essai en rouge, puis lecture des pixels) :

  1. La règle porte sur « rect, circle, path, polygon » — les quatre sont
     bien recolorés, y compris dans un groupe transformé.
  2. Elle porte sur « fill », et sur lui seul. UN TRAIT AU SENS SVG —
     stroke — NE SERAIT PAS RECOLORÉ : il resterait noir sur un thème
     sombre. Tout ce que ce fichier dessine est donc une surface pleine,
     jamais un contour. C'est la contrainte qui décide de la forme des
     aides ci-dessous : « trait » y veut dire « rectangle long », et
     « cercle vide » veut dire « anneau à deux sous-chemins ».

LE COMMENTAIRE DE CHAQUE SVG EST À L'INTÉRIEUR DE LA BALISE <svg>, jamais
au-dessus : posé avant, il repousse la balise hors de la fenêtre que
gdk-pixbuf examine, et GTK refuse de lire l'image en annonçant un format
inconnu. Le projet a déjà payé cette erreur une fois — voir la tête de
shell/data/icons/hicolor/scalable/status/claude-os-cloche-symbolic.svg.
"""

import argparse
import math
import os
import shutil
import sys

# ---------------------------------------------------------------------------
# La grammaire, en cinq nombres
# ---------------------------------------------------------------------------
# Toutes les icônes symboliques sont dessinées dans un carré de 16, marge
# comprise. Ce sont ces constantes, et elles seules, qui font la famille.

GRILLE = 16.0   # côté du carré de dessin
MARGE  = 1.0    # bord laissé vide : l'icône vit dans 14 × 14
TRAIT  = 1.5    # épaisseur de tout ce qui est un trait
ANGLE  = 2.0    # rayon des angles d'un contenant (écran, boîte, fenêtre)
BOUT   = TRAIT / 2.0   # rayon des extrémités d'un trait : elles sont rondes

# TRAIT = 1,5 ET PAS 2 : c'est l'épaisseur de la cloche du centre de
# notifications, seule icône que le projet possédait avant celle-ci. Un jeu
# plus gras aurait été plus net au pixel près sur un écran à 16 px, mais la
# cloche aurait dépareillé — et c'est elle qu'on voit à côté de l'heure toute
# la journée.

ENCRE = "#222222"   # remplacée par GTK à l'affichage ; visible seule hors GTK


# ---------------------------------------------------------------------------
# Primitives — tout rend une donnée de chemin SVG
# ---------------------------------------------------------------------------
def n(v):
    """Un nombre court : SVG n'a que faire de dix-sept décimales."""
    s = f"{v:.3f}".rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


class Forme:
    """Un chemin, sa transformation éventuelle, et sa règle de remplissage.

    Les trois voyagent ensemble parce qu'ils se décident ensemble : un
    anneau EST un chemin à deux boucles ET la règle « pair-impair » ; les
    séparer revenait à tenir, dans svg(), la liste des indices de formes
    trouées — une liste qui se serait désaccordée à la première icône
    réordonnée."""

    __slots__ = ("d", "tr", "pair_impair")

    def __init__(self, d, tr=None, pair_impair=False):
        self.d, self.tr, self.pair_impair = d, tr, pair_impair


def rect(x, y, w, h, r=0.0):
    """Rectangle, éventuellement à coins arrondis."""
    r = min(r, w / 2, h / 2)
    if r <= 0:
        return f"M{n(x)} {n(y)}h{n(w)}v{n(h)}h{n(-w)}z"
    return (f"M{n(x + r)} {n(y)}"
            f"H{n(x + w - r)}A{n(r)} {n(r)} 0 0 1 {n(x + w)} {n(y + r)}"
            f"V{n(y + h - r)}A{n(r)} {n(r)} 0 0 1 {n(x + w - r)} {n(y + h)}"
            f"H{n(x + r)}A{n(r)} {n(r)} 0 0 1 {n(x)} {n(y + h - r)}"
            f"V{n(y + r)}A{n(r)} {n(r)} 0 0 1 {n(x + r)} {n(y)}Z")


def barre(x1, y1, x2, y2, e=TRAIT):
    """Un trait entre deux points, bouts ronds.

    C'est la primitive la plus utilisée du fichier : un « trait » n'existant
    pas en surface pleine, chaque trait est ce rectangle tourné."""
    dx, dy = x2 - x1, y2 - y1
    lon = math.hypot(dx, dy)
    if lon == 0:
        return disque(x1, y1, e / 2)
    ang = math.degrees(math.atan2(dy, dx))
    # L'ORDRE DES DEUX TRANSFORMATIONS N'EST PAS INTERCHANGEABLE : SVG les
    # applique de droite à gauche, donc « translate puis rotate » fait
    # tourner le trait autour de SON origine, et non autour de celle du
    # dessin. Écrit dans l'autre sens, chaque trait oblique partait à
    # l'opposé de l'icône.
    return Forme(rect(0, -e / 2, lon, e, e / 2),
                 f"translate({n(x1)} {n(y1)}) rotate({n(ang)})")


def h(x1, x2, y, e=TRAIT):
    """Trait horizontal, de x1 à x2, centré sur y."""
    return rect(x1, y - e / 2, x2 - x1, e, e / 2)


def v(y1, y2, x, e=TRAIT):
    """Trait vertical, de y1 à y2, centré sur x."""
    return rect(x - e / 2, y1, e, y2 - y1, e / 2)


def cercle(cx, cy, r, sens=1):
    """Disque plein. « sens » n'a d'effet qu'au sein d'un anneau."""
    b = 1 if sens > 0 else 0
    return (f"M{n(cx - r)} {n(cy)}"
            f"A{n(r)} {n(r)} 0 1 {b} {n(cx + r)} {n(cy)}"
            f"A{n(r)} {n(r)} 0 1 {b} {n(cx - r)} {n(cy)}Z")


disque = cercle


def anneau(cx, cy, r, e=TRAIT):
    """Cercle vide : deux sous-chemins et la règle « pair-impair »."""
    return Forme(cercle(cx, cy, r + e / 2) + cercle(cx, cy, r - e / 2),
                 pair_impair=True)


def contour(x, y, w, hh, r=ANGLE, e=TRAIT):
    """Rectangle vide — un écran, une fenêtre, une boîte."""
    return Forme(rect(x, y, w, hh, r)
                 + rect(x + e, y + e, w - 2 * e, hh - 2 * e, max(r - e, 0.3)),
                 pair_impair=True)


def arc(cx, cy, r, a0, a1, e=TRAIT, bouts=False):
    """Portion d'anneau, angles en degrés, 0 à droite et croissant vers le bas.

    « bouts » arrondit les extrémités : indispensable sur les ondes du Wi-Fi
    et du volume, où deux arcs voisins se lisent comme une famille."""
    ro, ri = r + e / 2, r - e / 2
    ra0, ra1 = math.radians(a0), math.radians(a1)
    grand = 1 if abs(a1 - a0) > 180 else 0
    p = (f"M{n(cx + ro * math.cos(ra0))} {n(cy + ro * math.sin(ra0))}"
         f"A{n(ro)} {n(ro)} 0 {grand} 1 "
         f"{n(cx + ro * math.cos(ra1))} {n(cy + ro * math.sin(ra1))}")
    if bouts:
        p += (f"A{n(e / 2)} {n(e / 2)} 0 0 1 "
              f"{n(cx + ri * math.cos(ra1))} {n(cy + ri * math.sin(ra1))}")
    else:
        p += f"L{n(cx + ri * math.cos(ra1))} {n(cy + ri * math.sin(ra1))}"
    p += (f"A{n(ri)} {n(ri)} 0 {grand} 0 "
          f"{n(cx + ri * math.cos(ra0))} {n(cy + ri * math.sin(ra0))}")
    if bouts:
        p += f"A{n(e / 2)} {n(e / 2)} 0 0 1 {n(cx + ro * math.cos(ra0))} {n(cy + ro * math.sin(ra0))}"
    return p + "Z"


def polygone(*points):
    """Surface pleine définie par ses sommets — un triangle de lecture, un
    chevron déjà épaissi."""
    d = f"M{n(points[0][0])} {n(points[0][1])}"
    for x, y in points[1:]:
        d += f"L{n(x)} {n(y)}"
    return d + "Z"


def chevron(cx, cy, taille, sens, e=TRAIT):
    """« > » et ses trois rotations. sens : 'droite', 'gauche', 'haut', 'bas'."""
    t = taille
    branches = {
        "droite": ((cx - t / 2, cy - t), (cx + t / 2, cy), (cx - t / 2, cy + t)),
        "gauche": ((cx + t / 2, cy - t), (cx - t / 2, cy), (cx + t / 2, cy + t)),
        "haut":   ((cx - t, cy + t / 2), (cx, cy - t / 2), (cx + t, cy + t / 2)),
        "bas":    ((cx - t, cy - t / 2), (cx, cy + t / 2), (cx + t, cy - t / 2)),
    }[sens]
    a, b, c = branches
    return [barre(a[0], a[1], b[0], b[1], e), barre(b[0], b[1], c[0], c[1], e)]


def fleche(x1, y1, x2, y2, tete=2.6, e=TRAIT):
    """Trait fléché : le fût, puis deux barbes."""
    ang = math.atan2(y2 - y1, x2 - x1)
    out = [barre(x1, y1, x2, y2, e)]
    for d in (+1, -1):
        a = ang + math.pi + d * math.radians(38)
        out.append(barre(x2, y2, x2 + tete * math.cos(a), y2 + tete * math.sin(a), e))
    return out


# ---------------------------------------------------------------------------
# Assemblage d'un fichier
# ---------------------------------------------------------------------------
def aplatir(formes):
    """Une aide rend parfois une liste de formes (un chevron en fait deux).
    On accepte donc les listes imbriquées plutôt que d'obliger chaque icône
    à les recoudre elle-même."""
    plat = []
    for f in formes:
        if isinstance(f, (list, tuple)) and not isinstance(f, Forme):
            plat.extend(aplatir(f))
        elif isinstance(f, str):
            plat.append(Forme(f))
        else:
            plat.append(f)
    return plat


def svg(formes, taille=GRILLE, note="", couleur=ENCRE):
    """Compose le fichier à partir de chaînes de chemin ou de Formes."""
    corps = []
    for f in aplatir(formes):
        regle = ' fill-rule="evenodd"' if f.pair_impair else ""
        path = f'<path d="{f.d}" fill="{couleur}"{regle}/>'
        corps.append(f'  <g transform="{f.tr}">{path}</g>' if f.tr else f'  {path}')

    # Le commentaire est DANS la balise <svg> : voir la tête du fichier.
    tete = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{n(taille)}" '
            f'height="{n(taille)}" viewBox="0 0 {n(taille)} {n(taille)}">')
    if note:
        tete += f"\n  <!-- Claude OS — {note} -->"
    return "\n".join(['<?xml version="1.0" encoding="UTF-8"?>', tete] + corps + ["</svg>", ""])


def pointe(x, y, ang, t=2.4):
    """Pointe de flèche pleine, centrée sur (x, y) et dirigée par « ang »
    (degrés). Sert aux flèches courbes, où deux barbes droites décrochent
    visiblement de la courbe."""
    a = math.radians(ang)
    ca, sa = math.cos(a), math.sin(a)
    def p(dx, dy):
        return (x + dx * ca - dy * sa, y + dx * sa + dy * ca)
    return polygone(p(t * 0.62, 0), p(-t * 0.38, t * 0.62), p(-t * 0.38, -t * 0.62))


def ellipse_anneau(cx, cy, rx, ry, e=TRAIT):
    """Ellipse vide — le méridien d'un globe, et rien d'autre pour l'instant."""
    def contour_ellipse(rx, ry):
        return (f"M{n(cx)} {n(cy - ry)}"
                f"A{n(rx)} {n(ry)} 0 0 1 {n(cx)} {n(cy + ry)}"
                f"A{n(rx)} {n(ry)} 0 0 1 {n(cx)} {n(cy - ry)}Z")
    return Forme(contour_ellipse(rx + e / 2, ry + e / 2)
                 + contour_ellipse(max(rx - e / 2, 0.2), max(ry - e / 2, 0.2)),
                 pair_impair=True)


def troue(plein, *trous):
    """Une surface pleine percée : le classique trou de serrure d'un cadenas,
    l'échancrure d'un croissant de lune."""
    return Forme(plein + "".join(trous), pair_impair=True)


# ===========================================================================
# LE CATALOGUE SYMBOLIQUE
#
# Un dictionnaire nom -> (contexte, description, formes). Les noms ne sont
# pas choisis : ce sont EXACTEMENT ceux que le shell demande à GTK, relevés
# dans shell/src/*.c. Une icône dessinée sous un autre nom ne serait jamais
# affichée, et une icône manquante retombe silencieusement sur le thème
# hérité — d'où le contrôle de couverture en fin de fichier.
# ===========================================================================

def haut_parleur():
    """Le corps commun aux quatre icônes de volume. Plein, et non en
    contour : à 16 px, un cône évidé se referme visuellement."""
    return polygone((2.4, 6.4), (5.2, 6.4), (8.5, 3.1),
                    (8.5, 12.9), (5.2, 9.6), (2.4, 9.6))


def ondes(combien):
    """Les arcs du volume, ouverts à droite. Rayons en progression régulière :
    c'est leur espacement constant qui fait qu'on lit un niveau et non trois
    traits."""
    return [arc(8.5, 8, r, -48, 48, e=1.4, bouts=True)
            for r in (2.3, 4.0, 5.7)[:combien]]


def batterie(remplissage=None, dedans=None):
    """Corps + borne, et ce qu'on met dedans : un niveau, ou un signe.

    HORIZONTALE, comme sur la barre d'état de ChromeOS : la barre d'état de
    Claude OS est haute de deux lignes, une pile verticale y aurait été
    minuscule."""
    formes = [contour(1.2, 4.3, 11.8, 7.4, r=2.2),
              rect(13.6, 6.4, 1.2, 3.2, 0.6)]
    if remplissage:
        # Intérieur utile : de 2,85 à 11,35 — la borne exclue, le trait exclu.
        formes.append(rect(2.85, 5.95, 8.5 * remplissage, 4.1, 0.9))
    if dedans:
        formes.extend(dedans)
    return formes


def rune_bluetooth(cx=8.0):
    """Le losange double. Cinq segments, et pas un de plus : la rune est
    connue, la styliser la rend méconnaissable."""
    return [v(1.8, 14.2, cx),
            barre(cx, 1.8, cx + 3.4, 5.2),
            barre(cx + 3.4, 5.2, cx - 3.4, 10.8),
            barre(cx - 3.4, 5.2, cx + 3.4, 10.8),
            barre(cx + 3.4, 10.8, cx, 14.2)]


def eventail(combien, cx=8.0, cy=12.3):
    """Le Wi-Fi : un point et jusqu'à trois arcs. Le point reste TOUJOURS,
    même à zéro barre — sans lui, « aucun signal » ne se distingue pas d'une
    icône qui n'a pas fini de charger."""
    formes = [disque(cx, cy, 1.15)]
    formes += [arc(cx, cy, r, 218, 322, e=1.4, bouts=True)
               for r in (3.1, 5.6, 8.1)[:combien]]
    return formes


def cadenas(ouvert=False):
    """Cadenas plein, trou de serrure percé. Le même dessin sert au verrou de
    session et au cadenas des réseaux protégés : ce sont la même idée."""
    corps = rect(3.2, 7.0, 9.6, 6.9, 1.7)
    trou = cercle(8, 9.8, 1.0) + rect(7.45, 10.0, 1.1, 2.2, 0.5)
    anse = arc(8 if not ouvert else 10.4, 7.0, 2.7, 180, 360, e=1.4)
    return [troue(corps, trou), anse]


def engrenage(cx=8.0, cy=8.0, r=3.1, dents=8, longueur=2.3, e=2.1):
    """Roue dentée : un anneau, et des dents rayonnantes.

    Les dents sont des BARRES et non des trapèzes : à 16 px la différence ne
    se voit pas, et les barres tiennent la même épaisseur que le reste du
    jeu."""
    formes = [anneau(cx, cy, r - 0.35, e=1.5)]
    for i in range(dents):
        a = math.radians(i * 360.0 / dents)
        formes.append(barre(cx + (r - 0.2) * math.cos(a), cy + (r - 0.2) * math.sin(a),
                            cx + (r + longueur) * math.cos(a), cy + (r + longueur) * math.sin(a),
                            e=e))
    return formes


def ecran(x=1.4, y=2.6, w=13.2, hh=9.4):
    """Moniteur : dalle, pied, socle."""
    return [contour(x, y, w, hh, r=2.0),
            v(y + hh, y + hh + 1.6, x + w / 2, e=1.4),
            h(x + w / 2 - 2.6, x + w / 2 + 2.6, y + hh + 1.7, e=1.4)]


def dossier(ouvert=False):
    """Le dossier, plein. La languette occupe le tiers gauche : plus courte
    elle disparaît à 16 px, plus longue le dossier devient une boîte."""
    dos = (f"M3.0 3.3 H6.3 L7.9 5.1 H13.0 "
           f"A1.3 1.3 0 0 1 14.3 6.4 V11.9 "
           f"A1.3 1.3 0 0 1 13.0 13.2 H3.0 "
           f"A1.3 1.3 0 0 1 1.7 11.9 V4.6 "
           f"A1.3 1.3 0 0 1 3.0 3.3 Z")
    if not ouvert:
        return [dos]

    # OUVERT : LE DOS SE RÉDUIT À SON BANDEAU. Dessiné entier, il se fondait
    # avec le rabat — deux surfaces de même encre ne font qu'une silhouette,
    # et l'on voyait un dossier fermé. Il ne reste donc du dos que ce qui
    # dépasse du rabat, en haut et à gauche ; c'est ce décrochement, et lui
    # seul, qui dit que le dossier est ouvert.
    bandeau = ("M1.7 12.4 V4.6 A1.3 1.3 0 0 1 3.0 3.3 H6.3 L7.9 5.1 H13.0 "
               "A1.3 1.3 0 0 1 14.3 6.4 V7.4 H4.6 Z")
    rabat = "M4.2 8.0 H15.2 L12.9 13.4 H1.6 Z"
    return [bandeau, rabat]


def cadre_image():
    """Le cadre commun aux deux icônes d'image."""
    return contour(1.5, 3.0, 13.0, 10.0, r=2.0)


def barre_oblique():
    """La barre qui raye : « coupé », « absent », « désactivé ».

    TOUJOURS LE MÊME SENS, du bas-gauche vers le haut-droit. Deux sens
    mélangés dans un jeu d'icônes se lisent comme deux significations."""
    return barre(2.8, 13.2, 13.2, 2.8, e=1.5)


CATALOGUE = {}


def icone(nom, contexte, note):
    """Décorateur : la fonction décorée rend les formes de l'icône."""
    def prendre(f):
        CATALOGUE[nom] = (contexte, note, f)
        return f
    return prendre


# --------------------------------------------------------------------- son
@icone("audio-volume-muted-symbolic", "status", "volume coupé")
def _():
    return [haut_parleur(), barre(10.4, 5.6, 14.2, 10.4), barre(14.2, 5.6, 10.4, 10.4)]


@icone("audio-volume-low-symbolic", "status", "volume faible")
def _():
    return [haut_parleur()] + ondes(1)


@icone("audio-volume-medium-symbolic", "status", "volume moyen")
def _():
    return [haut_parleur()] + ondes(2)


@icone("audio-volume-high-symbolic", "status", "volume fort")
def _():
    return [haut_parleur()] + ondes(3)


@icone("audio-headphones-symbolic", "devices", "casque")
def _():
    return [arc(8, 8.4, 5.3, 182, 358, e=1.5),
            rect(1.7, 8.0, 3.0, 5.6, 1.4),
            rect(11.3, 8.0, 3.0, 5.6, 1.4)]


@icone("audio-speakers-symbolic", "devices", "enceintes")
def _():
    return [contour(3.4, 1.4, 9.2, 13.2, r=1.8),
            anneau(8, 10.0, 2.0, e=1.4),
            disque(8, 4.6, 0.85)]


# ----------------------------------------------------------------- batterie
def eclair(cx=7.1, cy=8.0, larg=2.4, haut=4.4):
    """L'éclair de la charge en cours."""
    base = [(0.18, -1.0), (-0.5, 0.22), (-0.06, 0.22),
            (-0.18, 1.0), (0.5, -0.22), (0.06, -0.22)]
    return polygone(*[(cx + px * larg, cy + py * haut / 2) for px, py in base])


# DEDANS : de 2,85 à 11,35. Au-delà commence le trait du corps, et la borne.
BAT_X0, BAT_X1 = 2.85, 11.35
BAT_ECLAIR = (5.5, 8.7)   # la bande que l'éclair se réserve


def batterie_niveau(pourcent, charge=False):
    """Un cran de la famille « battery-level-NNN ».

    LA BARRE EST COUPÉE EN DEUX AUTOUR DE L'ÉCLAIR, par arithmétique et non
    par découpe « pair-impair » : quand la charge est trop basse pour
    atteindre l'éclair, un trou pair-impair posé hors de la barre se serait
    dessiné comme une forme PLEINE — un second éclair fantôme à côté du
    vrai. Deux soustractions valent mieux qu'une règle de remplissage dont
    le résultat dépend de ce qu'on ignore."""
    fin = BAT_X0 + (BAT_X1 - BAT_X0) * pourcent / 100.0
    formes = [contour(1.2, 4.3, 11.8, 7.4, r=2.2), rect(13.6, 6.4, 1.2, 3.2, 0.6)]

    if not charge:
        if fin > BAT_X0 + 0.2:
            formes.append(rect(BAT_X0, 5.95, fin - BAT_X0, 4.1, 0.9))
        return formes

    g, d = BAT_ECLAIR
    if min(fin, g) > BAT_X0 + 0.2:
        formes.append(rect(BAT_X0, 5.95, min(fin, g) - BAT_X0, 4.1, 0.9))
    if fin > d + 0.2:
        formes.append(rect(d, 5.95, fin - d, 4.1, 0.9))
    formes.append(eclair())
    return formes


# LA FAMILLE EST ENGENDRÉE, ET C'EST INDISPENSABLE. shell/src/status.c ne
# demande pas un nom écrit en clair : il COMPOSE « battery-level-%d%s-symbolic »
# à partir de la charge arrondie à la dizaine. Un thème qui ne dessine que
# « battery-level-100 » laisse donc la barre d'état afficher, dix fois sur
# onze, la batterie d'un autre thème — ce qui est exactement arrivé ici le
# 14 septembre 2026, sur une machine à 67 %, et que le contrôle de couverture
# ne pouvait pas voir puisqu'il ne relève que les chaînes littérales.
for _pct in range(0, 101, 10):
    for _charge in (False, True):
        _nom = f"battery-level-{_pct}{'-charging' if _charge else ''}-symbolic"
        _note = f"batterie à {_pct} %" + (", en charge" if _charge else "")
        icone(_nom, "status", _note)(
            (lambda p, c: (lambda: batterie_niveau(p, c)))(_pct, _charge))


@icone("battery-good-symbolic", "status", "batterie, charge confortable")
def _():
    return batterie(0.66)


@icone("battery-low-symbolic", "status", "batterie faible")
def _():
    return batterie(0.22)


@icone("battery-caution-symbolic", "status", "batterie critique")
def _():
    return batterie(dedans=[v(6.0, 9.2, 8.0, e=1.5), disque(8.0, 10.5, 0.8)])


@icone("ac-adapter-symbolic", "devices", "alimentation secteur")
def _():
    """La fiche : deux broches, un corps, un cordon.

    PLEIN ET NON EN CONTOUR, contrairement au reste des contenants de ce jeu.
    Le corps d'une fiche ne contient rien qu'on doive voir : évidé, il se
    lisait comme une petite boîte posée entre deux traits. C'est la seule
    silhouette qui dise « prise » sans légende, et elle demande une masse.

    Ce n'est pas la prise murale : c'est la fiche. Les deux se valent en
    signification, la fiche se reconnaît mieux à 16 px — un rectangle percé
    de deux points est aussi bien un interrupteur."""
    return [v(1.4, 5.8, 5.9), v(1.4, 5.8, 10.1),
            rect(3.3, 5.0, 9.4, 5.6, 1.7),
            v(10.4, 14.6, 8)]


# ---------------------------------------------------------------- bluetooth
@icone("bluetooth-symbolic", "status", "Bluetooth")
def _():
    return rune_bluetooth()


@icone("bluetooth-active-symbolic", "status", "Bluetooth, appareil connecté")
def _():
    # La rune glisse à gauche pour laisser place aux deux ondes : c'est le
    # même décalage que le Wi-Fi fait entre « éteint » et « connecté ».
    return rune_bluetooth(5.6) + [arc(5.6, 8, 6.0, -40, 40, e=1.4, bouts=True),
                                  arc(5.6, 8, 8.0, -28, 28, e=1.4, bouts=True)]


@icone("bluetooth-disabled-symbolic", "status", "Bluetooth éteint")
def _():
    return rune_bluetooth() + [barre_oblique()]


# ------------------------------------------------------------------- réseau
@icone("network-wireless-signal-excellent-symbolic", "status", "Wi-Fi, plein signal")
def _():
    return eventail(3)


@icone("network-wireless-signal-good-symbolic", "status", "Wi-Fi, bon signal")
def _():
    return eventail(2)


@icone("network-wireless-signal-ok-symbolic", "status", "Wi-Fi, signal moyen")
def _():
    return eventail(1)


@icone("network-wireless-signal-weak-symbolic", "status", "Wi-Fi, signal faible")
def _():
    return eventail(0)


@icone("network-wireless-offline-symbolic", "status", "Wi-Fi éteint")
def _():
    return eventail(3) + [barre_oblique()]


@icone("network-offline-symbolic", "status", "aucun réseau")
def _():
    # LE MÉRIDIEN A ÉTÉ RETIRÉ, pas oublié. Anneau, équateur, méridien et
    # barre faisaient quatre traits qui se croisent dans un cercle de cinq
    # de rayon : à 16 px, cela ne se lisait plus comme un globe mais comme
    # une tache. Un globe se reconnaît à son ellipse ; c'est l'équateur,
    # redondant avec elle, qui est parti.
    return [anneau(8, 8, 5.0), ellipse_anneau(8, 8, 2.7, 5.0, e=1.4),
            barre_oblique()]


@icone("network-server-symbolic", "devices", "serveur de fichiers")
def _():
    return [contour(1.6, 2.6, 12.8, 4.6, r=1.4),
            contour(1.6, 8.8, 12.8, 4.6, r=1.4),
            disque(4.2, 4.9, 0.75), disque(4.2, 11.1, 0.75),
            h(6.4, 11.8, 4.9, e=1.1), h(6.4, 11.8, 11.1, e=1.1)]


@icone("channel-secure-symbolic", "status", "réseau protégé")
def _():
    return cadenas()


# ------------------------------------------------------------ écran, veille
@icone("display-brightness-symbolic", "status", "luminosité")
def _():
    formes = [anneau(8, 8, 2.6, e=1.5)]
    for i in range(8):
        a = math.radians(i * 45)
        formes.append(barre(8 + 4.5 * math.cos(a), 8 + 4.5 * math.sin(a),
                            8 + 6.3 * math.cos(a), 8 + 6.3 * math.sin(a), e=1.4))
    return formes


@icone("weather-clear-night-symbolic", "status", "nuit")
def _():
    return [troue(cercle(7.6, 8.2, 5.8), cercle(11.4, 4.6, 5.2))]


@icone("preferences-desktop-symbolic", "categories", "apparence du bureau")
def _():
    return ecran() + [h(3.9, 12.1, 5.6, e=1.2), disque(6.2, 5.6, 1.35),
                      h(3.9, 12.1, 9.0, e=1.2), disque(9.8, 9.0, 1.35)]


@icone("preferences-system-symbolic", "categories", "réglages du système")
def _():
    return engrenage()


@icone("application-x-executable-symbolic", "mimetypes", "programme")
def _():
    return [contour(2.0, 2.0, 12.0, 12.0, r=2.4), engrenage(8, 8, 2.0, 6, 1.2, 1.5)]


# ------------------------------------------------------------ fin de session
@icone("system-lock-screen-symbolic", "status", "écran verrouillé")
def _():
    return cadenas()


@icone("system-log-out-symbolic", "actions", "fermer la session")
def _():
    return [contour(1.6, 2.2, 6.4, 11.6, r=1.6)] + fleche(8.8, 8, 14.4, 8, tete=2.6)


@icone("system-reboot-symbolic", "actions", "redémarrer")
def _():
    return [arc(8, 8, 5.0, -52, 250, e=1.5, bouts=True),
            pointe(8 + 5.0 * math.cos(math.radians(-52)),
                   8 + 5.0 * math.sin(math.radians(-52)), -52 + 90, 3.0)]


@icone("view-refresh-symbolic", "actions", "actualiser")
def _():
    return CATALOGUE["system-reboot-symbolic"][2]()


@icone("system-shutdown-symbolic", "actions", "éteindre")
def _():
    return [arc(8, 8.7, 4.9, -60, 240, e=1.5, bouts=True), v(1.7, 7.6, 8, e=1.5)]


# ------------------------------------------------------------------- média
@icone("media-playback-start-symbolic", "actions", "lecture")
def _():
    return [polygone((4.6, 3.0), (13.0, 8.0), (4.6, 13.0))]


@icone("media-playback-pause-symbolic", "actions", "pause")
def _():
    return [rect(4.0, 3.0, 2.9, 10.0, 1.1), rect(9.1, 3.0, 2.9, 10.0, 1.1)]


@icone("media-playback-stop-symbolic", "actions", "arrêt")
def _():
    return [rect(3.4, 3.4, 9.2, 9.2, 1.6)]


@icone("media-skip-forward-symbolic", "actions", "piste suivante")
def _():
    return [polygone((3.0, 3.4), (10.0, 8.0), (3.0, 12.6)),
            rect(10.8, 3.4, 2.4, 9.2, 1.1)]


@icone("media-skip-backward-symbolic", "actions", "piste précédente")
def _():
    return [polygone((13.0, 3.4), (6.0, 8.0), (13.0, 12.6)),
            rect(2.8, 3.4, 2.4, 9.2, 1.1)]


@icone("media-eject-symbolic", "actions", "éjecter")
def _():
    return [polygone((8.0, 2.8), (13.4, 9.0), (2.6, 9.0)),
            rect(2.6, 10.8, 10.8, 2.4, 1.1)]


@icone("media-view-subtitles-symbolic", "actions", "sous-titres")
def _():
    return [contour(1.4, 3.0, 13.2, 10.0, r=2.0),
            h(3.6, 8.4, 8.6, e=1.2), h(9.6, 12.4, 8.6, e=1.2),
            h(3.6, 6.4, 10.8, e=1.2), h(7.6, 12.4, 10.8, e=1.2)]


# ---------------------------------------------------------------- fenêtres
@icone("view-app-grid-symbolic", "actions", "toutes les applications")
def _():
    return [disque(x, y, 1.3) for y in (4.2, 8.0, 11.8) for x in (4.2, 8.0, 11.8)]


@icone("view-fullscreen-symbolic", "actions", "plein écran")
def _():
    formes = []
    for sx, sy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        x = 8 + sx * -5.6
        y = 8 + sy * -5.6
        formes += [h(x, x + sx * 3.6, y, e=1.5), v(y, y + sy * 3.6, x, e=1.5)]
    return formes


@icone("view-restore-symbolic", "actions", "restaurer la fenêtre")
def _():
    formes = []
    for sx, sy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        x = 8 + sx * -2.0
        y = 8 + sy * -2.0
        formes += [h(x, x - sx * 3.6, y, e=1.5), v(y, y - sy * 3.6, x, e=1.5)]
    return formes


@icone("window-close-symbolic", "actions", "fermer la fenêtre")
def _():
    return [barre(3.9, 3.9, 12.1, 12.1), barre(12.1, 3.9, 3.9, 12.1)]


# ------------------------------------------------------------- déplacements
@icone("go-next-symbolic", "actions", "suivant")
def _():
    return chevron(8.4, 8, 3.4, "droite")


@icone("go-previous-symbolic", "actions", "précédent")
def _():
    return chevron(7.6, 8, 3.4, "gauche")


@icone("go-up-symbolic", "actions", "dossier parent")
def _():
    return chevron(8, 7.6, 3.4, "haut")


@icone("go-jump-symbolic", "actions", "aller à")
def _():
    return fleche(2.4, 8, 12.6, 8, tete=3.0)


@icone("object-select-symbolic", "actions", "choisi")
def _():
    return [barre(2.8, 8.4, 6.4, 12.0), barre(6.4, 12.0, 13.2, 4.2)]


@icone("value-increase-symbolic", "actions", "augmenter")
def _():
    return [h(2.8, 13.2, 8), v(2.8, 13.2, 8)]


@icone("value-decrease-symbolic", "actions", "diminuer")
def _():
    return [h(2.8, 13.2, 8)]


@icone("object-rotate-right-symbolic", "actions", "pivoter à droite")
def _():
    return [arc(8, 8.4, 5.0, -150, 120, e=1.5, bouts=True),
            pointe(8 + 5.0 * math.cos(math.radians(-150)),
                   8.4 + 5.0 * math.sin(math.radians(-150)), -150 + 90, 3.0)]


@icone("object-rotate-left-symbolic", "actions", "pivoter à gauche")
def _():
    return [arc(8, 8.4, 5.0, 60, 330, e=1.5, bouts=True),
            pointe(8 + 5.0 * math.cos(math.radians(330)),
                   8.4 + 5.0 * math.sin(math.radians(330)), 330 - 90, 3.0)]


# ------------------------------------------------------------- documents
@icone("document-edit-symbolic", "actions", "renommer, modifier")
def _():
    # UNE SEULE SILHOUETTE, DESSINÉE À PLAT PUIS TOURNÉE. Composé de trois
    # formes posées chacune à sa place — un fût, une virole, une pointe —
    # le crayon se disloquait : à 16 px, un demi-pixel d'écart entre deux
    # pièces se voit comme une fêlure. Le contour est donc continu, et la
    # virole n'est plus une pièce mais un trait posé en travers.
    corps = polygone((0, 0), (2.4, -1.45), (12.4, -1.45),
                     (12.4, 1.45), (2.4, 1.45))
    virole = rect(9.3, -1.45, 0.85, 2.9)
    tourne = "translate(3.1 12.9) rotate(-45)"
    # La virole est un VIDE et non une pièce : posée par-dessus, de la même
    # encre que le fût, elle ne se voyait pas.
    return [Forme(corps + virole, tourne, pair_impair=True)]


@icone("document-open-symbolic", "actions", "ouvrir")
def _():
    return dossier(ouvert=True)


@icone("folder-open-symbolic", "places", "dossier ouvert")
def _():
    return dossier(ouvert=True)


@icone("image-x-generic-symbolic", "mimetypes", "image")
def _():
    return [cadre_image(), disque(5.2, 6.4, 1.2),
            polygone((3.1, 12.0), (6.9, 7.6), (9.2, 10.2),
                     (10.9, 8.4), (12.9, 12.0))]


@icone("image-missing-symbolic", "status", "image illisible")
def _():
    return [cadre_image(), barre_oblique()]


@icone("input-keyboard-symbolic", "devices", "clavier à l'écran")
def _():
    touches = []
    for x in (3.4, 5.8, 8.2, 10.6):
        touches.append(rect(x - 0.75, 5.7, 1.5, 1.3, 0.5))
    for x in (4.0, 6.4, 8.8, 11.2):
        touches.append(rect(x - 0.75, 7.9, 1.5, 1.3, 0.5))
    touches.append(rect(4.6, 10.1, 6.8, 1.3, 0.6))
    return [contour(1.0, 3.8, 14.0, 8.4, r=1.8)] + touches


@icone("dialog-information-symbolic", "status", "information")
def _():
    return [anneau(8, 8, 5.5), disque(8, 5.0, 0.9), v(6.9, 11.4, 8, e=1.5)]


@icone("user-trash-symbolic", "places", "corbeille")
def _():
    dehors = polygone((3.3, 5.6), (12.7, 5.6), (11.8, 13.9), (4.2, 13.9))
    dedans = polygone((4.9, 7.1), (11.1, 7.1), (10.4, 12.4), (5.6, 12.4))
    return [h(2.2, 13.8, 4.4, e=1.5),
            rect(6.2, 1.9, 3.6, 1.5, 0.6),
            troue(dehors, dedans)]


@icone("phone-symbolic", "devices", "téléphone")
def _():
    return [contour(4.2, 1.2, 7.6, 13.6, r=1.8), h(6.6, 9.4, 12.6, e=1.1)]


# ------------------------------------------------------- profils d'énergie
def cadran(angle_aiguille):
    """Les trois profils d'énergie ne diffèrent QUE par l'angle de
    l'aiguille : c'est ce qui les fait lire comme une échelle et non comme
    trois symboles sans rapport."""
    a = math.radians(angle_aiguille)
    return [arc(8, 10.4, 5.2, 180, 360, e=1.5, bouts=True),
            barre(8, 10.4, 8 + 4.0 * math.cos(a), 10.4 + 4.0 * math.sin(a), e=1.5),
            disque(8, 10.4, 1.1)]


@icone("power-profile-power-saver-symbolic", "status", "profil économe")
def _():
    return cadran(214)


@icone("power-profile-balanced-symbolic", "status", "profil équilibré")
def _():
    return cadran(270)


@icone("power-profile-performance-symbolic", "status", "profil performance")
def _():
    return cadran(326)


# ===========================================================================
# LES ICÔNES LIVRÉES AVEC LE SHELL
#
# Elles ne vont PAS dans le thème de la distribution, mais dans
# shell/data/icons/hicolor, d'où le shell les charge lui-même — et c'est la
# règle établie par la cloche du centre de notifications le 8 septembre 2026 :
# une icône dont le shell ne peut pas se passer ne doit pas dépendre du thème
# d'icônes choisi. Qui bascule sur Adwaita garde ses trois modes d'énergie ;
# sans cela il verrait trois carrés barrés là où le bureau dit ce que la
# machine est en train de faire.
#
# Toutes préfixées « claude-os- » : elles ne masquent donc rien.
# ===========================================================================

LIVREES = {}


def livree(nom, note):
    def prendre(f):
        LIVREES[nom] = (note, f)
        return f
    return prendre


def lettre(c, x, y, w, hh, e=0.55):
    """Quatre lettres, dessinées au trait, pour le badge « AUTO ».

    PAS DE <text> DANS LE SVG. GTK recolore « rect, circle, path, polygon »
    et rien d'autre : un élément texte serait resté noir sur un thème sombre.
    Il aurait fallu en plus qu'une police précise soit installée, et qu'elle
    donne les mêmes chasses partout. Quatre lettres géométriques coûtent
    moins cher que cette dépendance."""
    d = x + w / 2
    if c == "A":
        return [barre(x, y + hh, d, y, e), barre(d, y, x + w, y + hh, e),
                h(x + w * 0.2, x + w * 0.8, y + hh * 0.66, e)]
    if c == "U":
        return [v(y, y + hh - w / 2, x, e), v(y, y + hh - w / 2, x + w, e),
                arc(d, y + hh - w / 2, w / 2, 0, 180, e=e, bouts=True)]
    if c == "T":
        return [h(x, x + w, y, e), v(y, y + hh, d, e)]
    if c == "O":
        return [Forme(rect(x, y, w, hh, w / 2)
                      + rect(x + e, y + e, w - 2 * e, hh - 2 * e,
                             max(w / 2 - e, 0.2)),
                      pair_impair=True)]
    return []


@livree("claude-os-mode-travail-symbolic", "mode Travail — le cadran")
def _():
    """Le demi-cadran, aiguille au plus haut.

    TROIS OBJETS DIFFERENTS, ET NON TROIS NUANCES DU MEME. Les trois modes
    ont d'abord eu trois cadrans que seule l'inclinaison d'une aiguille
    distinguait : c'est ce que dessine Papirus, et le projet l'avait deja
    ecarte le 11 septembre 2026 — « à 18 px on ne les distingue pas »
    (docs/07). Le coin les montre UN par UN, en grand et sans libellé :
    l'exigence y est encore plus forte qu'en Console.

    D'où cette famille-ci, qui est celle d'Adwaita : cadran, balance,
    feuille. Trois objets sans rapport, reconnaissables seuls — et c'est
    l'ancienne, celle que le bureau portait avant d'avoir son propre jeu.

    Le demi-cadran plutôt que le compteur rond entier : c'est la forme
    d'Adwaita, et l'aiguille y a plus de course visible pour un même
    diamètre."""
    a = math.radians(-38)
    bout = (8 + 3.6 * math.cos(a), 10.6 + 3.6 * math.sin(a))
    return [arc(8, 10.6, 5.4, 180, 360, e=1.5, bouts=True),
            barre(8, 10.6, bout[0], bout[1], e=1.4),
            pointe(8 + 4.6 * math.cos(a), 10.6 + 4.6 * math.sin(a), -38, 2.6),
            disque(8, 10.6, 1.2)]


@livree("claude-os-mode-nomade-symbolic", "mode Nomade — la feuille")
def _():
    """Une feuille, nervures comprises. Le signe universel de l'économie,
    et le seul des trois qui ne parle pas de mesure. C'est aussi celui de
    l'ancienne famille qu'on garde tel quel : il n'y avait rien à lui
    reprocher."""
    def contour_feuille(k):
        # Deux arcs opposés : la forme en amande. « k » rentre la pointe
        # pour obtenir la feuille intérieure, donc le contour.
        x0, y0 = 2.6 + k, 13.4 - k
        x1, y1 = 13.4 - k, 2.6 + k
        c = 5.6 - k * 1.6
        return (f"M{n(x0)} {n(y0)}"
                f"C{n(x0)} {n(y0 - c)} {n(x1 - c)} {n(y1)} {n(x1)} {n(y1)}"
                f"C{n(x1)} {n(y1 + c)} {n(x0 + c)} {n(y0)} {n(x0)} {n(y0)}Z")
    formes = [Forme(contour_feuille(0) + contour_feuille(1.25), pair_impair=True),
              barre(3.4, 12.6, 11.6, 4.4, e=0.9)]
    # Trois nervures d'un seul côté, de plus en plus courtes vers la pointe.
    #
    # PERPENDICULAIRES À LA CÔTE, ET C'EST TOUT LE SUJET. Posées à -100° au
    # lieu de -135°, elles montaient presque droit, traversaient la feuille
    # de part en part, et le dessin devenait un épi. Une nervure part de la
    # côte et va vers le BORD : son angle se déduit de celui de la côte,
    # moins un quart de tour.
    cote = math.degrees(math.atan2(4.4 - 12.6, 11.6 - 3.4))   # -45°
    for pos, lon in ((0.28, 2.2), (0.46, 1.9), (0.64, 1.5)):
        bx = 3.4 + (11.6 - 3.4) * pos
        by = 12.6 + (4.4 - 12.6) * pos
        a = math.radians(cote - 90)
        formes.append(barre(bx, by, bx + lon * math.cos(a), by + lon * math.sin(a),
                            e=0.75))
    return formes


@livree("claude-os-mode-automatique-symbolic", "mode Automatique — la balance")
def _():
    """Une balance à deux plateaux.

    ELLE REMPLACE UN BADGE « AUTO ». Le badge disait la chose par un mot, ce
    qui est l'aveu qu'on n'a pas trouvé l'image : quatre lettres à déchiffrer
    dans un coin d'écran, et illisibles dès qu'on descend à la taille de la
    Console. La balance, elle, dit « l'équilibre » sans être lue — et c'est
    exactement ce que ce mode est : ni le cadran poussé, ni la feuille.

    C'est aussi celle de l'ancienne famille, que l'utilisateur est venu
    rechercher.

    LE FLÉAU EST HORIZONTAL, et cela compte : une balance penchée dirait
    qu'un plateau l'emporte, c'est-à-dire le contraire d'un équilibre."""
    return [# le fléau, et le pivot dessous
            h(2.2, 13.8, 5.4, e=1.3),
            v(5.4, 11.4, 8, e=1.3),
            # le socle : un triangle, la seule forme qui tienne debout
            polygone((5.2, 13.6), (10.8, 13.6), (8, 10.6)),
            # les deux plateaux, suspendus aux extrémités
            arc(3.6, 6.2, 2.4, 0, 180, e=1.2, bouts=True),
            arc(12.4, 6.2, 2.4, 0, 180, e=1.2, bouts=True),
            v(5.4, 6.2, 3.6, e=0.7),
            v(5.4, 6.2, 12.4, e=0.7)]


# ===========================================================================
# LES ICÔNES EN COULEUR
#
# Applications, dossiers, types de fichiers. Elles ne sont pas recolorées par
# GTK — elles portent leurs couleurs.
#
# LE GLYPHE EST EXACTEMENT CELUI DES ICÔNES SYMBOLIQUES, agrandi par une
# transformation. C'est le choix qui fait tenir le jeu ensemble : le dossier
# du dock et le dossier de la barre latérale sont le MÊME dessin, à l'échelle
# près, et non deux interprétations du même objet par deux mains différentes.
# En prime, corriger un glyphe corrige les deux.
# ===========================================================================

IVOIRE = "#faf9f5"   # le glyphe posé sur une teinte, dans les trois familles

# Les teintes, prises aux deux chartes du projet : la palette de marque
# d'Anthropic pour les chaudes, celle des couleurs de contraste pour les
# froides. Chaque entrée est un couple (haut, bas) : le dégradé descend
# toujours vers le plus sombre, jamais l'inverse — une lumière qui vient
# d'en haut est la seule que l'œil ne discute pas.
TEINTES = {
    "argile":    ("#e08a6a", "#c05f3e"),
    "kraft":     ("#dda171", "#c07d4d"),
    "ardoise":   ("#7d8ea1", "#5a6877"),
    "bleu":      ("#4a87e6", "#2a62bd"),
    "turquoise": ("#2fa0b0", "#0d6e7c"),
    "emeraude":  ("#3ba07a", "#1e7053"),
    "violet":    ("#a273e0", "#7a4cb6"),
    "framboise": ("#d7639a", "#ab3a69"),
    "ocre":      ("#b98a34", "#82600f"),
    "encre":     ("#4a4a46", "#262623"),
    "brique":    ("#cf6352", "#a33a2c"),
}

TUILE = 64.0        # côté des icônes en couleur
ECHELLE = 2.5       # le glyphe de 16 porté à 40
DECALE = (TUILE - 16 * ECHELLE) / 2


def tuile(teinte, r=15.0):
    """Le carré arrondi des applications. Rayon de 15 sur 64 : la forme de
    ChromeOS, plus douce qu'un carré et franchement moins ronde qu'un
    galet."""
    return rect(2.0, 2.0, TUILE - 4, TUILE - 4, r)


def glyphe(formes, echelle=ECHELLE, dx=None, dy=None):
    """Reprend un dessin de la grille de 16 et le pose sur la tuile."""
    dx = DECALE if dx is None else dx
    dy = DECALE if dy is None else dy
    return (aplatir(formes), f"translate({n(dx)} {n(dy)}) scale({n(echelle)})")


def composer(calques, taille=TUILE, note="", degrade=None):
    """Un fichier à plusieurs couches : chaque calque est (formes, couleur,
    transformation-ou-None)."""
    corps = []
    if degrade is not None:
        haut, bas = degrade
        corps.append(
            '  <defs><linearGradient id="f" x1="0" y1="0" x2="0" y2="1">'
            f'<stop offset="0" stop-color="{haut}"/>'
            f'<stop offset="1" stop-color="{bas}"/></linearGradient></defs>')
    for formes, couleur, tr in calques:
        dedans = []
        for f in aplatir(formes):
            regle = ' fill-rule="evenodd"' if f.pair_impair else ""
            p = f'<path d="{f.d}" fill="{couleur}"{regle}/>'
            dedans.append(f'<g transform="{f.tr}">{p}</g>' if f.tr else p)
        corps.append(f'  <g transform="{tr}">{"".join(dedans)}</g>' if tr
                     else "  " + "".join(dedans))

    tete = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{n(taille)}" '
            f'height="{n(taille)}" viewBox="0 0 {n(taille)} {n(taille)}">')
    if note:
        tete += f"\n  <!-- Claude OS — {note} -->"
    return "\n".join(['<?xml version="1.0" encoding="UTF-8"?>', tete]
                     + corps + ["</svg>", ""])


COULEURS = {}


def icone_couleur(nom, contexte, note):
    def prendre(f):
        COULEURS[nom] = (contexte, note, f)
        return f
    return prendre


# ------------------------------------------------------- quelques glyphes
def maison():
    toit = polygone((8, 1.9), (14.6, 7.4), (12.9, 7.4), (8, 3.7), (3.1, 7.4), (1.4, 7.4))
    murs = polygone((3.0, 7.0), (13.0, 7.0), (13.0, 14.2), (3.0, 14.2))
    porte = rect(6.6, 9.6, 2.8, 4.6, 0.5)
    return [toit, troue(murs, porte)]


def globe():
    return [anneau(8, 8, 5.4), h(2.6, 13.4, 8, e=1.4),
            ellipse_anneau(8, 8, 2.9, 5.4, e=1.4)]


def note_musique():
    return [v(4.4, 12.0, 11.4, e=1.5), h(11.4, 13.6, 4.4, e=1.5),
            v(4.4, 4.9, 13.6, e=1.5), disque(9.6, 12.0, 2.0)]


def page(corne=3.6):
    """La feuille des types de fichiers, coin replié."""
    x, y, w, hh = 3.0, 1.6, 10.0, 12.8
    silhouette = (f"M{n(x + 1.2)} {n(y)}H{n(x + w - corne)}L{n(x + w)} {n(y + corne)}"
                  f"V{n(y + hh - 1.2)}A1.2 1.2 0 0 1 {n(x + w - 1.2)} {n(y + hh)}"
                  f"H{n(x + 1.2)}A1.2 1.2 0 0 1 {n(x)} {n(y + hh - 1.2)}"
                  f"V{n(y + 1.2)}A1.2 1.2 0 0 1 {n(x + 1.2)} {n(y)}Z")
    return silhouette


def invite():
    """L'invite du terminal : un chevron et un tiret de saisie."""
    return chevron(5.4, 6.6, 2.6, "droite") + [h(8.4, 12.6, 10.6, e=1.5)]


def disque_dur():
    """Le cylindre à trois disques du stockage.

    UN BOÎTIER RECTANGULAIRE AVAIT ÉTÉ ESSAYÉ D'ABORD : posé sur une tuile,
    il occupait tout le carré et se lisait comme un badge, pas comme un
    disque. Le cylindre, lui, a une silhouette qui n'appartient qu'à lui."""
    rx, ry = 5.0, 1.9
    def ellipse(cy):
        return (f"M{n(8 - rx)} {n(cy)}A{n(rx)} {n(ry)} 0 0 0 {n(8 + rx)} {n(cy)}"
                f"A{n(rx)} {n(ry)} 0 0 0 {n(8 - rx)} {n(cy)}Z")
    flancs = (f"M{n(8 - rx)} {n(4.2)}V{n(11.8)}"
              f"A{n(rx)} {n(ry)} 0 0 0 {n(8 + rx)} {n(11.8)}V{n(4.2)}Z")
    # Les deux rainures sont DÉCOUPÉES dans le cylindre, et non posées
    # dessus : le glyphe est d'une seule encre, une ellipse posée par-dessus
    # aurait été invisible. « pair-impair » compte les traversées — la bande
    # entre les deux ellipses d'une rainure en compte deux, donc vide ; ce
    # qu'elle entoure en compte trois, donc plein.
    rainures = "".join(ellipse(cy) + ellipse(cy - 1.1)
                       for cy in (7.4, 10.0))
    return [Forme(flancs + ellipse(4.2) + rainures, pair_impair=True)]


def grappe():
    """Trois postes reliés : le voisinage réseau."""
    return [disque(8, 3.4, 2.0), disque(3.6, 12.4, 2.0), disque(12.4, 12.4, 2.0),
            barre(8, 5.4, 4.4, 10.6, e=1.2), barre(8, 5.4, 11.6, 10.6, e=1.2)]


# ------------------------------------------------------------ applications
def app(teinte, formes, echelle=ECHELLE):
    """Une application : la tuile teintée, le glyphe ivoire par-dessus."""
    g, tr = glyphe(formes, echelle,
                   (TUILE - 16 * echelle) / 2, (TUILE - 16 * echelle) / 2)
    return [([tuile(teinte)], "url(#f)", None), (g, IVOIRE, tr)], TEINTES[teinte]


@icone_couleur("system-file-manager", "apps", "Fichiers")
def _():
    return app("argile", dossier())


@icone_couleur("preferences-system", "apps", "Réglages")
def _():
    return app("ardoise", engrenage())


@icone_couleur("image-viewer", "apps", "Images")
def _():
    return app("turquoise", CATALOGUE["image-x-generic-symbolic"][2]())


@icone_couleur("video-player", "apps", "Vidéo")
def _():
    return app("violet", [polygone((4.8, 3.0), (13.0, 8.0), (4.8, 13.0))])


@icone_couleur("utilities-terminal", "apps", "Terminal")
def _():
    return app("encre", invite())


@icone_couleur("web-browser", "apps", "Navigateur")
def _():
    return app("bleu", globe())


@icone_couleur("text-editor", "apps", "Éditeur de texte")
def _():
    return app("ocre", [page(), Forme(h(5.0, 11.0, 6.6, e=1.2)),
                        Forme(h(5.0, 11.0, 9.0, e=1.2)),
                        Forme(h(5.0, 9.0, 11.4, e=1.2))])


@icone_couleur("preferences-desktop", "apps", "Apparence")
def _():
    return app("framboise", CATALOGUE["preferences-desktop-symbolic"][2]())


# ----------------------------------------------------------------- dossiers
def porte_dossier(teinte, marque=None):
    """Un dossier, éventuellement marqué.

    LA MARQUE EST PETITE ET CENTRÉE DANS LE CORPS, jamais posée en coin : un
    dossier « Images » se reconnaît à sa marque, et une marque de coin
    disparaît la première quand la vue passe en petites icônes."""
    # Le dossier n'est pas un carré : il est posé par son CENTRE GÉOMÉTRIQUE
    # et non par la marge de la tuile, sinon il flotte en haut du carré.
    calques = [(aplatir(dossier()), "url(#f)", "translate(8 7.25) scale(3)")]
    if marque is not None:
        # La marque se centre sur le CORPS du dossier (sous la languette),
        # pas sur le dossier entier : centrée sur l'ensemble, elle mordait
        # sur la languette et le dossier cessait de se lire.
        e = 1.5
        calques.append((aplatir(marque), IVOIRE,
                        f"translate(20 22.7) scale({n(e)})"))
    return calques, TEINTES[teinte]


@icone_couleur("folder", "places", "dossier")
def _():
    return porte_dossier("kraft")


@icone_couleur("inode-directory", "places", "dossier")
def _():
    return porte_dossier("kraft")


@icone_couleur("folder-documents", "places", "Documents")
def _():
    return porte_dossier("kraft", [page(corne=2.8)])


@icone_couleur("folder-download", "places", "Téléchargements")
def _():
    return porte_dossier("kraft", fleche(8, 3.0, 8, 12.4, tete=3.4, e=1.8))


@icone_couleur("folder-pictures", "places", "Images")
def _():
    return porte_dossier("kraft", CATALOGUE["image-x-generic-symbolic"][2]())


@icone_couleur("folder-music", "places", "Musique")
def _():
    return porte_dossier("kraft", note_musique())


@icone_couleur("folder-videos", "places", "Vidéos")
def _():
    return porte_dossier("kraft", [polygone((4.8, 3.0), (13.0, 8.0), (4.8, 13.0))])


@icone_couleur("folder-network", "places", "dossier partagé")
def _():
    return porte_dossier("ardoise", grappe())


@icone_couleur("folder-remote", "places", "lecteur réseau monté")
def _():
    return porte_dossier("ardoise", globe())


@icone_couleur("user-home", "places", "dossier personnel")
def _():
    return app("argile", maison())


@icone_couleur("user-desktop", "places", "Bureau")
def _():
    return app("ardoise", ecran())


@icone_couleur("user-trash", "places", "corbeille")
def _():
    return app("ardoise", CATALOGUE["user-trash-symbolic"][2]())


@icone_couleur("drive-harddisk", "devices", "disque")
def _():
    return app("ardoise", disque_dur())


@icone_couleur("network-workgroup", "places", "voisinage réseau")
def _():
    return app("ardoise", grappe())


@icone_couleur("network-server", "places", "serveur")
def _():
    return app("ardoise", CATALOGUE["network-server-symbolic"][2]())


# ------------------------------------------------------------ types de fichiers
def type_fichier(teinte, marque=None):
    """La feuille, et ce qui dit ce qu'elle contient."""
    calques = [(aplatir([page()]), "url(#f)",
                f"translate({n(DECALE)} {n(DECALE)}) scale({n(ECHELLE)})")]
    if marque is not None:
        e = 1.5
        dx = (TUILE - 16 * e) / 2
        calques.append((aplatir(marque), IVOIRE,
                        f"translate({n(dx)} {n(dx + 4)}) scale({n(e)})"))
    return calques, TEINTES[teinte]


@icone_couleur("text-x-generic", "mimetypes", "texte")
def _():
    return type_fichier("ardoise", [Forme(h(4.2, 11.8, 5.6, e=1.3)),
                                    Forme(h(4.2, 11.8, 8.2, e=1.3)),
                                    Forme(h(4.2, 9.4, 10.8, e=1.3))])


@icone_couleur("text-x-script", "mimetypes", "script")
def _():
    return type_fichier("turquoise", invite())


@icone_couleur("image-x-generic", "mimetypes", "image")
def _():
    return type_fichier("emeraude", CATALOGUE["image-x-generic-symbolic"][2]())


@icone_couleur("video-x-generic", "mimetypes", "vidéo")
def _():
    return type_fichier("violet", [polygone((5.0, 3.4), (12.6, 8.0), (5.0, 12.6))])


@icone_couleur("audio-x-generic", "mimetypes", "son")
def _():
    return type_fichier("framboise", note_musique())


@icone_couleur("application-pdf", "mimetypes", "PDF")
def _():
    return type_fichier("brique", [Forme(h(3.4, 12.6, 6.0, e=1.4)),
                                   Forme(h(3.4, 12.6, 9.0, e=1.4)),
                                   Forme(h(3.4, 8.4, 12.0, e=1.4))])


@icone_couleur("application-x-executable", "mimetypes", "programme")
def _():
    return type_fichier("ocre", engrenage(8, 8, 2.6, 8, 1.6, 1.7))


@icone_couleur("package-x-generic", "mimetypes", "archive")
def _():
    return type_fichier("argile", [contour(3.0, 4.4, 10.0, 8.4, r=1.4),
                                   Forme(h(3.0, 13.0, 7.4, e=1.4)),
                                   Forme(v(4.4, 7.4, 8.0, e=1.4))])


# ===========================================================================
# Écriture du thème
# ===========================================================================
CONTEXTES = {
    "actions":    "Actions",
    "apps":       "Applications",
    "categories": "Categories",
    "devices":    "Devices",
    "mimetypes":  "MimeTypes",
    "places":     "Places",
    "status":     "Status",
}

INDEX_TETE = """# Claude OS — le thème d'icônes de la distribution.
#
# ENGENDRÉ PAR tools/fabrique-icones.py. Ne pas modifier à la main : la
# prochaine exécution réécrirait le répertoire entier.
#
# INHERITS=Papirus : ce thème couvre ce que Claude OS AFFICHE — les {n_sym}
# pictogrammes que le shell demande à GTK, les dossiers de la barre latérale
# de Fichiers, les types de fichiers courants et les applications du dock.
# Il ne couvre pas les milliers d'icônes du reste du monde, et ne prétend pas
# le faire : tout ce qu'il ne dessine pas retombe sur Papirus, exactement
# comme avant. Un thème qui aurait voulu tout redessiner aurait surtout
# affiché des carrés barrés.
#
# Les applications d'éditeurs — Chromium, Claude Desktop — gardent DÉLIBÉ-
# RÉMENT leur propre icône : redessiner la marque de quelqu'un d'autre n'est
# pas une question de style.

[Icon Theme]
Name=Claude OS
Comment=Icônes dessinées pour Claude OS — {n_sym} symboliques, {n_col} en couleur
Inherits=Papirus,hicolor
Example=system-file-manager
"""


def ecrire_theme(racine, bavard=True):
    """(Re)construit le répertoire du thème. Il est EFFACÉ d'abord : une
    icône retirée du catalogue doit disparaître du thème, sinon elle
    continuerait de s'afficher sans que rien ne la produise."""
    if os.path.isdir(racine):
        shutil.rmtree(racine)

    repertoires = []
    ecrits = []

    for nom, (ctx, note, fonction) in sorted(CATALOGUE.items()):
        rep = f"symbolic/{ctx}"
        os.makedirs(os.path.join(racine, rep), exist_ok=True)
        if rep not in repertoires:
            repertoires.append(rep)
        chemin = os.path.join(racine, rep, nom + ".svg")
        with open(chemin, "w", encoding="utf-8") as f:
            f.write(svg(fonction(), note=note))
        ecrits.append(chemin)

    for nom, (ctx, note, fonction) in sorted(COULEURS.items()):
        rep = f"scalable/{ctx}"
        os.makedirs(os.path.join(racine, rep), exist_ok=True)
        if rep not in repertoires:
            repertoires.append(rep)
        calques, degrade = fonction()
        chemin = os.path.join(racine, rep, nom + ".svg")
        with open(chemin, "w", encoding="utf-8") as f:
            f.write(composer(calques, note=note, degrade=degrade))
        ecrits.append(chemin)

    lignes = [INDEX_TETE.format(n_sym=len(CATALOGUE), n_col=len(COULEURS)),
              "Directories=" + ",".join(sorted(repertoires)), ""]
    for rep in sorted(repertoires):
        symbolique = rep.startswith("symbolic")
        ctx = CONTEXTES[rep.split("/")[1]]
        lignes += [f"[{rep}]",
                   f"Context={ctx}",
                   f"Size={16 if symbolique else 64}",
                   "MinSize=8" if symbolique else "MinSize=16",
                   "MaxSize=512",
                   "Type=Scalable",
                   ""]
    with open(os.path.join(racine, "index.theme"), "w", encoding="utf-8") as f:
        f.write("\n".join(lignes))

    if bavard:
        print(f"{len(ecrits)} icônes écrites dans {racine}")
    return ecrits


def ecrire_livrees(racine):
    """Écrit les icônes que le shell porte lui-même.

    LE RÉPERTOIRE N'EST PAS EFFACÉ, contrairement à celui du thème : la
    cloche du centre de notifications y vit, écrite à la main le 8 septembre
    2026, et son fichier porte l'explication d'un piège que personne ne doit
    avoir à repayer. On écrit donc fichier par fichier."""
    os.makedirs(racine, exist_ok=True)
    ecrits = []
    for nom, (note, fonction) in sorted(LIVREES.items()):
        chemin = os.path.join(racine, nom + ".svg")
        with open(chemin, "w", encoding="utf-8") as f:
            f.write(svg(fonction(), note=note))
        ecrits.append(chemin)
    print(f"{len(ecrits)} icônes livrées avec le shell, dans {racine}")
    return ecrits


# ---------------------------------------------------------------------------
# Couverture : ce que le shell demande, et ce que le thème dessine
# ---------------------------------------------------------------------------
def couverture(racine_depot):
    """Relève les noms d'icônes cités dans le C du shell et dit lesquels ce
    thème ne dessine pas.

    CE CONTRÔLE EXISTE PARCE QUE L'ERREUR EST MUETTE. Une icône absente du
    thème ne provoque rien : GTK descend dans Papirus et affiche autre chose.
    On obtient un bureau presque cohérent, avec trois pictogrammes d'une
    autre main — et personne ne sait dire lesquels sans les compter."""
    import re
    demandes = set()
    src = os.path.join(racine_depot, "shell", "src")
    motif = re.compile(r'"([a-z][a-z0-9-]*-symbolic)"')
    for nom in sorted(os.listdir(src)):
        if not nom.endswith(".c"):
            continue
        with open(os.path.join(src, nom), encoding="utf-8", errors="replace") as f:
            demandes.update(motif.findall(f.read()))

    # Les icônes que le shell se fournit à lui-même ne regardent pas ce
    # thème : elles sont dans shell/data/icons, et ecrire_livrees() les
    # écrit. On vérifie tout de même qu'elles EXISTENT, pour la même raison
    # qu'on vérifie les autres -- une absence ne provoque rien.
    livrees = {d for d in demandes if d.startswith("claude-os-")}
    for d in sorted(livrees):
        if d not in LIVREES and d != "claude-os-cloche-symbolic":
            print(f"  ATTENTION : {d} demandé par le shell, dessiné nulle part")
    demandes -= livrees

    # LES NOMS COMPOSÉS À L'EXÉCUTION ÉCHAPPENT À CE RELEVÉ, et il faut le
    # dire plutôt que de les laisser passer pour des icônes en trop.
    # status.c compose « battery-level-%d%s-symbolic » depuis la charge : le
    # grep ne voit que le gabarit, jamais les vingt-deux noms réels.
    composes = re.compile(r"^battery-level-\d+(-charging)?-symbolic$")
    demandes |= {k for k in CATALOGUE if composes.match(k)}

    manquantes = sorted(d for d in demandes if d not in CATALOGUE)
    inutilisees = sorted(k for k in CATALOGUE if k not in demandes)
    return sorted(demandes), manquantes, inutilisees


# ---------------------------------------------------------------------------
# Planche de contact
# ---------------------------------------------------------------------------
def planche(racine, sortie, fond="#1f1f1d", encre="#faf9f5"):
    """Rend toutes les icônes côte à côte, en PNG.

    ON REGARDE LES QUATRE-VINGT-QUINZE D'UN SEUL COUP, et c'est le seul moyen
    de voir ce qui dépareille : une épaisseur qui décroche, un glyphe trop
    grand de deux dixièmes, un dessin qui ne se lit pas à 16 px. Icône par
    icône, tout paraît toujours correct."""
    # GdkPixbuf SEUL, ET PAS CAIRO. Composer la planche avec pycairo aurait
    # été plus direct, mais aurait ajouté un paquet à une machine qui compte
    # ses mégaoctets — pour un outil de relecture qui ne tourne jamais en
    # session. GdkPixbuf est déjà là : il charge le SVG par librsvg et sait
    # superposer.
    import gi
    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf, GLib, Gio

    fichiers = []
    racines = [racine]
    # Les icônes livrées avec le shell figurent sur la planche : elles font
    # partie du même jeu, et c'est ensemble qu'on les juge.
    depot = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    racines.append(os.path.join(depot, "shell", "data", "icons"))
    for r in racines:
        for rep, _, noms in os.walk(r):
            for nom in sorted(noms):
                if nom.endswith(".svg"):
                    fichiers.append((os.path.join(rep, nom),
                                     "symbolic" in rep or "status" in rep,
                                     nom[:-4]))
    fichiers.sort(key=lambda t: (not t[1], t[2]))

    CASE, COTE, COLS = 74, 48, 12
    lignes = (len(fichiers) + COLS - 1) // COLS
    planche = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, True, 8,
                                   CASE * COLS, CASE * lignes)
    r, g, b = (int(fond[i:i + 2], 16) for i in (1, 3, 5))
    planche.fill((r << 24) | (g << 16) | (b << 8) | 0xFF)

    for i, (chemin, est_sym, nom) in enumerate(fichiers):
        x = (i % COLS) * CASE + (CASE - COTE) // 2
        y = (i // COLS) * CASE + (CASE - COTE) // 2
        donnees = open(chemin, "rb").read()
        if est_sym:
            # On imite ce que GTK fait : la feuille de style qui impose la
            # couleur du texte. Sans elle, la planche montrerait l'encre de
            # secours et non ce qu'on verra à l'écran.
            donnees = donnees.replace(b'fill="#222222"',
                                      b'fill="' + encre.encode() + b'"')
        flux = Gio.MemoryInputStream.new_from_bytes(GLib.Bytes.new(donnees))
        pb = GdkPixbuf.Pixbuf.new_from_stream_at_scale(flux, COTE, COTE, True, None)
        pb.composite(planche, x, y, pb.get_width(), pb.get_height(), x, y,
                     1.0, 1.0, GdkPixbuf.InterpType.BILINEAR, 255)

    planche.savev(sortie, "png", [], [])
    print(f"planche de contact : {sortie} ({len(fichiers)} icônes)")


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--vers", default=None,
                    help="répertoire du thème (défaut : rootfs/usr/share/icons/Claude-OS)")
    ap.add_argument("--planche", metavar="PNG", nargs="?", const="planche-icones.png",
                    help="écrit aussi une planche de contact")
    args = ap.parse_args()

    depot = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    racine = args.vers or os.path.join(depot, "rootfs", "usr", "share",
                                       "icons", "Claude-OS")
    ecrire_theme(racine)
    ecrire_livrees(os.path.join(depot, "shell", "data", "icons", "hicolor",
                                "scalable", "status"))

    demandes, manquantes, inutilisees = couverture(depot)
    print(f"le shell demande {len(demandes)} icônes symboliques ; "
          f"{len(demandes) - len(manquantes)} sont dessinées ici")
    if manquantes:
        print("  NON DESSINÉES (elles retomberont sur Papirus) :")
        for m in manquantes:
            print("   ", m)
    if inutilisees:
        print("  dessinées sans que le shell les demande :")
        for i in inutilisees:
            print("   ", i)

    if args.planche:
        planche(racine, args.planche)
    return 0 if not manquantes else 0


if __name__ == "__main__":
    sys.exit(main())
