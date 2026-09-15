#!/usr/bin/env python3
"""Claude OS — où le doigt se pose vraiment, mesuré sur la dalle.

Le doigt virtuel (essais/doigt.py) pose le contact exactement où on le lui
dit. Une dalle, non : elle confirme un contact après une trame ou deux, et
rapporte alors une position qui a déjà bougé. C'est ce décalage qui décide
si un glissé venu du cadre tombe ou non dans la lisière d'un tiroir.

Ce programme lit l'écran tactile directement (evdev, sans le compositeur) et
rend, pour chaque contact : le PREMIER point rapporté, celui de la trame
suivante, le trajet total, la durée, et le nombre de doigts simultanés —
une paume qui effleure le cadre est un contact comme un autre, et GTK ne
suit que le premier.

Usage :
    claude-os-root python3 shell/essais/sonde-contacts.py [secondes] [sortie] [nom]

« nom » vise une autre dalle que le Goodix par un morceau de son nom — le
doigt virtuel d'essais/doigt.py, par exemple, qui sert à éprouver cette sonde.
En ARGUMENT et non en variable d'environnement : claude-os-root ne transmet
pas l'environnement, et une sonde qui vise le mauvais périphérique rend un
fichier vide sans rien dire.

Les coordonnées sont rendues en PIXELS D'ÉCRAN (1920x1080 par défaut,
SONDE_ECRAN=LxH pour en changer), à partir des bornes déclarées par le
périphérique.
"""

import fcntl, os, select, struct, sys, time

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y = 0x2f, 0x35, 0x36
ABS_MT_TRACKING_ID = 0x39
EVENT = "=qqHHi"
TAILLE = struct.calcsize(EVENT)

L, H = (int(v) for v in os.environ.get("SONDE_ECRAN", "1920x1080").split("x"))

# LES DEUX LARGEURS DE tiroir.c, ET ELLES DIFFÈRENT. La dalle de MADOO ne
# rapporte aucun contact en deçà de 32 px de son bord gauche ; celle-là fait
# donc 48 px, celle de droite 24. À tenir en accord avec LISIERE_GAUCHE_PX et
# LISIERE_DROITE_PX : une sonde qui juge « hors lisière » d'après une largeur
# périmée rend un verdict faux sur des chiffres justes.
LISIERE_G, LISIERE_D = 48, 24


def trouver_dalle():
    """Le nœud de la dalle, d'après /proc/bus/input/devices.

    Le Goodix de MADOO par défaut ; SONDE_DALLE vise autre chose par un
    morceau de son nom — le doigt virtuel d'essais/doigt.py, par exemple,
    qui sert à éprouver cette sonde elle-même.
    """
    vise = sys.argv[3] if len(sys.argv) > 3 else "GDIX"
    bloc, nom = [], None
    for ligne in open("/proc/bus/input/devices"):
        if ligne.startswith("N: Name="):
            nom = ligne.split('"')[1]
        elif ligne.startswith("H: Handlers=") and nom:
            if vise in nom and "Stylus" not in nom:
                for h in ligne.split("=", 1)[1].split():
                    if h.startswith("event"):
                        return "/dev/input/" + h, nom
        elif not ligne.strip():
            nom = None
    return None, None


def bornes(fd, axe):
    taille = struct.calcsize("=iiiiii")
    req = (2 << 30) | (taille << 16) | (ord("E") << 8) | (0x40 + axe)
    brut = fcntl.ioctl(fd, req, b"\0" * taille)
    _, mini, maxi, _, _, _ = struct.unpack("=iiiiii", brut)
    return mini, maxi


