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

    /* Couleur de contraste -- ce qui colore un bouton actif, une selection,
     * le point sous une application ouverte. Vide : celle du theme.
     *
     * ELLE NE VIT PAS DANS LE THEME, et c'est le but : on veut pouvoir
     * garder les surfaces d'un theme en changeant ce qui les souligne.
     * Voir shell_accents (). */
    char     *accent;

    /* L'OMBRE PORTEE DU COIN, en quatre nombres.
     *
     * POURQUOI ELLE SE REGLE. Le coin ecrit en blanc sur ce qui se trouve
     * dessous -- fond d'ecran, page web blanche, video sombre. Il n'existe
     * pas de reglage d'ombre juste pour ces trois cas a la fois : le bon
     * equilibre se trouve a l'oeil, sur SON fond d'ecran, et il se trouve
     * en quelques essais. Recompiler entre chaque essai est le plus sur
     * moyen de s'arreter au premier « ca ira ».
     *
     * Ce sont donc quatre cles de shell.conf, relues a chaud comme tout le
     * reste : on ecrit, on enregistre, le coin change sous les yeux.
     *
     * CE N'EST PAS DANS LE PANNEAU DE REGLAGES, ET C'EST DELIBERE -- voir
     * le commentaire de tete de settings.c : on y regle des habitudes, pas
     * des details d'implementation. « Rayon de diffusion de l'ombre » n'est
     * pas un choix d'utilisateur, c'est un choix de dessin, qu'on fait une
     * fois. Une fois trouve, il devient le defaut ci-dessous et personne
     * n'a plus a y toucher.
     *
     * DEUX OMBRES, comme celles du dock : une courte et dense decalee vers
     * le bas, qui donne le contour ; une large sans decalage, qui pose le
     * halo. L'une sans l'autre donne soit un lisere dur, soit un flou qui
     * ne detache rien. */
    int       ombre_opacite;    /* pourcent, les deux ombres a la fois     */
    int       ombre_flou;       /* rayon du halo, en pixels                */
    int       ombre_contour;    /* rayon de l'ombre courte, en pixels      */
    int       ombre_decalage;   /* descente de l'ombre courte, en pixels   */

    /* Transparence des surfaces du bureau -- dock, barre d'etat, Console,
     * lanceur, fenetres du systeme.
     *
     * FAUX PAR DEFAUT, et pas par prudence d'affichage : une surface
     * translucide interdit au compositeur de la poser sans melange, et se
     * paie en remplissage GPU donc en watts. Sur une machine qui consomme
     * 6,8 W au repos, c'est un choix, pas un reglage de confort gratuit.
     * Le raisonnement complet est en tete de style/verre.css. */
    gboolean  transparence;

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

    /* Opacite des avis systeme, en pourcent -- le compte a rebours ET les
     * messages courts, qui partagent tout (voir avis.h). Se regle parce que
     * le bon equilibre depend du fond d'ecran et de la vue de chacun : trop
     * discret il ne previent pas, trop marque il occupe le milieu de
     * l'ecran. Un curseur coute moins cher qu'un debat.
     *
     * Elle compte plus qu'avant : les avis sont blancs et au centre, la ou
     * une fenetre claire se trouve souvent -- c'est ce reglage, et lui seul,
     * qui rattrape un avis qu'on ne distingue pas. */
    int       energie_opacite;

    /* Compte a rebours avant chaque baisse d'ecran, en secondes.
     *
     * COMMUN AUX TROIS MODES. Il n'appartenait d'abord qu'a « Travail »,
     * ou l'on suppose l'utilisateur present et concentre. Mais la gene
     * qu'il corrige -- l'ecran qui baisse au milieu d'un paragraphe -- ne
     * depend pas du mode : elle depend de ce qu'on est en train de faire.
     * Zero le supprime. */
    int       energie_preavis;

    /* Verrouillage de l'ecran. Le delai se compte DEPUIS L'EXTINCTION, pas
     * depuis le debut de l'inactivite : c'est un sursis. On revient dans la
     * minute, un geste rend la main sans rien taper ; au-dela, le code PIN.
     *
     * Sans etage « eteindre », pas de verrouillage : le verrou s'ancre sur
     * lui, et un ecran qui ne s'eteint jamais n'a pas de reveil. */
    gboolean  energie_verrou;
    int       energie_verrou_delai;

    /* L'etage « suspendre » ne s'ouvre que si CE drapeau est vrai, quels que
     * soient les delais.
     *
     * Il valait FALSE parce qu'on croyait la reprise cassee. Elle ne l'est
     * pas : mesure du 14 septembre 2026, capot ouvert a 07:24:00, « PM:
     * suspend exit » dans la foulee.
     *
     * Il reste FALSE, mais PAR CHOIX depuis ce jour-la : l'ordinateur ne
     * s'endort que sur la batterie, au seuil d'abri, et non sur
     * l'inactivite -- qui ne dit rien de ce que la machine est en train de
     * faire. Le raisonnement complet est en tete de energie.h. */
    gboolean  energie_suspendre_permis;

    /* Surveillance de la charge -- voir batterie.h pour le raisonnement.
     *
     * Trois seuils en POURCENT, du plus haut au plus bas : prevenir,
     * insister, se mettre a l'abri. Les deux premiers parlent, le dernier
     * agit. Ils se reglent parce que ce sont des habitudes de travail et non
     * des constantes : qui reste pres d'une prise veut qu'on le laisse
     * tranquille jusqu'au bout, qui travaille en deplacement veut etre
     * prevenu tot.
     *
     * « abri_action » dit ce que la machine fait au dernier seuil ; la table
     * des valeurs possibles vit dans abris-batterie.c, et fait foi. */
    /* Ce que la fermeture du capot declenche -- voir capot.h. Le shell
     * prend la main sur logind pour que ce choix vive ici, avec les autres,
     * plutot que dans /etc. La table des valeurs possibles est dans
     * actions-capot.c, et fait foi. */
    char     *energie_capot_action;

    int       energie_bat_prevenir;
    int       energie_bat_insister;
    int       energie_bat_abri;
    char     *energie_bat_abri_action;

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

