/* =========================================================================
 * Claude-OS Shell — configuration et style, partages
 *
 * Le dock et la barre d'etat doivent lire EXACTEMENT la meme configuration.
 * Quand seul le dock la lisait, un « theme=dark » dans shell.conf donnait un
 * dock sombre et une barre claire cote a cote.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

typedef struct {
    char    **pinned;      /* identifiants .desktop, dans l'ordre d'affichage */
    char     *font;        /* famille de police de l'interface                */
    char     *icon_theme;  /* theme d'icones                                  */
    char     *theme;         /* nom du theme : voir shell_themes ()           */
    gboolean  dark;          /* deduit du theme, pour les widgets GTK natifs  */
    gboolean  reserve_space; /* le dock repousse-t-il les fenetres maximisees */

    /* Fond d'ecran. Chemin vide : le degrade dessine par le shell. */
    char     *wallpaper;     /* chemin d'une image, ou ""                    */
    gboolean  wallpaper_fill;/* couvrir en rognant plutot que tout montrer   */

    /* Mise en veille progressive -- voir energie.h pour le raisonnement.
     * Les delais sont en SECONDES ; zero ferme l'etage.
     *
     * TROIS MODES, ET LE MODE EST UN CHOIX, PAS UNE DEDUCTION.
     *
     * La version precedente derivait le comportement de la prise : secteur
     * ou batterie. C'etait commode et illisible -- personne ne savait dire
     * ce que la machine allait faire sans regarder le cable. On nomme donc
     * les modes, et l'utilisateur en choisit un.
     *
     * Chaque mode a SON jeu de delais. Croiser les modes avec la source
     * d'alimentation donnerait six jeux a regler, ce qui reviendrait a
     * rendre le panneau illisible pour eviter un clic. */
    gboolean  energie_active;
    char     *energie_mode;   /* « travail », « automatique », « nomade »   */
    int       energie_niveau; /* pourcent vise par l'etage « attenuer »     */

    /* Opacite du cadran de preavis, en pourcent. Se regle parce que le bon
     * equilibre depend du fond d'ecran et de la vue de chacun : trop
     * discret il ne previent pas, trop marque il occupe le coin de
     * l'ecran. Un curseur coute moins cher qu'un debat. */
    int       energie_opacite;

    /* Compte a rebours avant chaque baisse d'ecran, en secondes.
     *
     * COMMUN AUX TROIS MODES. Il n'appartenait d'abord qu'a « Travail »,
     * ou l'on suppose l'utilisateur present et concentre. Mais la gene
     * qu'il corrige -- l'ecran qui baisse au milieu d'un paragraphe -- ne
     * depend pas du mode : elle depend de ce qu'on est en train de faire.
     * Zero le supprime. */
    int       energie_preavis;

    /* L'etage « suspendre » ne s'ouvre que si CE drapeau est vrai, quels que
     * soient les delais. Il vaut FALSE par defaut : le 9 septembre 2026,
     * onze suspensions consecutives n'ont pas repris sur cette machine. */
    gboolean  energie_suspendre_permis;

    int       energie_travail_attenuer;
    int       energie_travail_eteindre;

    int       energie_auto_attenuer;
    int       energie_auto_eteindre;
    int       energie_auto_suspendre;

    int       energie_nomade_attenuer;
    int       energie_nomade_eteindre;
    int       energie_nomade_suspendre;
} ShellConfig;

/* Lit ~/.config/claude-os/shell.conf. Absent, les valeurs par defaut
 * s'appliquent : le shell doit fonctionner sans qu'aucun fichier n'ait ete
 * ecrit. Ne renvoie jamais NULL. */
ShellConfig *shell_config_load (void);

/* Un theme disponible. */
typedef struct {
    const char *id;       /* valeur ecrite dans shell.conf                    */
    const char *nom;      /* libelle montre a l'utilisateur                   */
    gboolean    sombre;
    const char *police;   /* famille par defaut du theme                      */
} ShellTheme;

/* Table des themes, terminee par un id NULL. Ajouter un theme, c'est ajouter
 * une ligne ici et un fichier style/theme-<id>.css : aucune regle de
 * shell.css n'est a toucher, elle ne connait que des noms de jetons. */
const ShellTheme *shell_themes (void);

/* Le theme actif, jamais NULL : un identifiant inconnu renvoie le premier. */
const ShellTheme *shell_theme_actif (const ShellConfig *cfg);

/* Pose le theme, ET « dark » avec lui. Rend FALSE si l'identifiant est
 * inconnu, la configuration restant alors inchangee.
 *
 * POURQUOI CE SETTER EXISTE plutot que d'ecrire cfg->theme directement :
 * « dark » se DEDUIT du theme, et les deux champs doivent bouger ensemble.
 * L'ecran de connexion posait cfg->theme seul -- la feuille de style suivait,
 * mais gtk-application-prefer-dark-theme restait a FALSE, et les widgets
 * natifs (le champ de mot de passe le premier) se dessinaient clairs sur un
 * fond sombre. Un champ ecrit a la main est un invariant qu'on oublie ;
 * une fonction, non. */
gboolean shell_config_set_theme (ShellConfig *cfg, const char *id);

/* Cette famille de police est-elle reellement installee ?
 *
 * Le panneau de reglages en a besoin : un theme qui demande une police
 * absente ne change rien a l'ecran, et sans un mot d'explication cela passe
 * pour une panne. */
gboolean shell_police_installee (const char *famille);

/* Charge le theme demande puis shell.css. Rappelable : le fournisseur du
 * theme est remplace, pas empile. */
void shell_styles_load (const char *theme);

/* Meme chose, a la signature du signal « startup » de GtkApplication : a
 * brancher avec la configuration en donnee utilisateur. */
void shell_styles_startup (GtkApplication *app, gpointer cfg);

/* Applique le theme d'icones et la police.
 *
 * A appeler AVANT de construire le moindre bouton : le repli d'icone du dock
 * interroge le theme actif, un theme pose apres coup arriverait trop tard.
 *
 * Reappelable : la regle de police est portee par un fournisseur CSS unique,
 * mis a jour plutot qu'empile. Sans cela chaque relecture ajouterait une
 * regle de plus, et l'ancienne police continuerait de peser dans la
 * cascade. */
void shell_config_apply (const ShellConfig *cfg);

/* Ecrit la configuration dans ~/.config/claude-os/shell.conf.
 *
 * Le fichier existant est relu puis modifie cle par cle : commentaires et
 * reglages inconnus de cette version sont preserves. Reecrire le fichier de
 * zero les perdrait a la premiere sauvegarde. */
gboolean shell_config_save (const ShellConfig *cfg, GError **error);

/* Libere une configuration. */
void shell_config_free (ShellConfig *cfg);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ShellConfig, shell_config_free)

/* Appelee quand shell.conf change sur le disque, avec la configuration
 * relue. Elle appartient a l'appelant, qui doit la liberer. */
typedef void (*ShellConfigChangedFunc) (ShellConfig *cfg, gpointer user_data);

/* Surveille shell.conf et previent a chaque modification.
 *
 * C'est ce qui permet au panneau de reglages d'agir sur un dock deja lance :
 * il ecrit le fichier, chaque composant le relit. Aucun protocole a
 * inventer, et la configuration reste la seule source de verite. */
void shell_config_watch (ShellConfigChangedFunc cb, gpointer user_data);
