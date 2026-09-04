#include "toplevels.h"

#include <gtk/gtk.h>
#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>

#include <string.h>            /* strrchr */

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

/* La version 2 a introduit l'etat « fullscreen », la 3 l'evenement
 * « parent ». On prend ce que le compositeur offre, sans rien exiger
 * au-dela de la version 1 : titre, app_id et etats y sont deja. */
#define FTL_VERSION_MAX 3

static struct {
    GPtrArray          *windows;      /* ShellWindow *, ordre d'apparition  */
    ShellToplevelsFunc  on_change;
    gpointer            data;
    struct wl_seat     *seat;
    gboolean            available;
} T;

/* -------------------------------------------------------------------------
 * Rapprochement des identifiants
 * ------------------------------------------------------------------------- */
static char *
normalise (const char *id)
{
    if (id == NULL)
        return NULL;

    /* « org.gnome.Nautilus » -> « nautilus ». Un app_id en notation inversee
     * ne se compare pas tel quel a un identifiant .desktop court. */
    const char *last = strrchr (id, '.');
    const char *base = (last != NULL && *(last + 1) != '\0') ? last + 1 : id;

    return g_ascii_strdown (base, -1);
}

/* Suffixes admis pour un rapprochement par prefixe.
 *
 * Une liste fermee, et non une regle generale sur le separateur. Celle-ci
 * concluait que « claude » et « claude-os-reglages » designent la meme
 * application : le premier prefixe le second, et un tiret suit. Consequence
 * observee sur la machine -- des que Claude Desktop tournait, le dock
 * marquait les Reglages comme lances, en affichait l'infobulle, et le clic
 * n'ouvrait plus les reglages mais ramenait la fenetre de Claude.
 *
 * Enumerer les suffixes reellement rencontres conserve le cas qui avait
 * motive la regle -- chromium / chromium-browser -- sans jamais confondre
 * deux applications distinctes dont l'une prefixe l'autre. */
static const char *const SUFFIXES_VARIANTES[] = {
    "-browser",    /* chromium / chromium-browser */
    "-desktop",    /* claude   / claude-desktop   */
    "-esr",        /* firefox  / firefox-esr      */
    "-bin",
    "-stable",
    "-nightly",
    NULL
};

gboolean
shell_app_id_matches (const char *desktop_id, const char *app_id)
{
    if (desktop_id == NULL || app_id == NULL)
        return FALSE;

    g_autofree char *a = normalise (desktop_id);
    g_autofree char *b = normalise (app_id);
    if (a == NULL || b == NULL)
        return FALSE;

    if (g_strcmp0 (a, b) == 0)
        return TRUE;

    const char *court = strlen (a) < strlen (b) ? a : b;
    const char *long_ = (court == a) ? b : a;
    size_t n = strlen (court);

    if (strncmp (court, long_, n) != 0)
        return FALSE;

    /* Le reste doit etre EXACTEMENT un suffixe connu, pas seulement commencer
     * par un separateur. C'est cette egalite stricte qui ecarte
     * « -os-reglages ». */
    for (int i = 0; SUFFIXES_VARIANTES[i] != NULL; i++)
        if (g_strcmp0 (long_ + n, SUFFIXES_VARIANTES[i]) == 0)
            return TRUE;

    return FALSE;
}

/* -------------------------------------------------------------------------
 * Suivi des fenetres
 * ------------------------------------------------------------------------- */
static ShellWindow *
window_for (struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    for (guint i = 0; i < T.windows->len; i++) {
        ShellWindow *w = g_ptr_array_index (T.windows, i);
        if (w->handle == handle)
            return w;
    }
    return NULL;
}

static void
window_free (gpointer data)
{
    ShellWindow *w = data;
    g_free (w->app_id);
    g_free (w->title);
    g_free (w);
}

static void
on_title (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
          const char *title)
{
    (void) data;
    ShellWindow *w = window_for (handle);
    if (w == NULL)
        return;
    g_free (w->title);
    w->title = g_strdup (title);
}

static void
on_app_id (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
           const char *app_id)
{
    (void) data;
    ShellWindow *w = window_for (handle);
    if (w == NULL)
        return;
    g_free (w->app_id);
    w->app_id = g_strdup (app_id);
}

/* Le tableau d'etats est complet a chaque envoi : ce qui n'y figure pas est
 * faux. On repart de zero plutot que d'accumuler. */
static void
on_state (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
          struct wl_array *states)
{
    (void) data;
    ShellWindow *w = window_for (handle);
    if (w == NULL)
        return;

    w->activated = FALSE;
    w->minimized = FALSE;

    uint32_t *s;
    wl_array_for_each (s, states) {
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
            w->activated = TRUE;
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)
            w->minimized = TRUE;
    }
}

/* « done » valide le lot d'evenements recus depuis le precedent. Prevenir le
 * dock avant, sur chaque evenement, le ferait se reconstruire alors qu'un
 * changement est encore a moitie transmis. */
