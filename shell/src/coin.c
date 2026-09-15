#include "coin.h"

#include <gtk4-layer-shell.h>

#include "energie.h"

/* LE COIN EST DESORMAIS A RAS DU CADRE, ET SON RETRAIT EST UN PADDING.
 *
 * Il gardait 12 px de marge layer-shell -- la trame du bureau, celle que le
 * dock tient deja. Mais depuis qu'il porte un VOILE (voir « .coin » dans
 * shell.css), cette marge laissait une bande transparente entre le degrade
 * et le vrai coin de l'ecran : le voile s'arretait avant le bord, et la
 * coupure se voyait sur un fond clair.
 *
 * La fenetre s'ancre donc au cadre, et les 12 px reviennent en padding CSS.
 * Rien ne bouge a l'ecran ; le degrade, lui, va jusqu'au bout. */

/* Taille du glyphe de mode. 96 px : il passe DERRIERE l'heure, en retrait
 * d'opacite (voir « .coin-mode » dans shell.css), et un filigrane se
 * reconnait d'autant mieux qu'il est grand. 76 px avaient ete essayes
 * d'abord : lisible, mais le glyphe se tenait timidement au-dessus du texte
 * au lieu de le porter. */
#define MODE_PX 96

static struct {
    GtkWidget *fenetre;
    GtkWidget *rangee;      /* l'ancre des surfaces qui se posent dessus    */
    GtkWidget *temps;       /* la boite heure + date, a largeur fixe        */
    GtkWidget *heure, *date;
    GtkWidget *mode;
    GtkWidget *cloche, *reseau, *bt;
    GtkWidget *pourcent, *prise, *batterie;
    GtkCssProvider *style;
    int        opacite;
    GDBusProxy *bt_proxy;
    gboolean    efface;     /* une fenetre occupe l'ecran entier            */
} C;

/* -------------------------------------------------------------------------
 * L'aspect, engendre -- opacite de l'encre ET ombre portee
 *
 * Meme jeton @avis que les avis systeme, meme reglage d'opacite, meme
 * raison : ce qui flotte sur le fond d'ecran doit le faire d'une seule
 * voix.
 *
 * L'OMBRE EST ICI ET NON DANS shell.css parce qu'elle se REGLE. La feuille
 * de style en porte une version statique, qui documente la forme et sert de
 * repli ; celle-ci la recouvre avec les valeurs de shell.conf.
 *
 * UNE SEULE REGLE POUR TOUTES LES CLASSES DU COIN, et non une par classe :
 * deux regles finissent toujours par diverger au premier ajustement.
 *
 * « -gtk-icon-shadow » ET NON « text-shadow » pour les icones : GTK ne
 * traite pas une image comme du texte. Et surtout pas « icon-shadow », le
 * nom de GTK 3, que GTK 4 refuse -- « No property named icon-shadow » au
 * chargement, sans que rien d'autre ne s'arrete.
 * ------------------------------------------------------------------------- */
/* LES CINQ VALEURS SONT RECOPIEES, PAS POINTEES.
 *
 * status.c LIBERE la configuration a la fin de chaque relecture a chaud :
 * garder le pointeur ici en ferait un pointeur mort des le retour. Ce
 * projet a deja paye trois usages apres liberation dans le volet des
 * lecteurs reseau, aucun visible a la lecture, tous trouves au segfault.
 * Cinq entiers se recopient. */
static struct {
    int encre, opacite, flou, contour, decalage;
    gboolean pose;
} A;

