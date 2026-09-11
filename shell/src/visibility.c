#include "visibility.h"

/* Combien de temps « aucune fenetre active » doit durer avant qu'on y
 * croie.
 *
 * Passer d'une fenetre a l'autre n'est PAS un evenement unique : le
 * compositeur retire l'etat « active » a l'ancienne, valide le lot, puis le
 * donne a la nouvelle et valide un second lot (labwc, focus_change_notify ;
 * wlr-foreign-toplevel valide fenetre par fenetre). Entre les deux, pendant
 * un instant, aucune fenetre n'est active. Reagir tout de suite ferait
 * surgir le dock a chaque Alt-Tab, pour le renvoyer une image plus tard.
 *
 * Les deux lots partent dans la meme iteration du compositeur ; quelques
 * millisecondes suffiraient. 150 ms couvrent une machine chargee, et ne se
 * sentent pas quand on ferme la derniere fenetre. */
#define DELAI_AUCUNE_MS 150

static struct {
    ShellVisibilityFunc cb;
    gpointer            data;
    ShellVisEtat        etat;
    guint64             actif;       /* serie de la fenetre active, 0 aucune */
    guint               attente;     /* confirmation de « aucune »           */
} V = { NULL, NULL, SHELL_VIS_BUREAU, 0, 0 };

static void
aller (ShellVisEtat etat)
{
    if (etat == V.etat)
        return;
    V.etat = etat;
    if (V.cb != NULL)
        V.cb (etat, V.data);
}

/* Ce que « montrer » veut dire depend de ce qu'il y a dessous. */
static ShellVisEtat
etat_visible (void)
{
    return V.actif != 0 ? SHELL_VIS_CONVOQUE : SHELL_VIS_BUREAU;
}

static gboolean
confirmer_aucune (gpointer data)
{
    (void) data;
    V.attente = 0;
    if (V.actif == 0)
        aller (SHELL_VIS_BUREAU);
    return G_SOURCE_REMOVE;
}

void
shell_visibility_init (ShellVisibilityFunc cb, gpointer user_data)
{
    V.cb   = cb;
    V.data = user_data;
}

void
shell_visibility_fenetre_active (guint64 serie)
{
    if (serie == V.actif)
        return;
    V.actif = serie;

    if (serie != 0) {
        /* Une fenetre vient d'etre activee : c'est tout le sens de la
         * demande. Clic dessus, lancement, Alt-Tab -- on s'efface. */
        if (V.attente != 0) {
            g_source_remove (V.attente);
            V.attente = 0;
        }
        aller (SHELL_VIS_CACHE);
        return;
    }

    if (V.attente == 0)
        V.attente = g_timeout_add (DELAI_AUCUNE_MS, confirmer_aucune, NULL);
}

void
shell_visibility_basculer (void)
{
    aller (V.etat == SHELL_VIS_CACHE ? etat_visible () : SHELL_VIS_CACHE);
}

void
shell_visibility_convoquer (void)
{
    if (V.etat == SHELL_VIS_CACHE)
        aller (etat_visible ());
}

void
shell_visibility_congedier (void)
{
    if (V.etat == SHELL_VIS_CONVOQUE)
        aller (SHELL_VIS_CACHE);
}

void
shell_visibility_cacher (void)
{
    aller (SHELL_VIS_CACHE);
}

ShellVisEtat
shell_visibility_etat (void)
{
    return V.etat;
}
