/* Claude OS -- la cinematheque du lecteur video. Voir l'en-tete pour le
 * pourquoi ; ici, le comment et les pieges.
 */

#define _GNU_SOURCE 1

#include "video-bibliotheque.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <string.h>

/* Seize neuvièmes, et une taille fixe : une grille dont les cases changent de
 * taille selon l'image qu'elles portent est illisible. */
#define VIGNETTE_L 280
#define VIGNETTE_H 158

/* Au-dela, on arrete de parcourir : un dossier personnel entier mis dans la
 * bibliotheque ne doit pas faire enfler la memoire sans fin. */
#define MAX_ENTREES 4000
#define MAX_PROFONDEUR 4

typedef struct {
    gchar   *chemin;
    gchar   *titre;
    gint64   vue;          /* horodatage de la derniere lecture, 0 sinon */
    double   duree;
} Entree;

struct _VideoBib {
    VideoBibChoix   choix;
    VideoBibOuvrir  ouvrir;
    gpointer        usager;

    GtkWidget      *racine;
    GtkWidget      *pile;          /* recents / bibliotheque / accueil     */
    GtkWidget      *bascule;
    GtkWidget      *grille_rec;
    GtkWidget      *grille_bib;
    GtkWidget      *compte;        /* « 184 vidéos »                       */
    GtkWidget      *rec_vide;

    GKeyFile       *cat;           /* le catalogue, sur le disque          */
    GPtrArray      *dossiers;      /* gchar*                               */
    GHashTable     *connues;       /* chemin -> Entree*, tout le catalogue */

    GCancellable   *annulation;

    /* LE FIL DES VIGNETTES. Un seul, et une file : deux cents films, c'est
     * deux cents decodages, et ils ne doivent ni bloquer l'interface ni se
     * disputer le decodeur du film en cours. */
    GThread        *fil;
    GAsyncQueue    *file;
    gboolean        quitte;
};

/* DEUX ADAPTATEURS, ET NON DEUX CONVERSIONS.
 *
 * GdkPixbufDestroyNotify et GClosureNotify ne prennent pas les memes
 * arguments que g_free. Convertir le pointeur de fonction « marche » sur
 * cette machine et casse ailleurs -- c'est un comportement indefini, et le
 * compilateur a raison de s'en plaindre. Deux lignes suffisent a bien
 * faire. */
static void liberer_pixels(guchar *pixels, gpointer data)
{
    (void) data;
    g_free(pixels);
}

static void liberer_donnee(gpointer data, GClosure *fermeture)
{
    (void) fermeture;
    g_free(data);
}

/* ------------------------------------------------------------- catalogue */

static gchar *chemin_catalogue(void)
{
    return g_build_filename(g_get_user_state_dir(), "claude-os",
                            "video-bibliotheque", NULL);
}

static gchar *chemin_vignette(const char *fichier)
{
    g_autofree gchar *somme = g_compute_checksum_for_string(G_CHECKSUM_SHA256,
                                                            fichier, -1);
    g_autofree gchar *nom = g_strconcat(somme, ".png", NULL);
    return g_build_filename(g_get_user_cache_dir(), "claude-os", "video",
                            "vignettes", nom, NULL);
}

/* Les cles d'un GKeyFile ne supportent ni « = » ni « [ » : on echappe. */
static gchar *cle_de(const char *chemin) { return g_uri_escape_string(chemin, NULL, TRUE); }

static void entree_libre(gpointer p)
{
    Entree *e = p;
    g_free(e->chemin);
    g_free(e->titre);
    g_free(e);
}

static void catalogue_ecrire(VideoBib *b)
{
    g_autofree gchar *f = chemin_catalogue();
    g_autofree gchar *dossier = g_path_get_dirname(f);
    g_mkdir_with_parents(dossier, 0700);

    GError *e = NULL;
    if (!g_key_file_save_to_file(b->cat, f, &e)) {
        /* INVARIANT N.4 : meme un catalogue sans importance dit quand il
         * echoue. Un disque plein se remarque ici avant ailleurs. */
        g_message("video-bib : catalogue non enregistré (%s)",
                  e ? e->message : "?");
        g_clear_error(&e);
    }
}

