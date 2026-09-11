/* =========================================================================
 * Claude OS — la saisie au clavier à l'écran. Voir saisie.h pour le pourquoi.
 * ========================================================================= */

/* memfd_create : meson compile en c11 strict, qui ne l'expose pas. */
#define _GNU_SOURCE

#include "saisie.h"

#include <gtk/gtk.h>
#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

/* Valeurs de text-input-v3 (content_purpose, content_hint). Recopiées :
 * input-method-v2 les transmet telles quelles mais n'en porte pas les
 * constantes, et tirer tout text-input-v3 pour six nombres serait lourd. */
#define BUT_CHIFFRES      2
#define BUT_NOMBRE        3
#define BUT_TELEPHONE     4
#define BUT_MOT_DE_PASSE  8
#define BUT_PIN           9
#define INDICE_MASQUE     0x40    /* hidden_text    */
#define INDICE_SENSIBLE   0x80    /* sensitive_data */

/* Codes XKB de la disposition fabriquée : les commandes d'abord, puis Maj,
 * puis un code par caractère. Le code evdev envoyé est le code XKB moins 8
 * (convention de wl_keyboard). 255 est la limite des clients X — sans objet
 * ici, mais aucune raison de la franchir. */
#define PREMIER_CODE  9
#define CODE_MAJ      (PREMIER_CODE + SHELL_TOUCHE_N)
#define CODE_TEXTE_0  (CODE_MAJ + 1)
#define DERNIER_CODE  255

static const char *const COMMANDES[SHELL_TOUCHE_N] = {
    [SHELL_TOUCHE_EFFACER]    = "BackSpace",
    [SHELL_TOUCHE_ENTREE]     = "Return",
    [SHELL_TOUCHE_GAUCHE]     = "Left",
    [SHELL_TOUCHE_DROITE]     = "Right",
    [SHELL_TOUCHE_TABULATION] = "Tab",
};

static struct {
    struct wl_display                        *display;
    struct wl_seat                           *seat;
    struct zwp_input_method_manager_v2       *im_gestion;
    struct zwp_virtual_keyboard_manager_v1   *vk_gestion;
    struct zwp_input_method_v2               *im;
    struct zwp_virtual_keyboard_v1           *vk;

    GPtrArray  *caracteres;   /* char *, dans l'ordre des codes          */
    GHashTable *codes;        /* caractère -> code XKB                   */
    guint32     masque_maj;

    ShellSaisieFunc cb;
    gpointer        donnees;

    /* input-method : l'état ne vaut qu'à « done ». */
    gboolean       actif_attente;
    guint32        but_attente;
    guint32        indice_attente;
    gboolean       actif;
    ShellSaisieBut but;
} S;

/* -------------------------------------------------------------------------
 * La disposition fabriquée
 * ------------------------------------------------------------------------- */
static char *
nom_touche (gunichar c)
{
    char nom[64];
    xkb_keysym_t sym = xkb_utf32_to_keysym (c);
    if (sym == XKB_KEY_NoSymbol || xkb_keysym_get_name (sym, nom, sizeof nom) < 0)
        return NULL;
    return g_strdup (nom);
}

