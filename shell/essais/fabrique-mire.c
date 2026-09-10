/* Claude OS -- fabrique de mires video, pour le banc d'essai du lecteur.
 *
 * POURQUOI FABRIQUER PLUTOT QUE TELECHARGER.
 *
 * Un record d'economie d'energie se compare, et une comparaison exige que les
 * deux mesures portent sur le MEME contenu. Une video prise au hasard change
 * de debit d'une scene a l'autre : le decodeur travaille plus dans l'action
 * que sur un plan fixe, et deux mesures de trente secondes ne tombent jamais
 * sur les memes trente secondes. La mire, elle, est identique a la seconde
 * pres, reproductible d'une machine a l'autre, et ne pese rien dans le depot
 * puisqu'elle se refabrique.
 *
 * Elle porte en plus deux reperes que ni un film ni une bande d'essai du
 * commerce ne donnent :
 *
 *   -- UN ECLAIR BLANC au debut de chaque seconde, et un BIP de 1 kHz au meme
 *      instant. La synchronisation audio-video se juge alors a l'oeil et a
 *      l'oreille, sans instrument : si l'eclair et le bip se separent, la
 *      derive se voit.
 *   -- UN COMPTEUR D'IMAGES en binaire, seize carrees en haut de l'image.
 *      Une image perdue ou repetee se lit directement a l'ecran, et une
 *      capture suffit a dire OU la lecture a saute.
 *
 * La trame est volontairement DETAILLEE et MOUVANTE : des diagonales qui
 * defilent, que le codeur ne peut pas resumer en un vecteur de mouvement
 * gratuit. Une mire trop simple se decode pour rien et flatterait nos
 * mesures.
 *
 * SOUS-TITRES : une ligne par seconde, quand la sortie est un .mkv.
 * Matroska accepte le SubRip tel quel -- le texte est ecrit dans le paquet,
 * sans codeur -- et cela donne de quoi verifier que les sous-titres
 * apparaissent AU BON MOMENT, ce qu'un fichier de film ne permet pas de
 * juger sans le connaitre par coeur.
 *
 * Construction :  bash shell/essais/construire.sh
 * Usage        :  ./fabrique-mire mire-h264.mp4 [secondes] [codec]
 *                 codec : h264 (defaut) | hevc | vp9
 *                 .mkv en sortie => une piste de sous-titres en plus
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LARGEUR    1920
#define HAUTEUR    1080
#define CADENCE      30
#define ECHANT    48000   /* audio, Hz */
#define BLOC       1024   /* echantillons par trame audio */

/* Ce programme n'utilise pas GLib -- il n'a pas d'interface, et une
 * dependance de moins est une dependance de moins. Deux ou trois noms
 * familiers suffisent. */
typedef int gboolean;
#define TRUE 1
#define FALSE 0
#define gchar char
#define g_snprintf snprintf

static gboolean g_str_has_suffix_simple(const char *s, const char *fin)
{
    size_t ls = strlen(s), lf = strlen(fin);
    return ls >= lf && strcmp(s + ls - lf, fin) == 0;
}

/* INVARIANT N.4 DU PROJET : rien ne s'eteint en silence. Toute erreur
 * libav est rendue lisible et arrete le programme. */
static void fatal(const char *quoi, int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (code < 0)
        av_strerror(code, buf, sizeof buf);
    fprintf(stderr, "fabrique-mire : %s%s%s\n", quoi,
            code < 0 ? " -- " : "", code < 0 ? buf : "");
    exit(1);
}

/* ------------------------------------------------------------------ image */

/* Le compteur binaire : seize carres de 64 px, bit de poids fort a gauche.
 * Blanc = 1. Il occupe la bande du haut, hors de la zone ou l'on regarde. */
static void compteur(uint8_t *Y, int stride, int64_t n)
{
    for (int bit = 0; bit < 16; bit++) {
        int allume = (n >> (15 - bit)) & 1;
        int x0 = 32 + bit * 72, y0 = 32;
        for (int y = y0; y < y0 + 64; y++)
            memset(Y + y * stride + x0, allume ? 235 : 16, 64);
    }
}

/* La meme mire, sur seize bits par composante quand il le faut. Le motif est
 * identique : c'est ce qui permet de comparer une lecture 8 bits et une
 * lecture 10 bits sans changer autre chose que la profondeur. */
