#define _GNU_SOURCE            /* memfd_create */

/* =========================================================================
 * Claude-OS — le verrou d'ecran
 *
 * Il masque la session SANS la fermer, et la rend contre le code PIN ou le
 * mot de passe. Rien n'est ferme, rien n'est relance : les applications
 * continuent derriere, les telechargements aussi.
 *
 * POURQUOI PAS DE GTK ICI, ALORS QUE TOUT LE RESTE EN EST FAIT
 *
 * Le protocole ext-session-lock-v1 demande au client de creer LUI-MEME une
 * surface par ecran, d'un type que GTK ne sait pas produire :
 * gtk4-layer-shell ne couvre que zwlr_layer_shell_v1, et aucune
 * bibliotheque equivalente pour le verrouillage n'existe dans Debian
 * trixie -- verifie. On dessine donc en Wayland brut et en Cairo.
 *
 * POURQUOI CE PROTOCOLE ET PAS UNE SURFACE « PAR-DESSUS TOUT »
 *
 * Une surface layer-shell en couche OVERLAY aurait permis de reutiliser
 * l'ecran de connexion tel quel, clavier tactile compris. Mais elle
 * disparait avec le processus : un plantage, et l'ecran se deverrouille
 * tout seul. ext-session-lock fait l'inverse -- si le verrou meurt, le
 * compositeur GARDE l'ecran bloque.
 *
 * C'est plus sur, et c'est aussi le danger : un defaut ici enferme dehors.
 * D'ou « --essai », qui verrouille pour de vrai puis rend la main tout seul
 * au bout de quelques secondes, quoi qu'il arrive. Le vrai chemin de code
 * est exerce, sans pari. A lancer AVANT de brancher le verrou, jamais
 * apres. Le getty du tty1 reste le dernier recours (invariant n^o 5).
 *
 * CE QU'IL NE FAIT PAS ENCORE : le clavier tactile. En mode tablette, il
 * faudra le clavier physique. C'est la premiere chose a ajouter.
 * ========================================================================= */

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <security/pam_appl.h>

#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "ext-session-lock-v1-client-protocol.h"

#define COFFRE_SOCK  "/run/claude-os/coffre.sock"
#define COFFRE_DELAI 15
#define PIN_LONGUEUR 6

typedef struct {
    double fond[3], texte[3], attenue[3], accent[3], danger[3];
} Palette;

/* Les deux palettes du bureau, en dur : sans GTK il n'y a pas de feuille de
 * style a lire, et le verrou doit s'afficher meme si tout le reste est
 * tombe. Le theme vient du coffre, qui est le seul a pouvoir lire le
 * repertoire personnel -- voir docs/09. */
static const Palette CLAIR = {
    { 0.961, 0.957, 0.949 }, { 0.122, 0.118, 0.114 },
    { 0.451, 0.435, 0.412 }, { 0.788, 0.463, 0.310 },
    { 0.729, 0.243, 0.196 },
};
static const Palette SOMBRE = {
    { 0.122, 0.118, 0.114 }, { 0.961, 0.957, 0.949 },
    { 0.596, 0.580, 0.553 }, { 0.847, 0.541, 0.396 },
    { 0.898, 0.400, 0.353 },
};

typedef struct Ecran Ecran;

static struct {
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_shm        *shm;
    struct wl_seat       *seat;
    struct wl_keyboard   *clavier;
    struct ext_session_lock_manager_v1 *gestionnaire;
    struct ext_session_lock_v1         *verrou;

    struct xkb_context *xkb;
    struct xkb_keymap  *keymap;
    struct xkb_state   *xkb_etat;

    GList      *ecrans;          /* Ecran *                                 */
    GMainLoop  *boucle;
    guint       source;

    const Palette *palette;
    char       *utilisateur;
    gboolean    a_un_pin;
    int         essais_restants;

    GString    *saisie;
    gboolean    mode_pin;        /* FALSE : saisie du mot de passe          */
    char       *message;         /* NULL : le message par defaut            */
    gboolean    occupe;          /* verification en cours                   */
    gboolean    fini;
} V;

