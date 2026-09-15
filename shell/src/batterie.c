/* Surveillance de la charge — voir batterie.h pour le raisonnement complet. */

#include "batterie.h"
#include "sysfs.h"
#include "logind.h"
#include "avis.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <linux/netlink.h>

/* Marge de rearmement, en points de pourcentage. Un seuil qui vient de
 * parler se tait jusqu'a ce que la charge soit remontee de CE tant au-dessus
 * de lui. Trois points : assez pour qu'une charge qui oscille ne relance
 * rien, assez peu pour qu'un branchement bref suivi d'un debranchement
 * previenne a nouveau. */
#define MARGE_REARMEMENT 3

/* Bornes de l'intervalle de lecture, en secondes. Le calcul vise le quart du
 * temps restant avant le prochain seuil ; ces bornes l'empechent de devenir
 * absurde dans les deux sens -- une batterie qui se vide en dix minutes ne
 * justifie pas de lire toutes les secondes, et une batterie pleine ne
 * justifie pas d'attendre une heure. */
#define INTERVALLE_MIN    20
#define INTERVALLE_MAX   300
#define INTERVALLE_SECTEUR 300
/* Quand le courant est nul ou illisible -- batterie pleine, pilote muet --
 * on ne peut rien estimer : cadence de repli. */
#define INTERVALLE_AVEUGLE 120

/* Duree des avis, en secondes -- ceux qui passent au centre de l'ecran,
 * au-dessus du dock, et disparaissent seuls (avis.h).
 *
 * DEUX VALEURS ET NON UNE. Le branchement est une confirmation : on vient de
 * faire le geste, on attend juste de savoir qu'il a pris, et quatre secondes
 * suffisent -- au-dela l'avis devient un reproche. Un seuil de batterie, lui,
 * n'a ete demande par personne : il doit survivre au temps qu'on met a lever
 * les yeux. */
#define AVIS_SECTEUR  4
#define AVIS_SEUIL    7

typedef enum { SEUIL_PREVENIR = 0, SEUIL_INSISTER, SEUIL_ABRI, SEUILS } Seuil;

static struct {
    gboolean actif;          /* une batterie a ete trouvee                  */
    guint    source;         /* la prochaine lecture                        */
    guint32  notif_id;       /* pour remplacer l'avis precedent, pas l'empiler */

    int      seuils[SEUILS];
    gboolean arme[SEUILS];   /* ce seuil peut-il encore parler              */
    const ShellAbriBatterie *abri;

    gboolean etait_sur_secteur;
    gboolean abri_engage;     /* on a deja agi : ne pas le refaire en boucle */

    ShellBatterieLueFunc lue_fn;
    gpointer             lue_data;

    int      prise_fd;        /* netlink : les uevents du noyau              */
    guint    prise_source;    /* la surveillance de ce descripteur           */
    guint    prise_rebond;    /* le regroupement des rafales                 */
} B;

/* -------------------------------------------------------------------------
 * Lire la machine
 *
 * Cette batterie rapporte en CHARGE (µAh) et non en energie : elle n'a ni
 * energy_now ni power_now -- verifie le 14 septembre 2026. On garde tout de
 * meme les deux conventions, comme panel.c, parce qu'un module qui ne marche
 * que sur une machine est un module qu'on reecrira.
 * ------------------------------------------------------------------------- */
