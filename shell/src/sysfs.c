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
