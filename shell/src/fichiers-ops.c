#include "fichiers-ops.h"

#include <string.h>            /* strrchr */

/* Le transtypage direct de g_object_ref vers GCopyFunc fait a juste titre
 * rouspeter le compilateur : les deux signatures different. */
static gpointer
copier_ref (gconstpointer objet, gpointer data)
{
    (void) data;
    return g_object_ref ((gpointer) objet);
}

#define DELAI_FENETRE_MS 400    /* avant d'afficher la progression */

typedef struct {
    OpGenre       genre;
    GList        *sources;      /* GFile*, possedes */
    GFile        *destination;
    GtkWindow    *parent;
    OpFiniFunc    fini;
    gpointer      data;

    GCancellable *annulation;

    /* Partage entre les deux fils. Le fil de travail ecrit, le fil
     * principal lit toutes les 200 ms pour rafraichir la fenetre. */
    GMutex        verrou;
    char         *courant;      /* nom du fichier en cours */
    guint         traites;

    GtkWidget    *fenetre;
    GtkWidget    *etiquette;
    GtkWidget    *barre;
    guint         minuterie;    /* rafraichissement */
    guint         apparition;   /* delai avant d'afficher la fenetre */
} Op;

/* ------------------------------------------------------------------------- */
char *
fichiers_nom_libre (GFile *dossier, const char *nom)
{
    g_autoptr(GFile) essai = g_file_get_child (dossier, nom);
    if (!g_file_query_exists (essai, NULL))
        return g_strdup (nom);

    /* Le suffixe se pose AVANT l'extension : « notes (copie).txt » et non
     * « notes.txt (copie) », qui perdrait l'association au type. */
    const char *point = strrchr (nom, '.');
    /* Un point en tete n'est pas une extension : « .bashrc » est un nom. */
    g_autofree char *base = (point != NULL && point != nom)
                          ? g_strndup (nom, (gsize) (point - nom))
                          : g_strdup (nom);
    const char *ext = (point != NULL && point != nom) ? point : "";

    for (int i = 1; i < 1000; i++) {
        g_autofree char *candidat = (i == 1)
            ? g_strdup_printf ("%s (copie)%s", base, ext)
            : g_strdup_printf ("%s (copie %d)%s", base, i, ext);

        g_autoptr(GFile) f = g_file_get_child (dossier, candidat);
        if (!g_file_query_exists (f, NULL))
            return g_steal_pointer (&candidat);
    }
    return g_strdup (nom);      /* mille copies : on laisse echouer proprement */
}

/* ------------------------------------------------------------------------- */
static void
note_courant (Op *op, GFile *f)
{
    g_autofree char *nom = g_file_get_basename (f);

    g_mutex_lock (&op->verrou);
    g_free (op->courant);
    op->courant = g_steal_pointer (&nom);
    op->traites++;
    g_mutex_unlock (&op->verrou);
}

static gboolean copier_un   (GFile *src, GFile *dst, Op *op, GError **err);
static gboolean supprimer_un (GFile *f, Op *op, GError **err);

/* Enumere les enfants de `dossier` et applique `action` a chacun.
 * Renvoie FALSE a la premiere erreur. */
static gboolean
pour_chaque_enfant (GFile *dossier, Op *op, GError **err,
                    gboolean (*action) (GFile *enfant, const char *nom,
                                        gpointer ctx, GError **err),
                    gpointer ctx)
{
    g_autoptr(GFileEnumerator) e = g_file_enumerate_children (
        dossier,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, op->annulation, err);
    if (e == NULL)
        return FALSE;

    for (;;) {
        GFileInfo *info = NULL;
        GFile     *enfant = NULL;

        if (!g_file_enumerator_iterate (e, &info, &enfant, op->annulation, err))
            return FALSE;
        if (info == NULL)
            return TRUE;        /* fin de l'enumeration */

        if (!action (enfant, g_file_info_get_name (info), ctx, err))
            return FALSE;
    }
}

/* --- copie ---------------------------------------------------------------- */
typedef struct { GFile *dst; Op *op; } CtxCopie;

static gboolean
copier_enfant (GFile *enfant, const char *nom, gpointer ctx, GError **err)
{
    CtxCopie *c = ctx;
    g_autoptr(GFile) cible = g_file_get_child (c->dst, nom);
    return copier_un (enfant, cible, c->op, err);
}

