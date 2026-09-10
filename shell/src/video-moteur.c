/* Claude OS -- moteur de lecture. Voir video-moteur.h pour l'architecture ;
 * ici, le detail et les pieges.
 */

#include "video-moteur.h"
#include "video-audio.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include <math.h>

/* TROIS IMAGES D'AVANCE, ET PAS TRENTE.
 *
 * Chaque image 1080p en NV12 occupe une surface VA-API de 3 Mo. Sur une
 * machine a 3,7 Gio soudes, une file profonde se paie en memoire pour un
 * benefice nul : le decodage materiel prend 0,83 ms par image, il n'a aucun
 * besoin d'avance. Trois images suffisent a absorber un hoquet du systeme de
 * fichiers, et le decodeur VA-API a lui-meme besoin de garder quelques
 * surfaces libres pour travailler. */
#define FILE_IMAGES 3

/* Le bloc audio decode, en images. Au-dela, on rend la main : le tampon de
 * sortie ne fait que 200 ms. */
#define BLOC_AUDIO 4096

struct _VideoMoteur {
    gchar            *chemin;
    VideoRappels      rappels;
    gpointer          usager;

    AVFormatContext  *fmt;
    AVBufferRef      *materiel;

    int               piste_v, piste_a;
    AVCodecContext   *dec_v, *dec_a;
    const char       *nom_codec;
    gboolean          en_materiel;
    int               largeur, hauteur;
    double            duree;

    SwrContext       *reech;
    float            *bloc;          /* audio converti, entrelace          */
    int               bloc_images;

    VideoAudio       *audio;
    int               taux_audio, canaux_audio;

    /* --- partage entre le fil principal et celui du decodage --- */
    GThread          *fil;
    GMutex            verrou;
    GCond             cond;          /* reveille le decodeur               */

    GQueue           *images;        /* AVFrame*, au plus FILE_IMAGES      */
    gboolean          quitte;
    gboolean          en_pause;
    gboolean          demux_fini;    /* plus rien a lire dans le fichier   */
    gboolean          fin_signalee;

    gboolean          saut_demande;
    double            saut_cible;

    /* LE RATTRAPAGE : ou l'on veut vraiment arriver.
     *
     * Un demux ne sait sauter que sur une image-cle. Avec un groupe d'images
     * d'une seconde -- ce que produit toute video du commerce -- viser 32,9 s
     * fait donc commencer la lecture a 32,0 s. Personne ne remarque cela sur
     * un bouton « -10 s » ; tout le monde le remarque en tirant une
     * glissiere, ou l'image affichee ne correspond plus a l'endroit tenu
     * sous le doigt.
     *
     * On decode donc du point d'ancrage jusqu'a la cible en jetant ce qui
     * precede. Ce que cela coute : au pire un groupe d'images, soit 30
     * images a 0,83 ms -- 25 ms, invisibles. */
    double            rattrapage;      /* -1 : rien a rattraper            */
    gboolean          rattrape_v, rattrape_a;

    double            pts_dernier;   /* derniere image montree             */
    double            horloge_secours;   /* quand il n'y a pas d'audio     */
    gint64            depart_secours;    /* monotonic, us                  */

    gint64            sautees, vues;
    double            ecart_somme, ecart_max;   /* synchronisation */
    VideoEtat         etat;
    double            volume;
    gboolean          muet;
};

/* ------------------------------------------------------------- utilitaires */

static void dire_erreur(const char *quoi, int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (code < 0) av_strerror(code, buf, sizeof buf);
    g_warning("video-moteur : %s%s%s", quoi, code < 0 ? " -- " : "",
              code < 0 ? buf : "");
}

/* Repondre VAAPI est ce qui garde l'image dans la memoire du GPU. Repondre
 * autre chose la fait redescendre et multiplie la consommation par huit,
 * sans le moindre message. Voir video-image.h. */
static enum AVPixelFormat choisir_format(AVCodecContext *ctx,
                                         const enum AVPixelFormat *formats)
{
    VideoMoteur *m = ctx->opaque;
    for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_VAAPI) return *p;

    /* Ce n'est PAS une erreur -- AV1 sur cette machine passe par la, et il
     * faut qu'il joue. Mais cela se dit, parce que cela se paie. */
    if (m) m->en_materiel = FALSE;
    g_message("video-moteur : %s n'est pas accelere sur cette machine, "
              "decodage logiciel", m ? m->nom_codec : "ce codec");
    return formats[0];
}

/* ---------------------------------------------------------------- horloge */

