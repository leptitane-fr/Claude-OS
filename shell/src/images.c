/* =========================================================================
 * Claude-OS — Images, la visionneuse
 *
 * Une fenêtre ORDINAIRE, comme Fichiers : elle se déplace, se redimensionne
 * et se réduit. On l'ouvre d'un double-clic sur une image, et l'on parcourt
 * ensuite tout le dossier sans revenir au gestionnaire de fichiers.
 *
 * DISPOSITION
 *
 *   +---------------------------------------------------------------+
 *   | ‹ ›  photo.jpg  3 / 42          − 38 % +   ⟲ ⟳   ▭ ⊕ ⛶        |
 *   +---------------------------------------------------------------+
 *   |  (‹)                                                   (›)    |
 *   |                        l'image                                |
 *   +---------------------------------------------------------------+
 *   | 4032 × 3024 · 3,2 Mo · image JPEG · 12/03/2026    ~/Images    |
 *   +---------------------------------------------------------------+
 *
 * En plein écran, les deux barres s'effacent. Il ne reste que l'image, deux
 * flèches et une pilule qui dit où l'on est ; tout disparaît au bout de
 * quelques secondes d'immobilité, pointeur compris, et revient au moindre
 * mouvement ou d'un appui du doigt.
 *
 * TROIS FAÇONS D'AVANCER, et elles font toutes la même chose
 *
 *   le doigt       balayage sur l'image — voir images-vue.c, qui traite
 *                  aussi pincer et tourner à deux doigts
 *   le clavier     ← → , Page préc./suiv., Espace, Début, Fin
 *   la souris      les flèches de la barre ou celles posées sur l'image,
 *                  la molette, ou un glisser bouton enfoncé
 *
 * LA TOUCHE « PLEIN ÉCRAN » DU CHROMEBOOK N'ARRIVE JAMAIS ICI
 *
 * labwc la lie à ToggleFullscreen (rc.xml, XF86FullScreen) et la garde pour
 * lui. Le compositeur met la fenêtre en plein écran de son propre chef ;
 * c'est donc la PROPRIÉTÉ « fullscreened » que l'on suit, pas une touche.
 * Qu'on passe par F11, par un double appui, par le bouton ou par cette
 * touche, l'interface réagit de la même façon.
 *
 * DÉCODER À LA TAILLE DE L'ÉCRAN, PAS À CELLE DU FICHIER
 *
 * Une photo de téléphone fait 4032 × 3024 : 35 Mo une fois décodée en RVB,
 * sur une machine qui n'a que 4 Go soudés. Or l'écran n'en montre jamais
 * plus de 1920 pixels de large. L'image est donc décodée RÉDUITE, à la plus
 * petite taille qui couvre encore le plus grand écran branché.
 *
 * Mesuré sur MADOO (Pentium Silver N6000), photo de 4032 × 3024 :
 *
 *     pleine résolution           85 ms    35 Mo
 *     réduite à 1920 px          112 ms     8 Mo
 *     réduite à la moitié         50 ms     9 Mo
 *
 * La ligne du milieu est le piège. libjpeg ne sait réduire que par 2, 4 ou 8
 * PENDANT le décodage ; demander 1920 lui fait décoder 2016, puis gdk-pixbuf
 * rééchantillonne le reste — plus lentement que tout le décodage. On réduit
 * donc par une puissance de deux, jamais à la taille exacte de l'écran : voir
 * on_taille.
 *
 * La pleine résolution n'est décodée qu'à la demande, quand on zoome
 * au-delà de ce que la texture réduite peut montrer — et seulement pour
 * l'image courante. La toile tient sa géométrie en pixels de l'image
 * d'origine : la texture fine remplace la grossière sans que rien ne bouge.
 *
 * On garde en mémoire trois images décodées : la courante et ses deux
 * voisines. C'est ce qui permet au balayage de montrer la suivante sous le
 * doigt, avant même qu'on l'ait choisie.
 * ========================================================================= */

#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "images-vue.h"

#define DELAI_MASQUAGE  2500    /* ms d'immobilité avant d'effacer les commandes */
/* Sous ce seuil, la roue d'attente ne ferait que clignoter : une image
 * décodée à la taille de l'écran arrive souvent plus vite. */
#define DELAI_ATTENTE   200     /* ms */
#define DELAI_RELECTURE 400     /* ms de calme dans le dossier avant de le relire */

/* Côté le plus long d'une « pleine » résolution. À ce plafond, une image 4:3
 * pèse déjà 150 Mo en RVB (8192 × 6144 × 3). Au-delà — panoramas, scans à
 * 600 ppp —, l'agrandissement extrême se fera avec un peu de flou plutôt
 * qu'au prix de la mémoire. */
#define PLEINE_MAX      8192
#define LIMITE_DEFAUT   1920
#define PAS_ZOOM        1.25
#define TAMPON          (64 * 1024)

/* -------------------------------------------------------------------------
 * Les images du dossier
 * ------------------------------------------------------------------------- */
typedef struct {
    GFile  *fichier;
    char   *nom;
    char   *cle;        /* clé de tri                         */
    char   *type;       /* type MIME, deviné sur le nom       */
    goffset taille;
    gint64  modifie;    /* secondes Unix, 0 si inconnu        */
} Entree;

static void
entree_free (gpointer p)
{
    Entree *e = p;
    g_object_unref (e->fichier);
    g_free (e->nom);
    g_free (e->cle);
    g_free (e->type);
    g_free (e);
}

/* -------------------------------------------------------------------------
 * Une image décodée, ou en passe de l'être
 *
 * COMPTÉE PAR RÉFÉRENCES, et c'est ce qui la protège. Un décodage en cours
 * tient la sienne : si l'utilisateur est passé trois images plus loin
 * entre-temps, la charge a quitté le cache et porte « abandonnee » — le
 * rappel le voit, jette le résultat, et libère. Sans cela, le rappel
 * écrirait dans une structure déjà rendue.
 * ------------------------------------------------------------------------- */
typedef struct {
    int           refs;
    GFile        *fichier;
    gint64        modifie;       /* date du fichier décodé               */
    GdkTexture   *texture;       /* à la taille de l'écran, ou moins     */
    GdkTexture   *pleine;        /* pleine résolution — courante seule   */
    int           largeur;       /* image d'origine, orientée            */
    int           hauteur;
    GdkPixbufAnimation *anim;    /* GIF animé                            */
    char         *erreur;
    GCancellable *annul;         /* décodage réduit en cours             */
    GCancellable *annul_pleine;  /* décodage pleine résolution en cours  */
    gboolean      abandonnee;
} Charge;

static struct {
    GtkApplication *app;
    GtkWidget  *fenetre;
    ImagesVue  *vue;

    GtkWidget  *revele_barre, *revele_etat;
    GtkWidget  *nom, *position, *zoom, *b_plein;
    GtkWidget  *etat, *etat_dossier;
    GtkWidget  *fleche_g, *fleche_d, *pilule, *pilule_texte;
    GtkWidget  *attente;
    GtkWidget  *erreur, *erreur_detail;
    GtkWidget  *vide, *vide_detail;

    gboolean    plein;
    gboolean    controles;      /* flèches et pilule visibles          */
    guint       masquer_id;
    guint       attente_id;
    double      mx, my;         /* dernier pointeur, repère fenêtre    */

    GFile        *dossier;
    GFileMonitor *moniteur;
    guint         relire_id;
    GCancellable *annul_liste;
    GPtrArray    *entrees;      /* Entree*, triées                     */
    int           index;        /* -1 : rien à montrer                 */
    gboolean      ouverture;    /* un fichier est en route             */

    GPtrArray    *cache;        /* Charge*, trois au plus              */
    Charge       *affichee;     /* celle que montre la toile           */
    GHashTable   *types;        /* types MIME décodables               */

    GdkPixbufAnimationIter *iter;
    Charge       *anim_charge;
    guint         anim_id;
} I = { .index = -1 };

static void rafraichir (void);
static void maj_barre (void);
static void maj_etat (void);
static void maj_zoom (void);
static void montrer_controles (gboolean montrer);
static void lire_dossier (GFile *dossier, GFile *voulu, gboolean nouveau);

static Entree *
entree (int i)
{
    if (I.entrees == NULL || i < 0 || i >= (int) I.entrees->len)
        return NULL;
    return g_ptr_array_index (I.entrees, i);
}

static int
nombre (void)
{
    return I.entrees != NULL ? (int) I.entrees->len : 0;
}

static void
activer (const char *nom, gboolean oui)
{
    GAction *a = g_action_map_lookup_action (G_ACTION_MAP (I.app), nom);
    if (a != NULL)
        g_simple_action_set_enabled (G_SIMPLE_ACTION (a), oui);
}

/* -------------------------------------------------------------------------
 * Formats
 *
 * DEMANDÉS À GDK-PIXBUF, PAS ÉCRITS ICI. Installer un chargeur — WebP, AVIF
 * — suffit alors à ce que la visionneuse l'ouvre et en montre les fichiers,
 * sans une ligne à changer ni à recompiler.
 * ------------------------------------------------------------------------- */
static void
recenser_types (void)
{
    I.types = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    GSList *formats = gdk_pixbuf_get_formats ();
    for (GSList *l = formats; l != NULL; l = l->next) {
        GdkPixbufFormat *f = l->data;
        if (gdk_pixbuf_format_is_disabled (f))
            continue;
        g_auto(GStrv) mimes = gdk_pixbuf_format_get_mime_types (f);
        for (char **m = mimes; m != NULL && *m != NULL; m++)
            g_hash_table_add (I.types, g_strdup (*m));
    }
    g_slist_free (formats);
}

