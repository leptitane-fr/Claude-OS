/* =========================================================================
 * Claude-OS Shell — gestionnaire de fichiers : le modele
 *
 * Un element de la liste, et de quoi lire un repertoire.
 *
 * POURQUOI UN OBJET ET NON UNE STRUCTURE
 *
 * GtkColumnView, GtkGridView et GtkListView ne consomment que des
 * GListModel, dont les elements sont des GObject. C'est aussi ce qui rend
 * les vues VIRTUELLES : elles ne construisent de widgets que pour ce qui
 * est a l'ecran. Un repertoire de dix mille fichiers ne coute alors que dix
 * mille petits objets, la ou construire dix mille lignes de widgets
 * mettrait la machine a genoux.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

#define FICHIERS_TYPE_ITEM (fichier_item_get_type ())
G_DECLARE_FINAL_TYPE (FichierItem, fichier_item, FICHIERS, ITEM, GObject)

/* Champs lus directement : ce composant est d'un seul tenant, et une
 * vingtaine d'accesseurs triviaux n'apporteraient rien. */
struct _FichierItem {
    GObject   parent_instance;

    GFile    *file;
    char     *nom;           /* nom d'affichage                             */
    char     *cle_tri;       /* cle de collation : « Éclair » avant « Zebre »*/
    GIcon    *icone;
    char     *type_texte;    /* « Dossier », « Image PNG »…                 */
    goffset   taille;
    gint64    modifie;       /* secondes depuis l'epoque                    */
    gboolean  dossier;
    gboolean  cache;
    gboolean  lien;
};

FichierItem *fichier_item_new (GFile *parent, GFileInfo *info);

/* Textes prets a afficher. A liberer. */
char *fichier_item_taille_texte (FichierItem *it);
char *fichier_item_date_texte   (FichierItem *it);

/* -------------------------------------------------------------------------
 * Lecture d'un repertoire
 *
 * ASYNCHRONE, et par lots. Un repertoire sur une cle USB lente bloquerait
 * l'interface pendant toute son enumeration ; par lots, les premiers
 * fichiers s'affichent tout de suite et le reste arrive au fil de l'eau.
 * ------------------------------------------------------------------------- */

/* Appelee quand la lecture est finie, ou a echoue (erreur non NULL). */
typedef void (*FichiersLuFunc) (GListStore *contenu, GError *erreur, gpointer data);

/* Remplit `magasin` avec le contenu de `dossier`. Un appel annule le
 * precedent : naviguer vite ne doit pas melanger deux repertoires. */
void fichiers_lire (GFile *dossier, GListStore *magasin,
                    FichiersLuFunc fini, gpointer data);