/* L'heure de reference. L'audio quand il y en a ; sinon une horloge
 * monotone que la pause arrete. Jamais un compteur d'images : il derive. */
static double horloge(VideoMoteur *m)
{
    double t;
    if (m->audio && video_audio_horloge(m->audio, &t))
        return t;

    if (m->en_pause || m->depart_secours == 0)
        return m->horloge_secours;

    return m->horloge_secours +
           (g_get_monotonic_time() - m->depart_secours) / 1e6;
}

static void horloge_arreter(VideoMoteur *m)
{
    if (m->depart_secours != 0) {
        m->horloge_secours +=
            (g_get_monotonic_time() - m->depart_secours) / 1e6;
        m->depart_secours = 0;
    }
}

static void horloge_partir(VideoMoteur *m)
{
    if (m->depart_secours == 0) m->depart_secours = g_get_monotonic_time();
}

static void horloge_poser(VideoMoteur *m, double t)
{
    m->horloge_secours = t;
    if (m->depart_secours != 0) m->depart_secours = g_get_monotonic_time();
}

/* ------------------------------------------------------------- le decodage */

static void vider_file(VideoMoteur *m)
{
    AVFrame *f;
    while ((f = g_queue_pop_head(m->images))) av_frame_free(&f);
}

/* Convertit et pousse l'audio vers la sortie. Rend FALSE si l'on doit
 * attendre que le tampon se vide. */
static gboolean pousser_audio(VideoMoteur *m, AVFrame *trame, double pts)
{
    int max = swr_get_out_samples(m->reech, trame->nb_samples);
    if (max <= 0) return TRUE;

    if (max > m->bloc_images) {
        m->bloc_images = max;
        m->bloc = g_realloc(m->bloc,
                            (size_t)max * m->canaux_audio * sizeof(float));
    }

    uint8_t *sortie[1] = { (uint8_t *)m->bloc };
    int n = swr_convert(m->reech, sortie, max,
                        (const uint8_t **)trame->extended_data,
                        trame->nb_samples);
    if (n <= 0) return TRUE;

    int ecrit = 0;
    while (ecrit < n) {
        int k = video_audio_ecrire(m->audio, m->bloc + (size_t)ecrit * m->canaux_audio,
                                   n - ecrit, pts + (double)ecrit / m->taux_audio);
        ecrit += k;
        if (ecrit < n) {
            /* LE TAMPON EST PLEIN, ET C'EST LA SITUATION NORMALE. On attend
             * qu'il se vide de moitie plutot que de repasser toutes les cinq
             * millisecondes : cent millisecondes d'attente, c'est dix
             * reveils par seconde au lieu de deux cents, pour exactement le
             * meme resultat. */
            g_mutex_lock(&m->verrou);
            if (!m->quitte && !m->saut_demande) {
                gint64 fin = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
                g_cond_wait_until(&m->cond, &m->verrou, fin);
            }
            gboolean stop = m->quitte || m->saut_demande;
            g_mutex_unlock(&m->verrou);
            if (stop) return FALSE;
        }
    }
    return TRUE;
}

static void executer_saut(VideoMoteur *m, double cible)
{
    int64_t ou = (int64_t)(cible * AV_TIME_BASE);
    int r = avformat_seek_file(m->fmt, -1, INT64_MIN, ou, ou, 0);
    if (r < 0) {
        dire_erreur("avformat_seek_file", r);
        return;
    }

    if (m->dec_v) avcodec_flush_buffers(m->dec_v);
    if (m->dec_a) avcodec_flush_buffers(m->dec_a);
    if (m->audio) video_audio_vider(m->audio);

    g_mutex_lock(&m->verrou);
    vider_file(m);
    m->demux_fini   = FALSE;
    m->fin_signalee = FALSE;
    m->pts_dernier  = cible;
    m->rattrapage   = cible;
    m->rattrape_v   = (m->dec_v != NULL);
    m->rattrape_a   = (m->dec_a != NULL && m->audio != NULL);
    horloge_poser(m, cible);
    g_mutex_unlock(&m->verrou);
}

