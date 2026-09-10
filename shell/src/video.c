/* Claude OS -- « Vidéo », le lecteur. Phase 1 : le noyau de lecture, dans une
 * fenetre nue. L'interface demandee -- image sans bordure, capsule de
 * commandes, glissiere revelee au survol -- vient ensuite.
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
 */

#include "video-moteur.h"
#include "video-image.h"

#include <gtk/gtk.h>

typedef struct {
    GtkApplication *app;
    GtkWidget      *fenetre;
    GtkWidget      *image;
    GtkWidget      *offload;

    VideoMoteur    *moteur;
    VideoImage      images;
    guint           battement;      /* identifiant du tick callback        */

    gchar          *fichier;
    int             duree_essai;    /* --essai=N : quitter apres N secondes */
    gboolean        plein;          /* --plein-ecran                        */
    gboolean        scenario;       /* --scenario : parcours automatique    */
    int             etape;
    gint64          depart;
    gint64          battements;     /* combien de fois l'horloge a battu   */
} App;

/* ------------------------------------------------------------ affichage */

static gboolean sur_battement(GtkWidget *w, GdkFrameClock *horloge, gpointer u)
{
    App *a = u;
    a->battements++;

    if (a->duree_essai > 0 &&
        g_get_monotonic_time() - a->depart > (gint64)a->duree_essai * G_USEC_PER_SEC) {
        gtk_window_close(GTK_WINDOW(a->fenetre));
        return G_SOURCE_REMOVE;
    }

    AVFrame *trame = video_moteur_image_due(a->moteur);
    if (!trame) return G_SOURCE_CONTINUE;

    GdkTexture *t = video_image_texture(&a->images, trame,
                                        gtk_widget_get_display(a->image));
    /* La texture dmabuf garde sa propre reference sur les tampons : la trame
     * decodee peut etre rendue au decodeur des maintenant, ce qui lui evite
     * d'attendre une surface libre. */
    av_frame_free(&trame);

    if (t) {
        gtk_picture_set_paintable(GTK_PICTURE(a->image), GDK_PAINTABLE(t));
        g_object_unref(t);
    }
    return G_SOURCE_CONTINUE;
}

/* LE BATTEMENT NE SE POSE QUE SUR UN WIDGET DEJA AFFICHE.
 *
 * Piege paye le 10 septembre 2026, et il ne se voit pas a la lecture du
 * code : gtk_widget_add_tick_callback() sur un widget PAS ENCORE REALISE
 * enregistre bien le rappel -- gtk_widget_get_realized() dira ensuite oui,
 * l'identifiant sera valide -- mais l'horloge d'images n'est jamais mise en
 * marche. Le rappel est alors appele UNE FOIS, a la premiere image que GTK
 * dessine de toute facon, puis plus jamais. La fenetre reste sur cette
 * unique image, sans une erreur, et tout le reste du lecteur parait en
 * cause : le decodeur, l'audio, le compositeur.
 *
 * Le battement est donc pose au « map » et retire au « unmap ». Benefice de
 * cote, et il est exactement dans la ligne du projet : une fenetre reduite
 * cesse de battre, donc de decoder, sans une ligne de plus. */
static void battement_selon(App *a, gboolean actif)
{
    if (actif && !gtk_widget_get_mapped(a->image)) return;

    if (actif && !a->battement) {
        a->battement = gtk_widget_add_tick_callback(a->image, sur_battement,
                                                    a, NULL);
    } else if (!actif && a->battement) {
        gtk_widget_remove_tick_callback(a->image, a->battement);
        a->battement = 0;
    }
}

/* --------------------------------------------------------------- moteur */

static void sur_etat(VideoEtat etat, gpointer u)
{
    App *a = u;
    battement_selon(a, etat == VIDEO_LIT);
}

static void sur_apparition(GtkWidget *w, gpointer u)
{
    App *a = u;
    battement_selon(a, video_moteur_etat(a->moteur) == VIDEO_LIT);
}

static void sur_disparition(GtkWidget *w, gpointer u)
{
    battement_selon((App *)u, FALSE);
}

