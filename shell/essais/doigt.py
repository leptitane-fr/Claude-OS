#!/usr/bin/env python3
"""Claude OS — un doigt virtuel pour le banc.

Le pointeur virtuel (essais/pointeur.c) parle au compositeur par
wlr-virtual-pointer : ses évènements entrent DANS labwc, mais pas par le même
chemin qu'un contact réel. Or le glissé du doigt ne suit pas le chemin du
pointeur — labwc traite le toucher dans touch.c, et rc.xml le configure avec
« mouseEmulation=no », donc sans repasser par la souris. Un geste au pointeur
ne prouve donc rien du geste au doigt, ce que les bancs du dock et des
tiroirs disent tous les deux en toutes lettres.

Ce programme fabrique un ÉCRAN TACTILE par uinput : le noyau le voit comme un
périphérique d'entrée ordinaire, libinput le classe comme tactile absolu, et
labwc le route comme la dalle Goodix de MADOO. Un glissé joué ici traverse
donc exactement la pile qu'un doigt traverse.

CE PROBE NE SERT PAS AU BANC SANS ÉCRAN. Un périphérique uinput est vu par
libinput, donc par le compositeur de la SESSION ; le labwc du banc tourne avec
WLR_LIBINPUT_NO_DEVICES=1 et n'a aucun périphérique. Ce doigt-là s'éprouve sur
la machine, sur la session en cours.

Usage (root — uinput appartient à root) :
    claude-os-root python3 shell/essais/doigt.py glisse X Y X2 Y2 [ms]
    claude-os-root python3 shell/essais/doigt.py touche X Y

L'écran fait 1920x1080 par défaut ; DOIGT_ECRAN=LxH pour en changer.
"""

import fcntl, os, struct, sys, time

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
BTN_TOUCH = 0x14a
ABS_X, ABS_Y = 0x00, 0x01
ABS_MT_SLOT = 0x2f
ABS_MT_POSITION_X, ABS_MT_POSITION_Y = 0x35, 0x36
ABS_MT_TRACKING_ID = 0x39
INPUT_PROP_DIRECT = 0x01

UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
UI_SET_EVBIT   = 0x40045564
UI_SET_KEYBIT  = 0x40045565
UI_SET_ABSBIT  = 0x40045567
UI_SET_PROPBIT = 0x4004556e

L, H = (int(v) for v in os.environ.get("DOIGT_ECRAN", "1920x1080").split("x"))


def ecrire(fd, type_, code, valeur):
    """Un struct input_event : timeval (deux longs), type, code, valeur.

    « q » et NON « l » : en mode standard, struct écrit « l » sur quatre
    octets, et l'évènement faisait 16 octets là où le noyau en attend 24 sur
    x86_64. Il refusait par EINVAL, sans un mot de plus — payé le
    15 septembre 2026.
    """
    os.write(fd, struct.pack("=qqHHi", 0, 0, type_, code, valeur))


def syn(fd):
    ecrire(fd, EV_SYN, SYN_REPORT, 0)


def creer():
    # Le module n'est pas chargé par défaut sur Claude OS ; on le charge à la
    # demande plutôt que de laisser le programme échouer sur un ENOENT.
    if not os.path.exists("/dev/uinput"):
        os.system("modprobe uinput")
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    for ev in (EV_SYN, EV_KEY, EV_ABS):
        fcntl.ioctl(fd, UI_SET_EVBIT, ev)
    fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH)
    fcntl.ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT)   # une dalle, pas un pavé
    for ax in (ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_TRACKING_ID,
               ABS_MT_POSITION_X, ABS_MT_POSITION_Y):
        fcntl.ioctl(fd, UI_SET_ABSBIT, ax)

    absmax = [0] * 64
    absmin = [0] * 64
    for ax in (ABS_X, ABS_MT_POSITION_X):
        absmax[ax] = L - 1
    for ax in (ABS_Y, ABS_MT_POSITION_Y):
        absmax[ax] = H - 1
    absmax[ABS_MT_SLOT] = 9
    absmax[ABS_MT_TRACKING_ID] = 65535
    absmin[ABS_MT_TRACKING_ID] = -1

    dev = struct.pack("=80sHHHHI", b"Claude OS doigt de banc", 0x18, 0x27c6, 0x0e88, 1, 0)
    dev += struct.pack("=64i", *absmax) + struct.pack("=64i", *absmin)
    dev += struct.pack("=64i", *([0] * 64)) + struct.pack("=64i", *([0] * 64))
    os.write(fd, dev)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    # libinput doit voir arriver le périphérique et l'ouvrir : sans cette
    # attente, les premiers contacts partent dans le vide.
    time.sleep(1.2)
    return fd