static gpointer fil_decodage(gpointer data)
{
    VideoMoteur *m = data;
    AVPacket *paquet = av_packet_alloc();
    AVFrame  *trame  = av_frame_alloc();
    if (!paquet || !trame) return NULL;

    while (TRUE) {
        g_mutex_lock(&m->verrou);

        /* On dort tant qu'il n'y a rien a faire : file pleine, ou fin du
         * fichier atteinte. C'est ici que se realise le « zero reveil » --
         * pas de delai, pas de scrutation, une condition. */
        while (!m->quitte && !m->saut_demande &&
               (g_queue_get_length(m->images) >= FILE_IMAGES || m->demux_fini))
            g_cond_wait(&m->cond, &m->verrou);

        if (m->quitte) { g_mutex_unlock(&m->verrou); break; }

        gboolean saut = m->saut_demande;
        double   cible = m->saut_cible;
        m->saut_demande = FALSE;
        g_mutex_unlock(&m->verrou);

        if (saut) { executer_saut(m, cible); continue; }

        int r = av_read_frame(m->fmt, paquet);
        if (r == AVERROR_EOF) {
            /* Vidange des decodeurs : sans elle, les dernieres images
             * restent dedans et la lecture s'arrete avant la fin. */
            if (m->dec_v) avcodec_send_packet(m->dec_v, NULL);
            if (m->dec_a) avcodec_send_packet(m->dec_a, NULL);
        } else if (r < 0) {
            dire_erreur("av_read_frame", r);
            g_mutex_lock(&m->verrou); m->demux_fini = TRUE; g_mutex_unlock(&m->verrou);
            continue;
        }

        if (r != AVERROR_EOF) {
            if (paquet->stream_index == m->piste_v && m->dec_v) {
                int s = avcodec_send_packet(m->dec_v, paquet);
                if (s < 0 && s != AVERROR(EAGAIN))
                    dire_erreur("send_packet video", s);
            } else if (paquet->stream_index == m->piste_a && m->dec_a) {
                int s = avcodec_send_packet(m->dec_a, paquet);
                if (s < 0 && s != AVERROR(EAGAIN))
                    dire_erreur("send_packet audio", s);
            }
        }
        av_packet_unref(paquet);

        /* On depile les deux decodeurs a chaque tour : un paquet audio peut
         * liberer une image video restee dans la file interne. */
        for (int passe = 0; passe < 2; passe++) {
            AVCodecContext *d = passe == 0 ? m->dec_v : m->dec_a;
            if (!d) continue;
            gboolean video = (passe == 0);
            AVRational tb = m->fmt->streams[video ? m->piste_v : m->piste_a]->time_base;

            while (TRUE) {
                int q = avcodec_receive_frame(d, trame);
                if (q == AVERROR(EAGAIN)) break;
                if (q == AVERROR_EOF) {
                    if (r == AVERROR_EOF) {
                        g_mutex_lock(&m->verrou);
                        m->demux_fini = TRUE;
                        g_mutex_unlock(&m->verrou);
                    }
                    break;
                }
                if (q < 0) { dire_erreur("receive_frame", q); break; }

                int64_t pts_brut = trame->best_effort_timestamp != AV_NOPTS_VALUE
                                 ? trame->best_effort_timestamp : trame->pts;
                double pts = pts_brut == AV_NOPTS_VALUE
                           ? 0.0 : pts_brut * av_q2d(tb);

                /* Pendant le rattrapage, tout ce qui precede la cible est
                 * jete : ni affiche, ni entendu. */
                g_mutex_lock(&m->verrou);
                gboolean jeter = FALSE;
                if (m->rattrapage >= 0) {
                    if (pts < m->rattrapage - 0.001) {
                        jeter = TRUE;
                    } else {
                        if (video) m->rattrape_v = FALSE;
                        else       m->rattrape_a = FALSE;
                        if (!m->rattrape_v && !m->rattrape_a)
                            m->rattrapage = -1.0;
    m->volume  = 1.0;
                    }
                }
                g_mutex_unlock(&m->verrou);

                if (jeter) { av_frame_unref(trame); continue; }

                if (video) {
                    AVFrame *copie = av_frame_alloc();
                    av_frame_move_ref(copie, trame);
                    copie->pts = (int64_t)(pts * AV_TIME_BASE);

                    g_mutex_lock(&m->verrou);
                    g_queue_push_tail(m->images, copie);
                    g_mutex_unlock(&m->verrou);
                } else {
                    pousser_audio(m, trame, pts);
                    av_frame_unref(trame);
                }
            }
        }

        if (r == AVERROR_EOF) {
            g_mutex_lock(&m->verrou);
            m->demux_fini = TRUE;
            g_mutex_unlock(&m->verrou);
        }
    }

    av_frame_free(&trame);
    av_packet_free(&paquet);
    return NULL;
}

/* ------------------------------------------------------------- ouverture */