static gboolean
lire_charge (const char *dir, double *maintenant, double *plein, double *courant)
{
    g_autofree char *e = shell_sysfs_read (dir, "energy_now");
    g_autofree char *f = shell_sysfs_read (dir, "energy_full");
    g_autofree char *p = shell_sysfs_read (dir, "power_now");
    if (e != NULL && f != NULL) {
        *maintenant = g_ascii_strtod (e, NULL);
        *plein      = g_ascii_strtod (f, NULL);
        *courant    = (p != NULL) ? ABS (g_ascii_strtod (p, NULL)) : 0.0;
        return *plein > 0.0;
    }

    g_autofree char *c = shell_sysfs_read (dir, "charge_now");
    g_autofree char *d = shell_sysfs_read (dir, "charge_full");
    g_autofree char *i = shell_sysfs_read (dir, "current_now");
    if (c == NULL || d == NULL)
        return FALSE;

    *maintenant = g_ascii_strtod (c, NULL);
    *plein      = g_ascii_strtod (d, NULL);
    *courant    = (i != NULL) ? ABS (g_ascii_strtod (i, NULL)) : 0.0;
    return *plein > 0.0;
}

gboolean
shell_batterie_etat (int *pourcent, gboolean *sur_secteur, double *heures)
{
    g_autofree char *dir = shell_battery_dir ();
    if (dir == NULL)
        return FALSE;

    g_autofree char *cap = shell_sysfs_read (dir, "capacity");
    if (cap == NULL)
        return FALSE;

    if (pourcent)    *pourcent    = atoi (cap);
    if (sur_secteur) *sur_secteur = shell_sur_secteur ();
    if (heures)      *heures      = -1.0;

    double maintenant, plein, courant;
    if (heures != NULL && !shell_sur_secteur ()
        && lire_charge (dir, &maintenant, &plein, &courant) && courant > 0.0)
        *heures = maintenant / courant;

    return TRUE;
}

/* -------------------------------------------------------------------------
 * Quand faut-il regarder a nouveau ?
 *
 * Le quart du temps qui reste avant le prochain seuil. « Le quart » n'a rien
 * d'une science : c'est ce qui laisse trois lectures pour voir arriver le
 * seuil, meme si la consommation double entre-temps.
 * ------------------------------------------------------------------------- */
static int
prochain_intervalle (const char *dir, int pourcent, gboolean sur_secteur)
{
    if (sur_secteur)
        return INTERVALLE_SECTEUR;

    /* Le seuil le plus haut qui soit encore DEVANT nous. */
    int cible = -1;
    for (int s = 0; s < SEUILS; s++)
        if (B.seuils[s] < pourcent && B.seuils[s] > cible)
            cible = B.seuils[s];
    if (cible < 0)
        cible = 0;          /* tous franchis : on vise la panne seche */

    double maintenant, plein, courant;
    if (!lire_charge (dir, &maintenant, &plein, &courant) || courant <= 0.0)
        return INTERVALLE_AVEUGLE;

    double reste = maintenant - (plein * cible / 100.0);
    if (reste <= 0.0)
        return INTERVALLE_MIN;

    int quart = (int) (reste / courant * 3600.0 / 4.0);
    return CLAMP (quart, INTERVALLE_MIN, INTERVALLE_MAX);
}

/* -------------------------------------------------------------------------
 * Prevenir
 *
 * Par org.freedesktop.Notifications, que le shell sert lui-meme
 * (notifications.c). Passer par le bus plutot que par un appel interne n'est
 * pas un detour : l'avis entre ainsi dans la cloche et dans l'historique,
 * comme n'importe quelle notification, sans que ce module ait a connaitre
 * le centre.
 *
 * « replaces_id » remplace l'avis precedent au lieu d'en empiler un second :
 * a 8 % on ne veut pas lire celui de 20 % en dessous.
 * ------------------------------------------------------------------------- */
static void
on_notifie (GObject *src, GAsyncResult *res, gpointer data)
{
    (void) data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) rep =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
    if (rep == NULL) {
        g_message ("batterie : avis non delivre — %s", err->message);
        return;
    }
    g_variant_get (rep, "(u)", &B.notif_id);
}