static gboolean
copier_un (GFile *src, GFile *dst, Op *op, GError **err)
{
    if (g_cancellable_set_error_if_cancelled (op->annulation, err))
        return FALSE;

    note_courant (op, src);

    g_autoptr(GFileInfo) info = g_file_query_info (
        src, G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, op->annulation, err);
    if (info == NULL)
        return FALSE;

    if (g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY) {
        /* NOFOLLOW_SYMLINKS : un lien est copie comme lien, pas comme une
         * seconde copie de sa cible. ALL_METADATA garde les dates. */
        return g_file_copy (src, dst,
                            G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA,
                            op->annulation, NULL, NULL, err);
    }

    if (!g_file_make_directory (dst, op->annulation, err)) {
        /* Le dossier existe deja : ce n'est pas une erreur, on y verse. */
        if (!g_error_matches (*err, G_IO_ERROR, G_IO_ERROR_EXISTS))
            return FALSE;
        g_clear_error (err);
    }

    CtxCopie c = { dst, op };
    return pour_chaque_enfant (src, op, err, copier_enfant, &c);
}

/* --- suppression ---------------------------------------------------------- */
static gboolean
supprimer_enfant (GFile *enfant, const char *nom, gpointer ctx, GError **err)
{
    (void) nom;
    return supprimer_un (enfant, ctx, err);
}

static gboolean
supprimer_un (GFile *f, Op *op, GError **err)
{
    if (g_cancellable_set_error_if_cancelled (op->annulation, err))
        return FALSE;

    note_courant (op, f);

    g_autoptr(GFileInfo) info = g_file_query_info (
        f, G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, op->annulation, err);
    if (info == NULL)
        return FALSE;

    /* En profondeur d'abord : un dossier ne se supprime que vide. */
    if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY
        && !pour_chaque_enfant (f, op, err, supprimer_enfant, op))
        return FALSE;

    return g_file_delete (f, op->annulation, err);
}

/* --- le fil de travail ---------------------------------------------------- */
static void
travail (GTask *task, gpointer source, gpointer data, GCancellable *annulation)
{
    Op *op = data;
    g_autoptr(GError) err = NULL;
    (void) source; (void) annulation;

    for (GList *l = op->sources; l != NULL && err == NULL; l = l->next) {
        GFile *src = l->data;

        switch (op->genre) {
        case OP_CORBEILLE:
            note_courant (op, src);
            g_file_trash (src, op->annulation, &err);
            break;

        case OP_SUPPRIMER:
            supprimer_un (src, op, &err);
            break;

        case OP_COPIER:
        case OP_DEPLACER: {
            g_autofree char *nom = g_file_get_basename (src);
            g_autofree char *libre = fichiers_nom_libre (op->destination, nom);
            g_autoptr(GFile) dst = g_file_get_child (op->destination, libre);

            if (op->genre == OP_COPIER) {
                copier_un (src, dst, op, &err);
                break;
            }

            /* Un deplacement sur le meme systeme de fichiers n'est qu'un
             * changement de nom : instantane, et sans recopier un octet. On
             * l'essaie toujours en premier. */
            note_courant (op, src);
            if (g_file_move (src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                             op->annulation, NULL, NULL, &err))
                break;

            /* WOULD_RECURSE : un dossier, d'un disque a un autre. GIO ne
             * sait pas le faire, on copie puis on supprime. */
            if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE))
                break;
            g_clear_error (&err);

            if (copier_un (src, dst, op, &err))
                supprimer_un (src, op, &err);
            break;
        }
        }
    }

    if (err != NULL)
        g_task_return_error (task, g_steal_pointer (&err));
    else
        g_task_return_boolean (task, TRUE);
}

/* --- fenetre de progression ----------------------------------------------- */
static gboolean
rafraichir (gpointer data)
{
    Op *op = data;

    g_mutex_lock (&op->verrou);
    g_autofree char *texte = g_strdup (op->courant ? op->courant : "");
    guint n = op->traites;
    g_mutex_unlock (&op->verrou);

    if (op->etiquette != NULL) {
        g_autofree char *ligne = g_strdup_printf ("%s   (%u traité%s)",
                                                  texte, n, n > 1 ? "s" : "");
        gtk_label_set_text (GTK_LABEL (op->etiquette), ligne);
    }
    /* Barre pulsante : connaitre l'avancement exact demanderait de compter
     * tous les fichiers avant de commencer, c'est-a-dire de parcourir deux
     * fois l'arborescence. Le nom qui defile dit deja que ca avance. */
    if (op->barre != NULL)
        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (op->barre));

    return G_SOURCE_CONTINUE;
}

