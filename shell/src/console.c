/* =========================================================================
 * Claude OS — les rangees de la Console : son, luminosite, alimentation.
 * ========================================================================= */

#define _DEFAULT_SOURCE

#include "console.h"
#include "sysfs.h"
#include "config.h"   /* ShellConfig : la rangee de veille lit et ecrit shell.conf */
#include "energie.h"  /* la table des modes, definie une seule fois */

#include <gio/gio.h>
#include <glib/gstdio.h>   /* g_access */
#include <fcntl.h>
#include <signal.h>        /* kill : la deconnexion */
#include <sys/statvfs.h>   /* la place qui reste sur le disque */
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Un curseur deplace produit des dizaines de « value-changed » par seconde.
 * Ecrire a chaque fois lancerait autant de processus wpctl, ou autant
 * d'ecritures sysfs. On attend que le doigt se stabilise. */
#define ECRITURE_DIFFEREE_MS 90

/* GEL APRES UNE ACTION DE L'UTILISATEUR.
 *
 * La Console se relit desormais en boucle tant qu'elle est ouverte, pour
 * suivre les touches du clavier. Cette relecture et le doigt sur le curseur
 * visent le meme widget, et sans precaution ils se disputent :
 *
 *   le doigt pose le curseur a 40 %   ->  l'ecriture est differee de 90 ms
 *   la relecture tombe dans l'intervalle, lit encore 55 %, et REPOSE 55 %
 *   le curseur saute en arriere sous le doigt.
 *
 * Toute action de l'utilisateur gele donc la relecture le temps que
 * l'ecriture parte et que le service la prenne en compte. Large exprès :
 * pendant un glissement continu, chaque mouvement repousse le gel, et on ne
 * relit qu'une fois le doigt leve. */
#define GEL_APRES_ACTION_US (700 * 1000)

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
    gint64     gel;                /* relecture suspendue jusqu'a cette date */
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
    if (g_get_monotonic_time () < s->gel)
        return;                    /* le doigt est sur le curseur */
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

    s->gel = g_get_monotonic_time () + GEL_APRES_ACTION_US;

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

    s->gel = g_get_monotonic_time () + GEL_APRES_ACTION_US;
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
    gint64     gel;                /* meme role que pour le son */
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
    if (g_get_monotonic_time () < l->gel)
        return;                    /* le doigt est sur le curseur */

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
/* g_file_set_contents() NE PEUT PAS ecrire dans sysfs : elle passe par un
 * fichier temporaire cree a cote de la cible, puis un renommage. Sysfs ne
 * laisse creer aucun fichier, et l'erreur parle d'un « brightness.79UMV3 »
 * introuvable -- ce qui envoie chercher du cote des droits, a tort.
 *
 * Ce repli n'avait donc JAMAIS fonctionne. Personne ne l'avait vu parce que
 * logind, la voie principale, reussit toujours en session normale ; le
 * defaut ne se decouvre que la ou logind manque. Trouve le 9 septembre 2026
 * depuis le module d'energie, qui portait le meme code. */
