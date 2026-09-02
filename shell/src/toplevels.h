/* =========================================================================
 * Claude-OS Shell — suivi des fenetres ouvertes
 *
 * Le dock a besoin de savoir ce qui tourne : pour allumer le point sous une
 * icone, pour lister les fenetres au survol, et pour en ramener une au
 * premier plan. Rien de tout cela n'est deductible autrement.
 *
 * Protocole : wlr-foreign-toplevel-management-v1, celui qu'utilisent les
 * barres de taches. Le XML n'etant empaquete nulle part dans Debian trixie
 * (aucun paquet wlr-protocols), il est verse dans le depot et le code client
 * genere a la compilation.
 *
 * Discipline d'energie : aucune consultation periodique. Le compositeur
 * envoie un evenement quand un etat change, et rien le reste du temps.
 * ========================================================================= */
#pragma once

#include <glib.h>

typedef struct _ShellWindow ShellWindow;

struct _ShellWindow {
    char     *app_id;
    char     *title;
    gboolean  activated;
    gboolean  minimized;
    gpointer  handle;      /* zwlr_foreign_toplevel_handle_v1 *             */
};

/* Appelee apres chaque lot d'evenements valide par le compositeur. */
typedef void (*ShellToplevelsFunc) (gpointer user_data);

/* A appeler une fois la fenetre presentee : la connexion Wayland de GTK doit
 * deja exister. Sans compositeur compatible, le shell continue de
 * fonctionner : la liste reste simplement vide. */
void shell_toplevels_init (ShellToplevelsFunc on_change, gpointer user_data);

/* Fenetres actuellement ouvertes, dans leur ordre d'apparition.
 * Le tableau appartient au module et change a chaque rappel : ne pas le
 * conserver au-dela du traitement en cours. Elements : ShellWindow *. */
const GPtrArray *shell_toplevels_get (void);

/* Ramene une fenetre au premier plan. */
void shell_toplevel_activate (const ShellWindow *win);

/* Rapproche un identifiant .desktop d'un app_id Wayland.
 *
 * Les deux ne coincident pas toujours : « org.gnome.Nautilus » cote fenetre
 * pour « nautilus.desktop », et la casse varie. Expose ici parce que le dock
 * en a besoin pour relier une icone a ses fenetres. */
gboolean shell_app_id_matches (const char *desktop_id, const char *app_id);
