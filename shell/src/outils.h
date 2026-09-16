/* =========================================================================
 * Claude-OS Shell — LE CONTRAT DES OUTILS
 *
 * CE FICHIER EST LE DOCUMENT DE RÉFÉRENCE. Une application de cette
 * distribution ne dessine pas son chrome : elle DÉCLARE ce qu'elle sait
 * faire et où l'on peut aller, et le bureau le dessine pour elle. Tout ce
 * qu'il faut savoir pour écrire une application qui s'intègre au bureau est
 * ici et dans docs/14 ; rien d'autre n'est à deviner.
 *
 * POURQUOI
 *
 * Chaque application qui s'ajoute apporte sa propre barre d'outils, son
 * propre volet latéral, ses propres boutons — et le bureau devient une
 * collection de dialectes. En rassemblant tout cela dans le dock, on gagne
 * trois choses qu'aucune convention de style n'obtient :
 *
 *   - UN SEUL ENDROIT OÙ REGARDER. Les outils de l'application active sont
 *     toujours au même endroit de l'écran, quelle que soit l'application.
 *   - UNE SEULE SOURCE DE VÉRITÉ. Le menu contextuel, la barre du dock et
 *     le clavier lisent le même modèle. Un état à synchroniser de moins,
 *     comme le trieur unique des quatre vues de Fichiers.
 *   - DE LA PLACE. Une fenêtre qui ne porte plus ni barre ni volet rend
 *     150 px de haut et 200 de large à son contenu, sur une dalle qui n'en
 *     a que 1920 × 1080.
 *
 * Le prix est nommé, et il est réel : une fenêtre nue pilotée au clic droit
 * est admirable pour qui sait, opaque pour qui ne sait pas. C'est un choix
 * assumé pour cette distribution, pas une bonne pratique générale.
 *
 * -------------------------------------------------------------------------
 * LES EMPLACEMENTS — CE QUI EXISTE, ET OÙ
 * -------------------------------------------------------------------------
 *
 * Le dock a deux faces, et un flip les échange (voir retourneur.h).
 *
 *   Face BUREAU, celle d'aujourd'hui :
 *
 *      ╭───────────────────────────────────────╮
 *      │  ⊞ │ ▣ ▣ ▣ ▣ ▣ │ ▣ ▣                  │
 *      ╰───────────────────────────────────────╯
 *        lanceur, épinglées, ouvertes
 *
 *   Face ÉTABLI, celle que ce contrat remplit :
 *
 *                    ┌─────────────────────┐
 *                    │       AUVENT        │   ← se déploie vers le haut
 *      ╭─────────────┴─────────────────────┴───────────────────╮
 *      │ 🏠 ★ 💾 ☁ │ 🗺 🔍 │ ▣ ▣ ▣ │ ⤺ │
 *      ╰───────────────────────────────────────────────────────╯
 *          LIEUX      OUTILS    apps   retour
 *
 * QUATRE EMPLACEMENTS SONT OFFERTS AUX APPLICATIONS, et deux sont réservés
 * au bureau. Une application ne peut rien poser ailleurs, et c'est ce qui
 * garantit que deux applications se ressemblent.
 *
 *   « lieux »   — Où l'on peut aller. Dossiers, favoris, lecteurs, sources,
 *                 onglets, projets : ce qui se choisit et où l'on revient.
 *                 Dessiné en icônes avec libellé, l'entrée courante allumée.
 *   « outils »  — Ce qu'on déclenche. Boutons à icône.
 *   « auvent »  — Ce qui demande de la place : une saisie, une liste, un
 *                 réglage. Un volet qui monte au-dessus de la pilule, ouvert
 *                 par un bouton de la zone « outils », refermé au geste
 *                 suivant. C'est là que va tout ce qui ne tient pas sur une
 *                 ligne de 72 px.
 *
 *   Réservés au bureau, non déclarables : la bande des applications
 *   ouvertes, puis le bouton de retour au bureau, tous deux À DROITE.
 *   L'ordre suit celui de la face lanceur, où les applications ouvertes sont
 *   déjà à droite : rien de ce qui est commun aux deux faces ne doit bouger
 *   au retournement.
 *
 * L'ÉTABLI NE PORTE QUE DES BOUTONS, et c'est une règle, pas un goût.
 *
 * Il y avait une cinquième zone, « fil », pour un chemin d'Ariane : une
 * suite de mots séparés de chevrons. À l'usage, elle cassait le rythme de la
 * rangée — l'œil ne savait plus ce qui se clique et ce qui se lit. Elle a été
 * retirée le 16 septembre 2026, et le contrat est passé en version 2.
 *
 * CE QUI SE LIT VA AILLEURS. Un chemin, une position, un état : dans le
 * titre de la fenêtre, dans sa barre d'état, dans son contenu. Ce qui doit
 * rester atteignable depuis le dock devient un BOUTON qui déploie un auvent
 * — voir « liste » dans auvent.h. Fichiers en donne l'exemple : un bouton
 * « chemin », et les étapes dans le volet.
 *
 * CE QUI NE VA PAS DANS LA BARRE. Les actions d'édition — copier, coller,
 * renommer, supprimer, trier, changer de vue — restent au menu contextuel
 * et au clavier. La barre porte la NAVIGATION et les PORTES ; le clic droit
 * porte les VERBES. Cette séparation est une décision de l'utilisateur, le
 * 16 septembre 2026, et pas une limite technique : rien n'empêcherait d'y
 * mettre des verbes, et c'est précisément pour cela qu'il faut l'écrire.
 *
 * -------------------------------------------------------------------------
 * LE TRANSPORT — RIEN D'INVENTÉ
 * -------------------------------------------------------------------------
 *
 * org.gtk.Menus et org.gtk.Actions, les deux interfaces que GTK exporte et
 * importe nativement. Le dock parle déjà org.gtk.Actions à la barre d'état
 * depuis le 11 septembre 2026 : le canal est éprouvé, et une application
 * qui n'est pas en GTK peut les implémenter, elles sont spécifiées.
 *
 * TOUT VIT AU MÊME CHEMIN, sur le nom de bus de l'application :
 *
 *   /os/claude/shell/outils
 *      ├── os.claude.shell.Outils   la découverte (ce fichier)
 *      ├── org.gtk.Menus            le modèle de la barre
 *      └── org.gtk.Actions          ce que la barre déclenche
 *
 * Un chemin FIXE, et non dérivé de l'identifiant de l'application : une
 * application non-GTK n'a pas à reproduire la règle de dérivation de
 * GApplication pour se faire entendre.
 *
 * -------------------------------------------------------------------------
 * LA PRÉSENTATION — QUI PARLE LE PREMIER, ET POURQUOI
 * -------------------------------------------------------------------------
 *
 * L'APPLICATION SE PRÉSENTE AU DOCK. Elle ne l'attend pas, et le dock ne la
 * cherche pas.
 *
 * L'autre voie a été écartée : le dock ne découvre une fenêtre que par
 * wlr-foreign-toplevel-management-v1, donc au moment où elle s'active. Une
 * application ne saurait alors qu'au premier clic si ses outils sont pris
 * en charge — et son volet de repli apparaîtrait puis disparaîtrait sous
 * les yeux de l'utilisateur. Se présenter à la publication ferme ce trou.
 *
 *   1. L'application exporte ses trois interfaces au chemin ci-dessus.
 *   2. Elle surveille le nom « os.claude.shell.dock ».
 *        absent  → sur_prise(FALSE) : elle montre ses replis.
 *        présent → elle se présente, en activant l'action « outils-
 *                  presenter » du dock avec son propre nom de bus.
 *   3. Le dock lit « Contrat », importe le menu et les actions, puis appelle
 *      Prise(TRUE) — ou Prise(FALSE) s'il ne sait pas lire ce contrat-là.
 *   4. Le dock disparaît → sur_prise(FALSE), et les replis reviennent.
 *
 * AUCUNE MINUTERIE, AUCUNE SCRUTATION. Tout part d'un événement : un nom
 * qui apparaît sur le bus, un appel de méthode. C'est la discipline du
 * projet, et ici elle tombe juste : l'absence du dock se LIT sur le bus,
 * elle ne se déduit pas d'un délai écoulé.
 *
 * LE REPLI N'EST PAS FACULTATIF. Une application dont les outils vivent
 * dans un autre processus dépend de ce processus ; elle doit rester
 * utilisable sans lui — lancée seule depuis un terminal, au banc d'essai,
 * ou le jour où le dock tombe. Toute application de cette distribution
 * garde donc son volet interne, escamoté tant que Prise(TRUE) tient.
 *
 * -------------------------------------------------------------------------
 * LE MODÈLE — COMMENT DÉCRIRE SA BARRE
 * -------------------------------------------------------------------------
 *
 * Un GMenuModel, dont CHAQUE SECTION porte l'emplacement qu'elle vise et
 * chaque entrée la forme qu'elle prend :
 *
 *   GMenu *barre = g_menu_new ();
 *
 *   GMenu *lieux = g_menu_new ();
 *   GMenuItem *it = g_menu_item_new ("Images", "outils.aller");
 *   g_menu_item_set_attribute (it, SHELL_OUTILS_A_FORME, "s", SHELL_OUTILS_LIEU);
 *   g_menu_item_set_attribute (it, "target", "s", "file:///home/stef/Images");
 *   g_menu_item_set_attribute (it, "icon",   "s", "folder-pictures-symbolic");
 *   g_menu_append_item (lieux, it);
 *
 *   GMenuItem *sec = g_menu_item_new_section ("Personnel", G_MENU_MODEL (lieux));
 *   g_menu_item_set_attribute (sec, SHELL_OUTILS_A_ZONE, "s", SHELL_OUTILS_ZONE_LIEUX);
 *   g_menu_append_item (barre, sec);
 *
 * L'ENTRÉE COURANTE SE DIT PAR L'ÉTAT DE L'ACTION, pas par un attribut.
 * « outils.aller » est une action À ÉTAT dont l'état est l'URI du dossier
 * où l'on se trouve ; le dock allume l'entrée dont la cible vaut cet état,
 * exactement comme GMenu allume un bouton radio. Une application qui
 * inventerait un attribut « courant » aurait deux vérités à tenir d'accord.
 *
 * LE MODÈLE PEUT CHANGER À TOUT MOMENT. org.gtk.Menus signale ses propres
 * changements : il suffit de modifier le GMenu, le dock suit. C'est ainsi
 * que le fil d'Ariane se met à jour à chaque navigation, sans un appel.
 *
 * L'AUVENT, en v1, ne connaît qu'un contrôle : la saisie.
 *
 *   GMenuItem *it = g_menu_item_new ("Rechercher", "outils.chercher");
 *   g_menu_item_set_attribute (it, SHELL_OUTILS_A_FORME,    "s", SHELL_OUTILS_AUVENT);
 *   g_menu_item_set_attribute (it, SHELL_OUTILS_A_CONTROLE, "s", SHELL_OUTILS_SAISIE);
 *   g_menu_item_set_attribute (it, SHELL_OUTILS_A_INVITE,   "s", "Nom du fichier…");
 *   g_menu_item_set_attribute (it, "icon", "s", "system-search-symbolic");
 *
 * Le bouton paraît dans la zone « outils » ; au clic, l'auvent monte avec
 * un champ, et chaque frappe active « outils.chercher » avec le texte (une
 * chaîne). Fermer l'auvent l'active une dernière fois avec la chaîne vide.
 *
 * C'EST LE DOCK QUI DESSINE LE CONTRÔLE, ET L'APPLICATION N'EN VOIT QUE LA
 * VALEUR. Un vocabulaire fermé, et non un langage de description
 * d'interface : la cohérence visuelle est alors garantie par construction,
 * et une application ne PEUT PAS dessiner dans une surface qui appartient à
 * un autre processus. Les contrôles « liste » et « choix » sont prévus et
 * ne sont pas écrits ; les déclarer aujourd'hui ne produirait rien, et le
 * dock le dira sur sa sortie d'erreur plutôt que d'afficher un trou.
 *
 * TROIS FAÇONS DE REFERMER L'AUVENT, et il en faut trois : tant qu'il est
 * ouvert, il tient le clavier de la session (voir auvent.h). Échap, un clic
 * n'importe où ailleurs, et le passage à une autre application.
 *
 * -------------------------------------------------------------------------
 * ÉPROUVER SANS LE DOCK
 * -------------------------------------------------------------------------
 *
 *   claude-os-outils os.claude.shell.fichiers
 *
 * lit la barre publiée, l'imprime telle qu'elle est, et sait jouer le rôle
 * du dock — Prise comprise. C'est par lui que ce contrat a été éprouvé
 * avant qu'une seule ligne d'interface ne soit écrite.
 * ========================================================================= */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* -------------------------------------------------------------------------
 * Le contrat, en constantes
 *
 * Nommées plutôt qu'écrites en clair pour que « grep SHELL_OUTILS_ » donne
 * la liste complète de ce qui circule entre les deux processus.
 * ------------------------------------------------------------------------- */

