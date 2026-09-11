/* Claude OS -- « Vidéo », le lecteur.
 *
 * CE QUI BAT LA MESURE, ET POURQUOI CE N'EST PAS UN MINUTEUR.
 *
 * L'affichage est commande par le « frame clock » de GTK, c'est-a-dire par
 * les frame callbacks du compositeur. Trois consequences, toutes voulues :
 *
 *   -- on n'affiche jamais plus d'images que l'ecran n'en montre ;
 *   -- quand la fenetre est masquee, le compositeur cesse d'appeler, GTK
 *      cesse de battre, et le decodage video s'arrete de lui-meme. Aucune
 *      detection de visibilite a ecrire, donc aucune a rater ;
 *   -- en pause, le battement est retire : plus un reveil.
 *
 * Un g_timeout_add(1000/30, ...) aurait fait la meme chose a l'ecran, et
 * aurait battu dans le vide fenetre masquee comme en pause.
 *
 * LA FORME DEMANDEE, ET CE QU'ELLE IMPLIQUE.
 *
 *   +-------------------------------+
 *   |                               |  la video, SANS AUCUNE BORDURE :
 *   |            video              |  ni cadre, ni marge, ni coin arrondi,
 *   |                               |  ET SANS BARRE DE TITRE.
 *   +-------------------------------+
 *   :        (fond transparent)     :  <- au survol, la glissiere apparait ICI
 *   :     ( o====|--------- )       :
 *   +-------------------------------+
 *   |     ( capsule des commandes ) |
 *   +-------------------------------+
 *
 * LA FENETRE N'EST PAS DECOREE ET SON FOND EST TRANSPARENT. Il n'y a donc
 * que trois choses a l'ecran : l'image, la capsule, et la glissiere quand on
 * la demande. Le bureau se voit entre les deux.
 *
 * Consequence a connaitre : sans barre de titre, la fenetre se deplace a
 * l'Alt-glisser du compositeur -- ou en tirant LA CAPSULE, qui est un
 * GtkWindowHandle pour cette raison.
 *
 * L'espace vide n'est pas une marge : c'est une ZONE SENSIBLE. La glissiere
 * y apparait en fondu, PAR-DESSUS le vide, sans jamais deplacer la video ni
 * la capsule -- un decalage de mise en page a chaque survol serait
 * insupportable a l'usage. D'ou une GtkOverlay, et non une boite.
 *
 * LES COMMANDES, ET QUAND ELLES SE MONTRENT.
 *
 *                      en fenetre                en plein ecran
 *   capsule            toujours visible          cachee, revient au geste
 *   glissiere          au survol de la zone      avec la capsule
 *   mouvement souris   rien                      fait tout revenir
 *   appui simple       revele + lecture/pause    idem
 *   appui double       PLEIN ECRAN               PLEIN ECRAN
 *
 * L'APPUI SIMPLE EST RETARDE, ET IL LE FAUT. Sans cela, le premier appui
 * d'un double appui aurait deja bascule la lecture avant que le second
 * n'arrive : on obtenait « pause puis plein ecran », ce qui est exactement
 * ce qu'on ne veut pas. Le retard est celui du systeme (gtk-double-click-
 * time), borne a 300 ms -- au-dela, la pause se sent. Le RETOUR VISUEL, lui,
 * est immediat : les commandes apparaissent des le premier contact.
 */

#include "video-moteur.h"
#include "video-image.h"
#include "config.h"

#include <gtk/gtk.h>

/* Hauteur de l'espace vide. Assez haut pour qu'un doigt le vise sans viser,
 * assez bas pour rester un intervalle et non un bandeau. */
#define ZONE_VIDE_PX 46

/* Combien de temps la glissiere reste apres le dernier contact tactile.
 * C'est le SEUL minuteur du lecteur, il est a un coup, et il n'existe que
 * pendant les quelques secondes qui suivent un toucher. */
#define RETRAIT_S 4

typedef struct {
    GtkApplication *app;
    GtkWidget      *fenetre;
    GtkWidget      *image;
    GtkWidget      *offload;

    GtkWidget      *zone;          /* l'espace vide, sensible au survol   */
    GtkWidget      *reveleur;      /* celui de la glissiere               */
    GtkWidget      *glissiere;
    GtkWidget      *capsule;
    GtkWidget      *rev_capsule;   /* la capsule s'efface en plein ecran  */
    GtkWidget      *commandes;     /* zone + capsule, flottant au bas     */
    GtkWidget      *racine;        /* la GtkOverlay qui porte tout        */
    GtkWidget      *rev_fermer;    /* la croix, au survol de l'image      */
    GtkWidget      *accueil;       /* « Ouvrir une vidéo… », fenetre vide */
    GtkWidget      *panneau;       /* le selecteur, DANS la fenetre       */
    GtkWidget      *choix;         /* le GtkFileChooserWidget             */
    GtkWidget      *b_valider;
    guint           retrait_fermer;

    GtkWidget      *b_lecture;
    GtkWidget      *b_prec;
    GtkWidget      *b_suiv;
    GtkWidget      *b_plein;
    GtkWidget      *b_ouvrir;
    GtkWidget      *b_son;
    GtkWidget      *volume;
    GtkWidget      *l_position;
    GtkWidget      *l_duree;
    GtkWidget      *l_codec;
    GtkWidget      *st_texte;      /* le sous-titre, par-dessus l'image   */
    GtkWidget      *b_pistes;

    VideoMoteur    *moteur;
    VideoImage      images;
    guint           battement;
    guint           retrait;        /* minuteur a un coup du retrait      */
    guint           appui_simple;   /* minuteur qui distingue un appui d'un
                                     * double appui                       */

    gboolean        plein_ecran;
    gboolean        commandes_vues; /* en plein ecran seulement           */

    /* LA LISTE DE LECTURE : les videos du dossier, triees comme Fichiers.
     * Chargee en ASYNCHRONE, et ce n'est pas un luxe -- un dossier peut
     * etre sur un partage reseau, et g_file_enumerate_children bloque
     * jusqu'au delai TCP si le serveur est eteint. Le projet a deja paye
     * cette lecon dans Fichiers. */
    GPtrArray      *dossier;        /* gchar*, chemins tries              */
    int             rang;           /* -1 : liste inconnue                */
    GCancellable   *annulation;     /* l'enumeration meurt avec la fenetre */

    gint64          geste_us;       /* dernier geste sur la glissiere     */
    int             seconde_affichee;
    gboolean        survol;

    gchar          *fichier;
    int             duree_essai;
    gboolean        plein;
    gboolean        scenario;
    gboolean        ouvrir_au_depart;  /* banc : montrer le panneau      */
    gboolean        sans_offload;   /* banc : pour mesurer ce qu'il coute  */
    gboolean        revele;         /* --revele : banc, pour la capture     */
    gboolean        avec_st;        /* --sous-titres : banc, active la 1re  */
    int             etape;
    gint64          depart;
    gint64          battements;
    /* LA CADENCE REELLE : combien de rafraichissements d'ecran chaque image
     * reste affichee. A 23,976 im/s sur 60 Hz, la suite correcte est
     * 3,2,3,2,3... Toute autre suite se voit, meme sans perdre une image. */
    int             depuis_image;
    gint64          derniere_image;
    gint64          cadence[16];
    int             dernier_seau;
    gint64          alternances, repetitions;
} App;

static void bilan(App *a, const char *quand);
static void act_sourdine(GtkButton *b, gpointer u);
static void ouvrir_fichier(App *a, const char *chemin);
static void demander_fichier(App *a);
static void panneau_cacher(App *a);
static void reprise_ecrire(const char *fichier, double position, double duree);
static GtkWidget *construire_panneau(App *a);
static GPtrArray *lecteurs_reseau_montes(void);
static void reveler(App *a, gboolean visible, gboolean momentane);

/* ---------------------------------------------------- reprendre ou l'on en

   REPRENDRE, MAIS PAS N'IMPORTE QUAND. Trois garde-fous, et chacun evite un
   agacement precis :

     -- rien sous deux minutes de film : on ne « reprend » pas un clip ;
     -- rien sous trente secondes de lecture : ouvrir, fermer aussitot et
        retrouver le film a huit secondes est plus genant qu'utile ;
     -- rien dans la derniere minute : un film fini doit se rouvrir au
        debut, pas sur son generique.

   Le fichier est un simple GKeyFile dans l'etat de session, pas dans la
   configuration : c'est une trace d'usage, pas un reglage. Il est borne a
   cent entrees, faute de quoi il grossirait indefiniment sur une machine
   qui sert de lecteur. */

#define REPRISE_FILM_MIN   120.0
#define REPRISE_DEBUT_MIN   30.0
#define REPRISE_FIN_MARGE   60.0
#define REPRISE_MAX_ENTREES  100

static gchar *chemin_reprises(void)
{
    return g_build_filename(g_get_user_state_dir(), "claude-os",
                            "video-reprises", NULL);
}

static void reprise_ecrire(const char *fichier, double position, double duree)
{
    if (duree < REPRISE_FILM_MIN) return;

    g_autofree gchar *chemin = chemin_reprises();
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_key_file_load_from_file(kf, chemin, G_KEY_FILE_NONE, NULL);

    g_autofree gchar *cle = g_uri_escape_string(fichier, NULL, TRUE);

    if (position < REPRISE_DEBUT_MIN || position > duree - REPRISE_FIN_MARGE) {
        g_key_file_remove_key(kf, "reprises", cle, NULL);
    } else {
        g_key_file_set_double(kf, "reprises", cle, position);
    }

    /* Bornage : au-dela de cent entrees on repart d'un fichier propre
     * plutot que d'inventer une politique de peremption pour une trace
     * d'usage sans importance. */
    gsize n = 0;
    g_autofree gchar **cles = g_key_file_get_keys(kf, "reprises", &n, NULL);
    if (n > REPRISE_MAX_ENTREES) {
        g_key_file_remove_group(kf, "reprises", NULL);
        g_key_file_set_double(kf, "reprises", cle, position);
    }

    g_autofree gchar *dossier = g_path_get_dirname(chemin);
    g_mkdir_with_parents(dossier, 0700);

    GError *e = NULL;
    if (!g_key_file_save_to_file(kf, chemin, &e)) {
        /* INVARIANT N.4 : meme une trace sans importance dit quand elle
         * echoue. Un disque plein se remarque ici avant ailleurs. */
        g_message("video : reprise non enregistrée (%s)", e ? e->message : "?");
        g_clear_error(&e);
    }
}

/* LE DERNIER DOSSIER OUVERT, garde a cote des reprises.
 *
 * Sans lui, la boite s'ouvre sur « Recents » -- qui, sur cette machine, ne
 * contient que deux entrees sans rapport -- et il faut retraverser toute
 * l'arborescence a chaque fois. Un lecteur video s'ouvre la ou etait le
 * dernier film. */
