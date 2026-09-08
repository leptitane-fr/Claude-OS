/* =========================================================================
 * Claude OS — les rangees de la Console : son, luminosite, alimentation.
 * ========================================================================= */

#define _DEFAULT_SOURCE

#include "console.h"
#include "sysfs.h"

#include <gio/gio.h>
#include <glib/gstdio.h>   /* g_access */
#include <stdlib.h>
#include <string.h>

/* Un curseur deplace produit des dizaines de « value-changed » par seconde.
 * Ecrire a chaque fois lancerait autant de processus wpctl, ou autant
 * d'ecritures sysfs. On attend que le doigt se stabilise. */
#define ECRITURE_DIFFEREE_MS 90

/* Une action d'alimentation demande confirmation, et l'armement retombe seul.
 * Quatre secondes : assez pour lire « Confirmer ? » et cliquer, trop court
 * pour qu'un clic distrait deux minutes plus tard eteigne la machine. */
#define CONFIRMATION_MS 4000

/* =========================================================================
 *  SON
 * ========================================================================= */

typedef struct {
    GtkWidget *boite;
    GtkWidget *icone;
    GtkWidget *echelle;
    GtkWidget *valeur;
    gboolean   muet;
    gboolean   disponible;
    gboolean   apercu;
    gboolean   ecriture_en_cours;  /* ignore les retours pendant qu'on ecrit */
    guint      differe;
} Son;

static void
son_icone (Son *s, int pourcent)
{
    const char *nom = s->muet || pourcent == 0 ? "audio-volume-muted-symbolic"
                    : pourcent < 34            ? "audio-volume-low-symbolic"
                    : pourcent < 67            ? "audio-volume-medium-symbolic"
                                               : "audio-volume-high-symbolic";
    gtk_image_set_from_icon_name (GTK_IMAGE (s->icone), nom);
}

static void
son_afficher (Son *s, int pourcent)
{
    son_icone (s, pourcent);
    if (s->muet) {
        gtk_label_set_text (GTK_LABEL (s->valeur), "Muet");
    } else {
        g_autofree char *t = g_strdup_printf ("%d %%", pourcent);
        gtk_label_set_text (GTK_LABEL (s->valeur), t);
    }
}

static void
son_indisponible (Son *s)
{
    s->disponible = FALSE;
    gtk_widget_set_sensitive (s->echelle, FALSE);
    gtk_widget_set_sensitive (s->icone, FALSE);
    gtk_label_set_text (GTK_LABEL (s->valeur), "Aucune sortie");
    gtk_image_set_from_icon_name (GTK_IMAGE (s->icone), "audio-volume-muted-symbolic");
    gtk_widget_set_tooltip_text (s->boite,
        "Aucune sortie audio. Sur cette machine la carte son ne s'initialise "
        "pas — voir docs/01 §1.8.");
}

/* « wpctl get-volume » repond « Volume: 0.42 » ou « Volume: 0.42 [MUTED] ». */
static void
on_volume_lu (GObject *src, GAsyncResult *res, gpointer data)
{
    Son *s = data;
    g_autoptr(GError) err = NULL;
    g_autofree char *sortie = NULL;

    if (!g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res,
                                               &sortie, NULL, &err)
        || sortie == NULL
        || !g_subprocess_get_successful (G_SUBPROCESS (src))) {
        son_indisponible (s);
        return;
    }

    const char *p = strstr (sortie, "Volume:");
    if (p == NULL) { son_indisponible (s); return; }

    double v = g_ascii_strtod (p + 7, NULL);
    s->muet = (strstr (sortie, "MUTED") != NULL);
    s->disponible = TRUE;
    gtk_widget_set_sensitive (s->echelle, TRUE);
    gtk_widget_set_sensitive (s->icone, TRUE);

    int pourcent = (int) (v * 100.0 + 0.5);
    s->ecriture_en_cours = TRUE;
    gtk_range_set_value (GTK_RANGE (s->echelle), pourcent);
    s->ecriture_en_cours = FALSE;
    son_afficher (s, pourcent);
}

