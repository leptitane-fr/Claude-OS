/* =========================================================================
 * Claude-OS Shell — lecteurs reseau : le modele
 *
 * Un lecteur reseau declare, ce qu'il faut pour l'atteindre, et de quoi le
 * connecter ou le rompre. Partage entre « Fichiers » -- qui les montre dans
 * son volet lateral -- et « Reglages », qui les declare.
 *
 * POURQUOI DES MONTAGES DU NOYAU, ET NON gvfs
 *
 * Mesure du 9 septembre 2026 : « gvfs-backends » tire 43 paquets sur cette
 * machine -- MTP, gphoto2, iOS, les codecs AV1 et HEIF -- pour des appareils
 * photo et des telephones dont il n'est pas question ici. Les memes quatre
 * protocoles par le noyau en demandent 19, dont 3 pour SMB seul.
 *
 * Et surtout : un montage gvfs n'existe que pour les applications GIO. Un
 * montage du noyau est un repertoire ORDINAIRE. Le terminal, Chromium, ses
 * boites « Enregistrer sous », Claude Desktop -- tout le systeme voit le
 * partage, sans qu'aucun de ces programmes ait rien a savoir du reseau.
 *
 * LE MOT DE PASSE N'EST PAS DANS LE FICHIER DE CONFIGURATION
 *
 * Il vit dans le trousseau, par libsecret -- gnome-keyring tourne deja sur
 * cette machine, avec son composant « secrets ». Le fichier « lecteurs » ne
 * contient que ce qui n'est pas secret, et peut donc etre lu, copie ou
 * verse dans un depot sans precaution particuliere.
 * ========================================================================= */
#pragma once

/* GIO, et non GTK : ce modele ne manipule aucun widget. C'est ce qui permet
 * a claude-os-lecteurs-auto -- lance a l'ouverture de session -- de le
 * reutiliser tel quel sans embarquer un runtime GTK4 complet, mesure a
 * ~40 Mo sur cette machine qui n'a que 4 Go. */
#include <gio/gio.h>

typedef enum {
    RESEAU_SMB,        /* partages Windows, NAS                             */
    RESEAU_NFS,        /* partages Unix                                     */
    RESEAU_SFTP,       /* un dossier distant par SSH                        */
    RESEAU_DAV,        /* Nextcloud, ownCloud                               */
} ReseauProtocole;

typedef struct {
    /* Identifiant stable. C'est la cle de section du fichier, la cle du
     * trousseau, ET le nom du repertoire de montage : le renommer romprait
     * les trois. Le libelle affiche se change librement, lui. */
    char            *id;
    char            *nom;

    ReseauProtocole  protocole;
    char            *serveur;
    char            *partage;      /* partage SMB, export NFS, chemin distant */
    char            *utilisateur;  /* vide : invite, pour SMB                */
    char            *domaine;
    char            *schema;       /* « http » ou « https », pour WebDAV     */
    char            *options;      /* options de montage supplementaires     */

    /* Connecter a l'ouverture de session. Faux par defaut : un partage
     * injoignable au demarrage ferait attendre la session sans rien dire. */
    gboolean         automatique;
} Lecteur;

void     lecteur_free (Lecteur *l);
Lecteur *lecteur_copie (const Lecteur *l);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Lecteur, lecteur_free)

/* Identifiant textuel du protocole, tel qu'il est ecrit dans le fichier. */
const char      *reseau_protocole_id  (ReseauProtocole p);
ReseauProtocole  reseau_protocole_lire (const char *id);
const char      *reseau_protocole_nom (ReseauProtocole p);  /* libelle affiche */

/* -------------------------------------------------------------------------
 * La liste declaree — ~/.config/claude-os/lecteurs
 * ------------------------------------------------------------------------- */

/* Les lecteurs declares, dans l'ordre du fichier. Jamais NULL : sans
 * fichier, une liste vide. A liberer avec g_ptr_array_unref. */
GPtrArray *reseau_charger (void);

/* Reecrit le fichier. La liste donnee fait foi : ce qui n'y est plus est
 * retire du fichier. */
gboolean reseau_enregistrer (GPtrArray *lecteurs, GError **erreur);

/* Fabrique un identifiant a partir d'un libelle, unique dans la liste. */
char *reseau_id_depuis_nom (const char *nom, GPtrArray *existants);

/* -------------------------------------------------------------------------
 * Etat
 * ------------------------------------------------------------------------- */

/* Le repertoire ou ce lecteur se monte. Sous /run : un tmpfs, donc aucun
 * point de montage mort ne survit a un redemarrage. A liberer. */
char    *reseau_point_montage (const Lecteur *l);

/* Le libelle du lecteur dont `chemin` est exactement la racine, ou NULL.
 *
 * C'est ce qui permet au fil d'Ariane d'afficher « Vidéos du NAS » au lieu
 * de « run › claude-os › reseau › nas-videos ». Le chemin de montage est un
 * detail d'implementation ; l'imposer a la lecture serait le contraire de
 * l'integration recherchee. A liberer. */
char    *reseau_nom_du_point (const char *chemin);
gboolean reseau_est_connecte  (const Lecteur *l);

/* Previent quand un montage apparait ou disparait, quelle qu'en soit
 * l'origine -- y compris un « umount » tape dans un terminal.
 *
 * AUCUNE SCRUTATION : GUnixMountMonitor s'appuie sur la notification que le
 * noyau emet sur /proc/self/mountinfo. Entre deux changements, cela ne
 * coute rien -- la meme regle que le module d'energie. */
typedef void (*ReseauChangeFunc) (gpointer data);
void reseau_surveiller (ReseauChangeFunc cb, gpointer data);

/* A appeler quand `data` est detruit : le moniteur, lui, ne l'est jamais. */
void reseau_ne_plus_surveiller (gpointer data);

/* -------------------------------------------------------------------------
 * Connexion
 *
 * ASYNCHRONE, sans exception. Un montage joint une machine par le reseau :
 * il repond en quelques millisecondes, ou il fait attendre le delai TCP
 * complet quand le serveur est eteint. Le faire de facon synchrone figerait
 * l'interface pendant ce delai -- et c'est justement le cas ou l'utilisateur
 * a le plus besoin de pouvoir cliquer ailleurs.
 * ------------------------------------------------------------------------- */

/* `erreur` NULL : c'est monte. Sinon le message est destine a l'ecran. */
typedef void (*ReseauFiniFunc) (const Lecteur *l, GError *erreur, gpointer data);

/* Connecte. `mot_de_passe` non NULL remplace celui du trousseau et, si
 * `retenir` est vrai, y est enregistre. NULL : on prend celui du trousseau,
 * et a defaut on tente en invite. */
void reseau_connecter (const Lecteur *l, const char *mot_de_passe,
                       gboolean retenir, ReseauFiniFunc fini, gpointer data);

void reseau_deconnecter (const Lecteur *l, ReseauFiniFunc fini, gpointer data);

/* -------------------------------------------------------------------------
 * Trousseau
 * ------------------------------------------------------------------------- */

/* Y a-t-il un mot de passe retenu pour ce lecteur ? Le panneau de reglages
 * s'en sert pour dire « mot de passe enregistre » plutot que de le montrer. */
void reseau_secret_present (const Lecteur *l, GAsyncReadyCallback cb, gpointer data);
gboolean reseau_secret_present_fin (GAsyncResult *res);

/* Efface le mot de passe retenu. Appele aussi quand un lecteur est supprime :
 * un secret orphelin dans le trousseau ne se retrouve jamais. */
void reseau_secret_effacer (const Lecteur *l);