static void dernier_dossier_ecrire(const char *chemin)
{
    g_autofree gchar *f = chemin_reprises();
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_key_file_load_from_file(kf, f, G_KEY_FILE_NONE, NULL);

    g_autoptr(GFile) fic = g_file_new_for_path(chemin);
    g_autoptr(GFile) dir = g_file_get_parent(fic);
    if (!dir) return;
    g_autofree gchar *d = g_file_get_path(dir);
    if (!d) return;

    g_key_file_set_string(kf, "reprises", "dernier-dossier", d);
    g_autofree gchar *dossier = g_path_get_dirname(f);
    g_mkdir_with_parents(dossier, 0700);
    g_key_file_save_to_file(kf, f, NULL);
}

static gchar *dernier_dossier_lire(void)
{
    g_autofree gchar *f = chemin_reprises();
    g_autoptr(GKeyFile) kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, f, G_KEY_FILE_NONE, NULL)) return NULL;
    return g_key_file_get_string(kf, "reprises", "dernier-dossier", NULL);
}

static double reprise_lire(const char *fichier)
{
    g_autofree gchar *chemin = chemin_reprises();
    g_autoptr(GKeyFile) kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, chemin, G_KEY_FILE_NONE, NULL))
        return 0.0;

    g_autofree gchar *cle = g_uri_escape_string(fichier, NULL, TRUE);
    return g_key_file_get_double(kf, "reprises", cle, NULL);
}

/* ------------------------------------------------------------- affichage */

static gchar *duree_texte(double s)
{
    if (s < 0) s = 0;
    int total = (int)(s + 0.5);
    int h = total / 3600, m = (total / 60) % 60, sec = total % 60;
    return h > 0 ? g_strdup_printf("%d:%02d:%02d", h, m, sec)
                 : g_strdup_printf("%d:%02d", m, sec);
}

/* La position ne se reecrit QUE quand la seconde change. Sans ce garde-fou,
 * l'etiquette serait reconstruite quatre-vingts fois par seconde pour
 * afficher le meme texte, et chaque reecriture invalide une zone de la
 * fenetre -- donc reveille le compositeur pour rien. */
static void rafraichir_position(App *a, double position)
{
    int s = (int)position;
    if (s == a->seconde_affichee) return;
    a->seconde_affichee = s;

    gchar *t = duree_texte(position);
    gtk_label_set_text(GTK_LABEL(a->l_position), t);
    g_free(t);
}

static void rafraichir_glissiere(App *a, double position)
{
    /* Pendant qu'on tire la glissiere, on ne la contredit pas : sans ce
     * delai, le curseur saute sous le doigt entre deux positions. */
    if (g_get_monotonic_time() - a->geste_us < 300 * G_TIME_SPAN_MILLISECOND)
        return;
    if (!gtk_revealer_get_reveal_child(GTK_REVEALER(a->reveleur)))
        return;                     /* invisible : rien a redessiner      */

    g_signal_handlers_block_matched(a->glissiere, G_SIGNAL_MATCH_DATA, 0, 0,
                                    NULL, NULL, a);
    gtk_range_set_value(GTK_RANGE(a->glissiere), position);
    g_signal_handlers_unblock_matched(a->glissiere, G_SIGNAL_MATCH_DATA, 0, 0,
                                      NULL, NULL, a);
}

/* LE SOUS-TITRE COUTE, ET IL FAUT LE SAVOIR.
 *
 * Tant qu'aucun texte n'est affiche, l'etiquette est CACHEE -- et non pas
 * vide. La difference n'est pas cosmetique : un widget vide mais visible
 * par-dessus l'image suffit a faire renoncer GTK au chemin sans copie,
 * puisqu'il y a alors quelque chose a composer au-dessus de la
 * sous-surface. Une video sans sous-titres ne doit rien payer pour ceux
 * qu'elle n'a pas.
 *
 * Et l'etiquette n'est reecrite que lorsque le TEXTE change, pas a chaque
 * image : une replique reste deux secondes a l'ecran, soit cent soixante
 * battements pendant lesquels il n'y a rien a faire. */
static void rafraichir_sous_titre(App *a)
{
    g_autofree gchar *texte = NULL;
    if (!video_moteur_sous_titre(a->moteur, &texte)) return;

    if (texte && *texte) {
        gtk_label_set_markup(GTK_LABEL(a->st_texte), texte);
        gtk_widget_set_visible(a->st_texte, TRUE);
    } else {
        gtk_widget_set_visible(a->st_texte, FALSE);
        gtk_label_set_text(GTK_LABEL(a->st_texte), "");
    }
}

static gboolean sur_battement(GtkWidget *w, GdkFrameClock *horloge, gpointer u)
{
    (void) w; (void) horloge;
    App *a = u;
    a->battements++;

    if (a->duree_essai > 0 &&
        g_get_monotonic_time() - a->depart > (gint64)a->duree_essai * G_USEC_PER_SEC) {
        gtk_window_close(GTK_WINDOW(a->fenetre));
        return G_SOURCE_REMOVE;
    }

    a->depuis_image++;

    /* QUAND CETTE IMAGE SERA-T-ELLE VRAIMENT A L'ECRAN ?
     *
     * GTK le sait : l'horloge d'images porte l'instant de balayage prevu et
     * la periode de rafraichissement. On decide donc pour CET instant-la,
     * decale d'un demi-rafraichissement -- ce qui revient a choisir l'image
     * la plus proche du balayage plutot que la derniere qui soit deja due.
     *
     * Sans cela, une gigue d'une milliseconde sur l'horloge audio suffit a
     * faire basculer une image d'un rafraichissement au suivant. Mesure sur
     * une mire a 30 im/s : 268 images tenues deux rafraichissements, 143
     * trois, 30 quatre -- alors que « deux » partout est la seule reponse
     * juste. Aucune image perdue, et pourtant cela se voit. */
    gint64 intervalle = 0, presentation = 0;
    gint64 maintenant_us = g_get_monotonic_time();
    gdk_frame_clock_get_refresh_info(horloge, maintenant_us,
                                     &intervalle, &presentation);
    if (intervalle <= 0) intervalle = 16667;     /* 60 Hz, a defaut de mieux */

    double avance = 0.0;
    if (presentation > 0) {
        double d = (presentation - maintenant_us) / 1e6;
        /* Un ecart absurde -- horloge pas encore etablie -- ne doit pas
         * propulser la lecture en avant. */
        if (d > -0.1 && d < 0.2) avance = d;
    }
    /* Un demi-rafraichissement : cela revient a choisir l'image la PLUS
     * PROCHE du balayage, plutot que la derniere qui soit deja due. Sans
     * cela, une image dont l'echeance tombe a un cheveu du balayage bascule
     * au rafraichissement suivant, et l'intervalle passe de 33 a 50 ms. */
    avance += intervalle / 2e6;

    AVFrame *trame = video_moteur_image_due(a->moteur, avance);
    if (trame) {
        /* L'INTERVALLE ENTRE DEUX IMAGES AFFICHEES, en millisecondes. C'est
         * lui qui se voit : pour une video a 30 im/s il doit valoir 33 ms a
         * chaque fois. Compter les battements ne dit rien -- l'horloge de
         * GTK en glisse de parasites a deux millisecondes d'intervalle. */
        gint64 now2 = g_get_monotonic_time();
        if (a->derniere_image)
            a->cadence[CLAMP((int)((now2 - a->derniere_image) / 1000 / 8), 0, 15)]++;
        if (a->derniere_image) {
            int ms = (int)((now2 - a->derniere_image) / 1000);
            int seau = (ms < 42) ? 2 : 3;          /* 2 ou 3 rafraichissements */
            if (a->dernier_seau) {
                if (seau == a->dernier_seau) a->repetitions++;
                else                         a->alternances++;
            }
            a->dernier_seau = seau;
        }
        a->derniere_image = now2;
        a->depuis_image = 0;
    }

    /* IMAGE_DUE PEUT AVOIR TOUT DEMOLI SOUS NOS PIEDS.
     *
     * C'est elle qui constate la fin du fichier et appelle le rappel de
     * fin ; celui-ci peut fermer la fenetre, ce qui detruit les widgets et
     * ferme le moteur. Tout ce qui suit toucherait alors des pointeurs
     * morts. Trouve le 10 septembre 2026 en laissant une mire aller
     * jusqu'au bout : « gtk_label_set_text: assertion GTK_IS_LABEL failed »,
     * juste apres le bilan. */
    if (!a->moteur) {
        av_frame_free(&trame);
        return G_SOURCE_REMOVE;
    }

    if (trame) {
        GdkTexture *t = video_image_texture(&a->images, trame,
                                            gtk_widget_get_display(a->image));
        /* La texture dmabuf garde sa propre reference sur les tampons : la
         * trame decodee peut etre rendue au decodeur des maintenant, ce qui
         * lui evite d'attendre une surface libre. */
        av_frame_free(&trame);

        if (t) {
            gtk_picture_set_paintable(GTK_PICTURE(a->image), GDK_PAINTABLE(t));
            g_object_unref(t);
        }
    }

    double position = video_moteur_position(a->moteur);
    rafraichir_position(a, position);
    rafraichir_glissiere(a, position);
    rafraichir_sous_titre(a);
    return G_SOURCE_CONTINUE;
}

/* Le battement n'existe QUE pendant la lecture, et seulement sur un widget
 * deja affiche.
 *
 * PIEGE PAYE LE 10 SEPTEMBRE 2026, et il ne se voit pas a la lecture :
 * gtk_widget_add_tick_callback() sur un widget pas encore realise enregistre
 * le rappel et rend un identifiant valide, mais n'a pas d'horloge a mettre
 * en marche. Le rappel est alors appele UNE fois, a la premiere image que
 * GTK dessine de toute facon, puis plus jamais -- sans une erreur. */
static void battement_selon(App *a, gboolean actif)
{
    if (actif && !gtk_widget_get_mapped(a->image)) return;

    if (actif && !a->battement)
        a->battement = gtk_widget_add_tick_callback(a->image, sur_battement, a, NULL);
    else if (!actif && a->battement) {
        gtk_widget_remove_tick_callback(a->image, a->battement);
        a->battement = 0;
    }
}

/* --------------------------------------------------- montrer et cacher

   UN SEUL ENDROIT DECIDE DE CE QUI EST VISIBLE, et il tient compte du plein
   ecran. Deux regles cote a cote -- « en fenetre la capsule reste, en plein
   ecran elle s'efface » -- eparpillees dans dix rappels, c'est la garantie
   d'un etat incoherent au premier cas non prevu. */

static void appliquer_visibilite(App *a)
{
    gboolean capsule = !a->plein_ecran || a->commandes_vues;
    gtk_revealer_set_reveal_child(GTK_REVEALER(a->rev_capsule), capsule);

    /* EN PLEIN ECRAN, CACHEES, LES COMMANDES NE SONT PLUS UNE CIBLE.
     *
     * Elles flottent par-dessus l'image ; l'espace vide continuerait a
     * recevoir les appuis d'une bande de cinquante pixels au bas de l'ecran,
     * ou il ne se passerait rien. */
    if (a->commandes)
        gtk_widget_set_can_target(a->commandes,
                                  !a->plein_ecran || a->commandes_vues);

    /* La glissiere suit la capsule en plein ecran ; en fenetre elle garde
     * sa propre vie, revelee au survol de l'espace vide. */
    if (a->plein_ecran)
        gtk_revealer_set_reveal_child(GTK_REVEALER(a->reveleur),
                                      a->commandes_vues);
}