/* Version du contrat. Le dock refuse ce qu'il ne sait pas lire, et le dit ;
 * il ne fait jamais semblant. À incrémenter dès qu'une application écrite
 * pour l'ancien contrat cesserait d'être comprise. */
/* 2 depuis le 16 septembre 2026 : la zone « fil » a disparu, et avec elle
 * la forme « etape ». Une application écrite pour le contrat 1 déclarerait
 * une zone que le dock ne connaît plus -- son contenu tomberait dans les
 * outils, en boutons de texte, et l'on retrouverait précisément le défaut
 * qui a motivé le changement. Le dock refuse donc, et le dit. */
#define SHELL_OUTILS_CONTRAT   2u

#define SHELL_OUTILS_CHEMIN    "/os/claude/shell/outils"
#define SHELL_OUTILS_IFACE     "os.claude.shell.Outils"
#define SHELL_OUTILS_DOCK      "os.claude.shell.dock"

/* L'action du dock par laquelle une application se présente, et le préfixe
 * sous lequel le dock insère les actions importées : le modèle nomme donc
 * ses actions « outils.quelquechose », et le groupe exporté les porte sans
 * préfixe. */
#define SHELL_OUTILS_PRESENTER "outils-presenter"
#define SHELL_OUTILS_PREFIXE   "outils"

