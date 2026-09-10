/* Claude OS -- sonde du chemin d'affichage sans copie.
 *
 * CE PROGRAMME DECIDE DE L'ARCHITECTURE DU LECTEUR VIDEO. Il est ecrit avant
 * lui, et volontairement jetable.
 *
 * La question qu'il tranche : une image decodee par le materiel peut-elle
 * arriver a l'ecran SANS jamais etre recopiee, sous labwc, avec GTK 4.18 ?
 * Le chemin espere est
 *
 *     VA-API  ->  dmabuf  ->  GdkDmabufTexture  ->  GtkGraphicsOffload
 *                                                    -> sous-surface Wayland
 *
 * Si ce chemin se prend, le decodage ne coute que le bloc materiel dedie,
 * l'image n'est jamais lue ni ecrite par le processeur, et le compositeur
 * peut la donner telle quelle au balayage. Si ce chemin ne se prend pas, il
 * faut le savoir MAINTENANT et changer d'architecture, plutot que d'ecrire un
 * lecteur entier autour d'une hypothese.
 *
 * QUATRE MODES, POUR QUE LA MESURE AIT UN TEMOIN. Un chiffre seul ne prouve
 * rien : c'est l'ecart entre ces quatre chemins qui dit ce que le zero-copie
 * rapporte, sur cette machine et pas sur une autre.
 *
 *   --mode=offload   VA-API -> dmabuf -> GtkGraphicsOffload   (le chemin vise)
 *   --mode=dmabuf    idem, mais sans GtkGraphicsOffload : GTK importe le
 *                    dmabuf et le compose lui-meme au GPU. Mesure ce que
 *                    coute la composition, isolement.
 *   --mode=copie     VA-API, puis av_hwframe_transfer_data : l'image
 *                    redescend en memoire centrale. C'est le chemin naif,
 *                    celui qu'on ecrit sans y penser.
 *   --mode=logiciel  aucun materiel, decodage sur les quatre coeurs. Le pire
 *                    cas, et la reference de ce qu'on veut eviter.
 *
 * Verifier que l'offload est REELLEMENT pris (GTK n'expose aucune API pour
 * le demander) :
 *
 *     GDK_DEBUG=offload ./sonde-offload mire.mp4 --mode=offload
 *
 * GDK ecrit alors une ligne par image sur stderr. Pas de ligne = pas
 * d'offload, quoi qu'affiche la fenetre.
 *
 * Construction :  bash shell/essais/construire.sh sonde-offload
 */

#include <gtk/gtk.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include <drm_fourcc.h>
#include <stdio.h>
#include <string.h>

typedef enum { M_OFFLOAD, M_DMABUF, M_COPIE, M_LOGICIEL } Mode;

static const char *NOM_MODE[] = { "offload", "dmabuf", "copie", "logiciel" };

typedef struct {
    Mode              mode;
    const char       *fichier;
    int               duree;        /* secondes avant de quitter, 0 = tout   */
    gboolean          muet;         /* sans fenetre : mesure du decodage seul */
    gboolean          plein;        /* plein ecran : le balayage direct est possible */
    /* LE TEMOIN. Une image, puis plus rien : meme fenetre, meme plein
     * ecran, meme occultation du reste du bureau -- mais aucun decodage et
     * aucune image nouvelle. C'est LUI le repere, et non le bureau au repos :
     * un plein ecran masque tout ce qui tournait derriere, et comparer une
     * lecture en plein ecran a un bureau visible mesure surtout ce qu'on a
     * cache. */
    gboolean          fige;

    AVFormatContext  *fmt;
    AVCodecContext   *dec;
    AVBufferRef      *materiel;
    int               piste;
    AVPacket         *paquet;
    AVFrame          *trame;        /* ce que rend le decodeur              */
    AVFrame          *recue;        /* image redescendue, mode copie        */
    struct SwsContext *conv;        /* YUV -> BGRA, modes copie et logiciel */

    GtkWidget        *fenetre;
    GtkWidget        *image;
    GtkWidget        *offload;

    gint64            depart;
    gint64            images;
    gint64            echecs;
    gint64            us_decodage;  /* temps passe dans le decodeur         */
    gint64            us_texture;   /* temps passe a fabriquer la texture   */
    gboolean          dit_premiere; /* le diagnostic n'est imprime qu'une fois */
    gboolean          fini;
} Sonde;