static gboolean ouvrir_piste(VideoMoteur *m, int piste, gboolean video,
                             GError **erreur)
{
    const AVCodec *codec = avcodec_find_decoder(
        m->fmt->streams[piste]->codecpar->codec_id);
    if (!codec) {
        g_set_error(erreur, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "aucun décodeur pour cette piste");
        return FALSE;
    }

    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec) {
        g_set_error(erreur, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "avcodec_alloc_context3 a échoué");
        return FALSE;
    }

    int r = avcodec_parameters_to_context(dec, m->fmt->streams[piste]->codecpar);
    if (r < 0) {
        avcodec_free_context(&dec);
        g_set_error(erreur, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "avcodec_parameters_to_context a échoué");
        return FALSE;
    }

    if (video) {
        m->nom_codec = codec->name;
        dec->opaque  = m;
        if (m->materiel) {
            dec->hw_device_ctx = av_buffer_ref(m->materiel);
            dec->get_format    = choisir_format;
            m->en_materiel     = TRUE;
        }
    } else {
        dec->thread_count = 1;    /* l'audio ne merite pas quatre coeurs */
    }

    r = avcodec_open2(dec, codec, NULL);
    if (r < 0) {
        avcodec_free_context(&dec);
        g_set_error(erreur, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "avcodec_open2 a échoué pour %s", codec->name);
        return FALSE;
    }

    if (video) { m->dec_v = dec; m->largeur = dec->width; m->hauteur = dec->height; }
    else       { m->dec_a = dec; }
    return TRUE;
}

VideoMoteur *video_moteur_ouvrir(const char *chemin,
                                 const VideoRappels *rappels, gpointer u,
                                 GError **erreur)
{
    VideoMoteur *m = g_new0(VideoMoteur, 1);
    m->chemin  = g_strdup(chemin);
    m->usager  = u;
    m->piste_v = m->piste_a = -1;
    m->images  = g_queue_new();
    m->rattrapage = -1.0;
    m->volume  = 1.0;
    m->etat    = VIDEO_ARRETE;
    if (rappels) m->rappels = *rappels;
    g_mutex_init(&m->verrou);
    g_cond_init(&m->cond);

    int r = avformat_open_input(&m->fmt, chemin, NULL, NULL);
    if (r < 0) {
        g_set_error(erreur, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "impossible d'ouvrir « %s »", chemin);
        video_moteur_fermer(m);
        return NULL;
    }
    r = avformat_find_stream_info(m->fmt, NULL);
    if (r < 0) {
        g_set_error(erreur, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "aucune information de flux dans « %s »", chemin);
        video_moteur_fermer(m);
        return NULL;
    }

    if (m->fmt->duration != AV_NOPTS_VALUE)
        m->duree = (double)m->fmt->duration / AV_TIME_BASE;

    m->piste_v = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    m->piste_a = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    if (m->piste_v < 0 && m->piste_a < 0) {
        g_set_error(erreur, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "ce fichier ne contient ni vidéo ni audio");
        video_moteur_fermer(m);
        return NULL;
    }

    if (m->piste_v >= 0) {
        /* Le materiel est une OPTION, pas une exigence : sans lui le lecteur
         * doit encore jouer. Son absence se dit, elle n'arrete rien. */
        int h = av_hwdevice_ctx_create(&m->materiel, AV_HWDEVICE_TYPE_VAAPI,
                                       "/dev/dri/renderD128", NULL, 0);
        if (h < 0) {
            g_message("video-moteur : VA-API indisponible (%d) -- "
                      "décodage logiciel, plus coûteux", h);
            m->materiel = NULL;
        }
        if (!ouvrir_piste(m, m->piste_v, TRUE, erreur)) {
            video_moteur_fermer(m);
            return NULL;
        }
    }

    if (m->piste_a >= 0) {
        GError *e = NULL;
        if (!ouvrir_piste(m, m->piste_a, FALSE, &e)) {
            /* Une piste audio illisible ne doit pas empecher de voir le
             * film. On le dit, et on continue sans son. */
            g_message("video-moteur : piste audio inutilisable (%s) -- "
                      "lecture sans son", e ? e->message : "?");
            g_clear_error(&e);
            m->piste_a = -1;
        }
    }

    if (m->dec_a) {
        m->taux_audio   = m->dec_a->sample_rate;
        m->canaux_audio = m->dec_a->ch_layout.nb_channels;
        if (m->canaux_audio > 2) m->canaux_audio = 2;   /* la machine est stereo */

        AVChannelLayout sortie;
        av_channel_layout_default(&sortie, m->canaux_audio);
        r = swr_alloc_set_opts2(&m->reech, &sortie, AV_SAMPLE_FMT_FLT,
                                m->taux_audio, &m->dec_a->ch_layout,
                                m->dec_a->sample_fmt, m->dec_a->sample_rate,
                                0, NULL);
        av_channel_layout_uninit(&sortie);
        if (r < 0 || swr_init(m->reech) < 0) {
            g_message("video-moteur : rééchantillonneur indisponible -- "
                      "lecture sans son");
            m->piste_a = -1;
            avcodec_free_context(&m->dec_a);
        } else {
            GError *e = NULL;
            m->audio = video_audio_ouvrir(m->taux_audio, m->canaux_audio, &e);
            if (!m->audio) {
                g_message("video-moteur : sortie audio indisponible (%s) -- "
                          "lecture sans son", e ? e->message : "?");
                g_clear_error(&e);
            }
        }
    }

    m->fil = g_thread_new("claude-os-video-decodage", fil_decodage, m);
    return m;
}

