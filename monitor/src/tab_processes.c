#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <errno.h>
#include <signal.h>
#include "tabs.h"
#include "ui.h"
#include "config.h"
#include "util.h"

#define ROW_H 28
#define HEAD_H 32

static struct {
    struct pg_widget *table, *entry, *count, *end_btn, *kill_btn;
    int view[MAX_PROCS];
    int nview;
    int scroll;
    uint32_t selected;
    int hover;
    int pending_sig;
    uint32_t pending_pid;
    char pending_name[40];
} pt;

static const char *const col_names[SORT_COUNT] = {"PID", "Name", "User", "State", "CPU", "Memory", "Threads"};
static const int col_width[SORT_COUNT] = {72, 0, 110, 84, 84, 110, 84};

static int col_x[SORT_COUNT + 1];

static void layout_cols(int x, int w)
{
    int fixed = 0;
    for (int i = 0; i < SORT_COUNT; i++)
        fixed += col_width[i];
    int name_w = w - fixed - 24;
    if (name_w < 140)
        name_w = 140;
    int cx = x + 12;
    for (int i = 0; i < SORT_COUNT; i++) {
        col_x[i] = cx;
        cx += i == SORT_NAME ? name_w : col_width[i];
    }
    col_x[SORT_COUNT] = cx;
}

static int cmp(const void *pa, const void *pb)
{
    const struct proc_info *a = &g_sys.procs[*(const int *)pa], *b = &g_sys.procs[*(const int *)pb];
    int r = 0;
    switch (g_cfg.sort_column) {
    case SORT_PID: r = a->pid < b->pid ? -1 : a->pid > b->pid; break;
    case SORT_NAME: r = strcasecmp(a->name, b->name); break;
    case SORT_USER: r = strcasecmp(a->user, b->user); break;
    case SORT_STATE: r = strcmp(a->state, b->state); break;
    case SORT_CPU: r = a->cpu_pm < b->cpu_pm ? -1 : a->cpu_pm > b->cpu_pm; break;
    case SORT_MEM: r = a->rss_kb < b->rss_kb ? -1 : a->rss_kb > b->rss_kb; break;
    case SORT_THREADS: r = a->threads < b->threads ? -1 : a->threads > b->threads; break;
    }
    if (g_cfg.sort_desc)
        r = -r;
    if (!r)
        r = a->pid < b->pid ? -1 : a->pid > b->pid;
    return r;
}

static int view_row_of(uint32_t pid)
{
    for (int i = 0; i < pt.nview; i++)
        if (g_sys.procs[pt.view[i]].pid == pid)
            return i;
    return -1;
}

static void clamp_scroll(void)
{
    int visible = pt.table ? pt.table->rect.h - HEAD_H : 0;
    int max = pt.nview * ROW_H - visible;
    if (max < 0)
        max = 0;
    if (pt.scroll > max)
        pt.scroll = max;
    if (pt.scroll < 0)
        pt.scroll = 0;
}

static void update_buttons(void)
{
    const struct proc_info *p = pt.selected ? sys_find_proc(pt.selected) : NULL;
    bool ok = p && p->pid > 1 && !p->kernel && (!p->locked || g_sys.is_root);
    pg_widget_set_enabled(pt.end_btn, ok);
    pg_widget_set_enabled(pt.kill_btn, ok);
    char text[48];
    snprintf(text, sizeof(text), "%d of %d processes", pt.nview, g_sys.nprocs);
    pg_label_set_text(pt.count, text);
}

void tab_processes_refresh(void)
{
    const char *filter = pg_entry_text(pt.entry);
    pt.nview = 0;
    for (int i = 0; i < g_sys.nprocs; i++) {
        const struct proc_info *p = &g_sys.procs[i];
        if (!g_cfg.show_system && p->locked)
            continue;
        if (filter && *filter) {
            char pid[16];
            snprintf(pid, sizeof(pid), "%u", p->pid);
            if (!u_contains_nocase(p->name, filter) && !u_contains_nocase(p->user, filter) && !strstr(pid, filter))
                continue;
        }
        pt.view[pt.nview++] = i;
    }
    qsort(pt.view, (size_t)pt.nview, sizeof(int), cmp);
    if (pt.selected && !sys_find_proc(pt.selected))
        pt.selected = 0;
    clamp_scroll();
    update_buttons();
    pg_widget_queue_draw(pt.table);
}