struct Ecran {
    struct wl_output               *output;
    struct wl_surface              *surface;
    struct ext_session_lock_surface_v1 *lock_surface;
    uint32_t width, height;
};

static void peindre_tous (void);

/* LE JOURNAL S'ECRIT EN UTF-8, DIRECTEMENT.
 *
 * g_message passe par g_printerr, qui TRANSCODE vers l'encodage de la
 * locale. Un programme lance par un service systemd n'en a pas : LANG est
 * vide, la locale est « C », et tous les accents ressortent en « ? ».
 * Constate ici meme le 9 septembre 2026 -- « session verrouill?e » -- et
 * deja documente dans docs/09 pour le coffre. Un journal qu'on relit mal
 * est un journal qu'on ne relit pas. */
static GLogWriterOutput
journal_ecrire (GLogLevelFlags niveau, const GLogField *champs, gsize n,
                gpointer donnee)
{
    (void) niveau; (void) donnee;
    for (gsize i = 0; i < n; i++)
        if (g_strcmp0 (champs[i].key, "MESSAGE") == 0) {
            fputs ((const char *) champs[i].value, stderr);
            fputc ('\n', stderr);
            fflush (stderr);
        }
    return G_LOG_WRITER_HANDLED;
}

/* -------------------------------------------------------------------------
 * Le coffre — meme protocole que l'ecran de connexion, voir docs/09
 * ------------------------------------------------------------------------- */
static JsonNode *
coffre_demande (const char *verbe, const char *pin)
{
    g_autoptr(GSocketClient) c = g_socket_client_new ();
    g_socket_client_set_timeout (c, COFFRE_DELAI);
    g_autoptr(GSocketAddress) adresse = g_unix_socket_address_new (COFFRE_SOCK);
    g_autoptr(GError) err = NULL;

    g_autoptr(GSocketConnection) cx = g_socket_client_connect (
        c, G_SOCKET_CONNECTABLE (adresse), NULL, &err);
    if (cx == NULL) {
        g_warning ("coffre injoignable : %s", err->message);
        return NULL;
    }

    g_autoptr(JsonBuilder) b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "verbe");
    json_builder_add_string_value (b, verbe);
    json_builder_set_member_name (b, "utilisateur");
    json_builder_add_string_value (b, V.utilisateur);
    if (pin != NULL) {
        json_builder_set_member_name (b, "pin");
        json_builder_add_string_value (b, pin);
    }
    json_builder_end_object (b);

    g_autoptr(JsonGenerator) g = json_generator_new ();
    json_generator_set_root (g, json_builder_get_root (b));
    g_autofree char *ligne = json_generator_to_data (g, NULL);
    g_autofree char *envoi = g_strdup_printf ("%s\n", ligne);

    GOutputStream *sortie = g_io_stream_get_output_stream (G_IO_STREAM (cx));
    if (!g_output_stream_write_all (sortie, envoi, strlen (envoi), NULL, NULL, &err)) {
        g_warning ("coffre : envoi impossible — %s", err->message);
        return NULL;
    }

    g_autoptr(GDataInputStream) entree = g_data_input_stream_new (
        g_io_stream_get_input_stream (G_IO_STREAM (cx)));
    g_autofree char *reponse = g_data_input_stream_read_line (entree, NULL, NULL, &err);
    if (reponse == NULL)
        return NULL;

    g_autoptr(JsonParser) p = json_parser_new ();
    if (!json_parser_load_from_data (p, reponse, -1, NULL))
        return NULL;
    return json_node_ref (json_parser_get_root (p));
}

/* -------------------------------------------------------------------------
 * PAM — la meme porte que l'ecran de connexion
 *
 * Le mot de passe (tape, ou rendu par le coffre contre le PIN) est remis a
 * PAM tel quel. C'est ce qui garantit que le trousseau s'ouvre : voir
 * docs/09, l'architecture entiere du code PIN vient de la.
 * ------------------------------------------------------------------------- */
