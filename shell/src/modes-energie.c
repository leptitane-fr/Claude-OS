/* =========================================================================
 * Claude-OS Shell — la definition des trois modes d'energie
 *
 * SEPARE DE energie.c PARCE QUE TROIS PROGRAMMES EN ONT BESOIN.
 *
 * La barre d'etat applique les modes, la Console les propose, le panneau de
 * reglages en regle les durees. Laisser la table dans energie.c aurait
 * oblige a lier tout le module -- Wayland, layer-shell, le retroeclairage --
 * dans un panneau de reglages qui ne veut qu'une liste de libelles.
 *
 * Elle vit donc dans « commun », a cote de la configuration. C'est LA
 * definition des modes : personne n'en fabrique une seconde ailleurs, et
 * c'est ce qui garantit que la Console annonce exactement ce que la barre
 * applique.
 * ========================================================================= */
#include "energie.h"

/* LES ICONES, choisies le 11 septembre 2026 pour la Console et la barre
 * d'etat : la famille « power-profile » que GNOME emploie pour ses profils
 * d'energie. Voir shell_energie_mode_icone() pour le theme ou on les prend. */
static const ShellModeEnergie MODES[] = {
    { "travail", "Travail", "power-profile-performance-symbolic",
      "L'ordinateur ne dort jamais et le réseau reste actif. L'écran "
      "s'atténue puis s'éteint, précédé d'un compte à rebours.", FALSE },
    { "automatique", "Automatique", "power-profile-balanced-symbolic",
      "L'équilibre. L'écran s'atténue, s'éteint, puis l'ordinateur se met "
      "en veille.", TRUE },
    { "nomade", "Nomade", "power-profile-power-saver-symbolic",
      "Le plus économe. Délais courts et veille rapide.", TRUE },
    { NULL, NULL, NULL, NULL, FALSE },
};

const ShellModeEnergie *
shell_energie_modes (void)
{
    return MODES;
}

const ShellModeEnergie *
shell_energie_mode_actif (const ShellConfig *cfg)
{
    for (int i = 0; MODES[i].id != NULL; i++)
        if (g_strcmp0 (cfg->energie_mode, MODES[i].id) == 0)
            return &MODES[i];
    return &MODES[1];   /* automatique : le defaut, jamais NULL */
}

void
shell_energie_delais (const ShellConfig *cfg, int *preavis, int *attenuer,
                      int *eteindre, int *suspendre)
{
    shell_energie_delais_mode (cfg, shell_energie_mode_actif (cfg),
                               preavis, attenuer, eteindre, suspendre);
}

void
shell_energie_delais_mode (const ShellConfig *cfg, const ShellModeEnergie *m,
                           int *preavis, int *attenuer, int *eteindre,
                           int *suspendre)
{
    int att, ete, sus;
    if (m == NULL)
        m = shell_energie_mode_actif (cfg);

    /* Le preavis vaut pour les TROIS modes : la gene qu'il corrige depend
     * de ce qu'on est en train de faire, pas du profil choisi. */
    int pre = cfg->energie_preavis;

    if (g_strcmp0 (m->id, "travail") == 0) {
        att = cfg->energie_travail_attenuer;
        ete = cfg->energie_travail_eteindre;
        sus = 0;
    } else if (g_strcmp0 (m->id, "nomade") == 0) {
        att = cfg->energie_nomade_attenuer;
        ete = cfg->energie_nomade_eteindre;
        sus = cfg->energie_nomade_suspendre;
    } else {
        att = cfg->energie_auto_attenuer;
        ete = cfg->energie_auto_eteindre;
        sus = cfg->energie_auto_suspendre;
    }

    /* LES ETAGES SONT REMIS DANS L'ORDRE, ET C'EST INDISPENSABLE.
     *
     * J'avais ecrit que l'ordre attenuer -> eteindre -> suspendre n'etait
     * pas exposé et ne pouvait donc pas etre casse. C'etait faux : les
     * listes de durees du panneau sont independantes, et rien n'empechait
     * « attenuer 3 min, eteindre 1 min ». Trouve dans le shell.conf reel le
     * 9 septembre 2026 -- l'ecran se serait eteint avant d'avoir baisse.
     *
     * On ne refuse pas la valeur et on ne la corrige pas dans le fichier :
     * on la borne ici, au seul endroit qui repond a la question. La Console
     * et les Reglages interrogeant cette meme fonction, ils annoncent la
     * duree REELLEMENT appliquee -- un panneau qui afficherait 1 min pendant
     * que la machine en attend 3 serait pire que le desordre lui-meme. */
    if (ete > 0 && ete < att) ete = att;
    if (sus > 0 && sus < ete) sus = ete;

    if (preavis)   *preavis   = pre;
    if (attenuer)  *attenuer  = att;
    if (eteindre)  *eteindre  = ete;
    /* Un mode qui ne dort pas ne dort pas, quelle que soit la valeur ecrite
     * dans le fichier. C'est ce qui rend un mode indenaturable. */
    if (suspendre) *suspendre = m->veille_ordi ? sus : 0;
}

/* L'ICONE D'UN MODE, PRISE CHEZ ADWAITA MEME SOUS PAPIRUS.
 *
 * Papirus dessine les trois profils comme trois cadrans identiques dont
 * seule l'aiguille change d'orientation : a 18 px dans la barre d'etat,
 * rien ne les distingue. Adwaita dessine un compteur, une balance et une
 * feuille -- trois objets, qui disent ce que decrivent les resumes. Compare
 * au rendu le 11 septembre 2026.
 *
 * On prend donc le fichier d'Adwaita, qui est une dependance de GTK et
 * toujours installe. Son nom finit en « -symbolic.svg » : GTK le recolore
 * comme n'importe quelle icone symbolique, il suit donc le theme clair ou
 * sombre. S'il manque -- autre version d'Adwaita, autre arborescence --,
 * repli sur le nom, que le theme d'icones en vigueur resoudra. */
GIcon *
shell_energie_mode_icone (const ShellModeEnergie *m)
{
    g_autofree char *fichier = g_strdup_printf (
        "/usr/share/icons/Adwaita/symbolic/status/%s.svg", m->icone);
    if (g_file_test (fichier, G_FILE_TEST_EXISTS)) {
        g_autoptr(GFile) f = g_file_new_for_path (fichier);
        return g_file_icon_new (f);
    }
    return g_themed_icon_new (m->icone);
}
