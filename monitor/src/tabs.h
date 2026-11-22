#pragma once
#include "pg/pg.h"

enum { TAB_OVERVIEW = 0, TAB_PROCESSES, TAB_RESOURCES, TAB_NETWORK, TAB_DISKS, TAB_SETTINGS, TAB_COUNT };
enum { CHANGE_THEME = 1, CHANGE_INTERVAL, CHANGE_HISTORY, CHANGE_SORT, CHANGE_FILTER, CHANGE_OTHER };

struct pg_widget *tab_overview_new(void);
struct pg_widget *tab_processes_new(void);
struct pg_widget *tab_resources_new(void);
struct pg_widget *tab_network_new(void);
struct pg_widget *tab_disks_new(void);
struct pg_widget *tab_settings_new(void);

void tab_processes_refresh(void);
void tab_settings_sync(void);

void app_config_changed(int what);
void app_show_tab(int index);