static void draw_chevron(struct pg_canvas *c, int cx, int cy, bool down, uint32_t color)
{
    float d = down ? 1.0f : -1.0f;
    pgc_line(c, (float)cx - 4, (float)cy - 2 * d, (float)cx, (float)cy + 2 * d, 1.6f, color);
    pgc_line(c, (float)cx, (float)cy + 2 * d, (float)cx + 4, (float)cy - 2 * d, 1.6f, color);
}

static void draw_table(struct pg_widget *w, struct pg_canvas *c, void *data)
{
    (void)data;
    struct pg_rect r = w->rect;
    pgc_fill(c, r.x, r.y, r.w, r.h, g_theme.background);
    layout_cols(r.x, r.w);
    pgc_fill(c, r.x, r.y, r.w, HEAD_H, g_theme.surface);
    pgc_fill(c, r.x, r.y + HEAD_H - 1, r.w, 1, g_theme.border);
    int fh = pgc_font_h(PGF_SMALL);
    for (int i = 0; i < SORT_COUNT; i++) {
        bool active = i == g_cfg.sort_column;
        uint32_t col = active ? g_theme.accent : g_theme.foreground_dim;
        bool right = i == SORT_CPU || i == SORT_MEM || i == SORT_THREADS;
        int cw = col_x[i + 1] - col_x[i] - 12;
        int tw = pgc_text_w(PGF_SMALL, col_names[i]);
        int tx = right ? col_x[i] + cw - tw - (active ? 14 : 0) : col_x[i];
        pgc_text(c, PGF_SMALL, tx, r.y + (HEAD_H - fh) / 2, col_names[i], col);
        if (active)
            draw_chevron(c, tx + tw + 9, r.y + HEAD_H / 2, g_cfg.sort_desc, col);
    }
    struct pg_clip saved = pgc_clip_push(c, r.x, r.y + HEAD_H, r.w, r.h - HEAD_H);
    int first = pt.scroll / ROW_H;
    int bh = pgc_font_h(PGF_BODY);
    for (int row = first; row < pt.nview; row++) {
        int ry = r.y + HEAD_H + row * ROW_H - pt.scroll;
        if (ry > r.y + r.h)
            break;
        const struct proc_info *p = &g_sys.procs[pt.view[row]];
        bool sel = p->pid == pt.selected;
        if (sel)
            pgc_fill(c, r.x, ry, r.w, ROW_H, g_theme.selection);
        else if (row == pt.hover)
            pgc_fill(c, r.x, ry, r.w, ROW_H, g_theme.surface2);
        else if (row & 1)
            pgc_fill_a(c, r.x, ry, r.w, ROW_H, g_theme.surface, 150);
        uint32_t tc = sel ? g_theme.selection_text : g_theme.foreground;
        uint32_t dim = sel ? g_theme.selection_text : g_theme.foreground_dim;
        int ty = ry + (ROW_H - bh) / 2;
        char buf[40];
        snprintf(buf, sizeof(buf), "%u", p->pid);
        pgc_text(c, PGF_BODY, col_x[SORT_PID], ty, buf, dim);
        int nx = col_x[SORT_NAME];
        if (p->locked) {
            int ip = pgc_icon_px(PG_ICON_S);
            pgc_icon(c, PGI_LOCK, PG_ICON_S, nx, ry + (ROW_H - ip) / 2, g_theme.warning);
            nx += ip + 6;
        }
        pgc_text_fit(c, PGF_BODY, nx, ty, col_x[SORT_USER] - nx - 10, p->name, tc);
        pgc_text_fit(c, PGF_BODY, col_x[SORT_USER], ty, col_x[SORT_STATE] - col_x[SORT_USER] - 10, p->user, dim);
        pgc_text_fit(c, PGF_BODY, col_x[SORT_STATE], ty, col_x[SORT_CPU] - col_x[SORT_STATE] - 10, p->state, dim);
        int cw;
        u_percent(buf, sizeof(buf), p->cpu_pm);
        cw = col_x[SORT_CPU + 1] - col_x[SORT_CPU] - 12;
        ui_text_right(c, PGF_BODY, col_x[SORT_CPU] + cw, ty, buf,
                      sel ? tc : grad_at(&g_theme.cpu, p->cpu_pm * 5 > 1000 ? 1000 : p->cpu_pm * 5));
        u_bytes(buf, sizeof(buf), p->rss_kb * 1024, g_cfg.units);
        cw = col_x[SORT_MEM + 1] - col_x[SORT_MEM] - 12;
        ui_text_right(c, PGF_BODY, col_x[SORT_MEM] + cw, ty, buf, tc);
        snprintf(buf, sizeof(buf), "%u", p->threads);
        cw = col_x[SORT_THREADS + 1] - col_x[SORT_THREADS] - 12;
        ui_text_right(c, PGF_BODY, col_x[SORT_THREADS] + cw, ty, buf, dim);
    }
    pgc_clip_pop(c, saved);
    int total = pt.nview * ROW_H, vis = r.h - HEAD_H;
    if (total > vis) {
        int bar_h = vis * vis / total;
        if (bar_h < 24)
            bar_h = 24;
        int by = r.y + HEAD_H + (vis - bar_h) * pt.scroll / (total - vis);
        pgc_rrect(c, r.x + r.w - 8, by, 5, bar_h, 2, g_theme.border, 255);
    }
    if (!pt.nview)
        pgc_text_center(c, PGF_BODY, r.x, r.y + HEAD_H + 30, r.w, "No matching processes", g_theme.foreground_dim);
}