/* Lance wpctl SANS bloquer. Un g_spawn_sync suffirait d'ordinaire — wpctl
 * repond en quelques millisecondes — mais si PipeWire est bloque, et sur
 * cette machine l'audio est en panne, l'interface entiere se figerait a
 * l'ouverture de la Console. */
static void
son_lancer (Son *s, GAsyncReadyCallback fini, char **argv)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GSubprocess) proc =
        g_subprocess_newv ((const char * const *) argv,
                           G_SUBPROCESS_FLAGS_STDOUT_PIPE
                         | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
    if (proc == NULL) {
        /* wpctl absent : wireplumber n'est pas installe. */
        son_indisponible (s);
        return;
    }
    g_subprocess_communicate_utf8_async (proc, NULL, NULL, fini, s);
}

void
console_son_relire (GtkWidget *rangee)
{
    Son *s = g_object_get_data (G_OBJECT (rangee), "son");
    if (s == NULL || s->apercu)
        return;
    char *argv[] = { (char *) "wpctl", (char *) "get-volume",
                     (char *) "@DEFAULT_AUDIO_SINK@", NULL };
    son_lancer (s, on_volume_lu, argv);
}

static gboolean
on_son_ecrire (gpointer data)
{
    Son *s = data;
    s->differe = 0;

    int pourcent = (int) gtk_range_get_value (GTK_RANGE (s->echelle));
    g_autofree char *arg = g_strdup_printf ("%d%%", pourcent);

    g_autoptr(GError) err = NULL;
    g_autoptr(GSubprocess) proc = g_subprocess_new (
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &err, "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", arg, NULL);
    if (proc == NULL)
        g_message ("volume : %s", err->message);

    return G_SOURCE_REMOVE;
}

static void
on_son_change (GtkRange *r, gpointer data)
{
    Son *s = data;
    if (s->ecriture_en_cours)
        return;

    int pourcent = (int) gtk_range_get_value (r);
    /* Bouger le curseur sort du silence : c'est ce qu'on attend d'un
     * curseur de volume, et cela evite d'avoir a le desactiver a la main. */
    if (s->muet && pourcent > 0)
        s->muet = FALSE;
    son_afficher (s, pourcent);

    if (s->apercu)
        return;
    if (s->differe != 0)
        g_source_remove (s->differe);
    s->differe = g_timeout_add (ECRITURE_DIFFEREE_MS, on_son_ecrire, s);
}

static void
on_muet_bascule (GtkButton *b, gpointer data)
{
    Son *s = data;
    (void) b;
    if (!s->disponible)
        return;

    s->muet = !s->muet;
    son_afficher (s, (int) gtk_range_get_value (GTK_RANGE (s->echelle)));

    if (s->apercu)
        return;
    g_autoptr(GError) err = NULL;
    g_autoptr(GSubprocess) proc = g_subprocess_new (
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &err, "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", NULL);
    if (proc == NULL)
        g_message ("sourdine : %s", err->message);
}

/* -------------------------------------------------------------------------
 * Une rangee de curseur : icone cliquable, curseur, valeur. Le meme squelette
 * sert au son et a la luminosite — deux mises en page differentes pour deux
 * reglages qui se ressemblent seraient une faute d'ergonomie avant d'etre une
 * duplication de code.
 * ------------------------------------------------------------------------- */
