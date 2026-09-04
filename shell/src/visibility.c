#include "visibility.h"

static struct {
    ShellVisibilityFunc cb;
    gpointer            data;
    gboolean            visible;
} V = { NULL, NULL, TRUE };

void
shell_visibility_init (ShellVisibilityFunc cb, gpointer user_data)
{
    V.cb      = cb;
    V.data    = user_data;
    V.visible = TRUE;
}

void
shell_visibility_toggle (void)
{
    if (V.cb == NULL)
        return;

    V.visible = !V.visible;
    V.cb (V.visible, V.data);
}