static gboolean
decodable (const char *type)
{
    if (type == NULL)
        return FALSE;
    if (g_hash_table_contains (I.types, type))
        return TRUE;

    /* Les alias : ce que GIO nomme d'une façon, gdk-pixbuf peut le déclarer
     * sous une autre — image/x-ms-bmp et image/bmp. */
    GHashTableIter it;
    gpointer cle;
    g_hash_table_iter_init (&it, I.types);
    while (g_hash_table_iter_next (&it, &cle, NULL))
        if (g_content_type_is_a (type, cle))
            return TRUE;
    return FALSE;
}

/* Le côté le plus long du plus grand écran branché, en pixels physiques.
 * Relu à chaque décodage : un écran externe branché en cours de route doit
 * profiter de sa définition dès l'image suivante. */
static int
limite_ecran (void)
{
    GListModel *ecrans = gdk_display_get_monitors (gdk_display_get_default ());
    int limite = 0;

    for (guint i = 0; i < g_list_model_get_n_items (ecrans); i++) {
        g_autoptr(GdkMonitor) m = g_list_model_get_item (ecrans, i);
        GdkRectangle g;
        gdk_monitor_get_geometry (m, &g);
        double e = gdk_monitor_get_scale (m);
        limite = MAX (limite, (int) ceil (MAX (g.width, g.height) * e));
    }
    return limite > 0 ? limite : LIMITE_DEFAUT;
}

/* -------------------------------------------------------------------------
 * Décodage, hors du fil principal
 * ------------------------------------------------------------------------- */
typedef struct {
    GFile *fichier;
    int    plafond;     /* côté le plus long à ne pas dépasser, 0 : aucun */
} Travail;

static void
travail_free (gpointer p)
{
    Travail *t = p;
    g_object_unref (t->fichier);
    g_free (t);
}

typedef struct {
    GdkTexture         *texture;
    GdkPixbufAnimation *anim;
    int                 largeur, hauteur;
} Resultat;

static void
resultat_free (gpointer p)
{
    Resultat *r = p;
    g_clear_object (&r->texture);
    g_clear_object (&r->anim);
    g_free (r);
}

typedef struct {
    int plafond;
    int largeur, hauteur;    /* celles du fichier, avant réduction */
} Taille;

/* Appelé par le chargeur dès qu'il a lu l'en-tête : c'est le seul moment où
 * l'on peut encore lui demander de décoder plus petit.
 *
 * LE PLUS PETIT DIVISEUR EN PUISSANCE DE DEUX qui fasse passer l'image sous
 * le plafond. La taille demandée est alors exactement celle que libjpeg
 * produit en réduisant pendant le décodage — ⌈w / d⌉ —, et gdk-pixbuf n'a
 * rien à rééchantillonner derrière lui. Voir la mesure en tête de fichier.
 *
 * Au-delà d'un huitième, on se résout à rééchantillonner : c'est le cas d'un
 * panorama de 40 000 pixels, pour lequel la mémoire compte plus que les
 * millisecondes. */
static void
on_taille (GdkPixbufLoader *ch, int w, int h, gpointer data)
{
    Taille *t = data;
    t->largeur = w;
    t->hauteur = h;

    int cote = MAX (w, h);
    if (t->plafond <= 0 || cote <= t->plafond)
        return;

    int d = 2;
    while (d < 8 && (cote + d - 1) / d > t->plafond)
        d *= 2;

    if ((cote + d - 1) / d <= t->plafond) {
        gdk_pixbuf_loader_set_size (ch, (w + d - 1) / d, (h + d - 1) / d);
    } else {
        double k = (double) t->plafond / cote;
        gdk_pixbuf_loader_set_size (ch, MAX (1, (int) round (w * k)),
                                        MAX (1, (int) round (h * k)));
    }
}

/* Les pixels du pixbuf DEVIENNENT ceux de la texture : le GBytes garde le
 * pixbuf en vie. gdk_texture_new_for_pixbuf, lui, les recopie — pour une
 * pleine résolution, 35 Mo de plus le temps de la conversion. */
static GdkTexture *
texture_sans_copie (GdkPixbuf *pb)
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

/* Ici au contraire on COPIE : l'itérateur d'une animation réutilise le même
 * pixbuf d'une image à l'autre, et la texture précédente changerait sous
 * les yeux du moteur de rendu. */
static GdkTexture *
texture_copiee (GdkPixbuf *pb)
{
    g_autoptr(GBytes) octets = g_bytes_new (gdk_pixbuf_read_pixels (pb),
                                            gdk_pixbuf_get_byte_length (pb));
    return gdk_memory_texture_new (
        gdk_pixbuf_get_width (pb), gdk_pixbuf_get_height (pb),
        gdk_pixbuf_get_has_alpha (pb) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
        octets, gdk_pixbuf_get_rowstride (pb));
}

/* UN CHARGEUR ALIMENTÉ À LA MAIN, plutôt que gdk_pixbuf_new_from_file :
 * c'est la seule voie qui réunit les trois choses voulues — décoder réduit
 * (« size-prepared »), s'interrompre quand on a changé d'image (le flux lit
 * par morceaux et consulte l'annulation entre deux), et récupérer
 * l'animation d'un GIF avec la même lecture. */
static void
decoder (GTask *tache, gpointer source, gpointer donnees, GCancellable *annul)
{
    (void) source;
    Travail *t = donnees;
    GError *err = NULL;

    g_autoptr(GFileInputStream) flux = g_file_read (t->fichier, annul, &err);
    if (flux == NULL) {
        g_task_return_error (tache, err);
        return;
    }

    g_autoptr(GdkPixbufLoader) ch = gdk_pixbuf_loader_new ();
    Taille taille = { .plafond = t->plafond };
    g_signal_connect (ch, "size-prepared", G_CALLBACK (on_taille), &taille);

    g_autofree guchar *tampon = g_malloc (TAMPON);
    gboolean lu = TRUE, ecrit = TRUE;
    for (;;) {
        gssize n = g_input_stream_read (G_INPUT_STREAM (flux), tampon, TAMPON,
                                        annul, &err);
        if (n < 0) { lu = FALSE; break; }
        if (n == 0) break;
        if (!gdk_pixbuf_loader_write (ch, tampon, (gsize) n, &err)) {
            ecrit = FALSE;
            break;
        }
    }

    /* Un échec d'écriture a DÉJÀ fermé le chargeur — gdk-pixbuf le fait
     * lui-même, et le refermer déclencherait un avertissement critique. Un
     * échec de lecture, non : il faut le fermer, sans quoi il avertit à sa
     * destruction. */
    if (!ecrit) {
        g_task_return_error (tache, err);
        return;
    }
    GError *err_fin = NULL;
    gboolean ferme = gdk_pixbuf_loader_close (ch, lu ? &err_fin : NULL);
    if (!lu) {
        g_task_return_error (tache, err);
        return;
    }
    if (!ferme) {
        g_task_return_error (tache, err_fin);
        return;
    }

    GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf (ch);
    if (pb == NULL) {
        g_task_return_new_error (tache, GDK_PIXBUF_ERROR,
                                 GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                 "Le fichier ne contient aucune image lisible.");
        return;
    }

    Resultat *r = g_new0 (Resultat, 1);
    GdkPixbufAnimation *anim = gdk_pixbuf_loader_get_animation (ch);
    if (anim != NULL && !gdk_pixbuf_animation_is_static_image (anim))
        r->anim = g_object_ref (anim);

    /* L'orientation EXIF est APPLIQUÉE aux pixels : une photo prise
     * téléphone en main s'affiche debout, comme dans la galerie qui l'a
     * prise. Les orientations 5 à 8 échangent largeur et hauteur. */
    g_autoptr(GdkPixbuf) droite = gdk_pixbuf_apply_embedded_orientation (pb);
    r->texture = texture_sans_copie (droite);

    const char *o = gdk_pixbuf_get_option (pb, "orientation");
    int orientation = o != NULL ? atoi (o) : 1;
    gboolean couchee = orientation >= 5 && orientation <= 8;
    r->largeur = couchee ? taille.hauteur : taille.largeur;
    r->hauteur = couchee ? taille.largeur : taille.hauteur;
    if (r->largeur <= 0 || r->hauteur <= 0) {
        r->largeur = gdk_pixbuf_get_width (droite);
        r->hauteur = gdk_pixbuf_get_height (droite);
    }

    g_task_return_pointer (tache, r, resultat_free);
}

/* -------------------------------------------------------------------------
 * Le cache : la courante et ses deux voisines
 * ------------------------------------------------------------------------- */
static Charge *
charge_ref (Charge *c)
{
    c->refs++;
    return c;
}

static void
charge_unref (Charge *c)
{
    if (c == NULL || --c->refs > 0)
        return;
    g_object_unref (c->fichier);
    g_clear_object (&c->texture);
    g_clear_object (&c->pleine);
    g_clear_object (&c->anim);
    g_clear_object (&c->annul);
    g_clear_object (&c->annul_pleine);
    g_free (c->erreur);
    g_free (c);
}

/* Fonction de libération du cache : sortir une charge du cache, c'est
 * l'abandonner — ses décodages sont interrompus, et leurs rappels sauront
 * qu'il ne faut plus rien en faire. */
static void
charge_abandonner (gpointer p)
{
    Charge *c = p;
    c->abandonnee = TRUE;
    if (c->annul != NULL)
        g_cancellable_cancel (c->annul);
    if (c->annul_pleine != NULL)
        g_cancellable_cancel (c->annul_pleine);
    charge_unref (c);
}

static Charge *
charge_trouver (GFile *f)
{
    for (guint i = 0; i < I.cache->len; i++) {
        Charge *c = g_ptr_array_index (I.cache, i);
        if (g_file_equal (c->fichier, f))
            return c;
    }
    return NULL;
}

static gboolean
est_courante (Charge *c)
{
    Entree *e = entree (I.index);
    return e != NULL && g_file_equal (e->fichier, c->fichier);
}

static void on_decodee (GObject *src, GAsyncResult *res, gpointer data);
static void on_pleine  (GObject *src, GAsyncResult *res, gpointer data);

