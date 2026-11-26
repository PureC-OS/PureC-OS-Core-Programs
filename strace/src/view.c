#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include "view.h"
#include "model.h"

#define RH 20
#define HEAD_H 24
#define IND 18
#define ROWCAP 62000
#define SLOW_NS 100000000ULL

enum { VR_PROC = 1, VR_SYS, VR_EXIT };

struct vrow {
    uint8_t kind;
    uint16_t proc;
    int16_t depth;
    uint32_t seq;
};

static struct vrow *rows;
static int nrows;
static int scroll;
static int sel = -1;
static uint8_t sel_kind;
static uint16_t sel_proc;
static uint32_t sel_seq;
static bool autoscroll = true;
static char filter[64];
static struct pg_widget *tree_w, *detail_w, *stats_w;
static bool dragging;
static void (*auto_cb)(bool on);

void view_on_autoscroll(void (*fn)(bool on))
{
    auto_cb = fn;
}

static void stop_auto(void)
{
    if (autoscroll && auto_cb)
        auto_cb(false);
    autoscroll = false;
}

static void add_row(uint8_t kind, int proc, uint32_t seq, int depth)
{
    if (nrows >= ROWCAP)
        return;
    struct vrow *r = &rows[nrows++];
    r->kind = kind;
    r->proc = (uint16_t)proc;
    r->seq = seq;
    r->depth = (int16_t)depth;
}

static const char *strcasestr_local(const char *h, const char *n)
{
    size_t nl = strlen(n);
    for (; *h; h++)
        if (!strncasecmp(h, n, nl))
            return h;
    return NULL;
}

static bool match(const struct item *it)
{
    if (!filter[0])
        return true;
    char tmp[24];
    const char *name = tl_sysname(it->rec.nr, tmp, sizeof(tmp));
    if (strcasestr_local(name, filter))
        return true;
    if ((it->rec.flags & (TRACE_REC_STR1 | TRACE_REC_STR2)) && strcasestr_local(it->rec.str, filter))
        return true;
    return false;
}

static void walk(int pi, int depth, int guard)
{
    if (nrows >= ROWCAP - 1 || guard > 48)
        return;
    add_row(VR_PROC, pi, 0, depth);
    struct proc *p = &P[pi];
    if (!p->expanded)
        return;
    uint32_t s = p->first;
    int budget = ROWCAP;
    while (s && budget-- > 0) {
        struct item *it = item_at(s);
        if (!it)
            break;
        if (it->kind == IT_SYS) {
            if (match(it))
                add_row(VR_SYS, pi, s, depth + 1);
        } else if (it->kind == IT_CHILD) {
            if (P[it->child].used)
                walk(it->child, depth + 1, guard + 1);
        } else
            add_row(VR_EXIT, pi, s, depth + 1);
        s = it->next;
    }
}

static int content_h(void)
{
    return nrows * RH;
}

static int view_h(void)
{
    return tree_w ? tree_w->rect.h - HEAD_H : 0;
}

static void clamp_scroll(void)
{
    int max = content_h() - view_h();
    if (max < 0)
        max = 0;
    if (scroll > max)
        scroll = max;
    if (scroll < 0)
        scroll = 0;
}

void view_rebuild(void)
{
    if (!rows)
        rows = malloc(sizeof(*rows) * ROWCAP);
    if (!rows)
        return;
    nrows = 0;
    int roots[MAX_PROCS];
    int n = model_roots(roots, MAX_PROCS);
    for (int i = 0; i < n; i++)
        walk(roots[i], 0, 0);
    sel = -1;
    if (sel_kind)
        for (int i = 0; i < nrows; i++)
            if (rows[i].kind == sel_kind && rows[i].proc == sel_proc && rows[i].seq == sel_seq) {
                sel = i;
                break;
            }
    if (autoscroll)
        scroll = content_h();
    clamp_scroll();
    view_redraw();
}

void view_redraw(void)
{
    if (tree_w)
        pg_widget_queue_draw(tree_w);
    if (detail_w)
        pg_widget_queue_draw(detail_w);
    if (stats_w)
        pg_widget_queue_draw(stats_w);
}