static void plainte(const char *quoi, int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (code < 0) av_strerror(code, buf, sizeof buf);
    fprintf(stderr, "sonde : %s%s%s\n", quoi, code < 0 ? " -- " : "",
            code < 0 ? buf : "");
}

/* ------------------------------------------------------------- ouverture */

/* Le decodeur reclame le format qu'il veut rendre. Repondre VAAPI est ce qui
 * garde l'image dans la memoire du GPU ; repondre autre chose la fait
 * redescendre, et tout l'interet disparait sans le moindre message. */
static enum AVPixelFormat choisir_format(AVCodecContext *ctx,
                                         const enum AVPixelFormat *formats)
{
    for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_VAAPI) return *p;

    fprintf(stderr, "sonde : le decodeur ne propose pas VAAPI -- "
                    "ce codec n'est pas accelere sur cette machine.\n");
    return formats[0];
}

static gboolean ouvrir(Sonde *s)
{
    int r = avformat_open_input(&s->fmt, s->fichier, NULL, NULL);
    if (r < 0) { plainte("avformat_open_input", r); return FALSE; }

    r = avformat_find_stream_info(s->fmt, NULL);
    if (r < 0) { plainte("avformat_find_stream_info", r); return FALSE; }

    const AVCodec *codec = NULL;
    s->piste = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (s->piste < 0) { plainte("aucune piste video", s->piste); return FALSE; }

    s->dec = avcodec_alloc_context3(codec);
    if (!s->dec) { plainte("avcodec_alloc_context3", 0); return FALSE; }

    r = avcodec_parameters_to_context(s->dec, s->fmt->streams[s->piste]->codecpar);
    if (r < 0) { plainte("avcodec_parameters_to_context", r); return FALSE; }

    if (s->mode != M_LOGICIEL) {
        r = av_hwdevice_ctx_create(&s->materiel, AV_HWDEVICE_TYPE_VAAPI,
                                   "/dev/dri/renderD128", NULL, 0);
        if (r < 0) {
            plainte("av_hwdevice_ctx_create (VAAPI)", r);
            return FALSE;
        }
        s->dec->hw_device_ctx = av_buffer_ref(s->materiel);
        s->dec->get_format    = choisir_format;
    } else {
        /* Le pire cas, mais honnete : un decodeur logiciel utilise tous les
         * coeurs, et c'est precisement ce qu'on veut mesurer. */
        s->dec->thread_count = 0;
    }

    r = avcodec_open2(s->dec, codec, NULL);
    if (r < 0) { plainte("avcodec_open2", r); return FALSE; }

    fprintf(stderr, "sonde : %s -- %s %dx%d, mode %s\n", s->fichier,
            codec->name, s->dec->width, s->dec->height, NOM_MODE[s->mode]);

    s->paquet = av_packet_alloc();
    s->trame  = av_frame_alloc();
    s->recue  = av_frame_alloc();
    return s->paquet && s->trame && s->recue;
}

/* --------------------------------------------------------------- decodage */

/* Rend TRUE tant qu'une image a ete decodee. Bloque le temps qu'il faut :
 * cette sonde ne cherche pas la fluidite, elle cherche un chemin. */
static gboolean image_suivante(Sonde *s)
{
    while (1) {
        int r = avcodec_receive_frame(s->dec, s->trame);
        if (r == 0) return TRUE;
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) {
            plainte("avcodec_receive_frame", r);
            return FALSE;
        }
        if (r == AVERROR_EOF) return FALSE;

        r = av_read_frame(s->fmt, s->paquet);
        if (r == AVERROR_EOF) {
            avcodec_send_packet(s->dec, NULL);   /* vidange */
            continue;
        }
        if (r < 0) { plainte("av_read_frame", r); return FALSE; }

        if (s->paquet->stream_index == s->piste) {
            r = avcodec_send_packet(s->dec, s->paquet);
            if (r < 0 && r != AVERROR(EAGAIN)) plainte("avcodec_send_packet", r);
        }
        av_packet_unref(s->paquet);
    }
}

/* --------------------------------------------------------------- textures */