static void
apparence_appliquer (void)
{
    if (!A.pose)
        return;

    /* g_ascii_formatd ET NON %.3f : en français, printf écrit « 0,850 », et
     * la virgule coupe alpha() en deux arguments — la règle est alors
     * rejetée en silence. Le piège est décrit en tête de avis.c. */
    char encre[G_ASCII_DTOSTR_BUF_SIZE], ombre[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd (encre, sizeof encre, "%.3f", CLAMP (A.encre, 5, 100) / 100.0);
    g_ascii_formatd (ombre, sizeof ombre, "%.3f", CLAMP (A.opacite, 0, 100) / 100.0);

    int flou     = CLAMP (A.flou,     0, 40);
    int contour  = CLAMP (A.contour,  0, 40);
    int decalage = CLAMP (A.decalage, 0, 20);

    g_autofree char *css = g_strdup_printf (
        ".coin { color: alpha(@avis, %s); }\n"
        ".coin-heure, .coin-date, .coin-pourcent {"
        " text-shadow: 0 %dpx %dpx alpha(@ombre-encre, %s),"
        "              0 0 %dpx alpha(@ombre-encre, %s); }\n"
        ".coin-mode, .coin-cloche, .coin-reseau, .coin-bt,"
        ".coin-prise, .coin-batterie {"
        " -gtk-icon-shadow: 0 %dpx %dpx alpha(@ombre-encre, %s),"
        "                   0 0 %dpx alpha(@ombre-encre, %s); }\n",
        encre,
        decalage, contour, ombre, flou, ombre,
        decalage, contour, ombre, flou, ombre);

    if (C.style == NULL) {
        C.style = gtk_css_provider_new ();
        gtk_style_context_add_provider_for_display (
            gdk_display_get_default (), GTK_STYLE_PROVIDER (C.style),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    gtk_css_provider_load_from_string (C.style, css);
}

void
shell_coin_apparence (const ShellConfig *cfg)
{
    A.encre    = cfg->energie_opacite;
    A.opacite  = cfg->ombre_opacite;
    A.flou     = cfg->ombre_flou;
    A.contour  = cfg->ombre_contour;
    A.decalage = cfg->ombre_decalage;
    A.pose     = TRUE;
    apparence_appliquer ();
}

/* -------------------------------------------------------------------------
 * Le Bluetooth, et pourquoi le coin le suit LUI-MEME
 *
 * Le reseau vient de status.c, qui tient deja un proxy NetworkManager
 * permanent ; la charge vient de batterie.c. Le Bluetooth, lui, n'avait
 * qu'un seul observateur : la tuile de la Console -- et panel.c ne cherche
 * l'adaptateur qu'AU PREMIER AFFICHAGE du panneau, a dessein, pour ne pas
 * reveiller BlueZ au demarrage pour un etat que personne ne regarde.
 *
 * Or maintenant quelqu'un le regarde en permanence. Un temoin permanent
 * demande une source permanente : le coin ouvre donc son propre proxy sur
 * « Powered » de l'adaptateur. C'est une propriete deja publiee, sans
 * scrutation, et le seul cout est la connexion au bus systeme -- que le
 * processus tient de toute facon pour logind.
 * ------------------------------------------------------------------------- */
static void
bt_appliquer (gboolean allume)
{
    gtk_image_set_from_icon_name (GTK_IMAGE (C.bt),
                                  allume ? "bluetooth-symbolic"
                                         : "bluetooth-disabled-symbolic");
    /* Eteint, le temoin s'efface au lieu de disparaitre : une icone qui
     * s'en va laisse la colonne sauter d'un cran, et l'oeil croit qu'autre
     * chose a change. */
    if (allume)
        gtk_widget_remove_css_class (C.bt, "eteint");
    else
        gtk_widget_add_css_class (C.bt, "eteint");
}

static void
bt_relire (GDBusProxy *proxy)
{
    g_autoptr(GVariant) v = g_dbus_proxy_get_cached_property (proxy, "Powered");
    bt_appliquer (v != NULL && g_variant_get_boolean (v));
}

static void
on_bt_props (GDBusProxy *proxy, GVariant *changed, GStrv invalidated,
             gpointer data)
{
    (void) changed; (void) invalidated; (void) data;
    bt_relire (proxy);
}

static void
on_bt_proxy (GObject *src, GAsyncResult *res, gpointer data)
{
    (void) src; (void) data;
    g_autoptr(GError) err = NULL;
    C.bt_proxy = g_dbus_proxy_new_for_bus_finish (res, &err);
    if (C.bt_proxy == NULL) {
        /* DIT, ET NON AVALE : sans BlueZ le temoin resterait eteint sans
         * qu'on sache si c'est l'adaptateur ou le shell qui manque. */
        g_message ("coin : adaptateur Bluetooth injoignable — %s", err->message);
        return;
    }
    g_signal_connect (C.bt_proxy, "g-properties-changed",
                      G_CALLBACK (on_bt_props), NULL);
    bt_relire (C.bt_proxy);
}

/* Le chemin de l'adaptateur, par l'ObjectManager de BlueZ. Asynchrone de
 * bout en bout : au demarrage de la session, un appel synchrone au bus
 * systeme retarderait l'apparition du coin. */
static void
on_bt_objets (GObject *src, GAsyncResult *res, gpointer data)
{
    (void) data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) rep =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
    if (rep == NULL) {
        g_message ("coin : BlueZ muet — %s", err->message);
        return;
    }

    g_autoptr(GVariant) objets = g_variant_get_child_value (rep, 0);
    GVariantIter it;
    g_variant_iter_init (&it, objets);

    const char *chemin;
    GVariant   *ifaces;
    while (g_variant_iter_loop (&it, "{&o@a{sa{sv}}}", &chemin, &ifaces)) {
        g_autoptr(GVariant) adaptateur =
            g_variant_lookup_value (ifaces, "org.bluez.Adapter1", NULL);
        if (adaptateur == NULL)
            continue;

        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
                                  NULL, "org.bluez", chemin,
                                  "org.bluez.Adapter1", NULL, on_bt_proxy, NULL);
        /* Sortie anticipee : g_variant_iter_loop ne liberera plus pour nous. */
        g_variant_unref (ifaces);
        return;
    }
    g_message ("coin : aucun adaptateur Bluetooth");
}

static void
bt_init (void)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (bus == NULL) {
        g_message ("coin : bus systeme injoignable — %s", err->message);
        return;
    }
    g_dbus_connection_call (bus, "org.bluez", "/",
                            "org.freedesktop.DBus.ObjectManager",
                            "GetManagedObjects", NULL,
                            G_VARIANT_TYPE ("(a{oa{sa{sv}}})"),
                            G_DBUS_CALL_FLAGS_NONE, 2000, NULL,
                            on_bt_objets, NULL);
}

