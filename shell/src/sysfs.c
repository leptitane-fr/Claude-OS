#include "sysfs.h"

char *
shell_sysfs_read (const char *dir, const char *file)
{
    g_autofree char *path = g_build_filename (dir, file, NULL);
    char *content = NULL;
    if (!g_file_get_contents (path, &content, NULL, NULL))
        return NULL;
    return g_strstrip (content);
}

char *
shell_battery_dir (void)
{
    const char *base = "/sys/class/power_supply";
    g_autoptr(GDir) dir = g_dir_open (base, 0, NULL);
    if (dir == NULL)
        return NULL;

    const char *name;
    while ((name = g_dir_read_name (dir)) != NULL) {
        if (!g_str_has_prefix (name, "BAT"))
            continue;
        return g_build_filename (base, name, NULL);
    }
    return NULL;
}

gboolean
shell_sur_secteur (void)
{
    static const char *base = "/sys/class/power_supply";
    g_autoptr(GDir) d = g_dir_open (base, 0, NULL);
    if (d == NULL)
        return TRUE;          /* voir sysfs.h : le doute profite au secteur */

    gboolean vu_une_source = FALSE;
    const char *nom;
    while ((nom = g_dir_read_name (d)) != NULL) {
        g_autofree char *dir  = g_build_filename (base, nom, NULL);
        g_autofree char *type = shell_sysfs_read (dir, "type");
        if (type == NULL)
            continue;
        if (g_strcmp0 (type, "Mains") != 0 && g_strcmp0 (type, "USB") != 0)
            continue;         /* Battery : ce n'est pas une source externe */

        g_autofree char *online = shell_sysfs_read (dir, "online");
        if (online == NULL)
            continue;
        vu_une_source = TRUE;
        if (g_strcmp0 (online, "1") == 0)
            return TRUE;
    }
    /* Des sources existent et toutes disent « 0 » : nous sommes sur batterie.
     * Aucune source du tout : machine fixe, ou noyau muet -- secteur. */
    return !vu_une_source;
}