static gboolean retirer_commandes(gpointer u)
{
    App *a = u;
    a->retrait = 0;
    if (a->survol) return G_SOURCE_REMOVE;   /* le pointeur est dessus */

    a->commandes_vues = FALSE;
    if (!a->plein_ecran)
        gtk_revealer_set_reveal_child(GTK_REVEALER(a->reveleur), FALSE);
    appliquer_visibilite(a);
    return G_SOURCE_REMOVE;
}

/* « momentane » : ce qui est montre se retirera seul. C'est le cas au doigt
 * et au mouvement de pointeur ; ce n'est pas le cas quand le pointeur
 * SEJOURNE sur l'espace vide, ou la glissiere doit rester tant qu'il y est. */
static void commandes_montrer(App *a, gboolean momentane)
{
    a->commandes_vues = TRUE;
    if (!a->plein_ecran)
        gtk_revealer_set_reveal_child(GTK_REVEALER(a->reveleur), TRUE);
    appliquer_visibilite(a);

    if (a->retrait) { g_source_remove(a->retrait); a->retrait = 0; }
    if (momentane)
        a->retrait = g_timeout_add_seconds(RETRAIT_S, retirer_commandes, a);
}

static void commandes_cacher(App *a)
{
    if (a->retrait) { g_source_remove(a->retrait); a->retrait = 0; }
    a->commandes_vues = FALSE;
    if (!a->plein_ecran)
        gtk_revealer_set_reveal_child(GTK_REVEALER(a->reveleur), FALSE);
    appliquer_visibilite(a);
}

/* Compatibilite avec le reste du fichier : « reveler » ne concerne que la
 * glissiere, et n'a de sens qu'en fenetre. */
static void reveler(App *a, gboolean visible, gboolean momentane)
{
    if (visible) commandes_montrer(a, momentane);
    else         commandes_cacher(a);
}

static void sur_entree_zone(GtkEventControllerMotion *c, double x, double y, gpointer u)
{
    (void) c; (void) x; (void) y;
    App *a = u;
    a->survol = TRUE;
    commandes_montrer(a, FALSE);
}

static void sur_sortie_zone(GtkEventControllerMotion *c, gpointer u)
{
    (void) c;
    App *a = u;
    a->survol = FALSE;
    if (a->plein_ecran) commandes_montrer(a, TRUE);   /* se retirera seul */
    else                commandes_cacher(a);
}

/* LE MOUVEMENT DE POINTEUR, EN PLEIN ECRAN SEULEMENT.
 *
 * Il rappelle tout, puis tout se retire seul. En fenetre il ne fait rien :
 * la capsule y est deja la, et faire clignoter la glissiere au moindre
 * deplacement de souris serait insupportable. */
/* LA CROIX DE FERMETURE, au survol de l'image.
 *
 * Elle n'est pas decorative : SANS BARRE DE TITRE, c'est le seul moyen de
 * fermer l'application a la souris. Son absence a enferme l'utilisateur le
 * 10 septembre 2026 -- une boite d'ouverture modale s'etait glissee derriere
 * une autre fenetre, son « Annuler » etait hors d'atteinte, et la fenetre
 * dessous n'avait rien a cliquer. */
static gboolean retirer_croix(gpointer u)
{
    App *a = u;
    a->retrait_fermer = 0;
    gtk_revealer_set_reveal_child(GTK_REVEALER(a->rev_fermer), FALSE);
    return G_SOURCE_REMOVE;
}

static void croix_montrer(App *a)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(a->rev_fermer), TRUE);
    if (a->retrait_fermer) g_source_remove(a->retrait_fermer);
    a->retrait_fermer = g_timeout_add_seconds(RETRAIT_S, retirer_croix, a);
}

static void sur_mouvement(GtkEventControllerMotion *c, double x, double y, gpointer u)
{
    (void) c; (void) x; (void) y;
    App *a = u;

    /* La croix apparait dans les deux modes : c'est une sortie de secours,
     * elle ne doit pas dependre du mode ou l'on se trouve. */
    croix_montrer(a);

    if (!a->plein_ecran) return;
    if (a->commandes_vues && a->retrait) return;   /* deja montre, minuteur armé */
    commandes_montrer(a, TRUE);
}

static void act_fermer(GtkButton *b, gpointer u)
{
    (void) b;
    gtk_window_close(GTK_WINDOW(((App *)u)->fenetre));
}

/* ------------------------------------------------------- la liste de lecture

   LES VIDEOS DU DOSSIER, dans l'ordre de Fichiers, pour que « precedente »
   et « suivante » veuillent dire quelque chose.

   TOUT EST ASYNCHRONE, et ce n'est pas un luxe : le dossier peut etre un
   partage reseau, et g_file_enumerate_children BLOQUE jusqu'au delai TCP si
   le serveur ne repond plus. Fichiers a paye cette lecon sur un
   g_file_query_exists ; on ne la repaie pas ici. */

static void rafraichir_navigation(App *a)
{
    gboolean liste = (a->dossier != NULL && a->dossier->len > 1 && a->rang >= 0);
    gtk_widget_set_sensitive(a->b_prec, liste && a->rang > 0);
    gtk_widget_set_sensitive(a->b_suiv, liste && a->rang < (int)a->dossier->len - 1);
}

static int comparer_noms(gconstpointer x, gconstpointer y)
{
    const char *a = *(const char * const *)x;
    const char *b = *(const char * const *)y;
    /* collate_key_for_filename, comme Fichiers et comme la visionneuse :
     * « episode2 » avant « episode10 ». */
    g_autofree gchar *na = g_path_get_basename(a);
    g_autofree gchar *nb = g_path_get_basename(b);
    g_autofree gchar *ca = g_utf8_collate_key_for_filename(na, -1);
    g_autofree gchar *cb = g_utf8_collate_key_for_filename(nb, -1);
    return strcmp(ca, cb);
}

static void dossier_termine(App *a)
{
    g_ptr_array_sort(a->dossier, comparer_noms);

    a->rang = -1;
    for (guint i = 0; i < a->dossier->len; i++)
        if (g_strcmp0(g_ptr_array_index(a->dossier, i), a->fichier) == 0) {
            a->rang = (int)i;
            break;
        }
    rafraichir_navigation(a);
}

static void sur_lot(GObject *src, GAsyncResult *res, gpointer u);

static void demander_lot(App *a, GFileEnumerator *e)
{
    g_file_enumerator_next_files_async(e, 64, G_PRIORITY_LOW, a->annulation,
                                       sur_lot, a);
}

static void sur_lot(GObject *src, GAsyncResult *res, gpointer u)
{
    GFileEnumerator *e = G_FILE_ENUMERATOR(src);
    GError *err = NULL;
    GList *lot = g_file_enumerator_next_files_finish(e, res, &err);

    if (err) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_message("video : dossier illisible (%s) -- pas de navigation",
                      err->message);
        g_clear_error(&err);
        g_object_unref(e);
        return;
    }

    App *a = u;
    if (!lot) {                       /* fini */
        dossier_termine(a);
        g_object_unref(e);
        return;
    }

    GFile *dir = g_file_enumerator_get_container(e);
    for (GList *l = lot; l; l = l->next) {
        GFileInfo *info = l->data;
        const char *type = g_file_info_get_content_type(info);
        if (g_file_info_get_is_hidden(info)) continue;
        if (!type || !g_str_has_prefix(type, "video/")) continue;

        g_autoptr(GFile) f = g_file_get_child(dir, g_file_info_get_name(info));
        gchar *chemin = g_file_get_path(f);
        if (chemin) g_ptr_array_add(a->dossier, chemin);
    }
    g_list_free_full(lot, g_object_unref);

    demander_lot(a, e);               /* le lot suivant */
}

