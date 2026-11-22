#include <stdio.h>
#include <string.h>
#include "tabs.h"
#include "ui.h"
#include "graph.h"
#include "config.h"
#include "util.h"

static void core_cells(struct pg_canvas *c, struct pg_rect in)
{
    int n = g_sys.ncores;
    int gap = 10;
    int cols = (in.w + gap) / (210 + gap);
    if (cols < 1)
        cols = 1;
    if (cols > n)
        cols = n;
    int rows = (n + cols - 1) / cols;
    int cw = (in.w - gap * (cols - 1)) / cols;
    int chh = (in.h - gap * (rows - 1)) / rows;
    if (chh < 52) {
        heat_draw(c, in.x, in.y, in.w, in.h, g_sys.core_pm, g_sys.core_online, n);
        return;
    }
    if (chh > 110)
        chh = 110;
    for (int i = 0; i < n; i++) {
        int cx = in.x + (i % cols) * (cw + gap), cy = in.y + (i / cols) * (chh + gap);
        pgc_rrect(c, cx, cy, cw, chh, 8, g_theme.surface2, 255);
        char label[24], pct[16];
        snprintf(label, sizeof(label), "Core %d", i);
        ui_text(c, PGF_SMALL, cx + 10, cy + 6, label, g_theme.foreground_dim);
        if (g_sys.core_online[i]) {
            u_percent(pct, sizeof(pct), g_sys.core_pm[i]);
            ui_text_right(c, PGF_SMALL, cx + cw - 10, cy + 6, pct, grad_at(&g_theme.cpu, g_sys.core_pm[i]));
        } else
            ui_text_right(c, PGF_SMALL, cx + cw - 10, cy + 6, "offline", g_theme.foreground_dim);
        int gy = cy + 8 + pgc_font_h(PGF_SMALL);
        struct series s = {&g_sys.core_hist[i], &g_theme.cpu};
        struct pg_clip saved = pgc_clip_push(c, cx + 6, gy, cw - 12, cy + chh - gy - 6);
        graph_draw(c, cx + 6, gy, cw - 12, cy + chh - gy - 6, &s, 1, 1000, g_cfg.history);
        pgc_clip_pop(c, saved);
    }
}

static void card_cpu(struct pg_canvas *c, int x, int y, int w, int h)
{
    char right[96], big[24];
    snprintf(right, sizeof(right), "%s", g_sys.cpu_model[0] ? g_sys.cpu_model : "");
    struct pg_rect in = ui_card(c, x, y, w, h, "Processor", right);
    if (g_cfg.cpu_view == CPUVIEW_CORES && g_sys.ncores > 0) {
        u_percent(big, sizeof(big), g_sys.cpu_pm);
        ui_text(c, PGF_HEAD, in.x, in.y - 4, big, grad_at(&g_theme.cpu, g_sys.cpu_pm));
        ui_text(c, PGF_SMALL, in.x + pgc_text_w(PGF_HEAD, big) + 10, in.y + 4, "total", g_theme.foreground_dim);
        int top = in.y + pgc_font_h(PGF_HEAD) + 4;
        struct pg_rect cells = {in.x, top, in.w, in.y + in.h - top};
        core_cells(c, cells);
        return;
    }
    u_percent(big, sizeof(big), g_sys.cpu_pm);
    ui_text(c, PGF_HEAD, in.x, in.y - 4, big, grad_at(&g_theme.cpu, g_sys.cpu_pm));
    int top = in.y + pgc_font_h(PGF_HEAD) + 4;
    int gh = (in.y + in.h - top) * 62 / 100;
    struct series s = {&g_sys.cpu_hist, &g_theme.cpu};
    graph_draw(c, in.x, top, in.w, gh, &s, 1, 1000, g_cfg.history);
    int hy = top + gh + 12;
    heat_draw(c, in.x, hy, in.w, in.y + in.h - hy, g_sys.core_pm, g_sys.core_online, g_sys.ncores);
}