static gboolean
lumiere_par_sysfs (Lumiere *l, int valeur)
{
    g_autofree char *chemin = g_build_filename (l->dir, "brightness", NULL);
    g_autofree char *texte  = g_strdup_printf ("%d\n", valeur);

    int fd = open (chemin, O_WRONLY);
    if (fd < 0) {
        g_message ("luminosite : ouverture de %s refusee — %s",
                   chemin, g_strerror (errno));
        return FALSE;
    }
    gboolean ok = (write (fd, texte, strlen (texte)) >= 0);
    if (!ok)
        g_message ("luminosite : ecriture refusee — %s", g_strerror (errno));
    close (fd);
    return ok;
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

    l->gel = g_get_monotonic_time () + GEL_APRES_ACTION_US;

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
 *  SYSTEME — ce que la machine a sous le capot
 *
 * Trois mesures, et seulement trois : la memoire disponible, la place qui
 * reste sur le disque, la charge du processeur. Ce sont les chiffres qu'on
 * vient chercher quand la machine rame ou qu'une copie refuse de finir ;
 * le reste — le detail par coeur, les processus, le debit reseau — est le
 * travail d'un moniteur, pas d'une Console. Elle dit l'etat, elle ne
 * diagnostique pas.
 *
 * TOUT SE LIT DANS /proc ET /sys, RIEN NE S'AJOUTE. Ni libgtop ni demon :
 * quatre fichiers texte et un appel systeme suffisent, et le shell ne lie
 * pas une bibliotheque entiere pour lire des nombres — c'est la regle deja
 * posee par sysfs.h pour la batterie.
 *
 * DISPONIBLE N'EST PAS LIBRE, ET C'EST LE PIEGE DE CETTE CARTE.
 * « MemFree » ne compte que la memoire que personne n'a touchee : sur
 * MADOO il annonce 0,5 Gio quand 1,3 sont reellement disponibles, le cache
 * et les tampons etant rendus des qu'on les reclame. C'est « MemAvailable »
 * que le noyau calcule pour cela. Afficher MemFree ferait passer pour
 * exsangue une machine qui respire.
 *
 * LA CHARGE DU PROCESSEUR EST UNE DIFFERENCE, PAS UNE LECTURE. /proc/stat
 * ne donne que des compteurs cumules depuis le demarrage ; leur valeur
 * instantanee ne veut rien dire. Il faut deux lectures et l'ecart entre
 * elles — d'ou le tiret affiche les deux premieres secondes qui suivent
 * l'ouverture, le temps que la seconde arrive. Combler ce trou par la
 * moyenne depuis le demarrage serait plus joli, et faux.
 *
 * LA TEMPERATURE SE CHERCHE PAR SON NOM. Les zones thermiques ne sont pas
 * numerotees dans un ordre garanti, et sur MADOO la zone 0 est
 * « INT3400 », qui rapporte 20 °C fixes — une consigne de pilote, pas la
 * temperature d'une puce. On cherche donc « x86_pkg_temp », puis « TCPU »
 * a defaut, et on se tait si aucune des deux n'existe.
 *
 * DISCIPLINE D'ENERGIE, LA MEME QUE LE RESTE DE LA CONSOLE : rien n'est lu
 * tant que la Console est fermee. La relecture suit la minuterie de
 * panel.c — celle qui sert deja aux watts de la batterie, et qui s'arrete
 * a la fermeture.
 * ========================================================================= */

/* Seuils au-dela desquels la jauge passe au rouge. Ils ne portent que sur
 * la memoire et le disque : un processeur a 100 % fait son travail, un
 * disque plein empeche de travailler. */
#define MEMOIRE_TENDUE_PCT 15   /* moins de 15 % de disponible             */
#define DISQUE_TENDU_PCT   10   /* moins de 10 % de libre                  */

/* Au-dela de cet ecart, la lecture precedente de /proc/stat ne sert plus
 * de reference : la Console a ete refermee entre-temps, et la difference
 * porterait sur toute la duree pendant laquelle personne ne regardait. */
#define PROCESSEUR_ECART_MAX_US (5 * G_USEC_PER_SEC)

typedef struct {
    GtkWidget *valeur;
    GtkWidget *jauge;
} Mesure;

typedef struct {
    GtkWidget *boite;
    Mesure     memoire;
    Mesure     disque;
    Mesure     processeur;
    gboolean   apercu;

    char      *zone_temp;      /* fichier temp de la zone retenue, ou NULL  */
    gboolean   zone_cherchee;  /* la recherche n'a lieu qu'une fois         */

    guint64    cpu_total;      /* derniere lecture de /proc/stat            */
    guint64    cpu_repos;
    gint64     cpu_date;       /* quand — 0 si aucune reference             */
} Systeme;

/* « 3,7 Gio », « 40 Gio », « 976 Mio ». Le shell tourne en francais et le
 * %.1f de la libc y ecrit une virgule : c'est justement ce qu'on veut
 * afficher, comme les watts de la carte batterie. */
static char *
taille_lisible (guint64 octets)
{
    double gio = octets / (1024.0 * 1024.0 * 1024.0);
    if (gio >= 10.0)
        return g_strdup_printf ("%.0f Gio", gio);
    if (gio >= 1.0)
        return g_strdup_printf ("%.1f Gio", gio);
    return g_strdup_printf ("%.0f Mio", octets / (1024.0 * 1024.0));
}

/* Une valeur de /proc/meminfo, en kio. La cle est donnee avec ses
 * deux-points, et doit commencer une ligne : sans cette verification, une
 * recherche de « MemFree: » repondrait aussi bien depuis le milieu d'un
 * autre nom. Zero si la ligne manque. */
static guint64
meminfo_kio (const char *texte, const char *cle)
{
    for (const char *p = strstr (texte, cle); p != NULL; p = strstr (p + 1, cle)) {
        if (p != texte && p[-1] != '\n')
            continue;
        return g_ascii_strtoull (p + strlen (cle), NULL, 10);
    }
    return 0;
}

/* Premiere ligne de /proc/stat : « cpu » suivi des temps cumules, en tics.
 * On somme tout, et on retient a part les deux champs de repos — idle et
 * iowait. La machine qui attend son disque n'est pas occupee. */
static gboolean
processeur_compteurs (guint64 *total, guint64 *repos)
{
    g_autofree char *texte = NULL;
    if (!g_file_get_contents ("/proc/stat", &texte, NULL, NULL))
        return FALSE;
    if (!g_str_has_prefix (texte, "cpu "))
        return FALSE;

    guint64 somme = 0, calme = 0;
    char *fin = texte + 4;
    for (int i = 0; i < 10; i++) {
        char *debut = fin;
        guint64 v = g_ascii_strtoull (debut, &fin, 10);
        if (fin == debut)
            break;                      /* fin de ligne */
        somme += v;
        if (i == 3 || i == 4)           /* idle, iowait */
            calme += v;
    }
    if (somme == 0)
        return FALSE;

    *total = somme;
    *repos = calme;
    return TRUE;
}

/* Le fichier « temp » de la zone thermique qui parle du processeur, ou
 * NULL. Voir la tete de section : la zone se cherche par son type, jamais
 * par son numero. */
static char *
zone_temperature (void)
{
    static const char *types[] = { "x86_pkg_temp", "TCPU", NULL };

    for (int t = 0; types[t] != NULL; t++) {
        for (int i = 0; i < 32; i++) {
            g_autofree char *dir = g_strdup_printf ("/sys/class/thermal/thermal_zone%d", i);
            g_autofree char *type = shell_sysfs_read (dir, "type");
            if (type == NULL)
                continue;
            if (g_strcmp0 (type, types[t]) == 0)
                return g_strdup_printf ("%s/temp", dir);
        }
    }
    return NULL;
}

/* Une mesure : son nom a gauche, sa valeur a droite, sa jauge dessous.
 * Les trois se ressemblent parce qu'elles se comparent — l'oeil descend
 * une colonne de jauges et voit tout de suite laquelle est pleine. */
static GtkWidget *
mesure_construire (Mesure *m, const char *nom, int largeur_valeur)
{
    GtkWidget *etiquette = gtk_label_new (nom);
    gtk_widget_add_css_class (etiquette, "qs-sys-nom");
    gtk_widget_set_halign (etiquette, GTK_ALIGN_START);
    gtk_widget_set_hexpand (etiquette, TRUE);

    m->valeur = gtk_label_new ("—");
    gtk_widget_add_css_class (m->valeur, "qs-sys-valeur");
    gtk_widget_set_halign (m->valeur, GTK_ALIGN_END);
    /* Largeur fixe, comme les curseurs au-dessus et comme l'heure du coin :
     * « 9 Gio libres » est plus etroit que « 40 Gio libres », et la carte
     * changerait de largeur au fil des chiffres. */
    gtk_label_set_width_chars (GTK_LABEL (m->valeur), largeur_valeur);
    gtk_label_set_xalign (GTK_LABEL (m->valeur), 1.0);

    GtkWidget *tete = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append (GTK_BOX (tete), etiquette);
    gtk_box_append (GTK_BOX (tete), m->valeur);

    m->jauge = gtk_progress_bar_new ();
    gtk_widget_add_css_class (m->jauge, "qs-sys-jauge");

    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append (GTK_BOX (boite), tete);
    gtk_box_append (GTK_BOX (boite), m->jauge);
    return boite;
}

/* La jauge montre TOUJOURS ce qui est pris, le texte TOUJOURS ce qui
 * reste. L'inverse a ete essaye sur le papier : une jauge qui se vide en
 * se remplissant de rouge ne se lit pas. */
static void
mesure_poser (Mesure *m, double part_prise, const char *texte, gboolean tendu)
{
    gtk_label_set_text (GTK_LABEL (m->valeur), texte);
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (m->jauge),
                                   CLAMP (part_prise, 0.0, 1.0));
    if (tendu)
        gtk_widget_add_css_class (m->jauge, "tendu");
    else
        gtk_widget_remove_css_class (m->jauge, "tendu");
}