/* -------------------------------------------------------------------------
 * RIEN NE BOUGE, ET C'EST MESURE
 *
 * Le coin est ancre en bas a DROITE : sa largeur suit son contenu, donc son
 * bord gauche recule ou avance a chaque changement de contenu. Trois choses
 * changeaient :
 *
 *   1. L'HEURE. En chiffres proportionnels, « 11:11 » est plus etroit que
 *      « 10:00 ». La boite heure/date prend le plus large de ses deux
 *      lignes ; selon la minute, c'est l'heure ou la date qui l'emporte, et
 *      le bloc entier sautait d'un pixel a l'autre A CHAQUE MINUTE.
 *      Corrige par « font-feature-settings: tnum » dans la feuille de
 *      style : les chiffres tabulaires ont tous la meme chasse.
 *
 *   2. LA DATE. « mardi 15 septembre » et « mercredi 1 octobre » n'ont pas
 *      la meme longueur. Une fois par jour, mais tout aussi visible.
 *
 *   3. LA FICHE SECTEUR, qui apparaissait et disparaissait de la colonne.
 *
 * Pour 1 et 2 : la boite recoit une largeur FIXE, celle de la plus large
 * date de l'annee, MESUREE et non ecrite en dur -- elle depend de la police,
 * qui se regle. Vingt-huit jours consecutifs couvrent les sept jours de la
 * semaine, douze mois couvrent les douze noms : 336 mesures d'une chaine
 * courte, une fois, au demarrage et a chaque changement de police.
 *
 * Pour 3 : voir shell_coin_batterie(), la fiche garde sa place.
 * ------------------------------------------------------------------------- */
static void
caler_largeur_temps (void)
{
    if (C.temps == NULL || C.date == NULL || C.heure == NULL)
        return;

    g_autoptr(PangoLayout) l = gtk_widget_create_pango_layout (C.date, NULL);
    int max = 0, w, h;

    for (int mois = 1; mois <= 12; mois++) {
        for (int jour = 1; jour <= 28; jour++) {
            g_autoptr(GDateTime) d =
                g_date_time_new_local (2027, mois, jour, 12, 0, 0);
            if (d == NULL)
                continue;
            g_autofree char *s = g_date_time_format (d, "%A %-d %B");
            pango_layout_set_text (l, s, -1);
            pango_layout_get_pixel_size (l, &w, &h);
            if (w > max) max = w;
        }
    }

    /* L'heure aussi : a grand corps elle peut depasser la date. Chiffres
     * tabulaires obligent, « 00:00 » vaut n'importe quelle heure. */
    g_autoptr(PangoLayout) lh = gtk_widget_create_pango_layout (C.heure, "00:00");
    pango_layout_get_pixel_size (lh, &w, &h);
    if (w > max) max = w;

    /* DIT, parce qu'une largeur qui part a zero -- police introuvable,
     * widget pas encore realise -- donnerait un coin qui se remet a bouger
     * sans qu'on sache pourquoi. */
    if (max <= 0) {
        g_message ("coin : largeur du bloc heure/date non mesurable");
        return;
    }
    gtk_widget_set_size_request (C.temps, max, -1);
}

