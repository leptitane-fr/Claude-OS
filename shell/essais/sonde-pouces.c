/* =========================================================================
 * Claude OS — sonde des pouces : jusqu'où tombent-ils, tablette en main ?
 *
 * Le mode « deux mains » du clavier à l'écran place deux claviers sur les
 * bords gauche et droit, à la façon des manettes d'une console, et recadre
 * l'écran entre eux. Leur largeur ne se décide pas au jugé : elle se MESURE,
 * l'utilisateur tenant la tablette et tapant du pouce dans le vide.
 *
 * Ce programme couvre tout l'écran d'une surface qui ABSORBE les touchers —
 * rien n'est cliqué derrière —, dessine chaque point, les journalise, et
 * calcule à la fin la portée de chaque pouce.
 *
 * Construction et usage :
 *   bash shell/essais/construire.sh sonde-pouces
 *   shell/essais/build/sonde-pouces [journal.csv] [durée en s, 120 par défaut]
 *
 * Le journal : une ligne par événement, « ms,genre,suite,x,y », en pixels
 * de la surface — ceux de l'écran, écran non tourné.
 * ========================================================================= */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <math.h>
#include <stdio.h>

typedef struct { double x, y; gboolean fin; } Point;

static struct {
    GtkWidget *zone;
    GArray    *appuis;          /* Point : les débuts de contact           */
    GArray    *traces;          /* Point : tous les passages, pour dessiner */
    FILE      *journal;
    gint64     debut;
    int        largeur, hauteur;
    GtkWidget *compte;
} P;

static void
dessiner (GtkDrawingArea *a, cairo_t *cr, int l, int h, gpointer d)
{
    (void) a; (void) d;
    P.largeur = l;
    P.hauteur = h;

    cairo_set_source_rgba (cr, 0.05, 0.06, 0.08, 0.82);
    cairo_paint (cr);

    /* La médiane : ce qui est à gauche compte pour le pouce gauche. */
    cairo_set_source_rgba (cr, 1, 1, 1, 0.15);
    cairo_set_line_width (cr, 2);
    cairo_move_to (cr, l / 2.0, 0);
    cairo_line_to (cr, l / 2.0, h);
    cairo_stroke (cr);

    /* Les traces, larges : c'est en coloriant l'écran qu'on voit les trous.
     * Ce programme sert à deux mesures — la portée des pouces, et la
     * recherche de zones mortes — et la seconde demande de VOIR la
     * couverture pendant qu'on la produit. */
    for (guint i = 0; i < P.traces->len; i++) {
        Point *p = &g_array_index (P.traces, Point, i);
        gboolean gauche = p->x < l / 2.0;
        cairo_set_source_rgba (cr, gauche ? 0.26 : 0.98, gauche ? 0.52 : 0.74,
                               gauche ? 0.96 : 0.02, 0.5);
        cairo_arc (cr, p->x, p->y, 7, 0, 2 * G_PI);
        cairo_fill (cr);
    }
    for (guint i = 0; i < P.appuis->len; i++) {
        Point *p = &g_array_index (P.appuis, Point, i);
        gboolean gauche = p->x < l / 2.0;
        cairo_set_source_rgba (cr, gauche ? 0.26 : 0.98, gauche ? 0.52 : 0.74,
                               gauche ? 0.96 : 0.02, 0.9);
        cairo_arc (cr, p->x, p->y, 9, 0, 2 * G_PI);
        cairo_fill (cr);
    }
}

static void
noter (const char *genre, GdkEventSequence *s, double x, double y)
{
    gint64 ms = (g_get_monotonic_time () - P.debut) / 1000;
    /* EN ENTIERS, ET CE N'EST PAS UN DÉTAIL. « %.1f » écrit « 136,7 » : GTK
     * a passé le programme dans la locale française, et la virgule
     * décimale double le séparateur du CSV. C'est arrivé à la première
     * mesure, le 11 septembre 2026. Le pixel près suffit. */
    if (P.journal != NULL)
        fprintf (P.journal, "%" G_GINT64_FORMAT ",%s,%p,%ld,%ld\n",
                 ms, genre, (void *) s, lround (x), lround (y));
}

