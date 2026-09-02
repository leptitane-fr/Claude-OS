/* =========================================================================
 * Claude-OS Shell — configuration et style, partages
 *
 * Le dock et la barre d'etat doivent lire EXACTEMENT la meme configuration.
 * Quand seul le dock la lisait, un « theme=dark » dans shell.conf donnait un
 * dock sombre et une barre claire cote a cote.
 * ========================================================================= */
#pragma once

#include <gtk/gtk.h>

typedef struct {
    char    **pinned;      /* identifiants .desktop, dans l'ordre d'affichage */
    char     *font;        /* famille de police de l'interface                */
    char     *icon_theme;  /* theme d'icones                                  */
    gboolean  dark;          /* theme sombre                                  */
    gboolean  reserve_space; /* le dock repousse-t-il les fenetres maximisees */
} ShellConfig;

/* Lit ~/.config/claude-os/shell.conf. Absent, les valeurs par defaut
 * s'appliquent : le shell doit fonctionner sans qu'aucun fichier n'ait ete
 * ecrit. Ne renvoie jamais NULL. */
ShellConfig *shell_config_load (void);

/* Charge tokens.css puis shell.css pour l'affichage courant. A brancher sur
 * le signal « startup » de GtkApplication. */
void shell_styles_load (void);

/* Applique le theme d'icones et la police.
 *
 * A appeler AVANT de construire le moindre bouton : le repli d'icone du dock
 * interroge le theme actif, un theme pose apres coup arriverait trop tard.
 *
 * Reappelable : la regle de police est portee par un fournisseur CSS unique,
 * mis a jour plutot qu'empile. Sans cela chaque relecture ajouterait une
 * regle de plus, et l'ancienne police continuerait de peser dans la
 * cascade. */
void shell_config_apply (const ShellConfig *cfg);

/* Ecrit la configuration dans ~/.config/claude-os/shell.conf.
 *
 * Le fichier existant est relu puis modifie cle par cle : commentaires et
 * reglages inconnus de cette version sont preserves. Reecrire le fichier de
 * zero les perdrait a la premiere sauvegarde. */
gboolean shell_config_save (const ShellConfig *cfg, GError **error);

/* Libere une configuration. */
void shell_config_free (ShellConfig *cfg);

/* Appelee quand shell.conf change sur le disque, avec la configuration
 * relue. Elle appartient a l'appelant, qui doit la liberer. */
typedef void (*ShellConfigChangedFunc) (ShellConfig *cfg, gpointer user_data);

/* Surveille shell.conf et previent a chaque modification.
 *
 * C'est ce qui permet au panneau de reglages d'agir sur un dock deja lance :
 * il ecrit le fichier, chaque composant le relit. Aucun protocole a
 * inventer, et la configuration reste la seule source de verite. */
void shell_config_watch (ShellConfigChangedFunc cb, gpointer user_data);
