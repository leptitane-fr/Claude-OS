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
 * L'ETAGE « SUSPENDRE » EST DESACTIVE PAR DEFAUT -- ET LA RAISON A CHANGE
 * DEUX FOIS. Lire ce qui suit en entier avant d'y toucher : deux diagnostics
 * successifs se sont contredits, et le troisieme est mesure.
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
 * 16 SEPTEMBRE 2026 : LE DEMENTI ETAIT FAUX A SON TOUR, ET LE 9 AVAIT RAISON.
 *
 * Ce qui precede a tenu deux jours. La reprise NE FONCTIONNAIT PAS -- pas en
 * s2idle. Mesure du 16 septembre : 98 tentatives de veille, 98 gels, le
 * journal s'arretant chaque fois sur « PM: suspend entry (s2idle) », batterie
 * entre 65 et 78 %. Pas une seule reprise. La machine n'etait pas a plat.
 *
 * Les cycles de 64 secondes ne sont pas la batterie : c'est le CHIEN DE GARDE
 * DE L'EC, qui l'ecrit lui-meme dans le journal du firmware --
 * /sys/firmware/log, « ap hang detected », 117 fois. L'EC constate que le
 * processeur ne repond plus, patiente, et le reinitialise. Le cycle regulier
 * n'etait donc pas une machine qui meurt de faim, mais un chien de garde qui
 * fait son travail, a intervalle fixe.
 *
 * La mesure du 14 -- capot ferme a 07:23:42, rouvert a 07:24:00 -- etait une
 * veille de DIX-HUIT SECONDES. Elle est vraie, et elle ne generalise pas : un
 * cas contre 98. C'est la meme faute qu'en septembre, dans l'autre sens.
 *
 * LA VEILLE EST REPAREE, ET C'ETAIT LE MODE, PAS LA MACHINE. Le firmware
 * MrChromebox annonce « ACPI: PM: (supports S0 S3 S4 S5) » : il offre le S3,
 * contrairement au firmware ChromeOS d'origine. En S3, cinq reveils sur cinq,
 * dont un par la chaine logind complete. « mem_sleep_default=deep » est
 * desormais dans le fragment GRUB. Le protocole qui a disculpe le logiciel :
 * pm_test a « freezer », « devices », « platform » -- les seules etapes que
 * s2idle accepte -- passe les trois.
 *
 * CE QUI MANQUAIT ETAIT DONC LES DEUX : une reprise qui marche, ET un preavis
 * avec une porte de sortie. Le preavis existe -- voir batterie.h -- et la
 * reprise depuis le 16 septembre.
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