void view_set_filter(const char *text)
{
    snprintf(filter, sizeof(filter), "%s", text ? text : "");
    view_rebuild();
}

void view_set_autoscroll(bool on)
{
    autoscroll = on;
    if (on) {
        scroll = content_h();
        clamp_scroll();
        view_redraw();
    }
}

bool view_autoscroll(void)
{
    return autoscroll;
}

int view_row_count(void)
{
    return nrows;
}

void view_clear_selection(void)
{
    sel = -1;
    sel_kind = 0;
    scroll = 0;
}

static void select_row(int i, bool reveal)
{
    if (i < 0 || i >= nrows)
        return;
    sel = i;
    sel_kind = rows[i].kind;
    sel_proc = rows[i].proc;
    sel_seq = rows[i].seq;
    if (reveal) {
        if (i * RH < scroll)
            scroll = i * RH;
        else if ((i + 1) * RH > scroll + view_h())
            scroll = (i + 1) * RH - view_h();
        clamp_scroll();
    }
    view_redraw();
}

void view_reveal_stopped(void)
{
    for (int i = 0; i < nrows; i++)
        if (rows[i].kind == VR_SYS && P[rows[i].proc].stopped && P[rows[i].proc].stopped_seq == rows[i].seq) {
            select_row(i, true);
            return;
        }
}

static void arrow(struct pg_canvas *c, int x, int y, bool open, uint32_t color)
{
    if (open) {
        pgc_line(c, (float)x, (float)y + 3, (float)x + 4, (float)y + 7, 1.6f, color);
        pgc_line(c, (float)x + 4, (float)y + 7, (float)x + 8, (float)y + 3, 1.6f, color);
    } else {
        pgc_line(c, (float)x + 2, (float)y, (float)x + 6, (float)y + 4, 1.6f, color);
        pgc_line(c, (float)x + 6, (float)y + 4, (float)x + 2, (float)y + 8, 1.6f, color);
    }
}

static void draw_row(struct pg_canvas *c, const struct pg_rect *r, int i, int y)
{
    const struct pg_theme *th = pg_theme();
    struct vrow *vr = &rows[i];
    struct proc *p = &P[vr->proc];
    int font = PGF_MONO_S;
    int fh = pgc_font_h(font);
    int ty = y + (RH - fh) / 2;
    bool is_stopped = vr->kind == VR_SYS && p->stopped && p->stopped_seq == vr->seq;
    if (i == sel)
        pgc_fill(c, r->x, y, r->w, RH, th->surface3);
    else if (is_stopped)
        pgc_fill_a(c, r->x, y, r->w, RH, th->warning, 70);
    else if (i & 1)
        pgc_fill_a(c, r->x, y, r->w, RH, th->surface2, 60);
    if (is_stopped)
        pgc_fill(c, r->x, y, 3, RH, th->warning);
    int base = r->x + 10;
    for (int d = 1; d <= vr->depth; d++)
        pgc_fill_a(c, base + d * IND - IND / 2, y, 1, RH, th->text3, 90);
    int x = base + vr->depth * IND;
    int right = r->x + r->w - 12;
    int dur_w = 84, ret_w = 170;
    if (vr->kind == VR_PROC) {
        arrow(c, x, y + 6, p->expanded, th->text2);
        char line[160], info[96];
        snprintf(line, sizeof(line), "[%u] %s", p->pid, p->name);
        int w = pgc_text(c, PGF_MONO_S_BOLD, x + 16, ty, line, p->alive ? th->accent : th->text2);
        const char *state = p->stopped ? "STOPPED" : p->alive ? "running" : p->exit_text;
        snprintf(info, sizeof(info), "%s  %llu calls  %llu errors", state, (unsigned long long)p->calls,
                 (unsigned long long)p->errors);
        uint32_t ic = p->stopped ? th->warning : p->errors ? th->text2 : th->text3;
        if (!p->alive && strstr(p->exit_text, "killed"))
            ic = th->danger;
        pgc_text(c, font, x + 28 + w, ty, info, ic);
        return;
    }
    if (vr->kind == VR_EXIT) {
        struct item *it = item_at(vr->seq);
        char line[96];
        snprintf(line, sizeof(line), "+++ %s +++", p->exit_text[0] ? p->exit_text : "exited");
        bool bad = it && (it->rec.args[0] || it->rec.ret);
        pgc_text(c, font, x + 16, ty, line, bad ? th->danger : th->text3);
        return;
    }
    struct item *it = item_at(vr->seq);
    if (!it)
        return;
    char tmp[24], args[320], ret[64], dur[32], line[360];
    const char *name = tl_sysname(it->rec.nr, tmp, sizeof(tmp));
    tl_format_args(&it->rec, args, sizeof(args), true);
    int nw = pgc_text(c, PGF_MONO_S_BOLD, x + 16, ty, name, is_stopped ? th->warning : th->info);
    snprintf(line, sizeof(line), "(%s)", args);
    int maxw = right - ret_w - dur_w - (x + 16 + nw) - 12;
    if (maxw > 20)
        pgc_text_fit(c, font, x + 16 + nw, ty, maxw, line, th->text);
    if (!it->done) {
        pgc_text(c, font, right - dur_w - ret_w, ty, is_stopped ? "stopped at entry" : "...", is_stopped ? th->warning : th->text3);
        return;
    }
    bool err = tl_format_ret(it->rec.nr, it->rec.ret, ret, sizeof(ret));
    pgc_text_fit(c, font, right - dur_w - ret_w, ty, ret_w - 8, ret, err ? th->danger : th->success);
    tl_format_duration(it->rec.duration_ns, dur, sizeof(dur));
    pgc_text(c, font, right - pgc_text_w(font, dur), ty, dur, it->rec.duration_ns >= SLOW_NS ? th->warning : th->text3);
}