static GtkWidget *
rangee_curseur (const char *icone_nom, GtkWidget **icone_out,
                GtkWidget **echelle_out, GtkWidget **valeur_out,
                GtkWidget **bouton_out)
{
    GtkWidget *icone = gtk_image_new_from_icon_name (icone_nom);
    gtk_image_set_pixel_size (GTK_IMAGE (icone), 18);
    gtk_widget_add_css_class (icone, "qs-curseur-icone");

    GtkWidget *bouton = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (bouton), icone);
    gtk_widget_add_css_class (bouton, "qs-curseur-bouton");

    GtkWidget *echelle = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL,
                                                   0, 100, 1);
    gtk_scale_set_draw_value (GTK_SCALE (echelle), FALSE);
    gtk_widget_set_hexpand (echelle, TRUE);
    gtk_widget_add_css_class (echelle, "qs-curseur");

    GtkWidget *valeur = gtk_label_new ("-- %");
    gtk_widget_add_css_class (valeur, "qs-curseur-valeur");
    gtk_widget_set_halign (valeur, GTK_ALIGN_END);
    /* Largeur fixe : sans cela le curseur se retrecit d'un pixel a chaque
     * changement de chiffre, et l'ensemble tremble sous le doigt. */
    gtk_label_set_width_chars (GTK_LABEL (valeur), 6);

    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class (boite, "qs-rangee");
    gtk_box_append (GTK_BOX (boite), bouton);
    gtk_box_append (GTK_BOX (boite), echelle);
    gtk_box_append (GTK_BOX (boite), valeur);

    *icone_out = icone; *echelle_out = echelle;
    *valeur_out = valeur; *bouton_out = bouton;
    return boite;
}

GtkWidget *
console_son_new (gboolean apercu)
{
    Son *s = g_new0 (Son, 1);
    s->apercu = apercu;

    GtkWidget *bouton;
    s->boite = rangee_curseur ("audio-volume-high-symbolic", &s->icone,
                               &s->echelle, &s->valeur, &bouton);

    g_signal_connect (s->echelle, "value-changed", G_CALLBACK (on_son_change), s);
    g_signal_connect (bouton, "clicked", G_CALLBACK (on_muet_bascule), s);
    g_object_set_data_full (G_OBJECT (s->boite), "son", s, g_free);

    if (apercu) {
        s->disponible = TRUE;
        gtk_range_set_value (GTK_RANGE (s->echelle), 62);
        son_afficher (s, 62);
    }
    return s->boite;
}

/* =========================================================================
 *  LUMINOSITE
 * ========================================================================= */

typedef struct {
    GtkWidget *boite;
    GtkWidget *icone;
    GtkWidget *echelle;
    GtkWidget *valeur;
    char      *dir;            /* /sys/class/backlight/<qqch>              */
    char      *nom;            /* « intel_backlight » — le %k de la regle   */
    GDBusProxy *logind;        /* session logind, si elle repond            */
    gboolean   logind_utilisable;  /* rabattu au premier refus de logind    */
    int        maxi;
    gboolean   inscriptible;
    gboolean   apercu;
    gboolean   ecriture_en_cours;
    guint      differe;
} Lumiere;

/* Le premier ecran retro-eclaire declare par le noyau. Sur MADOO c'est
 * « intel_backlight » ; le nom n'est pas garanti, on prend ce qu'on trouve. */
static char *
retroeclairage_dir (void)
{
    static const char *base = "/sys/class/backlight";
    g_autoptr(GDir) d = g_dir_open (base, 0, NULL);
    if (d == NULL)
        return NULL;
    const char *nom;
    while ((nom = g_dir_read_name (d)) != NULL)
        return g_build_filename (base, nom, NULL);
    return NULL;
}

static void
lumiere_afficher (Lumiere *l, int pourcent)
{
    g_autofree char *t = g_strdup_printf ("%d %%", pourcent);
    gtk_label_set_text (GTK_LABEL (l->valeur), t);
    /* UN SEUL NOM, ET PAS TROIS.
     *
     * « display-brightness-low/medium/high-symbolic » n'existent que dans
     * Papirus ; Adwaita ne connait que « display-brightness-symbolic ». Trois
     * icones graduees donneraient donc un carre barre a la moindre bascule de
     * theme d'icones — verifie en listant les deux themes. Le pourcentage est
     * a cote, en chiffres : la gradation n'apprend rien de plus. */
    (void) pourcent;
}

