#include "etabli.h"

#include <string.h>

#include "auvent.h"
#include "outils.h"

#define ICONE_LIEU   24
#define ICONE_OUTIL  20

struct _ShellEtabli {
    GtkWidget  parent_instance;

    GtkWidget *colonne;     /* l'auvent au-dessus, la pilule en dessous     */
    GtkWidget *rangee;      /* la pilule                                    */
    ShellAuvent *auvent;
    GtkWidget *apps;        /* au bureau, fourni par le dock                */
    GtkWidget *lieux;
    GtkWidget *fil;
    GtkWidget *fil_defil;   /* un chemin profond déborde : il défile        */
    GtkWidget *outils;
    GtkWidget *retour;

    GMenuModel   *barre;
    GActionGroup *actions;
    gulong        sur_items;
    gulong        sur_etat;

    /* UN GDBusMenuModel ARRIVE PAR ÉTAGES, et c'est le piège de tout ce
     * fichier. Le modèle racine signale « trois sections » bien avant que
     * ces sections aient le moindre contenu : chacune est un modèle à elle,
     * qui se remplit par le bus et émet SON PROPRE « items-changed ».
     *
     * Écouter le seul modèle racine, c'est donc reconstruire un établi de
     * trois sections vides, et ne plus jamais être prévenu. Mesuré au banc
     * le 16 septembre 2026 : trois sections annoncées, zéro entrée posée,
     * et pas une plainte -- l'établi n'avait rien à redire de ce qu'il
     * n'avait pas reçu.
     *
     * On suit donc chaque sous-modèle rencontré, et l'on coupe tout à la
     * reconstruction suivante : les sections d'hier ne sont pas celles
     * d'aujourd'hui. */
    GPtrArray    *suivis;     /* Suivi*, un par sous-modèle écouté */

    /* Les entrées allumables : bouton → cible. L'allumage ne reconstruit
     * rien, il ne fait que poser une classe (voir l'en-tête). */
    GPtrArray *allumables;   /* GtkWidget*, non possédés   */
    GPtrArray *cibles;       /* GVariant*, possédés        */
    GPtrArray *sur_actions;  /* char*, le nom nu de l'action */

    ShellEtabliRetour retour_cb;
    gpointer          retour_data;
    ShellEtabliTaille taille_cb;
    gpointer          taille_data;
    ShellEtabliAuvent auvent_cb;
    gpointer          auvent_data;
};

/* Ce qu'un bouton d'auvent a besoin de savoir pour ouvrir le sien. Attaché
 * au bouton, libéré avec lui : les entrées sont refaites à chaque
 * reconstruction, et une table parallèle se désynchroniserait. */
typedef struct {
    ShellEtabli *etabli;
    char        *action;     /* le nom NU, sans le préfixe */
    char        *controle;
    char        *invite;
} Ouvreur;

static void
ouvreur_libre (gpointer data)
{
    Ouvreur *o = data;
    g_free (o->action);
    g_free (o->controle);
    g_free (o->invite);
    g_free (o);
}

typedef struct {
    GMenuModel *modele;
    gulong      handler;
} Suivi;

static void
suivi_libre (gpointer data)
{
    Suivi *s = data;
    if (s->handler != 0)
        g_signal_handler_disconnect (s->modele, s->handler);
    g_object_unref (s->modele);
    g_free (s);
}

G_DEFINE_FINAL_TYPE (ShellEtabli, shell_etabli, GTK_TYPE_WIDGET)

/* ------------------------------------------------------------------------- */
static char *
attribut (GMenuModel *m, int i, const char *nom)
{
    g_autoptr(GVariant) v = g_menu_model_get_item_attribute_value (
        m, i, nom, G_VARIANT_TYPE_STRING);
    return v ? g_variant_dup_string (v, NULL) : NULL;
}

/* Le repli d'icône, comme partout dans ce shell : une icône absente du thème
 * affiche un carré barré, ce qui est bien plus laid qu'un pictogramme
 * générique. GTK descend dans Papirus avant d'abandonner, mais pas toujours
 * jusqu'à quelque chose de sensé. */