static void
prevenir (const char *titre, const char *corps, gboolean urgent)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) bus =
        g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &err);
    if (bus == NULL) {
        g_message ("batterie : bus de session injoignable — %s", err->message);
        return;
    }

    GVariantBuilder actions, hints;
    g_variant_builder_init (&actions, G_VARIANT_TYPE ("as"));
    g_variant_builder_init (&hints, G_VARIANT_TYPE ("a{sv}"));
    /* 2 = critique au sens de la specification : le centre ne la fera pas
     * disparaitre toute seule. C'est voulu au dernier seuil. */
    g_variant_builder_add (&hints, "{sv}", "urgency",
                           g_variant_new_byte (urgent ? 2 : 1));

    g_dbus_connection_call (bus, "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
        "Notify",
        g_variant_new ("(susssasa{sv}i)", "Claude OS", B.notif_id,
                       urgent ? "battery-caution-symbolic"
                              : "battery-low-symbolic",
                       titre, corps, &actions, &hints,
                       urgent ? 0 : 20000),
        G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        on_notifie, NULL);
}

/* -------------------------------------------------------------------------
 * Agir
 *
 * FALSE au second argument : on ne force pas, exactement comme l'etage
 * « suspendre » de energie.c. logind interroge les inhibiteurs, et une
 * application qui a demande a ne pas etre interrompue l'emporte.
 *
 * Ce choix se discute ICI et pas ailleurs : au dernier seuil, un inhibiteur
 * qui gagne, c'est une session perdue. Mais forcer, c'est couper la parole a
 * un enregistrement en cours pour une estimation de pourcentage. On previent
 * donc fort, et on laisse l'inhibiteur gagner -- il a une raison d'exister,
 * et l'utilisateur a ete averti deux fois avant.
 * ------------------------------------------------------------------------- */
static void
mettre_a_l_abri (void)
{
    const char *methode = NULL;
    if (g_strcmp0 (B.abri->id, "hiberner") == 0)       methode = "Hibernate";
    else if (g_strcmp0 (B.abri->id, "suspendre") == 0) methode = "Suspend";
    else if (g_strcmp0 (B.abri->id, "eteindre") == 0)  methode = "PowerOff";

    if (methode == NULL) {
        g_message ("batterie : seuil d'abri atteint, action « rien »");
        return;
    }

    /* CE REPLI N'EST PAS DECORATIF. Demander une hibernation impossible --
     * swap trop petit, « resume » absent, noyau sans support -- rend une
     * erreur que personne ne lit, et la machine meurt quand meme : le pire
     * des deux mondes, puisqu'on croyait etre a l'abri. Faute de pouvoir
     * hiberner, une extinction propre sauve au moins ce qui est enregistre. */
    if (!shell_logind_sait_faire (methode)) {
        g_message ("batterie : « %s » indisponible — extinction a la place",
                   methode);
        methode = "PowerOff";
    }

    g_message ("batterie : mise a l'abri — %s", methode);
    shell_logind_appeler (methode);
}

/* -------------------------------------------------------------------------
 * LA PRISE, ELLE, PREVIENT -- ET C'EST MESURE
 *
 * batterie.h explique pourquoi la CHARGE se scrute : sept minutes d'ecoute
 * pendant une charge active, neuf changements de pourcentage, zero
 * evenement ; l'essai refait en decharge, meme resultat. Cette conclusion
 * tient, et la scrutation ci-dessous reste.
 *
 * MAIS ELLE NE VAUT QUE POUR LE POURCENTAGE. Brancher ou debrancher est un
 * evenement materiel, et le noyau l'annonce : le pilote ACPI de l'adaptateur
 * appelle power_supply_changed(), qui emet un uevent sur la classe
 * power_supply. Mesure du 15 septembre 2026 sur MADOO, socket netlink en
 * ecoute et « udevadm trigger --subsystem-match=power_supply » : cinq
 * messages recus, dont celui de l'adaptateur, POWER_SUPPLY_ONLINE dans la
 * charge utile.
 *
 * ET C'EST EXACTEMENT LA DIFFERENCE QUI COMPTAIT. Un pourcentage qui change
 * peut attendre la prochaine lecture -- il aura a peine bouge. Un cable
 * qu'on branche, non : l'avis « En charge » doit repondre au geste, pas
 * arriver jusqu'a cinq minutes plus tard, ce qui etait le comportement
 * observe et rapporte.
 *
 * SANS PRIVILEGE, ET SANS LIBUDEV. Le groupe 1 de NETLINK_KOBJECT_UEVENT est
 * celui des uevents du NOYAU ; il est declare NL_CFG_F_NONROOT_RECV, donc un
 * processus ordinaire peut s'y abonner -- verifie en s'y abonnant. Le groupe
 * 2 est celui que rediffuse udevd, et lui demanderait libudev pour un
 * service identique : une dependance de plus pour lire les memes octets.
 *
 * LA SCRUTATION RESTE, en filet. Si ce socket ne s'ouvrait pas -- noyau
 * different, bac a sable -- on retombe simplement sur le comportement
 * d'avant : plus lent, jamais muet.
 * ------------------------------------------------------------------------- */