static void
systeme_memoire (Systeme *s)
{
    g_autofree char *texte = NULL;
    if (!g_file_get_contents ("/proc/meminfo", &texte, NULL, NULL))
        return;

    guint64 total = meminfo_kio (texte, "MemTotal:") * 1024;
    guint64 dispo = meminfo_kio (texte, "MemAvailable:") * 1024;
    if (total == 0)
        return;

    g_autofree char *libre = taille_lisible (dispo);
    g_autofree char *dit   = g_strdup_printf ("%s libres", libre);
    double part = 1.0 - (double) dispo / (double) total;
    mesure_poser (&s->memoire, part,
                  dit, dispo * 100 < total * MEMOIRE_TENDUE_PCT);

    /* L'echange dans l'infobulle, pas sur la carte. Il ne se regarde qu'une
     * fois la question posee — « pourquoi est-elle lente ? » — et une
     * quatrieme jauge pour une reponse aussi rare encombrerait les trois
     * autres. */
    guint64 ech_total = meminfo_kio (texte, "SwapTotal:") * 1024;
    guint64 ech_libre = meminfo_kio (texte, "SwapFree:") * 1024;
    g_autofree char *t_total = taille_lisible (total);
    g_autofree char *bulle = NULL;
    if (ech_total > 0) {
        g_autofree char *e_pris = taille_lisible (ech_total - ech_libre);
        g_autofree char *e_tout = taille_lisible (ech_total);
        bulle = g_strdup_printf ("%s de mémoire · échange : %s utilisés sur %s",
                                 t_total, e_pris, e_tout);
    } else {
        bulle = g_strdup_printf ("%s de mémoire · aucun échange", t_total);
    }
    gtk_widget_set_tooltip_text (s->memoire.jauge, bulle);
}