void
console_lumiere_relire (GtkWidget *rangee)
{
    Lumiere *l = g_object_get_data (G_OBJECT (rangee), "lumiere");
    if (l == NULL || l->apercu || l->dir == NULL || l->maxi <= 0)
        return;

    g_autofree char *cur = shell_sysfs_read (l->dir, "brightness");
    if (cur == NULL)
        return;
    int pourcent = (int) ((atoi (cur) * 100.0) / l->maxi + 0.5);

    l->ecriture_en_cours = TRUE;
    gtk_range_set_value (GTK_RANGE (l->echelle), pourcent);
    l->ecriture_en_cours = FALSE;
    lumiere_afficher (l, pourcent);
}

/* Le curseur renonce, et dit pourquoi. Appele quand les DEUX voies ont
 * echoue : un curseur qui bouge sans effet est pire que pas de curseur. */
static void
lumiere_renoncer (Lumiere *l, const char *cause)
{
    if (!l->inscriptible)
        return;                      /* deja dit — ne pas le repeter */
    g_message ("luminosite : %s", cause);
    l->inscriptible = FALSE;
    gtk_widget_set_sensitive (l->echelle, FALSE);
    gtk_label_set_text (GTK_LABEL (l->valeur), "Verrou.");
    gtk_widget_set_tooltip_text (l->boite,
        "La luminosité n'a pas pu être écrite : ni logind ni "
        "/sys/class/backlight ne l'ont acceptée. Ajouter le compte au groupe "
        "« video » (sudo usermod -aG video <compte>) puis rouvrir la session.");
}

/* LA VOIE DE SECOURS : ecrire dans sysfs. Elle exige que le fichier soit
 * ouvert au groupe « video » ET que le compte y appartienne — ce qui, pour
 * l'appartenance, ne prend effet qu'a la session suivante. C'est precisement
 * la raison pour laquelle ce n'est plus la voie principale. */
static gboolean
lumiere_par_sysfs (Lumiere *l, int valeur)
{
    g_autofree char *chemin = g_build_filename (l->dir, "brightness", NULL);
    g_autofree char *texte  = g_strdup_printf ("%d\n", valeur);
    g_autoptr(GError) err = NULL;

    if (g_file_set_contents (chemin, texte, -1, &err))
        return TRUE;
    g_message ("luminosite : sysfs refuse — %s", err->message);
    return FALSE;
}

/* LA REPONSE PEUT ARRIVER APRES LA FERMETURE DU PANNEAU.
 *
 * L'appel est asynchrone et porte un delai de deux secondes. Si la Console se
 * referme entre-temps, la rangee est detruite, « lumiere_free » libere la
 * structure — et cette fonction travaillerait sur de la memoire rendue. Une
 * reference prise sur le widget avant l'appel, relachee ici, retarde sa
 * destruction jusqu'a la reponse. */
static void
on_logind_repond (GObject *src, GAsyncResult *res, gpointer data)
{
    Lumiere *l = data;
    GtkWidget *garde = l->boite;          /* la reference prise avant l'appel */
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) r = g_dbus_proxy_call_finish (G_DBUS_PROXY (src), res, &err);

    if (r != NULL) {
        g_object_unref (garde);
        return;                                   /* ecrit, rien a dire */
    }
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_object_unref (garde);
        return;
    }

    /* logind a refuse. Les motifs connus : session inactive sur le siege, ou
     * ecran qui n'appartient pas a ce siege. On ne suppose pas lequel — on
     * essaie l'autre voie, et l'on ne renonce que si elle echoue aussi. */
    g_message ("luminosite : logind refuse — %s", err->message);
    l->logind_utilisable = FALSE;

    int pourcent = (int) gtk_range_get_value (GTK_RANGE (l->echelle));
    int valeur = (int) (pourcent * (double) l->maxi / 100.0 + 0.5);
    if (valeur < 1)
        valeur = 1;
    if (!lumiere_par_sysfs (l, valeur))
        lumiere_renoncer (l, "logind et sysfs ont tous deux refuse");
    g_object_unref (garde);
}