static void sur_enumeration(GObject *src, GAsyncResult *res, gpointer u)
{
    GError *err = NULL;
    GFileEnumerator *e = g_file_enumerate_children_finish(G_FILE(src), res, &err);
    if (!e) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_message("video : dossier illisible (%s) -- pas de navigation",
                      err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    demander_lot((App *)u, e);
}

static void dossier_charger(App *a, const char *chemin)
{
    if (a->dossier) g_ptr_array_unref(a->dossier);
    a->dossier = g_ptr_array_new_with_free_func(g_free);
    a->rang = -1;
    rafraichir_navigation(a);

    g_autoptr(GFile) f   = g_file_new_for_path(chemin);
    g_autoptr(GFile) dir = g_file_get_parent(f);
    if (!dir) return;

    g_file_enumerate_children_async(dir,
        G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
        G_FILE_QUERY_INFO_NONE, G_PRIORITY_LOW, a->annulation,
        sur_enumeration, a);
}

/* ------------------------------------------------- l'appui et le double appui

   LE PROBLEME : un double appui commence par un appui simple. Agir tout de
   suite sur le premier, c'est mettre en pause PUIS passer en plein ecran --
   ce que l'utilisateur voyait, et ne voulait pas.

   LA REPONSE : le retour visuel est immediat -- les commandes apparaissent
   des le premier contact -- mais la bascule lecture/pause est retardee du
   temps que le systeme accorde au double clic. Si un second appui arrive
   avant, il annule le premier et prend le plein ecran. */

static void basculer_plein(App *a);
static void ajuster_marge(App *a);

static int delai_double_appui(App *a)
{
    int ms = 400;
    GtkSettings *r = gtk_widget_get_settings(a->fenetre);
    if (r) g_object_get(r, "gtk-double-click-time", &ms, NULL);
    /* Borne haute : au-dela, la pause se sent comme une lenteur. */
    return CLAMP(ms, 150, 300);
}

static gboolean appui_simple_expire(gpointer u)
{
    App *a = u;
    a->appui_simple = 0;

    gboolean lisait = (video_moteur_etat(a->moteur) == VIDEO_LIT);
    video_moteur_basculer(a->moteur);

    /* En pause on laisse les commandes ; on vient de les demander. En
     * lecture elles se retirent seules -- c'est ce qui a ete demande :
     * « en pause, un simple appui remet la lecture et l'ensemble disparait
     * apres quelques secondes ». */
    commandes_montrer(a, lisait ? FALSE : TRUE);
    return G_SOURCE_REMOVE;
}

static void sur_appui(GtkGestureClick *g, int n, double x, double y, gpointer u)
{
    (void) g; (void) x; (void) y;
    App *a = u;
    if (!a->moteur) return;

    if (n >= 2) {
        /* Le double appui annule l'appui simple en attente. */
        if (a->appui_simple) { g_source_remove(a->appui_simple); a->appui_simple = 0; }
        basculer_plein(a);
        return;
    }

    commandes_montrer(a, TRUE);          /* retour visuel immediat */
    croix_montrer(a);
    if (a->appui_simple) g_source_remove(a->appui_simple);
    a->appui_simple = g_timeout_add(delai_double_appui(a), appui_simple_expire, a);
}

/* --------------------------------------------------------------- moteur */

static void rafraichir_bouton_lecture(App *a)
{
    gboolean lit = video_moteur_etat(a->moteur) == VIDEO_LIT;
    gtk_button_set_icon_name(GTK_BUTTON(a->b_lecture),
                             lit ? "media-playback-pause-symbolic"
                                 : "media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(a->b_lecture, lit ? "Pause" : "Lecture");
}

static void sur_etat(VideoEtat etat, gpointer u)
{
    App *a = u;
    battement_selon(a, etat == VIDEO_LIT);
    rafraichir_bouton_lecture(a);

    /* A l'arret la position n'avance plus : on la pose une derniere fois,
     * sinon l'etiquette garde la seconde d'avant. */
    if (etat != VIDEO_LIT) {
        double p = video_moteur_position(a->moteur);
        a->seconde_affichee = -1;
        rafraichir_position(a, p);
        rafraichir_glissiere(a, p);
    }
}

static void bilan(App *a, const char *quand)
{
    if (!a->moteur) return;
    double sec = (g_get_monotonic_time() - a->depart) / 1e6;
    double lec = 0, lec_max = 0, dec = 0;
    video_moteur_temps(a->moteur, &lec, &lec_max, &dec);
    g_message("video : %s -- lecture du fichier %.1f ms/appel (pire %.0f ms) ; "
              "décodage %.1f ms/image ; %ld famines",
              quand, lec, lec_max, dec,
              (long)video_moteur_famines(a->moteur));
    g_message("video : %s -- %ld images vues, %ld sautées ; "
              "%ld sans copie, %ld recopiées ; "
              "synchro %.1f ms en moyenne, %.1f ms au pire ; "
              "horloge d'affichage %.1f Hz",
              quand,
              (long)video_moteur_images_vues(a->moteur),
              (long)video_moteur_images_sautees(a->moteur),
              (long)a->images.sans_copie, (long)a->images.recopiees,
              video_moteur_ecart_moyen(a->moteur) * 1000.0,
              video_moteur_ecart_max(a->moteur) * 1000.0,
              sec > 0 ? a->battements / sec : 0.0);

    GString *c = g_string_new("video : intervalles entre images (ms) --");
    for (int i = 0; i < 16; i++)
        if (a->cadence[i])
            g_string_append_printf(c, " %d-%d:%ld", i * 8, i * 8 + 7,
                                   (long)a->cadence[i]);
    g_string_append_printf(c, "  |  alternances %ld, répétitions %ld",
                           (long)a->alternances, (long)a->repetitions);
    g_message("%s", c->str);
    g_string_free(c, TRUE);
}

static void sur_fin(gpointer u)
{
    App *a = u;
    bilan(a, "fin du fichier");
    reveler(a, TRUE, FALSE);          /* on rend la main a l'utilisateur */
    if (a->duree_essai > 0) gtk_window_close(GTK_WINDOW(a->fenetre));
}

static void sur_erreur(const char *message, gpointer u)
{
    (void)u;
    g_warning("video : %s", message);
}

/* ------------------------------------------------------------ commandes */

static void act_basculer(GtkButton *b, gpointer u)
{ (void) b; video_moteur_basculer(((App *)u)->moteur); }

/* ALLER A UNE AUTRE VIDEO DU DOSSIER.
 *
 * Les boutons « -10 s / +10 s » ont ete retires : les fleches du clavier et
 * la glissiere font deja ce travail, et mieux. Dans un dossier de series ou
 * de vacances, ce qu'on veut sous le doigt, c'est la video suivante. */
static void aller_a(App *a, int rang)
{
    if (!a->dossier || rang < 0 || rang >= (int)a->dossier->len) return;

    /* On note ou l'on en etait AVANT de quitter, sinon la reprise du film
     * qu'on laisse est perdue. */
    if (a->moteur && a->fichier)
        reprise_ecrire(a->fichier, video_moteur_position(a->moteur),
                       video_moteur_duree(a->moteur));

    g_free(a->fichier);
    a->fichier = g_strdup(g_ptr_array_index(a->dossier, rang));
    a->rang = rang;
    rafraichir_navigation(a);
    ouvrir_fichier(a, a->fichier);
}

static void act_precedente(GtkButton *b, gpointer u)
{ (void) b; App *a = u; aller_a(a, a->rang - 1); }

static void act_suivante(GtkButton *b, gpointer u)
{ (void) b; App *a = u; aller_a(a, a->rang + 1); }

static void basculer_plein(App *a)
{
    if (gtk_window_is_fullscreen(GTK_WINDOW(a->fenetre)))
        gtk_window_unfullscreen(GTK_WINDOW(a->fenetre));
    else
        gtk_window_fullscreen(GTK_WINDOW(a->fenetre));
}

static void act_plein(GtkButton *b, gpointer u) { (void) b; basculer_plein((App *)u); }

/* Ouvrir une autre video sans fermer l'application. Ctrl-O fait de meme ;
 * un bouton se voit, un raccourci se devine. */
static void act_ouvrir(GtkButton *b, gpointer u)
{
    (void) b;
    App *a = u;
    if (a->moteur && a->fichier)
        reprise_ecrire(a->fichier, video_moteur_position(a->moteur),
                       video_moteur_duree(a->moteur));
    demander_fichier(a);
}

/* LE PLEIN ECRAN SE LIT SUR LA FENETRE, PAS SUR LE BOUTON.
 *
 * labwc garde pour lui la touche « plein ecran » du Chromebook : la fenetre
 * peut donc changer d'etat sans que nous l'ayons demande. Suivre la
 * propriete « fullscreened » est la seule facon d'avoir une icone qui dit
 * vrai. Meme lecon que la visionneuse d'images. */
static void sur_plein_change(GObject *o, GParamSpec *p, gpointer u)
{
    (void) o; (void) p;
    App *a = u;
    gboolean plein = gtk_window_is_fullscreen(GTK_WINDOW(a->fenetre));

    gtk_button_set_icon_name(GTK_BUTTON(a->b_plein),
                             plein ? "view-restore-symbolic"
                                   : "view-fullscreen-symbolic");
    gtk_widget_set_tooltip_text(a->b_plein,
                                plein ? "Quitter le plein écran" : "Plein écran");

    a->plein_ecran = plein;

    /* EN ENTRANT, TOUT S'EFFACE ; EN SORTANT, LA CAPSULE REVIENT.
     *
     * On ne laisse pas les commandes affichees a l'entree en plein ecran :
     * on y va pour voir l'image, pas la capsule. Elles reviennent au premier
     * mouvement ou au premier appui. */
    if (plein) commandes_cacher(a);
    else       { a->commandes_vues = FALSE; appliquer_visibilite(a); }
    ajuster_marge(a);
}

static void rafraichir_son(App *a)
{
    gboolean muet = video_moteur_est_muet(a->moteur);
    double   v    = video_moteur_volume_actuel(a->moteur);
    const char *icone = (muet || v <= 0.001) ? "audio-volume-muted-symbolic"
                      : v < 0.5              ? "audio-volume-medium-symbolic"
                                             : "audio-volume-high-symbolic";
    gtk_button_set_icon_name(GTK_BUTTON(a->b_son), icone);
    gtk_widget_set_tooltip_text(a->b_son, muet ? "Rétablir le son" : "Couper le son");
}

static void act_sourdine(GtkButton *b, gpointer u)
{
    (void) b;
    App *a = u;
    video_moteur_sourdine(a->moteur, !video_moteur_est_muet(a->moteur));
    rafraichir_son(a);
}

static void sur_volume(GtkRange *r, gpointer u)
{
    App *a = u;
    video_moteur_volume(a->moteur, gtk_range_get_value(r));
    if (video_moteur_est_muet(a->moteur)) video_moteur_sourdine(a->moteur, FALSE);
    rafraichir_son(a);
}

/* On saute a CHAQUE mouvement de la glissiere, sans attendre le lacher.
 * Ce n'est pas couteux : le moteur ne garde qu'UNE demande de saut en
 * attente, les demandes rapides s'ecrasent donc les unes les autres et seule
 * la derniere est executee. On voit defiler les images sous le doigt sans
 * payer un decodage par pixel parcouru. */
static gboolean sur_glissement(GtkRange *r, GtkScrollType t, double valeur, gpointer u)
{
    (void) r; (void) t;
    App *a = u;
    a->geste_us = g_get_monotonic_time();
    video_moteur_sauter(a->moteur, valeur);
    a->seconde_affichee = -1;
    rafraichir_position(a, valeur);
    reveler(a, TRUE, TRUE);
    return FALSE;
}

/* ------------------------------------------------------------- les pistes

   Un menu construit A CHAQUE OUVERTURE, et non une fois pour toutes : les
   pistes ne changent pas, mais la piste ACTIVE, si -- et un menu qui ne
   coche pas la bonne ligne est pire que pas de menu. */

typedef struct { App *a; VideoTypePiste type; int index; } Choix;

static void choix_libre(gpointer p, GClosure *c) { (void) c; g_free(p); }

static void sur_choix_piste(GtkCheckButton *b, gpointer u)
{
    Choix *c = u;
    if (!gtk_check_button_get_active(b)) return;
    video_moteur_choisir_piste(c->a->moteur, c->type, c->index);
}

static void ajouter_section(App *a, GtkWidget *boite, const char *titre,
                            VideoTypePiste type)
{
    GPtrArray *pistes = video_moteur_pistes(a->moteur, type);

    /* Une seule entree pour l'audio -- le cas courant -- ne merite pas une
     * section : on n'offre pas un choix qui n'en est pas un. */
    if (pistes->len <= 1) { g_ptr_array_unref(pistes); return; }

    GtkWidget *t = gtk_label_new(titre);
    gtk_widget_add_css_class(t, "video-menu-titre");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0);
    gtk_box_append(GTK_BOX(boite), t);

    GtkCheckButton *premier = NULL;
    for (guint i = 0; i < pistes->len; i++) {
        VideoPiste *p = g_ptr_array_index(pistes, i);
        GtkWidget *b = gtk_check_button_new_with_label(p->nom);
        gtk_check_button_set_group(GTK_CHECK_BUTTON(b), premier);
        if (!premier) premier = GTK_CHECK_BUTTON(b);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(b), p->active);

        Choix *c = g_new0(Choix, 1);
        c->a = a; c->type = type; c->index = p->index;
        g_signal_connect_data(b, "toggled", G_CALLBACK(sur_choix_piste),
                              c, choix_libre, 0);
        gtk_box_append(GTK_BOX(boite), b);
    }
    g_ptr_array_unref(pistes);
}

