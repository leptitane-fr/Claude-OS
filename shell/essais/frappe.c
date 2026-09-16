/* =========================================================================
 * Claude OS — clavier virtuel pour le banc
 *
 * Le labwc sans ecran du banc n'a ni souris ni clavier. `pointeur` lui donne
 * la premiere depuis le 11 septembre 2026 ; ce programme lui donne le
 * second, par le protocole virtual-keyboard-unstable-v1 -- celui-la meme que
 * le clavier a l'ecran du mode tablette utilise deja (saisie.c).
 *
 * POURQUOI IL A FALLU L'ECRIRE. L'auvent du dock porte une SAISIE
 * (auvent.h) : un champ qu'on remplit, dont chaque frappe part vers
 * l'application. Aucun banc de ce depot ne savait taper -- on pouvait donc
 * eprouver que le volet s'ouvrait, et rien de ce qu'il sert a faire.
 *
 * UNE KEYMAP FABRIQUEE POUR CE QU'ON TAPE, et pas la disposition du
 * systeme. Chercher le keycode d'un caractere dans un AZERTY demande de
 * connaitre ses niveaux, ses groupes et ses touches mortes ; et le banc
 * taperait alors autre chose selon la machine. On construit donc une
 * disposition ou chaque caractere voulu a SA touche, dans l'ordre. C'est le
 * procede de wtype, et il rend la frappe independante du systeme.
 *
 * Usage :
 *   frappe "du texte"        tape la chaine, caractere par caractere
 *   frappe --touche Escape   une touche nommee (nom de keysym X11)
 *
 * Les deux peuvent s'enchainer :
 *   frappe "mire" --touche Return
 * ========================================================================= */

/* memfd_create() est une extension GNU : sans _GNU_SOURCE, glibc ne la
 * declare pas et MFD_CLOEXEC n'existe pas. _POSIX_C_SOURCE seul ne suffit
 * donc pas -- meme famille de piege que le nanosleep de pointeur.c. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"

#define MAX_TOUCHES 200      /* une keymap de banc n'a pas a etre grande */

static struct zwp_virtual_keyboard_manager_v1 *gestionnaire;
static struct wl_seat                         *siege;
static struct zwp_virtual_keyboard_v1         *clavier;

/* Les keysyms qu'on va poser, un par touche. L'indice + 8 est le keycode :
 * 8 est le decalage historique entre les codes du noyau et ceux de X11, que
 * XKB conserve. */
static xkb_keysym_t touches[MAX_TOUCHES];
static int          n_touches;

