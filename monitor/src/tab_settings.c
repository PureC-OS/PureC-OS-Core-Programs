#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tabs.h"
#include "ui.h"
#include "config.h"
#include "theme.h"
#include "util.h"

static struct {
    struct pg_widget *theme, *interval, *history, *history_label, *start, *show_sys, *sort_col, *sort_order,
        *cpu_view, *units, *smooth, *themes_hint;
    const char *theme_names[THEME_MAX];
    bool building;
} st;

static const int interval_values[] = {250, 500, 1000, 2000, 3000, 5000};
static const char *const interval_labels[] = {"250 ms", "500 ms", "1 second", "2 seconds", "3 seconds", "5 seconds"};
static const char *const tab_labels[] = {"Overview", "Processes", "Resources", "Network", "Disks", "Settings"};
static const char *const sort_labels[] = {"PID", "Name", "User", "State", "CPU", "Memory", "Threads"};
static const char *const unit_labels[] = {"Automatic", "KiB", "MiB"};
static const char *const order_labels[] = {"Descending", "Ascending"};
static const char *const view_labels[] = {"Per core", "Combined"};

static struct pg_widget *row(const char *title, const char *hint, struct pg_widget *control)
{
    struct pg_widget *r = pg_hbox(16);
    pg_box_set_padding(r, 0, 8, 0, 8);
    struct pg_widget *texts = pg_vbox(2);
    pg_widget_set_expand(texts, true, false);
    pg_widget_set_align(texts, PG_ALIGN_FILL, PG_ALIGN_CENTER);
    pg_widget_add(texts, pg_label_new_font(title, PGF_BODY, PG_COLOR_TEXT));
    if (hint) {
        struct pg_widget *h = pg_label_new_font(hint, PGF_SMALL, PG_COLOR_TEXT3);
        pg_label_set_wrap(h, true);
        pg_widget_add(texts, h);
    }
    pg_widget_add(r, texts);
    pg_widget_set_align(control, PG_ALIGN_END, PG_ALIGN_CENTER);
    pg_widget_add(r, control);
    return r;
}

static void fill_themes(void)
{
    int n = theme_count();
    for (int i = 0; i < n; i++)
        st.theme_names[i] = theme_entry_name(i);
    pg_dropdown_set_items(st.theme, st.theme_names, n);
    int cur = theme_find(g_cfg.theme);
    pg_dropdown_set_selected(st.theme, cur < 0 ? 0 : cur);
}

static void history_text(void)
{
    char t[40];
    snprintf(t, sizeof(t), "%d samples", g_cfg.history);
    pg_label_set_text(st.history_label, t);
}

void tab_settings_sync(void)
{
    st.building = true;
    fill_themes();
    int ii = 2;
    for (int i = 0; i < 6; i++)
        if (interval_values[i] == g_cfg.interval_ms)
            ii = i;
    pg_dropdown_set_selected(st.interval, ii);
    pg_slider_set_value(st.history, g_cfg.history);
    history_text();
    pg_dropdown_set_selected(st.start, g_cfg.start_tab);
    pg_toggle_set_active(st.show_sys, g_cfg.show_system);
    pg_dropdown_set_selected(st.sort_col, g_cfg.sort_column);
    pg_segmented_set_selected(st.sort_order, g_cfg.sort_desc ? 0 : 1);
    pg_segmented_set_selected(st.cpu_view, g_cfg.cpu_view);
    pg_dropdown_set_selected(st.units, g_cfg.units);
    pg_toggle_set_active(st.smooth, g_cfg.smooth);
    st.building = false;
}

static void changed(struct pg_widget *w, void *data)
{
    (void)data;
    if (st.building)
        return;
    int what = CHANGE_OTHER;
    if (w == st.theme) {
        int i = pg_dropdown_selected(w);
        if (i >= 0)
            u_copy(g_cfg.theme, sizeof(g_cfg.theme), theme_entry_name(i));
        what = CHANGE_THEME;
    } else if (w == st.interval) {
        int i = pg_dropdown_selected(w);
        if (i >= 0 && i < 6)
            g_cfg.interval_ms = interval_values[i];
        what = CHANGE_INTERVAL;
    } else if (w == st.history) {
        int v = pg_slider_value(w);
        g_cfg.history = (v + 5) / 10 * 10;
        if (g_cfg.history < 30)
            g_cfg.history = 30;
        history_text();
        what = CHANGE_HISTORY;
    } else if (w == st.start)
        g_cfg.start_tab = pg_dropdown_selected(w);
    else if (w == st.show_sys) {
        g_cfg.show_system = pg_toggle_active(w);
        what = CHANGE_FILTER;
    } else if (w == st.sort_col) {
        g_cfg.sort_column = pg_dropdown_selected(w);
        what = CHANGE_SORT;
    } else if (w == st.sort_order) {
        g_cfg.sort_desc = pg_segmented_selected(w) == 0;
        what = CHANGE_SORT;
    } else if (w == st.cpu_view)
        g_cfg.cpu_view = pg_segmented_selected(w);
    else if (w == st.units)
        g_cfg.units = pg_dropdown_selected(w);
    else if (w == st.smooth)
        g_cfg.smooth = pg_toggle_active(w);
    app_config_changed(what);
}