static GtkWidget *
image (const char *nom, int taille)
{
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
    GtkWidget *img = gtk_image_new_from_icon_name (
        (nom != NULL && gtk_icon_theme_has_icon (theme, nom))
            ? nom : "application-x-executable");
    gtk_image_set_pixel_size (GTK_IMAGE (img), taille);
    return img;
}

/* Le nom nu d'une action : « outils.aller » → « aller ». Le groupe importé
 * porte les actions sans préfixe ; c'est le modèle qui les préfixe, et c'est
 * sous « outils » que le dock insère le groupe. Voir outils.h. */
static const char *
nom_nu (const char *action)
{
    const char *point = action ? strchr (action, '.') : NULL;
    return point ? point + 1 : action;
}

static void
retenir_allumable (ShellEtabli *e, GtkWidget *w, const char *action, GVariant *but)
{
    if (action == NULL || but == NULL)
        return;
    g_ptr_array_add (e->allumables, w);
    g_ptr_array_add (e->cibles, g_variant_ref_sink (but));
    g_ptr_array_add (e->sur_actions, g_strdup (nom_nu (action)));
}

/* Allumer ce qui doit l'être, et rien d'autre.
 *
 * AUCUNE RECONSTRUCTION ICI, et c'est tout l'intérêt : l'entrée courante
 * change à chaque navigation, et refaire la rangée déplacerait la largeur de
 * la pilule -- donc la surface, donc les popovers de labwc. */
static void
allumer (ShellEtabli *e)
{
    for (guint i = 0; i < e->allumables->len; i++) {
        GtkWidget  *w   = g_ptr_array_index (e->allumables, i);
        GVariant   *but = g_ptr_array_index (e->cibles, i);
        const char *act = g_ptr_array_index (e->sur_actions, i);

        gboolean ici = FALSE;
        if (e->actions != NULL) {
            g_autoptr(GVariant) etat =
                g_action_group_get_action_state (e->actions, act);
            ici = (etat != NULL && g_variant_equal (etat, but));
        }
        if (ici)
            gtk_widget_add_css_class (w, "ici");
        else
            gtk_widget_remove_css_class (w, "ici");
    }
}

/* -------------------------------------------------------------------------
 * L'auvent
 * ------------------------------------------------------------------------- */

/* La valeur du contrôle part vers l'application, telle quelle.
 *
 * L'ACTION EST ACTIVÉE DIRECTEMENT, et non par un GtkActionable : le bouton
 * ouvre le volet, c'est le VOLET qui porte la valeur. Les brancher tous deux
 * sur la même action reviendrait à l'activer sans paramètre au clic, ce que
 * GTK refuse bruyamment -- payé le 16 septembre 2026, un avertissement par
 * survol. */
static void
sur_valeur (const char *valeur, gpointer data)
{
    Ouvreur *o = data;

    if (o->etabli->actions == NULL)
        return;
    g_action_group_activate_action (o->etabli->actions, o->action,
                                    g_variant_new_string (valeur));
}

static void
on_ouvrir (GtkButton *b, gpointer data)
{
    Ouvreur *o = data;
    (void) b;

    shell_auvent_ouvrir (o->etabli->auvent, o->controle, o->invite,
                         sur_valeur, o);
}

static void
on_auvent (gboolean ouvert, gpointer data)
{
    ShellEtabli *e = data;

    /* Le dock en a besoin pour DEUX choses : le mode clavier de sa surface,
     * et la fermeture de ses popovers -- la hauteur va changer. */
    if (e->auvent_cb != NULL)
        e->auvent_cb (ouvert, e->auvent_data);
    if (e->taille_cb != NULL)
        e->taille_cb (e->taille_data);
}

void
shell_etabli_fermer_auvent (ShellEtabli *e)
{
    if (e != NULL)
        shell_auvent_fermer (e->auvent);
}

gboolean
shell_etabli_auvent_ouvert (ShellEtabli *e)
{
    return e != NULL && shell_auvent_ouvert (e->auvent);
}