static void catalogue_lire(VideoBib *b)
{
    g_autofree gchar *f = chemin_catalogue();
    g_key_file_load_from_file(b->cat, f, G_KEY_FILE_NONE, NULL);

    g_ptr_array_set_size(b->dossiers, 0);
    gsize n = 0;
    g_auto(GStrv) liste = g_key_file_get_string_list(b->cat, "dossiers",
                                                     "liste", &n, NULL);
    for (gsize i = 0; i < n; i++)
        g_ptr_array_add(b->dossiers, g_strdup(liste[i]));
}

static void dossier_ajouter(VideoBib *b, const char *dossier)
{
    for (guint i = 0; i < b->dossiers->len; i++)
        if (g_strcmp0(g_ptr_array_index(b->dossiers, i), dossier) == 0) return;

    g_ptr_array_add(b->dossiers, g_strdup(dossier));

    g_autofree const char **tab = g_new0(const char *, b->dossiers->len + 1);
    for (guint i = 0; i < b->dossiers->len; i++)
        tab[i] = g_ptr_array_index(b->dossiers, i);
    g_key_file_set_string_list(b->cat, "dossiers", "liste",
                               tab, b->dossiers->len);
    catalogue_ecrire(b);
}

static gchar *duree_texte(double s);

/* -------------------------------------------------------- les vignettes */

typedef struct {
    VideoBib *bib;
    gchar    *chemin;
    GtkWidget *image;      /* la carte a mettre a jour, ou NULL            */
    GtkWidget *duree_vue;  /* l'etiquette de duree, remplie apres coup      */
    gchar    *titre;       /* rempli par le fil                            */
    double    duree;
} Tache;

static void tache_libre(Tache *t)
{
    g_free(t->chemin);
    g_free(t->titre);
    g_free(t);
}

/* FABRIQUER UNE VIGNETTE : ouvrir, se placer a un dixieme du film, decoder
 * UNE image, la reduire, l'ecrire.
 *
 * Un dixieme, et non le debut : les premieres secondes d'un film sont
 * presque toujours noires, et une bibliotheque de rectangles noirs ne sert
 * a rien. */
static gboolean vignette_fabriquer(const char *fichier, const char *sortie,
                                   gchar **titre, double *duree)
{
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, fichier, NULL, NULL) < 0) return FALSE;
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); return FALSE; }

    if (fmt->duration != AV_NOPTS_VALUE) *duree = (double)fmt->duration / AV_TIME_BASE;

    AVDictionaryEntry *t = av_dict_get(fmt->metadata, "title", NULL, 0);
    if (t && t->value && *t->value) *titre = g_strdup(t->value);

    const AVCodec *codec = NULL;
    int piste = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (piste < 0 || !codec) { avformat_close_input(&fmt); return FALSE; }

    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec) { avformat_close_input(&fmt); return FALSE; }
    avcodec_parameters_to_context(dec, fmt->streams[piste]->codecpar);

    /* DECODAGE LOGICIEL, ET C'EST VOULU. Une seule image ne merite pas
     * qu'on dispute le decodeur materiel au film en cours de lecture. */
    dec->thread_count = 1;
    if (avcodec_open2(dec, codec, NULL) < 0) {
        avcodec_free_context(&dec); avformat_close_input(&fmt); return FALSE;
    }

    if (*duree > 20.0) {
        int64_t ou = (int64_t)(*duree * 0.1 * AV_TIME_BASE);
        if (avformat_seek_file(fmt, -1, INT64_MIN, ou, ou, 0) >= 0)
            avcodec_flush_buffers(dec);
    }

    AVPacket *pk = av_packet_alloc();
    AVFrame  *tr = av_frame_alloc();
    gboolean  ok = FALSE;
    int       tours = 0;

    while (!ok && tours++ < 400 && av_read_frame(fmt, pk) >= 0) {
        if (pk->stream_index == piste && avcodec_send_packet(dec, pk) >= 0) {
            while (avcodec_receive_frame(dec, tr) == 0) {
                struct SwsContext *sws = sws_getContext(
                    tr->width, tr->height, tr->format,
                    VIGNETTE_L, VIGNETTE_H, AV_PIX_FMT_RGB24,
                    SWS_BILINEAR, NULL, NULL, NULL);
                if (sws) {
                    int pas = VIGNETTE_L * 3;
                    guchar *pix = g_malloc((gsize)pas * VIGNETTE_H);
                    uint8_t *plans[4] = { pix, NULL, NULL, NULL };
                    int      pass[4]  = { pas, 0, 0, 0 };
                    sws_scale(sws, (const uint8_t * const *)tr->data,
                              tr->linesize, 0, tr->height, plans, pass);
                    sws_freeContext(sws);

                    GdkPixbuf *pb = gdk_pixbuf_new_from_data(
                        pix, GDK_COLORSPACE_RGB, FALSE, 8,
                        VIGNETTE_L, VIGNETTE_H, pas,
                        liberer_pixels, NULL);
                    if (pb) {
                        g_autofree gchar *dossier = g_path_get_dirname(sortie);
                        g_mkdir_with_parents(dossier, 0700);
                        ok = gdk_pixbuf_save(pb, sortie, "png", NULL, NULL);
                        g_object_unref(pb);
                    } else {
                        g_free(pix);
                    }
                }
                av_frame_unref(tr);
                break;
            }
        }
        av_packet_unref(pk);
    }

    av_frame_free(&tr);
    av_packet_free(&pk);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return ok;
}