static void sur_menu_pistes(GtkMenuButton *b, GParamSpec *p, gpointer u)
{
    (void) p;
    App *a = u;
    if (!gtk_menu_button_get_active(b)) return;

    GtkWidget *boite = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(boite, "video-menu");
    ajouter_section(a, boite, "Piste audio", VIDEO_PISTE_AUDIO);
    ajouter_section(a, boite, "Sous-titres", VIDEO_PISTE_SOUS_TITRE);

    if (!gtk_widget_get_first_child(boite)) {
        GtkWidget *rien = gtk_label_new("Ce fichier n'a qu'une piste");
        gtk_widget_add_css_class(rien, "video-menu-titre");
        gtk_box_append(GTK_BOX(boite), rien);
    }

    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), boite);
    /* GTK_POS_TOP : ne PAS laisser GTK retourner le popover. Ne au ras du
     * bas de l'ecran il s'ouvrirait vers le bas puis serait retourne, et le
     * retournement annule le decalage -- piege paye par le centre de
     * notifications, il vaut ici aussi. */
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_TOP);
    gtk_menu_button_set_popover(b, pop);
}

/* -------------------------------------------------------------- clavier */

static gboolean sur_touche(GtkEventControllerKey *c, guint val, guint code,
                           GdkModifierType mod, gpointer u)
{
    (void) c; (void) code; (void) mod;
    App *a = u;
    switch (val) {
        case GDK_KEY_space:
        case GDK_KEY_k:      video_moteur_basculer(a->moteur);       return TRUE;
        case GDK_KEY_Left:   video_moteur_avancer(a->moteur, -10.0); return TRUE;
        case GDK_KEY_Right:  video_moteur_avancer(a->moteur, +10.0); return TRUE;
        case GDK_KEY_Down:   video_moteur_avancer(a->moteur,  -5.0); return TRUE;
        case GDK_KEY_Up:     video_moteur_avancer(a->moteur,  +5.0); return TRUE;
        case GDK_KEY_Home:   video_moteur_sauter(a->moteur, 0.0);    return TRUE;
        case GDK_KEY_Page_Up:   aller_a(a, a->rang - 1);             return TRUE;
        case GDK_KEY_Page_Down: aller_a(a, a->rang + 1);             return TRUE;
        case GDK_KEY_m:      act_sourdine(NULL, a);                  return TRUE;
        case GDK_KEY_f:
        case GDK_KEY_F11:    basculer_plein(a);                      return TRUE;
        case GDK_KEY_Escape:
            if (a->panneau && gtk_widget_get_visible(a->panneau)) {
                panneau_cacher(a);
                return TRUE;
            }
            if (gtk_window_is_fullscreen(GTK_WINDOW(a->fenetre)))
                gtk_window_unfullscreen(GTK_WINDOW(a->fenetre));
            else
                gtk_window_close(GTK_WINDOW(a->fenetre));
            return TRUE;
        case GDK_KEY_q:      gtk_window_close(GTK_WINDOW(a->fenetre)); return TRUE;
        case GDK_KEY_o:
        case GDK_KEY_O:
            if (mod & GDK_CONTROL_MASK) {
                /* Le moteur en cours est fermé par ouvrir_fichier ; on note
                 * d'abord où l'on en était, sinon la reprise du film qu'on
                 * quitte est perdue. */
                if (a->moteur && a->fichier)
                    reprise_ecrire(a->fichier, video_moteur_position(a->moteur),
                                   video_moteur_duree(a->moteur));
                demander_fichier(a);
                return TRUE;
            }
            return FALSE;
    }
    return FALSE;
}

/* ------------------------------------------------------------- scenario */

/* UN PARCOURS AUTOMATIQUE, PARCE QUE L'ECRAN N'EST PAS TOUJOURS DISPONIBLE.
 * Pause, reprise, sauts : les gestes qui cassent un lecteur, et ceux qu'on
 * ne peut pas eprouver sans les faire. Mode de banc, pas mode du lecteur. */
static gboolean sur_scenario(gpointer u)
{
    App *a = u;
    VideoMoteur *m = a->moteur;
    if (!m) return G_SOURCE_REMOVE;

    static const char *ATTENDU[] = {
        "lecture depuis le debut", "pause", "reprise",
        "saut a 40 s", "saut arriere de 10 s", "retour au debut", "fin"
    };
    g_message("scenario : etape %d (%s) -- position %.2f s, etat %d",
              a->etape, ATTENDU[MIN(a->etape, 6)],
              video_moteur_position(m), video_moteur_etat(m));

    switch (a->etape++) {
        case 0: video_moteur_pause(m);          break;
        case 1: video_moteur_lire(m);           break;
        case 2: video_moteur_sauter(m, 40.0);   break;
        case 3: video_moteur_avancer(m, -10.0); break;
        case 4: video_moteur_sauter(m, 0.0);    break;
        default:
            bilan(a, "fin du scenario");
            gtk_window_close(GTK_WINDOW(a->fenetre));
            return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* -------------------------------------------------------------- fenetre */

static void sur_apparition(GtkWidget *w, gpointer u)
{
    (void) w;
    App *a = u;
    battement_selon(a, video_moteur_etat(a->moteur) == VIDEO_LIT);
}

static void sur_disparition(GtkWidget *w, gpointer u)
{ (void) w; battement_selon((App *)u, FALSE); }

static gboolean sur_fermeture(GtkWindow *w, gpointer u)
{
    (void) w;
    App *a = u;
    bilan(a, "fermeture");
    if (a->appui_simple) { g_source_remove(a->appui_simple); a->appui_simple = 0; }
    if (a->annulation)   g_cancellable_cancel(a->annulation);
    g_clear_pointer(&a->dossier, g_ptr_array_unref);
    if (a->moteur && a->fichier)
        reprise_ecrire(a->fichier, video_moteur_position(a->moteur),
                       video_moteur_duree(a->moteur));
    battement_selon(a, FALSE);
    if (a->retrait) { g_source_remove(a->retrait); a->retrait = 0; }
    /* Le moteur ferme son fil AVANT que la fenetre ne disparaisse : un fil
     * de decodage qui pousse une image dans une file detruite ne se voit
     * qu'au coredump. */
    g_clear_pointer(&a->moteur, video_moteur_fermer);
    video_image_fin(&a->images);
    return FALSE;
}

static GtkWidget *bouton(const char *icone, const char *classe,
                         GCallback rappel, App *a)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icone);
    gtk_widget_add_css_class(b, "video-bouton");
    if (classe) gtk_widget_add_css_class(b, classe);
    /* Un bouton qui prend le focus au clic coupe la frappe physique --
     * lecon de l'ecran de connexion, elle vaut ici aussi : l'espace doit
     * mettre en pause meme apres avoir clique sur un bouton. */
    gtk_widget_set_focus_on_click(b, FALSE);
    if (rappel) g_signal_connect(b, "clicked", rappel, a);
    return b;
}

static GtkWidget *construire_capsule(App *a)
{
    GtkWidget *c = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(c, "video-capsule");
    gtk_widget_set_halign(c, GTK_ALIGN_CENTER);

    a->l_position = gtk_label_new("0:00");
    gtk_widget_add_css_class(a->l_position, "video-temps");
    /* Une largeur fixe : sinon la capsule change de taille au passage de 9 a
     * 10 secondes, et tous les boutons se decalent sous le doigt. */
    gtk_label_set_width_chars(GTK_LABEL(a->l_position), 5);
    gtk_label_set_xalign(GTK_LABEL(a->l_position), 1.0);

    a->l_duree = gtk_label_new("0:00");
    gtk_widget_add_css_class(a->l_duree, "video-temps");
    gtk_widget_add_css_class(a->l_duree, "video-temps-total");
    gtk_label_set_width_chars(GTK_LABEL(a->l_duree), 5);
    gtk_label_set_xalign(GTK_LABEL(a->l_duree), 0.0);

    a->b_lecture = bouton("media-playback-start-symbolic", "video-bouton-grand",
                          G_CALLBACK(act_basculer), a);
    a->b_prec = bouton("media-skip-backward-symbolic", NULL,
                       G_CALLBACK(act_precedente), a);
    a->b_suiv = bouton("media-skip-forward-symbolic", NULL,
                       G_CALLBACK(act_suivante), a);
    gtk_widget_set_tooltip_text(a->b_prec, "Vidéo précédente");
    gtk_widget_set_tooltip_text(a->b_suiv, "Vidéo suivante");
    gtk_widget_set_sensitive(a->b_prec, FALSE);
    gtk_widget_set_sensitive(a->b_suiv, FALSE);
    a->b_plein   = bouton("view-fullscreen-symbolic", NULL,
                          G_CALLBACK(act_plein), a);
    a->b_son     = bouton("audio-volume-high-symbolic", NULL,
                          G_CALLBACK(act_sourdine), a);

    a->volume = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01);
    gtk_widget_add_css_class(a->volume, "video-volume");
    gtk_range_set_value(GTK_RANGE(a->volume), 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(a->volume), FALSE);
    gtk_widget_set_size_request(a->volume, 96, -1);
    gtk_widget_set_valign(a->volume, GTK_ALIGN_CENTER);
    g_signal_connect(a->volume, "value-changed", G_CALLBACK(sur_volume), a);

    a->l_codec = gtk_label_new("");
    gtk_widget_add_css_class(a->l_codec, "video-codec");

    a->b_ouvrir = bouton("document-open-symbolic", NULL,
                         G_CALLBACK(act_ouvrir), a);
    gtk_widget_set_tooltip_text(a->b_ouvrir, "Ouvrir une vidéo…");

    a->b_pistes = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(a->b_pistes),
                                  "media-view-subtitles-symbolic");
    gtk_widget_add_css_class(a->b_pistes, "video-bouton");
    gtk_widget_set_focus_on_click(a->b_pistes, FALSE);
    gtk_widget_set_tooltip_text(a->b_pistes, "Pistes et sous-titres");
    g_signal_connect(a->b_pistes, "notify::active",
                     G_CALLBACK(sur_menu_pistes), a);

    GtkWidget *barre = gtk_label_new("/");
    gtk_widget_add_css_class(barre, "video-temps");

    gtk_box_append(GTK_BOX(c), a->l_position);
    gtk_box_append(GTK_BOX(c), barre);
    gtk_box_append(GTK_BOX(c), a->l_duree);
    gtk_box_append(GTK_BOX(c), a->b_prec);
    gtk_box_append(GTK_BOX(c), a->b_lecture);
    gtk_box_append(GTK_BOX(c), a->b_suiv);
    gtk_box_append(GTK_BOX(c), a->b_son);
    gtk_box_append(GTK_BOX(c), a->volume);
    gtk_box_append(GTK_BOX(c), a->l_codec);
    gtk_box_append(GTK_BOX(c), a->b_ouvrir);
    gtk_box_append(GTK_BOX(c), a->b_pistes);
    gtk_box_append(GTK_BOX(c), a->b_plein);
    return c;
}

/* L'ESPACE VIDE ET SA GLISSIERE. Une GtkOverlay, et non une boite : la
 * glissiere doit apparaitre PAR-DESSUS le vide, sans en changer la hauteur.
 * Toute autre disposition ferait sauter la video a chaque survol. */