static void draw_tree(struct pg_widget *w, struct pg_canvas *c, void *data)
{
    (void)data;
    const struct pg_theme *th = pg_theme();
    struct pg_rect r = w->rect;
    struct pg_clip saved = pgc_clip_push(c, r.x, r.y, r.w, r.h);
    pgc_fill(c, r.x, r.y, r.w, r.h, th->surface);
    clamp_scroll();
    int top = r.y + HEAD_H;
    struct pg_clip body = pgc_clip_push(c, r.x, top, r.w, r.h - HEAD_H);
    int first = scroll / RH;
    int y = top - scroll % RH;
    for (int i = first; i < nrows && y < r.y + r.h; i++, y += RH)
        draw_row(c, &r, i, y);
    if (!nrows)
        pgc_text_center(c, PGF_BODY, r.x, top + 40, r.w, "No trace yet. Run a command or attach to a process.", th->text3);
    int total = content_h(), vh = view_h();
    if (total > vh && vh > 0) {
        int th_h = vh * vh / total;
        if (th_h < 28)
            th_h = 28;
        int span = vh - th_h, max = total - vh;
        int ty = top + (max ? span * scroll / max : 0);
        pgc_rrect(c, r.x + r.w - 8, ty, 5, th_h, 2, th->text3, 150);
    }
    pgc_clip_pop(c, body);
    pgc_fill(c, r.x, r.y, r.w, HEAD_H, th->surface2);
    pgc_fill(c, r.x, r.y + HEAD_H - 1, r.w, 1, th->border);
    int hy = r.y + (HEAD_H - pgc_font_h(PGF_SMALL)) / 2;
    pgc_text(c, PGF_SMALL, r.x + 26, hy, "Process / system call", th->text2);
    pgc_text(c, PGF_SMALL, r.x + r.w - 12 - 84 - 170, hy, "Result", th->text2);
    pgc_text(c, PGF_SMALL, r.x + r.w - 12 - pgc_text_w(PGF_SMALL, "Duration"), hy, "Duration", th->text2);
    pgc_clip_pop(c, saved);
}

static void toggle(int i)
{
    P[rows[i].proc].expanded = !P[rows[i].proc].expanded;
    view_rebuild();
}

static void set_scroll_from_y(int y, struct pg_rect r)
{
    int total = content_h(), vh = view_h();
    if (total <= vh)
        return;
    int th_h = vh * vh / total;
    if (th_h < 28)
        th_h = 28;
    int span = vh - th_h;
    int pos = y - (r.y + HEAD_H) - th_h / 2;
    if (span > 0)
        scroll = pos * (total - vh) / span;
    stop_auto();
    clamp_scroll();
}