static gboolean
on_lumiere_ecrire (gpointer data)
{
    Lumiere *l = data;
    l->differe = 0;

    int pourcent = (int) gtk_range_get_value (GTK_RANGE (l->echelle));
    int valeur = (int) (pourcent * (double) l->maxi / 100.0 + 0.5);
    if (valeur < 1)
        valeur = 1;   /* jamais zero : un ecran noir n'est pas un reglage */

    /* LOGIND D'ABORD, ET C'EST UN CHANGEMENT DE DOCTRINE.
     *
     * La regle udev ouvre le fichier « brightness » de chaque ecran au
     * groupe « video », et provision.sh y ajoute le compte. Mais une appartenance a
     * un groupe ne prend effet qu'a la session SUIVANTE : entre les deux, le
     * curseur se verrouille en renvoyant l'utilisateur a provision.sh, qu'il
     * vient justement de lancer. Constate sur la machine le 8 septembre.
     *
     * logind, lui, n'a besoin d'aucun groupe : il verifie que l'appelant est
     * la session active du siege et ecrit pour lui. Aucune relance, aucune
     * reouverture de session. C'est la voie de GNOME et de KDE.
     *
     * En ASYNCHRONE : un aller-retour sur le bus systeme pendant qu'un doigt
     * fait glisser un curseur figerait le panneau si le bus tardait. */
    if (l->logind != NULL && l->logind_utilisable) {
        g_object_ref (l->boite);      /* relachee dans on_logind_repond */
        g_dbus_proxy_call (l->logind, "SetBrightness",
                           g_variant_new ("(ssu)", "backlight", l->nom,
                                          (guint32) valeur),
                           G_DBUS_CALL_FLAGS_NONE, 2000, NULL,
                           on_logind_repond, l);
        return G_SOURCE_REMOVE;
    }

    if (!lumiere_par_sysfs (l, valeur))
        lumiere_renoncer (l, "sysfs refuse et logind est indisponible");
    return G_SOURCE_REMOVE;
}

static void
on_lumiere_change (GtkRange *r, gpointer data)
{
    Lumiere *l = data;
    if (l->ecriture_en_cours)
        return;

    lumiere_afficher (l, (int) gtk_range_get_value (r));

    if (l->apercu || !l->inscriptible)
        return;
    if (l->differe != 0)
        g_source_remove (l->differe);
    l->differe = g_timeout_add (ECRITURE_DIFFEREE_MS, on_lumiere_ecrire, l);
}

static void
lumiere_free (gpointer data)
{
    Lumiere *l = data;
    g_clear_object (&l->logind);
    g_free (l->nom);
    g_free (l->dir);
    g_free (l);
}