static void
systeme_disque (Systeme *s)
{
    struct statvfs st;
    if (statvfs ("/", &st) != 0)
        return;

    /* f_bavail et non f_bfree : les blocs reserves a root ne sont pas
     * disponibles pour le compte qui regarde. La jauge peut donc afficher
     * quelques points de plus que « df », qui sort les reserves du calcul
     * des deux cotes ; c'est la place reellement utilisable qui compte
     * ici, pas la comptabilite du systeme de fichiers. */
    guint64 libre = (guint64) st.f_bavail * st.f_frsize;
    guint64 total = (guint64) st.f_blocks * st.f_frsize;
    if (total == 0)
        return;

    g_autofree char *reste = taille_lisible (libre);
    g_autofree char *dit   = g_strdup_printf ("%s libres", reste);
    g_autofree char *tout  = taille_lisible (total);
    g_autofree char *bulle = g_strdup_printf ("Racine du système · %s au total", tout);

    mesure_poser (&s->disque, 1.0 - (double) libre / (double) total,
                  dit, libre * 100 < total * DISQUE_TENDU_PCT);
    gtk_widget_set_tooltip_text (s->disque.jauge, bulle);
}

static void
systeme_processeur (Systeme *s)
{
    guint64 total = 0, repos = 0;
    if (!processeur_compteurs (&total, &repos))
        return;

    gint64 maintenant = g_get_monotonic_time ();
    gboolean utilisable = (s->cpu_date != 0
                           && maintenant - s->cpu_date < PROCESSEUR_ECART_MAX_US
                           && total > s->cpu_total);

    if (!s->zone_cherchee) {
        s->zone_cherchee = TRUE;
        s->zone_temp = zone_temperature ();
    }

    if (utilisable) {
        guint64 ecart = total - s->cpu_total;
        guint64 calme = repos - s->cpu_repos;
        int pourcent = (int) ((ecart - MIN (calme, ecart)) * 100 / ecart);

        g_autofree char *degres = NULL;
        if (s->zone_temp != NULL) {
            g_autofree char *milli = NULL;
            if (g_file_get_contents (s->zone_temp, &milli, NULL, NULL))
                degres = g_strdup_printf (" · %d °C",
                                          (int) (g_ascii_strtoll (milli, NULL, 10) / 1000));
        }
        g_autofree char *dit = g_strdup_printf ("%d %%%s", pourcent,
                                                degres ? degres : "");
        mesure_poser (&s->processeur, pourcent / 100.0, dit, FALSE);
    }

    s->cpu_total = total;
    s->cpu_repos = repos;
    s->cpu_date  = maintenant;
}

void
console_systeme_relire (GtkWidget *rangee)
{
    Systeme *s = g_object_get_data (G_OBJECT (rangee), "systeme");
    if (s == NULL || s->apercu)
        return;

    systeme_memoire (s);
    systeme_disque (s);
    systeme_processeur (s);
}

static void
systeme_free (gpointer data)
{
    Systeme *s = data;
    g_free (s->zone_temp);
    g_free (s);
}