static bool tree_input(struct pg_widget *w, const struct pg_event *e, void *data)
{
    (void)data;
    struct pg_rect r = w->rect;
    if (e->type == PG_EVENT_WHEEL) {
        scroll -= e->wheel * RH * 3;
        if (e->wheel > 0)
            stop_auto();
        clamp_scroll();
        pg_widget_queue_draw(w);
        return true;
    }
    if (e->type == PG_EVENT_POINTER_UP) {
        dragging = false;
        return false;
    }
    if (e->type == PG_EVENT_POINTER_MOVE && dragging) {
        set_scroll_from_y(e->y, r);
        pg_widget_queue_draw(w);
        return true;
    }
    if (e->type == PG_EVENT_POINTER_DOWN && e->button == PG_BUTTON_LEFT) {
        pg_widget_grab_focus(w);
        if (e->x >= r.x + r.w - 14 && e->y >= r.y + HEAD_H) {
            dragging = true;
            set_scroll_from_y(e->y, r);
            pg_widget_queue_draw(w);
            return true;
        }
        if (e->y < r.y + HEAD_H)
            return true;
        int i = (e->y - r.y - HEAD_H + scroll) / RH;
        if (i < 0 || i >= nrows)
            return true;
        select_row(i, false);
        if (rows[i].kind == VR_PROC)
            toggle(i);
        return true;
    }
    if (e->type == PG_EVENT_KEY_DOWN) {
        int page = view_h() / RH;
        int cur = sel < 0 ? 0 : sel;
        if (e->key == PG_KEY_DOWN)
            select_row(sel < 0 ? 0 : cur + 1, true);
        else if (e->key == PG_KEY_UP)
            select_row(sel < 0 ? 0 : cur - 1, true);
        else if (e->key == PG_KEY_PAGE_DOWN)
            select_row(cur + page < nrows ? cur + page : nrows - 1, true);
        else if (e->key == PG_KEY_PAGE_UP)
            select_row(cur - page > 0 ? cur - page : 0, true);
        else if (e->key == PG_KEY_HOME)
            select_row(0, true);
        else if (e->key == PG_KEY_END)
            select_row(nrows - 1, true);
        else if ((e->key == PG_KEY_RIGHT || e->key == PG_KEY_LEFT) && sel >= 0 && rows[sel].kind == VR_PROC) {
            bool want = e->key == PG_KEY_RIGHT;
            if (P[rows[sel].proc].expanded != want)
                toggle(sel);
        } else if (e->key == PG_KEY_ENTER && sel >= 0 && rows[sel].kind == VR_PROC)
            toggle(sel);
        else
            return false;
        stop_auto();
        return true;
    }
    return false;
}

struct pg_widget *view_tree_new(void)
{
    tree_w = pg_drawing_new(300, 200, draw_tree, tree_input, NULL);
    pg_widget_set_expand(tree_w, true, true);
    return tree_w;
}

static int line(struct pg_canvas *c, int x, int y, const char *label, const char *value, uint32_t color)
{
    const struct pg_theme *th = pg_theme();
    pgc_text(c, PGF_SMALL, x, y, label, th->text3);
    pgc_text_fit(c, PGF_MONO_S, x + 110, y, 1000, value, color);
    return y + pgc_font_h(PGF_MONO_S) + 5;
}