/* Retour dans le fil principal : poser l'image et noter ce qu'on a appris. */
static gboolean vignette_posee(gpointer p)
{
    Tache *t = p;
    VideoBib *b = t->bib;

    if (!b->quitte) {
        g_autofree gchar *v = chemin_vignette(t->chemin);
        if (t->image && GTK_IS_PICTURE(t->image) &&
            g_file_test(v, G_FILE_TEST_EXISTS)) {
            GdkTexture *tex = gdk_texture_new_from_filename(v, NULL);
            if (tex) {
                gtk_picture_set_paintable(GTK_PICTURE(t->image), GDK_PAINTABLE(tex));
                g_object_unref(tex);
            }
        }

        /* LA DUREE N'EST CONNUE QU'APRES COUP : elle vient de l'ouverture
         * du fichier, que seule la fabrication de la vignette fait. La
         * carte existait deja ; on la complete plutot que de la refaire. */
        if (t->duree > 0 && t->duree_vue && GTK_IS_LABEL(t->duree_vue)) {
            g_autofree gchar *d = duree_texte(t->duree);
            gtk_label_set_text(GTK_LABEL(t->duree_vue), d);
            gtk_widget_set_visible(t->duree_vue, TRUE);
        }

        g_autofree gchar *cle = cle_de(t->chemin);
        gboolean neuf = FALSE;
        if (t->titre && *t->titre) {
            g_key_file_set_string(b->cat, "titres", cle, t->titre);
            neuf = TRUE;
        }
        if (t->duree > 0) {
            g_key_file_set_double(b->cat, "durees", cle, t->duree);
            neuf = TRUE;
        }
        if (neuf) catalogue_ecrire(b);
    }

    tache_libre(t);
    return G_SOURCE_REMOVE;
}

static gpointer fil_vignettes(gpointer data)
{
    VideoBib *b = data;

    while (TRUE) {
        Tache *t = g_async_queue_pop(b->file);
        if (!t || b->quitte) { if (t) tache_libre(t); break; }

        g_autofree gchar *v = chemin_vignette(t->chemin);
        if (!g_file_test(v, G_FILE_TEST_EXISTS))
            vignette_fabriquer(t->chemin, v, &t->titre, &t->duree);

        g_idle_add(vignette_posee, t);
    }
    return NULL;
}

/* Demande la vignette d'une carte. Appele au « map » de la carte : rien
 * n'est fabrique pour ce qu'on ne regarde pas. */
static void sur_carte_affichee(GtkWidget *image, gpointer u)
{
    VideoBib *b = g_object_get_data(G_OBJECT(image), "bib");
    const char *chemin = u;
    if (!b || b->quitte) return;
    if (g_object_get_data(G_OBJECT(image), "demandee")) return;
    g_object_set_data(G_OBJECT(image), "demandee", GINT_TO_POINTER(1));

    Tache *t = g_new0(Tache, 1);
    t->bib = b;
    t->chemin = g_strdup(chemin);
    t->image = image;
    t->duree_vue = g_object_get_data(G_OBJECT(image), "duree");
    g_async_queue_push(b->file, t);
}

/* ------------------------------------------------------------ les cartes */

static gchar *duree_texte(double s)
{
    if (s <= 0) return g_strdup("");
    int total = (int)(s + 0.5);
    int h = total / 3600, m = (total / 60) % 60;
    if (h > 0)     return g_strdup_printf("%d h %02d", h, m);
    if (total >= 60) return g_strdup_printf("%d min", m);
    /* « 0 min » pour un extrait de cinq secondes ne dit rien. */
    return g_strdup_printf("%d s", total);
}

