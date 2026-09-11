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

/* LES CODES DE TOUCHES NE SONT PAS INDIFFÉRENTS, ET CELA A ÉTÉ PAYÉ.
 *
 * La première version donnait à chaque caractère un code à la suite des
 * autres, en croyant que seul comptait le symbole qu'on y attache. Vu le
 * 11 septembre 2026 dans Claude Desktop : « ; : ! » faisaient sauter le
 * curseur au lieu de s'écrire. Ils avaient reçu les codes evdev 101, 103
 * et 105 — KEY_LINEFEED, KEY_UP, KEY_LEFT. Chromium (donc Electron) déduit
 * du CODE la touche physique (DomCode, KeyboardEvent.code) et, pour une
 * flèche, exécute le déplacement quel que soit le symbole. ⌫ et ↵
 * occupaient les codes 1 à 5, dont KEY_ESC : ils marchaient par chance.
 *
 * D'où la règle, et elle vaut pour qui touchera à ce fichier :
 *
 *   - une touche de commande porte SON code evdev réel (⌫ = 14, ↵ = 28…) ;
 *   - un caractère ne se loge QUE sur une position de touche ordinaire —
 *     lettres, chiffres, ponctuation : celles dont le client lit le symbole
 *     dans la disposition. Il y en a 48 ; chacune porte quatre niveaux,
 *     comme une vraie disposition (aucun, Maj, AltGr, Maj+AltGr), soit 192
 *     places. Les modificateurs de chaque niveau sont demandés à
 *     xkbcommon, pas devinés. */
static const guint32 POSITIONS[] = {
     2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13,      /* 1 … =           */
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,      /* q … ]           */
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,      /* a … `           */
    43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,          /* \ z … /         */
    86,                                                  /* < (102e touche) */
};
#define NIVEAUX 4
#define PLACES  (G_N_ELEMENTS (POSITIONS) * NIVEAUX)

/* Codes evdev réels (linux/input-event-codes.h). Le code XKB est le code
 * evdev plus 8, par convention de wl_keyboard. */
#define EV_MAJ     42      /* KEY_LEFTSHIFT */
#define EV_ALTGR  100      /* KEY_RIGHTALT  */

static const struct { guint32 evdev; const char *symbole; } COMMANDES[SHELL_TOUCHE_N] = {
    [SHELL_TOUCHE_EFFACER]    = { 14, "BackSpace" },
    [SHELL_TOUCHE_ENTREE]     = { 28, "Return"    },
    [SHELL_TOUCHE_GAUCHE]     = {105, "Left"      },
    [SHELL_TOUCHE_DROITE]     = {106, "Right"     },
    [SHELL_TOUCHE_TABULATION] = { 15, "Tab"       },
};

/* Où frapper un caractère : un code XKB et les modificateurs de son niveau. */
typedef struct {
    guint32 code;
    guint32 modificateurs;
} Frappe;

static struct {
    struct wl_display                        *display;
    struct wl_seat                           *seat;
    struct zwp_input_method_manager_v2       *im_gestion;
    struct zwp_virtual_keyboard_manager_v1   *vk_gestion;
    struct zwp_input_method_v2               *im;
    struct zwp_virtual_keyboard_v1           *vk;