GtkWidget *
console_systeme_new (gboolean apercu)
{
    Systeme *s = g_new0 (Systeme, 1);
    s->apercu = apercu;

    s->boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class (s->boite, "qs-card");
    gtk_widget_add_css_class (s->boite, "qs-sys");

    /* Les largeurs de valeur sont comptees sur le pire cas de chacune :
     * « 976 Mio libres » pour les deux tailles, « 100 % · 100 °C » pour le
     * processeur. */
    gtk_box_append (GTK_BOX (s->boite),
                    mesure_construire (&s->memoire, "Mémoire", 15));
    gtk_box_append (GTK_BOX (s->boite),
                    mesure_construire (&s->disque, "Disque", 15));
    gtk_box_append (GTK_BOX (s->boite),
                    mesure_construire (&s->processeur, "Processeur", 15));

    g_object_set_data_full (G_OBJECT (s->boite), "systeme", s, systeme_free);

    if (apercu) {
        mesure_poser (&s->memoire, 0.64, "1,3 Gio libres", FALSE);
        mesure_poser (&s->disque, 0.26, "40 Gio libres", FALSE);
        mesure_poser (&s->processeur, 0.12, "12 % · 42 °C", FALSE);
    } else {
        /* Premiere lecture ici : la carte arrive remplie, sauf le
         * processeur qui attend sa seconde lecture. */
        systeme_memoire (s);
        systeme_disque (s);
        systeme_processeur (s);
    }
    return s->boite;
}

/* =========================================================================
 *  ALIMENTATION ET SESSION
 *
 * Cinq boutons, icones seules depuis le 11 septembre 2026 : les libelles
 * « Eteindre », « Redemarrer », « Veille » ont ete retires a la demande de
 * l'utilisateur, les icones suffisant. La place gagnee accueille les deux
 * gestes qui manquaient : fermer la session, et verrouiller l'ecran sans la
 * fermer. Le nom de chaque bouton reste dans son infobulle -- c'est aussi
 * lui que lit le lecteur d'ecran.
 * ========================================================================= */

typedef enum {
    ACTION_LOGIND,          /* PowerOff, Reboot, Suspend sur le Manager      */
    ACTION_DECONNEXION,     /* ferme la session : labwc s'arrete             */
    ACTION_VERROU,          /* l'ecran de verrouillage, session intacte      */
} ActionType;

typedef struct {
    GtkWidget  *bouton;
    GtkWidget  *icone;
    const char *libelle;        /* « Éteindre » : infobulle et lecteur d'ecran */
    const char *icone_nom;
    const char *methode;        /* « PowerOff » — pour ACTION_LOGIND           */
    ActionType  type;
    gboolean    confirmer;      /* deux temps, ou tout de suite ?              */
    ConsoleFermer fermer;
    gpointer      fermer_data;
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
    gtk_image_set_from_icon_name (GTK_IMAGE (a->icone), a->icone_nom);
    gtk_widget_set_tooltip_text (a->bouton, a->libelle);
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

/* Les appels a logind lisent leur reponse. Ils partaient avec un rappel
 * NULL, et un refus -- de polkit, d'un inhibiteur -- se perdait sans un
 * mot : on cliquait « Eteindre » deux fois, et il ne se passait rien. */
static void
on_logind_action (GObject *src, GAsyncResult *res, gpointer data)
{
    const char *quoi = data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) r =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
    if (r == NULL)
        g_message ("alimentation : « %s » refuse par logind — %s", quoi, err->message);
}

/* FERMER LA SESSION, C'EST ARRETER LE COMPOSITEUR.
 *
 * C'est ce que fait « labwc --exit » : un SIGTERM au processus que designe
 * LABWC_PID, variable que labwc pose dans l'environnement de tout ce qu'il
 * lance -- la barre en fait partie, par l'autostart. labwc s'arrete
 * proprement, claude-os-session consigne son code de retour, greetd rend
 * l'ecran de connexion.
 *
 * On verifie que ce numero designe bien labwc avant de tirer : une variable
 * heritee d'une session precedente designerait n'importe qui.
 *
 * Sans LABWC_PID -- barre lancee a la main --, repli sur logind, qui termine
 * la session de l'appelant. */
static void
fermer_la_session (GDBusConnection *bus)
{
    const char *texte = g_getenv ("LABWC_PID");
    int pid = texte ? atoi (texte) : 0;

    if (pid > 1) {
        g_autofree char *chemin = g_strdup_printf ("/proc/%d/comm", pid);
        g_autofree char *nom = NULL;
        if (g_file_get_contents (chemin, &nom, NULL, NULL)
            && g_str_has_prefix (g_strstrip (nom), "labwc")) {
            if (kill (pid, SIGTERM) == 0)
                return;
            g_message ("deconnexion : SIGTERM a labwc (%d) refuse — %s",
                       pid, g_strerror (errno));
        } else {
            g_message ("deconnexion : LABWC_PID=%s ne designe pas labwc", texte);
        }
    }

    g_message ("deconnexion : repli sur logind (Session.Terminate)");
    g_dbus_connection_call (bus, "org.freedesktop.login1",
                            "/org/freedesktop/login1/session/self",
                            "org.freedesktop.login1.Session", "Terminate",
                            NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                            on_logind_action, (gpointer) "Terminate");
}