/* La trame DRM_PRIME doit VIVRE aussi longtemps que la texture : c'est elle
 * qui tient les descripteurs de fichier du dmabuf. GTK les duplique a
 * l'import, mais rien ne garantit qu'il l'ait fait quand nous rendons la
 * main -- et un fd referme trop tot donne une image noire, pas une erreur. */
static void liberer_trame(gpointer p) { AVFrame *f = p; av_frame_free(&f); }

/* NV12 arrive de VA-API en DEUX COUCHES separees (R8 puis GR88), et non en
 * une seule couche NV12 : c'est ainsi que FFmpeg exporte les surfaces. GTK,
 * lui, veut UN fourcc et N plans. La traduction est ici, et elle est le seul
 * endroit ou ce programme peut echouer en silence si l'on n'y prend garde. */
static guint32 fourcc_compose(const AVDRMFrameDescriptor *d, int *n_plans)
{
    int total = 0;
    for (int i = 0; i < d->nb_layers; i++) total += d->layers[i].nb_planes;
    *n_plans = total;

    if (d->nb_layers == 1) return d->layers[0].format;

    if (d->nb_layers == 2 && total == 2) {
        guint32 a = d->layers[0].format, b = d->layers[1].format;
        if (a == DRM_FORMAT_R8  && b == DRM_FORMAT_GR88)  return DRM_FORMAT_NV12;
        if (a == DRM_FORMAT_R16 && b == DRM_FORMAT_GR1616) return DRM_FORMAT_P010;
    }
    return 0;
}

static GdkTexture *texture_dmabuf(Sonde *s, GdkDisplay *ecran)
{
    AVFrame *drm = av_frame_alloc();
    if (!drm) return NULL;
    drm->format = AV_PIX_FMT_DRM_PRIME;

    int r = av_hwframe_map(drm, s->trame, AV_HWFRAME_MAP_READ);
    if (r < 0) {
        plainte("av_hwframe_map vers DRM_PRIME", r);
        av_frame_free(&drm);
        return NULL;
    }

    const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)drm->data[0];
    int n_plans = 0;
    guint32 fourcc = fourcc_compose(d, &n_plans);
    if (fourcc == 0) {
        fprintf(stderr, "sonde : disposition dmabuf non traduite -- "
                "%d couche(s), %d objet(s)\n", d->nb_layers, d->nb_objects);
        av_frame_free(&drm);
        return NULL;
    }

    GdkDmabufTextureBuilder *b = gdk_dmabuf_texture_builder_new();
    gdk_dmabuf_texture_builder_set_display(b, ecran);
    gdk_dmabuf_texture_builder_set_width(b, s->trame->width);
    gdk_dmabuf_texture_builder_set_height(b, s->trame->height);
    gdk_dmabuf_texture_builder_set_fourcc(b, fourcc);
    gdk_dmabuf_texture_builder_set_modifier(b, d->objects[0].format_modifier);
    gdk_dmabuf_texture_builder_set_n_planes(b, n_plans);

    int p = 0;
    for (int i = 0; i < d->nb_layers; i++) {
        for (int j = 0; j < d->layers[i].nb_planes; j++, p++) {
            const AVDRMPlaneDescriptor *pl = &d->layers[i].planes[j];
            gdk_dmabuf_texture_builder_set_fd(b, p, d->objects[pl->object_index].fd);
            gdk_dmabuf_texture_builder_set_offset(b, p, pl->offset);
            gdk_dmabuf_texture_builder_set_stride(b, p, pl->pitch);
        }
    }

    if (!s->dit_premiere) {
        s->dit_premiere = TRUE;
        fprintf(stderr, "sonde : premiere image -- fourcc %.4s, modificateur "
                "0x%016llx, %d plan(s), %d objet(s)\n",
                (const char *)&fourcc,
                (unsigned long long)d->objects[0].format_modifier,
                n_plans, d->nb_objects);
    }

    GError *err = NULL;
    GdkTexture *t = gdk_dmabuf_texture_builder_build(b, liberer_trame, drm, &err);
    g_object_unref(b);

    if (!t) {
        /* CE MESSAGE EST LE RESULTAT DE LA SONDE quand elle echoue : il dit
         * exactement quel format GTK a refuse, ce qu'aucune supposition ne
         * pourrait donner. */
        fprintf(stderr, "sonde : GTK REFUSE ce dmabuf -- %s\n",
                err ? err->message : "sans motif");
        g_clear_error(&err);
        av_frame_free(&drm);
        return NULL;
    }
    return t;
}

