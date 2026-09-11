/* Claude OS -- sortie audio du lecteur video. Voir video-audio.h pour le
 * pourquoi ; ici, le comment et les pieges.
 */

/* SPA (PipeWire) se sert de locale_t, uselocale et LC_ALL_MASK dans un
 * en-tete en ligne. Sous -std=c11 strict, __STRICT_ANSI__ les masque et la
 * compilation echoue sur « unknown type name locale_t » -- une erreur qui
 * designe un en-tete du systeme et non le notre, donc facile a mal
 * diagnostiquer. _GNU_SOURCE doit venir AVANT le premier include. */
#define _GNU_SOURCE 1

#include "video-audio.h"

#include <gio/gio.h>            /* G_IO_ERROR, pour les GError rendus  */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>    /* spa_strerror : un code SPA en clair */

#include <math.h>
#include <string.h>

/* Deux cents millisecondes de tampon. Assez pour absorber un hoquet du
 * decodeur, assez peu pour qu'un saut soit immediat -- ce qui est vide au
 * saut, l'oreille l'entend comme un temps de reaction. */
#define TAMPON_MS 1000

struct _VideoAudio {
    struct pw_thread_loop *boucle;
    struct pw_stream      *flux;
    struct spa_hook        ecoute;

    int      taux;
    int      canaux;

    /* LE TAMPON CIRCULAIRE, ET SON VERROU.
     *
     * Le rappel de PipeWire tourne dans un fil temps reel ; le decodeur
     * ecrit depuis le sien. Un mutex tenu le temps d'un memcpy est
     * acceptable ici -- ce n'est pas de la synthese, la section critique
     * dure quelques microsecondes et ne peut pas se bloquer sur autre
     * chose. Ce qui serait fautif serait d'y allouer, d'y journaliser ou
     * d'y attendre. */
    GMutex   verrou;
    float   *anneau;          /* entrelace, taille = capacite * canaux      */
    int      capacite;        /* en images                                  */
    int      tete;            /* prochaine ecriture                         */
    int      queue;           /* prochaine lecture                          */
    int      remplissage;     /* en images                                  */

    /* L'horodatage du PROCHAIN echantillon a ecrire. C'est de lui que se
     * deduit toute la position : ce qu'on entend est ce qu'on a ecrit,
     * moins ce qui attend encore. */
    double   pts_ecriture;
    gboolean pts_connu;

    gboolean en_pause;
    gboolean joue_quelque_chose;
    gdouble  gain;            /* 0 a 1, lu par le fil temps reel        */
};

/* ------------------------------------------------------------ le rappel RT */

static void sur_traitement(void *data)
{
    VideoAudio *a = data;

    struct pw_buffer *b = pw_stream_dequeue_buffer(a->flux);
    if (!b) return;                       /* le graphe est en avance */

    struct spa_data *d = &b->buffer->datas[0];
    if (!d->data) { pw_stream_queue_buffer(a->flux, b); return; }

    int pas = (int)sizeof(float) * a->canaux;
    int max = (int)(d->maxsize / pas);
    int veut = b->requested ? (int)b->requested : max;
    if (veut > max) veut = max;

    float *sortie = d->data;
    int    ecrit  = 0;

    g_mutex_lock(&a->verrou);
    while (ecrit < veut && a->remplissage > 0) {
        int dispo = a->capacite - a->queue;          /* jusqu'au bout        */
        int n = MIN(veut - ecrit, MIN(dispo, a->remplissage));
        memcpy(sortie + (size_t)ecrit * a->canaux,
               a->anneau + (size_t)a->queue * a->canaux,
               (size_t)n * pas);
        a->queue = (a->queue + n) % a->capacite;
        a->remplissage -= n;
        ecrit += n;
    }
    g_mutex_unlock(&a->verrou);

    /* LE GAIN, ICI ET PAS AILLEURS. Une multiplication par echantillon sur
     * ce que l'on vient de copier : le volume reagit en un quantum, et rien
     * n'est deja « teinte » dans le tampon. A 1.0 exactement on ne touche a
     * rien -- le cas courant ne doit pas payer pour l'exception. */
    double g = a->gain;
    if (g != 1.0 && ecrit > 0) {
        int total = ecrit * a->canaux;
        for (int i = 0; i < total; i++) sortie[i] *= (float)g;
    }

    /* SOUS-ALIMENTATION : on complete par du silence plutot que de rendre un
     * tampon court. Un tampon court fait sauter le graphe entier, et le
     * craquement s'entend sur tout le bureau, pas seulement dans le lecteur. */
    if (ecrit < veut)
        memset(sortie + (size_t)ecrit * a->canaux, 0,
               (size_t)(veut - ecrit) * pas);

    if (ecrit > 0) a->joue_quelque_chose = TRUE;

    d->chunk->offset = 0;
    d->chunk->stride = pas;
    d->chunk->size   = (uint32_t)(veut * pas);
    b->size          = (uint64_t)veut;

    pw_stream_queue_buffer(a->flux, b);
}