static void reveal(int row)
{
    int visible = pt.table->rect.h - HEAD_H;
    if (row * ROW_H < pt.scroll)
        pt.scroll = row * ROW_H;
    else if ((row + 1) * ROW_H > pt.scroll + visible)
        pt.scroll = (row + 1) * ROW_H - visible;
    clamp_scroll();
}

static void select_row(int row)
{
    if (row < 0 || row >= pt.nview)
        return;
    pt.selected = g_sys.procs[pt.view[row]].pid;
    reveal(row);
    update_buttons();
    pg_widget_queue_draw(pt.table);
}

static bool table_input(struct pg_widget *w, const struct pg_event *e, void *data)
{
    (void)data;
    struct pg_rect r = w->rect;
    if (e->type == PG_EVENT_POINTER_MOVE || e->type == PG_EVENT_ENTER) {
        int row = e->y >= r.y + HEAD_H ? (e->y - r.y - HEAD_H + pt.scroll) / ROW_H : -1;
        if (row >= pt.nview)
            row = -1;
        if (row != pt.hover) {
            pt.hover = row;
            pg_widget_queue_draw(w);
        }
        return false;
    }
    if (e->type == PG_EVENT_LEAVE) {
        pt.hover = -1;
        pg_widget_queue_draw(w);
        return false;
    }
    if (e->type == PG_EVENT_WHEEL) {
        pt.scroll -= e->wheel * ROW_H * 3;
        clamp_scroll();
        pg_widget_queue_draw(w);
        return true;
    }
    if (e->type == PG_EVENT_POINTER_DOWN && e->button == PG_BUTTON_LEFT) {
        pg_widget_grab_focus(w);
        if (e->y < r.y + HEAD_H) {
            for (int i = 0; i < SORT_COUNT; i++)
                if (e->x >= col_x[i] - 6 && e->x < col_x[i + 1] - 6) {
                    if (g_cfg.sort_column == i)
                        g_cfg.sort_desc = !g_cfg.sort_desc;
                    else {
                        g_cfg.sort_column = i;
                        g_cfg.sort_desc = i == SORT_CPU || i == SORT_MEM || i == SORT_THREADS;
                    }
                    app_config_changed(CHANGE_SORT);
                    break;
                }
            return true;
        }
        select_row((e->y - r.y - HEAD_H + pt.scroll) / ROW_H);
        return true;
    }
    if (e->type == PG_EVENT_KEY_DOWN) {
        int cur = view_row_of(pt.selected);
        int page = (r.h - HEAD_H) / ROW_H;
        if (e->key == PG_KEY_DOWN)
            select_row(cur < 0 ? 0 : cur + 1);
        else if (e->key == PG_KEY_UP)
            select_row(cur < 0 ? 0 : cur - 1);
        else if (e->key == PG_KEY_PAGE_DOWN)
            select_row(cur + page < pt.nview ? cur + page : pt.nview - 1);
        else if (e->key == PG_KEY_PAGE_UP)
            select_row(cur - page > 0 ? cur - page : 0);
        else if (e->key == PG_KEY_HOME)
            select_row(0);
        else if (e->key == PG_KEY_END)
            select_row(pt.nview - 1);
        else
            return false;
        return true;
    }
    return false;
}