static void sur_carte_activee(GtkFlowBox *b, GtkFlowBoxChild *enfant, gpointer u)
{
    (void) b;
    VideoBib *bib = u;
    const char *chemin = g_object_get_data(G_OBJECT(enfant), "chemin");
    if (chemin && bib->choix) bib->choix(chemin, bib->usager);
}

static GtkWidget *carte_nouvelle(VideoBib *b, const Entree *e)
{
    GtkWidget *boite = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(boite, "video-carte");

    GtkWidget *image = gtk_picture_new();
    gtk_widget_add_css_class(image, "video-carte-image");
    gtk_widget_set_size_request(image, VIGNETTE_L, VIGNETTE_H);
    gtk_picture_set_content_fit(GTK_PICTURE(image), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink(GTK_PICTURE(image), TRUE);
    g_object_set_data(G_OBJECT(image), "bib", b);

    /* La vignette n'est demandee qu'a l'apparition de la carte. */
    g_signal_connect_data(image, "map", G_CALLBACK(sur_carte_affichee),
                          g_strdup(e->chemin), liberer_donnee, 0);

    GtkWidget *titre = gtk_label_new(e->titre);
    gtk_widget_add_css_class(titre, "video-carte-titre");
    gtk_label_set_ellipsize(GTK_LABEL(titre), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(titre), 2);
    gtk_label_set_wrap(GTK_LABEL(titre), TRUE);
    gtk_label_set_xalign(GTK_LABEL(titre), 0.0);
    gtk_widget_set_size_request(titre, VIGNETTE_L, -1);

    g_autofree gchar *d = duree_texte(e->duree);
    GtkWidget *sous = gtk_label_new(d);
    gtk_widget_add_css_class(sous, "video-carte-duree");
    gtk_label_set_xalign(GTK_LABEL(sous), 0.0);

    g_object_set_data(G_OBJECT(image), "duree", sous);
    gtk_widget_set_visible(sous, *d != '\0');

    gtk_box_append(GTK_BOX(boite), image);
    gtk_box_append(GTK_BOX(boite), titre);
    gtk_box_append(GTK_BOX(boite), sous);

    GtkWidget *enfant = gtk_flow_box_child_new();
    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(enfant), boite);
    g_object_set_data_full(G_OBJECT(enfant), "chemin", g_strdup(e->chemin), g_free);
    return enfant;
}

/* ------------------------------------------------------------ le parcours */

typedef struct { VideoBib *bib; int profondeur; } Parcours;

static void parcourir(VideoBib *b, GFile *dir, int profondeur);

static void sur_lot(GObject *src, GAsyncResult *res, gpointer u)
{
    GFileEnumerator *e = G_FILE_ENUMERATOR(src);
    Parcours *p = u;
    GError *err = NULL;
    GList *lot = g_file_enumerator_next_files_finish(e, res, &err);

    if (err || !lot) {
        if (err && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_message("video-bib : dossier illisible (%s)", err->message);
        g_clear_error(&err);
        g_object_unref(e);
        g_free(p);
        return;
    }

    VideoBib *b = p->bib;
    GFile *dir = g_file_enumerator_get_container(e);

    for (GList *l = lot; l; l = l->next) {
        GFileInfo *info = l->data;
        if (g_file_info_get_is_hidden(info)) continue;

        g_autoptr(GFile) f = g_file_get_child(dir, g_file_info_get_name(info));
        g_autofree gchar *chemin = g_file_get_path(f);
        if (!chemin) continue;

        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
            if (p->profondeur < MAX_PROFONDEUR) parcourir(b, f, p->profondeur + 1);
            continue;
        }

        const char *type = g_file_info_get_content_type(info);
        if (!type || !g_str_has_prefix(type, "video/")) continue;
        if (g_hash_table_size(b->connues) >= MAX_ENTREES) break;
        if (g_hash_table_contains(b->connues, chemin)) continue;

        Entree *ent = g_new0(Entree, 1);
        ent->chemin = g_strdup(chemin);
        g_autofree gchar *cle = cle_de(chemin);
        ent->titre = g_key_file_get_string(b->cat, "titres", cle, NULL);
        if (!ent->titre || !*ent->titre) {
            g_free(ent->titre);
            g_autofree gchar *base = g_path_get_basename(chemin);
            gchar *point = strrchr(base, '.');
            if (point && point != base) *point = '\0';
            ent->titre = g_strdup(base);
        }
        ent->duree = g_key_file_get_double(b->cat, "durees", cle, NULL);
        ent->vue   = g_key_file_get_int64(b->cat, "vues", cle, NULL);

        g_hash_table_insert(b->connues, g_strdup(chemin), ent);
        gtk_flow_box_append(GTK_FLOW_BOX(b->grille_bib), carte_nouvelle(b, ent));
    }
    g_list_free_full(lot, g_object_unref);

    g_autofree gchar *n = g_strdup_printf("%u vidéo%s",
                                          g_hash_table_size(b->connues),
                                          g_hash_table_size(b->connues) > 1 ? "s" : "");
    gtk_label_set_text(GTK_LABEL(b->compte), n);

    g_file_enumerator_next_files_async(e, 32, G_PRIORITY_LOW, b->annulation,
                                       sur_lot, p);
}