static gboolean on_lecture (gpointer data);

/* Une rafale d'uevents -- cinq d'un coup, un par alimentation -- ne doit
 * declencher qu'une lecture. */
static gboolean
prise_relire (gpointer data)
{
    (void) data;
    B.prise_rebond = 0;

    /* DIT, ET PAS SEULEMENT FAIT. C'est la seule trace qui distingue « le
     * noyau a prevenu » de « la scrutation est passee par la » -- et sans
     * elle, un avis tardif ne dirait pas si l'abonnement a echoue ou si le
     * pilote est muet. Rare par nature : sept minutes de charge active
     * n'avaient produit aucun evenement (batterie.h). */
    g_message ("batterie : uevent d'alimentation — lecture immediate");

    if (B.source != 0) {
        g_source_remove (B.source);
        B.source = 0;
    }
    on_lecture (NULL);
    return G_SOURCE_REMOVE;
}

static gboolean
on_uevent (gint fd, GIOCondition cond, gpointer data)
{
    (void) data;

    if (cond & (G_IO_ERR | G_IO_HUP)) {
        g_message ("batterie : netlink ferme — retour a la seule scrutation");
        B.prise_source = 0;
        return G_SOURCE_REMOVE;
    }

    /* Le message est une suite de chaines nul-terminees. On ne cherche qu'une
     * chose : le sous-systeme. Le groupe 1 porte TOUS les uevents du noyau --
     * USB, entrees, blocs -- et se reveiller pour eux serait payer une
     * scrutation deguisee. */
    char tampon[8192];
    ssize_t n;
    gboolean concerne = FALSE;

    while ((n = recv (fd, tampon, sizeof tampon - 1, MSG_DONTWAIT)) > 0) {
        tampon[n] = '\0';
        for (ssize_t i = 0; i < n; i += (ssize_t) strlen (tampon + i) + 1)
            if (g_strcmp0 (tampon + i, "SUBSYSTEM=power_supply") == 0) {
                concerne = TRUE;
                break;
            }
    }

    if (!concerne)
        return G_SOURCE_CONTINUE;

    if (B.prise_rebond != 0)
        g_source_remove (B.prise_rebond);
    B.prise_rebond = g_timeout_add (250, prise_relire, NULL);
    return G_SOURCE_CONTINUE;
}