static gboolean
on_evenement (GtkEventControllerLegacy *c, GdkEvent *ev, gpointer d)
{
    (void) c; (void) d;
    GdkEventType t = gdk_event_get_event_type (ev);
    double x, y;
    if (!gdk_event_get_position (ev, &x, &y))
        return FALSE;

    GdkEventSequence *s = gdk_event_get_event_sequence (ev);
    Point p = { x, y, FALSE };
    switch (t) {
    case GDK_TOUCH_BEGIN:
        noter ("appui", s, x, y);
        g_array_append_val (P.appuis, p);
        g_array_append_val (P.traces, p);
        break;
    case GDK_TOUCH_UPDATE:
        noter ("glisse", s, x, y);
        g_array_append_val (P.traces, p);
        break;
    case GDK_TOUCH_END:
    case GDK_TOUCH_CANCEL:
        noter ("leve", s, x, y);
        break;
    case GDK_BUTTON_PRESS:
        noter ("souris", NULL, x, y);   /* noté, pas compté : ce n'est pas un pouce */
        break;
    default:
        return FALSE;
    }
    g_autofree char *m = g_strdup_printf ("%u appuis", P.appuis->len);
    gtk_label_set_text (GTK_LABEL (P.compte), m);
    gtk_widget_queue_draw (P.zone);
    return TRUE;
}

static int
comparer (gconstpointer a, gconstpointer b)
{
    double x = *(const double *) a, y = *(const double *) b;
    return (x > y) - (x < y);
}

/* Le centile q (0..1) d'un tableau de doubles, trié sur place. */
static double
centile (GArray *v, double q)
{
    if (v->len == 0)
        return NAN;
    g_array_sort (v, comparer);
    guint i = (guint) lround (q * (v->len - 1));
    return g_array_index (v, double, i);
}

static void
bilan_cote (const char *nom, gboolean gauche)
{
    g_autoptr(GArray) portee = g_array_new (FALSE, FALSE, sizeof (double));
    g_autoptr(GArray) ys = g_array_new (FALSE, FALSE, sizeof (double));
    for (guint i = 0; i < P.appuis->len; i++) {
        Point *p = &g_array_index (P.appuis, Point, i);
        if ((p->x < P.largeur / 2.0) != gauche)
            continue;
        /* La portée se compte DEPUIS LE BORD : c'est la largeur qu'un
         * clavier collé à ce bord devra couvrir. */
        double d = gauche ? p->x : P.largeur - p->x;
        g_array_append_val (portee, d);
        g_array_append_val (ys, p->y);
    }
    printf ("%s : %u appuis\n", nom, portee->len);
    if (portee->len == 0)
        return;
    printf ("  portée depuis le bord (px) : médiane %.0f, 90 %% %.0f, 95 %% %.0f, max %.0f\n",
            centile (portee, 0.5), centile (portee, 0.9), centile (portee, 0.95),
            centile (portee, 1.0));
    printf ("  hauteur (px, 0 en haut)    : min %.0f, 5 %% %.0f, médiane %.0f, 95 %% %.0f, max %.0f\n",
            centile (ys, 0.0), centile (ys, 0.05), centile (ys, 0.5),
            centile (ys, 0.95), centile (ys, 1.0));
}

/* La carte de couverture : l'écran en cases de 40 px, et ce qui n'a jamais
 * été touché. Une case creuse ENTOURÉE de cases touchées est une zone
 * morte ; une case creuse au bord de ce qu'on a balayé n'est rien. */
static void
bilan_couverture (void)
{
    const int PAS = 40;
    int cl = (P.largeur + PAS - 1) / PAS, ch = (P.hauteur + PAS - 1) / PAS;
    g_autofree int *n = g_new0 (int, cl * ch);
    for (guint i = 0; i < P.traces->len; i++) {
        Point *p = &g_array_index (P.traces, Point, i);
        int cx = CLAMP ((int) p->x / PAS, 0, cl - 1), cy = CLAMP ((int) p->y / PAS, 0, ch - 1);
        n[cy * cl + cx]++;
    }
    int touchees = 0, creuses = 0;
    printf ("\ncouverture, cases de %d px (# touchée, · jamais) :\n", PAS);
    for (int y = 0; y < ch; y++) {
        printf ("  ");
        for (int x = 0; x < cl; x++) {
            int v = n[y * cl + x];
            putchar (v > 4 ? '#' : v > 0 ? '+' : '.');
            if (v > 0)
                touchees++;
        }
        putchar ('\n');
    }
    /* Les creux entourés : au moins six des huit voisines touchées. */
    printf ("\nzones mortes probables (case vide, voisines touchées) :\n");
    for (int y = 1; y < ch - 1; y++)
        for (int x = 1; x < cl - 1; x++) {
            if (n[y * cl + x] != 0)
                continue;
            int voisins = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if ((dx || dy) && n[(y + dy) * cl + x + dx] > 0)
                        voisins++;
            if (voisins >= 6) {
                printf ("  x %4d..%4d  y %4d..%4d  (%d voisines touchées)\n",
                        x * PAS, (x + 1) * PAS, y * PAS, (y + 1) * PAS, voisins);
                creuses++;
            }
        }
    if (creuses == 0)
        printf ("  aucune — %d cases sur %d touchées\n", touchees, cl * ch);
}

