/* =========================================================================
 * Claude-OS Shell — mise en veille progressive
 *
 * Le retroeclairage est le premier poste de consommation de la machine :
 * environ 1 a 2 W sur les 6,8 W mesures a la batterie le 8 septembre 2026.
 * Le baisser quand personne ne regarde est le plus gros levier d'autonomie
 * qui reste, et il est entierement logiciel.
 *
 * DEUX PROFILS, choisis sur la source d'alimentation :
 *
 *   secteur  « Normal »   attenuer, puis eteindre l'ecran
 *   batterie « Econome »  attenuer, eteindre, puis suspendre
 *
 * L'ETAGE « SUSPENDRE » EST DESACTIVE PAR DEFAUT -- MAIS PAS POUR LA RAISON
 * QU'ON A CRUE PENDANT CINQ JOURS.
 *
 * Ce commentaire a longtemps accuse la reprise : le 9 septembre 2026, onze
 * suspensions n'avaient « jamais repris », le journal s'arretant net sur
 * « PM: suspend entry (s2idle) ». C'ETAIT UN FAUX DIAGNOSTIC, et il a coute
 * cet etage. Mesure du 14 septembre 2026, sur cette machine :
 *
 *     07:23:20  Lid closed.
 *     07:23:42  PM: suspend entry (s2idle)
 *     07:24:00  Lid opened.  ->  PM: suspend exit
 *
 * LA REPRISE FONCTIONNE. Si le journal s'arretait sur « suspend entry », ce
 * n'est pas que la machine ne se reveillait pas : c'est qu'elle MOURAIT en
 * veille, faute de courant. Un journal tronque ne dit pas pourquoi il est
 * tronque, et l'identifiant de demarrage neuf -- seul indice retenu a
 * l'epoque -- est le meme qu'on meure de faim ou qu'on echoue a reprendre.
 *
 * La preuve en grand, le matin du 14 : 101 demarrages enregistres, dont une
 * centaine entre 05:39 et 07:23, par cycles reguliers de 64 secondes --
 * demarrer, vivre 28 secondes, se rendormir capot ferme, mourir. Une machine
 * a plat qui n'arrive pas a se recharger parce qu'elle se rendort a chaque
 * fois qu'elle revient.
 *
 * CE QUI MANQUAIT N'ETAIT DONC PAS UNE REPRISE FIABLE, MAIS UN PREAVIS ET
 * UNE PORTE DE SORTIE. Les deux existent depuis : voir batterie.h.
 *
 * ET POURTANT L'ETAGE RESTE FERME -- MAINTENANT PAR CHOIX.
 *
 * Decision de l'utilisateur, le 14 septembre 2026, la veille profonde une
 * fois eprouvee : l'ecran s'attenue et s'eteint sur l'inactivite, mais
 * L'ORDINATEUR NE S'ENDORT QUE SUR LA BATTERIE -- au seuil d'abri regle
 * dans le panneau Energie, et par hibernation.
 *
 * Le raisonnement se tient : l'inactivite de l'utilisateur ne dit rien de
 * l'activite de la MACHINE. Une compilation, un telechargement, un transfert
 * vers le NAS continuent pendant qu'on va faire autre chose, et les
 * interrompre au bout de quinze minutes serait une nuisance pour un gain
 * nul quand la prise est au mur. La charge qui s'epuise, elle, est une vraie
 * echeance -- et c'est celle-la qui endort la machine.
 *
 * Les durees « Veille de l'ordinateur apres » des modes restent donc
 * enregistrees et sans effet. Ce n'est pas un oubli : c'est ce reglage-ci
 * qui les ferme, et il se change dans shell.conf.
 *
 * AUCUNE SCRUTATION -- c'est la regle du projet, et elle vaut doublement
 * pour un module dont l'objet est d'economiser. Le compositeur previent par
 * ext-idle-notify-v1 ; entre deux evenements, ce module ne coute rien. Les
 * trois signaux qui affinent la decision (charge, son, source) sont lus une
 * seule fois, au moment ou un etage va se declencher.
 *
 * LES INHIBITEURS SONT GRATUITS. La specification du protocole impose au
 * compositeur de NE PAS rendre la notification inactive tant qu'un
 * zwp_idle_inhibitor_v1 existe sur une surface visible. Un lecteur video ne
 * sera donc jamais interrompu, sans une ligne de code ici. Verifie sur la
 * machine le 9 septembre 2026 : aucun evenement tant que Claude Desktop
 * tenait son verrou d'eveil, les trois etages a l'heure des sa liberation.
 * ========================================================================= */