static void
lancer_decodage (Charge *c, gboolean pleine, int priorite)
{
    Travail *t = g_new0 (Travail, 1);
    t->fichier = g_object_ref (c->fichier);
    /* Réduite : tout ce qui reste au-dessus de l'écran, jusqu'au double
     * exclu. C'est la marge que laisse une réduction par puissance de deux —
     * la moitié suivante tomberait sous l'écran, et l'image ajustée serait
     * floue. */
    t->plafond = pleine ? PLEINE_MAX : 2 * limite_ecran () - 1;

    GCancellable *annul = g_cancellable_new ();
    if (pleine)
        c->annul_pleine = annul;
    else
        c->annul = annul;

    GTask *tache = g_task_new (NULL, annul, pleine ? on_pleine : on_decodee,
                               charge_ref (c));
    g_task_set_task_data (tache, t, travail_free);
    /* La courante avant les voisines : la file des fils de GIO est ordonnée
     * par priorité, et c'est l'image qu'on regarde qui doit arriver d'abord. */
    g_task_set_priority (tache, priorite);
    g_task_run_in_thread (tache, decoder);
    g_object_unref (tache);
}

static Charge *
charge_obtenir (Entree *e, int priorite)
{
    Charge *c = charge_trouver (e->fichier);

    /* Réécrit depuis le décodage — une retouche enregistrée par-dessus, une
     * photo qui finit de se copier : on redécode. Zéro veut dire « date
     * inconnue », ce qui arrive au fichier ouvert avant que le dossier soit
     * lu ; il ne doit pas déclencher un second décodage de la même image. */
    if (c != NULL && c->modifie != 0 && e->modifie != 0 && c->modifie != e->modifie) {
        g_ptr_array_remove (I.cache, c);
        c = NULL;
    }
    if (c != NULL) {
        if (c->modifie == 0)
            c->modifie = e->modifie;
        return c;
    }

    c = g_new0 (Charge, 1);
    c->refs    = 1;
    c->fichier = g_object_ref (e->fichier);
    c->modifie = e->modifie;
    g_ptr_array_add (I.cache, c);
    lancer_decodage (c, FALSE, priorite);
    return c;
}

/* Hors de la fenêtre [courante - 1, courante + 1], rien ne reste. La pleine
 * résolution, elle, ne survit pas à l'image courante : c'est la plus lourde,
 * et l'on n'y revient qu'en zoomant de nouveau. */
static void
elaguer (void)
{
    for (guint i = I.cache->len; i-- > 0; ) {
        Charge *c = g_ptr_array_index (I.cache, i);
        int rang = 2;
        for (int s = -1; s <= 1; s++) {
            Entree *e = entree (I.index + s);
            if (e != NULL && g_file_equal (e->fichier, c->fichier))
                rang = s;
        }
        if (rang == 2) {
            g_ptr_array_remove_index_fast (I.cache, i);
            continue;
        }
        if (rang != 0) {
            if (c->annul_pleine != NULL)
                g_cancellable_cancel (c->annul_pleine);
            g_clear_object (&c->pleine);
        }
    }
}

/* -------------------------------------------------------------------------
 * Animations
 * ------------------------------------------------------------------------- */
static void
anim_arreter (void)
{
    g_clear_handle_id (&I.anim_id, g_source_remove);
    g_clear_object (&I.iter);
    g_clear_pointer (&I.anim_charge, charge_unref);
}

static gboolean anim_avancer (gpointer data);

static void
anim_programmer (void)
{
    int delai = gdk_pixbuf_animation_iter_get_delay_time (I.iter);
    if (delai < 0)
        return;          /* dernière image d'une animation qui ne boucle pas */
    /* Beaucoup de GIF déclarent 0 ou 10 ms, que les navigateurs traitent
     * comme 100 ms. Sans plancher, un seul GIF occuperait un cœur. */
    I.anim_id = g_timeout_add (delai < 20 ? 100 : delai, anim_avancer, NULL);
}

static gboolean
anim_avancer (gpointer data)
{
    (void) data;
    I.anim_id = 0;
    gdk_pixbuf_animation_iter_advance (I.iter, NULL);
    g_autoptr(GdkTexture) t = texture_copiee (gdk_pixbuf_animation_iter_get_pixbuf (I.iter));
    images_vue_set_texture (I.vue, t);
    anim_programmer ();
    return G_SOURCE_REMOVE;
}

static void
anim_demarrer (Charge *c)
{
    I.anim_charge = charge_ref (c);
    I.iter = gdk_pixbuf_animation_get_iter (c->anim, NULL);
    anim_programmer ();
}

/* -------------------------------------------------------------------------
 * Affichage de l'image courante
 * ------------------------------------------------------------------------- */
static gboolean
montrer_attente (gpointer data)
{
    (void) data;
    I.attente_id = 0;
    gtk_widget_set_visible (I.attente, TRUE);
    gtk_spinner_start (GTK_SPINNER (I.attente));
    return G_SOURCE_REMOVE;
}

static void
arreter_attente (void)
{
    g_clear_handle_id (&I.attente_id, g_source_remove);
    gtk_spinner_stop (GTK_SPINNER (I.attente));
    gtk_widget_set_visible (I.attente, FALSE);
}

static void
montrer_courante (Charge *c)
{
    anim_arreter ();
    arreter_attente ();
    gtk_widget_set_visible (I.erreur, FALSE);

    if (I.affichee != c) {
        g_clear_pointer (&I.affichee, charge_unref);
        I.affichee = charge_ref (c);
    }

    if (c->texture != NULL) {
        images_vue_set_image (I.vue, c->pleine != NULL ? c->pleine : c->texture,
                              c->largeur, c->hauteur);
        if (c->anim != NULL)
            anim_demarrer (c);
    } else {
        images_vue_set_image (I.vue, NULL, 0, 0);
        if (c->erreur != NULL) {
            gtk_label_set_text (GTK_LABEL (I.erreur_detail), c->erreur);
            gtk_widget_set_visible (I.erreur, TRUE);
        } else {
            I.attente_id = g_timeout_add (DELAI_ATTENTE, montrer_attente, NULL);
        }
    }
    maj_zoom ();
    maj_etat ();
}

static void
poser_voisines (void)
{
    for (int s = -1; s <= 1; s += 2) {
        Entree *n = entree (I.index + s);
        Charge *c = n != NULL ? charge_obtenir (n, G_PRIORITY_LOW) : NULL;
        images_vue_set_voisine (I.vue, s, n != NULL,
                                c != NULL ? c->texture : NULL,
                                c != NULL ? c->largeur : 0,
                                c != NULL ? c->hauteur : 0);
    }
}

static void
charge_prete (Charge *c)
{
    if (I.fenetre == NULL)
        return;
    if (est_courante (c)) {
        montrer_courante (c);
        return;
    }
    for (int s = -1; s <= 1; s += 2) {
        Entree *n = entree (I.index + s);
        if (n != NULL && g_file_equal (n->fichier, c->fichier))
            images_vue_set_voisine (I.vue, s, TRUE, c->texture, c->largeur, c->hauteur);
    }
}

static void
on_decodee (GObject *src, GAsyncResult *res, gpointer data)
{
    (void) src;
    Charge *c = data;
    g_autoptr(GError) err = NULL;
    Resultat *r = g_task_propagate_pointer (G_TASK (res), &err);
    g_clear_object (&c->annul);

    if (!c->abandonnee) {
        if (r != NULL) {
            c->texture = g_steal_pointer (&r->texture);
            c->anim    = g_steal_pointer (&r->anim);
            c->largeur = r->largeur;
            c->hauteur = r->hauteur;
            charge_prete (c);
        } else if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            c->erreur = g_strdup (err->message);
            g_message ("« %s » illisible : %s",
                       g_file_peek_path (c->fichier), err->message);
            charge_prete (c);
        }
    }
    g_clear_pointer (&r, resultat_free);
    charge_unref (c);
}

/* Faut-il la pleine résolution ? Posé à chaque changement de zoom. */
static void
peut_etre_pleine (void)
{
    Charge *c = I.affichee;
    if (c == NULL || c->texture == NULL || c->pleine != NULL
        || c->annul_pleine != NULL || c->anim != NULL || !est_courante (c))
        return;
    if (images_vue_manque_de_details (I.vue))
        lancer_decodage (c, TRUE, G_PRIORITY_DEFAULT);
}

static void
on_pleine (GObject *src, GAsyncResult *res, gpointer data)
{
    (void) src;
    Charge *c = data;
    g_autoptr(GError) err = NULL;
    Resultat *r = g_task_propagate_pointer (G_TASK (res), &err);
    g_clear_object (&c->annul_pleine);

    if (r != NULL && !c->abandonnee && est_courante (c) && c == I.affichee) {
        g_set_object (&c->pleine, r->texture);
        images_vue_set_texture (I.vue, c->pleine);
    } else if (r == NULL && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_message ("pleine résolution de « %s » impossible : %s",
                   g_file_peek_path (c->fichier), err->message);
    } else if (r == NULL && !c->abandonnee && c == I.affichee) {
        /* Annulée parce qu'on était passé à une autre image, et l'on est
         * revenu entre-temps : la demande qui a pu être refusée pendant
         * qu'elle s'achevait est reposée ici. */
        peut_etre_pleine ();
    }
    g_clear_pointer (&r, resultat_free);
    charge_unref (c);
}

/* -------------------------------------------------------------------------
 * Navigation
 * ------------------------------------------------------------------------- */
static void
maj_vide (void)
{
    gboolean vide = nombre () == 0 && !I.ouverture;
    gtk_widget_set_visible (I.vide, vide);
    if (vide)
        gtk_label_set_text (GTK_LABEL (I.vide_detail), I.dossier != NULL
            ? "Ce dossier ne contient aucune image que cette visionneuse sache afficher."
            : "Ouvrez une image depuis Fichiers, glissez-la ici, ou choisissez-la.");
}