static GtkWidget *construire_zone(App *a)
{
    GtkWidget *vide = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(vide, -1, ZONE_VIDE_PX);

    a->glissiere = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                            0.0, 1.0, 0.1);
    gtk_widget_add_css_class(a->glissiere, "video-glissiere");
    gtk_scale_set_draw_value(GTK_SCALE(a->glissiere), FALSE);
    gtk_widget_set_valign(a->glissiere, GTK_ALIGN_CENTER);
    gtk_widget_set_focus_on_click(a->glissiere, FALSE);
    g_signal_connect(a->glissiere, "change-value", G_CALLBACK(sur_glissement), a);

    a->reveleur = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(a->reveleur),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(a->reveleur), 140);
    gtk_revealer_set_child(GTK_REVEALER(a->reveleur), a->glissiere);
    gtk_widget_set_valign(a->reveleur, GTK_ALIGN_CENTER);

    a->zone = gtk_overlay_new();
    gtk_widget_add_css_class(a->zone, "video-zone");
    gtk_overlay_set_child(GTK_OVERLAY(a->zone), vide);
    gtk_overlay_add_overlay(GTK_OVERLAY(a->zone), a->reveleur);

    GtkEventController *survol = gtk_event_controller_motion_new();
    g_signal_connect(survol, "enter", G_CALLBACK(sur_entree_zone), a);
    g_signal_connect(survol, "leave", G_CALLBACK(sur_sortie_zone), a);
    gtk_widget_add_controller(a->zone, survol);
    return a->zone;
}

/* OU EST L'IMAGE, EXACTEMENT.
 *
 * GtkPicture en mode CONTAIN centre la video et la borde de vide : sa
 * largeur affichee n'est PAS celle du widget des que les proportions
 * different. Or la glissiere doit faire la largeur de l'image -- pas celle
 * de la fenetre -- et la croix se poser dans le coin de l'image. Les deux
 * ont donc besoin de ce calcul, et personne d'autre ne le connait. */
static void geometrie_image(App *a, int cadre_l, int cadre_h,
                            int *large, int *haut)
{
    int vl = video_moteur_largeur(a->moteur);
    int vh = video_moteur_hauteur(a->moteur);

    if (vl <= 0 || vh <= 0 || cadre_l <= 0 || cadre_h <= 0) {
        *large = MAX(cadre_l, 0);
        *haut  = MAX(cadre_h, 0);
        return;
    }

    double r = (double)vl / (double)vh;
    int l = (int)(cadre_h * r + 0.5);
    if (l <= cadre_l) { *large = l; *haut = cadre_h; }
    else              { *large = cadre_l; *haut = (int)(cadre_l / r + 0.5); }
}

/* LA MARGE QUI TIENT LES DEUX MISES EN PAGE.
 *
 * Les commandes flottent TOUJOURS dans une GtkOverlay, au bas de l'image.
 * Ce qui change entre fenetre et plein ecran, c'est la marge basse de
 * l'image :
 *
 *   en fenetre      marge = hauteur des commandes -> elles sont SOUS la
 *                                                    video, sur le fond
 *                                                    transparent
 *   en plein ecran  marge = 0                     -> elles flottent PAR-
 *                                                    DESSUS l'image
 *
 * L'alternative -- les remettre dans la pile en plein ecran -- ferait sauter
 * l'image de cent pixels a chaque mouvement de souris. Et la hauteur est
 * MESUREE, jamais devinee : une constante en dur se decalerait au premier
 * changement de police ou de theme. */
static void ajuster_marge(App *a)
{
    int marge = 0;
    if (!a->plein_ecran && a->commandes) {
        int min = 0, nat = 0;
        gtk_widget_measure(a->commandes, GTK_ORIENTATION_VERTICAL, -1,
                           &min, &nat, NULL, NULL);
        marge = nat;
    }
    if (a->offload) gtk_widget_set_margin_bottom(a->offload, marge);
}

/* LE PLACEMENT DES DEUX FLOTTANTS, calcule plutot que confie a des
 * alignements : la glissiere doit faire la largeur de l'IMAGE, et la croix
 * se poser dans le coin haut droit de l'IMAGE. Ni l'une ni l'autre ne
 * s'exprime en termes de fenetre. */
static gboolean sur_position_flottant(GtkOverlay *o, GtkWidget *enfant,
                                      GdkRectangle *alloc, gpointer u)
{
    App *a = u;
    int W = gtk_widget_get_width(GTK_WIDGET(o));
    int H = gtk_widget_get_height(GTK_WIDGET(o));
    if (W <= 0 || H <= 0) return FALSE;

    int min = 0, nat_h = 0, nat_l = 0;
    gtk_widget_measure(a->commandes, GTK_ORIENTATION_VERTICAL, -1,
                       &min, &nat_h, NULL, NULL);

    /* Le cadre reellement donne a l'image : la fenetre, moins la place
     * reservee aux commandes en mode fenetre. */
    int cadre_h = H - (a->plein_ecran ? 0 : nat_h);
    int large = 0, haut = 0;
    geometrie_image(a, W, cadre_h, &large, &haut);

    if (enfant == a->commandes) {
        alloc->x      = (W - large) / 2;
        alloc->width  = large;
        alloc->y      = H - nat_h;
        alloc->height = nat_h;
        return TRUE;
    }

    if (enfant == a->accueil) return FALSE;   /* centre, GTK s'en charge */

    if (enfant == a->panneau) {
        /* Il occupe la fenetre en laissant voir un liseré de l'image
         * autour : on n'a pas quitte le lecteur, on ouvre un tiroir. */
        int marge = MIN(40, MIN(W, H) / 12);
        alloc->x = marge;  alloc->y = marge;
        alloc->width  = W - 2 * marge;
        alloc->height = H - 2 * marge;
        return TRUE;
    }

    if (enfant == a->rev_fermer) {
        gtk_widget_measure(a->rev_fermer, GTK_ORIENTATION_HORIZONTAL, -1,
                           &min, &nat_l, NULL, NULL);
        gtk_widget_measure(a->rev_fermer, GTK_ORIENTATION_VERTICAL, -1,
                           &min, &nat_h, NULL, NULL);
        /* Dans le coin de l'IMAGE, pas de la fenetre : sur une video au
         * format different, le coin de la fenetre est du vide. */
        int haut_image = (cadre_h - haut) / 2;
        alloc->x      = (W + large) / 2 - nat_l - 8;
        alloc->y      = haut_image + 8;
        alloc->width  = nat_l;
        alloc->height = nat_h;
        return TRUE;
    }
    return FALSE;
}

static void sur_commandes_affichees(GtkWidget *w, gpointer u)
{
    (void) w;
    ajuster_marge((App *)u);
}

static void construire(App *a)
{
    a->fenetre = gtk_application_window_new(a->app);
    gtk_window_set_title(GTK_WINDOW(a->fenetre), "Vidéo");
    gtk_window_set_default_size(GTK_WINDOW(a->fenetre), 1280, 760);
    gtk_widget_add_css_class(a->fenetre, "video-fenetre");

    /* NI BORDURE NI BARRE DE TITRE, et un fond transparent : il ne doit
     * rester a l'ecran que l'image, la capsule, et la glissiere quand on la
     * demande. Le compositeur ne decore pas ce qu'on declare non decore.
     *
     * Contrepartie assumee : la fenetre n'a plus de barre a saisir. Elle se
     * deplace a l'Alt-glisser de labwc, ou en tirant LA CAPSULE -- qui est un
     * GtkWindowHandle pour cette raison. */
    gtk_window_set_decorated(GTK_WINDOW(a->fenetre), FALSE);

    a->image = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(a->image), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_hexpand(a->image, TRUE);
    gtk_widget_set_vexpand(a->image, TRUE);
    g_signal_connect(a->image, "map",   G_CALLBACK(sur_apparition),  a);
    g_signal_connect(a->image, "unmap", G_CALLBACK(sur_disparition), a);

    /* LE WIDGET QUI PORTE TOUT LE BENEFICE. Il ne dessine rien : il demande a
     * GDK de confier l'image au compositeur, dans une sous-surface, plutot
     * que de la composer dans la fenetre. Voir docs/11.
     *
     * C'est aussi lui qui donne la « video sans aucune bordure » : le tampon
     * est pose tel quel, il n'y a materiellement rien autour. */
    a->offload = gtk_graphics_offload_new(a->image);
    gtk_graphics_offload_set_enabled(GTK_GRAPHICS_OFFLOAD(a->offload),
                                     a->sans_offload
                                        ? GTK_GRAPHICS_OFFLOAD_DISABLED
                                        : GTK_GRAPHICS_OFFLOAD_ENABLED);

    /* Le sous-titre par-dessus l'image. Cache tant qu'il n'y a rien a dire. */
    a->st_texte = gtk_label_new("");
    gtk_widget_add_css_class(a->st_texte, "video-sous-titre");
    gtk_label_set_justify(GTK_LABEL(a->st_texte), GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(GTK_LABEL(a->st_texte), TRUE);
    gtk_widget_set_halign(a->st_texte, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(a->st_texte, GTK_ALIGN_END);
    gtk_widget_set_visible(a->st_texte, FALSE);
    gtk_widget_set_can_target(a->st_texte, FALSE);   /* il ne vole pas l'appui */

    /* La capsule : dans un reveleur, parce qu'elle s'efface en plein ecran ;
     * dans un GtkWindowHandle, parce qu'elle sert de poignee de fenetre. */
    a->capsule = construire_capsule(a);
    GtkWidget *poignee = gtk_window_handle_new();
    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(poignee), a->capsule);
    gtk_widget_set_halign(poignee, GTK_ALIGN_CENTER);

    a->rev_capsule = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(a->rev_capsule),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(a->rev_capsule), 140);
    gtk_revealer_set_child(GTK_REVEALER(a->rev_capsule), poignee);
    gtk_revealer_set_reveal_child(GTK_REVEALER(a->rev_capsule), TRUE);
    gtk_widget_set_halign(a->rev_capsule, GTK_ALIGN_CENTER);

    a->commandes = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(a->commandes), construire_zone(a));
    gtk_box_append(GTK_BOX(a->commandes), a->rev_capsule);
    gtk_widget_set_valign(a->commandes, GTK_ALIGN_END);
    g_signal_connect(a->commandes, "map", G_CALLBACK(sur_commandes_affichees), a);

    /* LA CROIX, dans le coin haut droit de l'image. Sans barre de titre,
     * c'est la seule sortie a la souris -- voir croix_montrer(). */
    GtkWidget *croix = bouton("window-close-symbolic", "video-croix",
                              G_CALLBACK(act_fermer), a);
    gtk_widget_set_tooltip_text(croix, "Fermer");

    a->rev_fermer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(a->rev_fermer),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(a->rev_fermer), 140);
    gtk_revealer_set_child(GTK_REVEALER(a->rev_fermer), croix);
    gtk_revealer_set_reveal_child(GTK_REVEALER(a->rev_fermer), FALSE);

    /* L'ACCUEIL : ce que montre une fenetre sans film. Sans lui, il ne
     * resterait a l'ecran qu'une capsule flottante aux boutons eteints, sur
     * un fond transparent -- rien qui dise quoi faire. */
    a->accueil = gtk_button_new_with_label("Ouvrir une vidéo…");
    gtk_widget_add_css_class(a->accueil, "video-accueil");
    gtk_widget_set_halign(a->accueil, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(a->accueil, GTK_ALIGN_CENTER);
    g_signal_connect(a->accueil, "clicked", G_CALLBACK(act_ouvrir), a);

    a->racine = gtk_overlay_new();
    gtk_widget_add_css_class(a->racine, "video-pile");
    gtk_overlay_set_child(GTK_OVERLAY(a->racine), a->offload);
    gtk_overlay_add_overlay(GTK_OVERLAY(a->racine), a->st_texte);
    gtk_overlay_add_overlay(GTK_OVERLAY(a->racine), a->commandes);
    gtk_overlay_add_overlay(GTK_OVERLAY(a->racine), a->rev_fermer);
    gtk_overlay_add_overlay(GTK_OVERLAY(a->racine), a->accueil);

    a->panneau = construire_panneau(a);
    gtk_overlay_add_overlay(GTK_OVERLAY(a->racine), a->panneau);
    g_signal_connect(a->racine, "get-child-position",
                     G_CALLBACK(sur_position_flottant), a);

    GtkWidget *racine = a->racine;
    gtk_window_set_child(GTK_WINDOW(a->fenetre), racine);

    GtkEventController *clavier = gtk_event_controller_key_new();
    g_signal_connect(clavier, "key-pressed", G_CALLBACK(sur_touche), a);
    gtk_widget_add_controller(a->fenetre, clavier);

    /* L'APPUI EST POSE SUR L'IMAGE, ET SUR ELLE SEULE.
     *
     * Pose sur la racine -- ce qu'on avait fait d'abord --, il recevait AUSSI
     * les clics deja traites par les boutons de la capsule : le bouton
     * lecture basculait, puis l'appui retarde rebasculait 300 ms plus tard.
     * « Play/Pause instantanement », exactement ce que l'utilisateur a vu.
     *
     * Sur l'image, un clic de bouton ne nous parvient plus. En plein ecran,
     * la bande des commandes cachees cesse d'etre une cible (voir
     * appliquer_visibilite) pour que l'appui y atteigne l'image. */
    GtkGesture *appui = gtk_gesture_click_new();
    g_signal_connect(appui, "pressed", G_CALLBACK(sur_appui), a);
    gtk_widget_add_controller(a->image, GTK_EVENT_CONTROLLER(appui));

    /* Le mouvement de pointeur rappelle les commandes, en plein ecran. */
    GtkEventController *mouvement = gtk_event_controller_motion_new();
    g_signal_connect(mouvement, "motion", G_CALLBACK(sur_mouvement), a);
    gtk_widget_add_controller(racine, mouvement);

    g_signal_connect(a->fenetre, "notify::fullscreened",
                     G_CALLBACK(sur_plein_change), a);
    g_signal_connect(a->fenetre, "close-request", G_CALLBACK(sur_fermeture), a);
}

