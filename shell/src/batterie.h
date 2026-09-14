/* =========================================================================
 * Claude-OS Shell — surveillance de la charge, preavis et mise a l'abri
 *
 * POURQUOI CE MODULE EXISTE : parce que RIEN ne surveillait la batterie.
 *
 * Constate le 14 septembre 2026 sur MADOO : pas d'upower, aucun demon
 * d'energie, le seuil ACPI « alarm » a zero. Le shell ne lisait la charge
 * que lorsque la Console etait ouverte, c'est-a-dire quand l'utilisateur
 * regardait deja. Une machine qui ne sait pas qu'elle va manquer de courant
 * ne peut ni prevenir, ni se mettre a l'abri : elle s'arrete, et la session
 * est perdue.
 *
 * Ce n'etait pas une hypothese. Le matin du 14 septembre, le journal garde
 * 101 demarrages, dont une centaine entre 05:39 et 07:23, par cycles
 * reguliers de 64 secondes : demarrer, vivre 28 secondes, se rendormir capot
 * ferme, mourir. Cinq jours durant, ces morts ont ete imputees a une reprise
 * defaillante -- a tort, voir energie.h.
 *
 * -------------------------------------------------------------------------
 * IL FAUT SCRUTER, ET C'EST MESURE : LA MACHINE NE PREVIENT PAS
 *
 * La regle du projet est « aucune scrutation », et energie.c la tient grace
 * a ext-idle-notify-v1. Ce module ne PEUT pas la tenir, et ce n'est pas une
 * facilite. Deux voies ont ete essayees le 14 septembre 2026 :
 *
 *   1. Les uevents du noyau. Sept minutes d'ecoute
 *      (udevadm monitor --subsystem-match=power_supply) pendant une charge
 *      active : NEUF changements de pourcentage, ZERO evenement.
 *
 *   2. Le seuil materiel. « alarm » arme AU-DESSUS de la charge courante,
 *      donc franchi d'emblee : aucun evenement, capacity_level immobile
 *      sur « Normal ».
 *
 * Reserve honnete : ces deux essais ont eu lieu BATTERIE EN CHARGE. Le
 * pilote peut se comporter autrement en decharge, et l'essai reste a
 * refaire. Si un evenement apparait un jour, ce module doit s'y brancher et
 * la scrutation ci-dessous devient un simple filet.
 *
 * -------------------------------------------------------------------------
 * ALORS ON SCRUTE LE MOINS POSSIBLE : L'INTERVALLE SE CALCULE
 *
 * Scruter a cadence fixe, c'est payer le pire cas en permanence. On calcule
 * donc le temps qui reste AVANT LE PROCHAIN SEUIL, a partir de la charge et
 * du courant instantane (charge_now / current_now, en µAh et µA : cette
 * batterie rapporte en charge, pas en energie -- elle n'a ni energy_now ni
 * power_now, verifie le 14 septembre), et l'on se reveille au quart de ce
 * temps. Loin du seuil la machine dort ; pres du seuil elle regarde souvent.
 *
 * Sur secteur, une seule chose peut arriver -- qu'on debranche -- et elle
 * n'est pas urgente : intervalle long, fixe.
 *
 * Une lecture coute deux fichiers sysfs servis par le pilote ACPI, qui tient
 * son propre cache. Ce n'est pas une commande a l'EC par lecture, contrairement
 * a la scrutation de la rotation (docs/12).
 *
 * -------------------------------------------------------------------------
 * TROIS SEUILS, ET LE DERNIER AGIT
 *
 * Prevenir, insister, se mettre a l'abri. Les deux premiers parlent ; seul
 * le troisieme decide, et il fait ce que l'utilisateur a choisi dans le
 * panneau Energie -- hiberner de preference, car c'est le seul etat qui
 * survive a une coupure franche.
 *
 * LES VALEURS NE SONT PAS ECRITES ICI. Elles vivent dans shell.conf et se
 * reglent dans le panneau : ce sont des habitudes de travail, pas des
 * constantes physiques, et elles dependent de l'endroit ou l'on travaille.
 *
 * HYSTERESIS. Un seuil franchi est consomme : il ne reparle plus tant que la
 * charge n'est pas remontee nettement au-dessus, ou qu'on n'a pas rebranche.
 * Sans cela, une charge qui oscille autour de 20 % produit une notification
 * toutes les minutes, et l'utilisateur apprend a les ignorer -- ce qui est
 * exactement la panne qu'on repare.
 * ========================================================================= */
#pragma once

#include <glib.h>
#include "config.h"

/* Ce que la machine fait en arrivant au dernier seuil. L'identifiant est ce
 * qui s'ecrit dans shell.conf ; la table vit dans abris-batterie.c, et elle
 * est LA definition -- le panneau y prend ses libelles au lieu de les redire. */
typedef struct {
    const char *id;       /* « hiberner », « suspendre », « eteindre », « rien » */
    const char *nom;      /* « Mettre a l'abri sur disque »                      */
    const char *resume;   /* une phrase, montree sous le choix                   */
} ShellAbriBatterie;

/* Table terminee par un id NULL. */
const ShellAbriBatterie *shell_batterie_abris (void);

/* L'abri choisi, jamais NULL : un identifiant inconnu -- fichier d'une
 * version anterieure -- renvoie le premier de la table. */
const ShellAbriBatterie *shell_batterie_abri_actif (const ShellConfig *cfg);

/* Demarre la surveillance. Sans batterie -- machine fixe, ou sysfs muet --
 * le module s'efface et le shell continue normalement : il n'y a rien a
 * surveiller, et c'est un cas legitime, pas une panne.
 *
 * A appeler une fois la connexion D-Bus de session disponible : les preavis
 * passent par org.freedesktop.Notifications, que le shell sert lui-meme
 * (notifications.c). */
void shell_batterie_init (const ShellConfig *cfg);

/* Relit les seuils et reprogramme la prochaine lecture. A appeler depuis le
 * rappel de shell_config_watch, comme shell_energie_reconfigurer : le
 * panneau ecrit shell.conf, chaque composant relit. */
void shell_batterie_reconfigurer (const ShellConfig *cfg);

/* Etat courant, pour qui veut l'afficher sans relire sysfs lui-meme. Rend
 * FALSE s'il n'y a pas de batterie, ou si la lecture a echoue. « pourcent »
 * et « sur_secteur » sont toujours renseignes en cas de succes ; « heures »
 * ne vaut qu'en decharge, et reste negatif sinon. */
gboolean shell_batterie_etat (int *pourcent, gboolean *sur_secteur, double *heures);