/* Remet l'affichage en accord avec I.index et I.entrees. Ne réaffiche
 * l'image courante QUE si elle a changé : une relecture du dossier — une
 * photo ajoutée à côté — ne doit ni ramener le zoom à l'ajustement ni
 * relancer un GIF depuis le début. */
static void
rafraichir (void)
{
    Entree *e = entree (I.index);
    maj_vide ();

    if (e == NULL) {
        anim_arreter ();
        arreter_attente ();
        g_clear_pointer (&I.affichee, charge_unref);
        gtk_widget_set_visible (I.erreur, FALSE);
        images_vue_set_image (I.vue, NULL, 0, 0);
        images_vue_set_voisine (I.vue, -1, FALSE, NULL, 0, 0);
        images_vue_set_voisine (I.vue, +1, FALSE, NULL, 0, 0);
    } else {
        Charge *c = charge_obtenir (e, G_PRIORITY_DEFAULT);
        if (c != I.affichee)
            montrer_courante (c);
        poser_voisines ();
    }
    elaguer ();
    maj_barre ();
}

static void
aller (int i)
{
    if (i < 0 || i >= nombre () || i == I.index)
        return;
    I.index = i;
    rafraichir ();
}

/* -------------------------------------------------------------------------
 * Lecture du dossier
 * ------------------------------------------------------------------------- */
#define ATTRIBUTS "standard::name,standard::display-name,standard::type," \
                  "standard::fast-content-type,standard::is-hidden,"      \
                  "standard::is-backup,standard::size,time::modified"

typedef struct {
    GFile           *dossier;
    GFile           *voulu;     /* à montrer une fois la liste lue       */
    gboolean         nouveau;   /* dossier qu'on vient d'ouvrir          */
    GFileEnumerator *en;
    GPtrArray       *entrees;
    GCancellable    *annul;
} Lecture;

static void
lecture_free (Lecture *l)
{
    g_clear_object (&l->dossier);
    g_clear_object (&l->voulu);
    g_clear_object (&l->en);
    g_clear_pointer (&l->entrees, g_ptr_array_unref);
    g_clear_object (&l->annul);
    g_free (l);
}