void
shell_etabli_sur_auvent (ShellEtabli *e, ShellEtabliAuvent f, gpointer data)
{
    e->auvent_cb   = f;
    e->auvent_data = data;
}

/* -------------------------------------------------------------------------
 * Les formes
 * ------------------------------------------------------------------------- */
static void
brancher (GtkWidget *bouton, const char *action, GVariant *but)
{
    if (action == NULL)
        return;
    gtk_actionable_set_action_name (GTK_ACTIONABLE (bouton), action);
    if (but != NULL)
        gtk_actionable_set_action_target_value (GTK_ACTIONABLE (bouton), but);
}

/* Un lieu : icône au-dessus, libellé dessous. Le libellé est là parce qu'un
 * lieu n'est pas une application -- « Documents » et « Téléchargements »
 * partagent le même pictogramme de dossier dans la plupart des thèmes, et
 * une rangée d'icônes identiques ne sert à rien. */
static GtkWidget *
forme_lieu (const char *label, const char *icone, const char *astuce)
{
    GtkWidget *boite = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append (GTK_BOX (boite), image (icone, ICONE_LIEU));

    if (label != NULL) {
        GtkWidget *l = gtk_label_new (label);
        gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars (GTK_LABEL (l), 10);
        gtk_widget_add_css_class (l, "etabli-libelle");
        gtk_box_append (GTK_BOX (boite), l);
    }

    GtkWidget *b = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (b), boite);
    gtk_widget_add_css_class (b, "etabli-lieu");
    if (astuce != NULL)
        gtk_widget_set_tooltip_text (b, astuce);
    return b;
}

/* Une étape du fil d'Ariane : du texte, et rien d'autre. Le chevron est posé
 * par la zone, pas par l'entrée -- il sépare, il n'appartient à personne. */
static GtkWidget *
forme_etape (const char *label)
{
    GtkWidget *b = gtk_button_new_with_label (label ? label : "…");
    gtk_widget_add_css_class (b, "etabli-etape");
    return b;
}

static GtkWidget *
forme_bouton (const char *icone, const char *astuce, const char *cle)
{
    GtkWidget *b = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (b), image (icone, ICONE_OUTIL));
    gtk_widget_add_css_class (b, "etabli-outil");

    /* L'infobulle porte le raccourci quand l'application en a déclaré un.
     * MONTRER SEULEMENT : c'est l'application qui l'arme, le dock
     * n'intercepte aucune touche -- un dock qui volerait des touches serait
     * une source de pannes indéchiffrables. */
    if (astuce != NULL && cle != NULL) {
        g_autofree char *t = g_strdup_printf ("%s  (%s)", astuce, cle);
        gtk_widget_set_tooltip_text (b, t);
    } else if (astuce != NULL) {
        gtk_widget_set_tooltip_text (b, astuce);
    }
    return b;
}

/* -------------------------------------------------------------------------
 * Construction depuis le modèle
 * ------------------------------------------------------------------------- */
/* Les enfants d'une boîte. Le fil en compte deux par étape sauf la première
 * -- le chevron qui la précède -- d'où la division chez l'appelant. */
static int
compter (GtkWidget *boite)
{
    int n = 0;
    for (GtkWidget *c = gtk_widget_get_first_child (boite);
         c != NULL; c = gtk_widget_get_next_sibling (c))
        n++;
    return n;
}

static void
vider (GtkWidget *boite)
{
    GtkWidget *enfant = gtk_widget_get_first_child (boite);
    while (enfant != NULL) {
        GtkWidget *suivant = gtk_widget_get_next_sibling (enfant);
        gtk_box_remove (GTK_BOX (boite), enfant);
        enfant = suivant;
    }
}

/* LA ZONE VIENT DE LA SECTION, LA FORME VIENT DE L'ENTRÉE, et les deux ne se
 * commandent pas l'une l'autre. C'est le contrat (outils.h) : la section dit
 * OÙ, l'entrée dit COMMENT. Les confondre paraîtrait plus simple -- une
 * étape irait forcément au fil -- jusqu'au jour où une application voudra un
 * lieu dans la zone des outils, et devra mentir sur l'un pour obtenir
 * l'autre. */