/* INVARIANT N.4 : un flux qui tombe en erreur le DIT. Sans ce rappel, une
 * sortie refusee par le serveur donne un lecteur muet et sans explication. */
static void sur_etat(void *data, enum pw_stream_state vieux,
                     enum pw_stream_state neuf, const char *erreur)
{
    (void)data; (void)vieux;
    if (neuf == PW_STREAM_STATE_ERROR)
        g_warning("video-audio : le flux PipeWire est en erreur -- %s",
                  erreur ? erreur : "sans motif");
}

static const struct pw_stream_events EVENEMENTS = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = sur_etat,
    .process       = sur_traitement,
};

/* ------------------------------------------------------------- ouverture */

VideoAudio *video_audio_ouvrir(int taux, int canaux, GError **erreur)
{
    if (taux <= 0 || canaux <= 0 || canaux > 8) {
        g_set_error(erreur, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "format audio hors limites : %d Hz, %d canaux",
                    taux, canaux);
        return NULL;
    }

    /* pw_init peut etre appele plusieurs fois sans dommage, mais une seule
     * fois suffit et le lecteur n'a qu'une sortie. */
    static gsize une_fois = 0;
    if (g_once_init_enter(&une_fois)) {
        pw_init(NULL, NULL);
        g_once_init_leave(&une_fois, 1);
    }

    VideoAudio *a = g_new0(VideoAudio, 1);
    a->taux     = taux;
    a->canaux   = canaux;
    a->capacite = taux * TAMPON_MS / 1000;
    a->gain     = 1.0;
    a->anneau   = g_malloc0((size_t)a->capacite * canaux * sizeof(float));
    g_mutex_init(&a->verrou);

    a->boucle = pw_thread_loop_new("claude-os-video", NULL);
    if (!a->boucle) {
        g_set_error(erreur, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "pw_thread_loop_new a echoue");
        video_audio_fermer(a);
        return NULL;
    }

    /* UN QUANTUM LONG, DELIBEREMENT. Voir l'en-tete : moitie moins de
     * reveils, et aucune consequence perceptible sur une video. */
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE,     "Movie",
        PW_KEY_APP_NAME,       "Vidéo",
        PW_KEY_NODE_NAME,      "claude-os-video",
        PW_KEY_NODE_LATENCY,   "2048/48000",
        NULL);

    pw_thread_loop_lock(a->boucle);

    a->flux = pw_stream_new_simple(pw_thread_loop_get_loop(a->boucle),
                                   "Vidéo", props, &EVENEMENTS, a);
    if (!a->flux) {
        pw_thread_loop_unlock(a->boucle);
        g_set_error(erreur, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "pw_stream_new_simple a echoue");
        video_audio_fermer(a);
        return NULL;
    }

    uint8_t pod[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod, sizeof pod);
    struct spa_audio_info_raw info = {
        .format   = SPA_AUDIO_FORMAT_F32,
        .rate     = (uint32_t)taux,
        .channels = (uint32_t)canaux,
    };
    if (canaux == 1) {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (canaux == 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    const struct spa_pod *params[1] = {
        spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)
    };

    int r = pw_stream_connect(a->flux, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              PW_STREAM_FLAG_AUTOCONNECT |
                              PW_STREAM_FLAG_MAP_BUFFERS |
                              PW_STREAM_FLAG_RT_PROCESS,
                              params, 1);
    pw_thread_loop_unlock(a->boucle);

    if (r < 0) {
        g_set_error(erreur, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "pw_stream_connect a rendu %d (%s)", r, spa_strerror(r));
        video_audio_fermer(a);
        return NULL;
    }

    if (pw_thread_loop_start(a->boucle) < 0) {
        g_set_error(erreur, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "pw_thread_loop_start a echoue");
        video_audio_fermer(a);
        return NULL;
    }
    return a;
}

void video_audio_fermer(VideoAudio *a)
{
    if (!a) return;
    if (a->boucle) {
        pw_thread_loop_stop(a->boucle);
        if (a->flux) {
            pw_thread_loop_lock(a->boucle);
            pw_stream_destroy(a->flux);
            pw_thread_loop_unlock(a->boucle);
        }
        pw_thread_loop_destroy(a->boucle);
    }
    g_mutex_clear(&a->verrou);
    g_free(a->anneau);
    g_free(a);
}

/* --------------------------------------------------------------- ecriture */

int video_audio_place(VideoAudio *a)
{
    g_mutex_lock(&a->verrou);
    int p = a->capacite - a->remplissage;
    g_mutex_unlock(&a->verrou);
    return p;
}