static void
on_done (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void) data; (void) handle;
    if (T.on_change != NULL)
        T.on_change (T.data);
}

static void
on_closed (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void) data;
    ShellWindow *w = window_for (handle);
    if (w != NULL)
        g_ptr_array_remove (T.windows, w);   /* libere via free_func */

    zwlr_foreign_toplevel_handle_v1_destroy (handle);

    if (T.on_change != NULL)
        T.on_change (T.data);
}

static void
on_output_enter (void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                 struct wl_output *o) { (void) d; (void) h; (void) o; }
static void
on_output_leave (void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                 struct wl_output *o) { (void) d; (void) h; (void) o; }
static void
on_parent (void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
           struct zwlr_foreign_toplevel_handle_v1 *p) { (void) d; (void) h; (void) p; }

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    .title        = on_title,
    .app_id       = on_app_id,
    .output_enter = on_output_enter,
    .output_leave = on_output_leave,
    .state        = on_state,
    .done         = on_done,
    .closed       = on_closed,
    .parent       = on_parent,
};

static void
on_toplevel (void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
             struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void) data; (void) manager;

    ShellWindow *w = g_new0 (ShellWindow, 1);
    w->handle = handle;
    g_ptr_array_add (T.windows, w);

    zwlr_foreign_toplevel_handle_v1_add_listener (handle, &handle_listener, NULL);
}

static void
on_finished (void *data, struct zwlr_foreign_toplevel_manager_v1 *manager)
{
    (void) data;
    zwlr_foreign_toplevel_manager_v1_destroy (manager);
    T.available = FALSE;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = on_toplevel,
    .finished = on_finished,
};

static void
on_global (void *data, struct wl_registry *registry, uint32_t name,
           const char *interface, uint32_t version)
{
    (void) data;
    if (g_strcmp0 (interface, zwlr_foreign_toplevel_manager_v1_interface.name) != 0)
        return;

    struct zwlr_foreign_toplevel_manager_v1 *manager = wl_registry_bind (
        registry, name, &zwlr_foreign_toplevel_manager_v1_interface,
        MIN (version, (uint32_t) FTL_VERSION_MAX));
    zwlr_foreign_toplevel_manager_v1_add_listener (manager, &manager_listener, NULL);
    T.available = TRUE;
}

static void
on_global_remove (void *d, struct wl_registry *r, uint32_t n)
{ (void) d; (void) r; (void) n; }

static const struct wl_registry_listener registry_listener = {
    .global        = on_global,
    .global_remove = on_global_remove,
};

/* ------------------------------------------------------------------------- */
void
shell_toplevels_init (ShellToplevelsFunc on_change, gpointer user_data)
{
    T.windows   = g_ptr_array_new_with_free_func (window_free);
    T.on_change = on_change;
    T.data      = user_data;

    GdkDisplay *gdk = gdk_display_get_default ();
    if (!GDK_IS_WAYLAND_DISPLAY (gdk)) {
        g_message ("suivi des fenetres : session non Wayland, desactive");
        return;
    }

    /* L'activation d'une fenetre exige un siege : c'est lui qui porte la
     * notion de focus cote compositeur. */
    GdkSeat *gdk_seat = gdk_display_get_default_seat (gdk);
    if (gdk_seat != NULL)
        T.seat = gdk_wayland_seat_get_wl_seat (GDK_WAYLAND_SEAT (gdk_seat));

    /* On se greffe sur la connexion et la file d'evenements que GTK a deja
     * ouvertes : c'est GDK qui les lit dans sa boucle, nos rappels seront
     * donc appeles comme n'importe quel evenement GTK. Une seconde connexion
     * couterait un descripteur et une boucle de plus pour rien. */
    struct wl_display  *display  = gdk_wayland_display_get_wl_display (GDK_WAYLAND_DISPLAY (gdk));
    struct wl_registry *registry = wl_display_get_registry (display);
    wl_registry_add_listener (registry, &registry_listener, NULL);
    wl_display_roundtrip (display);

    if (!T.available)
        g_message ("suivi des fenetres : le compositeur n'annonce pas "
                   "wlr-foreign-toplevel-management ; le dock n'indiquera "
                   "pas les applications ouvertes");
}

const GPtrArray *
shell_toplevels_get (void)
{
    return T.windows;
}

void
shell_toplevel_activate (const ShellWindow *win)
{
    if (win == NULL || win->handle == NULL || T.seat == NULL)
        return;

    /* Une fenetre reduite doit d'abord etre retablie : l'activer sans cela
     * lui donnerait le focus tout en la laissant hors de vue. */
    if (win->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized (win->handle);

    zwlr_foreign_toplevel_handle_v1_activate (win->handle, T.seat);
}

void
shell_toplevel_close (const ShellWindow *win)
{
    if (win == NULL || win->handle == NULL)
        return;

    zwlr_foreign_toplevel_handle_v1_close (win->handle);
}