static GtkWidget *
boite_de_zone (ShellEtabli *e, const char *zone)
{
    if (g_str_equal (zone, SHELL_OUTILS_ZONE_LIEUX))
        return e->lieux;
    if (g_str_equal (zone, SHELL_OUTILS_ZONE_FIL))
        return e->fil;
    if (g_str_equal (zone, SHELL_OUTILS_ZONE_OUTILS))
        return e->outils;

    if (g_str_equal (zone, SHELL_OUTILS_ZONE_AUVENT)) {
        g_message ("établi : zone « auvent » pas encore écrite (étape 4) — "
                   "son contenu est posé dans les outils");
        return e->outils;
    }

    /* Une zone inconnue n'est pas une zone vide : le dire, et poser le
     * contenu quelque part de visible plutôt que de le perdre. */
    g_message ("établi : zone inconnue « %s » — posée dans les outils", zone);
    return e->outils;
}

static void
poser_entree (ShellEtabli *e, GMenuModel *m, int i, const char *zone,
              gboolean premier)
{
    g_autofree char *label  = attribut (m, i, G_MENU_ATTRIBUTE_LABEL);
    g_autofree char *action = attribut (m, i, G_MENU_ATTRIBUTE_ACTION);
    g_autofree char *icone  = attribut (m, i, "icon");
    g_autofree char *forme  = attribut (m, i, SHELL_OUTILS_A_FORME);
    g_autofree char *astuce = attribut (m, i, SHELL_OUTILS_A_ASTUCE);
    g_autofree char *cle    = attribut (m, i, SHELL_OUTILS_A_CLE);

    GVariant *but = g_menu_model_get_item_attribute_value (
        m, i, G_MENU_ATTRIBUTE_TARGET, NULL);

    GtkWidget  *ou = boite_de_zone (e, zone);
    const char *f  = forme ? forme : SHELL_OUTILS_BOUTON;
    GtkWidget  *bouton;

    if (g_str_equal (f, SHELL_OUTILS_LIEU)) {
        bouton = forme_lieu (label, icone, astuce ? astuce : label);

    } else if (g_str_equal (f, SHELL_OUTILS_ETAPE)) {
        /* Le chevron avant chaque étape sauf la première : il SÉPARE, il
         * n'appartient à aucune des deux. */
        if (!premier) {
            GtkWidget *chevron = gtk_label_new ("›");
            gtk_widget_add_css_class (chevron, "etabli-chevron");
            gtk_box_append (GTK_BOX (ou), chevron);
        }
        bouton = forme_etape (label);

    } else if (g_str_equal (f, SHELL_OUTILS_AUVENT)) {
        /* L'AUVENT EST L'ÉTAPE 4. En attendant, l'entrée existe et
         * fonctionne en bouton -- l'action part, simplement sans le
         * contrôle qui devait la remplir. Le dire plutôt que d'afficher un
         * trou : c'est la règle du contrat pour tout ce qu'on ne sait pas
         * encore dessiner. */
        /* LE BOUTON N'EST PAS BRANCHÉ SUR L'ACTION, il ouvre le volet.
         *
         * L'action d'un auvent attend la valeur du contrôle -- une chaîne
         * pour une saisie. La brancher AUSSI sur le bouton reviendrait à
         * l'activer sans paramètre au clic, ce que GTK refuse bruyamment :
         * un avertissement par survol, en boucle. C'est le volet qui porte
         * la valeur, et lui seul. */
        bouton = forme_bouton (icone, astuce ? astuce : label, cle);

        Ouvreur *ouvreur = g_new0 (Ouvreur, 1);
        ouvreur->etabli   = e;
        ouvreur->action   = g_strdup (nom_nu (action));
        ouvreur->controle = g_strdup (attribut (m, i, SHELL_OUTILS_A_CONTROLE));
        ouvreur->invite   = g_strdup (attribut (m, i, SHELL_OUTILS_A_INVITE));

        g_object_set_data_full (G_OBJECT (bouton), "ouvreur", ouvreur,
                                ouvreur_libre);
        g_signal_connect (bouton, "clicked", G_CALLBACK (on_ouvrir), ouvreur);

        gtk_box_append (GTK_BOX (ou), bouton);
        if (but != NULL)
            g_variant_unref (but);
        return;

    } else {
        bouton = forme_bouton (icone, astuce ? astuce : label, cle);
    }

    brancher (bouton, action, but);
    retenir_allumable (e, bouton, action, but);
    gtk_box_append (GTK_BOX (ou), bouton);

    if (but != NULL)
        g_variant_unref (but);
}