#pragma once

#include <glib.h>
#include <gio/gio.h>     /* GIcon : l'icone des modes */
#include "config.h"

/* -------------------------------------------------------------------------
 * Les trois modes, en un seul endroit
 *
 * LE MODE EST UN CHOIX, PAS UNE DEDUCTION. La version precedente derivait
 * le comportement de la prise ; personne ne savait dire ce que la machine
 * allait faire sans regarder le cable.
 *
 * La table est LA definition des modes. La Console y prend ses libelles, le
 * panneau de reglages ses titres, le module ses etages. Un mode ne peut donc
 * pas etre dénaturé depuis l'interface : on regle des durees, jamais ce
 * qu'un mode est.
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *id;          /* ce qui s'ecrit dans shell.conf              */
    const char *nom;         /* « Travail »                                 */
    const char *icone;
    const char *resume;      /* une phrase, montree sous les boutons        */
    gboolean    veille_ordi; /* le mode autorise-t-il l'etage « suspendre » */
} ShellModeEnergie;

/* Table terminee par un id NULL. */
const ShellModeEnergie *shell_energie_modes (void);

/* L'icone d'un mode, a poser par gtk_image_set_from_gicon(). A liberer. Voir
 * modes-energie.c : pour ces trois-la, le dessin d'Adwaita est prefere a
 * celui du theme en vigueur. */
GIcon *shell_energie_mode_icone (const ShellModeEnergie *m);

/* Le mode en vigueur, jamais NULL : un identifiant inconnu -- fichier d'une
 * version anterieure -- renvoie « automatique ». */
const ShellModeEnergie *shell_energie_mode_actif (const ShellConfig *cfg);

/* Les delais du mode en vigueur, en secondes. Zero ferme l'etage. Sert au
 * resume affiche par la Console et par les Reglages : ceux-ci ne
 * reimplementent pas le choix des durees, ils le demandent. */
void shell_energie_delais (const ShellConfig *cfg,
                           int *preavis, int *attenuer,
                           int *eteindre, int *suspendre);

/* Les delais d'un mode DONNE, pas forcement celui en vigueur. La Console en
 * a besoin pour decrire chaque bouton avant qu'on l'ait choisi : une
 * infobulle qui annoncerait les durees du mode courant sur les trois
 * boutons serait pire que pas d'infobulle du tout. */
void shell_energie_delais_mode (const ShellConfig *cfg,
                                const ShellModeEnergie *mode,
                                int *preavis, int *attenuer,
                                int *eteindre, int *suspendre);

/* A appeler une fois la fenetre presentee : la connexion Wayland de GTK doit
 * deja exister. Sans compositeur compatible, ou sans retroeclairage
 * pilotable, le module s'efface et la barre continue normalement. */
void shell_energie_init (const ShellConfig *cfg);

/* Relit les delais et reconstruit les minuteries. A appeler depuis le rappel
 * de shell_config_watch : le panneau de reglages ecrit shell.conf, chaque
 * composant relit, et la veille suit sans qu'on ait rien a redemarrer. */
void shell_energie_reconfigurer (const ShellConfig *cfg);

/* Verrouille l'ecran tout de suite, sans fermer la session : le bouton
 * « Verrouiller » de la Console. Passe par le meme lanceur que l'etage de
 * veille, qui sait qu'un verrou est deja en place et n'en lance pas un
 * second. */
void shell_energie_verrouiller (void);