/* Les emplacements, portés par l'attribut de section. */
#define SHELL_OUTILS_A_ZONE      "x-claude-zone"
#define SHELL_OUTILS_ZONE_LIEUX  "lieux"
#define SHELL_OUTILS_ZONE_OUTILS "outils"
#define SHELL_OUTILS_ZONE_AUVENT "auvent"

/* Les formes, portées par l'attribut d'entrée. Une entrée sans forme est un
 * bouton : c'est le cas le plus courant, il ne se déclare pas. */
#define SHELL_OUTILS_A_FORME     "x-claude-forme"
#define SHELL_OUTILS_LIEU        "lieu"
#define SHELL_OUTILS_BOUTON      "bouton"
#define SHELL_OUTILS_AUVENT      "auvent"

/* Le contrôle que l'auvent déploie. « saisie » seul est écrit à ce jour. */
/* Le contrôle que l'auvent déploie.
 *
 *   « saisie »  un champ ; chaque frappe part vers l'action de l'entrée.
 *   « liste »   un menu vertical ; CHAQUE LIGNE porte sa propre action et sa
 *               cible, déclarées dans un SOUS-MENU attaché à l'entrée
 *               d'auvent (G_MENU_LINK_SUBMENU). C'est par là que passe tout
 *               ce qui se lisait autrefois dans la rangée.
 */