static void
terminer (void)
{
    printf ("écran : %d x %d px\n", P.largeur, P.hauteur);
    bilan_cote ("pouce gauche", TRUE);
    bilan_cote ("pouce droit", FALSE);
    bilan_couverture ();
    fflush (stdout);
    if (P.journal != NULL)
        fclose (P.journal);
    P.journal = NULL;
}

static void
on_fini (GtkButton *b, gpointer app)
{
    (void) b;
    terminer ();
    g_application_quit (G_APPLICATION (app));
}

static gboolean
on_delai (gpointer app)
{
    printf ("(durée écoulée)\n");
    terminer ();
    g_application_quit (G_APPLICATION (app));
    return G_SOURCE_REMOVE;
}

static void
on_activate (GtkApplication *app, gpointer d)
{
    int duree = GPOINTER_TO_INT (d);
    GtkWidget *f = gtk_application_window_new (app);
    gtk_layer_init_for_window (GTK_WINDOW (f));
    gtk_layer_set_layer (GTK_WINDOW (f), GTK_LAYER_SHELL_LAYER_OVERLAY);
    for (int e = 0; e < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; e++)
        gtk_layer_set_anchor (GTK_WINDOW (f), e, TRUE);
    gtk_layer_set_exclusive_zone (GTK_WINDOW (f), -1);   /* tout l'écran, dock compris */
    gtk_layer_set_keyboard_mode (GTK_WINDOW (f), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_namespace (GTK_WINDOW (f), "claude-os-sonde-pouces");

    GtkWidget *sur = gtk_overlay_new ();
    P.zone = gtk_drawing_area_new ();
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (P.zone), dessiner, NULL, NULL);
    GtkEventController *leg = gtk_event_controller_legacy_new ();
    g_signal_connect (leg, "event", G_CALLBACK (on_evenement), NULL);
    gtk_widget_add_controller (P.zone, leg);
    gtk_overlay_set_child (GTK_OVERLAY (sur), P.zone);

    GtkWidget *haut = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign (haut, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (haut, GTK_ALIGN_START);
    gtk_widget_set_margin_top (haut, 40);
    GtkWidget *consigne = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (consigne),
        "<span size='x-large' foreground='white'><b>Tiens la tablette à deux mains.</b></span>\n"
        "<span size='large' foreground='#cccccc'>Balaie TOUT l'écran du doigt, comme pour le colorier :\n"
        "les trous se verront en direct. Insiste sur les bords et les coins.\n"
        "Rien n'est cliqué derrière. Touche « Terminer » quand c'est fait.</span>");
    gtk_label_set_justify (GTK_LABEL (consigne), GTK_JUSTIFY_CENTER);
    P.compte = gtk_label_new ("0 appui");
    GtkWidget *fini = gtk_button_new_with_label ("Terminer");
    gtk_widget_set_halign (fini, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request (fini, 220, 64);
    g_signal_connect (fini, "clicked", G_CALLBACK (on_fini), app);
    gtk_box_append (GTK_BOX (haut), consigne);
    gtk_box_append (GTK_BOX (haut), P.compte);
    gtk_box_append (GTK_BOX (haut), fini);
    gtk_overlay_add_overlay (GTK_OVERLAY (sur), haut);

    gtk_window_set_child (GTK_WINDOW (f), sur);
    gtk_window_present (GTK_WINDOW (f));
    P.debut = g_get_monotonic_time ();
    g_timeout_add_seconds (duree, on_delai, app);
}

int
main (int argc, char **argv)
{
    const char *chemin = argc > 1 ? argv[1] : "sonde-pouces.csv";
    int duree = argc > 2 ? atoi (argv[2]) : 120;
    P.appuis = g_array_new (FALSE, FALSE, sizeof (Point));
    P.traces = g_array_new (FALSE, FALSE, sizeof (Point));
    P.journal = fopen (chemin, "w");
    if (P.journal == NULL)
        perror (chemin);

    GtkApplication *app = gtk_application_new ("os.claude.essais.sonde-pouces",
                                               G_APPLICATION_NON_UNIQUE);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), GINT_TO_POINTER (duree));
    int r = g_application_run (G_APPLICATION (app), 0, NULL);
    g_object_unref (app);
    return r;
}