static void
on_action (GtkButton *b, gpointer data)
{
    Action *a = data;
    (void) b;

    /* PREMIER CLIC : ON ARME, ON N'AGIT PAS.
     *
     * Ces boutons sont a deux centimetres du reglage du volume, dans un
     * panneau qu'on ouvre plusieurs fois par jour. Un clic malheureux ne doit
     * pas fermer une session de travail. Arme, le bouton passe au rouge et
     * son icone devient une coche : « encore une fois pour confirmer », sans
     * texte -- il n'y a plus de libelle a remplacer par « Confirmer ? ». Le
     * second clic agit immediatement, pas de fenetre modale a chasser.
     *
     * Verrouiller ne demande rien : c'est reversible par definition. */
    if (a->confirmer && !a->arme) {
        a->arme = TRUE;
        gtk_image_set_from_icon_name (GTK_IMAGE (a->icone), "object-select-symbolic");
        g_autofree char *bulle = g_strdup_printf ("%s : toucher à nouveau pour confirmer",
                                                  a->libelle);
        gtk_widget_set_tooltip_text (a->bouton, bulle);
        gtk_widget_add_css_class (a->bouton, "arme");
        a->desarmement = g_timeout_add (CONFIRMATION_MS, on_desarmer, a);
        return;
    }
    action_desarmer (a);

    if (a->apercu)
        return;

    /* La Console est une surface layer-shell posee par-dessus tout : la
     * laisser ouverte pendant l'extinction fige l'ecran sur le panneau, et
     * elle passerait devant l'ecran de verrouillage le temps qu'il monte. */
    if (a->fermer != NULL)
        a->fermer (a->fermer_data);

    if (a->type == ACTION_VERROU) {
        shell_energie_verrouiller ();
        return;
    }

    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (bus == NULL) {
        g_warning ("bus systeme injoignable : %s", err->message);
        return;
    }

    if (a->type == ACTION_DECONNEXION) {
        fermer_la_session (bus);
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
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                            on_logind_action, (gpointer) a->methode);
}

static GtkWidget *
action_new (Action *a, const char *libelle, const char *icone_nom,
            ActionType type, const char *methode, gboolean confirmer,
            ConsoleFermer fermer, gpointer fermer_data, gboolean apercu)
{
    a->libelle   = libelle;
    a->icone_nom = icone_nom;
    a->type      = type;
    a->methode   = methode;
    a->confirmer   = confirmer;
    a->fermer      = fermer;
    a->fermer_data = fermer_data;
    a->apercu      = apercu;

    a->icone = gtk_image_new_from_icon_name (icone_nom);
    gtk_image_set_pixel_size (GTK_IMAGE (a->icone), 18);

    a->bouton = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (a->bouton), a->icone);
    gtk_widget_add_css_class (a->bouton, "qs-alim");
    gtk_widget_add_css_class (a->bouton, "qs-alim-icone");
    gtk_widget_set_hexpand (a->bouton, TRUE);
    gtk_widget_set_tooltip_text (a->bouton, libelle);
    /* Le nom accessible, puisqu'il n'y a plus de texte visible. */
    gtk_accessible_update_property (GTK_ACCESSIBLE (a->bouton),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, libelle, -1);
    g_signal_connect (a->bouton, "clicked", G_CALLBACK (on_action), a);
    return a->bouton;
}

/* -------------------------------------------------------------------------
 * Veille de l'ecran : Travail / Automatique / Nomade
 *
 * LES MODES NE SONT PAS DEFINIS ICI. Ils viennent de shell_energie_modes(),
 * et les durees de shell_energie_delais(). Cette rangee ne fait que montrer
 * et ecrire : si elle recalculait les delais de son cote, elle finirait par
 * annoncer autre chose que ce que le module applique -- c'est exactement la
 * sorte d'ecart qu'on ne voit qu'apres l'avoir subi.
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkWidget *btn[8];         /* un par mode, table terminee par NULL      */
    int        n;
    gboolean   apercu;
    gboolean   pose_en_cours;
} Energie;

