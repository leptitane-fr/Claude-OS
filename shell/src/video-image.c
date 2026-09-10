/* Claude OS -- de l'image decodee a la texture GTK. Voir video-image.h. */

#include "video-image.h"

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>

#include <drm_fourcc.h>

void video_image_init(VideoImage *vi)
{
    memset(vi, 0, sizeof *vi);
    vi->recue = av_frame_alloc();
}

void video_image_fin(VideoImage *vi)
{
    if (vi->conv) sws_freeContext(vi->conv);
    av_frame_free(&vi->recue);
    memset(vi, 0, sizeof *vi);
}

/* La trame DRM_PRIME doit VIVRE aussi longtemps que la texture : c'est elle
 * qui tient les descripteurs de fichier du dmabuf. Un fd referme trop tot ne
 * donne pas une erreur -- il donne une image noire, ce qui est bien pire. */
static void liberer_trame(gpointer p) { AVFrame *f = p; av_frame_free(&f); }

/* NV12 arrive de VA-API en DEUX COUCHES separees, R8 puis GR88 : c'est ainsi
 * que FFmpeg exporte les surfaces. GTK, lui, veut UN fourcc et N plans. Cette
 * traduction est obligatoire, et son absence est silencieuse. */
static guint32 fourcc_compose(const AVDRMFrameDescriptor *d, int *n_plans)
{
    int total = 0;
    for (int i = 0; i < d->nb_layers; i++) total += d->layers[i].nb_planes;
    *n_plans = total;

    if (d->nb_layers == 1) return d->layers[0].format;

    if (d->nb_layers == 2 && total == 2) {
        guint32 a = d->layers[0].format, b = d->layers[1].format;
        if (a == DRM_FORMAT_R8  && b == DRM_FORMAT_GR88)   return DRM_FORMAT_NV12;
        if (a == DRM_FORMAT_R16 && b == DRM_FORMAT_GR1616) return DRM_FORMAT_P010;
    }
    return 0;
}

static GdkTexture *sans_copie(VideoImage *vi, AVFrame *trame, GdkDisplay *ecran)
{
    AVFrame *drm = av_frame_alloc();
    if (!drm) return NULL;
    drm->format = AV_PIX_FMT_DRM_PRIME;

    int r = av_hwframe_map(drm, trame, AV_HWFRAME_MAP_READ);
    if (r < 0) { av_frame_free(&drm); return NULL; }

    const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)drm->data[0];
    int n_plans = 0;
    guint32 fourcc = fourcc_compose(d, &n_plans);
    if (fourcc == 0) {
        g_warning("video-image : disposition dmabuf non traduite -- "
                  "%d couche(s), %d objet(s)", d->nb_layers, d->nb_objects);
        av_frame_free(&drm);
        return NULL;
    }

    GdkDmabufTextureBuilder *b = gdk_dmabuf_texture_builder_new();
    gdk_dmabuf_texture_builder_set_display(b, ecran);
    gdk_dmabuf_texture_builder_set_width(b, trame->width);
    gdk_dmabuf_texture_builder_set_height(b, trame->height);
    gdk_dmabuf_texture_builder_set_fourcc(b, fourcc);
    gdk_dmabuf_texture_builder_set_modifier(b, d->objects[0].format_modifier);
    gdk_dmabuf_texture_builder_set_n_planes(b, n_plans);

    int p = 0;
    for (int i = 0; i < d->nb_layers; i++)
        for (int j = 0; j < d->layers[i].nb_planes; j++, p++) {
            const AVDRMPlaneDescriptor *pl = &d->layers[i].planes[j];
            gdk_dmabuf_texture_builder_set_fd(b, p, d->objects[pl->object_index].fd);
            gdk_dmabuf_texture_builder_set_offset(b, p, pl->offset);
            gdk_dmabuf_texture_builder_set_stride(b, p, pl->pitch);
        }

    if (!vi->dit_dmabuf) {
        vi->dit_dmabuf = TRUE;
        g_message("video-image : chemin sans copie -- fourcc %.4s, "
                  "modificateur 0x%016llx, %d plan(s)",
                  (const char *)&fourcc,
                  (unsigned long long)d->objects[0].format_modifier, n_plans);
    }

    GError *err = NULL;
    GdkTexture *t = gdk_dmabuf_texture_builder_build(b, liberer_trame, drm, &err);
    g_object_unref(b);

    if (!t) {
        /* Pas de silence : ce message dit exactement quel format GTK refuse,
         * et c'est la seule chose qui permette de comprendre pourquoi le
         * lecteur est soudain deux fois plus gourmand. */
        g_warning("video-image : GTK refuse ce dmabuf (%s) -- repli sur la "
                  "conversion logicielle, plus couteuse",
                  err ? err->message : "sans motif");
        g_clear_error(&err);
        av_frame_free(&drm);
        return NULL;
    }
    vi->sans_copie++;
    return t;
}

/* Le repli : l'image redescend en memoire, puis est convertie en BGRA. Deux
 * traversees completes par le processeur. On n'y vient que contraint. */
static GdkTexture *par_copie(VideoImage *vi, AVFrame *trame)
{
    AVFrame *src = trame;

    if (trame->hw_frames_ctx) {
        av_frame_unref(vi->recue);
        int r = av_hwframe_transfer_data(vi->recue, trame, 0);
        if (r < 0) {
            g_warning("video-image : av_hwframe_transfer_data a echoue (%d)", r);
            return NULL;
        }
        src = vi->recue;
    }

    if (!vi->dit_repli) {
        vi->dit_repli = TRUE;
        g_message("video-image : conversion logicielle (%s) -- "
                  "chemin plus couteux, voir docs/11",
                  av_get_pix_fmt_name(src->format));
    }

    vi->conv = sws_getCachedContext(vi->conv,
                                    src->width, src->height, src->format,
                                    src->width, src->height, AV_PIX_FMT_BGRA,
                                    SWS_BILINEAR, NULL, NULL, NULL);
    if (!vi->conv) {
        g_warning("video-image : sws_getCachedContext a echoue");
        return NULL;
    }

    int     pas    = src->width * 4;
    gsize   taille = (gsize)pas * src->height;
    guchar *pix    = g_malloc(taille);
    uint8_t *plans[4] = { pix, NULL, NULL, NULL };
    int      pass[4]  = { pas, 0, 0, 0 };

    sws_scale(vi->conv, (const uint8_t * const *)src->data, src->linesize,
              0, src->height, plans, pass);

    GBytes *o = g_bytes_new_take(pix, taille);
    GdkTexture *t = gdk_memory_texture_new(src->width, src->height,
                                           GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                           o, pas);
    g_bytes_unref(o);
    vi->recopiees++;
    return t;
}

GdkTexture *video_image_texture(VideoImage *vi, AVFrame *trame,
                                GdkDisplay *ecran)
{
    if (!trame) return NULL;

    if (trame->hw_frames_ctx) {
        GdkTexture *t = sans_copie(vi, trame, ecran);
        if (t) return t;
        /* Le repli est deja annonce par sans_copie(). */
    }
    return par_copie(vi, trame);
}
