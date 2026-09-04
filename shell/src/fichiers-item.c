#include "fichiers.h"

G_DEFINE_TYPE (FichierItem, fichier_item, G_TYPE_OBJECT)

static void
fichier_item_finalize (GObject *o)
{
    FichierItem *it = FICHIERS_ITEM (o);

    g_clear_object (&it->file);
    g_clear_object (&it->icone);
    g_free (it->nom);
    g_free (it->cle_tri);
    g_free (it->type_texte);

    G_OBJECT_CLASS (fichier_item_parent_class)->finalize (o);
}

static void
fichier_item_class_init (FichierItemClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = fichier_item_finalize;
}

static void
fichier_item_init (FichierItem *it)
{
    (void) it;
}

FichierItem *
fichier_item_new (GFile *parent, GFileInfo *info)
{
    FichierItem *it = g_object_new (FICHIERS_TYPE_ITEM, NULL);

    const char *nom = g_file_info_get_name (info);
    it->file = g_file_get_child (parent, nom);

    const char *affiche = g_file_info_get_display_name (info);
    it->nom = g_strdup (affiche != NULL ? affiche : nom);

    /* collate_key_for_filename, et non collate_key : elle range « photo2 »
     * avant « photo10 » en lisant les nombres comme des nombres, et ignore
     * le point initial des fichiers caches. C'est l'ordre qu'on attend d'un
     * gestionnaire de fichiers. */
    it->cle_tri = g_utf8_collate_key_for_filename (it->nom, -1);

    GFileType type = g_file_info_get_file_type (info);
    it->dossier = (type == G_FILE_TYPE_DIRECTORY);
    it->lien    = g_file_info_get_is_symlink (info);
    it->cache   = g_file_info_get_is_hidden (info) || g_file_info_get_is_backup (info);
    it->taille  = g_file_info_get_size (info);

    GIcon *ic = g_file_info_get_icon (info);
    it->icone = (ic != NULL) ? g_object_ref (ic) : g_themed_icon_new ("text-x-generic");

    const char *ct = g_file_info_get_content_type (info);
    if (it->dossier)
        it->type_texte = g_strdup ("Dossier");
    else if (ct != NULL)
        it->type_texte = g_content_type_get_description (ct);
    else
        it->type_texte = g_strdup ("Fichier");

    g_autoptr(GDateTime) dt = g_file_info_get_modification_date_time (info);
    it->modifie = (dt != NULL) ? g_date_time_to_unix (dt) : 0;

    return it;
}

char *
fichier_item_taille_texte (FichierItem *it)
{
    /* Un dossier n'a pas de taille propre, et la calculer voudrait dire
     * parcourir tout son contenu a chaque affichage. Windows ne le fait pas
     * non plus dans la liste ; on ne promet donc rien. */
    if (it->dossier)
        return g_strdup ("");

    /* Unites ecrites ici plutot que par g_format_size, qui suit la locale du
     * systeme : sur une machine dont la locale n'aurait pas ete mise en
     * francais, elle rendrait « 240.0 kB » au milieu d'une interface
     * francaise. Le reste du gestionnaire ne depend d'aucune locale, celle-ci
     * ne doit pas faire exception.
     *
     * Puissances de 1000, comme g_format_size : c'est la convention du
     * systeme de fichiers et des tailles annoncees par les fabricants. */
    static const char *const UNITES[] = { "o", "ko", "Mo", "Go", "To", NULL };

    double v = (double) it->taille;
    int u = 0;
    while (v >= 1000.0 && UNITES[u + 1] != NULL) {
        v /= 1000.0;
        u++;
    }

    /* Pas de decimale pour les octets : « 6,0 o » n'a pas de sens. */
    if (u == 0)
        return g_strdup_printf ("%.0f %s", v, UNITES[u]);
    return g_strdup_printf ("%.1f %s", v, UNITES[u]);
}

char *
fichier_item_date_texte (FichierItem *it)
{
    if (it->modifie == 0)
        return g_strdup ("");

    g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local (it->modifie);
    return g_date_time_format (dt, "%d/%m/%Y %H:%M");
}

/* -------------------------------------------------------------------------
 * Lecture d'un repertoire
 * ------------------------------------------------------------------------- */

