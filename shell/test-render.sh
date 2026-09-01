#!/usr/bin/env bash
# Claude-OS Shell -- banc d'essai visuel.
#
# Demarre labwc sans ecran, lance un composant, capture le rendu, et arrete
# tout. Permet de juger l'apparence reelle sans materiel graphique.
#
# Usage :  ./shell/test-render.sh ./build/dock capture.png [largeur] [hauteur]

set -uo pipefail

CMD="${1:?commande du composant a lancer}"
OUT="${2:-rendu.png}"
W="${3:-1920}"
H="${4:-1200}"          # resolution de l'ecran du Vivobook

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/claude-os-wl}"
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"

# Backend sans ecran + rendu logiciel : aucune carte graphique requise.
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
export WLR_HEADLESS_OUTPUTS=1
export GDK_BACKEND=wayland
export GSK_RENDERER=cairo          # evite d'exiger un GPU dans le conteneur

cleanup() { kill "${APP_PID:-}" "${BG_PID:-}" "${LABWC_PID:-}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

# GtkApplication exige un bus de session. dbus-run-session en fournit un
# jetable, ce qui evite l'avertissement « Unable to acquire session bus ».
if command -v dbus-run-session >/dev/null 2>&1 && [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    exec dbus-run-session -- "$0" "$@"
fi

labwc >/tmp/labwc-test.log 2>&1 &
LABWC_PID=$!

for _ in $(seq 1 40); do
    sock=$(ls "$XDG_RUNTIME_DIR"/wayland-[0-9] 2>/dev/null | head -1)
    [ -n "$sock" ] && break
    sleep 0.25
done
[ -n "${sock:-}" ] || { echo "labwc n'a pas demarre :"; tail -10 /tmp/labwc-test.log; exit 1; }
WAYLAND_DISPLAY="$(basename "$sock")"
export WAYLAND_DISPLAY

# La sortie sans ecran fait 1280x720 par defaut ; on la porte a la taille
# reelle de l'ecran cible pour juger les proportions.
if command -v wlr-randr >/dev/null 2>&1; then
    out=$(wlr-randr 2>/dev/null | head -1 | cut -d' ' -f1)
    [ -n "$out" ] && wlr-randr --output "$out" --custom-mode "${W}x${H}" >/dev/null 2>&1
fi

# Fond neutre : sur du noir pur, ni l'ombre ni le contraste des surfaces ne
# sont jugeables. On approche un fond d'ecran realiste.
if command -v swaybg >/dev/null 2>&1; then
    swaybg -c "${BACKDROP:-#3c4043}" >/dev/null 2>&1 &
    BG_PID=$!
    sleep 0.5
fi

$CMD >/tmp/composant-test.log 2>&1 &
APP_PID=$!

# Laisser le temps au composant de se dessiner et aux transitions de finir.
sleep 3

grim "$OUT" 2>/dev/null || { echo "capture impossible"; exit 1; }
echo "capture : $OUT ($(file -b "$OUT" | cut -d, -f2 | tr -d ' '))"

if ! kill -0 "$APP_PID" 2>/dev/null; then
    echo "AVERTISSEMENT : le composant s'est arrete avant la capture"
    tail -10 /tmp/composant-test.log | sed 's/^/    /'
fi