/* ------------------------------------------------------------------------- */
static GtkWidget *
temoin (const char *icone, int px, const char *classe)
{
    GtkWidget *i = gtk_image_new_from_icon_name (icone);
    gtk_image_set_pixel_size (GTK_IMAGE (i), px);
    gtk_widget_add_css_class (i, classe);
    gtk_widget_set_halign (i, GTK_ALIGN_END);
    return i;
}

/* REGION D'ENTREE VIDE : la surface se voit et ne s'attrape pas. Sans cela
 * elle poserait un rectangle mort par-dessus le coin du bureau -- et comme
 * elle y reste EN PERMANENCE, ce rectangle avalerait tout clic visant ce
 * qu'il y a dessous, pour toujours. C'est la contrepartie exacte de la
 * permanence, et elle n'est pas negociable. */
static void
verrouiller_entrees (GtkWidget *w)
{
    GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (w));
    if (surface == NULL)
        return;
    cairo_region_t *vide = cairo_region_create ();
    gdk_surface_set_input_region (surface, vide);
    cairo_region_destroy (vide);
}

/* La taille du coin change avec l'heure -- « 9:05 » est plus court que
 * « 12:46 » -- et avec le pourcentage. La region doit donc etre reposee a
 * chaque nouvelle disposition.
 *
 * PAR LE SIGNAL DE LA SURFACE, et non par « size-allocate » : GTK 4 ne
 * publie plus ce signal sur les widgets. C'est GdkSurface::layout qui dit
 * qu'une taille vient d'etre arretee, et c'est le seul endroit ou l'on soit
 * sur que la surface existe deja. */
static void
on_disposition (GdkSurface *surface, int largeur, int hauteur, gpointer data)
{
    (void) surface; (void) largeur; (void) hauteur;
    verrouiller_entrees (GTK_WIDGET (data));
}

static void
on_realise (GtkWidget *w, gpointer data)
{
    (void) data;
    verrouiller_entrees (w);
    GdkSurface *s = gtk_native_get_surface (GTK_NATIVE (w));
    if (s != NULL)
        g_signal_connect (s, "layout", G_CALLBACK (on_disposition), w);
}