static void image16(AVFrame *f, int64_t n)
{
    for (int y = 0; y < HAUTEUR; y++) {
        uint16_t *l = (uint16_t *)(f->data[0] + y * f->linesize[0]);
        for (int x = 0; x < LARGEUR; x++) {
            int v = ((x * 2 + y + (int)(n * 7)) >> 2) & 0xff;
            l[x] = (uint16_t)((64 + (v * 876) / 255));   /* plage 64-940 */
        }
    }
    for (int y = 0; y < HAUTEUR / 2; y++) {
        uint16_t *u = (uint16_t *)(f->data[1] + y * f->linesize[1]);
        uint16_t *v = (uint16_t *)(f->data[2] + y * f->linesize[2]);
        for (int x = 0; x < LARGEUR / 2; x++) {
            u[x] = (uint16_t)(512 + (int)(240 * sin((x + n * 3) * 0.01)));
            v[x] = (uint16_t)(512 + (int)(240 * cos((y - n * 2) * 0.013)));
        }
    }
    if (n % CADENCE == 0)
        for (int y = 140; y < 380; y++) {
            uint16_t *l = (uint16_t *)(f->data[0] + y * f->linesize[0]);
            for (int x = 32; x < 272; x++) l[x] = 940;
        }
    for (int bit = 0; bit < 16; bit++) {
        int allume = (n >> (15 - bit)) & 1;
        int x0 = 32 + bit * 72;
        for (int y = 32; y < 96; y++) {
            uint16_t *l = (uint16_t *)(f->data[0] + y * f->linesize[0]);
            for (int x = x0; x < x0 + 64; x++) l[x] = allume ? 940 : 64;
        }
    }
}

static void image(AVFrame *f, int64_t n)
{
    if (f->format == AV_PIX_FMT_YUV420P10LE) { image16(f, n); return; }

    /* Diagonales mouvantes : du detail reel a coder, a chaque image
     * different, sans etre du bruit -- le bruit ferait exploser le debit et
     * ne ressemblerait a aucune video reelle. */
    for (int y = 0; y < HAUTEUR; y++) {
        uint8_t *l = f->data[0] + y * f->linesize[0];
        for (int x = 0; x < LARGEUR; x++) {
            int v = ((x * 2 + y + (int)(n * 7)) >> 2) & 0xff;
            l[x] = 16 + (v * 219) / 255;   /* plage televisuelle 16-235 */
        }
    }

    /* Deux bandes de couleur qui glissent en sens inverse : elles font
     * travailler les plans de chrominance, souvent oublies des mires. */
    for (int y = 0; y < HAUTEUR / 2; y++) {
        uint8_t *u = f->data[1] + y * f->linesize[1];
        uint8_t *v = f->data[2] + y * f->linesize[2];
        for (int x = 0; x < LARGEUR / 2; x++) {
            u[x] = 128 + (int)(60 * sin((x + n * 3) * 0.01));
            v[x] = 128 + (int)(60 * cos((y - n * 2) * 0.013));
        }
    }

    /* L'ECLAIR : un carre blanc de 240 px au debut de chaque seconde, au
     * meme instant que le bip. C'est le repere de synchronisation. */
    if (n % CADENCE == 0) {
        for (int y = 140; y < 380; y++)
            memset(f->data[0] + y * f->linesize[0] + 32, 235, 240);
    }

    compteur(f->data[0], f->linesize[0], n);
}

/* ------------------------------------------------------------------ audio */

/* Un bip de 1 kHz sur les 100 premieres millisecondes de chaque seconde,
 * silence le reste du temps. Le silence n'est pas gratuit : il verifie que
 * le lecteur ne reveille pas la carte son entre deux bips. */
static void son(AVFrame *f, int64_t premier)
{
    float *g = (float *)f->data[0];
    float *d = (float *)f->data[1];
    for (int i = 0; i < f->nb_samples; i++) {
        int64_t s = premier + i;
        int64_t dans_seconde = s % ECHANT;
        float a = 0.0f;
        if (dans_seconde < ECHANT / 10) {
            /* Une enveloppe, sinon le bip claque et le claquement porte
             * plus loin que le bip lui-meme. */
            float env = sinf((float)M_PI * dans_seconde / (ECHANT / 10.0f));
            a = 0.25f * env * sinf(2.0f * (float)M_PI * 1000.0f * s / ECHANT);
        }
        g[i] = a;
        d[i] = a;
    }
}

/* ------------------------------------------------------------------- flux */

typedef struct {
    AVStream       *st;
    AVCodecContext *ctx;
    AVFrame        *trame;
    AVPacket       *paquet;
    int64_t         pts;
} Flux;

static void ecrire(AVFormatContext *fmt, Flux *fl, AVFrame *trame)
{
    int r = avcodec_send_frame(fl->ctx, trame);
    if (r < 0) fatal("avcodec_send_frame", r);

    while (1) {
        r = avcodec_receive_packet(fl->ctx, fl->paquet);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
        if (r < 0) fatal("avcodec_receive_packet", r);

        av_packet_rescale_ts(fl->paquet, fl->ctx->time_base, fl->st->time_base);
        fl->paquet->stream_index = fl->st->index;
        r = av_interleaved_write_frame(fmt, fl->paquet);
        if (r < 0) fatal("av_interleaved_write_frame", r);
    }
}

