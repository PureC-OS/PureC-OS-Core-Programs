#include <stdio.h>
#include <string.h>
#include "pg/pg.h"
#include "tabs.h"
#include "ui.h"
#include "config.h"
#include "theme.h"
#include "sysdata.h"

static struct {
    struct pg_window *win;
    struct pg_widget *bar, *stack, *tabs[TAB_COUNT];
    uint32_t timer, anim;
    int current;
} app;

static const char *const labels[TAB_COUNT] = {"Overview", "Processes", "Resources", "Network", "Disks", "Settings"};
static const int icons[TAB_COUNT] = {PGI_MONITOR, PGI_CPU_64_BIT, PGI_CHIP, PGI_LAN, PGI_HARDDISK, PGI_COG};

static void redraw_current(void)
{
    if (app.current != TAB_SETTINGS && app.current != TAB_PROCESSES)
        pg_widget_queue_draw(app.tabs[app.current]);
    else if (app.current == TAB_PROCESSES)
        tab_processes_refresh();
}

static bool tick(void *data)
{
    (void)data;
    sys_update();
    tab_processes_refresh();
    if (app.current != TAB_SETTINGS)
        redraw_current();
    return true;
}

static bool animate(void *data)
{
    (void)data;
    if (g_cfg.smooth && app.current != TAB_PROCESSES && app.current != TAB_SETTINGS && app.current != TAB_DISKS)
        pg_widget_queue_draw(app.tabs[app.current]);
    return true;
}

static void restart_timer(void)
{
    if (app.timer)
        pg_timer_remove(app.timer);
    app.timer = pg_timer_add((uint32_t)g_cfg.interval_ms, tick, NULL);
}

static void apply_theme(void)
{
    theme_apply(g_cfg.theme);
    pg_window_set_background(app.win, g_theme.background);
    pg_window_queue_draw(app.win);
}

void app_show_tab(int index)
{
    if (index < 0 || index >= TAB_COUNT)
        return;
    app.current = index;
    pg_stack_set_index(app.stack, index);
    ui_tabbar_select(app.bar, index);
    if (index == TAB_PROCESSES)
        tab_processes_refresh();
    pg_window_queue_draw(app.win);
}

static void tab_changed(int index)
{
    app_show_tab(index);
}

void app_config_changed(int what)
{
    if (what == CHANGE_THEME)
        apply_theme();
    else if (what == CHANGE_INTERVAL)
        restart_timer();
    else if (what == CHANGE_HISTORY)
        sys_set_history(g_cfg.history);
    else if (what == CHANGE_SORT || what == CHANGE_FILTER)
        tab_processes_refresh();
    if (what == CHANGE_SORT)
        tab_settings_sync();
    cfg_save();
    pg_window_queue_draw(app.win);
}

static bool on_key(struct pg_window *win, const struct pg_event *e, void *data)
{
    (void)data;
    if (e->type != PG_EVENT_KEY_DOWN)
        return false;
    if (ui_modal_active())
        return ui_modal_key(e);
    if ((e->mods & PG_MOD_CTRL) && (e->key == PG_KEY_Q || e->key == PG_KEY_W)) {
        pg_window_close(win);
        return true;
    }
    if ((e->mods & PG_MOD_CTRL) && e->key >= 0x1E && e->key < 0x1E + TAB_COUNT) {
        app_show_tab(e->key - 0x1E);
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    if (!pg_init(argc, argv)) {
        fprintf(stderr, "monitor: cannot connect to the display server\n");
        return 1;
    }
    cfg_load();
    theme_scan();
    sys_init(g_cfg.history);
    sys_update();
    app.win = pg_window_new("System Monitor", 1000, 680, PG_WINDOW_RESIZABLE);
    if (!app.win) {
        fprintf(stderr, "monitor: cannot create a window\n");
        return 1;
    }
    pg_window_set_min_size(app.win, 760, 560);
    pg_window_on_key(app.win, on_key, NULL);

    struct pg_widget *body = pg_vbox(0);
    app.bar = ui_tabbar_new(labels, icons, TAB_COUNT, tab_changed);
    pg_widget_add(body, app.bar);
    app.stack = pg_stack_new();
    pg_widget_set_expand(app.stack, true, true);
    app.tabs[TAB_OVERVIEW] = tab_overview_new();
    app.tabs[TAB_PROCESSES] = tab_processes_new();
    app.tabs[TAB_RESOURCES] = tab_resources_new();
    app.tabs[TAB_NETWORK] = tab_network_new();
    app.tabs[TAB_DISKS] = tab_disks_new();
    app.tabs[TAB_SETTINGS] = tab_settings_new();
    for (int i = 0; i < TAB_COUNT; i++)
        pg_widget_add(app.stack, app.tabs[i]);
    pg_widget_add(body, app.stack);
    pg_window_set_child(app.win, ui_overlay_new(body));

    apply_theme();
    app_show_tab(g_cfg.start_tab);
    tab_processes_refresh();
    restart_timer();
    app.anim = pg_timer_add(60, animate, NULL);
    int code = pg_run();
    pg_shutdown();
    return code;
}