void video_moteur_fermer(VideoMoteur *m)
{
    if (!m) return;

    if (m->fil) {
        g_mutex_lock(&m->verrou);
        m->quitte = TRUE;
        g_cond_broadcast(&m->cond);
        g_mutex_unlock(&m->verrou);
        g_thread_join(m->fil);
    }

    if (m->audio) video_audio_fermer(m->audio);
    if (m->reech) swr_free(&m->reech);
    g_free(m->bloc);

    if (m->images) { vider_file(m); g_queue_free(m->images); }

    avcodec_free_context(&m->dec_v);
    avcodec_free_context(&m->dec_a);
    av_buffer_unref(&m->materiel);
    if (m->fmt) avformat_close_input(&m->fmt);

    g_mutex_clear(&m->verrou);
    g_cond_clear(&m->cond);
    g_free(m->chemin);
    g_free(m);
}

/* --------------------------------------------------------------- commandes */

static void poser_etat(VideoMoteur *m, VideoEtat e)
{
    if (m->etat == e) return;
    m->etat = e;
    if (m->rappels.etat_change) m->rappels.etat_change(e, m->usager);
}

void video_moteur_lire(VideoMoteur *m)
{
    if (!m || m->etat == VIDEO_LIT) return;

    if (m->etat == VIDEO_FINI) video_moteur_sauter(m, 0.0);

    g_mutex_lock(&m->verrou);
    m->en_pause = FALSE;
    horloge_partir(m);
    g_cond_broadcast(&m->cond);
    g_mutex_unlock(&m->verrou);

    if (m->audio) video_audio_pause(m->audio, FALSE);
    poser_etat(m, VIDEO_LIT);
}

void video_moteur_pause(VideoMoteur *m)
{
    if (!m || m->etat != VIDEO_LIT) return;

    g_mutex_lock(&m->verrou);
    m->en_pause = TRUE;
    horloge_arreter(m);
    g_mutex_unlock(&m->verrou);

    if (m->audio) video_audio_pause(m->audio, TRUE);
    poser_etat(m, VIDEO_EN_PAUSE);
}

void video_moteur_basculer(VideoMoteur *m)
{
    if (!m) return;
    if (m->etat == VIDEO_LIT) video_moteur_pause(m);
    else                      video_moteur_lire(m);
}

VideoEtat video_moteur_etat(VideoMoteur *m)  { return m ? m->etat : VIDEO_ARRETE; }
double    video_moteur_duree(VideoMoteur *m) { return m ? m->duree : 0.0; }
gboolean  video_moteur_a_audio(VideoMoteur *m) { return m && m->audio; }

void video_moteur_volume(VideoMoteur *m, double v)
{
    if (!m) return;
    m->volume = CLAMP(v, 0.0, 1.0);
    if (m->audio) video_audio_gain(m->audio, m->muet ? 0.0 : m->volume);
}
double video_moteur_volume_actuel(VideoMoteur *m) { return m ? m->volume : 0.0; }
gboolean video_moteur_est_muet(VideoMoteur *m) { return m && m->muet; }

