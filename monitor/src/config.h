#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum { SORT_PID = 0, SORT_NAME, SORT_USER, SORT_STATE, SORT_CPU, SORT_MEM, SORT_THREADS, SORT_COUNT };
enum { CPUVIEW_CORES = 0, CPUVIEW_COMBINED };

struct mconfig {
    char theme[32];
    int interval_ms;
    int history;
    int start_tab;
    bool show_system;
    int sort_column;
    bool sort_desc;
    int cpu_view;
    int units;
    bool smooth;
};

extern struct mconfig g_cfg;

void cfg_defaults(void);
void cfg_dir(char *out, size_t cap);
void cfg_load(void);
void cfg_save(void);
