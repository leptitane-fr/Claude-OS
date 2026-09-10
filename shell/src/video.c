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
 *   |            video              |  ni cadre, ni marge, ni coin arrondi.
 *   |                               |
 *   +-------------------------------+
 *   |        (espace vide)          |  <- au survol, la glissiere apparait ICI
 *   |     ( o====|--------- )       |
 *   +-------------------------------+
 *   |     ( capsule des commandes ) |
 *   +-------------------------------+
 *
 * L'espace vide n'est pas une marge : c'est une ZONE SENSIBLE. La glissiere
 * y apparait en fondu, PAR-DESSUS le vide, sans jamais deplacer la video ni
 * la capsule -- un decalage de mise en page a chaque survol serait
 * insupportable a l'usage. D'ou une GtkOverlay, et non une boite.
 *
 * AU DOIGT, LE SURVOL N'EXISTE PAS. Un toucher n'importe ou revele la
 * glissiere ; c'est seulement une fois les commandes visibles qu'un appui
 * sur l'image met en pause. Sans cette regle, toucher l'ecran pour voir ou
 * l'on en est arreterait la lecture -- exactement l'inverse de ce qu'on
 * voulait.
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
    GtkWidget      *reveleur;
    GtkWidget      *glissiere;
    GtkWidget      *capsule;

    GtkWidget      *b_lecture;
    GtkWidget      *b_plein;
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

    gint64          geste_us;       /* dernier geste sur la glissiere     */
    int             seconde_affichee;
    gboolean        survol;

    gchar          *fichier;
    int             duree_essai;
    gboolean        plein;
    gboolean        scenario;
    gboolean        revele;         /* --revele : banc, pour la capture     */
    gboolean        avec_st;        /* --sous-titres : banc, active la 1re  */
    int             etape;
    gint64          depart;
    gint64          battements;
} App;

static void bilan(App *a, const char *quand);
static void act_sourdine(GtkButton *b, gpointer u);

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
    gboolean change = FALSE;
    const char *texte = video_moteur_sous_titre(a->moteur, &change);
    if (!change) return;

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

    AVFrame *trame = video_moteur_image_due(a->moteur);
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

/* ------------------------------------------------- la glissiere revelee */

static gboolean retirer_glissiere(gpointer u)
{
    App *a = u;
    a->retrait = 0;
    if (!a->survol)
        gtk_revealer_set_reveal_child(GTK_REVEALER(a->reveleur), FALSE);
    return G_SOURCE_REMOVE;
}

/* « momentane » : au doigt, la glissiere se retire seule ; a la souris,
 * elle suit le survol et n'a pas besoin de minuteur. */
static void reveler(App *a, gboolean visible, gboolean momentane)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(a->reveleur), visible);

    if (a->retrait) { g_source_remove(a->retrait); a->retrait = 0; }
    if (visible && momentane)
        a->retrait = g_timeout_add_seconds(RETRAIT_S, retirer_glissiere, a);
}

static void sur_entree_zone(GtkEventControllerMotion *c, double x, double y, gpointer u)
{
    (void) c; (void) x; (void) y;
    App *a = u;
    a->survol = TRUE;
    reveler(a, TRUE, FALSE);
}

static void sur_sortie_zone(GtkEventControllerMotion *c, gpointer u)
{
    (void) c;
    App *a = u;
    a->survol = FALSE;
    reveler(a, FALSE, FALSE);
}

/* LE TOUCHER, PARTOUT. Le premier contact revele, et ne met pas en pause.
 * Un appui sur l'image ne bascule la lecture que si les commandes sont deja
 * visibles -- sinon, regarder ou l'on en est arreterait le film. */
static void sur_appui(GtkGestureClick *g, int n, double x, double y, gpointer u)
{
    (void) x; (void) y;
    App *a = u;
    gboolean deja = gtk_revealer_get_reveal_child(GTK_REVEALER(a->reveleur));

    GdkEvent *ev = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(g));
    GdkDevice *dev = ev ? gdk_event_get_device(ev) : NULL;
    gboolean tactile = dev && gdk_device_get_source(dev) == GDK_SOURCE_TOUCHSCREEN;

    reveler(a, TRUE, tactile);
    if (!deja) return;                    /* le premier contact ne fait que reveler */

    if (n == 2 || tactile)
        video_moteur_basculer(a->moteur);
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

static void act_reculer(GtkButton *b, gpointer u)
{ (void) b; video_moteur_avancer(((App *)u)->moteur, -10.0); }

static void act_avancer(GtkButton *b, gpointer u)
{ (void) b; video_moteur_avancer(((App *)u)->moteur, +10.0); }

static void basculer_plein(App *a)
{
    if (gtk_window_is_fullscreen(GTK_WINDOW(a->fenetre)))
        gtk_window_unfullscreen(GTK_WINDOW(a->fenetre));
    else
        gtk_window_fullscreen(GTK_WINDOW(a->fenetre));
}