/* « 45 s », « 10 min », « jamais ». Les minutes rondes s'ecrivent en
 * minutes : « 600 s » est exact et illisible. */
static char *
duree_texte (int s)
{
    if (s <= 0)        return g_strdup ("jamais");
    if (s < 60)        return g_strdup_printf ("%d s", s);
    if (s % 60 == 0)   return g_strdup_printf ("%d min", s / 60);
    return g_strdup_printf ("%d min %d s", s / 60, s % 60);
}

/* CE TEXTE VA DANS UNE INFOBULLE, PAS DANS LA CONSOLE.
 *
 * Il y etait, sous les boutons, et il deformait le panneau : une etiquette
 * qui passe a la ligne fait varier la largeur de la Console selon le mode
 * choisi, et une surface qui change de taille sous le doigt est desagreable
 * a viser. Constate a l'usage le 9 septembre 2026.
 *
 * L'infobulle dit la meme chose, ne prend aucune place, et ne se montre
 * qu'a qui la cherche en s'attardant sur un bouton. */
static char *
energie_description (const ShellConfig *cfg, const ShellModeEnergie *mode)
{
    int pre, att, ete, sus;
    shell_energie_delais_mode (cfg, mode, &pre, &att, &ete, &sus);

    g_autoptr(GString) t = g_string_new (mode->resume);
    g_string_append (t, "\n\n");

    g_autofree char *a = duree_texte (att);
    g_autofree char *e = duree_texte (ete);
    g_string_append_printf (t, "Atténue après %s, éteint après %s", a, e);

    if (sus > 0) {
        g_autofree char *v = duree_texte (sus);
        /* On dit « verrouillée » plutot que de taire l'etage : un mode qui
         * annonce une veille qui n'arrive jamais est un mode qui ment. */
        g_string_append_printf (t, ", veille de l'ordinateur après %s%s", v,
                                cfg->energie_suspendre_permis
                                ? "" : " (verrouillée)");
    }
    if (pre > 0)
        g_string_append_printf (t, ". Compte à rebours %d s avant.", pre);
    else
        g_string_append_c (t, '.');

    if (!cfg->energie_active)
        g_string_append (t, "\n\nVeille désactivée dans les Réglages.");

    return g_string_free (g_steal_pointer (&t), FALSE);
}

static void
energie_poser (GtkToggleButton *b, gpointer data)
{
    Energie *en = data;

    /* Le groupe emet aussi pour le bouton qu'on vient de relacher, et
     * console_energie_relire() coche un bouton par programme. Sans ce
     * garde-fou, chaque changement ecrirait shell.conf deux fois. */
    if (en->pose_en_cours || !gtk_toggle_button_get_active (b))
        return;

    const ShellModeEnergie *modes = shell_energie_modes ();
    int i = 0;
    for (; i < en->n; i++)
        if (en->btn[i] == GTK_WIDGET (b))
            break;
    if (i == en->n)
        return;

    g_autoptr(ShellConfig) cfg = shell_config_load ();
    g_free (cfg->energie_mode);
    cfg->energie_mode = g_strdup (modes[i].id);

    if (en->apercu)
        return;

    g_autoptr(GError) err = NULL;
    if (!shell_config_save (cfg, &err))
        g_message ("energie : shell.conf non ecrit — %s", err->message);
}

void
console_energie_relire (GtkWidget *rangee)
{
    Energie *en = g_object_get_data (G_OBJECT (rangee), "energie");
    if (en == NULL)
        return;

    g_autoptr(ShellConfig) cfg = shell_config_load ();
    const ShellModeEnergie *actif = shell_energie_mode_actif (cfg);
    const ShellModeEnergie *modes = shell_energie_modes ();

    en->pose_en_cours = TRUE;
    for (int i = 0; i < en->n; i++) {
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (en->btn[i]),
                                      &modes[i] == actif);
        /* Les infobulles sont refaites a chaque relecture : les durees ont
         * pu changer dans les Reglages pendant que la Console etait
         * fermee, et une infobulle perimee ment plus surement qu'une
         * etiquette perimee -- on la consulte justement pour savoir. */
        g_autofree char *info = energie_description (cfg, &modes[i]);
        gtk_widget_set_tooltip_text (en->btn[i], info);
    }
    en->pose_en_cours = FALSE;
}