static Entree *
entree_depuis (GFile *f, GFileInfo *info)
{
    Entree *e = g_new0 (Entree, 1);
    e->fichier = g_object_ref (f);
    e->nom     = g_strdup (g_file_info_get_display_name (info));
    /* collate_key_for_filename, comme Fichiers : « photo2 » avant
     * « photo10 ». L'ordre des images est celui du dossier qu'on vient de
     * quitter, sinon « suivante » ne voudrait rien dire. */
    e->cle     = g_utf8_collate_key_for_filename (e->nom, -1);
    e->type    = g_strdup (g_file_info_get_attribute_string (
                     info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE));
    e->taille  = g_file_info_get_size (info);
    if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
        e->modifie = (gint64) g_file_info_get_attribute_uint64 (
                         info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    return e;
}

static int
cmp_entrees (gconstpointer a, gconstpointer b)
{
    const Entree *x = *(Entree * const *) a;
    const Entree *y = *(Entree * const *) b;
    return strcmp (x->cle, y->cle);
}

static void
adopter_liste (Lecture *l)
{
    /* Ce qu'on regardait, pour le retrouver dans la liste neuve. */
    g_autoptr(GFile) cible = NULL;
    if (l->voulu != NULL)
        cible = g_object_ref (l->voulu);
    else if (!l->nouveau && entree (I.index) != NULL)
        cible = g_object_ref (entree (I.index)->fichier);
    int ancien = I.index;

    g_ptr_array_sort (l->entrees, cmp_entrees);
    g_clear_pointer (&I.entrees, g_ptr_array_unref);
    I.entrees = g_steal_pointer (&l->entrees);
    I.ouverture = FALSE;

    int trouve = -1;
    for (int i = 0; cible != NULL && i < nombre (); i++)
        if (g_file_equal (entree (i)->fichier, cible)) {
            trouve = i;
            break;
        }
    /* L'image courante a disparu du dossier : on reste au même rang, ce qui
     * montre celle qui la suivait — le geste attendu après une suppression. */
    if (trouve < 0 && nombre () > 0)
        trouve = l->nouveau ? 0 : CLAMP (ancien, 0, nombre () - 1);

    I.index = trouve;
    rafraichir ();
}

static void
lecture_finie (Lecture *l, GError *err)
{
    if (!g_cancellable_is_cancelled (l->annul) && I.fenetre != NULL) {
        if (err != NULL)
            g_message ("lecture de « %s » impossible : %s",
                       g_file_peek_path (l->dossier), err->message);
        /* Un dossier illisible — droits, lecteur réseau tombé — ne doit pas
         * effacer l'image qu'on a pu ouvrir quand même. */
        if (err == NULL || l->entrees->len > 0)
            adopter_liste (l);
        else {
            I.ouverture = FALSE;
            maj_vide ();
        }
        if (I.annul_liste == l->annul)
            g_clear_object (&I.annul_liste);
    }
    lecture_free (l);
}

static void
on_lot (GObject *src, GAsyncResult *res, gpointer data)
{
    Lecture *l = data;
    g_autoptr(GError) err = NULL;
    GList *infos = g_file_enumerator_next_files_finish (G_FILE_ENUMERATOR (src), res, &err);

    if (err != NULL) {
        lecture_finie (l, err);
        return;
    }
    if (infos == NULL) {
        lecture_finie (l, NULL);
        return;
    }

    for (GList *i = infos; i != NULL; i = i->next) {
        GFileInfo *info = i->data;
        if (g_file_info_get_file_type (info) != G_FILE_TYPE_REGULAR)
            continue;

        g_autoptr(GFile) f = g_file_enumerator_get_child (l->en, info);
        gboolean voulu = l->voulu != NULL && g_file_equal (f, l->voulu);

        /* Le fichier qu'on a demandé entre toujours dans la liste, même
         * caché ou mal nommé : c'est lui qu'on veut voir. Les autres doivent
         * être visibles, et d'un type qu'on sait décoder. */
        if (!voulu) {
            if (g_file_info_get_is_hidden (info) || g_file_info_get_is_backup (info))
                continue;
            if (!decodable (g_file_info_get_attribute_string (
                                info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE)))
                continue;
        }
        g_ptr_array_add (l->entrees, entree_depuis (f, info));
    }
    g_list_free_full (infos, g_object_unref);

    g_file_enumerator_next_files_async (l->en, 256, G_PRIORITY_DEFAULT,
                                        l->annul, on_lot, l);
}

static void
on_enumerateur (GObject *src, GAsyncResult *res, gpointer data)
{
    Lecture *l = data;
    g_autoptr(GError) err = NULL;
    l->en = g_file_enumerate_children_finish (G_FILE (src), res, &err);
    if (l->en == NULL) {
        lecture_finie (l, err);
        return;
    }
    g_file_enumerator_next_files_async (l->en, 256, G_PRIORITY_DEFAULT,
                                        l->annul, on_lot, l);
}

/* ASYNCHRONE DE BOUT EN BOUT. Un dossier sur le NAS dont le serveur vient de
 * s'éteindre fait attendre chaque appel jusqu'au délai TCP : en synchrone, la
 * fenêtre gèlerait au moment précis où l'on veut la fermer. Fichiers l'a
 * appris à ses dépens avec g_file_query_exists. */
static void
lire_dossier (GFile *dossier, GFile *voulu, gboolean nouveau)
{
    if (I.annul_liste != NULL) {
        g_cancellable_cancel (I.annul_liste);
        g_clear_object (&I.annul_liste);
    }
    I.annul_liste = g_cancellable_new ();

    Lecture *l = g_new0 (Lecture, 1);
    l->dossier = g_object_ref (dossier);
    l->voulu   = voulu != NULL ? g_object_ref (voulu) : NULL;
    l->nouveau = nouveau;
    l->entrees = g_ptr_array_new_with_free_func (entree_free);
    l->annul   = g_object_ref (I.annul_liste);

    g_file_enumerate_children_async (dossier, ATTRIBUTS, G_FILE_QUERY_INFO_NONE,
                                     G_PRIORITY_DEFAULT, l->annul,
                                     on_enumerateur, l);
}

static gboolean
relire (gpointer data)
{
    (void) data;
    I.relire_id = 0;
    if (I.dossier != NULL)
        lire_dossier (I.dossier, NULL, FALSE);
    return G_SOURCE_REMOVE;
}

/* Une photo arrive, une autre est supprimée depuis Fichiers : la liste suit.
 * Relue d'un bloc après un temps de calme — une copie de trois cents photos
 * produit des milliers d'événements, et l'on ne relit qu'une fois à la
 * fin. */
static void
on_dossier_change (GFileMonitor *m, GFile *f, GFile *autre,
                   GFileMonitorEvent ev, gpointer data)
{
    (void) m; (void) f; (void) autre; (void) data;
    if (ev == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
        return;          /* des droits changés ne changent pas l'image */
    g_clear_handle_id (&I.relire_id, g_source_remove);
    I.relire_id = g_timeout_add (DELAI_RELECTURE, relire, NULL);
}

static void
surveiller (GFile *dossier)
{
    if (I.moniteur != NULL) {
        g_signal_handlers_disconnect_by_func (I.moniteur, on_dossier_change, NULL);
        g_file_monitor_cancel (I.moniteur);
        g_clear_object (&I.moniteur);
    }
    g_autoptr(GError) err = NULL;
    I.moniteur = g_file_monitor_directory (dossier, G_FILE_MONITOR_WATCH_MOVES,
                                           NULL, &err);
    if (I.moniteur == NULL) {
        g_message ("surveillance de « %s » impossible : %s",
                   g_file_peek_path (dossier), err->message);
        return;
    }
    g_signal_connect (I.moniteur, "changed", G_CALLBACK (on_dossier_change), NULL);
}

static void
changer_dossier (GFile *dossier)
{
    if (I.dossier != NULL && g_file_equal (I.dossier, dossier))
        return;
    g_set_object (&I.dossier, dossier);
    surveiller (dossier);
}

/* -------------------------------------------------------------------------
 * Ouvrir
 * ------------------------------------------------------------------------- */
static void
on_ouvrir_info (GObject *src, GAsyncResult *res, gpointer data)
{
    (void) data;
    GFile *f = G_FILE (src);
    g_autoptr(GError) err = NULL;
    g_autoptr(GFileInfo) info = g_file_query_info_finish (f, res, &err);

    if (I.fenetre == NULL)
        return;

    if (info == NULL) {
        I.ouverture = FALSE;
        g_message ("ouverture de « %s » impossible : %s",
                   g_file_peek_path (f), err->message);
        /* Rien d'autre à l'écran : on dit pourquoi. Une image déjà ouverte,
         * elle, reste en place — un glisser-déposer raté ne doit pas
         * effacer ce qu'on regardait. */
        if (nombre () == 0) {
            gtk_label_set_text (GTK_LABEL (I.erreur_detail), err->message);
            gtk_widget_set_visible (I.erreur, TRUE);
        }
        return;
    }

    if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
        changer_dossier (f);
        lire_dossier (f, NULL, TRUE);
        return;
    }

    g_autoptr(GFile) parent = g_file_get_parent (f);

    /* Même dossier, image déjà connue : on y va, sans rien relire. C'est le
     * cas d'un second double-clic dans la même fenêtre de Fichiers. */
    if (parent != NULL && I.dossier != NULL && g_file_equal (parent, I.dossier)) {
        for (int i = 0; i < nombre (); i++)
            if (g_file_equal (entree (i)->fichier, f)) {
                I.ouverture = FALSE;
                aller (i);
                return;
            }
    }

    /* L'IMAGE D'ABORD, LE DOSSIER ENSUITE. Une liste provisoire d'une seule
     * entrée fait partir le décodage tout de suite ; la lecture du dossier
     * suit en parallèle et remplacera la liste en retrouvant l'image. Sur un
     * dossier de milliers de photos, ou sur le NAS, on n'attend pas
     * l'énumération pour voir ce qu'on a demandé. */
    GPtrArray *seule = g_ptr_array_new_with_free_func (entree_free);
    g_ptr_array_add (seule, entree_depuis (f, info));
    g_clear_pointer (&I.entrees, g_ptr_array_unref);
    I.entrees = seule;
    I.index = 0;
    rafraichir ();

    if (parent != NULL) {
        changer_dossier (parent);
        lire_dossier (parent, f, TRUE);
    } else {
        I.ouverture = FALSE;
    }
}

static void
ouvrir (GFile *f)
{
    if (I.fenetre == NULL)
        return;
    I.ouverture = TRUE;
    gtk_widget_set_visible (I.erreur, FALSE);
    maj_vide ();
    g_file_query_info_async (f, ATTRIBUTS, G_FILE_QUERY_INFO_NONE,
                             G_PRIORITY_DEFAULT, NULL, on_ouvrir_info, NULL);
}

/* -------------------------------------------------------------------------
 * Barres
 * ------------------------------------------------------------------------- */
static void
maj_barre (void)
{
    Entree *e = entree (I.index);
    int n = nombre ();

    gtk_label_set_text (GTK_LABEL (I.nom), e != NULL ? e->nom : "");

    g_autofree char *pos = e != NULL ? g_strdup_printf ("%d / %d", I.index + 1, n)
                                     : g_strdup ("");
    gtk_label_set_text (GTK_LABEL (I.position), pos);

    g_autofree char *titre = e != NULL ? g_strdup_printf ("%s — Images", e->nom)
                                       : g_strdup ("Images");
    gtk_window_set_title (GTK_WINDOW (I.fenetre), titre);

    g_autofree char *pilule = e != NULL ? g_strdup_printf ("%s   ·   %s", e->nom, pos)
                                        : g_strdup ("");
    gtk_label_set_text (GTK_LABEL (I.pilule_texte), pilule);

    activer ("precedent", I.index > 0);
    activer ("premiere",  I.index > 0);
    activer ("suivant",   e != NULL && I.index < n - 1);
    activer ("derniere",  e != NULL && I.index < n - 1);
    activer ("dossier",   I.dossier != NULL);

    /* Les flèches posées sur l'image suivent la liste sans relancer leur
     * minuterie : avancer au clavier ne doit pas les faire apparaître. */
    montrer_controles (I.controles);
}

static void
maj_zoom (void)
{
    gboolean image = I.affichee != NULL && I.affichee->texture != NULL;
    double z = images_vue_get_zoom (I.vue);

    /* Une espace insécable avant « % », comme le veut la typographie
     * française, et qui empêche le signe de passer seul à la ligne. */
    g_autofree char *t = image ? g_strdup_printf ("%.0f\u00a0%%", MAX (1.0, round (z * 100.0)))
                               : g_strdup ("—");
    gtk_button_set_label (GTK_BUTTON (I.zoom), t);

    activer ("zoom-plus",      image && z < 15.99);
    activer ("zoom-moins",     image && !images_vue_est_ajustee (I.vue));
    activer ("zoom-ajuste",    image);
    activer ("zoom-reel",      image);
    activer ("zoom-bascule",   image);
    activer ("pivoter-gauche", image);
    activer ("pivoter-droite", image);
}

static char *
chemin_court (GFile *f)
{
    g_autofree char *p = g_file_get_parse_name (f);
    const char *maison = g_get_home_dir ();
    size_t n = strlen (maison);
    if (g_str_has_prefix (p, maison) && (p[n] == '/' || p[n] == '\0'))
        return g_strconcat ("~", p + n, NULL);
    return g_steal_pointer (&p);
}

static void
maj_etat (void)
{
    Entree *e = entree (I.index);
    Charge *c = I.affichee;
    GString *s = g_string_new (NULL);

    if (e != NULL) {
        const char *sep = "   ·   ";
        if (c != NULL && c->largeur > 0)
            g_string_append_printf (s, "%d × %d", c->largeur, c->hauteur);
        if (e->taille > 0) {
            g_autofree char *t = g_format_size (e->taille);
            g_string_append_printf (s, "%s%s", s->len ? sep : "", t);
        }
        if (e->type != NULL) {
            g_autofree char *d = g_content_type_get_description (e->type);
            g_string_append_printf (s, "%s%s", s->len ? sep : "", d);
        }
        if (e->modifie > 0) {
            /* Le format de la colonne « Modifié le » de Fichiers. */
            g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local (e->modifie);
            g_autofree char *d = g_date_time_format (dt, "%d/%m/%Y %H:%M");
            g_string_append_printf (s, "%s%s", s->len ? sep : "", d);
        }
    }
    gtk_label_set_text (GTK_LABEL (I.etat), s->str);
    g_string_free (s, TRUE);

    g_autofree char *d = I.dossier != NULL ? chemin_court (I.dossier) : g_strdup ("");
    gtk_label_set_text (GTK_LABEL (I.etat_dossier), d);
}

/* -------------------------------------------------------------------------
 * Commandes posées sur l'image
 *
 * PAS DE GtkRevealer ICI. Un révélateur replié garde sa place, et il reste
 * une cible pour le pointeur tant qu'il est « can-target » — ce qu'il doit
 * être pour que son bouton le soit. Un balayage commencé au bord de l'image
 * tomberait sur la flèche invisible, et rien ne glisserait. On fond donc le
 * bouton lui-même par son opacité, et on le retire des cibles.
 * ------------------------------------------------------------------------- */
static void
reveler (GtkWidget *w, gboolean montrer)
{
    if (montrer)
        gtk_widget_remove_css_class (w, "cachee");
    else
        gtk_widget_add_css_class (w, "cachee");
    gtk_widget_set_can_target (w, montrer);
}

static gboolean
pointeur_sur (GtkWidget *w)
{
    graphene_rect_t r;
    if (!gtk_widget_get_can_target (w)
        || !gtk_widget_compute_bounds (w, I.fenetre, &r))
        return FALSE;
    return graphene_rect_contains_point (&r, &GRAPHENE_POINT_INIT ((float) I.mx, (float) I.my));
}

static gboolean
on_masquer (gpointer data)
{
    (void) data;
    I.masquer_id = 0;
    /* On ne retire pas une flèche de sous le pointeur qui la vise. */
    if (pointeur_sur (I.fleche_g) || pointeur_sur (I.fleche_d) || pointeur_sur (I.pilule)) {
        I.masquer_id = g_timeout_add (DELAI_MASQUAGE, on_masquer, NULL);
        return G_SOURCE_REMOVE;
    }
    montrer_controles (FALSE);
    return G_SOURCE_REMOVE;
}

static void
montrer_controles (gboolean montrer)
{
    gboolean relancer = montrer && !I.controles;
    I.controles = montrer;

    gboolean image = entree (I.index) != NULL;
    reveler (I.fleche_g, montrer && I.index > 0);
    reveler (I.fleche_d, montrer && image && I.index < nombre () - 1);
    reveler (I.pilule,   montrer && I.plein && image);
    images_vue_masquer_curseur (I.vue, !montrer && I.plein);

    if (!montrer)
        g_clear_handle_id (&I.masquer_id, g_source_remove);
    else if (relancer || I.masquer_id == 0)
        I.masquer_id = g_timeout_add (DELAI_MASQUAGE, on_masquer, NULL);
}

/* Toute activité remet les commandes à l'écran, et repousse leur départ. */
static void
reveiller (void)
{
    g_clear_handle_id (&I.masquer_id, g_source_remove);
    I.controles = FALSE;
    montrer_controles (TRUE);
}

static void
on_mouvement (GtkEventControllerMotion *m, double x, double y, gpointer data)
{
    (void) m; (void) data;
    /* GTK émet aussi des mouvements immobiles — à l'entrée dans la fenêtre,
     * après une mise en page. Sans ce seuil, les commandes ne partiraient
     * jamais en plein écran. */
    if (fabs (x - I.mx) + fabs (y - I.my) < 3.0)
        return;
    I.mx = x;
    I.my = y;
    reveiller ();
}

/* -------------------------------------------------------------------------
 * Plein écran
 * ------------------------------------------------------------------------- */
static void
on_plein_ecran (GObject *o, GParamSpec *p, gpointer data)
{
    (void) o; (void) p; (void) data;

    I.plein = gtk_window_is_fullscreen (GTK_WINDOW (I.fenetre));
    gtk_revealer_set_reveal_child (GTK_REVEALER (I.revele_barre), !I.plein);
    gtk_revealer_set_reveal_child (GTK_REVEALER (I.revele_etat),  !I.plein);

    if (I.plein)
        gtk_widget_add_css_class (I.fenetre, "plein-ecran");
    else
        gtk_widget_remove_css_class (I.fenetre, "plein-ecran");

    gtk_button_set_icon_name (GTK_BUTTON (I.b_plein),
        I.plein ? "view-restore-symbolic" : "view-fullscreen-symbolic");
    gtk_widget_set_tooltip_text (I.b_plein,
        I.plein ? "Quitter le plein écran (Échap)" : "Plein écran (F11)");

    /* On montre tout, puis tout s'efface : c'est ainsi qu'on apprend où
     * sont les commandes sans qu'elles restent devant la photo. */
    reveiller ();
}

/* -------------------------------------------------------------------------
 * Actions
 *
 * Portées par l'APPLICATION, pas par la fenêtre : GtkApplication les publie
 * alors sur le bus de session. Le banc d'essai s'en sert pour piloter la
 * visionneuse sans clavier ni souris — voir les commandes gdbus du README.
 * ------------------------------------------------------------------------- */
static void
act_precedent (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    aller (I.index - 1);
}

static void
act_suivant (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    aller (I.index + 1);
}

static void
act_premiere (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    aller (0);
}

static void
act_derniere (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    aller (nombre () - 1);
}

static void
act_plein_ecran (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    if (gtk_window_is_fullscreen (GTK_WINDOW (I.fenetre)))
        gtk_window_unfullscreen (GTK_WINDOW (I.fenetre));
    else
        gtk_window_fullscreen (GTK_WINDOW (I.fenetre));
}

static void
act_quitter_plein (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    gtk_window_unfullscreen (GTK_WINDOW (I.fenetre));
}

static void
act_zoom_plus (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    images_vue_zoomer (I.vue, PAS_ZOOM);
}

static void
act_zoom_moins (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    images_vue_zoomer (I.vue, 1.0 / PAS_ZOOM);
}

static void
act_zoom_ajuste (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    images_vue_zoom_ajuste (I.vue);
}

static void
act_zoom_reel (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    images_vue_zoom_reel (I.vue);
}

/* Le pourcentage de la barre est aussi un bouton : il passe de l'image
 * entière à la taille réelle, et retour. C'est le zoom qu'on veut neuf fois
 * sur dix, en un seul geste. */
static void
act_zoom_bascule (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    if (images_vue_est_ajustee (I.vue))
        images_vue_zoom_reel (I.vue);
    else
        images_vue_zoom_ajuste (I.vue);
}

static void
act_pivoter_gauche (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    images_vue_pivoter (I.vue, -1);
}

static void
act_pivoter_droite (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    images_vue_pivoter (I.vue, +1);
}

/* « Afficher dans Fichiers » : le gestionnaire de fichiers de Claude OS,
 * nommément. Le type inode/directory pourrait être revendiqué par un
 * navigateur, qui ouvrirait alors un listing HTML du dossier. */
static void
act_dossier (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    if (I.dossier == NULL)
        return;

    g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new ("os.claude.shell.fichiers.desktop");
    if (info == NULL) {
        g_message ("os.claude.shell.fichiers.desktop introuvable : Fichiers n'est pas installé");
        return;
    }
    GList un = { .data = I.dossier, .next = NULL, .prev = NULL };
    g_autoptr(GAppLaunchContext) ctx = G_APP_LAUNCH_CONTEXT (
        gdk_display_get_app_launch_context (gdk_display_get_default ()));
    g_autoptr(GError) err = NULL;
    if (!g_app_info_launch (G_APP_INFO (info), &un, ctx, &err))
        g_message ("lancement de Fichiers impossible : %s", err->message);
}

static void
on_choisie (GObject *src, GAsyncResult *res, gpointer data)
{
    (void) data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GFile) f = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (src), res, &err);
    if (f != NULL)
        ouvrir (f);
    else if (err != NULL && !g_error_matches (err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
        g_message ("choix d'une image impossible : %s", err->message);
}

static void
act_ouvrir (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;

    g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dlg, "Ouvrir une image");

    g_autoptr(GtkFileFilter) filtre = gtk_file_filter_new ();
    gtk_file_filter_set_name (filtre, "Images");
    GHashTableIter it;
    gpointer type;
    g_hash_table_iter_init (&it, I.types);
    while (g_hash_table_iter_next (&it, &type, NULL))
        gtk_file_filter_add_mime_type (filtre, type);

    g_autoptr(GListStore) filtres = g_list_store_new (GTK_TYPE_FILE_FILTER);
    g_list_store_append (filtres, filtre);
    gtk_file_dialog_set_filters (dlg, G_LIST_MODEL (filtres));
    gtk_file_dialog_set_default_filter (dlg, filtre);

    /* On part d'où l'on est ; à défaut, du dossier Images de l'utilisateur. */
    g_autoptr(GFile) depart = NULL;
    if (I.dossier != NULL)
        depart = g_object_ref (I.dossier);
    else if (g_get_user_special_dir (G_USER_DIRECTORY_PICTURES) != NULL)
        depart = g_file_new_for_path (g_get_user_special_dir (G_USER_DIRECTORY_PICTURES));
    if (depart != NULL)
        gtk_file_dialog_set_initial_folder (dlg, depart);

    gtk_file_dialog_open (dlg, GTK_WINDOW (I.fenetre), NULL, on_choisie, NULL);
}