/* Chemin naif : l'image redescend en memoire centrale, puis est convertie en
 * BGRA pour GTK. Deux traversees completes de l'image par le processeur. */
static GdkTexture *texture_memoire(Sonde *s)
{
    AVFrame *src = s->trame;

    if (s->mode == M_COPIE) {
        av_frame_unref(s->recue);
        int r = av_hwframe_transfer_data(s->recue, s->trame, 0);
        if (r < 0) { plainte("av_hwframe_transfer_data", r); return NULL; }
        src = s->recue;
    }

    s->conv = sws_getCachedContext(s->conv, src->width, src->height, src->format,
                                   src->width, src->height, AV_PIX_FMT_BGRA,
                                   SWS_BILINEAR, NULL, NULL, NULL);
    if (!s->conv) { plainte("sws_getCachedContext", 0); return NULL; }

    int taille = src->width * 4 * src->height;
    guchar *pix = g_malloc(taille);
    uint8_t *plans[4] = { pix, NULL, NULL, NULL };
    int      pas[4]   = { src->width * 4, 0, 0, 0 };

    sws_scale(s->conv, (const uint8_t * const *)src->data, src->linesize,
              0, src->height, plans, pas);

    GBytes *o = g_bytes_new_take(pix, taille);
    GdkTexture *t = gdk_memory_texture_new(src->width, src->height,
                                           GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                           o, src->width * 4);
    g_bytes_unref(o);
    return t;
}

/* ------------------------------------------------------------------ boucle */

static gboolean sur_battement(gpointer data)
{
    Sonde *s = data;

    if (s->fini) return G_SOURCE_REMOVE;

    if (s->duree > 0 &&
        g_get_monotonic_time() - s->depart > (gint64)s->duree * G_USEC_PER_SEC) {
        s->fini = TRUE;
        if (s->fenetre) gtk_window_close(GTK_WINDOW(s->fenetre));
        else g_main_context_wakeup(NULL);
        return G_SOURCE_REMOVE;
    }

    if (s->fige && s->images > 0) {
        /* Une seule image aura ete affichee ; on ne decode plus rien, mais
         * la fenetre reste, pleine et opaque. */
        return G_SOURCE_CONTINUE;
    }

    gint64 t0 = g_get_monotonic_time();
    if (!image_suivante(s)) {
        s->fini = TRUE;
        if (s->fenetre) gtk_window_close(GTK_WINDOW(s->fenetre));
        else g_main_context_wakeup(NULL);
        return G_SOURCE_REMOVE;
    }
    gint64 t1 = g_get_monotonic_time();
    s->us_decodage += t1 - t0;

    if (!s->muet) {
        GdkTexture *t = (s->mode == M_OFFLOAD || s->mode == M_DMABUF)
                      ? texture_dmabuf(s, gtk_widget_get_display(s->image))
                      : texture_memoire(s);
        s->us_texture += g_get_monotonic_time() - t1;

        if (t) {
            gtk_picture_set_paintable(GTK_PICTURE(s->image), GDK_PAINTABLE(t));
            g_object_unref(t);
        } else {
            s->echecs++;
        }
    }

    av_frame_unref(s->trame);
    s->images++;
    return G_SOURCE_CONTINUE;
}

