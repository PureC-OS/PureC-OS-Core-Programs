#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/stat.h>
#include "config.h"
#include "util.h"

struct mconfig g_cfg;

static const char *const sort_names[SORT_COUNT] = {"pid", "name", "user", "state", "cpu", "memory", "threads"};
static const char *const unit_names[3] = {"auto", "kib", "mib"};
static const char *const tab_names[6] = {"overview", "processes", "resources", "network", "disks", "settings"};

void cfg_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    u_copy(g_cfg.theme, sizeof(g_cfg.theme), "Default");
    g_cfg.interval_ms = 1000;
    g_cfg.history = 120;
    g_cfg.start_tab = 0;
    g_cfg.show_system = true;
    g_cfg.sort_column = SORT_CPU;
    g_cfg.sort_desc = true;
    g_cfg.cpu_view = CPUVIEW_CORES;
    g_cfg.units = UNITS_AUTO;
    g_cfg.smooth = true;
}

void cfg_dir(char *out, size_t cap)
{
    const char *home = getenv("HOME");
    char tmp[96] = "";
    if (!home || !*home) {
        struct passwd *pw = getpwuid((uid_t)getuid());
        if (pw && pw->pw_dir && *pw->pw_dir)
            home = pw->pw_dir;
        else
            home = getuid() == 0 ? "/root" : "/";
    }
    u_copy(tmp, sizeof(tmp), home);
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/')
        tmp[--n] = 0;
    if (!strcmp(tmp, "/"))
        tmp[0] = 0;
    snprintf(out, cap, "%s/.config/monitor", tmp);
}

static int index_of(const char *const *names, int count, const char *v, int fallback)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(names[i], v))
            return i;
    return fallback;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void cfg_load(void)
{
    cfg_defaults();
    char dir[160], path[200];
    static char text[2048];
    cfg_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/monitor.conf", dir);
    if (!u_read_file(path, text, sizeof(text)))
        return;
    char *save = text;
    while (save && *save) {
        char *line = save;
        char *nl = strchr(save, '\n');
        if (nl) {
            *nl = 0;
            save = nl + 1;
        } else
            save = NULL;
        line = u_trim(line);
        if (!*line || *line == '#')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = u_trim(line), *val = u_trim(eq + 1);
        bool yes = !strcmp(val, "true") || !strcmp(val, "yes") || !strcmp(val, "1");
        if (!strcmp(key, "theme"))
            u_copy(g_cfg.theme, sizeof(g_cfg.theme), val);
        else if (!strcmp(key, "interval_ms"))
            g_cfg.interval_ms = clampi(atoi(val), 250, 5000);
        else if (!strcmp(key, "history"))
            g_cfg.history = clampi(atoi(val), 30, 600);
        else if (!strcmp(key, "start_tab"))
            g_cfg.start_tab = index_of(tab_names, 6, val, 0);
        else if (!strcmp(key, "show_system_processes"))
            g_cfg.show_system = yes;
        else if (!strcmp(key, "sort_column"))
            g_cfg.sort_column = index_of(sort_names, SORT_COUNT, val, SORT_CPU);
        else if (!strcmp(key, "sort_order"))
            g_cfg.sort_desc = strcmp(val, "asc") != 0;
        else if (!strcmp(key, "cpu_view"))
            g_cfg.cpu_view = !strcmp(val, "combined") ? CPUVIEW_COMBINED : CPUVIEW_CORES;
        else if (!strcmp(key, "units"))
            g_cfg.units = index_of(unit_names, 3, val, UNITS_AUTO);
        else if (!strcmp(key, "smooth"))
            g_cfg.smooth = yes;
    }
}

void cfg_save(void)
{
    char dir[160], parent[160], path[200];
    cfg_dir(dir, sizeof(dir));
    u_copy(parent, sizeof(parent), dir);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = 0;
        if (parent[0])
            mkdir(parent, 0755);
    }
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/monitor.conf", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "theme = %s\n", g_cfg.theme);
    fprintf(f, "interval_ms = %d\n", g_cfg.interval_ms);
    fprintf(f, "history = %d\n", g_cfg.history);
    fprintf(f, "start_tab = %s\n", tab_names[g_cfg.start_tab]);
    fprintf(f, "show_system_processes = %s\n", g_cfg.show_system ? "true" : "false");
    fprintf(f, "sort_column = %s\n", sort_names[g_cfg.sort_column]);
    fprintf(f, "sort_order = %s\n", g_cfg.sort_desc ? "desc" : "asc");
    fprintf(f, "cpu_view = %s\n", g_cfg.cpu_view == CPUVIEW_COMBINED ? "combined" : "cores");
    fprintf(f, "units = %s\n", unit_names[g_cfg.units]);
    fprintf(f, "smooth = %s\n", g_cfg.smooth ? "true" : "false");
    fclose(f);
}
