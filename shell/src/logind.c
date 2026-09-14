/* Les inhibiteurs et les methodes de logind — voir logind.h. */

#include "logind.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#define LOGIND_NOM    "org.freedesktop.login1"
#define LOGIND_CHEMIN "/org/freedesktop/login1"
#define LOGIND_IFACE  "org.freedesktop.login1.Manager"

int
shell_logind_inhiber (const char *quoi, const char *pourquoi, const char *mode)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) bus =
        g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (bus == NULL) {
        g_warning ("logind : bus systeme injoignable — %s", err->message);
        return -1;
    }

    g_autoptr(GUnixFDList) recu = NULL;
    g_autoptr(GVariant) rep = g_dbus_connection_call_with_unix_fd_list_sync (
        bus, LOGIND_NOM, LOGIND_CHEMIN, LOGIND_IFACE, "Inhibit",
        g_variant_new ("(ssss)", quoi, "Claude OS", pourquoi, mode),
        G_VARIANT_TYPE ("(h)"), G_DBUS_CALL_FLAGS_NONE, -1,
        NULL, &recu, NULL, &err);

    if (rep == NULL) {
        g_warning ("logind : inhibiteur « %s » refuse — %s", quoi, err->message);
        return -1;
    }

    gint32 indice = -1;
    g_variant_get (rep, "(h)", &indice);

    int fd = g_unix_fd_list_get (recu, indice, &err);
    if (fd < 0) {
        g_warning ("logind : descripteur d'inhibiteur illisible — %s",
                   err->message);
        return -1;
    }
    return fd;
}

gboolean
shell_logind_sait_faire (const char *methode)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) bus =
        g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (bus == NULL)
        return FALSE;

    g_autofree char *question = g_strconcat ("Can", methode, NULL);
    g_autoptr(GVariant) rep = g_dbus_connection_call_sync (
        bus, LOGIND_NOM, LOGIND_CHEMIN, LOGIND_IFACE, question, NULL,
        G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    if (rep == NULL) {
        g_message ("logind : %s sans reponse — %s", question, err->message);
        return FALSE;
    }

    const char *r = NULL;
    g_variant_get (rep, "(&s)", &r);
    return g_strcmp0 (r, "yes") == 0;
}

void
shell_logind_appeler (const char *methode)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) bus =
        g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (bus == NULL) {
        g_warning ("logind : bus systeme injoignable — %s", err->message);
        return;
    }

    /* FALSE : on ne force pas. logind interroge les inhibiteurs, et une
     * application qui a demande a ne pas etre interrompue l'emporte. */
    g_dbus_connection_call (bus, LOGIND_NOM, LOGIND_CHEMIN, LOGIND_IFACE,
                            methode, g_variant_new ("(b)", FALSE),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}