static void mem_row(struct pg_canvas *c, int x, int y, int w, const char *name, uint64_t used_kb, uint64_t total_kb,
                    const struct grad *g, const char *none)
{
    char a[32], b[32], text[80];
    ui_text(c, PGF_MEDIUM, x, y, name, g_theme.foreground);
    if (!total_kb) {
        ui_text_right(c, PGF_BODY, x + w, y + 1, none, g_theme.foreground_dim);
        bar_draw(c, x, y + 26, w, 12, 0, g);
        return;
    }
    u_bytes(a, sizeof(a), used_kb * 1024, g_cfg.units);
    u_bytes(b, sizeof(b), total_kb * 1024, g_cfg.units);
    uint32_t pm = (uint32_t)(used_kb * 1000 / total_kb);
    snprintf(text, sizeof(text), "%s of %s  (%u%%)", a, b, pm / 10);
    ui_text_right(c, PGF_BODY, x + w, y + 1, text, g_theme.foreground_dim);
    bar_draw(c, x, y + 26, w, 12, pm, g);
}

static void card_mem(struct pg_canvas *c, int x, int y, int w, int h)
{
    struct pg_rect in = ui_card(c, x, y, w, h, "Memory and swap", NULL);
    bool wide = in.w >= 640;
    int gw = wide ? in.w * 52 / 100 : in.w;
    int gh = wide ? in.h : in.h * 45 / 100;
    ui_legend(c, in.x, in.y - 4, "RAM", grad_at(&g_theme.mem, 700));
    struct series s[2] = {{&g_sys.mem_hist, &g_theme.mem}, {&g_sys.swap_hist, &g_theme.upload}};
    int ns = g_sys.swap_total_kb ? 2 : 1;
    if (ns == 2)
        ui_legend(c, in.x + 70, in.y - 4, "Swap", grad_at(&g_theme.upload, 600));
    graph_draw(c, in.x, in.y + 18, gw, gh - 18, s, ns, 1000, g_cfg.history);
    int bx = wide ? in.x + gw + 24 : in.x;
    int bw = wide ? in.w - gw - 24 : in.w;
    int by = wide ? in.y : in.y + gh + 12;
    mem_row(c, bx, by, bw, "RAM", g_sys.mem_used_kb, g_sys.mem_total_kb, &g_theme.mem, "");
    by += 54;
    uint64_t su = g_sys.swap_total_kb > g_sys.swap_free_kb ? g_sys.swap_total_kb - g_sys.swap_free_kb : 0;
    mem_row(c, bx, by, bw, "Swap", su, g_sys.swap_total_kb, &g_theme.upload, "not enabled");
    by += 54;
    if (by + pgc_font_h(PGF_BODY) <= in.y + in.h) {
        char v[96], f[32];
        u_bytes(f, sizeof(f), g_sys.mem_free_kb * 1024, g_cfg.units);
        snprintf(v, sizeof(v), "Free: %s", f);
        ui_text(c, PGF_BODY, bx, by, v, g_theme.foreground_dim);
        by += pgc_font_h(PGF_BODY) + 6;
    }
    if (g_sys.swap_total_kb && by + pgc_font_h(PGF_BODY) <= in.y + in.h) {
        char v[96];
        snprintf(v, sizeof(v), "Swap pages in: %llu   out: %llu", (unsigned long long)g_sys.swap_in,
                 (unsigned long long)g_sys.swap_out);
        ui_text_fit(c, PGF_BODY, bx, by, bw, v, g_theme.foreground_dim);
    }
}

static void draw(struct pg_widget *w, struct pg_canvas *c, void *data)
{
    (void)data;
    struct pg_rect r = w->rect;
    pgc_fill(c, r.x, r.y, r.w, r.h, g_theme.background);
    int pad = 16, gap = 14;
    int x = r.x + pad, y = r.y + pad, W = r.w - 2 * pad, H = r.h - 2 * pad;
    int mh = H * 36 / 100;
    if (mh < 190)
        mh = 190;
    int ch = H - mh - gap;
    if (ch < 180) {
        ch = H;
        mh = 0;
    }
    card_cpu(c, x, y, W, ch);
    if (mh)
        card_mem(c, x, y + ch + gap, W, mh);
}

struct pg_widget *tab_resources_new(void)
{
    struct pg_widget *w = pg_drawing_new(200, 200, draw, NULL, NULL);
    pg_widget_set_expand(w, true, true);
    return w;
}