#define SHELL_OUTILS_A_CONTROLE  "x-claude-controle"
#define SHELL_OUTILS_SAISIE      "saisie"  /* écrit le 16 septembre 2026 */
#define SHELL_OUTILS_LISTE       "liste"   /* écrit le 16 septembre 2026 */
#define SHELL_OUTILS_CHOIX       "choix"   /* prévu, non écrit */

/* Le texte d'invite d'une saisie, l'infobulle d'un bouton, et le raccourci
 * clavier à MONTRER — montrer seulement : c'est l'application qui l'arme,
 * le dock n'intercepte aucune touche. */
#define SHELL_OUTILS_A_INVITE    "x-claude-invite"
#define SHELL_OUTILS_A_ASTUCE    "x-claude-astuce"
#define SHELL_OUTILS_A_CLE       "x-claude-cle"

/* -------------------------------------------------------------------------
 * Publier sa barre
 * ------------------------------------------------------------------------- */

typedef struct _ShellOutils ShellOutils;

/* Le dock prend-il les outils en charge ?
 *
 * L'ÉTAT DE DÉPART EST FALSE, et il ne se rappelle pas : une application
 * s'ouvre avec ses replis en place, et les escamote si le rappel arrive
 * avec TRUE. Appelée aux seuls CHANGEMENTS, jamais pour rien — le dock
 * absent ne produit donc aucun appel, ce qui est déjà la bonne réponse. */