static void
act_fermer (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    if (I.fenetre != NULL)
        gtk_window_destroy (GTK_WINDOW (I.fenetre));
}

static const GActionEntry actions[] = {
    { "precedent",           act_precedent,      NULL, NULL, NULL, { 0 } },
    { "suivant",             act_suivant,        NULL, NULL, NULL, { 0 } },
    { "premiere",            act_premiere,       NULL, NULL, NULL, { 0 } },
    { "derniere",            act_derniere,       NULL, NULL, NULL, { 0 } },
    { "plein-ecran",         act_plein_ecran,    NULL, NULL, NULL, { 0 } },
    { "quitter-plein-ecran", act_quitter_plein,  NULL, NULL, NULL, { 0 } },
    { "zoom-plus",           act_zoom_plus,      NULL, NULL, NULL, { 0 } },
    { "zoom-moins",          act_zoom_moins,     NULL, NULL, NULL, { 0 } },
    { "zoom-ajuste",         act_zoom_ajuste,    NULL, NULL, NULL, { 0 } },
    { "zoom-reel",           act_zoom_reel,      NULL, NULL, NULL, { 0 } },
    { "zoom-bascule",        act_zoom_bascule,   NULL, NULL, NULL, { 0 } },
    { "pivoter-gauche",      act_pivoter_gauche, NULL, NULL, NULL, { 0 } },
    { "pivoter-droite",      act_pivoter_droite, NULL, NULL, NULL, { 0 } },
    { "dossier",             act_dossier,        NULL, NULL, NULL, { 0 } },
    { "ouvrir",              act_ouvrir,         NULL, NULL, NULL, { 0 } },
    { "fermer",              act_fermer,         NULL, NULL, NULL, { 0 } },
};

/* -------------------------------------------------------------------------
 * Clavier
 *
 * EN PHASE DE CAPTURE, avant les boutons. Sans cela, une flèche pressée
 * pendant qu'un bouton de la barre a le focus déplacerait ce focus au lieu
 * de changer d'image — et Espace « cliquerait » le bouton.
 * ------------------------------------------------------------------------- */
static gboolean
on_touche (GtkEventControllerKey *k, guint val, guint code,
           GdkModifierType mods, gpointer data)
{
    (void) k; (void) code; (void) data;

    mods &= gtk_accelerator_get_default_mod_mask ();
    /* Alt et Super appartiennent au compositeur et au système. */
    if (mods & (GDK_ALT_MASK | GDK_SUPER_MASK))
        return GDK_EVENT_PROPAGATE;
    gboolean ctrl = (mods & GDK_CONTROL_MASK) != 0;

    const char *action = NULL;
    switch (val) {
    case GDK_KEY_Left:  case GDK_KEY_KP_Left:
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up:
    case GDK_KEY_BackSpace:
        action = ctrl ? NULL : "precedent";
        break;
    case GDK_KEY_Right: case GDK_KEY_KP_Right:
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down:
    case GDK_KEY_space:
        action = ctrl ? NULL : "suivant";
        break;
    case GDK_KEY_Home: case GDK_KEY_KP_Home:
        action = "premiere";
        break;
    case GDK_KEY_End: case GDK_KEY_KP_End:
        action = "derniere";
        break;
    case GDK_KEY_F11: case GDK_KEY_f: case GDK_KEY_F:
        action = "plein-ecran";
        break;
    case GDK_KEY_Escape:
        action = I.plein ? "quitter-plein-ecran" : NULL;
        break;
    case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
        action = "zoom-plus";
        break;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
        action = "zoom-moins";
        break;
    /* CLAVIER AZERTY : les chiffres de la rangée du haut demandent Maj.
     * « à » et « & » sont ce que donnent ces deux touches sans elle —
     * celles qu'on presse en pensant « 0 » et « 1 ». */
    case GDK_KEY_0: case GDK_KEY_KP_0: case GDK_KEY_agrave:
        action = "zoom-ajuste";
        break;
    case GDK_KEY_1: case GDK_KEY_KP_1: case GDK_KEY_ampersand:
        action = "zoom-reel";
        break;
    case GDK_KEY_r:
        action = ctrl ? NULL : "pivoter-droite";
        break;
    case GDK_KEY_R:
        action = ctrl ? NULL : "pivoter-gauche";
        break;
    default:
        break;
    }

    if (action == NULL)
        return GDK_EVENT_PROPAGATE;
    g_action_group_activate_action (G_ACTION_GROUP (I.app), action, NULL);
    return GDK_EVENT_STOP;
}

