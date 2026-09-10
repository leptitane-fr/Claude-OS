#include "fichiers-apercu.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <unistd.h>

/* Au-dela, on ne decode pas : une image de cette taille est une anomalie
 * (un scan a 1200 dpi, un panorama), et la lire en entier sur un lecteur
 * reseau pour en tirer 128 pixels n'a pas de sens. */
#define APERCU_OCTETS_MAX   (64 * 1000 * 1000)

/* Textures gardees en memoire : 400 vignettes de 128 px, environ 25 Mo. */
#define APERCU_MAX          400

/* Deux decodages a la fois : un seul laisserait la file avancer au rythme
 * d'un disque lent ; quatre doubleraient la pointe de memoire pour un gain
 * que le Pentium N6000, a quatre coeurs sans hyperthreading, ne donne pas. */
#define APERCU_ACTIFS_MAX   2

#define TAMPON              (64 * 1024)

static struct {
    int           taille;       /* 128 ou 256 pixels                        */
    GHashTable   *mimes;        /* types que gdk-pixbuf sait lire           */
    char         *racine;       /* ~/.cache/thumbnails                      */
    GQueue        file;         /* FichierItem* en attente, ref prise        */
    GQueue        prets;        /* FichierItem* porteurs d'une texture       */
    int           actifs;
    GCancellable *generation;   /* annule a chaque changement de dossier    */
    gboolean      ecriture_signalee;
} A = { .taille = 128 };

typedef struct {
    FichierItem  *it;
    char         *uri;
    char         *vignette;     /* chemin dans le cache                     */
    gint64        modifie;
    int           taille;
} Travail;

static void
travail_free (gpointer p)
{
    Travail *t = p;
    g_object_unref (t->it);
    g_free (t->uri);
    g_free (t->vignette);
    g_free (t);
}

static void
initialiser (void)
{
    if (A.mimes != NULL)
        return;

    /* Les types lisibles sont ceux des chargeurs installes, et non une liste
     * ecrite ici : installer le chargeur webp ou heif les ajoute sans rien
     * recompiler. */
    A.mimes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GSList *formats = gdk_pixbuf_get_formats ();
    for (GSList *l = formats; l != NULL; l = l->next) {
        GdkPixbufFormat *f = l->data;
        if (gdk_pixbuf_format_is_disabled (f))
            continue;
        g_auto(GStrv) mimes = gdk_pixbuf_format_get_mime_types (f);
        for (int i = 0; mimes != NULL && mimes[i] != NULL; i++)
            g_hash_table_add (A.mimes, g_strdup (mimes[i]));
    }
    g_slist_free (formats);

    A.racine = g_build_filename (g_get_user_cache_dir (), "thumbnails", NULL);
    A.generation = g_cancellable_new ();
    g_queue_init (&A.file);
    g_queue_init (&A.prets);
}

/* -------------------------------------------------------------------------
 * Dans le fil de travail
 * ------------------------------------------------------------------------- */
static void
on_taille (GdkPixbufLoader *ch, int w, int h, gpointer data)
{
    int plafond = GPOINTER_TO_INT (data);
    int cote = MAX (w, h);

    /* Jamais agrandir : une icone de 48 px reste une icone de 48 px, la
     * vue se charge de l'afficher a sa taille. */
    if (cote <= plafond)
        return;

    double k = (double) plafond / cote;
    gdk_pixbuf_loader_set_size (ch, MAX (1, (int) (w * k + 0.5)),
                                    MAX (1, (int) (h * k + 0.5)));
}

/* Le cache ne vaut que s'il parle du fichier TEL QU'IL EST : la specification
 * y inscrit la date de modification, et une photo retouchee depuis doit
 * etre refaite. */
static GdkPixbuf *
lire_cache (Travail *t)
{
    g_autoptr(GdkPixbuf) pb = gdk_pixbuf_new_from_file (t->vignette, NULL);
    if (pb == NULL)
        return NULL;

    const char *mt = gdk_pixbuf_get_option (pb, "tEXt::Thumb::MTime");
    if (mt == NULL || g_ascii_strtoll (mt, NULL, 10) != t->modifie)
        return NULL;

    return g_steal_pointer (&pb);
}

/* Ecriture atomique, en 0600 : un fichier temporaire dans le meme
 * repertoire, renomme une fois complet. Un autre programme qui lirait le
 * cache au meme instant ne verrait jamais une vignette a moitie ecrite. */