int main(int argc, char **argv)
{
    const char *sortie   = argc > 1 ? argv[1] : "mire-h264.mp4";
    int         secondes = argc > 2 ? atoi(argv[2]) : 60;
    const char *codec    = argc > 3 ? argv[3] : "h264";

    if (secondes < 1) fatal("duree invalide", 0);

    const char *enc_nom = strcmp(codec, "hevc") == 0 ? "libx265"
                        : strcmp(codec, "hevc10") == 0 ? "libx265"
                        : strcmp(codec, "vp9")  == 0 ? "libvpx-vp9"
                        : "libx264";

    /* DIX BITS, ET CE N'EST PAS UN CAPRICE : c'est le seul cas qui produit
     * du P010 au lieu du NV12, donc le seul qui eprouve la traduction de
     * disposition dmabuf du lecteur pour ce format. Un chemin jamais
     * execute est un chemin dont on ne sait rien. */
    gboolean dix_bits = strcmp(codec, "hevc10") == 0;

    AVFormatContext *fmt = NULL;
    int r = avformat_alloc_output_context2(&fmt, NULL, NULL, sortie);
    if (r < 0 || !fmt) fatal("avformat_alloc_output_context2", r);

    /* ---- video ---- */
    const AVCodec *cv = avcodec_find_encoder_by_name(enc_nom);
    if (!cv) {
        fprintf(stderr, "fabrique-mire : codeur « %s » absent de cette "
                "installation de FFmpeg.\n", enc_nom);
        exit(1);
    }
    Flux v = {0};
    v.st  = avformat_new_stream(fmt, NULL);
    v.ctx = avcodec_alloc_context3(cv);
    if (!v.st || !v.ctx) fatal("allocation du flux video", 0);

    v.ctx->width      = LARGEUR;
    v.ctx->height     = HAUTEUR;
    v.ctx->pix_fmt    = dix_bits ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    v.ctx->time_base  = (AVRational){1, CADENCE};
    v.ctx->framerate  = (AVRational){CADENCE, 1};
    /* Un groupe d'images d'une seconde : c'est ce que fait une video du
     * commerce, et cela donne des points de recherche tous les 33 images --
     * de quoi eprouver la navigation sans la rendre irrealistement facile. */
    v.ctx->gop_size   = CADENCE;
    v.ctx->max_b_frames = 2;
    v.ctx->bit_rate   = 6000000;
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        v.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(v.ctx->priv_data, "preset", "veryfast", 0);

    r = avcodec_open2(v.ctx, cv, NULL);
    if (r < 0) fatal("avcodec_open2 (video)", r);
    avcodec_parameters_from_context(v.st->codecpar, v.ctx);
    v.st->time_base = v.ctx->time_base;

    v.trame = av_frame_alloc();
    v.trame->format = v.ctx->pix_fmt;
    v.trame->width  = LARGEUR;
    v.trame->height = HAUTEUR;
    r = av_frame_get_buffer(v.trame, 0);
    if (r < 0) fatal("av_frame_get_buffer (video)", r);
    v.paquet = av_packet_alloc();

    /* ---- audio ---- */
    const AVCodec *ca = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!ca) fatal("codeur AAC absent", 0);
    Flux a = {0};
    a.st  = avformat_new_stream(fmt, NULL);
    a.ctx = avcodec_alloc_context3(ca);
    if (!a.st || !a.ctx) fatal("allocation du flux audio", 0);

    a.ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    a.ctx->sample_rate = ECHANT;
    av_channel_layout_default(&a.ctx->ch_layout, 2);
    a.ctx->bit_rate    = 128000;
    a.ctx->time_base   = (AVRational){1, ECHANT};
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        a.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    r = avcodec_open2(a.ctx, ca, NULL);
    if (r < 0) fatal("avcodec_open2 (audio)", r);
    avcodec_parameters_from_context(a.st->codecpar, a.ctx);
    a.st->time_base = a.ctx->time_base;

    a.trame = av_frame_alloc();
    a.trame->format      = a.ctx->sample_fmt;
    a.trame->sample_rate = ECHANT;
    a.trame->nb_samples  = a.ctx->frame_size > 0 ? a.ctx->frame_size : BLOC;
    av_channel_layout_copy(&a.trame->ch_layout, &a.ctx->ch_layout);
    r = av_frame_get_buffer(a.trame, 0);
    if (r < 0) fatal("av_frame_get_buffer (audio)", r);
    a.paquet = av_packet_alloc();

    /* ---- sous-titres, si le conteneur les accepte ---- */
    Flux st = {0};
    gboolean avec_st = g_str_has_suffix_simple(sortie, ".mkv");
    if (avec_st) {
        st.st = avformat_new_stream(fmt, NULL);
        if (!st.st) fatal("allocation du flux de sous-titres", 0);
        /* PAS DE CODEUR : le SubRip est du texte, et Matroska le prend tel
         * quel. On decrit le flux, on ecrira les paquets a la main. */
        st.st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
        st.st->codecpar->codec_id   = AV_CODEC_ID_SUBRIP;
        st.st->time_base = (AVRational){1, 1000};
        av_dict_set(&st.st->metadata, "language", "fra", 0);
        av_dict_set(&st.st->metadata, "title", "Repères", 0);
    }

    /* ---- ouverture ---- */
    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        r = avio_open(&fmt->pb, sortie, AVIO_FLAG_WRITE);
        if (r < 0) fatal("avio_open", r);
    }
    r = avformat_write_header(fmt, NULL);
    if (r < 0) fatal("avformat_write_header", r);

    int64_t total_img = (int64_t)secondes * CADENCE;
    int64_t total_ech = (int64_t)secondes * ECHANT;

    fprintf(stderr, "fabrique-mire : %s, %d s, %s %s %dx%d@%d + AAC %d Hz\n",
            sortie, secondes, enc_nom, dix_bits ? "10 bits" : "8 bits",
            LARGEUR, HAUTEUR, CADENCE, ECHANT);

    /* ENTRELACEMENT A LA MAIN : on ecrit le flux qui est EN RETARD, sinon
     * av_interleaved_write_frame met tout en file et la memoire enfle --
     * 3,7 Gio soudes sur cette machine, une minute de 1080p ne tient pas en
     * file d'attente. */
    int st_suivant = 0;

    while (v.pts < total_img || a.pts < total_ech) {
        double t_v = (double)v.pts / CADENCE;
        double t_a = (double)a.pts / ECHANT;

        /* CHAQUE SOUS-TITRE A SON HEURE, ET PAS EN BLOC A LA FIN.
         *
         * Ecrits tous ensemble apres la video, ils se retrouvent
         * PHYSIQUEMENT a la fin du fichier : un lecteur qui lit
         * sequentiellement ne les rencontre qu'apres tout le reste, et
         * l'ecran reste vide sans la moindre erreur. Piege paye le
         * 10 septembre 2026 -- le decodeur s'ouvrait, la piste etait
         * annoncee, et rien ne s'affichait. */
        if (avec_st && st_suivant < secondes &&
            (double)st_suivant <= (t_v < t_a ? t_v : t_a)) {
            gchar texte[64];
            g_snprintf(texte, sizeof texte, "seconde %d", st_suivant);
            AVPacket *pk = av_packet_alloc();
            int taille = (int)strlen(texte);
            if (av_new_packet(pk, taille) < 0) fatal("av_new_packet", 0);
            memcpy(pk->data, texte, taille);
            pk->stream_index = st.st->index;
            pk->pts = pk->dts = (int64_t)st_suivant * 1000;
            pk->duration = 900;          /* 0,9 s : un blanc avant la suivante */
            r = av_interleaved_write_frame(fmt, pk);
            av_packet_free(&pk);
            if (r < 0) fatal("ecriture d'un sous-titre", r);
            st_suivant++;
        }

        if (v.pts < total_img && (t_v <= t_a || a.pts >= total_ech)) {
            r = av_frame_make_writable(v.trame);
            if (r < 0) fatal("av_frame_make_writable (video)", r);
            image(v.trame, v.pts);
            v.trame->pts = v.pts++;
            ecrire(fmt, &v, v.trame);
            if (v.pts % (CADENCE * 10) == 0)
                fprintf(stderr, "  %ld s\n", (long)(v.pts / CADENCE));
        } else {
            r = av_frame_make_writable(a.trame);
            if (r < 0) fatal("av_frame_make_writable (audio)", r);
            son(a.trame, a.pts);
            a.trame->pts = a.pts;
            a.pts += a.trame->nb_samples;
            ecrire(fmt, &a, a.trame);
        }
    }

    /* Vidange des deux codeurs : sans elle, les dernieres images restent
     * dans le codeur et le fichier se termine avant la mire. */
    ecrire(fmt, &v, NULL);
    ecrire(fmt, &a, NULL);

    r = av_write_trailer(fmt);
    if (r < 0) fatal("av_write_trailer", r);
    if (!(fmt->oformat->flags & AVFMT_NOFILE))
        avio_closep(&fmt->pb);

    av_frame_free(&v.trame); av_packet_free(&v.paquet); avcodec_free_context(&v.ctx);
    av_frame_free(&a.trame); av_packet_free(&a.paquet); avcodec_free_context(&a.ctx);
    avformat_free_context(fmt);

    fprintf(stderr, "fabrique-mire : termine.\n");
    return 0;
}