static void
prise_init (void)
{
    B.prise_fd = socket (PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                         NETLINK_KOBJECT_UEVENT);
    if (B.prise_fd < 0) {
        g_message ("batterie : netlink indisponible (%s) — seule la "
                   "scrutation previendra du branchement",
                   g_strerror (errno));
        return;
    }

    struct sockaddr_nl sa;
    memset (&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;   /* les uevents du noyau ; 2 serait ceux de udevd */

    if (bind (B.prise_fd, (struct sockaddr *) &sa, sizeof sa) < 0) {
        /* DIT, ET NON AVALE. Sans ce message, un noyau qui refuserait
         * l'abonnement rendrait simplement l'avis « En charge » tardif, et
         * l'on chercherait la cause dans l'affichage. */
        g_message ("batterie : abonnement netlink refuse (%s) — seule la "
                   "scrutation previendra du branchement",
                   g_strerror (errno));
        close (B.prise_fd);
        B.prise_fd = -1;
        return;
    }

    B.prise_source = g_unix_fd_add (B.prise_fd,
                                    G_IO_IN | G_IO_ERR | G_IO_HUP,
                                    on_uevent, NULL);
    g_message ("batterie : branchement et debranchement suivis par uevent");
}

/* ------------------------------------------------------------------------- */


static void
reprogrammer (int secondes)
{
    if (B.source != 0)
        g_source_remove (B.source);
    B.source = g_timeout_add_seconds (secondes, on_lecture, NULL);
}

/* LES TEXTES CI-DESSOUS SONT LUS PAR UN HUMAIN, ET PORTENT DONC LEURS
 * ACCENTS -- contrairement aux commentaires de ce fichier, qui n'en ont pas.
 * La regle du projet vaut pour le code, pas pour ce qui s'affiche : la
 * premiere notification livree disait « Pensez a brancher », et cela se
 * voyait a l'ecran. */
static void
franchir (Seuil s, int pourcent, double heures)
{
    B.arme[s] = FALSE;

    g_autofree char *reste = NULL;
    if (heures > 0.0)
        reste = g_strdup_printf (" Environ %d h %02d avant l'arrêt.",
                                 (int) heures, (int) ((heures - (int) heures) * 60));

    g_autofree char *corps = NULL;

    switch (s) {
    case SEUIL_PREVENIR:
        corps = g_strdup_printf ("Il reste %d %%.%s Pensez à brancher.",
                                 pourcent, reste ? reste : "");
        prevenir ("Batterie faible", corps, FALSE);
        shell_avis_message ("battery-low-symbolic", "Batterie faible", AVIS_SEUIL);
        break;
    case SEUIL_INSISTER:
        corps = g_strdup_printf ("Il reste %d %%.%s Branchez maintenant.",
                                 pourcent, reste ? reste : "");
        prevenir ("Batterie très faible", corps, TRUE);
        shell_avis_message ("battery-caution-symbolic", "Batterie très faible",
                            AVIS_SEUIL);
        break;
    case SEUIL_ABRI:
        if (B.abri_engage)
            return;
        B.abri_engage = TRUE;
        corps = g_strdup_printf ("Il reste %d %%. %s.", pourcent, B.abri->nom);
        prevenir ("Batterie critique", corps, TRUE);
        shell_avis_message ("battery-caution-symbolic", "Batterie critique",
                            AVIS_SEUIL);
        mettre_a_l_abri ();
        break;
    default:
        break;
    }
}

static gboolean
on_lecture (gpointer data)
{
    (void) data;
    B.source = 0;

    g_autofree char *dir = shell_battery_dir ();
    if (dir == NULL)
        return G_SOURCE_REMOVE;

    int pourcent; gboolean secteur; double heures;
    if (!shell_batterie_etat (&pourcent, &secteur, &heures)) {
        reprogrammer (INTERVALLE_AVEUGLE);
        return G_SOURCE_REMOVE;
    }

    /* LES DEUX BASCULES DE LA PRISE.
     *
     * UN AVIS, ET PAS DE NOTIFICATION. Brancher ou debrancher est un geste
     * qu'on vient de faire : on veut la confirmation tout de suite, et on
     * n'a aucune raison de la retrouver dans la cloche une heure plus tard.
     * C'est exactement ce que la surface d'avis sait faire et que le centre
     * de notifications ferait mal -- l'inverse des seuils, qui meritent les
     * deux.
     *
     * AUCUNE DES DEUX NE PEUT SE DECLENCHER A L'OUVERTURE DE SESSION : la
     * bascule se mesure contre « etait_sur_secteur », que
     * shell_batterie_init renseigne a l'etat reel avant la premiere lecture.
     * Sans cela, tout demarrage sur secteur aurait affiche « En charge ».
     *
     * L'ICONE DE « Sur batterie » EST CELLE DU NIVEAU REEL, pas une pile
     * generique : au moment ou l'on debranche, ce qu'on veut savoir est
     * precisement combien il reste. Meme famille d'icones que la barre
     * d'etat, meme arrondi a la dizaine. */
    if (secteur && !B.etait_sur_secteur) {
        for (int s = 0; s < SEUILS; s++)
            B.arme[s] = TRUE;
        B.abri_engage = FALSE;
        g_message ("batterie : sur secteur, seuils rearmes");
        shell_avis_message ("ac-adapter-symbolic", "En charge", AVIS_SECTEUR);
    } else if (!secteur && B.etait_sur_secteur) {
        g_message ("batterie : sur batterie, %d %%", pourcent);
        int cran = (pourcent + 5) / 10 * 10;
        if (cran > 100) cran = 100;
        g_autofree char *icone =
            g_strdup_printf ("battery-level-%d-symbolic", cran);
        shell_avis_message (icone, "Sur batterie", AVIS_SECTEUR);
    }
    B.etait_sur_secteur = secteur;

    if (!secteur) {
        for (int s = 0; s < SEUILS; s++) {
            if (!B.arme[s] && pourcent > B.seuils[s] + MARGE_REARMEMENT)
                B.arme[s] = TRUE;          /* hysteresis : voir batterie.h */
            if (B.arme[s] && pourcent <= B.seuils[s])
                franchir ((Seuil) s, pourcent, heures);
        }
    }

    /* APRES les seuils et les bascules, jamais avant : celui qui repeint
     * doit voir l'etat dans lequel ce tour de lecture l'a laisse. */
    if (B.lue_fn != NULL)
        B.lue_fn (B.lue_data);

    reprogrammer (prochain_intervalle (dir, pourcent, secteur));
    return G_SOURCE_REMOVE;
}

void
shell_batterie_sur_lecture (ShellBatterieLueFunc f, gpointer data)
{
    B.lue_fn   = f;
    B.lue_data = data;
}

static void
appliquer_config (const ShellConfig *cfg)
{
    B.seuils[SEUIL_PREVENIR] = cfg->energie_bat_prevenir;
    B.seuils[SEUIL_INSISTER] = cfg->energie_bat_insister;
    B.seuils[SEUIL_ABRI]     = cfg->energie_bat_abri;
    B.abri                   = shell_batterie_abri_actif (cfg);

    g_message ("batterie : seuils %d / %d / %d %%, abri « %s »",
               B.seuils[SEUIL_PREVENIR], B.seuils[SEUIL_INSISTER],
               B.seuils[SEUIL_ABRI], B.abri->id);
}

void
shell_batterie_init (const ShellConfig *cfg)
{
    g_autofree char *dir = shell_battery_dir ();
    if (dir == NULL) {
        g_message ("batterie : aucune batterie — surveillance inutile");
        return;
    }

    B.actif = TRUE;
    for (int s = 0; s < SEUILS; s++)
        B.arme[s] = TRUE;
    B.etait_sur_secteur = shell_sur_secteur ();
    B.prise_fd = -1;
    appliquer_config (cfg);
    prise_init ();

    /* Une premiere lecture tout de suite : ouvrir la session avec 4 % de
     * charge doit prevenir, pas attendre le premier intervalle. */
    on_lecture (NULL);
}

void
shell_batterie_reconfigurer (const ShellConfig *cfg)
{
    if (!B.actif)
        return;
    appliquer_config (cfg);
    on_lecture (NULL);
}