static void sur_activation(GtkApplication *app, gpointer data)
{
    Sonde *s = data;

    s->image = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(s->image), GTK_CONTENT_FIT_CONTAIN);

    GtkWidget *racine = s->image;
    if (s->mode == M_OFFLOAD) {
        /* LE WIDGET QUI CHANGE TOUT. Il ne dessine rien lui-meme : il demande
         * a GDK de confier son contenu au compositeur, dans une sous-surface,
         * au lieu de le composer dans la fenetre. */
        s->offload = gtk_graphics_offload_new(s->image);
        gtk_graphics_offload_set_enabled(GTK_GRAPHICS_OFFLOAD(s->offload),
                                         GTK_GRAPHICS_OFFLOAD_ENABLED);
        racine = s->offload;
    }

    s->fenetre = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(s->fenetre), "Sonde -- chemin sans copie");
    gtk_window_set_default_size(GTK_WINDOW(s->fenetre), 1280, 720);
    gtk_window_set_child(GTK_WINDOW(s->fenetre), racine);
    /* EN PLEIN ECRAN, ET SEULEMENT LA, wlroots peut donner le tampon
     * directement au balayage : plus une seule composition. C'est le
     * meilleur cas theorique, et il se mesure. */
    if (s->plein) gtk_window_fullscreen(GTK_WINDOW(s->fenetre));
    gtk_window_present(GTK_WINDOW(s->fenetre));

    s->depart = g_get_monotonic_time();
    /* Cadence fixe : la sonde ne synchronise rien, elle compte. La vraie
     * horloge du lecteur sera l'audio, pas un minuteur. */
    g_timeout_add(1000 / 30, sur_battement, s);
}

int main(int argc, char **argv)
{
    Sonde s = { .mode = M_OFFLOAD, .duree = 0 };

    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--mode=")) {
            const char *m = argv[i] + 7;
            if      (!strcmp(m, "offload"))  s.mode = M_OFFLOAD;
            else if (!strcmp(m, "dmabuf"))   s.mode = M_DMABUF;
            else if (!strcmp(m, "copie"))    s.mode = M_COPIE;
            else if (!strcmp(m, "logiciel")) s.mode = M_LOGICIEL;
            else { fprintf(stderr, "sonde : mode inconnu « %s »\n", m); return 2; }
        } else if (g_str_has_prefix(argv[i], "--duree=")) {
            s.duree = atoi(argv[i] + 8);
        } else if (!strcmp(argv[i], "--muet")) {
            s.muet = TRUE;
        } else if (!strcmp(argv[i], "--plein-ecran")) {
            s.plein = TRUE;
        } else if (!strcmp(argv[i], "--fige")) {
            s.fige = TRUE;
        } else if (argv[i][0] != '-') {
            s.fichier = argv[i];
        } else {
            fprintf(stderr, "sonde : option inconnue « %s »\n", argv[i]);
            return 2;
        }
    }

    if (!s.fichier) {
        fprintf(stderr,
            "Usage : sonde-offload <fichier> [--mode=offload|dmabuf|copie|logiciel]\n"
            "                      [--duree=<secondes>] [--muet] [--plein-ecran]\n"
            "                      [--fige]  (temoin : une image, aucun decodage)\n");
        return 2;
    }

    if (!ouvrir(&s)) return 1;

    int code = 0;
    if (s.muet) {
        /* Sans fenetre : mesure du decodage seul, sans GTK ni compositeur. */
        s.depart = g_get_monotonic_time();
        while (sur_battement(&s) == G_SOURCE_CONTINUE) { }
    } else {
        GtkApplication *app = gtk_application_new("os.claude.shell.sonde",
                                                  G_APPLICATION_NON_UNIQUE);
        g_signal_connect(app, "activate", G_CALLBACK(sur_activation), &s);
        code = g_application_run(G_APPLICATION(app), 0, NULL);
        g_object_unref(app);
    }

    gint64 ecoule = g_get_monotonic_time() - s.depart;
    double sec = ecoule / 1e6;
    fprintf(stderr,
        "\n== Sonde, mode %s ==\n"
        "  images            : %ld en %.1f s  (%.1f img/s)\n"
        "  decodage          : %.2f ms/image\n"
        "  fabrique texture  : %.2f ms/image\n"
        "  textures refusees : %ld%s\n",
        NOM_MODE[s.mode], (long)s.images, sec,
        s.images ? s.images / sec : 0.0,
        s.images ? s.us_decodage / 1000.0 / s.images : 0.0,
        s.images ? s.us_texture / 1000.0 / s.images : 0.0,
        (long)s.echecs,
        s.echecs ? "   <- LE CHEMIN N'EST PAS PRIS" : "");

    if (s.conv) sws_freeContext(s.conv);
    av_frame_free(&s.trame);
    av_frame_free(&s.recue);
    av_packet_free(&s.paquet);
    avcodec_free_context(&s.dec);
    av_buffer_unref(&s.materiel);
    avformat_close_input(&s.fmt);
    return code;
}
