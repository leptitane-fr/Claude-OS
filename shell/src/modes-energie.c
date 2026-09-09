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

static const ShellModeEnergie MODES[] = {
    { "travail", "Travail", "document-edit-symbolic",
      "L'ordinateur ne dort jamais et le réseau reste actif. L'écran "
      "s'atténue puis s'éteint, précédé d'un compte à rebours.", FALSE },
    { "automatique", "Automatique", "preferences-system-symbolic",
      "L'équilibre. L'écran s'atténue, s'éteint, puis l'ordinateur se met "
      "en veille.", TRUE },
    { "nomade", "Nomade", "battery-low-symbolic",
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
    int pre = 0, att, ete, sus;
    if (m == NULL)
        m = shell_energie_mode_actif (cfg);

    if (g_strcmp0 (m->id, "travail") == 0) {
        pre = cfg->energie_travail_preavis;
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

    if (preavis)   *preavis   = pre;
    if (attenuer)  *attenuer  = att;
    if (eteindre)  *eteindre  = ete;
    /* Un mode qui ne dort pas ne dort pas, quelle que soit la valeur ecrite
     * dans le fichier. C'est ce qui rend un mode indenaturable. */
    if (suspendre) *suspendre = m->veille_ordi ? sus : 0;
}