static void draw_detail(struct pg_widget *w, struct pg_canvas *c, void *data)
{
    (void)data;
    const struct pg_theme *th = pg_theme();
    struct pg_rect r = w->rect;
    struct pg_clip saved = pgc_clip_push(c, r.x, r.y, r.w, r.h);
    pgc_fill(c, r.x, r.y, r.w, r.h, th->bg);
    pgc_fill(c, r.x, r.y, r.w, 1, th->border);
    int x = r.x + 14, y = r.y + 10;
    char buf[400];
    if (sel < 0 || sel >= nrows) {
        pgc_text(c, PGF_BODY, x, y, "Select a row to see its details.", th->text3);
        pgc_clip_pop(c, saved);
        return;
    }
    struct vrow *vr = &rows[sel];
    struct proc *p = &P[vr->proc];
    if (vr->kind != VR_SYS) {
        snprintf(buf, sizeof(buf), "%u  %s", p->pid, p->name);
        y = line(c, x, y, "Process", buf, th->text);
        snprintf(buf, sizeof(buf), "%u", p->ppid);
        y = line(c, x, y, "Parent", buf, th->text);
        y = line(c, x, y, "State", p->stopped ? "stopped at a breakpoint" : p->alive ? "running" : p->exit_text, th->text);
        snprintf(buf, sizeof(buf), "%llu calls, %llu failed", (unsigned long long)p->calls, (unsigned long long)p->errors);
        line(c, x, y, "System calls", buf, th->text);
        pgc_clip_pop(c, saved);
        return;
    }
    struct item *it = item_at(vr->seq);
    if (!it) {
        pgc_clip_pop(c, saved);
        return;
    }
    char tmp[24], dec[200];
    const char *name = tl_sysname(it->rec.nr, tmp, sizeof(tmp));
    snprintf(buf, sizeof(buf), "%s  (number %u)", name, it->rec.nr);
    y = line(c, x, y, "System call", buf, th->info);
    snprintf(buf, sizeof(buf), "%u %s   thread %u   parent %u", it->rec.pid, p->name, it->rec.tid, it->rec.ppid);
    y = line(c, x, y, "Process", buf, th->text);
    snprintf(buf, sizeof(buf), "%llu.%06llu s kernel time", (unsigned long long)(it->rec.time_ns / 1000000000ULL),
             (unsigned long long)(it->rec.time_ns % 1000000000ULL / 1000));
    y = line(c, x, y, "Entered", buf, th->text);
    if (it->done) {
        char d[32], ret[64];
        tl_format_duration(it->rec.duration_ns, d, sizeof(d));
        bool err = tl_format_ret(it->rec.nr, it->rec.ret, ret, sizeof(ret));
        snprintf(buf, sizeof(buf), "%s   (raw %lld, 0x%llx)", ret, (long long)it->rec.ret, (unsigned long long)it->rec.ret);
        y = line(c, x, y, "Result", buf, err ? th->danger : th->success);
        y = line(c, x, y, "Duration", d, it->rec.duration_ns >= SLOW_NS ? th->warning : th->text);
    } else
        y = line(c, x, y, "Result", p->stopped && p->stopped_seq == vr->seq ? "stopped at entry, not executed yet" : "not finished", th->warning);
    if (it->rec.flags & (TRACE_REC_STR1 | TRACE_REC_STR2)) {
        snprintf(buf, sizeof(buf), "\"%s\"", it->rec.str);
        y = line(c, x, y, "String", buf, th->text);
    }
    int x2 = r.x + r.w / 2 + 20;
    int ay = r.y + 10;
    int n = tl_nargs(it->rec.nr);
    if (n < 3)
        n = 3;
    for (int i = 0; i < n && i < 6; i++) {
        char kind = tl_argkind(it->rec.nr, i);
        tl_format_arg(&it->rec, i, dec, sizeof(dec), true);
        snprintf(buf, sizeof(buf), "arg%d %s", i + 1, tl_kind_label(kind));
        pgc_text(c, PGF_SMALL, x2, ay, buf, th->text3);
        snprintf(buf, sizeof(buf), "0x%llx = %s", (unsigned long long)it->rec.args[i], dec);
        pgc_text_fit(c, PGF_MONO_S, x2 + 130, ay, r.x + r.w - x2 - 140, buf, th->text);
        ay += pgc_font_h(PGF_MONO_S) + 5;
    }
    pgc_clip_pop(c, saved);
}

struct pg_widget *view_detail_new(void)
{
    detail_w = pg_drawing_new(300, 170, draw_detail, NULL, NULL);
    pg_widget_set_expand(detail_w, true, false);
    return detail_w;
}

static int stat_proc(void)
{
    if (sel >= 0 && sel < nrows)
        return rows[sel].proc;
    for (int i = 0; i < M.nprocs; i++)
        if (P[i].used && P[i].parent < 0)
            return i;
    return -1;
}

