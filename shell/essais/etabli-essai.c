/* =========================================================================
 * Claude OS — Banc de l'établi : une application témoin, avec fenêtre
 *
 * Le témoin de claude-os-outils n'a pas de fenêtre, et c'est voulu : il
 * prouve qu'une application sans GTK peut porter le contrat. Mais le dock ne
 * découvre une application QUE par ses fenêtres — wlr-foreign-toplevel ne
 * connaît rien d'autre. Sans fenêtre, aucune barre ne sera jamais posée.
 *
 * D'où ce second témoin : une application GTK ordinaire, avec une vraie
 * fenêtre que le compositeur signale, et qui publie une barre. C'est lui qui
 * éprouve l'établi de bout en bout.
 *
 * IL PORTE AUSSI SON VOLET DE REPLI, et c'est la moitié la moins visible du
 * contrat : une application dont les outils vivent dans un autre processus
 * doit rester utilisable sans lui. Le volet est montré tant que le dock n'a
 * pas dit « Prise », escamoté ensuite. Le banc vérifie les deux états.
 *
 * Il se pilote par le bus, comme le reste :
 *
 *   gapplication action os.claude.shell.essai-etabli aller "'file:///tmp'"
 *   gapplication action os.claude.shell.essai-etabli creuser
 *   gapplication action os.claude.shell.essai-etabli etat
 *
 * Chaque ligne de trace commence par « [banc] ».
 * ========================================================================= */

#include <gtk/gtk.h>

#include "outils.h"

static struct {
    GtkWidget   *fenetre;
    GtkWidget   *repli;      /* le volet interne, montré sans dock */
    GtkWidget   *etiquette;
    GMenu       *barre;
    GMenu       *fil;
    ShellOutils *outils;
    gboolean     prise;
    int          profondeur;
} E;

/* -------------------------------------------------------------------------
 * Les actions : celles-là mêmes que porterait le menu contextuel
 * ------------------------------------------------------------------------- */
static void
sur_aller (GSimpleAction *a, GVariant *but, gpointer data)
{
    (void) data;
    /* POSER L'ÉTAT, C'EST DIRE OÙ L'ON EST. Le dock allume l'entrée dont la
     * cible vaut cet état — la sémantique radio de GMenu. Une vraie
     * application le ferait après avoir navigué, pas avant. */
    g_simple_action_set_state (a, g_variant_ref (but));
    g_print ("[banc] aller %s\n", g_variant_get_string (but, NULL));
    gtk_label_set_text (GTK_LABEL (E.etiquette), g_variant_get_string (but, NULL));
}

static void
sur_chercher (GSimpleAction *a, GVariant *texte, gpointer data)
{
    (void) a; (void) data;
    g_print ("[banc] chercher « %s »\n", g_variant_get_string (texte, NULL));
}

static const GActionEntry ACTIONS[] = {
    { "aller",    sur_aller,    "s", "'file:///home/stef'", NULL, { 0 } },
    { "chercher", sur_chercher, "s", NULL,                  NULL, { 0 } },
};

/* -------------------------------------------------------------------------
 * La barre
 * ------------------------------------------------------------------------- */
static GMenuItem *
entree (const char *label, const char *but, const char *icone, const char *forme)
{
    GMenuItem *it = g_menu_item_new (label, NULL);
    g_menu_item_set_action_and_target_value (it, "outils.aller",
                                             g_variant_new_string (but));
    if (icone != NULL)
        g_menu_item_set_attribute (it, "icon", "s", icone);
    if (forme != NULL)
        g_menu_item_set_attribute (it, SHELL_OUTILS_A_FORME, "s", forme);
    return it;
}

static void
section (GMenu *barre, const char *zone, const char *titre, GMenu *contenu)
{
    GMenuItem *sec = g_menu_item_new_section (titre, G_MENU_MODEL (contenu));
    g_menu_item_set_attribute (sec, SHELL_OUTILS_A_ZONE, "s", zone);
    g_menu_append_item (barre, sec);
    g_object_unref (sec);
}

static void
batir_barre (void)
{
    E.barre = g_menu_new ();

    GMenu *lieux = g_menu_new ();
    struct { const char *nom, *uri, *icone; } L[] = {
        { "Accueil",   "file:///home/stef",            "user-home-symbolic" },
        { "Documents", "file:///home/stef/Documents",  "folder-documents-symbolic" },
        { "Images",    "file:///home/stef/Images",     "folder-pictures-symbolic" },
        { "Corbeille", "file:///home/stef/.local/share/Trash/files",
                                                       "user-trash-symbolic" },
    };
    for (guint i = 0; i < G_N_ELEMENTS (L); i++) {
        GMenuItem *it = entree (L[i].nom, L[i].uri, L[i].icone, SHELL_OUTILS_LIEU);
        g_menu_append_item (lieux, it);
        g_object_unref (it);
    }
    section (E.barre, SHELL_OUTILS_ZONE_LIEUX, "Personnel", lieux);
    g_object_unref (lieux);

    GMenu *outils = g_menu_new ();

    /* LE CHEMIN EST UN BOUTON A AUVENT (contrat 2) : la zone « fil » n'existe
     * plus, l'etabli ne porte que des boutons. « creuser » allonge le
     * sous-menu, et le dock suit sans qu'on ait a le prevenir. */
    E.fil = g_menu_new ();
    GMenuItem *racine = entree ("Accueil", "file:///home/stef", NULL, NULL);
    g_menu_append_item (E.fil, racine);
    g_object_unref (racine);

    GMenuItem *chemin = g_menu_item_new ("Chemin", NULL);
    g_menu_item_set_attribute (chemin, SHELL_OUTILS_A_FORME, "s", SHELL_OUTILS_AUVENT);
    g_menu_item_set_attribute (chemin, SHELL_OUTILS_A_CONTROLE, "s", SHELL_OUTILS_LISTE);
    g_menu_item_set_attribute (chemin, "icon", "s", "view-list-symbolic");
    g_menu_item_set_link (chemin, G_MENU_LINK_SUBMENU, G_MENU_MODEL (E.fil));
    g_menu_append_item (outils, chemin);
    g_object_unref (chemin);
    GMenuItem *loupe = g_menu_item_new ("Rechercher", "outils.chercher");
    g_menu_item_set_attribute (loupe, SHELL_OUTILS_A_FORME, "s", SHELL_OUTILS_AUVENT);
    g_menu_item_set_attribute (loupe, SHELL_OUTILS_A_CONTROLE, "s", SHELL_OUTILS_SAISIE);
    g_menu_item_set_attribute (loupe, SHELL_OUTILS_A_INVITE, "s", "Nom du fichier…");
    g_menu_item_set_attribute (loupe, SHELL_OUTILS_A_CLE, "s", "Ctrl+F");
    g_menu_item_set_attribute (loupe, "icon", "s", "system-search-symbolic");
    g_menu_append_item (outils, loupe);
    g_object_unref (loupe);
    section (E.barre, SHELL_OUTILS_ZONE_OUTILS, NULL, outils);
    g_object_unref (outils);
}