/* Les attributs demandes, et rien de plus. Chaque attribut supplementaire
 * est un appel systeme de plus par fichier : sur un repertoire de plusieurs
 * milliers d'entrees, la difference se voit. */
#define ATTRIBUTS \
    G_FILE_ATTRIBUTE_STANDARD_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
    G_FILE_ATTRIBUTE_STANDARD_SIZE "," \
    G_FILE_ATTRIBUTE_STANDARD_ICON "," \
    G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," \
    G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," \
    G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP "," \
    G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK "," \
    G_FILE_ATTRIBUTE_TIME_MODIFIED

#define LOT 128                /* entrees par tour */

typedef struct {
    GFile          *dossier;
    GListStore     *magasin;
    FichiersLuFunc  fini;
    gpointer        data;
    GCancellable   *annulation;
} Lecture;

/* Une seule lecture a la fois. Naviguer plus vite que le disque ne repond
 * lancerait deux enumerations concurrentes, qui rempliraient le meme
 * magasin en s'entremelant -- deux repertoires melanges a l'ecran. */
static GCancellable *en_cours = NULL;

static void
lecture_free (Lecture *l)
{
    g_clear_object (&l->dossier);
    g_clear_object (&l->magasin);
    g_clear_object (&l->annulation);
    g_free (l);
}

static void suivant (GFileEnumerator *e, Lecture *l);

static void
on_lot (GObject *src, GAsyncResult *res, gpointer data)
{
    GFileEnumerator *e = G_FILE_ENUMERATOR (src);
    Lecture *l = data;
    g_autoptr(GError) error = NULL;

    GList *infos = g_file_enumerator_next_files_finish (e, res, &error);

    if (g_cancellable_is_cancelled (l->annulation)) {
        g_list_free_full (infos, g_object_unref);
        lecture_free (l);
        return;
    }

    if (error != NULL) {
        l->fini (l->magasin, error, l->data);
        lecture_free (l);
        return;
    }

    if (infos == NULL) {          /* plus rien : c'est fini */
        l->fini (l->magasin, NULL, l->data);
        lecture_free (l);
        return;
    }

    for (GList *i = infos; i != NULL; i = i->next) {
        g_autoptr(FichierItem) it = fichier_item_new (l->dossier, i->data);
        g_list_store_append (l->magasin, it);
    }
    g_list_free_full (infos, g_object_unref);

    suivant (e, l);
}

static void
suivant (GFileEnumerator *e, Lecture *l)
{
    g_file_enumerator_next_files_async (e, LOT, G_PRIORITY_DEFAULT,
                                        l->annulation, on_lot, l);
}

static void
on_enumerate (GObject *src, GAsyncResult *res, gpointer data)
{
    Lecture *l = data;
    g_autoptr(GError) error = NULL;

    GFileEnumerator *e =
        g_file_enumerate_children_finish (G_FILE (src), res, &error);

    if (g_cancellable_is_cancelled (l->annulation)) {
        g_clear_object (&e);
        lecture_free (l);
        return;
    }

    if (e == NULL) {
        l->fini (l->magasin, error, l->data);
        lecture_free (l);
        return;
    }

    /* L'enumerateur se libere avec le dernier lot : on le confie a la
     * chaine de rappels en l'attachant a l'annulation du contexte. */
    g_object_set_data_full (G_OBJECT (l->magasin), "enumerateur", e, g_object_unref);
    suivant (e, l);
}

void
fichiers_lire (GFile *dossier, GListStore *magasin,
               FichiersLuFunc fini, gpointer data)
{
    if (en_cours != NULL) {
        g_cancellable_cancel (en_cours);
        g_clear_object (&en_cours);
    }

    g_list_store_remove_all (magasin);

    Lecture *l = g_new0 (Lecture, 1);
    l->dossier    = g_object_ref (dossier);
    l->magasin    = g_object_ref (magasin);
    l->fini       = fini;
    l->data       = data;
    l->annulation = g_cancellable_new ();
    en_cours      = g_object_ref (l->annulation);

    g_file_enumerate_children_async (dossier, ATTRIBUTS,
                                     G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                                     l->annulation, on_enumerate, l);
}