static void sur_enumeration(GObject *src, GAsyncResult *res, gpointer u)
{
    Parcours *p = u;
    GError *err = NULL;
    GFileEnumerator *e = g_file_enumerate_children_finish(G_FILE(src), res, &err);
    if (!e) {
        if (err && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_message("video-bib : dossier illisible (%s)", err->message);
        g_clear_error(&err);
        g_free(p);
        return;
    }
    g_file_enumerator_next_files_async(e, 32, G_PRIORITY_LOW, p->bib->annulation,
                                       sur_lot, p);
}

/* TOUT EST ASYNCHRONE, SANS EXCEPTION. Un dossier sur un serveur endormi
 * gelerait la fenetre entiere jusqu'au delai TCP. */
static void parcourir(VideoBib *b, GFile *dir, int profondeur)
{
    Parcours *p = g_new0(Parcours, 1);
    p->bib = b;
    p->profondeur = profondeur;

    g_file_enumerate_children_async(dir,
        G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
        G_FILE_QUERY_INFO_NONE, G_PRIORITY_LOW, b->annulation,
        sur_enumeration, p);
}

/* ------------------------------------------------------------ les volets */

static int comparer_vues(gconstpointer x, gconstpointer y)
{
    const Entree *a = *(const Entree * const *)x;
    const Entree *c = *(const Entree * const *)y;
    return (c->vue > a->vue) - (c->vue < a->vue);   /* le plus recent d'abord */
}

static void remplir_recents(VideoBib *b)
{
    GtkWidget *enfant;
    while ((enfant = gtk_widget_get_first_child(b->grille_rec)))
        gtk_flow_box_remove(GTK_FLOW_BOX(b->grille_rec), enfant);

    g_autoptr(GPtrArray) liste = g_ptr_array_new_with_free_func(entree_libre);
    gsize n = 0;
    g_auto(GStrv) cles = g_key_file_get_keys(b->cat, "vues", &n, NULL);

    for (gsize i = 0; i < n; i++) {
        g_autofree gchar *chemin = g_uri_unescape_string(cles[i], NULL);
        if (!chemin || !g_file_test(chemin, G_FILE_TEST_EXISTS)) continue;

        Entree *e = g_new0(Entree, 1);
        e->chemin = g_strdup(chemin);
        e->vue    = g_key_file_get_int64(b->cat, "vues", cles[i], NULL);
        e->duree  = g_key_file_get_double(b->cat, "durees", cles[i], NULL);
        e->titre  = g_key_file_get_string(b->cat, "titres", cles[i], NULL);
        if (!e->titre || !*e->titre) {
            g_free(e->titre);
            g_autofree gchar *base = g_path_get_basename(chemin);
            gchar *point = strrchr(base, '.');
            if (point && point != base) *point = '\0';
            e->titre = g_strdup(base);
        }
        g_ptr_array_add(liste, e);
    }

    g_ptr_array_sort(liste, comparer_vues);
    for (guint i = 0; i < liste->len && i < 60; i++)
        gtk_flow_box_append(GTK_FLOW_BOX(b->grille_rec),
                            carte_nouvelle(b, g_ptr_array_index(liste, i)));

    /* Un volet vide DIT qu'il est vide. Une page blanche laisse croire a
     * un chargement qui n'arrive jamais. */
    gtk_widget_set_visible(b->rec_vide, liste->len == 0);
}

void video_bib_rafraichir(VideoBib *b)
{
    if (!b) return;

    catalogue_lire(b);

    GtkWidget *enfant;
    while ((enfant = gtk_widget_get_first_child(b->grille_bib)))
        gtk_flow_box_remove(GTK_FLOW_BOX(b->grille_bib), enfant);
    g_hash_table_remove_all(b->connues);

    for (guint i = 0; i < b->dossiers->len; i++) {
        g_autoptr(GFile) d = g_file_new_for_path(g_ptr_array_index(b->dossiers, i));
        parcourir(b, d, 0);
    }

    remplir_recents(b);

    gboolean vide = (b->dossiers->len == 0);
    gtk_stack_set_visible_child_name(GTK_STACK(b->pile),
                                     vide ? "accueil" : "biblio");
    gtk_widget_set_visible(b->bascule, !vide);
}

void video_bib_vue(VideoBib *b, const char *chemin)
{
    if (!b || !chemin) return;

    g_autofree gchar *cle = cle_de(chemin);
    g_key_file_set_int64(b->cat, "vues", cle, g_get_real_time() / G_USEC_PER_SEC);
    catalogue_ecrire(b);

    /* LE DOSSIER ENTRE DANS LA BIBLIOTHEQUE TOUT SEUL. Ouvrir un film, c'est
     * declarer l'endroit ou on les range : demander a l'utilisateur de le
     * faire une seconde fois serait de la paperasse. */
    g_autoptr(GFile) f = g_file_new_for_path(chemin);
    g_autoptr(GFile) dir = g_file_get_parent(f);
    if (dir) {
        g_autofree gchar *d = g_file_get_path(dir);
        if (d) dossier_ajouter(b, d);
    }
}

/* ------------------------------------------------------------- commandes */

static void sur_dossier_choisi(GObject *src, GAsyncResult *res, gpointer u)
{
    VideoBib *b = u;
    GError *e = NULL;
    g_autoptr(GFile) d = gtk_file_dialog_select_folder_finish(
        GTK_FILE_DIALOG(src), res, &e);
    if (!d) { g_clear_error(&e); return; }

    g_autofree gchar *chemin = g_file_get_path(d);
    if (chemin) {
        dossier_ajouter(b, chemin);
        video_bib_rafraichir(b);
    }
}

static void act_ajouter_dossier(GtkButton *bt, gpointer u)
{
    (void) bt;
    VideoBib *b = u;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Ajouter un dossier à la bibliothèque");
    GtkRoot *r = gtk_widget_get_root(b->racine);
    gtk_file_dialog_select_folder(d, GTK_IS_WINDOW(r) ? GTK_WINDOW(r) : NULL,
                                  NULL, sur_dossier_choisi, b);
    g_object_unref(d);
}

static void act_ouvrir_fichier(GtkButton *bt, gpointer u)
{
    (void) bt;
    VideoBib *b = u;
    if (b->ouvrir) b->ouvrir(b->usager);
}

/* ------------------------------------------------------------ fabrication */

static GtkWidget *grille_dans_defilement(GtkWidget **grille, VideoBib *b,
                                         const char *titre)
{
    *grille = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(*grille), GTK_SELECTION_NONE);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(*grille), TRUE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(*grille), TRUE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(*grille), 16);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(*grille), 16);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(*grille), 2);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(*grille), 8);
    gtk_widget_add_css_class(*grille, "video-grille");
    g_signal_connect(*grille, "child-activated",
                     G_CALLBACK(sur_carte_activee), b);

    GtkWidget *def = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(def),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(def), *grille);
    gtk_widget_set_vexpand(def, TRUE);

    GtkWidget *boite = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    if (titre) {
        GtkWidget *l = gtk_label_new(titre);
        gtk_widget_add_css_class(l, "video-bib-section");
        gtk_label_set_xalign(GTK_LABEL(l), 0.0);
        gtk_box_append(GTK_BOX(boite), l);
    }
    gtk_box_append(GTK_BOX(boite), def);
    return boite;
}