/* Allonger le fil : c'est l'épreuve du modèle qui change SOUS le dock. */
static void
act_creuser (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;

    static const char *noms[] = { "Images", "2026", "Septembre", "Brouillons" };
    if (E.profondeur >= (int) G_N_ELEMENTS (noms)) {
        g_print ("[banc] fil déjà au plus profond\n");
        return;
    }

    g_autofree char *uri = g_strdup_printf ("file:///creux/%d", E.profondeur);
    GMenuItem *it = entree (noms[E.profondeur], uri, NULL, NULL);
    g_menu_append_item (E.fil, it);
    g_object_unref (it);
    E.profondeur++;
    g_print ("[banc] fil creusé à %d étapes\n", E.profondeur + 1);
}

static void
act_etat (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    g_print ("[banc] etat prise=%d repli_visible=%d\n",
             E.prise ? 1 : 0,
             gtk_widget_get_visible (E.repli) ? 1 : 0);
}

static const GActionEntry ACTIONS_BANC[] = {
    { "creuser", act_creuser, NULL, NULL, NULL, { 0 } },
    { "etat",    act_etat,    NULL, NULL, NULL, { 0 } },
};

/* -------------------------------------------------------------------------
 * Le repli
 * ------------------------------------------------------------------------- */
static void
sur_prise (gboolean prise, gpointer data)
{
    (void) data;
    E.prise = prise;

    /* TOUT LE REPLI TIENT DANS CETTE LIGNE, et c'est ce qui rend une
     * application utilisable sans dock. Une vraie application montrerait ici
     * son volet d'emplacements ; le témoin se contente de le dire. */
    gtk_widget_set_visible (E.repli, !prise);

    g_print ("[banc] prise=%d — le volet interne est %s\n",
             prise ? 1 : 0, prise ? "escamoté" : "montré");
}

/* ------------------------------------------------------------------------- */
static void
on_activate (GtkApplication *app, gpointer data)
{
    (void) data;

    E.fenetre = gtk_application_window_new (app);
    gtk_window_set_title (GTK_WINDOW (E.fenetre), "Témoin de l'établi");
    gtk_window_set_default_size (GTK_WINDOW (E.fenetre), 900, 560);

    GtkWidget *rangee = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

    /* Le volet de repli : ce que l'application montre quand le dock ne
     * prend pas ses outils en charge. */
    E.repli = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request (E.repli, 180, -1);
    gtk_box_append (GTK_BOX (E.repli), gtk_label_new ("Volet interne"));
    gtk_box_append (GTK_BOX (E.repli), gtk_label_new ("(pas de dock)"));
    gtk_box_append (GTK_BOX (rangee), E.repli);

    E.etiquette = gtk_label_new ("file:///home/stef");
    gtk_widget_set_hexpand (E.etiquette, TRUE);
    gtk_box_append (GTK_BOX (rangee), E.etiquette);

    gtk_window_set_child (GTK_WINDOW (E.fenetre), rangee);

    GSimpleActionGroup *groupe = g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (groupe), ACTIONS,
                                     G_N_ELEMENTS (ACTIONS), NULL);
    gtk_widget_insert_action_group (E.fenetre, "outils", G_ACTION_GROUP (groupe));

    g_action_map_add_action_entries (G_ACTION_MAP (app), ACTIONS_BANC,
                                     G_N_ELEMENTS (ACTIONS_BANC), NULL);

    batir_barre ();

    gtk_window_present (GTK_WINDOW (E.fenetre));

    /* APRÈS la fenêtre : la publication déclenche la présentation au dock,
     * qui voudra savoir si notre fenêtre est active. */
    E.outils = shell_outils_publier (G_APPLICATION (app), "Témoin",
                                     G_ACTION_GROUP (groupe),
                                     G_MENU_MODEL (E.barre), sur_prise, NULL);
    if (E.outils == NULL)
        g_print ("[banc] rien publié : pas de bus ?\n");

    g_print ("[banc] prêt\n");
}

int
main (int argc, char **argv)
{
    GtkApplication *app = gtk_application_new ("os.claude.shell.essai-etabli",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
    int r = g_application_run (G_APPLICATION (app), argc, argv);
    shell_outils_retirer (E.outils);
    g_object_unref (app);
    return r;
}