def pose(fd, x, y, slot=0, ident=1):
    ecrire(fd, EV_ABS, ABS_MT_SLOT, slot)
    ecrire(fd, EV_ABS, ABS_MT_TRACKING_ID, ident)
    ecrire(fd, EV_ABS, ABS_MT_POSITION_X, x)
    ecrire(fd, EV_ABS, ABS_MT_POSITION_Y, y)
    ecrire(fd, EV_ABS, ABS_X, x)
    ecrire(fd, EV_ABS, ABS_Y, y)
    ecrire(fd, EV_KEY, BTN_TOUCH, 1)
    syn(fd)


def bouge(fd, x, y, slot=0):
    ecrire(fd, EV_ABS, ABS_MT_SLOT, slot)
    ecrire(fd, EV_ABS, ABS_MT_POSITION_X, x)
    ecrire(fd, EV_ABS, ABS_MT_POSITION_Y, y)
    ecrire(fd, EV_ABS, ABS_X, x)
    ecrire(fd, EV_ABS, ABS_Y, y)
    syn(fd)


def leve(fd, slot=0, dernier=True):
    ecrire(fd, EV_ABS, ABS_MT_SLOT, slot)
    ecrire(fd, EV_ABS, ABS_MT_TRACKING_ID, -1)
    if dernier:
        ecrire(fd, EV_KEY, BTN_TOUCH, 0)
    syn(fd)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); return 2
    fd = creer()
    try:
        if a[0] == "glisse":
            x, y, x2, y2 = (int(v) for v in a[1:5])
            duree = int(a[5]) / 1000 if len(a) > 5 else 0.30
            pas = 12
            pose(fd, x, y)
            for i in range(1, pas + 1):
                time.sleep(duree / pas)
                bouge(fd, x + (x2 - x) * i // pas, y + (y2 - y) * i // pas)
            time.sleep(0.05)
            leve(fd)
        elif a[0] == "fantome":
            # LE CAS QUI FAIT ÉCHOUER UN GESTE GTK : un contact fugace naît
            # sur la lisière juste avant le vrai glissé — la main qui entre
            # par le bord frôle le châssis. GtkGestureSingle donne le geste au
            # premier et ignore le second.
            #   fantome X Y X2 Y2 [xf yf]
            x, y, x2, y2 = (int(v) for v in a[1:5])
            xf = int(a[5]) if len(a) > 5 else 5
            yf = int(a[6]) if len(a) > 6 else y - 200
            pose(fd, xf, yf, slot=0, ident=7)        # le fantôme, d'abord
            time.sleep(0.04)
            pose(fd, x, y, slot=1, ident=8)          # puis l'index
            for k in range(1, 13):
                time.sleep(0.02)
                bouge(fd, x + (x2 - x) * k // 12, y + (y2 - y) * k // 12, slot=1)
            leve(fd, slot=0, dernier=False)
            time.sleep(0.03)
            leve(fd, slot=1, dernier=True)
        elif a[0] == "serie":
            # Plusieurs glissés sans détruire le périphérique entre deux :
            # c'est ce qui permet à une sonde lancée en parallèle de le
            # trouver et de le lire (essais/sonde-contacts.py).
            combien = int(a[1]) if len(a) > 1 else 3
            time.sleep(1.5)
            for i in range(combien):
                depart = 5 + i * 20
                pose(fd, depart, 500)
                for k in range(1, 13):
                    time.sleep(0.02)
                    bouge(fd, depart + 200 * k // 12, 500)
                leve(fd)
                print(f"  glissé {i + 1} : posé à x={depart}")
                time.sleep(1.5)
        elif a[0] == "touche":
            x, y = int(a[1]), int(a[2])
            pose(fd, x, y); time.sleep(0.08); leve(fd)
        else:
            print(__doc__); return 2
        time.sleep(0.4)
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
