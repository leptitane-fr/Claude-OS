/* Surveillance de la charge — voir batterie.h pour le raisonnement complet. */

#include "batterie.h"
#include "sysfs.h"

#include <gio/gio.h>
#include <stdlib.h>

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
/* logind sait-il faire « Can<methode> » ? Il repond « yes », « no », « na »
 * (pas de materiel pour ca) ou « challenge » (il faudrait s'authentifier).
 * Seul « yes » nous interesse : sur tout le reste on se rabattra. */
static gboolean
logind_sait_faire (GDBusConnection *bus, const char *methode)
{
    g_autofree char *question = g_strconcat ("Can", methode, NULL);
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) rep = g_dbus_connection_call_sync (
        bus, "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", question, NULL,
        G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    if (rep == NULL) {
        g_message ("batterie : %s sans reponse — %s", question, err->message);
        return FALSE;
    }
    const char *r = NULL;
    g_variant_get (rep, "(&s)", &r);
    return g_strcmp0 (r, "yes") == 0;
}

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

    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) bus =
        g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (bus == NULL) {
        g_message ("batterie : bus systeme injoignable — %s", err->message);
        return;
    }

    /* CE REPLI N'EST PAS DECORATIF. Demander une hibernation impossible --
     * swap trop petit, « resume » absent, noyau sans support -- rend une
     * erreur que personne ne lit, et la machine meurt quand meme : le pire
     * des deux mondes, puisqu'on croyait etre a l'abri. Faute de pouvoir
     * hiberner, une extinction propre sauve au moins ce qui est enregistre. */
    if (!logind_sait_faire (bus, methode)) {
        g_message ("batterie : « %s » indisponible — extinction a la place",
                   methode);
        methode = "PowerOff";
    }

    g_message ("batterie : mise a l'abri — %s", methode);
    g_dbus_connection_call (bus, "org.freedesktop.login1",
                            "/org/freedesktop/login1",
                            "org.freedesktop.login1.Manager", methode,
                            g_variant_new ("(b)", FALSE),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* ------------------------------------------------------------------------- */

static gboolean on_lecture (gpointer data);

static void
reprogrammer (int secondes)
{
    if (B.source != 0)
        g_source_remove (B.source);
    B.source = g_timeout_add_seconds (secondes, on_lecture, NULL);
}

static void
franchir (Seuil s, int pourcent, double heures)
{
    B.arme[s] = FALSE;

    g_autofree char *reste = NULL;
    if (heures > 0.0)
        reste = g_strdup_printf (" Environ %d h %02d avant l'arret.",
                                 (int) heures, (int) ((heures - (int) heures) * 60));

    g_autofree char *corps = NULL;

    switch (s) {
    case SEUIL_PREVENIR:
        corps = g_strdup_printf ("Il reste %d %%.%s Pensez a brancher.",
                                 pourcent, reste ? reste : "");
        prevenir ("Batterie faible", corps, FALSE);
        break;
    case SEUIL_INSISTER:
        corps = g_strdup_printf ("Il reste %d %%.%s Branchez maintenant.",
                                 pourcent, reste ? reste : "");
        prevenir ("Batterie tres faible", corps, TRUE);
        break;
    case SEUIL_ABRI:
        if (B.abri_engage)
            return;
        B.abri_engage = TRUE;
        corps = g_strdup_printf ("Il reste %d %%. %s.", pourcent, B.abri->nom);
        prevenir ("Batterie critique", corps, TRUE);
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

    /* Rebrancher remet tout a zero : les seuils reparleront au prochain
     * debranchement, meme si la charge n'a pas eu le temps de remonter. */
    if (secteur && !B.etait_sur_secteur) {
        for (int s = 0; s < SEUILS; s++)
            B.arme[s] = TRUE;
        B.abri_engage = FALSE;
        g_message ("batterie : sur secteur, seuils rearmes");
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

    reprogrammer (prochain_intervalle (dir, pourcent, secteur));
    return G_SOURCE_REMOVE;
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
    appliquer_config (cfg);

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