def main():
    duree = float(sys.argv[1]) if len(sys.argv) > 1 else 180.0
    sortie = sys.argv[2] if len(sys.argv) > 2 else "/tmp/claude-os-contacts.txt"

    noeud, nom = trouver_dalle()
    if noeud is None:
        cible = sys.argv[3] if len(sys.argv) > 3 else "GDIX"
        print(f"aucune dalle « {cible} » dans /proc/bus/input/devices", file=sys.stderr)
        return 1

    fd = os.open(noeud, os.O_RDONLY | os.O_NONBLOCK)
    xmin, xmax = bornes(fd, ABS_MT_POSITION_X)
    ymin, ymax = bornes(fd, ABS_MT_POSITION_Y)
    ex = lambda v: round((v - xmin) * (L - 1) / max(1, xmax - xmin))
    ey = lambda v: round((v - ymin) * (H - 1) / max(1, ymax - ymin))

    j = open(sortie, "w")
    entete = (f"dalle : {nom} ({noeud})\n"
              f"bornes brutes : x {xmin}..{xmax}, y {ymin}..{ymax}"
              f"  →  rendu en pixels d'écran {L}x{H}\n"
              f"écoute {duree:.0f} s à partir de {time.strftime('%H:%M:%S')}\n\n"
              f"lisière des tiroirs : {LISIERE_G} px à gauche, {LISIERE_D} px à droite"
              f"  (donc x ≤ {LISIERE_G - 1} ou x ≥ {L - LISIERE_D})\n\n"
              f"{'#':>3} {'bord':>7} {'pose x,y':>12} {'x mini':>7} {'trame+1':>8} "
              f"{'+50 ms':>8} {'trajet':>7} {'durée':>7} {'doigts':>6} {'lisière':>8}\n")
    j.write(entete); j.flush()

    slots = {}          # slot -> dict du contact en cours
    slot = 0
    n = 0
    fin = time.time() + duree
    tampon = b""

    # select plutôt qu'une lecture bloquante : sans lui, une écoute où
    # personne ne touche l'écran ne se réveille jamais pour voir que son temps
    # est écoulé.
    # L'ÉCOUTE SE TERMINE D'ELLE-MÊME. Six contacts et vingt-cinq secondes
    # sans rien : celui qui mesure a fini son geste, et il n'a pas à venir
    # arrêter le programme. Le temps donné en argument reste le plafond.
    assez, repos = 6, 25.0
    dernier = time.time()
    while time.time() < fin:
        if assez and n >= assez and time.time() - dernier > repos:
            j.write(f"\n(arrêt : {n} contacts et {repos:.0f} s sans rien)\n")
            break
        pret, _, _ = select.select([fd], [], [], 1.0)
        if not pret:
            continue
        try:
            tampon += os.read(fd, TAILLE * 64)
        except BlockingIOError:
            continue
        dernier = time.time()
        while len(tampon) >= TAILLE:
            sec, usec, t, code, val = struct.unpack(EVENT, tampon[:TAILLE])
            tampon = tampon[TAILLE:]
            maintenant = sec + usec / 1e6

            if t == EV_ABS and code == ABS_MT_SLOT:
                slot = val
            elif t == EV_ABS and code == ABS_MT_TRACKING_ID:
                if val >= 0:
                    slots[slot] = {"t0": maintenant, "x": None, "y": None,
                                   "points": [], "voisins": len(slots)}
                elif slot in slots:
                    c = slots.pop(slot)
                    if not c["points"]:
                        continue
                    n += 1
                    p = c["points"]
                    premier = p[0]
                    suivant = p[1] if len(p) > 1 else p[0]
                    apres = next((q for q in p if q[0] - premier[0] >= 0.050), p[-1])
                    xs = [ex(q[1]) for q in p]
                    x0, mini, maxi = xs[0], min(xs), max(xs)

                    # De quel bord venait le geste, et le contact est-il tombé
                    # dans la lisière ? C'est TOUTE la question : la lisière ne
                    # voit que ce qui se pose dans ses 24 px.
                    if x0 < L / 2:
                        bord, dedans = "gauche", mini <= LISIERE_G - 1
                    else:
                        bord, dedans = "droite", maxi >= L - LISIERE_D
                    j.write(f"{n:>3} {bord:>7} {x0:>5},{ey(premier[2]):<6} "
                            f"{mini:>7} {ex(suivant[1]):>8} {ex(apres[1]):>8} "
                            f"{maxi - mini:>7} {p[-1][0] - p[0][0]:>6.2f}s "
                            f"{c['voisins'] + 1:>6} "
                            f"{'OUI' if dedans else 'non':>8}\n")
                    j.flush()
            elif t == EV_ABS and code == ABS_MT_POSITION_X and slot in slots:
                slots[slot]["x"] = val
            elif t == EV_ABS and code == ABS_MT_POSITION_Y and slot in slots:
                slots[slot]["y"] = val
            elif t == EV_SYN and code == SYN_REPORT:
                for c in slots.values():
                    if c["x"] is not None and c["y"] is not None:
                        c["points"].append((maintenant, c["x"], c["y"]))

    j.write(f"\nfin de l'écoute : {n} contact(s).\n")
    j.close()
    os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