/* -------------------------------------------------------------------------
 * Signaux de la toile
 * ------------------------------------------------------------------------- */
static void
on_naviguer (ImagesVue *v, int sens, gpointer data)
{
    (void) v; (void) data;
    aller (I.index + sens);
}

static void
on_zoom_change (ImagesVue *v, gpointer data)
{
    (void) v; (void) data;
    maj_zoom ();
    peut_etre_pleine ();
}

/* Un appui du doigt montre les commandes, un second les retire. À la
 * souris, le seul mouvement du pointeur suffit ; voir images-vue.h. */
static void
on_touchee (ImagesVue *v, gpointer data)
{
    (void) v; (void) data;
    if (I.controles)
        montrer_controles (FALSE);
    else
        reveiller ();
}

static void
on_double (ImagesVue *v, gpointer data)
{
    (void) v; (void) data;
    g_action_group_activate_action (G_ACTION_GROUP (I.app), "plein-ecran", NULL);
}

static gboolean
on_depot (GtkDropTarget *t, const GValue *val, double x, double y, gpointer data)
{
    (void) t; (void) x; (void) y; (void) data;
    if (!G_VALUE_HOLDS (val, GDK_TYPE_FILE_LIST))
        return FALSE;
    GSList *fichiers = gdk_file_list_get_files (g_value_get_boxed (val));
    if (fichiers == NULL)
        return FALSE;
    ouvrir (G_FILE (fichiers->data));
    g_slist_free (fichiers);
    return TRUE;
}

/* -------------------------------------------------------------------------
 * Construction de la fenêtre
 * ------------------------------------------------------------------------- */

/* Même repli que Fichiers : une icône voisine plutôt que la page barrée que
 * GTK dessine quand le thème ne fournit pas le pictogramme. */
static GtkWidget *
outil (const char *icone, const char *infobulle, const char *action)
{
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
    GtkWidget *b = gtk_button_new_from_icon_name (
        gtk_icon_theme_has_icon (theme, icone) ? icone : "image-x-generic-symbolic");
    gtk_widget_add_css_class (b, "images-outil");
    gtk_widget_set_tooltip_text (b, infobulle);
    gtk_actionable_set_action_name (GTK_ACTIONABLE (b), action);
    /* Un clic ne prend pas le focus : le clavier doit continuer de parler
     * à l'image, et un bouton focalisé dessinerait un anneau sur la barre
     * après chaque clic. */
    gtk_widget_set_focus_on_click (b, FALSE);
    return b;
}

static GtkWidget *
separateur (void)
{
    GtkWidget *s = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class (s, "images-sep");
    return s;
}

/* Les commandes posées sur l'image. Un clic dessus les garde à l'écran :
 * on enchaîne souvent trois « suivante » au doigt sans bouger d'ailleurs. */
static void
on_commande_cliquee (GtkButton *b, gpointer data)
{
    (void) b; (void) data;
    reveiller ();
}

static GtkWidget *
fleche (const char *icone, const char *infobulle, const char *action, GtkAlign cote)
{
    GtkWidget *b = gtk_button_new_from_icon_name (icone);
    gtk_widget_add_css_class (b, "images-fleche");
    gtk_widget_set_tooltip_text (b, infobulle);
    gtk_actionable_set_action_name (GTK_ACTIONABLE (b), action);
    gtk_widget_set_focus_on_click (b, FALSE);
    gtk_widget_set_focusable (b, FALSE);
    gtk_widget_set_halign (b, cote);
    gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
    g_signal_connect (b, "clicked", G_CALLBACK (on_commande_cliquee), NULL);
    reveler (b, FALSE);
    return b;
}

static GtkWidget *
message (const char *icone, const char *titre, GtkWidget **detail)
{
    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class (boite, "images-message");
    gtk_widget_set_halign (boite, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (boite, GTK_ALIGN_CENTER);

    GtkWidget *img = gtk_image_new_from_icon_name (icone);
    gtk_image_set_pixel_size (GTK_IMAGE (img), 64);
    gtk_widget_add_css_class (img, "images-message-icone");

    GtkWidget *t = gtk_label_new (titre);
    gtk_widget_add_css_class (t, "images-message-titre");

    *detail = gtk_label_new ("");
    gtk_widget_add_css_class (*detail, "images-message-detail");
    gtk_label_set_wrap (GTK_LABEL (*detail), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (*detail), 52);
    gtk_label_set_justify (GTK_LABEL (*detail), GTK_JUSTIFY_CENTER);

    gtk_box_append (GTK_BOX (boite), img);
    gtk_box_append (GTK_BOX (boite), t);
    gtk_box_append (GTK_BOX (boite), *detail);
    gtk_widget_set_visible (boite, FALSE);
    return boite;
}

static GtkWidget *
construire_barre (void)
{
    GtkWidget *barre = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class (barre, "images-barre");

    gtk_box_append (GTK_BOX (barre), outil ("go-previous-symbolic", "Image précédente (←)", "app.precedent"));
    gtk_box_append (GTK_BOX (barre), outil ("go-next-symbolic",     "Image suivante (→)",  "app.suivant"));

    GtkWidget *titre = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class (titre, "images-titre");
    gtk_widget_set_hexpand (titre, TRUE);

    I.nom = gtk_label_new ("");
    gtk_widget_add_css_class (I.nom, "images-nom");
    /* Coupé au milieu : le début d'un nom de photo est souvent « IMG_2026 »,
     * c'est la fin qui les distingue. */
    gtk_label_set_ellipsize (GTK_LABEL (I.nom), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign (GTK_LABEL (I.nom), 0.0);

    I.position = gtk_label_new ("");
    gtk_widget_add_css_class (I.position, "images-position");

    gtk_box_append (GTK_BOX (titre), I.nom);
    gtk_box_append (GTK_BOX (titre), I.position);
    gtk_box_append (GTK_BOX (barre), titre);

    /* « value-decrease » et non « zoom-out » : Papirus dessine ce dernier en
     * carré plein, qui pèse plus lourd que le pourcentage qu'il encadre. Un
     * simple « − » et un simple « + » de part et d'autre de la pastille se
     * lisent comme un réglage, ce qu'ils sont. */
    gtk_box_append (GTK_BOX (barre), outil ("value-decrease-symbolic", "Zoom arrière (−)", "app.zoom-moins"));
    I.zoom = gtk_button_new_with_label ("—");
    gtk_widget_add_css_class (I.zoom, "images-zoom");
    gtk_widget_set_tooltip_text (I.zoom, "Image entière ou taille réelle (0 et 1)");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (I.zoom), "app.zoom-bascule");
    gtk_widget_set_focus_on_click (I.zoom, FALSE);
    gtk_widget_set_valign (I.zoom, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (barre), I.zoom);
    gtk_box_append (GTK_BOX (barre), outil ("value-increase-symbolic", "Zoom avant (+)", "app.zoom-plus"));

    gtk_box_append (GTK_BOX (barre), separateur ());
    gtk_box_append (GTK_BOX (barre), outil ("object-rotate-left-symbolic",
                                            "Pivoter à gauche (Maj+R)", "app.pivoter-gauche"));
    gtk_box_append (GTK_BOX (barre), outil ("object-rotate-right-symbolic",
                                            "Pivoter à droite (R)", "app.pivoter-droite"));

    gtk_box_append (GTK_BOX (barre), separateur ());
    gtk_box_append (GTK_BOX (barre), outil ("folder-open-symbolic",
                                            "Afficher dans Fichiers", "app.dossier"));
    gtk_box_append (GTK_BOX (barre), outil ("document-open-symbolic",
                                            "Ouvrir une image (Ctrl+O)", "app.ouvrir"));
    I.b_plein = outil ("view-fullscreen-symbolic", "Plein écran (F11)", "app.plein-ecran");
    gtk_box_append (GTK_BOX (barre), I.b_plein);

    return barre;
}

static GtkWidget *
construire_etat (void)
{
    GtkWidget *etat = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class (etat, "images-etat");

    I.etat = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (I.etat), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (I.etat), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (I.etat, TRUE);

    I.etat_dossier = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (I.etat_dossier), 1.0);
    /* Coupé au DÉBUT : c'est la fin du chemin qui dit où l'on est. */
    gtk_label_set_ellipsize (GTK_LABEL (I.etat_dossier), PANGO_ELLIPSIZE_START);
    gtk_label_set_max_width_chars (GTK_LABEL (I.etat_dossier), 48);

    gtk_box_append (GTK_BOX (etat), I.etat);
    gtk_box_append (GTK_BOX (etat), I.etat_dossier);
    return etat;
}