static void on_items (GMenuModel *m, int pos, int retires, int ajoutes,
                      gpointer data);
static void suivre (ShellEtabli *e, GMenuModel *m);

/* UNE SECTION PEUT EN CONTENIR D'AUTRES, et il faut y descendre.
 *
 * GMenuModel est récursif par nature, et une application a de bonnes raisons
 * de s'en servir : Fichiers compose sa barre à partir du modèle que son
 * volet des lieux tient à jour tout seul, et ce modèle a ses propres
 * sections -- Emplacements, Favoris, Périphériques. Les poser telles quelles
 * dans la barre donnait, au 16 septembre 2026, ZÉRO lieu et deux boutons
 * vides : chaque sous-section était traitée comme une entrée ordinaire,
 * sans libellé ni action.
 *
 * La zone se transmet de parent en enfant, sauf si l'enfant déclare la
 * sienne. Une application peut donc grouper sans avoir à répéter la zone sur
 * chaque morceau -- et une bibliothèque qui produit un modèle de lieux n'a
 * pas à savoir dans quelle zone on la posera. */
static void
poser_section (ShellEtabli *e, GMenuModel *section, const char *zone)
{
    int n = g_menu_model_get_n_items (section);
    int rang = 0;

    for (int i = 0; i < n; i++) {
        g_autoptr(GMenuModel) sous = g_menu_model_get_item_link (
            section, i, G_MENU_LINK_SECTION);

        if (sous != NULL) {
            suivre (e, sous);
            g_autofree char *sienne = attribut (section, i, SHELL_OUTILS_A_ZONE);
            poser_section (e, sous, sienne ? sienne : zone);
            continue;
        }
        poser_entree (e, section, i, zone, rang++ == 0);
    }
}


/* Écouter un sous-modèle : il se remplira après coup, et c'est lui qui le
 * dira. Voir le champ `suivis` pour la raison. */
static void
suivre (ShellEtabli *e, GMenuModel *m)
{
    Suivi *s = g_new0 (Suivi, 1);
    s->modele  = g_object_ref (m);
    s->handler = g_signal_connect (m, "items-changed", G_CALLBACK (on_items), e);
    g_ptr_array_add (e->suivis, s);
}