/* Une couleur de contraste disponible. */
typedef struct {
    const char *id;    /* valeur ecrite dans shell.conf, et nom du fichier  */
    const char *nom;   /* libelle montre a l'utilisateur                    */
} ShellAccent;

/* Table des couleurs de contraste, terminee par un id NULL. La premiere
 * ligne porte un id vide : c'est « celle du theme », l'absence de choix.
 *
 * MEME MECANIQUE QUE LES THEMES, a dessein : ajouter une couleur, c'est
 * ajouter une ligne ici et un fichier style/accent-<id>.css. Aucune regle
 * de shell.css n'est a toucher -- elle ne connait que @accent, @accent-hover,
 * @accent-press et @on-accent, et le fichier ne fait que les redefinir
 * par-dessus le theme. */
const ShellAccent *shell_accents (void);

/* La couleur active, jamais NULL : un identifiant inconnu renvoie la
 * premiere ligne, c'est-a-dire celle du theme. */
const ShellAccent *shell_accent_actif (const ShellConfig *cfg);

/* Cette famille de police est-elle reellement installee ?
 *
 * Le panneau de reglages en a besoin : un theme qui demande une police
 * absente ne change rien a l'ecran, et sans un mot d'explication cela passe
 * pour une panne. */
gboolean shell_police_installee (const char *famille);

/* Charge le theme, la couleur de contraste, le verre, puis shell.css.
 * Rappelable : les fournisseurs sont remplaces, pas empiles.
 *
 * ELLE PREND LA CONFIGURATION ENTIERE, ET NON LE SEUL NOM DU THEME.
 *
 * Elle ne prenait qu'un « const char *theme », et c'etait une invitation a
 * la meme faute que celle documentee au-dessus de shell_config_set_theme :
 * trois reglages decident desormais de l'aspect -- le theme, l'accent, la
 * transparence -- et un appelant qui n'en passe qu'un laisse les deux
 * autres a leur valeur precedente sans qu'aucun compilateur ne s'en
 * plaigne. La signature les tient donc ensemble. */
void shell_styles_load (const ShellConfig *cfg);

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