static GtkWidget *
construire_toile (void)
{
    GtkWidget *superpose = gtk_overlay_new ();

    I.vue = IMAGES_VUE (images_vue_new ());
    g_signal_connect (I.vue, "naviguer",    G_CALLBACK (on_naviguer),    NULL);
    g_signal_connect (I.vue, "zoom-change", G_CALLBACK (on_zoom_change), NULL);
    g_signal_connect (I.vue, "touche",      G_CALLBACK (on_touchee),     NULL);
    g_signal_connect (I.vue, "double-clic", G_CALLBACK (on_double),      NULL);
    gtk_overlay_set_child (GTK_OVERLAY (superpose), GTK_WIDGET (I.vue));

    I.attente = gtk_spinner_new ();
    gtk_widget_add_css_class (I.attente, "images-attente");
    gtk_widget_set_size_request (I.attente, 32, 32);
    gtk_widget_set_halign (I.attente, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (I.attente, GTK_ALIGN_CENTER);
    gtk_widget_set_can_target (I.attente, FALSE);
    gtk_widget_set_visible (I.attente, FALSE);
    gtk_overlay_add_overlay (GTK_OVERLAY (superpose), I.attente);

    I.erreur = message ("image-missing-symbolic", "Impossible d'afficher cette image",
                        &I.erreur_detail);
    gtk_widget_set_can_target (I.erreur, FALSE);
    gtk_overlay_add_overlay (GTK_OVERLAY (superpose), I.erreur);

    I.vide = message ("image-x-generic-symbolic", "Aucune image", &I.vide_detail);
    GtkWidget *choisir = gtk_button_new_with_label ("Choisir une image…");
    gtk_widget_add_css_class (choisir, "images-ouvrir");
    gtk_widget_set_halign (choisir, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top (choisir, 10);
    gtk_actionable_set_action_name (GTK_ACTIONABLE (choisir), "app.ouvrir");
    gtk_box_append (GTK_BOX (I.vide), choisir);
    gtk_overlay_add_overlay (GTK_OVERLAY (superpose), I.vide);

    I.fleche_g = fleche ("go-previous-symbolic", "Image précédente", "app.precedent", GTK_ALIGN_START);
    I.fleche_d = fleche ("go-next-symbolic",     "Image suivante",   "app.suivant",   GTK_ALIGN_END);
    gtk_overlay_add_overlay (GTK_OVERLAY (superpose), I.fleche_g);
    gtk_overlay_add_overlay (GTK_OVERLAY (superpose), I.fleche_d);

    I.pilule = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class (I.pilule, "images-pilule");
    gtk_widget_set_halign (I.pilule, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (I.pilule, GTK_ALIGN_START);
    I.pilule_texte = gtk_label_new ("");
    gtk_widget_add_css_class (I.pilule_texte, "images-pilule-texte");
    gtk_label_set_ellipsize (GTK_LABEL (I.pilule_texte), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars (GTK_LABEL (I.pilule_texte), 60);
    GtkWidget *sortir = outil ("view-restore-symbolic", "Quitter le plein écran (Échap)",
                               "app.quitter-plein-ecran");
    gtk_widget_set_focusable (sortir, FALSE);
    g_signal_connect (sortir, "clicked", G_CALLBACK (on_commande_cliquee), NULL);
    gtk_box_append (GTK_BOX (I.pilule), I.pilule_texte);
    gtk_box_append (GTK_BOX (I.pilule), sortir);
    reveler (I.pilule, FALSE);
    gtk_overlay_add_overlay (GTK_OVERLAY (superpose), I.pilule);

    return superpose;
}

/* -------------------------------------------------------------------------
 * Thème
 *
 * Exactement le chemin de Fichiers : la feuille du thème et shell.css,
 * rechargées à chaud quand les Réglages réécrivent shell.conf. La
 * visionneuse n'a pas de couleur à elle — que des jetons.
 * ------------------------------------------------------------------------- */
static void
on_config_relue (ShellConfig *cfg, gpointer data)
{
    (void) data;
    shell_styles_load (cfg);
    shell_config_apply (cfg);
    g_object_set (gtk_settings_get_default (),
                  "gtk-application-prefer-dark-theme", cfg->dark, NULL);
    shell_config_free (cfg);
}

/* La fenêtre fermée, l'application s'arrête — mais pas dans l'instant : un
 * décodage peut s'achever dans la même itération de la boucle, et son
 * rappel toucherait des widgets détruits. On coupe donc tout ici, et les
 * rappels asynchrones vérifient I.fenetre avant de rien afficher. */
static void
on_fenetre_detruite (GtkWidget *w, gpointer data)
{
    (void) w; (void) data;

    g_clear_handle_id (&I.masquer_id, g_source_remove);
    g_clear_handle_id (&I.attente_id, g_source_remove);
    g_clear_handle_id (&I.relire_id,  g_source_remove);
    anim_arreter ();
    if (I.annul_liste != NULL)
        g_cancellable_cancel (I.annul_liste);
    if (I.moniteur != NULL) {
        g_signal_handlers_disconnect_by_func (I.moniteur, on_dossier_change, NULL);
        g_file_monitor_cancel (I.moniteur);
        g_clear_object (&I.moniteur);
    }
    g_clear_pointer (&I.affichee, charge_unref);
    g_ptr_array_set_size (I.cache, 0);      /* abandonne tout ce qui décode */

    I.fenetre = NULL;
    I.vue     = NULL;
}

static void
on_startup (GtkApplication *app, gpointer cfg)
{
    shell_styles_startup (app, cfg);
    recenser_types ();
    I.cache = g_ptr_array_new_with_free_func (charge_abandonner);

    g_action_map_add_action_entries (G_ACTION_MAP (app), actions,
                                     G_N_ELEMENTS (actions), NULL);

    const struct { const char *action; const char *touches[3]; } raccourcis[] = {
        { "app.ouvrir", { "<Control>o", NULL, NULL } },
        { "app.fermer", { "<Control>w", "<Control>q", NULL } },
    };
    for (guint i = 0; i < G_N_ELEMENTS (raccourcis); i++)
        gtk_application_set_accels_for_action (app, raccourcis[i].action,
                                               raccourcis[i].touches);
}

static void
on_activate (GtkApplication *app, gpointer user_data)
{
    if (I.fenetre != NULL) {
        gtk_window_present (GTK_WINDOW (I.fenetre));
        return;
    }

    ShellConfig *cfg = user_data;
    shell_config_apply (cfg);
    /* Les widgets GTK ordinaires — infobulles, roue d'attente, boîte de
     * choix de fichier — ne passent pas par notre feuille de style. Sans
     * cela ils resteraient clairs dans une fenêtre sombre. */
    g_object_set (gtk_settings_get_default (),
                  "gtk-application-prefer-dark-theme", cfg->dark, NULL);

    I.app = app;
    I.fenetre = gtk_application_window_new (app);
    gtk_widget_add_css_class (I.fenetre, "shell");
    gtk_widget_add_css_class (I.fenetre, "images");
    gtk_window_set_title (GTK_WINDOW (I.fenetre), "Images");
    gtk_window_set_default_size (GTK_WINDOW (I.fenetre), 1280, 820);

    I.revele_barre = gtk_revealer_new ();
    gtk_revealer_set_transition_type (GTK_REVEALER (I.revele_barre),
                                      GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_child (GTK_REVEALER (I.revele_barre), construire_barre ());
    gtk_revealer_set_reveal_child (GTK_REVEALER (I.revele_barre), TRUE);

    I.revele_etat = gtk_revealer_new ();
    gtk_revealer_set_transition_type (GTK_REVEALER (I.revele_etat),
                                      GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_child (GTK_REVEALER (I.revele_etat), construire_etat ());
    gtk_revealer_set_reveal_child (GTK_REVEALER (I.revele_etat), TRUE);

    GtkWidget *pile = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (pile), I.revele_barre);
    gtk_box_append (GTK_BOX (pile), construire_toile ());
    gtk_box_append (GTK_BOX (pile), I.revele_etat);
    gtk_window_set_child (GTK_WINDOW (I.fenetre), pile);

    GtkEventController *k = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (k, GTK_PHASE_CAPTURE);
    g_signal_connect (k, "key-pressed", G_CALLBACK (on_touche), NULL);
    gtk_widget_add_controller (I.fenetre, k);

    /* « enter » autant que « motion » : un pointeur qui arrive d'un bond dans
     * la fenêtre — un stylet qui se pose, une souris qu'on déplace vite —
     * ne produit qu'une entrée, sans mouvement. Constaté au banc d'essai :
     * les flèches ne venaient pas. */
    GtkEventController *m = gtk_event_controller_motion_new ();
    g_signal_connect (m, "enter",  G_CALLBACK (on_mouvement), NULL);
    g_signal_connect (m, "motion", G_CALLBACK (on_mouvement), NULL);
    gtk_widget_add_controller (I.fenetre, m);

    GtkDropTarget *depot = gtk_drop_target_new (GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect (depot, "drop", G_CALLBACK (on_depot), NULL);
    gtk_widget_add_controller (I.fenetre, GTK_EVENT_CONTROLLER (depot));

    g_signal_connect (I.fenetre, "notify::fullscreened", G_CALLBACK (on_plein_ecran), NULL);
    g_signal_connect (I.fenetre, "destroy", G_CALLBACK (on_fenetre_detruite), NULL);

    rafraichir ();
    maj_zoom ();
    gtk_window_present (GTK_WINDOW (I.fenetre));

    shell_config_watch (on_config_relue, NULL);
}

static void
on_open (GApplication *app, GFile **fichiers, int n, const char *hint, gpointer data)
{
    (void) hint; (void) data;

    /* Une seule fenêtre, qu'on réutilise : ouvrir une autre image depuis
     * Fichiers remplace celle qu'on regardait. Deux visionneuses côte à
     * côte coûteraient deux runtimes GTK sur 4 Go. */
    if (I.fenetre == NULL)
        g_application_activate (app);
    if (n > 0)
        ouvrir (fichiers[0]);
    gtk_window_present (GTK_WINDOW (I.fenetre));
}

int
main (int argc, char **argv)
{
    ShellConfig *cfg = shell_config_load ();

    GtkApplication *app = gtk_application_new ("os.claude.shell.images",
                                               G_APPLICATION_HANDLES_OPEN);
    g_signal_connect (app, "startup",  G_CALLBACK (on_startup),  cfg);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), cfg);
    g_signal_connect (app, "open",     G_CALLBACK (on_open),     NULL);

    int status = g_application_run (G_APPLICATION (app), argc, argv);
    g_object_unref (app);
    shell_config_free (cfg);
    return status;
}