static void ouvrir_fichier(App *a, const char *chemin)
{
    GError *err = NULL;
    VideoRappels r = { sur_etat, sur_fin, sur_erreur };

    /* Remplacer un film par un autre : on arrête le battement AVANT de
     * fermer le moteur, sinon le prochain tick va chercher une image dans
     * un moteur qui n'existe plus. */
    if (a->moteur) {
        battement_selon(a, FALSE);
        g_clear_pointer(&a->moteur, video_moteur_fermer);
    }

    a->moteur = video_moteur_ouvrir(chemin, &r, a, &err);
    if (!a->moteur) {
        /* INVARIANT N.4 : on ne meurt pas en silence. */
        g_printerr("video : %s\n", err ? err->message : "ouverture impossible");
        g_clear_error(&err);
        gtk_window_close(GTK_WINDOW(a->fenetre));
        return;
    }

    gtk_widget_set_visible(a->accueil, FALSE);

    double duree = video_moteur_duree(a->moteur);
    g_message("video : %s -- %s %dx%d, %s, %.1f s",
              chemin, video_moteur_codec(a->moteur),
              video_moteur_largeur(a->moteur), video_moteur_hauteur(a->moteur),
              video_moteur_materiel(a->moteur) ? "décodage matériel"
                                               : "décodage logiciel",
              duree);

    gchar *t = duree_texte(duree);
    gtk_label_set_text(GTK_LABEL(a->l_duree), t);
    g_free(t);

    gtk_range_set_range(GTK_RANGE(a->glissiere), 0.0, MAX(duree, 0.1));

    gchar *nom = g_path_get_basename(chemin);
    gtk_window_set_title(GTK_WINDOW(a->fenetre), nom);
    g_free(nom);

    /* DIRE QUAND ON PAIE. Sur cette machine, un codec non accelere -- AV1 --
     * occupe les quatre coeurs. Le taire ferait passer une lecture couteuse
     * pour une lecture ordinaire. */
    if (!video_moteur_materiel(a->moteur)) {
        gtk_label_set_text(GTK_LABEL(a->l_codec), "décodage logiciel");
        gtk_widget_set_tooltip_text(a->l_codec,
            "Ce format n'est pas accéléré par le matériel de cette machine : "
            "la lecture consomme davantage.");
    }

    /* Les sous-titres ne s'activent pas d'office : afficher une langue que
     * personne n'a demandee serait presomptueux, et cela couterait le chemin
     * sans copie a qui n'en veut pas. Le bouton, lui, est la. */
    if (!video_moteur_a_audio(a->moteur)) {
        gtk_widget_set_sensitive(a->b_son, FALSE);
        gtk_widget_set_sensitive(a->volume, FALSE);
    }

    double reprise = reprise_lire(chemin);
    if (reprise > 0.0 && reprise < duree - REPRISE_FIN_MARGE) {
        video_moteur_sauter(a->moteur, reprise);
        /* On MONTRE la reprise plutot que de la subir : les commandes
         * apparaissent quelques secondes, la glissiere dit ou l'on est, et
         * revenir au debut est a un geste. Un lecteur qui repart au milieu
         * sans rien dire donne l'impression de s'etre trompe de fichier. */
        reveler(a, TRUE, TRUE);
        g_message("video : reprise à %.0f s", reprise);
    }

    /* Les proportions de la video viennent de changer : la glissiere et la
     * croix se placent d'apres elles, il faut refaire le calcul. */
    if (a->racine) gtk_widget_queue_allocate(a->racine);
    ajuster_marge(a);

    /* Le dossier est relu a chaque ouverture : on peut avoir change de
     * dossier par le selecteur, et le contenu a pu bouger entre-temps. */
    if (g_file_test(chemin, G_FILE_TEST_EXISTS)) dossier_charger(a, chemin);

    a->depart = g_get_monotonic_time();
    a->seconde_affichee = -1;
    video_moteur_lire(a->moteur);
    rafraichir_bouton_lecture(a);

    if (a->avec_st) {
        /* Mode de banc : sans pointeur on ne peut pas ouvrir le menu, et
         * c'est pourtant le seul moyen d'eprouver le rendu des sous-titres
         * et ce qu'ils coutent au chemin sans copie. */
        GPtrArray *st = video_moteur_pistes(a->moteur, VIDEO_PISTE_SOUS_TITRE);
        for (guint i = 0; i < st->len; i++) {
            VideoPiste *p = g_ptr_array_index(st, i);
            if (p->index >= 0) {
                video_moteur_choisir_piste(a->moteur, VIDEO_PISTE_SOUS_TITRE,
                                           p->index);
                break;
            }
        }
        g_ptr_array_unref(st);
    }
    if (a->revele) {
        reveler(a, TRUE, FALSE);
        /* Mode de banc : la croix ne se montre qu'au pointeur, que le banc
         * sans ecran n'a pas. */
        gtk_revealer_set_reveal_child(GTK_REVEALER(a->rev_fermer), TRUE);
    }
    if (a->scenario) g_timeout_add_seconds(3, sur_scenario, a);
}

/* ------------------------------------------------------ choisir un fichier

   LANCÉ SANS FICHIER, UN LECTEUR DOIT DEMANDER LEQUEL.
   
   La première version écrivait un mode d'emploi sur la sortie d'erreur et
   quittait. Depuis un terminal, c'est acceptable ; depuis le menu du bureau,
   où le .desktop ne passe aucun argument, cela donne une application qui
   « ne démarre pas » -- sans fenêtre, sans message, sans rien. Constaté sur
   MADOO le 10 septembre 2026, et c'est le premier essai qui l'a montré. */

/* LES LECTEURS RESEAU, DANS LE SELECTEUR.
 *
 * Ils sont montes sous /run/claude-os/reseau/<nom>. GIO ne les propose PAS
 * dans sa barre laterale : g_unix_mount_guess_should_display() ne retient
 * que /media, /run/media/<user> et le dossier personnel. Un partage monte,
 * parfaitement accessible, reste donc invisible de toute boite « Ouvrir ».
 *
 * On les ajoute donc a la main, lus dans /proc/mounts -- la source de
 * verite, plutot que la configuration : ce qui compte est ce qui est monte
 * MAINTENANT, pas ce qui est declare. */
static GPtrArray *lecteurs_reseau_montes(void)
{
    GPtrArray *points = g_ptr_array_new_with_free_func(g_free);

    g_autofree gchar *contenu = NULL;
    if (!g_file_get_contents("/proc/mounts", &contenu, NULL, NULL))
        return points;

    g_auto(GStrv) lignes = g_strsplit(contenu, "\n", -1);
    for (char **l = lignes; l && *l; l++) {
        g_auto(GStrv) champs = g_strsplit(*l, " ", 3);
        if (!champs[0] || !champs[1]) continue;
        if (!g_str_has_prefix(champs[1], "/run/claude-os/reseau/")) continue;

        /* /proc/mounts echappe les espaces en \040 : sans cela, un partage
         * nomme « Mes vidéos » donnerait un chemin tronque. */
        g_autofree gchar *point = g_strcompress(champs[1]);
        g_ptr_array_add(points, g_steal_pointer(&point));
    }
    return points;
}