/* LE BILAN, ET POURQUOI IL EST IMPRIME PLUTOT QUE SUPPOSE.
 *
 * Quatre chiffres suffisent a dire si une lecture s'est bien passee, et
 * aucun ne se devine a l'oeil : le nombre d'images sautees (zero, en regime
 * etabli), la part passee par le chemin sans copie (tout, sauf codec non
 * accelere), et l'ecart de synchronisation moyen et maximal. */
static void bilan(App *a, const char *quand)
{
    if (!a->moteur) return;
    double sec = (g_get_monotonic_time() - a->depart) / 1e6;
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
}

static void sur_fin(gpointer u)
{
    App *a = u;
    bilan(a, "fin du fichier");
    if (a->duree_essai > 0) gtk_window_close(GTK_WINDOW(a->fenetre));
}

static void sur_erreur(const char *message, gpointer u)
{
    (void)u;
    g_warning("video : %s", message);
}

static void bilan(App *a, const char *quand);

/* ------------------------------------------------------------- scenario */

/* UN PARCOURS AUTOMATIQUE, PARCE QUE L'ECRAN N'EST PAS TOUJOURS DISPONIBLE.
 *
 * Pause, reprise, saut avant, saut arriere, retour au debut : ce sont les
 * gestes qui cassent un lecteur, et ce sont ceux qu'on ne peut pas eprouver
 * sans les faire. Ce parcours les fait tout seul et dit ce qu'il constate,
 * pour que le banc sans ecran (essais/banc-video.sh) vaille verification.
 *
 * Il n'est actif que sur --scenario : c'est un mode de banc, pas un mode du
 * lecteur, et il est le seul endroit du programme ou vit un minuteur. */
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
        case 0: video_moteur_pause(m);              break;
        case 1: video_moteur_lire(m);               break;
        case 2: video_moteur_sauter(m, 40.0);       break;
        case 3: video_moteur_avancer(m, -10.0);     break;
        case 4: video_moteur_sauter(m, 0.0);        break;
        default:
            bilan(a, "fin du scenario");
            gtk_window_close(GTK_WINDOW(a->fenetre));
            return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* -------------------------------------------------------------- clavier */