static void act_plein(GtkButton *b, gpointer u) { (void) b; basculer_plein((App *)u); }

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
        case GDK_KEY_m:      act_sourdine(NULL, a);                  return TRUE;
        case GDK_KEY_f:
        case GDK_KEY_F11:    basculer_plein(a);                      return TRUE;
        case GDK_KEY_Escape:
            if (gtk_window_is_fullscreen(GTK_WINDOW(a->fenetre)))
                gtk_window_unfullscreen(GTK_WINDOW(a->fenetre));
            else
                gtk_window_close(GTK_WINDOW(a->fenetre));
            return TRUE;
        case GDK_KEY_q:      gtk_window_close(GTK_WINDOW(a->fenetre)); return TRUE;
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
    gtk_box_append(GTK_BOX(c),
                   bouton("media-seek-backward-symbolic", NULL,
                          G_CALLBACK(act_reculer), a));
    gtk_box_append(GTK_BOX(c), a->b_lecture);
    gtk_box_append(GTK_BOX(c),
                   bouton("media-seek-forward-symbolic", NULL,
                          G_CALLBACK(act_avancer), a));
    gtk_box_append(GTK_BOX(c), a->b_son);
    gtk_box_append(GTK_BOX(c), a->volume);
    gtk_box_append(GTK_BOX(c), a->l_codec);
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

static void construire(App *a)
{
    a->fenetre = gtk_application_window_new(a->app);
    gtk_window_set_title(GTK_WINDOW(a->fenetre), "Vidéo");
    gtk_window_set_default_size(GTK_WINDOW(a->fenetre), 1280, 760);
    gtk_widget_add_css_class(a->fenetre, "video-fenetre");

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
                                     GTK_GRAPHICS_OFFLOAD_ENABLED);

    /* Le sous-titre par-dessus l'image, dans une GtkOverlay. Cache tant
     * qu'il n'y a rien a dire -- voir rafraichir_sous_titre(). */
    a->st_texte = gtk_label_new("");
    gtk_widget_add_css_class(a->st_texte, "video-sous-titre");
    gtk_label_set_justify(GTK_LABEL(a->st_texte), GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(GTK_LABEL(a->st_texte), TRUE);
    gtk_widget_set_halign(a->st_texte, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(a->st_texte, GTK_ALIGN_END);
    gtk_widget_set_visible(a->st_texte, FALSE);
    gtk_widget_set_can_target(a->st_texte, FALSE);   /* il ne vole pas l'appui */

    GtkWidget *sur_image = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(sur_image), a->offload);
    gtk_overlay_add_overlay(GTK_OVERLAY(sur_image), a->st_texte);
    gtk_widget_set_vexpand(sur_image, TRUE);

    GtkWidget *pile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(pile, "video-pile");
    gtk_box_append(GTK_BOX(pile), sur_image);
    gtk_box_append(GTK_BOX(pile), construire_zone(a));
    a->capsule = construire_capsule(a);
    gtk_box_append(GTK_BOX(pile), a->capsule);

    gtk_window_set_child(GTK_WINDOW(a->fenetre), pile);

    GtkEventController *clavier = gtk_event_controller_key_new();
    g_signal_connect(clavier, "key-pressed", G_CALLBACK(sur_touche), a);
    gtk_widget_add_controller(a->fenetre, clavier);

    /* L'appui sur l'IMAGE, pas sur la fenetre entiere : un clic dans la
     * capsule doit actionner le bouton vise, pas mettre en pause. */
    GtkGesture *appui = gtk_gesture_click_new();
    g_signal_connect(appui, "pressed", G_CALLBACK(sur_appui), a);
    gtk_widget_add_controller(a->image, GTK_EVENT_CONTROLLER(appui));

    g_signal_connect(a->fenetre, "notify::fullscreened",
                     G_CALLBACK(sur_plein_change), a);
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
    if (a->revele)   reveler(a, TRUE, FALSE);
    if (a->scenario) g_timeout_add_seconds(3, sur_scenario, a);
}

static void sur_demarrage(GtkApplication *app, gpointer cfg)
{
    shell_styles_startup(app, cfg);
}

static void sur_activation(GApplication *app, gpointer u)
{
    App *a = u;
    if (!a->fichier) {
        g_printerr("Usage : claude-os-video <fichier> "
                   "[--plein-ecran] [--essai=<secondes>] [--scenario]\n");
        return;
    }

    ShellConfig *cfg = g_object_get_data(G_OBJECT(app), "cfg");
    if (cfg) {
        shell_config_apply(cfg);
        /* Les widgets GTK ordinaires -- infobulles, boites de dialogue -- ne
         * passent pas par notre feuille de style. Sans cela ils resteraient
         * clairs dans une fenetre sombre. */
        g_object_set(gtk_settings_get_default(),
                     "gtk-application-prefer-dark-theme", cfg->dark, NULL);
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
        } else if (!strcmp(argv[i], "--sous-titres")) {
            a.avec_st = TRUE;
        } else if (!strcmp(argv[i], "--revele")) {
            /* Mode de banc : sans pointeur, le survol ne peut pas etre
             * joue ; on montre la glissiere pour pouvoir la regarder. */
            a.revele = TRUE;
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