    GPtrArray  *caracteres;   /* char * ; l'indice i va en position i/4,
                                 niveau i%4                               */
    GHashTable *frappes;      /* caractère -> Frappe *                    */
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
nom_symbole (const char *car)
{
    char nom[64];
    xkb_keysym_t sym = xkb_utf32_to_keysym (g_utf8_get_char (car));
    if (sym == XKB_KEY_NoSymbol || xkb_keysym_get_name (sym, nom, sizeof nom) < 0) {
        g_warning ("clavier à l'écran : « %s » n'a pas de nom XKB, "
                   "sa touche n'écrira rien", car);
        return g_strdup ("NoSymbol");
    }
    return g_strdup (nom);
}

static char *
disposition_texte (void)
{
    GString *k = g_string_new ("xkb_keymap {\n"
                               "xkb_keycodes \"claude-os\" {\n"
                               "  minimum = 8;\n  maximum = 255;\n");
    for (int t = 0; t < SHELL_TOUCHE_N; t++)
        g_string_append_printf (k, "  <I%03u> = %u;\n",
                                COMMANDES[t].evdev + 8, COMMANDES[t].evdev + 8);
    g_string_append_printf (k, "  <I%03u> = %u;\n  <I%03u> = %u;\n",
                            EV_MAJ + 8, EV_MAJ + 8, EV_ALTGR + 8, EV_ALTGR + 8);
    for (guint p = 0; p < G_N_ELEMENTS (POSITIONS); p++)
        g_string_append_printf (k, "  <I%03u> = %u;\n",
                                POSITIONS[p] + 8, POSITIONS[p] + 8);

    /* « complete » pour les types et la compatibilité : c'est ce que toute
     * disposition XKB inclut — FOUR_LEVEL y est, et l'interprétation
     * d'ISO_Level3_Shift qui fait d'AltGr le modificateur LevelThree. */
    g_string_append (k, "};\n"
                        "xkb_types \"claude-os\" { include \"complete\" };\n"
                        "xkb_compat \"claude-os\" { include \"complete\" };\n"
                        "xkb_symbols \"claude-os\" {\n");

    for (int t = 0; t < SHELL_TOUCHE_N; t++)
        g_string_append_printf (k, "  key <I%03u> { [ %s ] };\n",
                                COMMANDES[t].evdev + 8, COMMANDES[t].symbole);
    g_string_append_printf (k, "  key <I%03u> { [ Shift_L ] };\n"
                               "  modifier_map Shift { <I%03u> };\n"
                               "  key <I%03u> { [ ISO_Level3_Shift ] };\n"
                               "  modifier_map Mod5 { <I%03u> };\n",
                            EV_MAJ + 8, EV_MAJ + 8, EV_ALTGR + 8, EV_ALTGR + 8);

    for (guint p = 0; p * NIVEAUX < S.caracteres->len; p++) {
        g_string_append_printf (k, "  key <I%03u> { type = \"FOUR_LEVEL\", [ ",
                                POSITIONS[p] + 8);
        for (guint n = 0; n < NIVEAUX; n++) {
            guint i = p * NIVEAUX + n;
            g_autofree char *nom = i < S.caracteres->len
                ? nom_symbole (g_ptr_array_index (S.caracteres, i))
                : g_strdup ("NoSymbol");
            g_string_append_printf (k, "%s%s", n > 0 ? ", " : "", nom);
        }
        g_string_append (k, " ] };\n");
    }
    g_string_append (k, "};\n};\n");
    return g_string_free (k, FALSE);
}

/* Construit, VÉRIFIE, puis envoie la disposition. Vérifiée ici, avec la même
 * bibliothèque que le compositeur : une disposition refusée par wlroots ne
 * donnerait aucune erreur au client — les touches ne feraient simplement
 * rien. Et c'est la disposition COMPILÉE qui dit, pour chaque caractère, où
 * il est et avec quels modificateurs : si elle ne le retrouve pas, on le
 * dit au lieu d'envoyer une touche au hasard. */
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

    g_hash_table_remove_all (S.frappes);
    guint egares = 0;
    for (guint i = 0; i < S.caracteres->len; i++) {
        const char *car = g_ptr_array_index (S.caracteres, i);
        guint32 code = POSITIONS[i / NIVEAUX] + 8;
        xkb_mod_mask_t masques[4];
        size_t n = xkb_keymap_key_get_mods_for_level (km, code, 0, i % NIVEAUX,
                                                      masques, G_N_ELEMENTS (masques));
        /* Contre-épreuve : ce niveau donne-t-il bien ce caractère ? */
        const xkb_keysym_t *syms;
        int nsyms = xkb_keymap_key_get_syms_by_level (km, code, 0, i % NIVEAUX, &syms);
        if (n == 0 || nsyms != 1
            || xkb_keysym_to_utf32 (syms[0]) != g_utf8_get_char (car)) {
            egares++;
            g_warning ("clavier à l'écran : « %s » introuvable dans la disposition "
                       "compilée (code %u, niveau %u)", car, code, i % NIVEAUX);
            continue;
        }
        Frappe *f = g_new (Frappe, 1);
        f->code = code;
        f->modificateurs = masques[0];
        g_hash_table_insert (S.frappes, (gpointer) car, f);
    }
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
    return egares == 0;
}

static gboolean
ajouter (const char *car)
{
    for (guint i = 0; i < S.caracteres->len; i++)
        if (g_str_equal (g_ptr_array_index (S.caracteres, i), car))
            return TRUE;
    if (S.caracteres->len >= PLACES) {
        g_warning ("clavier à l'écran : disposition pleine (%u places), « %s » perdu",
                   (guint) PLACES, car);
        return FALSE;
    }
    g_ptr_array_add (S.caracteres, g_strdup (car));
    return TRUE;
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

    Frappe *f = g_hash_table_lookup (S.frappes, caractere);
    if (f == NULL) {
        g_message ("clavier à l'écran : « %s » absent de la disposition, "
                   "ajouté (à inscrire dans la liste du clavier)", caractere);
        if (!ajouter (caractere))
            return;
        envoyer_disposition ();
        f = g_hash_table_lookup (S.frappes, caractere);
        if (f == NULL)
            return;
    }
    frapper (f->code, f->modificateurs);
}

void
shell_saisie_touche (ShellTouche t, gboolean maj)
{
    g_return_if_fail (t >= 0 && t < SHELL_TOUCHE_N);
    frapper (COMMANDES[t].evdev + 8, maj ? S.masque_maj : 0);
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
    /* Les clés sont les chaînes de S.caracteres, qui les possède ; les
     * valeurs, des Frappe à libérer. */
    S.frappes = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
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