static char *
disposition_texte (void)
{
    GString *k = g_string_new ("xkb_keymap {\n"
                               "xkb_keycodes \"claude-os\" {\n"
                               "  minimum = 8;\n  maximum = 255;\n");
    guint dernier = CODE_TEXTE_0 + S.caracteres->len;
    for (guint c = PREMIER_CODE; c < dernier; c++)
        g_string_append_printf (k, "  <C%03u> = %u;\n", c, c);

    /* « complete » pour les types et la compatibilité : c'est ce que toute
     * disposition XKB inclut, et xkeyboard-config est là — labwc en a
     * besoin pour la sienne. */
    g_string_append (k, "};\n"
                        "xkb_types \"claude-os\" { include \"complete\" };\n"
                        "xkb_compat \"claude-os\" { include \"complete\" };\n"
                        "xkb_symbols \"claude-os\" {\n");

    for (int t = 0; t < SHELL_TOUCHE_N; t++)
        g_string_append_printf (k, "  key <C%03u> { [ %s ] };\n",
                                PREMIER_CODE + t, COMMANDES[t]);
    g_string_append_printf (k, "  key <C%03u> { [ Shift_L ] };\n"
                               "  modifier_map Shift { <C%03u> };\n",
                            CODE_MAJ, CODE_MAJ);

    for (guint i = 0; i < S.caracteres->len; i++) {
        const char *car = g_ptr_array_index (S.caracteres, i);
        g_autofree char *nom = nom_touche (g_utf8_get_char (car));
        if (nom == NULL)
            g_warning ("clavier à l'écran : « %s » n'a pas de nom XKB, "
                       "sa touche n'écrira rien", car);
        g_string_append_printf (k, "  key <C%03u> { [ %s ] };\n",
                                CODE_TEXTE_0 + i, nom != NULL ? nom : "NoSymbol");
    }
    g_string_append (k, "};\n};\n");
    return g_string_free (k, FALSE);
}

/* Construit, VÉRIFIE, puis envoie la disposition. Vérifiée ici, avec la même
 * bibliothèque que le compositeur : une disposition refusée par wlroots ne
 * donnerait aucune erreur au client — les touches ne feraient simplement
 * rien. */