static gboolean
ecrire_cache (Travail *t, GdkPixbuf *pb, GError **err)
{
    g_autofree char *dir = g_path_get_dirname (t->vignette);
    if (g_mkdir_with_parents (dir, 0700) != 0) {
        g_set_error (err, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "%s : %s", dir, g_strerror (errno));
        return FALSE;
    }

    g_autofree char *tmp = g_strconcat (t->vignette, ".XXXXXX", NULL);
    int fd = g_mkstemp_full (tmp, O_RDWR, 0600);
    if (fd < 0) {
        g_set_error (err, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "%s : %s", tmp, g_strerror (errno));
        return FALSE;
    }
    close (fd);

    g_autofree char *mtime = g_strdup_printf ("%" G_GINT64_FORMAT, t->modifie);
    if (!gdk_pixbuf_save (pb, tmp, "png", err,
                          "tEXt::Thumb::URI", t->uri,
                          "tEXt::Thumb::MTime", mtime,
                          "tEXt::Software", "Claude OS Fichiers",
                          NULL)) {
        g_unlink (tmp);
        return FALSE;
    }

    if (g_rename (tmp, t->vignette) != 0) {
        g_set_error (err, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "%s : %s", t->vignette, g_strerror (errno));
        g_unlink (tmp);
        return FALSE;
    }
    return TRUE;
}

/* Un chargeur alimente par morceaux, comme dans la visionneuse : c'est la
 * seule voie qui reduise au decodage (« size-prepared ») ET consulte
 * l'annulation entre deux lectures. Quitter un dossier de photos ne doit pas
 * laisser deux decodages finir pour rien. */
static GdkPixbuf *
decoder (Travail *t, GCancellable *annul, GError **err)
{
    g_autoptr(GFile) f = g_file_new_for_uri (t->uri);
    g_autoptr(GFileInputStream) flux = g_file_read (f, annul, err);
    if (flux == NULL)
        return NULL;

    g_autoptr(GdkPixbufLoader) ch = gdk_pixbuf_loader_new ();
    g_signal_connect (ch, "size-prepared", G_CALLBACK (on_taille),
                      GINT_TO_POINTER (t->taille));

    g_autofree guchar *tampon = g_malloc (TAMPON);
    gboolean lu = TRUE;
    for (;;) {
        gssize n = g_input_stream_read (G_INPUT_STREAM (flux), tampon, TAMPON,
                                        annul, err);
        if (n < 0) { lu = FALSE; break; }
        if (n == 0) break;
        /* Un echec d'ecriture a deja ferme le chargeur (voir images.c). */
        if (!gdk_pixbuf_loader_write (ch, tampon, (gsize) n, err))
            return NULL;
    }

    if (!gdk_pixbuf_loader_close (ch, lu ? err : NULL) || !lu)
        return NULL;

    GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf (ch);
    if (pb == NULL) {
        g_set_error_literal (err, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                             "aucune image lisible");
        return NULL;
    }

    /* L'orientation EXIF appliquee : une photo prise debout s'affiche
     * debout, comme dans la visionneuse. */
    return gdk_pixbuf_apply_embedded_orientation (pb);
}

static void
fabriquer (GTask *tache, gpointer source, gpointer donnees, GCancellable *annul)
{
    (void) source;
    Travail *t = donnees;
    GError *err = NULL;

    GdkPixbuf *pb = lire_cache (t);
    if (pb != NULL) {
        g_task_return_pointer (tache, pb, g_object_unref);
        return;
    }

    pb = decoder (t, annul, &err);
    if (pb == NULL) {
        g_task_return_error (tache, err);
        return;
    }

    /* L'echec d'ecriture n'empeche pas d'afficher : il remonte a cote de la
     * vignette, pour etre dit une fois dans le journal. */
    GError *err_ecr = NULL;
    if (!ecrire_cache (t, pb, &err_ecr))
        g_object_set_data_full (G_OBJECT (tache), "erreur-ecriture",
                                g_strdup (err_ecr->message), g_free);
    g_clear_error (&err_ecr);

    g_task_return_pointer (tache, pb, g_object_unref);
}

/* -------------------------------------------------------------------------
 * Sur le fil principal
 * ------------------------------------------------------------------------- */
static void lancer (void);

/* Les pixels du pixbuf DEVIENNENT ceux de la texture, sans copie : le GBytes
 * garde le pixbuf en vie. */
static GdkTexture *
texture_de (GdkPixbuf *pb)
{
    GBytes *octets = g_bytes_new_with_free_func (
        gdk_pixbuf_get_pixels (pb), gdk_pixbuf_get_byte_length (pb),
        g_object_unref, g_object_ref (pb));
    GdkTexture *t = gdk_memory_texture_new (
        gdk_pixbuf_get_width (pb), gdk_pixbuf_get_height (pb),
        gdk_pixbuf_get_has_alpha (pb) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
        octets, gdk_pixbuf_get_rowstride (pb));
    g_bytes_unref (octets);
    return t;
}

static void
borner (void)
{
    while (g_queue_get_length (&A.prets) > APERCU_MAX) {
        FichierItem *vieux = g_queue_pop_head (&A.prets);
        /* La case qui l'affiche encore garde sa propre reference a la
         * texture ; l'element, lui, la redemandera au cache disque. */
        if (vieux->apercu_etat == APERCU_PRET) {
            g_clear_object (&vieux->apercu);
            vieux->apercu_etat = APERCU_INCONNU;
        }
        g_object_unref (vieux);
    }
}