VideoBib *video_bib_nouveau(VideoBibChoix choix, VideoBibOuvrir ouvrir,
                            gpointer usager)
{
    VideoBib *b = g_new0(VideoBib, 1);
    b->choix = choix;
    b->ouvrir = ouvrir;
    b->usager = usager;
    b->cat = g_key_file_new();
    b->dossiers = g_ptr_array_new_with_free_func(g_free);
    b->connues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entree_libre);
    b->annulation = g_cancellable_new();
    b->file = g_async_queue_new();

    /* ---- la barre ---- */
    GtkWidget *barre = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(barre, "video-bib-barre");

    GtkWidget *titre = gtk_label_new("Cinémathèque");
    gtk_widget_add_css_class(titre, "video-bib-titre");

    b->compte = gtk_label_new("");
    gtk_widget_add_css_class(b->compte, "video-bib-compte");
    gtk_widget_set_hexpand(b->compte, TRUE);
    gtk_label_set_xalign(GTK_LABEL(b->compte), 0.0);

    GtkWidget *ajout = gtk_button_new_with_label("Ajouter un dossier…");
    gtk_widget_add_css_class(ajout, "video-bib-bouton");
    g_signal_connect(ajout, "clicked", G_CALLBACK(act_ajouter_dossier), b);

    GtkWidget *fich = gtk_button_new_with_label("Ouvrir un fichier…");
    gtk_widget_add_css_class(fich, "video-bib-bouton");
    g_signal_connect(fich, "clicked", G_CALLBACK(act_ouvrir_fichier), b);

    gtk_box_append(GTK_BOX(barre), titre);
    gtk_box_append(GTK_BOX(barre), b->compte);
    gtk_box_append(GTK_BOX(barre), ajout);
    gtk_box_append(GTK_BOX(barre), fich);

    /* ---- les volets ---- */
    b->pile = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(b->pile),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_vexpand(b->pile, TRUE);

    GtkWidget *rec = grille_dans_defilement(&b->grille_rec, b, NULL);
    b->rec_vide = gtk_label_new("Aucune vidéo récente — ouvrez-en une, "
                                "elle apparaîtra ici");
    gtk_widget_add_css_class(b->rec_vide, "video-bib-vide");
    gtk_widget_set_vexpand(b->rec_vide, TRUE);
    gtk_box_prepend(GTK_BOX(rec), b->rec_vide);
    GtkWidget *bib = grille_dans_defilement(&b->grille_bib, b, NULL);
    gtk_stack_add_titled(GTK_STACK(b->pile), rec, "recents", "Récents");
    gtk_stack_add_titled(GTK_STACK(b->pile), bib, "biblio", "Bibliothèque");

    /* L'ACCUEIL DE LA TOUTE PREMIERE FOIS : un seul bouton, et rien d'autre
     * a comprendre. */
    GtkWidget *vide = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_halign(vide, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(vide, GTK_ALIGN_CENTER);
    GtkWidget *mot = gtk_label_new("Aucun dossier dans la bibliothèque");
    gtk_widget_add_css_class(mot, "video-bib-vide");
    GtkWidget *choisir = gtk_button_new_with_label("Sélectionner un dossier…");
    gtk_widget_add_css_class(choisir, "video-accueil");
    gtk_widget_set_halign(choisir, GTK_ALIGN_CENTER);
    g_signal_connect(choisir, "clicked", G_CALLBACK(act_ajouter_dossier), b);
    gtk_box_append(GTK_BOX(vide), mot);
    gtk_box_append(GTK_BOX(vide), choisir);
    gtk_stack_add_named(GTK_STACK(b->pile), vide, "accueil");

    b->bascule = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(b->bascule), GTK_STACK(b->pile));
    gtk_widget_set_halign(b->bascule, GTK_ALIGN_CENTER);

    b->racine = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(b->racine, "video-bib");
    gtk_box_append(GTK_BOX(b->racine), barre);
    gtk_box_append(GTK_BOX(b->racine), b->bascule);
    gtk_box_append(GTK_BOX(b->racine), b->pile);

    b->fil = g_thread_new("claude-os-video-vignettes", fil_vignettes, b);
    video_bib_rafraichir(b);
    return b;
}

GtkWidget *video_bib_widget(VideoBib *b) { return b ? b->racine : NULL; }

void video_bib_liberer(VideoBib *b)
{
    if (!b) return;
    b->quitte = TRUE;
    g_cancellable_cancel(b->annulation);
    if (b->file) g_async_queue_push(b->file, g_new0(Tache, 1));   /* reveille */
    if (b->fil) g_thread_join(b->fil);
    if (b->file) g_async_queue_unref(b->file);
    g_object_unref(b->annulation);
    g_hash_table_destroy(b->connues);
    g_ptr_array_unref(b->dossiers);
    g_key_file_free(b->cat);
    g_free(b);
}