static void
reconstruire (ShellEtabli *e)
{
    g_ptr_array_set_size (e->suivis, 0);
    vider (e->lieux);
    vider (e->fil);
    vider (e->outils);
    g_ptr_array_set_size (e->allumables, 0);
    g_ptr_array_set_size (e->cibles, 0);
    g_ptr_array_set_size (e->sur_actions, 0);

    int n = e->barre ? g_menu_model_get_n_items (e->barre) : 0;
    for (int i = 0; i < n; i++) {
        g_autoptr(GMenuModel) section = g_menu_model_get_item_link (
            e->barre, i, G_MENU_LINK_SECTION);

        /* UNE ENTRÉE HORS SECTION EST IGNORÉE, et on le dit. Le contrat veut
         * des sections : c'est la section qui porte la zone, et une entrée
         * sans zone n'a pas d'endroit où aller. */
        if (section == NULL) {
            g_message ("établi : entrée hors section, ignorée — le contrat "
                       "veut une section par zone (voir outils.h)");
            continue;
        }

        suivre (e, section);

        g_autofree char *zone = attribut (e->barre, i, SHELL_OUTILS_A_ZONE);
        poser_section (e, section, zone ? zone : SHELL_OUTILS_ZONE_OUTILS);
    }

    /* Les séparateurs ne se montrent que si ce qu'ils séparent existe : une
     * pilule vide bordée de trois traits verticaux se lit comme une panne. */
    gtk_widget_set_visible (e->lieux, gtk_widget_get_first_child (e->lieux) != NULL);
    gtk_widget_set_visible (e->fil_defil, gtk_widget_get_first_child (e->fil) != NULL);
    gtk_widget_set_visible (e->outils, gtk_widget_get_first_child (e->outils) != NULL);

    allumer (e);

    /* CE QU'IL PORTE, APRÈS CHAQUE RECONSTRUCTION. En débogage seulement --
     * un modèle distant arrive en plusieurs salves, et une ligne par salve
     * remplirait le journal d'une session entière.
     *
     * C'est la seule façon de vérifier de l'extérieur qu'une barre est
     * arrivée ENTIÈRE : le banc lit cette ligne, et personne ne peut
     * compter des widgets depuis un autre processus. */
    /* Le fil compte deux enfants par étape sauf la première -- le chevron
     * qui la précède. Vide, il en compte zéro, et non une : la formule
     * annonçait « 1 étapes » sur un fil qu'on n'avait pas encore reçu. */
    int etapes = compter (e->fil);
    g_debug ("établi : %d lieux, %d étapes, %d outils",
             compter (e->lieux), etapes == 0 ? 0 : etapes / 2 + 1,
             compter (e->outils));

    if (e->taille_cb != NULL)
        e->taille_cb (e->taille_data);
}

/* ------------------------------------------------------------------------- */
static void
on_items (GMenuModel *m, int pos, int retires, int ajoutes, gpointer data)
{
    (void) m; (void) pos; (void) retires; (void) ajoutes;
    reconstruire (SHELL_ETABLI (data));
}

static void
on_etat (GActionGroup *g, const char *nom, GVariant *etat, gpointer data)
{
    (void) g; (void) nom; (void) etat;
    allumer (SHELL_ETABLI (data));
}

void
shell_etabli_poser (ShellEtabli *e, GMenuModel *barre, GActionGroup *actions)
{
    if (e->barre != NULL && e->sur_items != 0)
        g_signal_handler_disconnect (e->barre, e->sur_items);
    if (e->actions != NULL && e->sur_etat != 0)
        g_signal_handler_disconnect (e->actions, e->sur_etat);
    e->sur_items = e->sur_etat = 0;

    g_clear_object (&e->barre);
    g_clear_object (&e->actions);

    if (barre != NULL) {
        e->barre = g_object_ref (barre);
        e->sur_items = g_signal_connect (barre, "items-changed",
                                         G_CALLBACK (on_items), e);
    }
    if (actions != NULL) {
        e->actions = g_object_ref (actions);
        e->sur_etat = g_signal_connect (actions, "action-state-changed",
                                        G_CALLBACK (on_etat), e);

        /* AMORCE. GDBusActionGroup ne demande rien au serveur tant qu'on ne
         * lui a rien demandé : sans cet appel, l'état des actions n'arrive
         * jamais, et aucune entrée ne s'allume. Payé sur claude-os-outils
         * le 16 septembre 2026. */
        g_strfreev (g_action_group_list_actions (actions));
    }

    /* UN VOLET OUVERT SUR LA BARRE D'UNE AUTRE APPLICATION ÉCRIRAIT DANS LE
     * VIDE : son action appartenait au groupe qu'on vient de remplacer. */
    shell_auvent_fermer (e->auvent);

    gtk_widget_insert_action_group (GTK_WIDGET (e), SHELL_OUTILS_PREFIXE,
                                    actions);
    reconstruire (e);
}

gboolean
shell_etabli_garni (ShellEtabli *e)
{
    return e != NULL
        && (gtk_widget_get_first_child (e->lieux)  != NULL
         || gtk_widget_get_first_child (e->fil)    != NULL
         || gtk_widget_get_first_child (e->outils) != NULL);
}

