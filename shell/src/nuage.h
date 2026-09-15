/* =========================================================================
 * Claude-OS Shell — lecteurs nuage : le modele
 *
 * Google Drive, OneDrive. Partage entre « Fichiers » -- qui les montre dans
 * son volet lateral -- et « Reglages », qui les declare.
 *
 * POURQUOI UN MODULE A PART, ET NON DEUX PROTOCOLES DE PLUS DANS reseau.c
 *
 * La question s'est posee, et la reponse tient au peu qu'ils ont en commun.
 * Un lecteur nuage n'a ni serveur, ni partage, ni domaine, ni mot de passe,
 * ni options de montage : les cinq champs de Lecteur resteraient vides, et
 * l'editeur du panneau de reglages n'en reutiliserait aucun. Ce qui se
 * partagerait vraiment -- la surveillance des montages et le lancement
 * asynchrone -- tient en une vingtaine de lignes.
 *
 * En face, reseau.c porte trois usages apres liberation trouves en cliquant
 * pour de vrai (voir docs/08). Y brancher un second modele pour economiser
 * vingt lignes aurait ete payer cher un gain nul.
 *
 * AUCUN PRIVILEGE, ET C'EST LA VRAIE DIFFERENCE
 *
 * Monter du CIFS ou du NFS est une operation du noyau, donc reservee a
 * root : claude-os-lecteur passe par le guichet. rclone, lui, monte par FUSE
 * sous le compte de l'utilisateur, dans un repertoire qui lui appartient.
 * claude-os-nuage REFUSE de tourner en root. Demander des droits dont on n'a
 * pas l'usage est la meilleure facon de s'habituer a les demander.
 *
 * LES JETONS NE SONT PAS DANS LE FICHIER DE CONFIGURATION
 *
 * Un jeton de rafraichissement rouvre le compte indefiniment, sans mot de
 * passe ni second facteur : il vaut un mot de passe. Il vit donc dans la
 * configuration CHIFFREE de rclone, dont la phrase est au trousseau -- voir
 * nuage-phrase.c. « ~/.config/claude-os/nuage » ne contient, lui, rien de
 * secret : la meme regle que pour les lecteurs reseau.
 * ========================================================================= */
#pragma once

/* GIO, et non GTK : ce modele ne manipule aucun widget, et peut donc servir
 * a un programme sans fenetre -- la connexion a l'ouverture de session --
 * sans embarquer un runtime GTK4 complet, mesure a ~40 Mo sur cette machine
 * qui n'a que 4 Go. Meme raisonnement qu'en tete de reseau.h. */
#include <gio/gio.h>

typedef enum {
    NUAGE_DRIVE,       /* Google Drive        */
    NUAGE_ONEDRIVE,    /* Microsoft OneDrive  */
} NuageFournisseur;

typedef struct {
    /* Identifiant stable. C'est la cle de section de NOTRE fichier, celle de
     * la configuration de rclone, ET le nom du repertoire de montage : le
     * renommer romprait les trois. Le libelle affiche se change librement. */
    char             *id;
    char             *nom;

    NuageFournisseur  fournisseur;

    /* L'adresse du compte connecte. Sert a l'affichage seul -- c'est rclone
     * qui detient le lien avec le compte, par son jeton. Savoir QUEL compte
     * est monte compte des qu'on en a deux. */
    char             *compte;

    /* Connecter a l'ouverture de session. Vrai par defaut ici, la ou il est
     * faux pour les lecteurs reseau : un NAS eteint fait attendre un delai
     * TCP, alors qu'un service en ligne repond ou ne repond pas. */
    gboolean          automatique;
} LecteurNuage;

void          nuage_free (LecteurNuage *l);
LecteurNuage *nuage_copie (const LecteurNuage *l);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (LecteurNuage, nuage_free)

const char       *nuage_fournisseur_id  (NuageFournisseur f);
NuageFournisseur  nuage_fournisseur_lire (const char *id);
const char       *nuage_fournisseur_nom (NuageFournisseur f);  /* libelle affiche */

/* Le nom d'icone du fournisseur, dans le theme de la distribution. */
const char       *nuage_fournisseur_icone (NuageFournisseur f);

/* -------------------------------------------------------------------------
 * La liste declaree — ~/.config/claude-os/nuage
 * ------------------------------------------------------------------------- */

/* Les lecteurs declares, dans l'ordre du fichier. Jamais NULL : sans
 * fichier, une liste vide. A liberer avec g_ptr_array_unref. */
GPtrArray *nuage_charger (void);

/* Reecrit le fichier. La liste donnee fait foi. */
gboolean nuage_enregistrer (GPtrArray *lecteurs, GError **erreur);

/* -------------------------------------------------------------------------
 * Etat
 * ------------------------------------------------------------------------- */

/* Le repertoire ou ce lecteur se monte, sous $XDG_RUNTIME_DIR : un tmpfs qui
 * appartient a l'utilisateur, efface a la fermeture de session. A liberer. */
char     *nuage_point_montage (const LecteurNuage *l);
gboolean  nuage_est_connecte  (const LecteurNuage *l);

/* Le libelle du lecteur dont `chemin` est exactement la racine, ou NULL.
 * C'est ce qui permet au fil d'Ariane d'afficher « Google Drive » plutot que
 * « run › user › 1000 › claude-os › nuage › gdrive ». A liberer. */
char     *nuage_nom_du_point (const char *chemin);

/* rclone est-il installe ? Sans lui, le panneau doit le DIRE plutot que de
 * laisser echouer chaque connexion avec un message de bas niveau. */
gboolean  nuage_outil_present (void);

/* Previent quand un montage apparait ou disparait, quelle qu'en soit
 * l'origine. Aucune scrutation : le noyau notifie /proc/self/mountinfo. */
typedef void (*NuageChangeFunc) (gpointer data);
void nuage_surveiller (NuageChangeFunc cb, gpointer data);
void nuage_ne_plus_surveiller (gpointer data);

/* -------------------------------------------------------------------------
 * Connexion — ASYNCHRONE, sans exception
 *
 * Un montage joint un service par le reseau : il repond en une seconde, ou
 * il fait attendre. Le faire de facon synchrone figerait l'interface au
 * moment precis ou l'utilisateur veut pouvoir cliquer ailleurs.
 * ------------------------------------------------------------------------- */

/* `erreur` NULL : c'est monte. Sinon le message est destine a l'ecran. */
typedef void (*NuageFiniFunc) (const LecteurNuage *l, GError *erreur, gpointer data);

void nuage_connecter   (const LecteurNuage *l, NuageFiniFunc fini, gpointer data);
void nuage_deconnecter (const LecteurNuage *l, NuageFiniFunc fini, gpointer data);
