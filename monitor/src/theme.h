#pragma once
#include <stdbool.h>
#include <stdint.h>

#define THEME_MAX 48

struct grad {
    uint32_t low, mid, high;
};

struct mtheme {
    char name[32];
    bool light;
    uint32_t background, surface, surface2, border;
    uint32_t foreground, foreground_dim;
    uint32_t accent, selection, selection_text;
    uint32_t warning, danger, success;
    struct grad cpu, mem, download, upload;
};

struct theme_entry {
    char name[32];
    char path[160];
};

extern struct mtheme g_theme;

void theme_scan(void);
int theme_count(void);
const char *theme_entry_name(int index);
int theme_find(const char *name);
bool theme_apply(const char *name);
void theme_set_default(void);
uint32_t grad_at(const struct grad *g, uint32_t permille);