GtkWidget *
console_lumiere_new (gboolean apercu)
{
    Lumiere *l = g_new0 (Lumiere, 1);
    l->apercu = apercu;

    GtkWidget *bouton;
    l->boite = rangee_curseur ("display-brightness-symbolic", &l->icone,
                               &l->echelle, &l->valeur, &bouton);
    /* L'icone de luminosite n'est pas un bouton : il n'y a pas d'equivalent
     * de la sourdine. On la laisse inerte plutot que d'offrir un clic qui ne
     * fait rien. */
    gtk_widget_set_sensitive (bouton, FALSE);
    gtk_widget_add_css_class (bouton, "inerte");

    g_signal_connect (l->echelle, "value-changed",
                      G_CALLBACK (on_lumiere_change), l);
    g_object_set_data_full (G_OBJECT (l->boite), "lumiere", l, lumiere_free);

    if (apercu) {
        l->inscriptible = TRUE;
        gtk_range_set_value (GTK_RANGE (l->echelle), 45);
        lumiere_afficher (l, 45);
        return l->boite;
    }

    l->dir = retroeclairage_dir ();
    if (l->dir == NULL) {
        gtk_widget_set_sensitive (l->echelle, FALSE);
        gtk_label_set_text (GTK_LABEL (l->valeur), "N/A");
        gtk_widget_set_tooltip_text (l->boite,
            "Aucun écran rétro-éclairé déclaré dans /sys/class/backlight.");
        return l->boite;
    }

    g_autofree char *m = shell_sysfs_read (l->dir, "max_brightness");
    l->maxi = m ? atoi (m) : 0;
    if (l->maxi <= 0) {
        gtk_widget_set_sensitive (l->echelle, FALSE);
        gtk_label_set_text (GTK_LABEL (l->valeur), "N/A");
        return l->boite;
    }

    l->nom = g_path_get_basename (l->dir);

    /* LA SESSION LOGIND, SUR LE BUS SYSTEME.
     *
     * « /session/self » designe la session de l'appelant : rien a chercher,
     * rien a deviner. On ne se contente pas de fabriquer le mandataire —
     * GDBus le rend meme quand personne ne porte le nom — on demande QUI le
     * porte. Sans proprietaire, logind ne tourne pas, et le mandataire ne
     * servirait qu'a echouer plus tard. */
    g_autoptr(GError) err = NULL;
    l->logind = g_dbus_proxy_new_for_bus_sync (
        G_BUS_TYPE_SYSTEM,
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES
            | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
        NULL, "org.freedesktop.login1",
        "/org/freedesktop/login1/session/self",
        "org.freedesktop.login1.Session", NULL, &err);
    if (l->logind != NULL) {
        g_autofree char *proprio = g_dbus_proxy_get_name_owner (l->logind);
        if (proprio == NULL)
            g_clear_object (&l->logind);
    } else {
        g_message ("luminosite : logind injoignable — %s", err->message);
    }
    l->logind_utilisable = (l->logind != NULL);

    /* DEUX VOIES, DONC DEUX CHANCES.
     *
     * On n'appelle pas SetBrightness ici pour savoir s'il marchera : ce serait
     * changer la luminosite a l'ouverture du panneau. La rangee reste donc
     * active des qu'UNE des deux voies est plausible, et c'est la premiere
     * ecriture qui tranche — elle sait renoncer en le disant. */
    g_autofree char *chemin = g_build_filename (l->dir, "brightness", NULL);
    gboolean sysfs_ouvert = (g_access (chemin, W_OK) == 0);
    l->inscriptible = (l->logind != NULL) || sysfs_ouvert;

    if (!l->inscriptible) {
        gtk_widget_set_sensitive (l->echelle, FALSE);
        gtk_label_set_text (GTK_LABEL (l->valeur), "Verrou.");
        gtk_widget_set_tooltip_text (l->boite,
            "Ni logind ni /sys/class/backlight n'acceptent l'écriture. "
            "Ajouter le compte au groupe « video » "
            "(sudo usermod -aG video <compte>) puis rouvrir la session.");
    }
    console_lumiere_relire (l->boite);
    return l->boite;
}

/* =========================================================================
 *  ALIMENTATION
 * ========================================================================= */

typedef struct {
    GtkWidget  *bouton;
    GtkWidget  *etiquette;
    const char *libelle;        /* « Éteindre »                              */
    const char *methode;        /* « PowerOff »                              */
    GtkWidget  *popover;
    gboolean    arme;
    guint       desarmement;
    gboolean    apercu;
} Action;

static void
action_desarmer (Action *a)
{
    a->arme = FALSE;
    if (a->desarmement != 0) {
        g_source_remove (a->desarmement);
        a->desarmement = 0;
    }
    gtk_label_set_text (GTK_LABEL (a->etiquette), a->libelle);
    gtk_widget_remove_css_class (a->bouton, "arme");
}