static void
on_annuler (GtkButton *b, gpointer data)
{
    Op *op = data;
    (void) b;
    g_cancellable_cancel (op->annulation);
}

static const char *
titre_de (OpGenre g)
{
    switch (g) {
    case OP_COPIER:    return "Copie en cours";
    case OP_DEPLACER:  return "Déplacement en cours";
    case OP_CORBEILLE: return "Mise à la corbeille";
    case OP_SUPPRIMER: return "Suppression en cours";
    }
    return "Opération en cours";
}

/* La fenetre n'apparait qu'apres un delai : renommer un fichier ou copier
 * trois octets se termine avant, et une fenetre qui clignote une fraction de
 * seconde est plus derangeante qu'utile. */
static gboolean
montrer_fenetre (gpointer data)
{
    Op *op = data;
    op->apparition = 0;

    GtkWidget *w = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (w), titre_de (op->genre));
    gtk_window_set_modal (GTK_WINDOW (w), TRUE);
    gtk_window_set_resizable (GTK_WINDOW (w), FALSE);
    gtk_window_set_deletable (GTK_WINDOW (w), FALSE);
    if (op->parent != NULL)
        gtk_window_set_transient_for (GTK_WINDOW (w), op->parent);
    gtk_widget_add_css_class (w, "shell");
    gtk_widget_add_css_class (w, "fichiers-progres");

    op->etiquette = gtk_label_new ("");
    gtk_label_set_ellipsize (GTK_LABEL (op->etiquette), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars (GTK_LABEL (op->etiquette), 44);
    gtk_widget_set_halign (op->etiquette, GTK_ALIGN_START);

    op->barre = gtk_progress_bar_new ();

    GtkWidget *annuler = gtk_button_new_with_label ("Annuler");
    gtk_widget_set_halign (annuler, GTK_ALIGN_END);
    g_signal_connect (annuler, "clicked", G_CALLBACK (on_annuler), op);

    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class (boite, "fichiers-progres-pile");
    gtk_box_append (GTK_BOX (boite), op->etiquette);
    gtk_box_append (GTK_BOX (boite), op->barre);
    gtk_box_append (GTK_BOX (boite), annuler);

    gtk_window_set_child (GTK_WINDOW (w), boite);
    gtk_window_set_default_size (GTK_WINDOW (w), 380, -1);
    gtk_window_present (GTK_WINDOW (w));

    op->fenetre = w;
    return G_SOURCE_REMOVE;
}

static void
op_free (Op *op)
{
    if (op->minuterie  != 0) g_source_remove (op->minuterie);
    if (op->apparition != 0) g_source_remove (op->apparition);
    if (op->fenetre != NULL) gtk_window_destroy (GTK_WINDOW (op->fenetre));

    g_list_free_full (op->sources, g_object_unref);
    g_clear_object (&op->destination);
    g_clear_object (&op->annulation);
    g_mutex_clear (&op->verrou);
    g_free (op->courant);
    g_free (op);
}

static void
on_travail_fini (GObject *src, GAsyncResult *res, gpointer data)
{
    Op *op = data;
    g_autoptr(GError) err = NULL;
    (void) src;

    g_task_propagate_boolean (G_TASK (res), &err);

    if (op->fini != NULL)
        op->fini (err, op->data);

    op_free (op);
}

void
fichiers_op (OpGenre genre, GList *sources, GFile *destination,
             GtkWindow *parent, OpFiniFunc fini, gpointer data)
{
    if (sources == NULL) {
        if (fini != NULL)
            fini (NULL, data);
        return;
    }

    Op *op = g_new0 (Op, 1);
    op->genre       = genre;
    op->sources     = g_list_copy_deep (sources, copier_ref, NULL);
    op->destination = destination ? g_object_ref (destination) : NULL;
    op->parent      = parent;
    op->fini        = fini;
    op->data        = data;
    op->annulation  = g_cancellable_new ();
    g_mutex_init (&op->verrou);

    op->apparition = g_timeout_add (DELAI_FENETRE_MS, montrer_fenetre, op);
    op->minuterie  = g_timeout_add (200, rafraichir, op);

    GTask *task = g_task_new (NULL, op->annulation, on_travail_fini, op);
    g_task_set_task_data (task, op, NULL);
    g_task_run_in_thread (task, travail);
    g_object_unref (task);
}
