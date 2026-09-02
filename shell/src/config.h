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
 * interroge le theme actif, un theme pose apres coup arriverait trop tard. */
void shell_config_apply (const ShellConfig *cfg);
