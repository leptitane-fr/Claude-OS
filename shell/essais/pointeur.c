/* =========================================================================
 * Claude OS — pointeur virtuel pour le banc
 *
 * Le labwc sans ecran du banc n'a ni souris ni doigt. Ce programme lui en
 * donne un, par le protocole wlr-virtual-pointer, pour cliquer et glisser
 * POUR DE VRAI : les evenements traversent le compositeur comme ceux d'une
 * souris, avec son empilement, ses zones d'entree et ses saisies. C'est ce
 * qui a permis, le 11 septembre 2026, de trouver que la bande du bord du
 * dock ne recevait rien -- ce qu'aucune capture seule n'aurait montre.
 *
 * Il ne remplace pas le doigt : un glisser au pointeur passe par le meme
 * GtkGestureDrag qu'un glisser au doigt, mais pas par le meme chemin dans
 * labwc (touch.c). Le geste reel reste a eprouver sur la machine.
 *
 * Usage, commandes enchainees :
 *   pointeur va X Y              deplacement absolu
 *            appui | lache       bouton gauche
 *            clic X Y            va + appui + lache
 *            glisse X Y X2 Y2 N  appui en (X,Y), N pas jusqu'a (X2,Y2), lache
 *            pause MS
 *
 * Les coordonnees sont celles de l'ecran : POINTEUR_ECRAN=1920x1080 par
 * defaut, la dalle de MADOO.
 *
 * ATTENTION, piege paye : un clic sur le fond ouvre le menu racine de
 * labwc, et le clic suivant sert a le refermer. Un scenario qui clique sur
 * le bureau puis ailleurs mesure le menu, pas le shell.
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct zwlr_virtual_pointer_manager_v1 *gestionnaire;
static struct wl_seat *siege;
static struct wl_display *affichage;
static struct zwlr_virtual_pointer_v1 *pointeur;
static int largeur = 1920, hauteur = 1080;

static void
annonce (void *d, struct wl_registry *r, uint32_t nom, const char *interface, uint32_t v)
{
    (void) d; (void) v;
    if (strcmp (interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
        gestionnaire = wl_registry_bind (r, nom, &zwlr_virtual_pointer_manager_v1_interface, 1);
    else if (strcmp (interface, wl_seat_interface.name) == 0)
        siege = wl_registry_bind (r, nom, &wl_seat_interface, 1);
}

static void
retrait (void *d, struct wl_registry *r, uint32_t nom)
{
    (void) d; (void) r; (void) nom;
}

static const struct wl_registry_listener ecoute = { annonce, retrait };

static uint32_t
maintenant_ms (void)
{
    struct timespec t;
    clock_gettime (CLOCK_MONOTONIC, &t);
    return (uint32_t) (t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void
dormir (int ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep (&t, NULL);
}

/* Un aller-retour apres chaque evenement : le suivant ne part qu'une fois
 * le precedent traite par le compositeur. Sans cela, un appui pouvait
 * arriver avant le deplacement qui devait le placer. */
static void
aller (int x, int y)
{
    zwlr_virtual_pointer_v1_motion_absolute (pointeur, maintenant_ms (),
                                             x, y, largeur, hauteur);
    zwlr_virtual_pointer_v1_frame (pointeur);
    wl_display_roundtrip (affichage);
}

static void
bouton (int etat)
{
    zwlr_virtual_pointer_v1_button (pointeur, maintenant_ms (), BTN_LEFT, etat);
    zwlr_virtual_pointer_v1_frame (pointeur);
    wl_display_roundtrip (affichage);
}

int
main (int argc, char **argv)
{
    const char *ecran = getenv ("POINTEUR_ECRAN");
    if (ecran != NULL && sscanf (ecran, "%dx%d", &largeur, &hauteur) != 2) {
        fprintf (stderr, "pointeur : POINTEUR_ECRAN illisible : %s\n", ecran);
        return 2;
    }

    affichage = wl_display_connect (NULL);
    if (affichage == NULL) {
        fprintf (stderr, "pointeur : aucun affichage Wayland (WAYLAND_DISPLAY ?)\n");
        return 1;
    }
    struct wl_registry *registre = wl_display_get_registry (affichage);
    wl_registry_add_listener (registre, &ecoute, NULL);
    wl_display_roundtrip (affichage);
    if (gestionnaire == NULL) {
        fprintf (stderr, "pointeur : le compositeur n'offre pas wlr-virtual-pointer\n");
        return 1;
    }
    pointeur = zwlr_virtual_pointer_manager_v1_create_virtual_pointer (gestionnaire, siege);
    wl_display_roundtrip (affichage);

    for (int i = 1; i < argc; i++) {
        if (strcmp (argv[i], "va") == 0 && i + 2 < argc) {
            aller (atoi (argv[i + 1]), atoi (argv[i + 2]));
            i += 2;
        } else if (strcmp (argv[i], "appui") == 0) {
            bouton (1);
        } else if (strcmp (argv[i], "lache") == 0) {
            bouton (0);
        } else if (strcmp (argv[i], "clic") == 0 && i + 2 < argc) {
            aller (atoi (argv[i + 1]), atoi (argv[i + 2]));
            i += 2;
            dormir (30); bouton (1); dormir (60); bouton (0);
        } else if (strcmp (argv[i], "glisse") == 0 && i + 5 < argc) {
            int x  = atoi (argv[i + 1]), y  = atoi (argv[i + 2]);
            int x2 = atoi (argv[i + 3]), y2 = atoi (argv[i + 4]);
            int n  = atoi (argv[i + 5]);
            i += 5;
            if (n < 1) n = 1;
            aller (x, y); dormir (30); bouton (1);
            for (int k = 1; k <= n; k++) {
                dormir (16);                /* une image a 60 Hz */
                aller (x + (x2 - x) * k / n, y + (y2 - y) * k / n);
            }
            dormir (30); bouton (0);
        } else if (strcmp (argv[i], "pause") == 0 && i + 1 < argc) {
            dormir (atoi (argv[i + 1]));
            i += 1;
        } else {
            fprintf (stderr, "pointeur : commande inconnue ou incomplete : %s\n", argv[i]);
            return 2;
        }
    }
    wl_display_roundtrip (affichage);
    return 0;
}