void
shell_coin_init (GtkApplication *app, const ShellConfig *cfg, gboolean apercu)
{
    (void) apercu;
    if (C.fenetre != NULL)
        return;

    GtkWidget *fenetre = gtk_application_window_new (app);
    C.fenetre = fenetre;
    gtk_widget_add_css_class (fenetre, "shell");
    gtk_widget_add_css_class (fenetre, "coin");

    gtk_layer_init_for_window (GTK_WINDOW (fenetre));
    /* OVERLAY : en TOP, labwc eteint la couche sous une fenetre plein
     * ecran, et le coin disparaitrait pendant une video -- c'est-a-dire
     * precisement quand on veut jeter un oeil a l'heure. Voir « A l'ecran
     * ou non » dans dock.c. */
    gtk_layer_set_layer (GTK_WINDOW (fenetre), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace (GTK_WINDOW (fenetre), "claude-os-coin");
    gtk_layer_set_keyboard_mode (GTK_WINDOW (fenetre),
                                 GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_anchor (GTK_WINDOW (fenetre), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (fenetre), GTK_LAYER_SHELL_EDGE_RIGHT,  TRUE);
    /* -1 : le coin IGNORE les zones reservees par les autres surfaces, et
     * n'en reserve aucune. Il ne repousse rien -- seul le dock le fait --
     * et il se pose au vrai bord de l'ecran, sur la ligne de base du dock. */
    gtk_layer_set_exclusive_zone (GTK_WINDOW (fenetre), -1);

    /* --- l'heure, la date --- */
    C.heure = gtk_label_new ("--:--");
    gtk_widget_add_css_class (C.heure, "coin-heure");
    gtk_widget_set_halign (C.heure, GTK_ALIGN_END);
    C.date = gtk_label_new ("");
    gtk_widget_add_css_class (C.date, "coin-date");
    gtk_widget_set_halign (C.date, GTK_ALIGN_END);

    GtkWidget *temps = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    C.temps = temps;
    gtk_widget_add_css_class (temps, "coin-temps");
    gtk_widget_set_valign (temps, GTK_ALIGN_END);
    gtk_box_append (GTK_BOX (temps), C.heure);
    gtk_box_append (GTK_BOX (temps), C.date);

    /* --- le mode d'energie, DERRIERE la droite de l'heure ---
     *
     * UN RECOUVREMENT, ET PAS UNE COLONNE DE PLUS. Range a cote, le glyphe
     * aurait pousse l'heure vers la gauche et fait du coin un bandeau ; par
     * dessus, il occupe le vide qui se trouve au-dessus de la date et ne
     * coute pas un pixel de largeur.
     *
     * LE GLYPHE EST L'ENFANT PRINCIPAL, L'HEURE EST SUPERPOSEE -- et non
     * l'inverse, comme c'etait le cas jusqu'au 15 septembre 2026.
     *
     * GtkOverlay dessine son enfant principal EN PREMIER, les superposes
     * par-dessus. Le glyphe etant superpose, il passait donc devant
     * l'heure. Tant qu'il n'etait qu'une forme claire en retrait, cela ne
     * se voyait pas ; depuis qu'il porte une ombre portee, cette ombre
     * tombait sur les chiffres et les salissait. Un filigrane se met
     * DERRIERE -- c'est la definition d'un filigrane.
     *
     * « measure_overlay » sur l'heure : sans lui, la superposition
     * prendrait la taille du glyphe, et le bloc heure/date serait ecrase
     * dans 96 px. C'est l'heure qui doit dicter la taille, comme avant --
     * seul l'ordre de dessin change. */
    C.mode = gtk_image_new_from_icon_name ("claude-os-mode-automatique-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (C.mode), MODE_PX);
    gtk_widget_add_css_class (C.mode, "coin-mode");
    gtk_widget_set_halign (C.mode, GTK_ALIGN_END);
    gtk_widget_set_valign (C.mode, GTK_ALIGN_START);

    GtkWidget *superpose = gtk_overlay_new ();
    gtk_overlay_set_child (GTK_OVERLAY (superpose), C.mode);
    gtk_overlay_add_overlay (GTK_OVERLAY (superpose), temps);
    gtk_overlay_set_measure_overlay (GTK_OVERLAY (superpose), temps, TRUE);

    /* --- la colonne des temoins --- */
    C.cloche = temoin ("claude-os-cloche-symbolic", 20, "coin-cloche");
    C.reseau = temoin ("network-wireless-offline-symbolic", 20, "coin-reseau");
    C.bt     = temoin ("bluetooth-disabled-symbolic", 20, "coin-bt");
    gtk_widget_add_css_class (C.bt, "eteint");

    C.pourcent = gtk_label_new ("--%");
    gtk_widget_add_css_class (C.pourcent, "coin-pourcent");
    gtk_widget_set_halign (C.pourcent, GTK_ALIGN_END);
    /* QUATRE CARACTERES, TOUJOURS : « 100% » est le plus large, et sans
     * cette reserve la colonne entiere se decalait en passant de 100 a 99.
     * Avec les chiffres tabulaires de la feuille de style, quatre chasses
     * valent quatre chasses quelle que soit la valeur. */
    gtk_label_set_width_chars (GTK_LABEL (C.pourcent), 4);

    C.prise    = temoin ("ac-adapter-symbolic", 18, "coin-prise");
    C.batterie = temoin ("battery-level-100-symbolic", 22, "coin-batterie");
    GtkWidget *charge = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_set_halign (charge, GTK_ALIGN_END);
    gtk_box_append (GTK_BOX (charge), C.prise);
    gtk_box_append (GTK_BOX (charge), C.batterie);
    /* OPACITE ZERO ET NON « MASQUEE ». Un widget masque ne recoit plus
     * d'allocation : la batterie glissait de 21 px vers la droite chaque
     * fois qu'on debranchait, et revenait en se branchant. La place de la
     * fiche est RESERVEE en permanence ; elle se remplit ou reste vide. */
    gtk_widget_set_opacity (C.prise, 0.0);

    GtkWidget *etat = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class (etat, "coin-etat");
    gtk_widget_set_valign (etat, GTK_ALIGN_END);
    gtk_box_append (GTK_BOX (etat), C.cloche);
    gtk_box_append (GTK_BOX (etat), C.reseau);
    gtk_box_append (GTK_BOX (etat), C.bt);
    gtk_box_append (GTK_BOX (etat), C.pourcent);
    gtk_box_append (GTK_BOX (etat), charge);

    GtkWidget *rangee = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (rangee, "coin-rangee");
    gtk_widget_set_valign (rangee, GTK_ALIGN_END);
    gtk_box_append (GTK_BOX (rangee), superpose);
    gtk_box_append (GTK_BOX (rangee), etat);

    C.rangee = rangee;
    gtk_window_set_child (GTK_WINDOW (fenetre), rangee);
    g_signal_connect (fenetre, "realize", G_CALLBACK (on_realise), NULL);

    shell_coin_mode (cfg);
    shell_coin_apparence (cfg);
    gtk_window_present (GTK_WINDOW (fenetre));
    caler_largeur_temps ();
    verrouiller_entrees (fenetre);

    bt_init ();
}

void
shell_coin_horloge (const char *heure, const char *date)
{
    if (C.heure == NULL)
        return;
    gtk_label_set_text (GTK_LABEL (C.heure), heure);
    gtk_label_set_text (GTK_LABEL (C.date), date);
}

void
shell_coin_mode (const ShellConfig *cfg)
{
    if (C.mode == NULL)
        return;
    const ShellModeEnergie *m = shell_energie_mode_actif (cfg);
    gtk_image_set_from_icon_name (GTK_IMAGE (C.mode), m->icone);
    gtk_widget_set_tooltip_text (C.mode, m->nom);
}

void
shell_coin_batterie (int pourcent, gboolean sur_secteur, const char *icone)
{
    if (C.batterie == NULL)
        return;
    g_autofree char *t = g_strdup_printf ("%d%%", pourcent);
    gtk_label_set_text (GTK_LABEL (C.pourcent), t);
    gtk_image_set_from_icon_name (GTK_IMAGE (C.batterie), icone);
    gtk_widget_set_opacity (C.prise, sur_secteur ? 1.0 : 0.0);
    verrouiller_entrees (C.fenetre);
}

void
shell_coin_reseau (const char *icone)
{
    if (C.reseau == NULL)
        return;
    gtk_image_set_from_icon_name (GTK_IMAGE (C.reseau), icone);
    /* Hors ligne, le temoin s'efface au lieu de disparaitre : meme
     * raisonnement que pour le Bluetooth. */
    if (g_strcmp0 (icone, "network-wireless-offline-symbolic") == 0)
        gtk_widget_add_css_class (C.reseau, "eteint");
    else
        gtk_widget_remove_css_class (C.reseau, "eteint");
}

void
shell_coin_non_lu (gboolean il_y_en_a)
{
    if (C.cloche == NULL)
        return;
    if (il_y_en_a)
        gtk_widget_add_css_class (C.cloche, "nouvelles");
    else
        gtk_widget_remove_css_class (C.cloche, "nouvelles");
}

GtkWidget *
shell_coin_ancre (void)
{
    return C.rangee;
}

void
shell_coin_plein_ecran (gboolean actif)
{
    if (C.fenetre == NULL || C.efface == actif)
        return;
    C.efface = actif;

    /* LA FENETRE EST MASQUEE, PAS VIDEE NI DEPLACEE. Une surface layer-shell
     * masquee ne coute plus rien au compositeur -- ni composition, ni
     * melange de sa transparence -- ce qui est precisement ce qu'on veut
     * pendant une lecture video, le poste le plus lourd de la machine.
     *
     * Et au retour, elle reparait telle quelle : l'heure a continue d'etre
     * ecrite pendant ce temps, la minuterie de status.c n'ayant aucune
     * raison de s'arreter pour si peu. */
    gtk_widget_set_visible (C.fenetre, !actif);
    if (!actif)
        verrouiller_entrees (C.fenetre);
}