void video_moteur_sourdine(VideoMoteur *m, gboolean muet)
{
    if (!m) return;
    m->muet = muet;
    if (m->audio) video_audio_gain(m->audio, muet ? 0.0 : m->volume);
}
gboolean  video_moteur_a_video(VideoMoteur *m) { return m && m->dec_v; }
int       video_moteur_largeur(VideoMoteur *m) { return m ? m->largeur : 0; }
int       video_moteur_hauteur(VideoMoteur *m) { return m ? m->hauteur : 0; }
gint64    video_moteur_images_sautees(VideoMoteur *m) { return m ? m->sautees : 0; }
gint64    video_moteur_images_vues(VideoMoteur *m)    { return m ? m->vues : 0; }
double    video_moteur_ecart_max(VideoMoteur *m) { return m ? m->ecart_max : 0.0; }
double    video_moteur_ecart_moyen(VideoMoteur *m)
{
    return (m && m->vues) ? m->ecart_somme / m->vues : 0.0;
}
gboolean  video_moteur_materiel(VideoMoteur *m) { return m && m->en_materiel; }
const char *video_moteur_codec(VideoMoteur *m)
{
    return m && m->nom_codec ? m->nom_codec : "";
}

double video_moteur_position(VideoMoteur *m)
{
    if (!m) return 0.0;
    /* En pause, l'horloge de secours ne bouge plus et l'audio ne rend rien :
     * c'est la derniere image montree qui dit la position. Sans cela, la
     * glissiere reculerait d'une demi-seconde a chaque pause. */
    if (m->etat != VIDEO_LIT) return m->pts_dernier;
    double t = horloge(m);
    return m->duree > 0 ? CLAMP(t, 0.0, m->duree) : MAX(t, 0.0);
}

void video_moteur_sauter(VideoMoteur *m, double secondes)
{
    if (!m) return;
    if (m->duree > 0) secondes = CLAMP(secondes, 0.0, m->duree);
    if (secondes < 0) secondes = 0;

    g_mutex_lock(&m->verrou);
    m->saut_demande = TRUE;
    m->saut_cible   = secondes;
    m->pts_dernier  = secondes;
    g_cond_broadcast(&m->cond);
    g_mutex_unlock(&m->verrou);

    if (m->etat == VIDEO_FINI) poser_etat(m, VIDEO_EN_PAUSE);
}

void video_moteur_avancer(VideoMoteur *m, double delta)
{
    if (m) video_moteur_sauter(m, video_moteur_position(m) + delta);
}

/* ------------------------------------------------- l'image due maintenant */

AVFrame *video_moteur_image_due(VideoMoteur *m)
{
    if (!m || !m->dec_v) return NULL;

    double maintenant = horloge(m);
    AVFrame *choisie = NULL;

    g_mutex_lock(&m->verrou);

    while (TRUE) {
        AVFrame *tete = g_queue_peek_head(m->images);
        if (!tete) break;

        double pts = (double)tete->pts / AV_TIME_BASE;

        /* La toute premiere image s'affiche sans attendre : au demarrage et
         * apres un saut, l'horloge audio n'existe pas encore et attendre
         * laisserait la fenetre noire une demi-seconde. */
        gboolean due = (pts <= maintenant + 0.001) || (m->vues == 0);
        if (!due) break;

        if (choisie) {
            /* Une image plus recente est deja due : la precedente est en
             * retard, on la saute. C'est ce compteur qu'on regarde quand la
             * lecture parait heurtee. */
            av_frame_free(&choisie);
            m->sautees++;
        }
        choisie = g_queue_pop_head(m->images);
        m->pts_dernier = pts;
    }

    if (choisie) {
        /* L'ecart est mesure AU MOMENT DE L'AFFICHAGE, sur l'image
         * reellement montree -- pas sur celle qu'on aurait voulu montrer.
         * C'est la seule facon d'attraper une derive lente. */
        double ecart = fabs((double)choisie->pts / AV_TIME_BASE - maintenant);
        m->ecart_somme += ecart;
        if (ecart > m->ecart_max) m->ecart_max = ecart;
        m->vues++;
        g_cond_broadcast(&m->cond);   /* de la place : le decodeur repart */
    }

    gboolean fini = m->demux_fini && g_queue_is_empty(m->images) &&
                    !m->fin_signalee;
    if (fini) m->fin_signalee = TRUE;

    g_mutex_unlock(&m->verrou);

    if (fini && (!m->audio || video_audio_epuise(m->audio))) {
        horloge_arreter(m);
        poser_etat(m, VIDEO_FINI);
        if (m->rappels.fin_atteinte) m->rappels.fin_atteinte(m->usager);
    } else if (fini) {
        /* Le son n'a pas fini de sortir : on redemandera. */
        g_mutex_lock(&m->verrou);
        m->fin_signalee = FALSE;
        g_mutex_unlock(&m->verrou);
    }

    return choisie;
}
