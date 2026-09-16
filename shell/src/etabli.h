/* =========================================================================
 * Claude-OS Shell — l'établi : la face outils du dock
 *
 * L'autre bout du contrat des outils (outils.h). Une application déclare sa
 * barre ; l'établi la dessine, dans la matière du bureau.
 *
 * C'EST LUI QUI DESSINE, ET L'APPLICATION NE VOIT QUE LES ACTIONS. Le
 * modèle reçu ne décrit pas une interface, il décrit une INTENTION : des
 * lieux où aller, un chemin où l'on est, des outils à déclencher. L'établi
 * choisit la forme. C'est ce qui garantit que deux applications se
 * ressemblent sans qu'aucune convention de style n'ait à être respectée.
 *
 * -------------------------------------------------------------------------
 * CINQ ZONES, DONT DEUX QUI N'APPARTIENNENT PAS AUX APPLICATIONS
 * -------------------------------------------------------------------------
 *
 *   ╭─────────────────────────────────────────────────────────────╮
 *   │ ▣ ▣ ▣ │ 🏠 ★ 💾 ☁ │ Accueil › Images › 2026 │ 🔍 │ ⤺ │
 *   ╰─────────────────────────────────────────────────────────────╯
 *     apps      lieux            fil               outils  retour
 *
 * « apps » et « retour » sont au bureau : une application ne peut pas y
 * poser quoi que ce soit, et c'est ce qui fait qu'on retrouve toujours le
 * chemin du retour, quelle que soit l'application au premier plan. Le dock
 * fournit le widget des applications ouvertes -- il sait déjà les
 * construire -- et l'établi fournit le reste.
 *
 * L'AUVENT est la cinquième zone, et la seule qui ne soit pas sur la ligne :
 * un volet qui monte au-dessus de la pilule, ouvert par un bouton de la zone
 * « outils ». Voir auvent.h -- notamment pour le clavier, que le dock doit
 * prendre le temps qu'il est ouvert.
 *
 * -------------------------------------------------------------------------
 * CE QUI SE RECONSTRUIT, ET CE QUI NE FAIT QUE S'ALLUMER
 * -------------------------------------------------------------------------
 *
 * Le modèle change souvent : un fil d'Ariane bouge à chaque navigation. Mais
 * l'entrée COURANTE change encore plus souvent, et elle ne vaut pas une
 * reconstruction -- d'autant qu'une reconstruction change la largeur, donc
 * la taille de la surface, donc la position des popovers de labwc.
 *
 * Deux chemins, donc :
 *
 *   - le modèle change      → on reconstruit, et `sur_taille` prévient le
 *                             dock, à qui il revient de fermer ses surfaces ;
 *   - un état d'action change → on ne fait qu'allumer et éteindre des
 *                             classes CSS. Rien ne bouge, rien ne se
 *                             réaligne, aucun popover ne se déplace.
 *
 * L'entrée courante se lit dans l'ÉTAT DE SON ACTION, comparé à sa cible --
 * la sémantique radio de GMenu. Voir outils.h : une application qui
 * inventerait un attribut « courant » aurait deux vérités à tenir d'accord.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SHELL_TYPE_ETABLI (shell_etabli_get_type ())
G_DECLARE_FINAL_TYPE (ShellEtabli, shell_etabli, SHELL, ETABLI, GtkWidget)

/* Le bouton de retour au bureau a été pressé. */
typedef void (*ShellEtabliRetour) (gpointer user_data);

/* L'auvent de l'établi s'ouvre ou se ferme. LE DOCK Y POSE LE MODE CLAVIER
 * DE SA SURFACE : une saisie qui ne reçoit pas les touches n'est qu'un
 * rectangle. Voir auvent.h, « le clavier, et c'est le vrai sujet ». */
typedef void (*ShellEtabliAuvent) (gboolean ouvert, gpointer user_data);

/* L'établi vient de changer d'encombrement -- une barre posée, retirée, ou
 * un modèle qui a bougé. LE DOCK Y FERME SES SURFACES : la largeur de la
 * face la plus large commande celle de la fenêtre, et labwc replace un
 * popover ouvert depuis l'ancienne origine de la sienne. */
typedef void (*ShellEtabliTaille) (gpointer user_data);

/* `apps` est le widget des applications ouvertes, construit par le dock. */
GtkWidget *shell_etabli_new (GtkWidget *apps);

void shell_etabli_sur_retour (ShellEtabli *e, ShellEtabliRetour f, gpointer data);
void shell_etabli_sur_auvent (ShellEtabli *e, ShellEtabliAuvent f, gpointer data);

/* Refermer l'auvent, s'il est ouvert. Le dock s'en sert avant de se
 * retourner et quand il change d'application : un volet de saisie laissé
 * ouvert sur la barre d'une application qu'on vient de quitter écrirait
 * dans le vide. */
void shell_etabli_fermer_auvent (ShellEtabli *e);
gboolean shell_etabli_auvent_ouvert (ShellEtabli *e);

/* Ouvrir l'auvent d'une entrée, désignée par le nom nu de son action.
 * C'est ce que demande une application qui arme son propre raccourci --
 * voir shell_outils_auvent(). Rend FALSE si aucune entrée ne correspond. */
gboolean shell_etabli_ouvrir_auvent (ShellEtabli *e, const char *action);
void shell_etabli_sur_taille (ShellEtabli *e, ShellEtabliTaille f, gpointer data);

/* Poser la barre d'une application, ou la retirer en passant NULL.
 *
 * `barre` et `actions` viennent du bus (g_dbus_menu_model_get,
 * g_dbus_action_group_get) et se remplissent APRÈS coup : l'établi suit
 * « items-changed » et se reconstruit quand le contenu arrive. Ne pas
 * s'étonner qu'il soit vide dans l'instant qui suit l'appel. */
void shell_etabli_poser (ShellEtabli *e, GMenuModel *barre, GActionGroup *actions);

/* Y a-t-il quelque chose à montrer ? Le dock s'en sert pour décider s'il
 * vaut la peine de se retourner : un établi vide n'est pas une face, c'est
 * un dock amputé de ses icônes. */
gboolean shell_etabli_garni (ShellEtabli *e);

/* Actionner le n-ième outil de la zone « outils », comme si on l'avait
 * cliqué. Rend FALSE s'il n'y en a pas tant.
 *
 * DEUX USAGES, ET LE SECOND JUSTIFIE LE PREMIER. Le banc d'essai n'a pas
 * d'yeux : il ne sait pas où le compositeur a posé un bouton, et un clic à
 * coordonnées devinées éprouverait surtout notre capacité à deviner. Mais
 * c'est aussi le point d'accroche d'un raccourci clavier -- « la recherche
 * du dock » sans quitter le clavier -- le jour où l'on en voudra un. */
gboolean shell_etabli_actionner_outil (ShellEtabli *e, int n);

G_END_DECLS