int video_audio_ecrire(VideoAudio *a, const float *entrelace, int images,
                       double pts)
{
    g_mutex_lock(&a->verrou);

    /* Le premier bloc apres un vidage repose l'horloge ; les suivants la
     * font simplement avancer. Recaler a chaque bloc ferait sauter la
     * position d'un demi-tampon a chaque appel. */
    if (!a->pts_connu) {
        a->pts_ecriture = pts;
        a->pts_connu    = TRUE;
    }

    int ecrit = 0;
    while (ecrit < images && a->remplissage < a->capacite) {
        int jusqu_au_bout = a->capacite - a->tete;
        int libre = a->capacite - a->remplissage;
        int n = MIN(images - ecrit, MIN(jusqu_au_bout, libre));
        memcpy(a->anneau + (size_t)a->tete * a->canaux,
               entrelace + (size_t)ecrit * a->canaux,
               (size_t)n * a->canaux * sizeof(float));
        a->tete = (a->tete + n) % a->capacite;
        a->remplissage += n;
        ecrit += n;
    }
    a->pts_ecriture += (double)ecrit / a->taux;

    g_mutex_unlock(&a->verrou);
    return ecrit;
}

/* --------------------------------------------------------------- horloge */

gboolean video_audio_horloge(VideoAudio *a, double *secondes)
{
    if (!a || !a->flux || !a->joue_quelque_chose) return FALSE;

    g_mutex_lock(&a->verrou);
    double  pts_ecriture = a->pts_ecriture;
    int     en_file      = a->remplissage;
    gboolean connu       = a->pts_connu;
    g_mutex_unlock(&a->verrou);

    if (!connu) return FALSE;

    struct pw_time t;
    double retard = 0.0, depuis = 0.0;
    if (pw_stream_get_time_n(a->flux, &t, sizeof t) == 0 && t.rate.denom > 0) {
        /* delay : le trajet jusqu'au haut-parleur. queued et buffered : ce
         * qui attend deja dans le flux et dans le reechantillonneur. Les
         * trois s'additionnent, et les oublier avance l'image d'autant. */
        retard = (double)t.delay * t.rate.num / t.rate.denom;
        if (a->taux > 0)
            retard += (double)(t.queued + t.buffered) / a->taux;

        /* ET SURTOUT : CET INSTANTANE A UN AGE.
         *
         * pw_time n'est actualise qu'une fois par cycle du graphe -- 42,7 ms
         * avec le quantum demande ici. Sans le terme qui suit, l'horloge est
         * un ESCALIER dont chaque marche vaut un cycle, et le haut-parleur,
         * lui, continue pendant ce temps.
         *
         * Ce que cela coute quand on l'oublie, mesure au banc le 10 septembre
         * 2026 sur une mire a 30 im/s : une marche de 42,7 ms couvre
         * 1,28 image, donc une fois sur quatre DEUX images deviennent dues au
         * meme battement et l'une des deux est sautee. Resultat : 23 images
         * affichees par seconde au lieu de 30, et 6,7 sauts par seconde --
         * parfaitement reguliers, ce qui donne une saccade que l'on prend
         * pour un decodeur trop lent alors qu'il ne l'est pas.
         *
         * pw_time.now est la date de l'instantane ; l'ecart avec maintenant
         * est du temps qui a REELLEMENT ete joue. Il est borne a deux cycles
         * par prudence : si le graphe s'arrete, l'horloge ne doit pas partir
         * a l'infini. */
        uint64_t maintenant_ns = pw_stream_get_nsec(a->flux);
        if (maintenant_ns > (uint64_t)t.now) {
            depuis = (double)(maintenant_ns - (uint64_t)t.now) / 1e9;
            double plafond = t.rate.denom > 0
                           ? 2.0 * 2048.0 / a->taux : 0.1;
            if (depuis > plafond) depuis = plafond;
        }
    }

    /* Ce qu'on ENTEND = ce qu'on a ecrit, moins ce qui n'est pas encore
     * sorti. Le maximum evite qu'une position negative n'apparaisse au tout
     * debut, quand le tampon se remplit avant que rien ne soit joue. */
    double pos = pts_ecriture - (double)en_file / a->taux - retard + depuis;
    *secondes = MAX(pos, 0.0);
    return TRUE;
}

void video_audio_pause(VideoAudio *a, gboolean en_pause)
{
    if (!a || !a->flux || a->en_pause == en_pause) return;
    a->en_pause = en_pause;

    /* set_active(false) arrete le rappel : plus un reveil tant que la
     * lecture est suspendue. C'est la regle d'energie n.2, et elle tient en
     * une ligne parce que PipeWire la porte deja. */
    pw_thread_loop_lock(a->boucle);
    pw_stream_set_active(a->flux, !en_pause);
    pw_thread_loop_unlock(a->boucle);
}

void video_audio_gain(VideoAudio *a, double gain)
{
    if (a) a->gain = CLAMP(gain, 0.0, 1.0);
}

void video_audio_vider(VideoAudio *a)
{
    if (!a) return;
    g_mutex_lock(&a->verrou);
    a->tete = a->queue = a->remplissage = 0;
    a->pts_connu = FALSE;
    g_mutex_unlock(&a->verrou);

    if (a->flux) {
        pw_thread_loop_lock(a->boucle);
        pw_stream_flush(a->flux, false);
        pw_thread_loop_unlock(a->boucle);
    }
}

gboolean video_audio_epuise(VideoAudio *a)
{
    if (!a) return TRUE;
    g_mutex_lock(&a->verrou);
    gboolean vide = (a->remplissage == 0);
    g_mutex_unlock(&a->verrou);
    return vide;
}