static gboolean sur_touche(GtkEventControllerKey *c, guint val, guint code,
                           GdkModifierType mod, gpointer u)
{
    App *a = u;
    switch (val) {
        case GDK_KEY_space:
        case GDK_KEY_k:          video_moteur_basculer(a->moteur);       return TRUE;
        case GDK_KEY_Left:       video_moteur_avancer(a->moteur, -10.0); return TRUE;
        case GDK_KEY_Right:      video_moteur_avancer(a->moteur, +10.0); return TRUE;
        case GDK_KEY_Down:       video_moteur_avancer(a->moteur,  -5.0); return TRUE;
        case GDK_KEY_Up:         video_moteur_avancer(a->moteur,  +5.0); return TRUE;
        case GDK_KEY_Home:       video_moteur_sauter(a->moteur, 0.0);    return TRUE;
        case GDK_KEY_f:
        case GDK_KEY_F11:
            if (gtk_window_is_fullscreen(GTK_WINDOW(a->fenetre)))
                gtk_window_unfullscreen(GTK_WINDOW(a->fenetre));
            else
                gtk_window_fullscreen(GTK_WINDOW(a->fenetre));
            return TRUE;
        case GDK_KEY_Escape:
            if (gtk_window_is_fullscreen(GTK_WINDOW(a->fenetre)))
                gtk_window_unfullscreen(GTK_WINDOW(a->fenetre));
            else
                gtk_window_close(GTK_WINDOW(a->fenetre));
            return TRUE;
        case GDK_KEY_q:          gtk_window_close(GTK_WINDOW(a->fenetre)); return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------- fenetre */

static void sur_fermeture(GtkWindow *w, gpointer u)
{
    App *a = u;
    bilan(a, "fermeture");
    battement_selon(a, FALSE);
    /* Le moteur ferme son fil AVANT que la fenetre ne disparaisse : un fil
     * de decodage qui pousse une image dans une file detruite ne se voit
     * qu'au coredump. */
    g_clear_pointer(&a->moteur, video_moteur_fermer);
    video_image_fin(&a->images);
}

static void construire(App *a)
{
    a->fenetre = gtk_application_window_new(a->app);
    gtk_window_set_title(GTK_WINDOW(a->fenetre), "Vidéo");
    gtk_window_set_default_size(GTK_WINDOW(a->fenetre), 1280, 720);

    a->image = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(a->image), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_hexpand(a->image, TRUE);
    gtk_widget_set_vexpand(a->image, TRUE);

    /* LE WIDGET QUI PORTE TOUT LE BENEFICE. Il ne dessine rien : il demande a
     * GDK de confier l'image au compositeur, dans une sous-surface, plutot
     * que de la composer dans la fenetre. Mesure du 10 septembre 2026 :
     * 0,2 W de moins en plein ecran, soit 9 % du SoC. Voir docs/11. */
    g_signal_connect(a->image, "map",   G_CALLBACK(sur_apparition), a);
    g_signal_connect(a->image, "unmap", G_CALLBACK(sur_disparition), a);

    a->offload = gtk_graphics_offload_new(a->image);
    gtk_graphics_offload_set_enabled(GTK_GRAPHICS_OFFLOAD(a->offload),
                                     GTK_GRAPHICS_OFFLOAD_ENABLED);

    gtk_window_set_child(GTK_WINDOW(a->fenetre), a->offload);

    GtkEventController *clavier = gtk_event_controller_key_new();
    g_signal_connect(clavier, "key-pressed", G_CALLBACK(sur_touche), a);
    gtk_widget_add_controller(a->fenetre, clavier);

    g_signal_connect(a->fenetre, "close-request", G_CALLBACK(sur_fermeture), a);
}

static void ouvrir_fichier(App *a, const char *chemin)
{
    GError *err = NULL;
    VideoRappels r = { sur_etat, sur_fin, sur_erreur };

    a->moteur = video_moteur_ouvrir(chemin, &r, a, &err);
    if (!a->moteur) {
        /* INVARIANT N.4 : on ne meurt pas en silence. */
        g_printerr("video : %s\n", err ? err->message : "ouverture impossible");
        g_clear_error(&err);
        gtk_window_close(GTK_WINDOW(a->fenetre));
        return;
    }

    g_message("video : %s -- %s %dx%d, %s, %.1f s",
              chemin, video_moteur_codec(a->moteur),
              video_moteur_largeur(a->moteur), video_moteur_hauteur(a->moteur),
              video_moteur_materiel(a->moteur) ? "décodage matériel"
                                               : "décodage logiciel",
              video_moteur_duree(a->moteur));

    a->depart = g_get_monotonic_time();
    video_moteur_lire(a->moteur);

    if (a->scenario) g_timeout_add_seconds(3, sur_scenario, a);
}

static void sur_activation(GApplication *app, gpointer u)
{
    App *a = u;
    if (!a->fichier) {
        g_printerr("Usage : claude-os-video <fichier> [--essai=<secondes>]\n");
        return;
    }
    video_image_init(&a->images);
    construire(a);
    if (a->plein) gtk_window_fullscreen(GTK_WINDOW(a->fenetre));
    gtk_window_present(GTK_WINDOW(a->fenetre));
    ouvrir_fichier(a, a->fichier);
}

int main(int argc, char **argv)
{
    App a = {0};

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--plein-ecran")) {
            a.plein = TRUE;
        } else if (!strcmp(argv[i], "--scenario")) {
            a.scenario = TRUE;
        } else if (g_str_has_prefix(argv[i], "--essai=")) {
            a.duree_essai = atoi(argv[i] + 8);
        } else if (argv[i][0] != '-') {
            g_free(a.fichier);
            a.fichier = g_strdup(argv[i]);
        } else {
            g_printerr("video : option inconnue « %s »\n", argv[i]);
            return 2;
        }
    }

    a.app = gtk_application_new("os.claude.shell.video", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(a.app, "activate", G_CALLBACK(sur_activation), &a);
    int code = g_application_run(G_APPLICATION(a.app), 0, NULL);

    g_object_unref(a.app);
    g_free(a.fichier);
    return code;
}