static gboolean
envoyer_disposition (void)
{
    g_autofree char *texte = disposition_texte ();

    struct xkb_context *ctx = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *km = ctx ? xkb_keymap_new_from_string (
        ctx, texte, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    if (km == NULL) {
        g_warning ("clavier à l'écran : disposition refusée par xkbcommon — "
                   "le clavier n'écrira rien. Disposition :\n%s", texte);
        if (ctx) xkb_context_unref (ctx);
        return FALSE;
    }
    xkb_mod_index_t maj = xkb_keymap_mod_get_index (km, XKB_MOD_NAME_SHIFT);
    S.masque_maj = maj == XKB_MOD_INVALID ? 0 : 1u << maj;
    xkb_keymap_unref (km);
    xkb_context_unref (ctx);

    size_t taille = strlen (texte) + 1;
    int fd = memfd_create ("claude-os-disposition", MFD_CLOEXEC);
    if (fd < 0 || write (fd, texte, taille) != (ssize_t) taille) {
        g_warning ("clavier à l'écran : disposition non transmise : %s",
                   g_strerror (errno));
        if (fd >= 0) close (fd);
        return FALSE;
    }
    /* libwayland duplique le descripteur en le mettant en file : on peut
     * fermer le nôtre tout de suite. */
    zwp_virtual_keyboard_v1_keymap (S.vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, taille);
    close (fd);
    wl_display_flush (S.display);
    return TRUE;
}

static void
ajouter (const char *car)
{
    if (g_hash_table_contains (S.codes, car))
        return;
    char *copie = g_strdup (car);
    g_ptr_array_add (S.caracteres, copie);
    g_hash_table_insert (S.codes, copie,
                         GUINT_TO_POINTER (CODE_TEXTE_0 + S.caracteres->len - 1));
}

/* -------------------------------------------------------------------------
 * Frapper
 * ------------------------------------------------------------------------- */
static void
frapper (guint32 code, guint32 modificateurs)
{
    if (S.vk == NULL)
        return;
    guint32 t = (guint32) (g_get_monotonic_time () / 1000);

    if (modificateurs != 0)
        zwp_virtual_keyboard_v1_modifiers (S.vk, modificateurs, 0, 0, 0);
    zwp_virtual_keyboard_v1_key (S.vk, t, code - 8, WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_virtual_keyboard_v1_key (S.vk, t, code - 8, WL_KEYBOARD_KEY_STATE_RELEASED);
    if (modificateurs != 0)
        zwp_virtual_keyboard_v1_modifiers (S.vk, 0, 0, 0, 0);

    /* Tout de suite : une frappe qui attend la prochaine itération de la
     * boucle GTK arriverait par à-coups. */
    wl_display_flush (S.display);
}

void
shell_saisie_texte (const char *caractere)
{
    if (S.vk == NULL || caractere == NULL || *caractere == '\0')
        return;

    gpointer code = g_hash_table_lookup (S.codes, caractere);
    if (code == NULL) {
        if (CODE_TEXTE_0 + S.caracteres->len >= DERNIER_CODE) {
            g_warning ("clavier à l'écran : disposition pleine, « %s » perdu", caractere);
            return;
        }
        g_message ("clavier à l'écran : « %s » absent de la disposition, "
                   "ajouté (à inscrire dans la liste du clavier)", caractere);
        ajouter (caractere);
        if (!envoyer_disposition ())
            return;
        code = g_hash_table_lookup (S.codes, caractere);
    }
    frapper (GPOINTER_TO_UINT (code), 0);
}

void
shell_saisie_touche (ShellTouche t, gboolean maj)
{
    g_return_if_fail (t >= 0 && t < SHELL_TOUCHE_N);
    frapper (PREMIER_CODE + t, maj ? S.masque_maj : 0);
}

/* -------------------------------------------------------------------------
 * input-method-v2 : quand un champ prend le focus
 * ------------------------------------------------------------------------- */
static void
on_activate (void *d, struct zwp_input_method_v2 *im)
{
    (void) d; (void) im;
    S.actif_attente = TRUE;
    /* Un nouveau champ : ce qu'on savait du précédent ne vaut plus. */
    S.but_attente = 0;
    S.indice_attente = 0;
}

static void
on_deactivate (void *d, struct zwp_input_method_v2 *im)
{
    (void) d; (void) im;
    S.actif_attente = FALSE;
}

static void
on_surrounding_text (void *d, struct zwp_input_method_v2 *im,
                     const char *texte, uint32_t curseur, uint32_t ancre)
{
    (void) d; (void) im; (void) texte; (void) curseur; (void) ancre;
}

static void
on_text_change_cause (void *d, struct zwp_input_method_v2 *im, uint32_t cause)
{
    (void) d; (void) im; (void) cause;
}

static void
on_content_type (void *d, struct zwp_input_method_v2 *im,
                 uint32_t indice, uint32_t but)
{
    (void) d; (void) im;
    S.indice_attente = indice;
    S.but_attente = but;
}

static ShellSaisieBut
but_de (guint32 but, guint32 indice)
{
    switch (but) {
    case BUT_CHIFFRES: case BUT_NOMBRE: case BUT_TELEPHONE: case BUT_PIN:
        return SHELL_SAISIE_CHIFFRES;
    case BUT_MOT_DE_PASSE:
        return SHELL_SAISIE_MOT_DE_PASSE;
    }
    if (indice & (INDICE_MASQUE | INDICE_SENSIBLE))
        return SHELL_SAISIE_MOT_DE_PASSE;
    return SHELL_SAISIE_TEXTE;
}

static void
on_done (void *d, struct zwp_input_method_v2 *im)
{
    (void) d; (void) im;
    ShellSaisieBut but = but_de (S.but_attente, S.indice_attente);

    if (S.actif_attente == S.actif && (!S.actif || but == S.but))
        return;

    S.actif = S.actif_attente;
    S.but = but;
    g_debug ("saisie : champ %s (but %u, indice 0x%x)",
             S.actif ? "actif" : "inactif", S.but_attente, S.indice_attente);
    if (S.cb != NULL)
        S.cb (S.actif, S.but, S.donnees);
}

static void
on_unavailable (void *d, struct zwp_input_method_v2 *im)
{
    (void) d;
    /* Une autre méthode de saisie tient déjà le siège — squeekboard lancé à
     * la main, par exemple. On se retire : il n'y en a qu'une par siège. */
    g_warning ("clavier à l'écran : une autre méthode de saisie occupe le "
               "siège ; le clavier n'apparaîtra plus seul");
    zwp_input_method_v2_destroy (im);
    S.im = NULL;
}

static const struct zwp_input_method_v2_listener im_ecoute = {
    .activate          = on_activate,
    .deactivate        = on_deactivate,
    .surrounding_text  = on_surrounding_text,
    .text_change_cause = on_text_change_cause,
    .content_type      = on_content_type,
    .done              = on_done,
    .unavailable       = on_unavailable,
};

/* -------------------------------------------------------------------------
 * Branchement
 * ------------------------------------------------------------------------- */
static void
on_global (void *d, struct wl_registry *r, uint32_t nom, const char *interface,
           uint32_t version)
{
    (void) d; (void) version;
    if (g_strcmp0 (interface, zwp_input_method_manager_v2_interface.name) == 0)
        S.im_gestion = wl_registry_bind (r, nom, &zwp_input_method_manager_v2_interface, 1);
    else if (g_strcmp0 (interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
        S.vk_gestion = wl_registry_bind (r, nom, &zwp_virtual_keyboard_manager_v1_interface, 1);
}

static void
on_global_remove (void *d, struct wl_registry *r, uint32_t nom)
{ (void) d; (void) r; (void) nom; }

static const struct wl_registry_listener registre_ecoute = {
    .global        = on_global,
    .global_remove = on_global_remove,
};

gboolean
shell_saisie_init (const char *const *textes, ShellSaisieFunc cb, gpointer user_data)
{
    S.cb = cb;
    S.donnees = user_data;
    S.caracteres = g_ptr_array_new_with_free_func (g_free);
    S.codes = g_hash_table_new (g_str_hash, g_str_equal);
    for (const char *const *t = textes; t != NULL && *t != NULL; t++)
        ajouter (*t);

    GdkDisplay *gdk = gdk_display_get_default ();
    if (!GDK_IS_WAYLAND_DISPLAY (gdk)) {
        g_message ("clavier à l'écran : session non Wayland, désactivé");
        return FALSE;
    }
    GdkSeat *siege = gdk_display_get_default_seat (gdk);
    if (siege != NULL)
        S.seat = gdk_wayland_seat_get_wl_seat (GDK_WAYLAND_SEAT (siege));

    /* Sur la connexion de GTK, comme le suivi des fenêtres : nos rappels sont
     * appelés par sa boucle, sans descripteur ni fil de plus. */
    S.display = gdk_wayland_display_get_wl_display (GDK_WAYLAND_DISPLAY (gdk));
    struct wl_registry *registre = wl_display_get_registry (S.display);
    wl_registry_add_listener (registre, &registre_ecoute, NULL);
    wl_display_roundtrip (S.display);

    if (S.seat == NULL || S.vk_gestion == NULL) {
        g_warning ("clavier à l'écran : %s — il n'écrira rien",
                   S.seat == NULL ? "aucun siège" : "le compositeur n'offre pas "
                   "zwp_virtual_keyboard_manager_v1");
        return FALSE;
    }

    S.vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard (S.vk_gestion, S.seat);
    if (!envoyer_disposition ()) {
        zwp_virtual_keyboard_v1_destroy (S.vk);
        S.vk = NULL;
        return FALSE;
    }

    if (S.im_gestion != NULL) {
        S.im = zwp_input_method_manager_v2_get_input_method (S.im_gestion, S.seat);
        zwp_input_method_v2_add_listener (S.im, &im_ecoute, NULL);
    } else {
        g_message ("clavier à l'écran : pas d'input-method-v2, il n'apparaîtra "
                   "que sur demande");
    }

    /* Une vraie leçon de ce projet (energie.c, 9 septembre 2026) : l'init a
     * lieu avant que la boucle ne tourne, et des requêtes restées en file
     * n'atteignent le compositeur qu'au premier événement venu. */
    wl_display_flush (S.display);
    g_message ("clavier à l'écran : %u caractères dans la disposition, "
               "input-method %s", S.caracteres->len, S.im ? "branché" : "absent");
    return TRUE;
}
