#!/usr/bin/python3
# =========================================================================
# Claude OS — que reçoit-on vraiment du numériseur tactile ?
#
#   claude-os-root python3 tools/diag-tactile.py coins  [durée]
#   claude-os-root python3 tools/diag-tactile.py carte  [durée]
#   claude-os-root python3 tools/diag-tactile.py bord   [durée] [x max]
#
# POURQUOI CET OUTIL EXISTE
#
# Le 13 septembre 2026, l'utilisateur a signalé des « zones mortes » au
# doigt. La sonde d'écran (shell/essais/sonde-pouces.c) a montré un trou
# rectangulaire en bas à gauche. Restait la question qui décide de tout :
# le trou est-il dans la DALLE, ou dans notre pile logicielle ? Ce
# programme lit /dev/input/event3 directement, sans labwc ni GTK : ce qu'il
# voit est ce que le matériel rapporte, et rien d'autre.
#
# LA LEÇON DE MÉTHODE, PAYÉE CE SOIR-LÀ : ÉTABLIR LE REPÈRE D'ABORD.
#
# J'ai d'abord supposé que l'axe X du numériseur était inversé, et conclu
# deux fois de suite — dans deux directions opposées — sur des cartes
# lues en miroir. Quatre appuis dans les coins ont montré que le repère
# est direct. Une mesure interprétée dans un repère supposé ne vaut rien :
# « coins » se lance AVANT tout le reste.
#
# CE QU'ON A TROUVÉ, ET CE QU'IL EN EST ADVENU
#
# Zone muette mesurée : x de 0 à ~170 px, y de ~375 à 1080 — 28 x 114 mm
# le long du bord gauche. Confirmée par trois voies : carte de couverture,
# appuis délibérés (aucun reçu), et un appui « fantôme » rapporté à
# y = 68 alors qu'il avait eu lieu à mi-hauteur.
#
# LE LENDEMAIN MATIN, ELLE AVAIT DISPARU — constaté par l'utilisateur.
# Le défaut est donc INTERMITTENT : ni la dalle ni le pilote ne sont morts.
# Piste non vérifiée : contact de nappe, température, ou état du
# micrologiciel du contrôleur. NE PAS inscrire cette zone en dur dans le
# clavier : relancer cet outil le jour où elle revient, et la mesurer à
# nouveau — c'est la seule façon de savoir si elle est au même endroit.
# =========================================================================
import collections, select, struct, sys, time

DEV = '/dev/input/event3'          # GDIX0000:00 27C6:0E88 Touchscreen
LX, LY = 23040, 12960              # étendue déclarée par le numériseur
ECRAN_X, ECRAN_Y = 1920, 1080
ABS_X, ABS_Y, BTN_TOUCH = 0x35, 0x36, 0x14a
TAILLE = struct.calcsize('llHHi')

def evenements(duree):
    """(x, y en pixels d'écran, nouveau_contact) tant qu'il reste du temps."""
    x = y = None
    with open(DEV, 'rb') as f:
        fin = time.monotonic() + duree
        while time.monotonic() < fin:
            if not select.select([f], [], [], 0.4)[0]:
                continue
            data = f.read(TAILLE * 64)
            for i in range(0, len(data), TAILLE):
                _, _, t, code, val = struct.unpack('llHHi', data[i:i + TAILLE])
                if t == 3 and code == ABS_X:
                    x = val * ECRAN_X // LX
                elif t == 3 and code == ABS_Y:
                    y = val * ECRAN_Y // LY
                    if x is not None:
                        yield x, y, False
                elif t == 1 and code == BTN_TOUCH and val == 1 and x is not None:
                    yield x, y, True

def coins(duree):
    print('Appuie dans les QUATRE COINS, en annonçant l\'ordre que tu suis.')
    print('Le repère est direct si le premier coin touché rend les mêmes')
    print('coordonnées que ce que tu vois à l\'écran.\n')
    n = 0
    for x, y, nouveau in evenements(duree):
        if nouveau:
            n += 1
            print(f'  {n:2}. x={x:4}  y={y:4}', flush=True)

def carte(duree):
    print('Balaie TOUT l\'écran, lentement et serré. Les trous se verront.\n')
    g = collections.Counter()
    for x, y, _ in evenements(duree):
        g[(x // 40, y // 40)] += 1
    print(f'{sum(g.values())} positions reçues ; carte, cases de 40 px :')
    for cy in range((ECRAN_Y + 39) // 40):
        print('  ' + ''.join('#' if g[(cx, cy)] > 4 else '+' if g[(cx, cy)] else '.'
                             for cx in range((ECRAN_X + 39) // 40)))
    creux = [(cx * 40, cy * 40) for cy in range(1, 26) for cx in range(1, 47)
             if not g[(cx, cy)]
             and sum(1 for dx in (-1, 0, 1) for dy in (-1, 0, 1)
                     if (dx or dy) and g[(cx + dx, cy + dy)]) >= 6]
    print(f'\n{len(creux)} case(s) vide(s) entourée(s) de cases touchées :')
    for x, y in creux[:40]:
        print(f'  x {x:4}..{x + 40:4}  y {y:4}..{y + 40:4}')

def bord(duree, xmax=640):
    print(f'Balaie la bande x < {xmax}, du haut vers le bas, serré.\n')
    g = collections.Counter()
    for x, y, _ in evenements(duree):
        if x < xmax:
            g[(x // 20, y // 20)] += 1
    print('premier x atteint, par tranche de 40 px de hauteur :')
    for cy in range(0, ECRAN_Y // 20, 2):
        xs = [cx * 20 for cx in range(xmax // 20) if g[(cx, cy)] or g[(cx, cy + 1)]]
        etat = f'x >= {min(xs)}' if xs else 'AUCUN contact'
        print(f'  y {cy * 20:4}..{cy * 20 + 40:4} : {etat}')

if __name__ == '__main__':
    quoi = sys.argv[1] if len(sys.argv) > 1 else 'coins'
    duree = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    if quoi == 'coins':
        coins(duree)
    elif quoi == 'carte':
        carte(duree)
    elif quoi == 'bord':
        bord(duree, int(sys.argv[3]) if len(sys.argv) > 3 else 640)
    else:
        print(__doc__)
        sys.exit(2)