typedef void (*ShellOutilsPriseFunc) (gboolean prise, gpointer data);

/* Exporte les trois interfaces, puis se présente au dock.
 *
 * `app`     l'application, pour sa connexion au bus. Elle doit être
 *           enregistrée (g_application_register) : c'est le cas dès
 *           « activate », d'où l'on appelle normalement cette fonction.
 * `titre`   le nom à montrer, si le dock a un jour à nommer la barre.
 * `actions` ce que la barre déclenche. Le MÊME groupe que celui du menu
 *           contextuel : c'est tout l'intérêt, une seule source de vérité.
 * `barre`   le modèle décrit plus haut. Modifiable à tout moment ensuite.
 *
 * Rend NULL et se plaint sur la sortie d'erreur s'il n'y a pas de bus de
 * session — l'application doit alors se comporter comme si le dock était
 * absent. Ne rend jamais NULL parce que le dock manque : c'est un état
 * ordinaire, rapporté par `sur_prise`. */
ShellOutils *shell_outils_publier (GApplication         *app,
                                   const char           *titre,
                                   GActionGroup         *actions,
                                   GMenuModel           *barre,
                                   ShellOutilsPriseFunc  sur_prise,
                                   gpointer              data);

/* Retire les exports et cesse de surveiller le dock. À appeler à la
 * fermeture ; sans cela le dock garderait une barre dont le propriétaire
 * est parti — il s'en aperçoit, mais après un aller-retour inutile. */
void shell_outils_retirer (ShellOutils *o);

/* L'état courant, pour qui préfère le demander que le retenir. */
gboolean shell_outils_prise (ShellOutils *o);

/* Demander au dock d'ouvrir l'auvent d'une de nos entrées, désignée par le
 * nom NU de son action (« chercher », et non « outils.chercher »).
 *
 * POURQUOI UNE APPLICATION EN A BESOIN. Elle arme ses propres raccourcis --
 * le dock n'intercepte aucune touche -- et Ctrl+F doit ouvrir la recherche
 * où qu'elle soit : dans la fenêtre tant qu'elle porte son chrome, dans
 * l'auvent dès que le dock tient la barre. L'application ne sait pas
 * dessiner l'auvent ; elle demande, et le contrat s'occupe du reste.
 *
 * Sans effet si le dock n'a pas pris la barre, ou s'il ne trouve pas
 * d'entrée pour cette action — il le dit sur sa sortie d'erreur. */
void shell_outils_auvent (ShellOutils *o, const char *action);

G_END_DECLS
