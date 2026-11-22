#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include "theme.h"
#include "util.h"
#include "config.h"
#include "pg/pg.h"

#define SYSTEM_THEME_DIR "/usr/share/monitor/themes"

struct mtheme g_theme;

static struct theme_entry entries[THEME_MAX];
static int entry_count;

static const struct mtheme builtin = {
    .name = "Default",
    .light = false,
    .background = 0x14161C,
    .surface = 0x1D2028,
    .surface2 = 0x262A35,
    .border = 0x323744,
    .foreground = 0xE6E8EE,
    .foreground_dim = 0x8D93A5,
    .accent = 0x5B9DFF,
    .selection = 0x2C4A7A,
    .selection_text = 0xFFFFFF,
    .warning = 0xF2B84B,
    .danger = 0xF0657A,
    .success = 0x5FD08B,
    .cpu = {0x3DDC97, 0xF2C94C, 0xF0657A},
    .mem = {0x5B9DFF, 0x9B7BFF, 0xE56BD0},
    .download = {0x2EC4B6, 0x5B9DFF, 0x9B7BFF},
    .upload = {0xF2B84B, 0xF2813C, 0xF0657A},
};

struct key_def {
    const char *key;
    size_t offset;
};

#define K(name, field) {name, __builtin_offsetof(struct mtheme, field)}

static const struct key_def keys[] = {
    K("background", background), K("surface", surface), K("surface2", surface2),
    K("border", border), K("foreground", foreground), K("foreground_dim", foreground_dim),
    K("accent", accent), K("selection", selection), K("selection_text", selection_text),
    K("warning", warning), K("danger", danger), K("success", success),
    K("cpu_low", cpu.low), K("cpu_mid", cpu.mid), K("cpu_high", cpu.high),
    K("mem_low", mem.low), K("mem_mid", mem.mid), K("mem_high", mem.high),
    K("download_low", download.low), K("download_mid", download.mid), K("download_high", download.high),
    K("upload_low", upload.low), K("upload_mid", upload.mid), K("upload_high", upload.high),
};

void theme_set_default(void)
{
    g_theme = builtin;
}

uint32_t grad_at(const struct grad *g, uint32_t p)
{
    if (p > 1000)
        p = 1000;
    if (p < 500)
        return pgc_mix(g->low, g->mid, p * 2 * 255 / 1000);
    return pgc_mix(g->mid, g->high, (p - 500) * 2 * 255 / 1000);
}

static bool load_file(const char *path, struct mtheme *out)
{
    static char text[4096];
    if (!u_read_file(path, text, sizeof(text)))
        return false;
    *out = builtin;
    out->name[0] = 0;
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
        if (!*line || *line == '#' || *line == ';')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = u_trim(line), *val = u_trim(eq + 1);
        if (!strcmp(key, "name")) {
            u_copy(out->name, sizeof(out->name), val);
            continue;
        }
        if (!strcmp(key, "light")) {
            out->light = !strcmp(val, "true") || !strcmp(val, "1") || !strcmp(val, "yes");
            continue;
        }
        bool ok;
        uint32_t color = u_parse_color(val, &ok);
        if (!ok)
            continue;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (!strcmp(keys[i].key, key))
                *(uint32_t *)((char *)out + keys[i].offset) = color;
    }
    return true;
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && entry_count < THEME_MAX) {
        size_t n = strlen(e->d_name);
        if (n < 7 || strcmp(e->d_name + n - 6, ".theme"))
            continue;
        struct theme_entry *t = &entries[entry_count];
        snprintf(t->path, sizeof(t->path), "%s/%s", dir, e->d_name);
        struct mtheme tmp;
        if (!load_file(t->path, &tmp))
            continue;
        if (tmp.name[0])
            u_copy(t->name, sizeof(t->name), tmp.name);
        else {
            u_copy(t->name, sizeof(t->name), e->d_name);
            t->name[n - 6] = 0;
        }
        bool dup = false;
        for (int i = 0; i < entry_count; i++)
            if (!strcmp(entries[i].name, t->name))
                dup = true;
        if (!dup)
            entry_count++;
    }
    closedir(d);
}

void theme_scan(void)
{
    entry_count = 0;
    u_copy(entries[0].name, sizeof(entries[0].name), builtin.name);
    entries[0].path[0] = 0;
    entry_count = 1;
    char user[160];
    cfg_dir(user, sizeof(user));
    char sub[200];
    scan_dir(SYSTEM_THEME_DIR);
    snprintf(sub, sizeof(sub), "%s/themes", user);
    scan_dir(sub);
}

int theme_count(void)
{
    return entry_count;
}

const char *theme_entry_name(int index)
{
    return index >= 0 && index < entry_count ? entries[index].name : "";
}

int theme_find(const char *name)
{
    for (int i = 0; i < entry_count; i++)
        if (!strcmp(entries[i].name, name))
            return i;
    return -1;
}

static void sync_toolkit_theme(void)
{
    unsigned n = pg_theme_count();
    for (unsigned i = 0; i < n; i++) {
        struct pg_theme t;
        const char *name = pg_theme_name(i);
        if (name && pg_theme_preview(name, &t) && t.light == g_theme.light) {
            pg_theme_set(name);
            return;
        }
    }
}

bool theme_apply(const char *name)
{
    int i = theme_find(name);
    if (i < 0) {
        theme_set_default();
        sync_toolkit_theme();
        return false;
    }
    if (!entries[i].path[0])
        g_theme = builtin;
    else if (!load_file(entries[i].path, &g_theme))
        g_theme = builtin;
    u_copy(g_theme.name, sizeof(g_theme.name), entries[i].name);
    sync_toolkit_theme();
    return i >= 0;
}