gboolean
shell_etabli_ouvrir_auvent (ShellEtabli *e, const char *action)
{
    if (e == NULL || action == NULL)
        return FALSE;

    /* On retrouve le bouton par son ouvreur : c'est lui qui porte le nom de
     * l'action, le contrôle et l'invite. Chercher dans le modèle donnerait
     * la même réponse au prix d'un second parcours qui pourrait diverger. */
    for (GtkWidget *c = gtk_widget_get_first_child (e->outils);
         c != NULL; c = gtk_widget_get_next_sibling (c)) {
        Ouvreur *o = g_object_get_data (G_OBJECT (c), "ouvreur");
        if (o == NULL || g_strcmp0 (o->action, action) != 0)
            continue;
        on_ouvrir (GTK_BUTTON (c), o);
        return TRUE;
    }
    return FALSE;
}

gboolean
shell_etabli_actionner_outil (ShellEtabli *e, int n)
{
    if (e == NULL || n < 0)
        return FALSE;

    int i = 0;
    for (GtkWidget *c = gtk_widget_get_first_child (e->outils);
         c != NULL; c = gtk_widget_get_next_sibling (c), i++) {
        if (i != n)
            continue;
        if (!GTK_IS_BUTTON (c))
            return FALSE;
        g_signal_emit_by_name (c, "clicked");
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------------- */
static void
on_retour (GtkButton *b, gpointer data)
{
    ShellEtabli *e = data;
    (void) b;
    if (e->retour_cb != NULL)
        e->retour_cb (e->retour_data);
}

void
shell_etabli_sur_retour (ShellEtabli *e, ShellEtabliRetour f, gpointer data)
{
    e->retour_cb   = f;
    e->retour_data = data;
}

void
shell_etabli_sur_taille (ShellEtabli *e, ShellEtabliTaille f, gpointer data)
{
    e->taille_cb   = f;
    e->taille_data = data;
}

/* -------------------------------------------------------------------------
 * Cycle de vie
 * ------------------------------------------------------------------------- */
static GtkWidget *
separateur (void)
{
    GtkWidget *s = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (s, "dock-separator");
    return s;
}

static GtkWidget *
zone (const char *classe)
{
    GtkWidget *b = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class (b, classe);
    gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
    return b;
}

static void
etabli_measure (GtkWidget *w, GtkOrientation o, int pour,
                int *min, int *nat, int *min_base, int *nat_base)
{
    ShellEtabli *e = SHELL_ETABLI (w);
    gtk_widget_measure (e->colonne, o, pour, min, nat, min_base, nat_base);
}

static void
etabli_size_allocate (GtkWidget *w, int largeur, int hauteur, int base)
{
    ShellEtabli *e = SHELL_ETABLI (w);
    gtk_widget_allocate (e->colonne, largeur, hauteur, base, NULL);
}

static void
etabli_dispose (GObject *o)
{
    ShellEtabli *e = SHELL_ETABLI (o);

    if (e->barre != NULL && e->sur_items != 0)
        g_signal_handler_disconnect (e->barre, e->sur_items);
    if (e->actions != NULL && e->sur_etat != 0)
        g_signal_handler_disconnect (e->actions, e->sur_etat);
    e->sur_items = e->sur_etat = 0;

    g_clear_object (&e->barre);
    g_clear_object (&e->actions);
    g_clear_pointer (&e->suivis, g_ptr_array_unref);
    g_clear_pointer (&e->allumables, g_ptr_array_unref);
    g_clear_pointer (&e->cibles, g_ptr_array_unref);
    g_clear_pointer (&e->sur_actions, g_ptr_array_unref);
    g_clear_pointer (&e->colonne, gtk_widget_unparent);

    G_OBJECT_CLASS (shell_etabli_parent_class)->dispose (o);
}

static void
shell_etabli_class_init (ShellEtabliClass *klass)
{
    GObjectClass   *oc = G_OBJECT_CLASS (klass);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS (klass);

    oc->dispose       = etabli_dispose;
    wc->measure       = etabli_measure;
    wc->size_allocate = etabli_size_allocate;
}

static void
shell_etabli_init (ShellEtabli *e)
{
    e->suivis      = g_ptr_array_new_with_free_func (suivi_libre);
    e->allumables  = g_ptr_array_new ();
    e->cibles      = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
    e->sur_actions = g_ptr_array_new_with_free_func (g_free);
}

GtkWidget *
shell_etabli_new (GtkWidget *apps)
{
    ShellEtabli *e = g_object_new (SHELL_TYPE_ETABLI, NULL);

    /* L'AUVENT AU-DESSUS, LA PILULE EN DESSOUS. Une colonne, et non deux
     * surfaces : le volet doit monter DU dock, pas apparaître à côté. */
    e->colonne = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign (e->colonne, GTK_ALIGN_END);
    gtk_widget_set_parent (e->colonne, GTK_WIDGET (e));

    e->auvent = SHELL_AUVENT (shell_auvent_new ());
    shell_auvent_sur_ouverture (e->auvent, on_auvent, e);
    gtk_box_append (GTK_BOX (e->colonne), GTK_WIDGET (e->auvent));

    e->rangee = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class (e->rangee, "dock");
    gtk_widget_add_css_class (e->rangee, "etabli");
    gtk_widget_set_halign (e->rangee, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (e->colonne), e->rangee);

    e->apps   = apps;
    e->lieux  = zone ("etabli-lieux");
    e->fil    = zone ("etabli-fil");
    e->outils = zone ("etabli-outils");

    /* LE FIL DÉFILE, ET IL EST LE SEUL À LE FAIRE. Un chemin profond
     * déborderait la pilule et pousserait le bouton de retour hors de
     * l'écran -- or c'est précisément le bouton dont on doit toujours
     * disposer. Les lieux, eux, sont en nombre borné par l'application. */
    e->fil_defil = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (e->fil_defil),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (e->fil_defil), e->fil);
    gtk_widget_set_hexpand (e->fil_defil, FALSE);

    /* SANS CECI, LE FIL EST ÉCRASÉ À ZÉRO. Un GtkScrolledWindow demande par
     * défaut la place minimale -- c'est-à-dire presque rien : il sait
     * défiler, donc il accepte n'importe quelle largeur. Dans une boîte, il
     * la prend. Vu au banc le 16 septembre 2026 : quatre étapes posées,
     * aucune visible, et pas un avertissement.
     *
     * Il demande donc la largeur de son contenu, plafonnée : au-delà, c'est
     * lui qui défile plutôt que la pilule qui pousse le bouton de retour
     * hors de l'écran. La hauteur, elle, se propage sans plafond -- une
     * rangée d'étapes n'a qu'une ligne. */
    gtk_scrolled_window_set_propagate_natural_width (
        GTK_SCROLLED_WINDOW (e->fil_defil), TRUE);
    gtk_scrolled_window_set_propagate_natural_height (
        GTK_SCROLLED_WINDOW (e->fil_defil), TRUE);
    gtk_scrolled_window_set_max_content_width (
        GTK_SCROLLED_WINDOW (e->fil_defil), 420);

    if (apps != NULL) {
        gtk_box_append (GTK_BOX (e->rangee), apps);
        gtk_box_append (GTK_BOX (e->rangee), separateur ());
    }
    gtk_box_append (GTK_BOX (e->rangee), e->lieux);
    gtk_box_append (GTK_BOX (e->rangee), e->fil_defil);
    gtk_box_append (GTK_BOX (e->rangee), e->outils);

    /* LE RETOUR EST TOUJOURS LÀ, et il est au bureau -- une application ne
     * peut ni le retirer ni le déplacer. C'est la seule garantie qu'on a de
     * pouvoir revenir, quelle que soit la barre qu'on nous a servie. */
    gtk_box_append (GTK_BOX (e->rangee), separateur ());
    e->retour = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (e->retour),
                          image ("go-home-symbolic", ICONE_OUTIL));
    gtk_widget_add_css_class (e->retour, "etabli-retour");
    gtk_widget_set_tooltip_text (e->retour, "Revenir au bureau");
    g_signal_connect (e->retour, "clicked", G_CALLBACK (on_retour), e);
    gtk_box_append (GTK_BOX (e->rangee), e->retour);

    return GTK_WIDGET (e);
}