static int
pam_dialogue (int n, const struct pam_message **msg,
              struct pam_response **rep, void *donnee)
{
    const char *mdp = donnee;
    struct pam_response *r = calloc ((size_t) n, sizeof *r);
    if (r == NULL)
        return PAM_BUF_ERR;

    for (int i = 0; i < n; i++)
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF)
            r[i].resp = strdup (mdp);
    *rep = r;
    return PAM_SUCCESS;
}

static gboolean
mot_de_passe_valide (const char *mdp)
{
    struct pam_conv conv = { pam_dialogue, (void *) mdp };
    pam_handle_t *pamh = NULL;

    if (pam_start ("claude-os-verrou", V.utilisateur, &conv, &pamh) != PAM_SUCCESS)
        return FALSE;
    int r = pam_authenticate (pamh, 0);
    pam_end (pamh, r);
    return r == PAM_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Deverrouillage
 * ------------------------------------------------------------------------- */
static void
deverrouiller (void)
{
    if (V.fini)
        return;
    V.fini = TRUE;
    if (V.verrou != NULL) {
        ext_session_lock_v1_unlock_and_destroy (V.verrou);
        V.verrou = NULL;
        wl_display_flush (V.display);
    }
    g_main_loop_quit (V.boucle);
}

static void
message_poser (const char *m)
{
    g_free (V.message);
    V.message = g_strdup (m);
}

static void
verifier (void)
{
    if (V.occupe || V.saisie->len == 0)
        return;
    V.occupe = TRUE;
    message_poser ("Vérification…");
    peindre_tous ();
    wl_display_flush (V.display);

    gboolean ok = FALSE;

    if (V.mode_pin) {
        g_autoptr(JsonNode) r = coffre_demande ("ouvrir", V.saisie->str);
        if (r != NULL && JSON_NODE_HOLDS_OBJECT (r)) {
            JsonObject *o = json_node_get_object (r);
            if (json_object_has_member (o, "mdp")) {
                const char *mdp = json_object_get_string_member (o, "mdp");
                ok = mot_de_passe_valide (mdp);
                if (!ok)
                    message_poser ("Le coffre a rendu un mot de passe refusé "
                                   "par PAM. Utilisez le mot de passe.");
            } else {
                V.essais_restants = json_object_has_member (o, "essais_restants")
                    ? (int) json_object_get_int_member (o, "essais_restants") : -1;
                const char *e = json_object_get_string_member_with_default (o, "erreur", "");
                if (g_strcmp0 (e, "verrouille") == 0) {
                    V.a_un_pin = FALSE;
                    V.mode_pin = FALSE;
                    message_poser ("Code PIN supprimé après cinq erreurs. "
                                   "Mot de passe.");
                } else {
                    message_poser (NULL);
                }
            }
        } else {
            message_poser ("Coffre injoignable. Utilisez le mot de passe.");
            V.mode_pin = FALSE;
        }
    } else {
        ok = mot_de_passe_valide (V.saisie->str);
        if (!ok)
            message_poser ("Mot de passe incorrect.");
    }

    /* La saisie est effacee dans tous les cas, y compris en cas de succes :
     * elle contient un secret et n'a plus de raison d'exister. */
    memset (V.saisie->str, 0, V.saisie->len);
    g_string_set_size (V.saisie, 0);
    V.occupe = FALSE;

    if (ok)
        deverrouiller ();
    else
        peindre_tous ();
}

/* -------------------------------------------------------------------------
 * Dessin
 * ------------------------------------------------------------------------- */
static void
texte_centre (cairo_t *cr, const char *texte, double cx, double y,
              int taille, const double *couleur, double alpha)
{
    PangoLayout *l = pango_cairo_create_layout (cr);
    g_autofree char *desc = g_strdup_printf ("Inter %d", taille);
    PangoFontDescription *fd = pango_font_description_from_string (desc);
    pango_layout_set_font_description (l, fd);
    pango_font_description_free (fd);
    pango_layout_set_text (l, texte, -1);

    int lw, lh;
    pango_layout_get_pixel_size (l, &lw, &lh);
    cairo_set_source_rgba (cr, couleur[0], couleur[1], couleur[2], alpha);
    cairo_move_to (cr, cx - lw / 2.0, y);
    pango_cairo_show_layout (cr, l);
    g_object_unref (l);
}

static void
peindre (Ecran *e, cairo_t *cr)
{
    const Palette *P = V.palette;
    double w = e->width, h = e->height, cx = w / 2.0;

    cairo_set_source_rgb (cr, P->fond[0], P->fond[1], P->fond[2]);
    cairo_paint (cr);

    texte_centre (cr, V.utilisateur, cx, h * 0.34, 20, P->texte, 1.0);

    const char *invite = V.mode_pin ? "Code PIN" : "Mot de passe";
    texte_centre (cr, invite, cx, h * 0.34 + 42, 12, P->attenue, 1.0);

    /* La saisie : des pastilles pour le PIN, dont on connait la longueur ;
     * une rangee qui grandit pour le mot de passe, dont on ne la connait
     * pas -- et l'afficher renseignerait qui regarde par-dessus l'epaule. */
    double y = h * 0.34 + 96;
    if (V.mode_pin) {
        double r = 9, ecart = 30;
        double x0 = cx - (PIN_LONGUEUR - 1) * ecart / 2.0;
        for (int i = 0; i < PIN_LONGUEUR; i++) {
            gboolean plein = (i < (int) V.saisie->len);
            cairo_set_source_rgba (cr, P->accent[0], P->accent[1], P->accent[2],
                                   plein ? 1.0 : 0.22);
            /* NOUVEAU SOUS-CHEMIN AVANT CHAQUE ARC.
             *
             * pango_cairo_show_layout laisse un point courant. Sans cette
             * ligne, cairo_arc s'y RACCORDE et trace un trait depuis le
             * dernier texte jusqu'au bord du premier cercle -- vu a l'ecran
             * le 9 septembre 2026, un fil oblique en travers du panneau. */
            cairo_new_sub_path (cr);
            cairo_arc (cr, x0 + i * ecart, y, r, 0, 2 * G_PI);
            plein ? cairo_fill (cr) : cairo_stroke (cr);
        }
    } else {
        int n = (int) MIN (V.saisie->len, 24);
        double r = 5, ecart = 16;
        double x0 = cx - (n - 1) * ecart / 2.0;
        cairo_set_source_rgb (cr, P->accent[0], P->accent[1], P->accent[2]);
        for (int i = 0; i < n; i++) {
            cairo_new_sub_path (cr);
            cairo_arc (cr, x0 + i * ecart, y, r, 0, 2 * G_PI);
            cairo_fill (cr);
        }
    }

    if (V.message != NULL)
        texte_centre (cr, V.message, cx, y + 44, 12, P->danger, 1.0);
    else if (V.mode_pin && V.essais_restants >= 0 && V.essais_restants < 5) {
        g_autofree char *t = g_strdup_printf (
            "%d essai%s avant suppression du code PIN",
            V.essais_restants, V.essais_restants > 1 ? "s" : "");
        texte_centre (cr, t, cx, y + 44, 12, P->danger, 1.0);
    }

    if (V.a_un_pin)
        texte_centre (cr, V.mode_pin
                      ? "Échap : saisir le mot de passe"
                      : "Échap : revenir au code PIN",
                      cx, h * 0.34 + 200, 11, P->attenue, 0.8);
}

/* Un tampon partage neuf a chaque image. Les surfaces de verrouillage sont
 * repeintes rarement -- une frappe, un message -- et garder deux tampons en
 * rotation pour cela couterait plus de code que de temps machine. */
static struct wl_buffer *
tampon_neuf (Ecran *e)
{
    int stride = (int) e->width * 4;
    size_t taille = (size_t) stride * e->height;

    int fd = memfd_create ("claude-os-verrou", MFD_CLOEXEC);
    if (fd < 0 || ftruncate (fd, (off_t) taille) != 0) {
        g_warning ("tampon impossible : %s", g_strerror (errno));
        if (fd >= 0) close (fd);
        return NULL;
    }
    void *donnees = mmap (NULL, taille, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (donnees == MAP_FAILED) {
        g_warning ("projection impossible : %s", g_strerror (errno));
        close (fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool (V.shm, fd, (int32_t) taille);
    struct wl_buffer *b = wl_shm_pool_create_buffer (
        pool, 0, (int32_t) e->width, (int32_t) e->height, stride,
        WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy (pool);
    close (fd);

    cairo_surface_t *s = cairo_image_surface_create_for_data (
        donnees, CAIRO_FORMAT_RGB24, (int) e->width, (int) e->height, stride);
    cairo_t *cr = cairo_create (s);
    peindre (e, cr);
    cairo_destroy (cr);
    cairo_surface_destroy (s);
    munmap (donnees, taille);
    return b;
}

static void
peindre_ecran (Ecran *e)
{
    if (e->width == 0 || e->height == 0)
        return;
    struct wl_buffer *b = tampon_neuf (e);
    if (b == NULL)
        return;
    wl_surface_attach (e->surface, b, 0, 0);
    wl_surface_damage_buffer (e->surface, 0, 0, (int32_t) e->width,
                              (int32_t) e->height);
    wl_surface_commit (e->surface);
}

static void
peindre_tous (void)
{
    for (GList *l = V.ecrans; l != NULL; l = l->next)
        peindre_ecran (l->data);
}

/* -------------------------------------------------------------------------
 * Clavier
 * ------------------------------------------------------------------------- */
static void
touche (void *d, struct wl_keyboard *k, uint32_t serie, uint32_t t,
        uint32_t code, uint32_t etat)
{
    (void) d; (void) k; (void) serie; (void) t;
    if (etat != WL_KEYBOARD_KEY_STATE_PRESSED || V.xkb_etat == NULL || V.occupe)
        return;

    xkb_keycode_t kc = code + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym (V.xkb_etat, kc);

    switch (sym) {
    case XKB_KEY_Escape:
        if (V.a_un_pin) {
            V.mode_pin = !V.mode_pin;
            g_string_set_size (V.saisie, 0);
            message_poser (NULL);
        }
        break;
    case XKB_KEY_BackSpace:
        if (V.saisie->len > 0)
            g_string_truncate (V.saisie, V.saisie->len - 1);
        message_poser (NULL);
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        verifier ();
        return;
    default: {
        uint32_t u = xkb_keysym_to_utf32 (sym);
        if (u < 32 || u == 127)
            break;
        if (V.mode_pin) {
            /* Le PIN n'accepte que des chiffres, et se verifie tout seul
             * au sixieme : demander « Entree » apres six chiffres serait
             * une frappe de plus pour rien. */
            if (u < '0' || u > '9')
                break;
            if (V.saisie->len < PIN_LONGUEUR)
                g_string_append_c (V.saisie, (char) u);
            message_poser (NULL);
            if (V.saisie->len == PIN_LONGUEUR) {
                peindre_tous ();
                verifier ();
                return;
            }
        } else {
            char tampon[8];
            int n = xkb_keysym_to_utf8 (sym, tampon, sizeof tampon);
            if (n > 1)
                g_string_append_len (V.saisie, tampon, n - 1);
            message_poser (NULL);
        }
        break;
    }
    }
    peindre_tous ();
}

static void
keymap (void *d, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t taille)
{
    (void) d; (void) k;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close (fd); return; }
    char *carte = mmap (NULL, taille, PROT_READ, MAP_PRIVATE, fd, 0);
    if (carte == MAP_FAILED) { close (fd); return; }

    xkb_keymap_unref (V.keymap);
    xkb_state_unref (V.xkb_etat);
    V.keymap = xkb_keymap_new_from_string (V.xkb, carte,
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    V.xkb_etat = (V.keymap != NULL) ? xkb_state_new (V.keymap) : NULL;
    munmap (carte, taille);
    close (fd);
}

static void
modificateurs (void *d, struct wl_keyboard *k, uint32_t serie,
               uint32_t depr, uint32_t verr, uint32_t bloq, uint32_t groupe)
{
    (void) d; (void) k; (void) serie;
    if (V.xkb_etat != NULL)
        xkb_state_update_mask (V.xkb_etat, depr, verr, bloq, 0, 0, groupe);
}

static void kb_entree (void *d, struct wl_keyboard *k, uint32_t s,
                       struct wl_surface *su, struct wl_array *a)
{ (void)d;(void)k;(void)s;(void)su;(void)a; }
static void kb_sortie (void *d, struct wl_keyboard *k, uint32_t s,
                       struct wl_surface *su)
{ (void)d;(void)k;(void)s;(void)su; }
static void kb_repet (void *d, struct wl_keyboard *k, int32_t r, int32_t del)
{ (void)d;(void)k;(void)r;(void)del; }

static const struct wl_keyboard_listener ecouteur_clavier = {
    keymap, kb_entree, kb_sortie, touche, modificateurs, kb_repet,
};

/* -------------------------------------------------------------------------
 * Surfaces de verrouillage
 * ------------------------------------------------------------------------- */
static void
surface_configure (void *d, struct ext_session_lock_surface_v1 *s,
                   uint32_t serie, uint32_t w, uint32_t h)
{
    Ecran *e = d;
    e->width = w;
    e->height = h;
    ext_session_lock_surface_v1_ack_configure (s, serie);
    peindre_ecran (e);
}

static const struct ext_session_lock_surface_v1_listener ecouteur_surface = {
    surface_configure,
};

static void
ecran_armer (Ecran *e)
{
    if (e->lock_surface != NULL || V.verrou == NULL)
        return;
    e->surface = wl_compositor_create_surface (V.compositor);
    e->lock_surface = ext_session_lock_v1_get_lock_surface (
        V.verrou, e->surface, e->output);
    ext_session_lock_surface_v1_add_listener (e->lock_surface,
                                              &ecouteur_surface, e);
}

/* -------------------------------------------------------------------------
 * Le verrou lui-meme
 * ------------------------------------------------------------------------- */
static void
verrou_locked (void *d, struct ext_session_lock_v1 *v)
{
    (void) d; (void) v;
    g_message ("verrou : session verrouillée");
}

static void
verrou_finished (void *d, struct ext_session_lock_v1 *v)
{
    (void) d; (void) v;
    /* Le compositeur refuse le verrouillage -- une autre instance tient
     * deja le verrou. On sort SANS appeler unlock_and_destroy : ce serait
     * deverrouiller la session d'un autre. */
    g_message ("verrou : refusé par le compositeur (déjà verrouillé ?)");
    V.fini = TRUE;
    g_main_loop_quit (V.boucle);
}

static const struct ext_session_lock_v1_listener ecouteur_verrou = {
    verrou_locked, verrou_finished,
};

/* -------------------------------------------------------------------------
 * Registre
 * ------------------------------------------------------------------------- */
static void
global (void *d, struct wl_registry *r, uint32_t nom, const char *iface, uint32_t ver)
{
    (void) d; (void) ver;
    if (strcmp (iface, wl_compositor_interface.name) == 0)
        V.compositor = wl_registry_bind (r, nom, &wl_compositor_interface, 4);
    else if (strcmp (iface, wl_shm_interface.name) == 0)
        V.shm = wl_registry_bind (r, nom, &wl_shm_interface, 1);
    else if (strcmp (iface, ext_session_lock_manager_v1_interface.name) == 0)
        V.gestionnaire = wl_registry_bind (r, nom,
            &ext_session_lock_manager_v1_interface, 1);
    else if (strcmp (iface, wl_seat_interface.name) == 0 && V.seat == NULL) {
        V.seat = wl_registry_bind (r, nom, &wl_seat_interface, 4);
        V.clavier = wl_seat_get_keyboard (V.seat);
        if (V.clavier != NULL)
            wl_keyboard_add_listener (V.clavier, &ecouteur_clavier, NULL);
    } else if (strcmp (iface, wl_output_interface.name) == 0) {
        Ecran *e = g_new0 (Ecran, 1);
        e->output = wl_registry_bind (r, nom, &wl_output_interface, 3);
        V.ecrans = g_list_append (V.ecrans, e);
        ecran_armer (e);
    }
}

static void global_parti (void *d, struct wl_registry *r, uint32_t n)
{ (void)d;(void)r;(void)n; }

static const struct wl_registry_listener ecouteur_registre = {
    global, global_parti,
};

/* -------------------------------------------------------------------------
 * Boucle
 * ------------------------------------------------------------------------- */
static gboolean
wayland_lisible (gint fd, GIOCondition c, gpointer d)
{
    (void) fd; (void) d;
    if ((c & (G_IO_ERR | G_IO_HUP)) || wl_display_dispatch (V.display) < 0) {
        g_message ("verrou : connexion Wayland rompue");
        V.source = 0;
        g_main_loop_quit (V.boucle);
        return G_SOURCE_REMOVE;
    }
    wl_display_flush (V.display);
    return G_SOURCE_CONTINUE;
}

static gboolean
essai_ecoule (gpointer d)
{
    (void) d;
    g_message ("verrou : fin de l'essai, déverrouillage automatique");
    deverrouiller ();
    return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
    int essai = 0;
    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix (argv[i], "--essai")) {
            const char *eg = strchr (argv[i], '=');
            essai = (eg != NULL) ? atoi (eg + 1) : 30;
            if (essai <= 0) essai = 30;
        } else {
            g_printerr ("argument ignoré : %s\n", argv[i]);
        }
    }

    g_log_set_writer_func (journal_ecrire, NULL, NULL);

    V.utilisateur = g_strdup (g_get_user_name ());
    V.saisie = g_string_new (NULL);
    V.essais_restants = -1;
    V.palette = &SOMBRE;

    /* Le coffre dit s'il y a un PIN, combien d'essais restent, et quel
     * theme le bureau utilise -- il est le seul a pouvoir lire le
     * repertoire personnel. S'il ne repond pas, on se rabat sur le mot de
     * passe : un coffre muet ne doit pas empecher de rentrer chez soi. */
    g_autoptr(JsonNode) etat = coffre_demande ("etat", NULL);
    if (etat != NULL && JSON_NODE_HOLDS_OBJECT (etat)) {
        JsonObject *o = json_node_get_object (etat);
        V.a_un_pin = json_object_get_boolean_member_with_default (o, "pin", FALSE);
        V.essais_restants = (int) json_object_get_int_member_with_default (
            o, "essais_restants", -1);
        const char *th = json_object_get_string_member_with_default (o, "theme", "");
        if (strstr (th, "clair") != NULL)
            V.palette = &CLAIR;
    }
    V.mode_pin = V.a_un_pin;

    V.display = wl_display_connect (NULL);
    if (V.display == NULL) {
        g_printerr ("verrou : pas de compositeur Wayland\n");
        return 2;
    }
    V.xkb = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
    V.registry = wl_display_get_registry (V.display);
    wl_registry_add_listener (V.registry, &ecouteur_registre, NULL);
    wl_display_roundtrip (V.display);

    if (V.gestionnaire == NULL || V.compositor == NULL || V.shm == NULL) {
        g_printerr ("verrou : le compositeur n'annonce pas "
                    "ext_session_lock_manager_v1\n");
        return 3;
    }

    V.verrou = ext_session_lock_manager_v1_lock (V.gestionnaire);
    ext_session_lock_v1_add_listener (V.verrou, &ecouteur_verrou, NULL);
    for (GList *l = V.ecrans; l != NULL; l = l->next)
        ecran_armer (l->data);
    wl_display_roundtrip (V.display);

    V.boucle = g_main_loop_new (NULL, FALSE);
    V.source = g_unix_fd_add (wl_display_get_fd (V.display), G_IO_IN,
                              wayland_lisible, NULL);
    if (essai > 0) {
        g_message ("verrou : mode essai, déverrouillage dans %d s", essai);
        g_timeout_add_seconds ((guint) essai, essai_ecoule, NULL);
    }

    wl_display_flush (V.display);
    g_main_loop_run (V.boucle);

    wl_display_roundtrip (V.display);
    wl_display_disconnect (V.display);
    return 0;
}