GtkWidget *
console_energie_new (gboolean apercu)
{
    Energie *en = g_new0 (Energie, 1);
    en->apercu = apercu;

    /* PLUS D'EN-TETE. Le titre « Veille de l'écran » et sa roue crantee ont
     * ete retires le 11 septembre 2026 : la roue ouvrait les Reglages, que
     * le bouton « Réglages » juste au-dessus ouvre deja. Deux portes vers la
     * meme piece, a trois centimetres l'une de l'autre. Les icones des modes
     * disent desormais ce que la rangee regle. */
    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (boite, "qs-rangee");

    GtkWidget *rangee = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous (GTK_BOX (rangee), TRUE);

    const ShellModeEnergie *modes = shell_energie_modes ();
    for (int i = 0; modes[i].id != NULL && i < (int) G_N_ELEMENTS (en->btn); i++) {
        /* L'icone au-dessus du nom : trois boutons cote a cote dans la
         * largeur de la Console, et « Automatique » ne laisserait pas la
         * place a une icone a cote de lui. Deux lignes font aussi une cible
         * plus haute pour le doigt. */
        g_autoptr(GIcon) gicone = shell_energie_mode_icone (&modes[i]);
        GtkWidget *icone = gtk_image_new_from_gicon (gicone);
        gtk_image_set_pixel_size (GTK_IMAGE (icone), 20);
        GtkWidget *nom = gtk_label_new (modes[i].nom);
        gtk_widget_add_css_class (nom, "qs-alim-nom");
        GtkWidget *contenu = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
        gtk_widget_set_halign (contenu, GTK_ALIGN_CENTER);
        gtk_box_append (GTK_BOX (contenu), icone);
        gtk_box_append (GTK_BOX (contenu), nom);

        en->btn[i] = gtk_toggle_button_new ();
        gtk_button_set_child (GTK_BUTTON (en->btn[i]), contenu);
        /* « qs-alim » donne la forme, « qs-mode » l'etat coche. Deux classes
         * plutot qu'une : les boutons de la rangee Alimentation partagent la
         * forme mais sont des ACTIONS, jamais cochees. Styler leur classe
         * commune pour l'etat coche aurait melange les deux natures. */
        gtk_widget_add_css_class (en->btn[i], "qs-alim");
        gtk_widget_add_css_class (en->btn[i], "qs-mode");
        if (i > 0)
            gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (en->btn[i]),
                                         GTK_TOGGLE_BUTTON (en->btn[0]));
        g_signal_connect (en->btn[i], "toggled",
                          G_CALLBACK (energie_poser), en);
        gtk_box_append (GTK_BOX (rangee), en->btn[i]);
        en->n = i + 1;
    }
    gtk_box_append (GTK_BOX (boite), rangee);

    g_object_set_data_full (G_OBJECT (boite), "energie", en, g_free);
    console_energie_relire (boite);
    return boite;
}

GtkWidget *
console_alimentation_new (ConsoleFermer fermer, gpointer data,
                          gboolean apercu)
{
    /* Les etats vivent aussi longtemps que la rangee ; ils sont liberes
     * avec elle par g_object_set_data_full. */
    Action *actions = g_new0 (Action, 5);

    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class (boite, "qs-alim-rangee");
    gtk_box_set_homogeneous (GTK_BOX (boite), TRUE);

    /* DU PLUS BENIN AU PLUS DEFINITIF, de gauche a droite. Eteindre au bout
     * de la rangee, la ou le pouce n'arrive pas par hasard ; Verrouiller en
     * premier, le seul qui ne demande pas de confirmation. */
    gtk_box_append (GTK_BOX (boite),
        action_new (&actions[0], "Verrouiller", "system-lock-screen-symbolic",
                    ACTION_VERROU, NULL, FALSE, fermer, data, apercu));
    gtk_box_append (GTK_BOX (boite),
        action_new (&actions[1], "Fermer la session", "system-log-out-symbolic",
                    ACTION_DECONNEXION, NULL, TRUE, fermer, data, apercu));
    gtk_box_append (GTK_BOX (boite),
        action_new (&actions[2], "Veille", "weather-clear-night-symbolic",
                    ACTION_LOGIND, "Suspend", TRUE, fermer, data, apercu));
    gtk_box_append (GTK_BOX (boite),
        action_new (&actions[3], "Redémarrer", "system-reboot-symbolic",
                    ACTION_LOGIND, "Reboot", TRUE, fermer, data, apercu));
    gtk_box_append (GTK_BOX (boite),
        action_new (&actions[4], "Éteindre", "system-shutdown-symbolic",
                    ACTION_LOGIND, "PowerOff", TRUE, fermer, data, apercu));

    g_object_set_data_full (G_OBJECT (boite), "actions", actions, g_free);
    return boite;
}