static void perform(void *data)
{
    (void)data;
    int sig = pt.pending_sig;
    const char *sname = sig == SIGKILL ? "SIGKILL" : "SIGTERM";
    char msg[160];
    if (kill((int)pt.pending_pid, sig) == 0) {
        snprintf(msg, sizeof(msg), "Sent %s to %s (PID %u)", sname, pt.pending_name, pt.pending_pid);
        ui_toast(msg, g_theme.success);
    } else {
        int err = errno;
        const char *why = err == EPERM ? "not permitted, the process belongs to another user"
                          : err == ESRCH ? "the process no longer exists"
                                         : strerror(err);
        snprintf(msg, sizeof(msg), "Cannot signal %s (PID %u): %s", pt.pending_name, pt.pending_pid, why);
        ui_toast(msg, g_theme.danger);
    }
}

static void ask(int sig)
{
    const struct proc_info *p = pt.selected ? sys_find_proc(pt.selected) : NULL;
    if (!p || p->pid <= 1 || p->kernel || (p->locked && !g_sys.is_root))
        return;
    pt.pending_sig = sig;
    pt.pending_pid = p->pid;
    u_copy(pt.pending_name, sizeof(pt.pending_name), p->name);
    char title[96], text[200];
    if (sig == SIGKILL) {
        snprintf(title, sizeof(title), "Kill %s?", p->name);
        snprintf(text, sizeof(text),
                 "PID %u will be stopped immediately and cannot clean up. Unsaved data in it will be lost.", p->pid);
    } else {
        snprintf(title, sizeof(title), "End %s?", p->name);
        snprintf(text, sizeof(text), "PID %u will be asked to terminate. It may save its data before it exits.", p->pid);
    }
    ui_confirm(title, text, sig == SIGKILL ? "Kill" : "End process", true, perform, NULL);
}

static void on_end(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    ask(SIGTERM);
}

static void on_kill(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    ask(SIGKILL);
}

static void on_filter(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    pt.scroll = 0;
    tab_processes_refresh();
}

struct pg_widget *tab_processes_new(void)
{
    struct pg_widget *root = pg_vbox(0);
    pg_widget_set_expand(root, true, true);
    pg_box_set_background(root, PG_COLOR_BG, 0);
    struct pg_widget *bar = pg_hbox(10);
    pg_box_set_padding(bar, 16, 12, 16, 10);
    pt.entry = pg_entry_new("Filter by name, user or PID");
    pg_widget_set_size(pt.entry, 300, -1);
    pg_widget_on(pt.entry, PG_SIGNAL_CHANGED, on_filter, NULL);
    pg_widget_add(bar, pt.entry);
    pt.count = pg_label_new_font("", PGF_SMALL, PG_COLOR_TEXT3);
    pg_widget_set_align(pt.count, PG_ALIGN_FILL, PG_ALIGN_CENTER);
    pg_widget_add(bar, pt.count);
    struct pg_widget *spacer = pg_spacer_new();
    pg_widget_set_expand(spacer, true, false);
    pg_widget_add(bar, spacer);
    pt.end_btn = pg_button_new("End process", PG_NO_ICON, PG_BUTTON_SECONDARY);
    pg_widget_on(pt.end_btn, PG_SIGNAL_CLICKED, on_end, NULL);
    pg_widget_add(bar, pt.end_btn);
    pt.kill_btn = pg_button_new("Kill", PG_NO_ICON, PG_BUTTON_DANGER);
    pg_widget_on(pt.kill_btn, PG_SIGNAL_CLICKED, on_kill, NULL);
    pg_widget_add(bar, pt.kill_btn);
    pg_widget_add(root, bar);
    pt.table = pg_drawing_new(200, 200, draw_table, table_input, NULL);
    pg_widget_set_expand(pt.table, true, true);
    pt.hover = -1;
    pg_widget_add(root, pt.table);
    return root;
}