static void
registre (void *d, struct wl_registry *r, uint32_t nom, const char *iface,
          uint32_t version)
{
    (void) d; (void) version;

    if (strcmp (iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
        gestionnaire = wl_registry_bind (r, nom,
            &zwp_virtual_keyboard_manager_v1_interface, 1);
    else if (strcmp (iface, wl_seat_interface.name) == 0)
        siege = wl_registry_bind (r, nom, &wl_seat_interface, 1);
}

static const struct wl_registry_listener ECOUTE = { registre, NULL };

/* Retenir un keysym, ou rendre celui qu'on a deja : deux « e » dans un mot
 * ne valent pas deux touches. */
static int
touche_pour (xkb_keysym_t sym)
{
    for (int i = 0; i < n_touches; i++)
        if (touches[i] == sym)
            return i;

    if (n_touches >= MAX_TOUCHES) {
        fprintf (stderr, "frappe : plus de %d touches distinctes\n", MAX_TOUCHES);
        exit (1);
    }
    touches[n_touches] = sym;
    return n_touches++;
}

/* La keymap, en texte. Un groupe, un niveau, une touche par keysym. */
static char *
keymap_texte (void)
{
    size_t taille = 4096 + (size_t) n_touches * 128;
    char  *s = malloc (taille);
    size_t n = 0;

    n += (size_t) snprintf (s + n, taille - n,
        "xkb_keymap {\n"
        "  xkb_keycodes {\n"
        "    minimum = 8;\n"
        "    maximum = %d;\n", n_touches + 8 + 1);

    for (int i = 0; i < n_touches; i++)
        n += (size_t) snprintf (s + n, taille - n, "    <K%d> = %d;\n", i, i + 9);

    n += (size_t) snprintf (s + n, taille - n,
        "  };\n"
        "  xkb_types { include \"complete\" };\n"
        "  xkb_compat { include \"complete\" };\n"
        "  xkb_symbols {\n"
        "    name[group1] = \"banc\";\n");

    for (int i = 0; i < n_touches; i++) {
        char nom[64];
        xkb_keysym_get_name (touches[i], nom, sizeof nom);
        n += (size_t) snprintf (s + n, taille - n,
                                "    key <K%d> { [ %s ] };\n", i, nom);
    }

    snprintf (s + n, taille - n, "  };\n};\n");
    return s;
}

static void
poser_keymap (void)
{
    char *texte = keymap_texte ();
    size_t taille = strlen (texte) + 1;

    /* memfd plutot qu'un fichier : rien a nettoyer, et la keymap ne traine
     * pas dans /tmp entre deux essais. */
    int fd = memfd_create ("keymap-banc", MFD_CLOEXEC);
    if (fd < 0 || ftruncate (fd, (off_t) taille) < 0) {
        perror ("frappe : memfd");
        exit (1);
    }
    void *carte = mmap (NULL, taille, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (carte == MAP_FAILED) {
        perror ("frappe : mmap");
        exit (1);
    }
    memcpy (carte, texte, taille);
    munmap (carte, taille);
    free (texte);

    zwp_virtual_keyboard_v1_keymap (clavier, XKB_KEYMAP_FORMAT_TEXT_V1,
                                    fd, (uint32_t) taille);
    close (fd);
}

static uint32_t
maintenant (void)
{
    struct timespec t;
    clock_gettime (CLOCK_MONOTONIC, &t);
    return (uint32_t) (t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void
appuyer (struct wl_display *d, int indice)
{
    /* Le keycode que le protocole attend est celui du NOYAU, soit huit de
     * moins que celui de XKB. La keymap pose <K0> = 9, le noyau dit 1. */
    uint32_t code = (uint32_t) indice + 1;

    zwp_virtual_keyboard_v1_key (clavier, maintenant (), code, 1);
    wl_display_flush (d);
    nanosleep (&(struct timespec){ 0, 12 * 1000000 }, NULL);
    zwp_virtual_keyboard_v1_key (clavier, maintenant (), code, 0);
    wl_display_flush (d);
    nanosleep (&(struct timespec){ 0, 12 * 1000000 }, NULL);
}

int
main (int argc, char **argv)
{
    if (argc < 2) {
        fprintf (stderr, "usage : %s \"texte\" | --touche <Nom>\n", argv[0]);
        return 2;
    }

    /* PREMIER PASSAGE : recenser ce qu'on va taper, pour fabriquer la
     * keymap. Elle doit etre posee avant la premiere touche. */
    int *suite = calloc ((size_t) argc * 256, sizeof (int));
    int  n_suite = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp (argv[i], "--touche") == 0 && i + 1 < argc) {
            xkb_keysym_t sym = xkb_keysym_from_name (argv[++i],
                                                     XKB_KEYSYM_NO_FLAGS);
            if (sym == XKB_KEY_NoSymbol) {
                fprintf (stderr, "frappe : touche inconnue « %s »\n", argv[i]);
                return 1;
            }
            suite[n_suite++] = touche_pour (sym);
            continue;
        }

        /* Le texte est en UTF-8 : on le parcourt en points de code, sans
         * quoi « é » partirait en deux touches illisibles. */
        const char *p = argv[i];
        while (*p != '\0') {
            /* Decodage UTF-8 a la main : le banc ne tire pas GLib pour
             * quatre lignes, et un caractere mal decoupe partirait en deux
             * touches illisibles. */
            uint32_t pc;
            int taille;
            unsigned char c = (unsigned char) *p;
            if (c < 0x80)      { pc = c; taille = 1; }
            else if (c < 0xE0) { pc = ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F); taille = 2; }
            else if (c < 0xF0) { pc = ((uint32_t)(c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); taille = 3; }
            else               { pc = ((uint32_t)(c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); taille = 4; }

            xkb_keysym_t sym = xkb_utf32_to_keysym (pc);
            if (sym == XKB_KEY_NoSymbol) {
                fprintf (stderr, "frappe : caractere U+%04X sans keysym\n", pc);
                return 1;
            }
            suite[n_suite++] = touche_pour (sym);
            p += taille;
        }
    }

    struct wl_display *display = wl_display_connect (NULL);
    if (display == NULL) {
        fprintf (stderr, "frappe : pas de compositeur (WAYLAND_DISPLAY ?)\n");
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry (display);
    wl_registry_add_listener (reg, &ECOUTE, NULL);
    wl_display_roundtrip (display);

    if (gestionnaire == NULL || siege == NULL) {
        fprintf (stderr, "frappe : le compositeur n'expose pas "
                         "virtual-keyboard-unstable-v1\n");
        return 1;
    }

    clavier = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard (
        gestionnaire, siege);
    poser_keymap ();
    wl_display_roundtrip (display);

    /* Un souffle avant la premiere touche : le compositeur vient de
     * recevoir une disposition, et la fenetre visee doit avoir le focus. */
    nanosleep (&(struct timespec){ 0, 80 * 1000000 }, NULL);

    for (int i = 0; i < n_suite; i++)
        appuyer (display, suite[i]);

    wl_display_roundtrip (display);
    free (suite);
    wl_display_disconnect (display);
    return 0;
}