static const struct pstat *cmp_stat;

static int stat_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    if (cmp_stat->calls[x] != cmp_stat->calls[y])
        return cmp_stat->calls[x] < cmp_stat->calls[y] ? 1 : -1;
    return x - y;
}

static void draw_stats(struct pg_widget *w, struct pg_canvas *c, void *data)
{
    (void)data;
    const struct pg_theme *th = pg_theme();
    struct pg_rect r = w->rect;
    struct pg_clip saved = pgc_clip_push(c, r.x, r.y, r.w, r.h);
    pgc_fill(c, r.x, r.y, r.w, r.h, th->bg);
    pgc_fill(c, r.x, r.y, r.w, 1, th->border);
    int pi = stat_proc();
    if (pi < 0 || !P[pi].st) {
        pgc_text(c, PGF_BODY, r.x + 14, r.y + 12, "No statistics yet.", th->text3);
        pgc_clip_pop(c, saved);
        return;
    }
    struct proc *p = &P[pi];
    char buf[160];
    snprintf(buf, sizeof(buf), "[%u] %s: %llu calls, %llu failed", p->pid, p->name, (unsigned long long)p->calls,
             (unsigned long long)p->errors);
    pgc_text(c, PGF_BODY, r.x + 14, r.y + 8, buf, th->text);
    int order[NSYS], n = 0;
    for (int i = 0; i < NSYS; i++)
        if (p->st->calls[i])
            order[n++] = i;
    cmp_stat = p->st;
    qsort(order, (size_t)n, sizeof(int), stat_cmp);
    int y = r.y + 8 + pgc_font_h(PGF_BODY) + 8;
    int bh = 14, step = 18;
    uint32_t maxc = n ? p->st->calls[order[0]] : 1;
    int label_w = 130, num_w = 210;
    int bar_x = r.x + 14 + label_w, bar_w = r.w - 28 - label_w - num_w;
    if (bar_w < 40)
        bar_w = 40;
    for (int i = 0; i < n && y + bh <= r.y + r.h; i++, y += step) {
        int nr = order[i];
        char tmp[24];
        pgc_text_fit(c, PGF_MONO_S, r.x + 14, y, label_w - 8, tl_sysname((uint32_t)nr, tmp, sizeof(tmp)), th->text);
        pgc_rrect(c, bar_x, y, bar_w, bh, 3, th->surface2, 255);
        int len = (int)((uint64_t)bar_w * p->st->calls[nr] / maxc);
        if (len < 2)
            len = 2;
        pgc_rrect(c, bar_x, y, len, bh, 3, th->accent, 255);
        if (p->st->errors[nr]) {
            int el = (int)((uint64_t)len * p->st->errors[nr] / p->st->calls[nr]);
            if (el < 2)
                el = 2;
            pgc_rrect(c, bar_x, y, el, bh, 3, th->danger, 255);
        }
        char d[32];
        tl_format_duration(p->st->ns[nr], d, sizeof(d));
        snprintf(buf, sizeof(buf), "%u calls  %u err  %s", p->st->calls[nr], p->st->errors[nr], d);
        pgc_text_fit(c, PGF_MONO_S, bar_x + bar_w + 10, y, num_w - 10, buf, th->text2);
    }
    pgc_clip_pop(c, saved);
}

struct pg_widget *view_stats_new(void)
{
    stats_w = pg_drawing_new(300, 170, draw_stats, NULL, NULL);
    pg_widget_set_expand(stats_w, true, false);
    return stats_w;
}

bool view_has_selection(void)
{
    return sel >= 0 && sel < nrows;
}

void view_expand_all(bool expanded)
{
    for (int i = 0; i < M.nprocs; i++)
        if (P[i].used)
            P[i].expanded = expanded;
    view_rebuild();
}

struct sbuf {
    char *p;
    size_t n, cap;
    bool bad;
};

static void sb_add(struct sbuf *b, const char *fmt, ...)
{
    if (b->bad)
        return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int need = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (need < 0) {
            b->bad = true;
            return;
        }
        if (b->p && b->n + (size_t)need < b->cap) {
            b->n += (size_t)need;
            return;
        }
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap <= b->n + (size_t)need)
            cap *= 2;
        char *np = realloc(b->p, cap);
        if (!np) {
            b->bad = true;
            return;
        }
        b->p = np;
        b->cap = cap;
    }
}