static void
on_fabrique (GObject *src, GAsyncResult *res, gpointer data)
{
    (void) src; (void) data;
    GTask *tache = G_TASK (res);
    Travail *t = g_task_get_task_data (tache);
    FichierItem *it = t->it;
    g_autoptr(GError) err = NULL;

    A.actifs--;

    g_autoptr(GdkPixbuf) pb = g_task_propagate_pointer (tache, &err);

    const char *ecr = g_object_get_data (G_OBJECT (tache), "erreur-ecriture");
    if (ecr != NULL && !A.ecriture_signalee) {
        /* Une fois par session : sur un cache en lecture seule, chaque
         * vignette echouerait pareil, et le journal n'en saurait rien de
         * plus a la millieme. */
        g_printerr ("fichiers: vignettes non conservées dans le cache : %s\n", ecr);
        A.ecriture_signalee = TRUE;
    }

    if (it->apercu_etat == APERCU_ATTENTE) {
        if (pb != NULL) {
            g_clear_object (&it->apercu);
            it->apercu = texture_de (pb);
            it->apercu_etat = APERCU_PRET;
            g_queue_push_tail (&A.prets, g_object_ref (it));
            borner ();
            g_signal_emit_by_name (it, "apercu-pret");
        } else if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            it->apercu_etat = APERCU_INCONNU;
        } else {
            it->apercu_etat = APERCU_AUCUN;
        }
    }

    lancer ();
}

static void
lancer (void)
{
    while (A.actifs < APERCU_ACTIFS_MAX && !g_queue_is_empty (&A.file)) {
        /* Par la tete : la derniere demandee, donc la derniere apparue a
         * l'ecran. */
        FichierItem *it = g_queue_pop_head (&A.file);

        g_autofree char *uri = g_file_get_uri (it->file);
        g_autofree char *md5 = g_compute_checksum_for_string (G_CHECKSUM_MD5, uri, -1);
        g_autofree char *nom = g_strconcat (md5, ".png", NULL);

        Travail *t = g_new0 (Travail, 1);
        t->it       = it;                       /* la reference de la file */
        t->uri      = g_steal_pointer (&uri);
        t->modifie  = it->modifie;
        t->taille   = A.taille;
        t->vignette = g_build_filename (A.racine,
                                        A.taille > 128 ? "large" : "normal",
                                        nom, NULL);

        GTask *tache = g_task_new (NULL, A.generation, on_fabrique, NULL);
        g_task_set_task_data (tache, t, travail_free);
        g_task_set_priority (tache, G_PRIORITY_LOW);
        g_task_run_in_thread (tache, fabriquer);
        g_object_unref (tache);
        A.actifs++;
    }
}

static gboolean
admissible (FichierItem *it)
{
    if (it->dossier || it->type_mime == NULL)
        return FALSE;
    if (it->taille <= 0 || it->taille > APERCU_OCTETS_MAX)
        return FALSE;
    if (!g_hash_table_contains (A.mimes, it->type_mime))
        return FALSE;

    /* La specification l'interdit : faire la vignette d'une vignette
     * remplirait le cache de lui-meme a chaque visite. */
    g_autofree char *chemin = g_file_get_path (it->file);
    if (chemin != NULL && g_str_has_prefix (chemin, A.racine))
        return FALSE;

    return TRUE;
}

void
fichiers_apercu_regler (int facteur_echelle)
{
    initialiser ();
    A.taille = facteur_echelle > 1 ? 256 : 128;
}

GdkTexture *
fichiers_apercu_obtenir (FichierItem *it)
{
    initialiser ();

    if (it->apercu_etat == APERCU_PRET)
        return it->apercu;

    if (it->apercu_etat == APERCU_INCONNU) {
        if (!admissible (it)) {
            it->apercu_etat = APERCU_AUCUN;
            return NULL;
        }
        it->apercu_etat = APERCU_ATTENTE;
        g_queue_push_head (&A.file, g_object_ref (it));
        lancer ();
    }
    return NULL;
}

void
fichiers_apercu_oublier (FichierItem *it)
{
    if (A.mimes == NULL || it->apercu_etat != APERCU_ATTENTE)
        return;

    /* Seulement si elle attend encore : un decodage commence va a son terme,
     * et la vignette servira au prochain passage. */
    if (g_queue_remove (&A.file, it)) {
        it->apercu_etat = APERCU_INCONNU;
        g_object_unref (it);
    }
}

void
fichiers_apercu_abandonner (void)
{
    if (A.mimes == NULL)
        return;

    g_cancellable_cancel (A.generation);
    g_clear_object (&A.generation);
    A.generation = g_cancellable_new ();

    FichierItem *it;
    while ((it = g_queue_pop_head (&A.file)) != NULL) {
        it->apercu_etat = APERCU_INCONNU;
        g_object_unref (it);
    }

    /* Les textures du dossier quitte partent avec ses elements. */
    while ((it = g_queue_pop_head (&A.prets)) != NULL)
        g_object_unref (it);
}