static void rescan(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    theme_scan();
    st.building = true;
    fill_themes();
    st.building = false;
    ui_toast("Theme list refreshed", g_theme.success);
}

static struct pg_widget *bind(struct pg_widget *w)
{
    pg_widget_on(w, PG_SIGNAL_CHANGED, changed, NULL);
    return w;
}

struct pg_widget *tab_settings_new(void)
{
    struct pg_widget *scroll = pg_scroll_new();
    pg_widget_set_expand(scroll, true, true);
    struct pg_widget *page = pg_vbox(14);
    pg_box_set_padding(page, 24, 20, 24, 24);
    struct pg_widget *title = pg_label_new_font("Settings", PGF_HEAD, PG_COLOR_TEXT);
    pg_widget_add(page, title);

    struct pg_widget *ap = pg_card_new("Appearance", "Colors of graphs and the monitor", PG_NO_ICON);
    st.theme = bind(pg_dropdown_new((const char *const[]){"Default"}, 1));
    pg_widget_set_size(st.theme, 220, -1);
    struct pg_widget *tb = pg_hbox(8);
    struct pg_widget *rs = pg_button_new("Rescan", PG_NO_ICON, PG_BUTTON_GHOST);
    pg_widget_on(rs, PG_SIGNAL_CLICKED, rescan, NULL);
    pg_widget_add(tb, rs);
    pg_widget_add(tb, st.theme);
    pg_widget_add(ap, row("Theme", "Files in /usr/share/monitor/themes and ~/.config/monitor/themes", tb));
    st.smooth = bind(pg_toggle_new(true));
    pg_widget_add(ap, row("Smooth graph scrolling", "Slide the graphs between samples", st.smooth));
    st.units = bind(pg_dropdown_new(unit_labels, 3));
    pg_widget_set_size(st.units, 180, -1);
    pg_widget_add(ap, row("Memory units", "How sizes are shown", st.units));
    pg_widget_add(page, ap);

    struct pg_widget *up = pg_card_new("Updates", "How often and how much history is kept", PG_NO_ICON);
    st.interval = bind(pg_dropdown_new(interval_labels, 6));
    pg_widget_set_size(st.interval, 180, -1);
    pg_widget_add(up, row("Update interval", "Time between samples", st.interval));
    struct pg_widget *hb = pg_hbox(12);
    st.history = bind(pg_slider_new(30, 600, 120));
    pg_widget_set_size(st.history, 220, -1);
    st.history_label = pg_label_new_font("", PGF_SMALL, PG_COLOR_TEXT2);
    pg_widget_set_size(st.history_label, 90, -1);
    pg_widget_add(hb, st.history);
    pg_widget_add(hb, st.history_label);
    pg_widget_add(up, row("Graph history", "Number of samples drawn in graphs", hb));
    st.start = bind(pg_dropdown_new(tab_labels, 6));
    pg_widget_set_size(st.start, 180, -1);
    pg_widget_add(up, row("Start tab", "Tab shown when the monitor opens", st.start));
    pg_widget_add(page, up);

    struct pg_widget *pr = pg_card_new("Processes", "List behaviour", PG_NO_ICON);
    st.show_sys = bind(pg_toggle_new(true));
    pg_widget_add(pr, row("Show system processes", "Processes owned by root and the kernel", st.show_sys));
    st.sort_col = bind(pg_dropdown_new(sort_labels, 7));
    pg_widget_set_size(st.sort_col, 180, -1);
    pg_widget_add(pr, row("Sort by", NULL, st.sort_col));
    st.sort_order = bind(pg_segmented_new(order_labels, 2));
    pg_widget_add(pr, row("Sort order", NULL, st.sort_order));
    pg_widget_add(page, pr);

    struct pg_widget *re = pg_card_new("Resources", "CPU display", PG_NO_ICON);
    st.cpu_view = bind(pg_segmented_new(view_labels, 2));
    pg_widget_add(re, row("CPU view", "Graph per core or one combined graph with a core heat map", st.cpu_view));
    pg_widget_add(page, re);

    pg_widget_add(scroll, page);
    tab_settings_sync();
    return scroll;
}