static char *sb_done(struct sbuf *b)
{
    if (b->bad) {
        free(b->p);
        return NULL;
    }
    return b->p;
}

char *view_selected_row_text(void)
{
    if (!view_has_selection())
        return NULL;
    struct vrow *vr = &rows[sel];
    struct proc *p = &P[vr->proc];
    struct sbuf b = {0};
    if (vr->kind == VR_PROC) {
        sb_add(&b, "[%u] %s  %s  %llu calls  %llu errors", p->pid, p->name,
               p->stopped ? "STOPPED" : p->alive ? "running" : p->exit_text, (unsigned long long)p->calls,
               (unsigned long long)p->errors);
    } else if (vr->kind == VR_EXIT) {
        sb_add(&b, "+++ %s +++", p->exit_text[0] ? p->exit_text : "exited");
    } else {
        struct item *it = item_at(vr->seq);
        if (!it)
            return NULL;
        char tmp[24], args[1024], ret[64], dur[32];
        tl_format_args(&it->rec, args, sizeof(args), true);
        sb_add(&b, "[%u] %s(%s)", it->rec.pid, tl_sysname(it->rec.nr, tmp, sizeof(tmp)), args);
        if (it->done) {
            tl_format_ret(it->rec.nr, it->rec.ret, ret, sizeof(ret));
            tl_format_duration(it->rec.duration_ns, dur, sizeof(dur));
            sb_add(&b, " = %s <%s>", ret, dur);
        } else
            sb_add(&b, " = ?");
    }
    return sb_done(&b);
}

char *view_selected_details_text(void)
{
    if (!view_has_selection())
        return NULL;
    struct vrow *vr = &rows[sel];
    struct proc *p = &P[vr->proc];
    struct sbuf b = {0};
    if (vr->kind != VR_SYS) {
        sb_add(&b, "Process: %u %s\nParent: %u\nState: %s\nSystem calls: %llu calls, %llu failed\n", p->pid, p->name,
               p->ppid, p->stopped ? "stopped at a breakpoint" : p->alive ? "running" : p->exit_text,
               (unsigned long long)p->calls, (unsigned long long)p->errors);
        return sb_done(&b);
    }
    struct item *it = item_at(vr->seq);
    if (!it)
        return NULL;
    char tmp[24], dec[1024], ret[64], dur[32];
    sb_add(&b, "System call: %s (number %u)\n", tl_sysname(it->rec.nr, tmp, sizeof(tmp)), it->rec.nr);
    sb_add(&b, "Process: %u %s  thread %u  parent %u\n", it->rec.pid, p->name, it->rec.tid, it->rec.ppid);
    sb_add(&b, "Entered: %llu.%06llu s\n", (unsigned long long)(it->rec.time_ns / 1000000000ULL),
           (unsigned long long)(it->rec.time_ns % 1000000000ULL / 1000));
    if (it->done) {
        tl_format_ret(it->rec.nr, it->rec.ret, ret, sizeof(ret));
        tl_format_duration(it->rec.duration_ns, dur, sizeof(dur));
        sb_add(&b, "Result: %s (raw %lld, 0x%llx)\nDuration: %s\n", ret, (long long)it->rec.ret,
               (unsigned long long)it->rec.ret, dur);
    } else
        sb_add(&b, "Result: not finished\n");
    if (it->rec.flags & (TRACE_REC_STR1 | TRACE_REC_STR2))
        sb_add(&b, "String: \"%s\"\n", it->rec.str);
    int n = tl_nargs(it->rec.nr);
    if (n < 3)
        n = 3;
    for (int i = 0; i < n && i < 6; i++) {
        tl_format_arg(&it->rec, i, dec, sizeof(dec), true);
        sb_add(&b, "arg%d %s: 0x%llx = %s\n", i + 1, tl_kind_label(tl_argkind(it->rec.nr, i)),
               (unsigned long long)it->rec.args[i], dec);
    }
    return sb_done(&b);
}