static gboolean
on_desarmer (gpointer data)
{
    Action *a = data;
    a->desarmement = 0;
    action_desarmer (a);
    return G_SOURCE_REMOVE;
}

static void
on_action (GtkButton *b, gpointer data)
{
    Action *a = data;
    (void) b;

    /* PREMIER CLIC : ON ARME, ON N'AGIT PAS.
     *
     * Ces trois boutons sont a deux centimetres du reglage du volume, dans
     * un panneau qu'on ouvre plusieurs fois par jour. Un clic malheureux ne
     * doit pas fermer une session de travail. Le second clic, lui, agit
     * immediatement — pas de fenetre modale a chasser. */
    if (!a->arme) {
        a->arme = TRUE;
        gtk_label_set_text (GTK_LABEL (a->etiquette), "Confirmer ?");
        gtk_widget_add_css_class (a->bouton, "arme");
        a->desarmement = g_timeout_add (CONFIRMATION_MS, on_desarmer, a);
        return;
    }
    action_desarmer (a);

    if (a->apercu)
        return;

    /* La Console est une surface layer-shell posee par-dessus tout : la
     * laisser ouverte pendant l'extinction fige l'ecran sur le panneau. */
    if (a->popover != NULL)
        gtk_popover_popdown (GTK_POPOVER (a->popover));

    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (bus == NULL) {
        g_warning ("bus systeme injoignable : %s", err->message);
        return;
    }
    /* « false » : ne pas demander de politesse aux applications. logind
     * s'occupe d'inhiber si un programme l'a demande. */
    g_dbus_connection_call (bus,
                            "org.freedesktop.login1",
                            "/org/freedesktop/login1",
                            "org.freedesktop.login1.Manager",
                            a->methode,
                            g_variant_new ("(b)", FALSE),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static GtkWidget *
action_new (Action *a, const char *libelle, const char *icone_nom,
            const char *methode, GtkWidget *popover, gboolean apercu)
{
    a->libelle = libelle;
    a->methode = methode;
    a->popover = popover;
    a->apercu  = apercu;

    GtkWidget *icone = gtk_image_new_from_icon_name (icone_nom);
    gtk_image_set_pixel_size (GTK_IMAGE (icone), 16);

    a->etiquette = gtk_label_new (libelle);
    gtk_widget_add_css_class (a->etiquette, "qs-alim-nom");

    GtkWidget *contenu = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_set_halign (contenu, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (contenu), icone);
    gtk_box_append (GTK_BOX (contenu), a->etiquette);

    a->bouton = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (a->bouton), contenu);
    gtk_widget_add_css_class (a->bouton, "qs-alim");
    gtk_widget_set_hexpand (a->bouton, TRUE);
    g_signal_connect (a->bouton, "clicked", G_CALLBACK (on_action), a);
    return a->bouton;
}

GtkWidget *
console_alimentation_new (GtkWidget *popover, gboolean apercu)
{
    /* Les trois etats vivent aussi longtemps que la rangee ; ils sont
     * liberes avec elle par g_object_set_data_full. */
    Action *actions = g_new0 (Action, 3);

    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class (boite, "qs-alim-rangee");
    gtk_box_set_homogeneous (GTK_BOX (boite), TRUE);

    gtk_box_append (GTK_BOX (boite),
        action_new (&actions[0], "Éteindre", "system-shutdown-symbolic",
                    "PowerOff", popover, apercu));
    gtk_box_append (GTK_BOX (boite),
        action_new (&actions[1], "Redémarrer", "system-reboot-symbolic",
                    "Reboot", popover, apercu));
    gtk_box_append (GTK_BOX (boite),
        action_new (&actions[2], "Veille", "weather-clear-night-symbolic",
                    "Suspend", popover, apercu));

    g_object_set_data_full (G_OBJECT (boite), "actions", actions, g_free);
    return boite;
}