/* ================================================ LE SELECTEUR EST UN PANNEAU

   ET NON UNE FENETRE, APRES DEUX PIEGES.

   Version 1 : une boite MODALE. Le 10 septembre, elle s'est glissee derriere
   une autre application ; son « Annuler » est passe hors de vue, et la
   fenetre dessous -- inerte parce que modale, et sans barre de titre --
   n'offrait plus rien a cliquer. Application enfermee.

   Version 2 : la meme, NON modale. Le 11 septembre, l'utilisateur decrit une
   fenetre « gelee au sens strict » : aucun clic ne passe, elle ne remonte
   meme pas au premier plan, et seul un clic droit sur l'icone du dock permet
   de fermer l'application. Une fenetre qui ne remonte pas au clic ne recoit
   aucun evenement : ce n'est pas de la lenteur, c'est une surface que le
   compositeur ne lui destine pas.

   Deux causes possibles -- le focus donne au parent, ou un empilement ou la
   fenetre transparente du lecteur passe par-dessus sa propre boite -- et
   toutes deux tiennent a la MEME chose : il y avait deux fenetres.

   Version 3, celle-ci : il n'y en a plus qu'une. Le selecteur est un panneau
   posé dans la fenetre du lecteur, dans la meme GtkOverlay que la capsule.
   Plus d'empilement, plus de focus a negocier, plus de transitoire : ce qui
   est visible est cliquable, par construction.

   Le prix : GtkFileChooserWidget est deprecie depuis GTK 4.10, comme la
   boite qu'il remplace. On l'assume -- l'API recommandee ne sait ni
   s'embarquer dans une fenetre, ni montrer les lecteurs reseau. */

static void panneau_cacher(App *a)
{
    if (!a->panneau) return;
    gtk_widget_set_visible(a->panneau, FALSE);
    gtk_widget_set_visible(a->accueil, a->moteur == NULL);
}

static void panneau_choisir(App *a)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    g_autoptr(GFile) f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(a->choix));
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (!f) return;

    g_autofree gchar *chemin = g_file_get_path(f);
    g_autofree gchar *uri    = g_file_get_uri(f);

    /* « Ouvrir » sur un DOSSIER y entre, il ne reste pas sans rien faire :
     * un bouton qui ne repond pas est indiscernable d'une application
     * bloquee, et ce lecteur en a deja assez donne sur ce chapitre. */
    if (chemin && g_file_test(chemin, G_FILE_TEST_IS_DIR)) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(a->choix), f, NULL);
        G_GNUC_END_IGNORE_DEPRECATIONS
        return;
    }

    panneau_cacher(a);

    if (a->moteur && a->fichier)
        reprise_ecrire(a->fichier, video_moteur_position(a->moteur),
                       video_moteur_duree(a->moteur));

    g_free(a->fichier);
    a->fichier = g_strdup(chemin ? chemin : uri);
    if (chemin) dernier_dossier_ecrire(chemin);
    ouvrir_fichier(a, a->fichier);
}

static void act_panneau_ouvrir(GtkButton *b, gpointer u) { (void) b; panneau_choisir((App *)u); }
static void act_panneau_annuler(GtkButton *b, gpointer u) { (void) b; panneau_cacher((App *)u); }
static void sur_fichier_active(GtkWidget *w, gpointer u) { (void) w; panneau_choisir((App *)u); }

static GtkWidget *construire_panneau(App *a)
{
    GtkWidget *boite = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(boite, "video-panneau");

    GtkWidget *barre = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(barre, "video-panneau-barre");

    GtkWidget *annuler = gtk_button_new_with_label("Annuler");
    g_signal_connect(annuler, "clicked", G_CALLBACK(act_panneau_annuler), a);

    GtkWidget *titre = gtk_label_new("Ouvrir une vidéo");
    gtk_widget_add_css_class(titre, "video-panneau-titre");
    gtk_widget_set_hexpand(titre, TRUE);

    a->b_valider = gtk_button_new_with_label("Ouvrir");
    gtk_widget_add_css_class(a->b_valider, "suggested-action");
    g_signal_connect(a->b_valider, "clicked", G_CALLBACK(act_panneau_ouvrir), a);

    gtk_box_append(GTK_BOX(barre), annuler);
    gtk_box_append(GTK_BOX(barre), titre);
    gtk_box_append(GTK_BOX(barre), a->b_valider);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    a->choix = gtk_file_chooser_widget_new(GTK_FILE_CHOOSER_ACTION_OPEN);
    GtkFileChooser *ch = GTK_FILE_CHOOSER(a->choix);

    GtkFileFilter *videos = gtk_file_filter_new();
    gtk_file_filter_set_name(videos, "Vidéos");
    gtk_file_filter_add_mime_type(videos, "video/*");
    gtk_file_chooser_add_filter(ch, videos);

    GtkFileFilter *tout = gtk_file_filter_new();
    gtk_file_filter_set_name(tout, "Tous les fichiers");
    gtk_file_filter_add_pattern(tout, "*");
    gtk_file_chooser_add_filter(ch, tout);

    /* LES LECTEURS RESEAU, A LA MAIN.
     *
     * Ils sont montes sous /run/claude-os/reseau/<nom>, et
     * g_unix_mount_guess_should_display() ne retient que /media,
     * /run/media/<user> et le dossier personnel : un partage monte,
     * parfaitement accessible, reste invisible de toute boite « Ouvrir ». */
    g_autoptr(GPtrArray) reseau = lecteurs_reseau_montes();
    for (guint i = 0; i < reseau->len; i++) {
        const char *point = g_ptr_array_index(reseau, i);
        g_autoptr(GFile) f = g_file_new_for_path(point);
        GError *e = NULL;
        if (!gtk_file_chooser_add_shortcut_folder(ch, f, &e)) {
            g_message("video : lecteur réseau « %s » non ajouté (%s)",
                      point, e ? e->message : "?");
            g_clear_error(&e);
        }
    }
    G_GNUC_END_IGNORE_DEPRECATIONS

    g_signal_connect(a->choix, "file-activated", G_CALLBACK(sur_fichier_active), a);
    gtk_widget_set_vexpand(a->choix, TRUE);

    gtk_box_append(GTK_BOX(boite), barre);
    gtk_box_append(GTK_BOX(boite), a->choix);
    gtk_widget_set_visible(boite, FALSE);
    return boite;
}

static void demander_fichier(App *a)
{
    if (!a->panneau) return;

    /* OUVRIR SUR UN VRAI DOSSIER, ET JAMAIS SUR « RECENTS ».
     *
     * C'est le mode par defaut de GTK, et sur cette machine il ne montre que
     * deux entrees sans rapport : le panneau a l'air vide, donc casse. */
    g_autofree gchar *depart = NULL;
    if (a->fichier) {
        g_autoptr(GFile) f = g_file_new_for_path(a->fichier);
        g_autoptr(GFile) dir = g_file_get_parent(f);
        if (dir) depart = g_file_get_path(dir);
    }
    if (!depart) depart = dernier_dossier_lire();
    if (!depart || !g_file_test(depart, G_FILE_TEST_IS_DIR)) {
        g_free(depart);
        depart = g_strdup(g_get_home_dir());
    }

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    g_autoptr(GFile) fdep = g_file_new_for_path(depart);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(a->choix), fdep, NULL);
    G_GNUC_END_IGNORE_DEPRECATIONS

    gtk_widget_set_visible(a->accueil, FALSE);
    gtk_widget_set_visible(a->panneau, TRUE);
    gtk_widget_grab_focus(a->choix);
}


static void sur_demarrage(GtkApplication *app, gpointer cfg)
{
    shell_styles_startup(app, cfg);
}

static void sur_activation(GApplication *app, gpointer u)
{
    App *a = u;
    ShellConfig *cfg = g_object_get_data(G_OBJECT(app), "cfg");
    if (cfg) {
        shell_config_apply(cfg);
        /* Les widgets GTK ordinaires -- infobulles, boites de dialogue -- ne
         * passent pas par notre feuille de style. Sans cela ils resteraient
         * clairs dans une fenetre sombre. */
        g_object_set(gtk_settings_get_default(),
                     "gtk-application-prefer-dark-theme", cfg->dark, NULL);
    }

    a->annulation = g_cancellable_new();
    a->rang = -1;
    video_image_init(&a->images);
    construire(a);
    /* LANCEE SANS FICHIER, L'APPLICATION N'OUVRE PLUS RIEN D'AUTORITE.
     *
     * Elle affichait aussitot la boite de selection. Ce n'est pas ce qu'on
     * attend d'une application qui s'ouvre : elle montre desormais son
     * invitation -- « Ouvrir une vidéo… » -- et c'est l'utilisateur qui
     * decide. */
    if (a->plein) gtk_window_fullscreen(GTK_WINDOW(a->fenetre));
    gtk_window_present(GTK_WINDOW(a->fenetre));

    if (a->fichier) {
        ouvrir_fichier(a, a->fichier);
    } else {
        /* Sans film, la croix reste affichee : c'est la seule sortie d'une
         * fenetre sans barre de titre, et elle ne doit pas dependre d'un
         * mouvement de souris qui n'a peut-etre pas eu lieu. */
        gtk_revealer_set_reveal_child(GTK_REVEALER(a->rev_fermer), TRUE);
        if (a->retrait_fermer) {
            g_source_remove(a->retrait_fermer);
            a->retrait_fermer = 0;
        }
        if (a->ouvrir_au_depart) demander_fichier(a);
    }
}

int main(int argc, char **argv)
{
    App a = {0};

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--plein-ecran")) {
            a.plein = TRUE;
        } else if (!strcmp(argv[i], "--scenario")) {
            a.scenario = TRUE;
        } else if (!strcmp(argv[i], "--sous-titres")) {
            a.avec_st = TRUE;
        } else if (!strcmp(argv[i], "--ouvrir")) {
            a.ouvrir_au_depart = TRUE;
        } else if (!strcmp(argv[i], "--sans-offload")) {
            a.sans_offload = TRUE;
        } else if (!strcmp(argv[i], "--revele")) {
            /* Mode de banc : sans pointeur, le survol ne peut pas etre
             * joue ; on montre la glissiere pour pouvoir la regarder. */
            a.revele = TRUE;
        } else if (g_str_has_prefix(argv[i], "--essai=")) {
            a.duree_essai = atoi(argv[i] + 8);
        } else if (argv[i][0] != '-') {
            /* LE .desktop PASSE UNE URI, PAS UN CHEMIN. « Exec=… %U » donne
             * « file:///home/… » ; g_file_new_for_commandline_arg accepte
             * les deux formes et rend un chemin local quand il y en a un. */
            g_autoptr(GFile) f = g_file_new_for_commandline_arg(argv[i]);
            g_autofree gchar *chemin = g_file_get_path(f);
            g_autofree gchar *uri    = g_file_get_uri(f);
            g_free(a.fichier);
            a.fichier = g_strdup(chemin ? chemin : uri);
        } else {
            g_printerr("video : option inconnue « %s »\n", argv[i]);
            return 2;
        }
    }

    ShellConfig *cfg = shell_config_load();

    a.app = gtk_application_new("os.claude.shell.video", G_APPLICATION_NON_UNIQUE);
    g_object_set_data(G_OBJECT(a.app), "cfg", cfg);
    g_signal_connect(a.app, "startup",  G_CALLBACK(sur_demarrage),  cfg);
    g_signal_connect(a.app, "activate", G_CALLBACK(sur_activation), &a);
    int code = g_application_run(G_APPLICATION(a.app), 0, NULL);

    g_object_unref(a.app);
    shell_config_free(cfg);
    g_free(a.fichier);
    return code;
}
