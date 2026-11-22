#include <stdio.h>
#include <string.h>
#include "tabs.h"
#include "ui.h"
#include "graph.h"
#include "config.h"
#include "util.h"

static int top_cpu(int *out, int max)
{
    int n = 0;
    bool used[MAX_PROCS] = {0};
    for (; n < max && n < g_sys.nprocs; n++) {
        int best = -1;
        for (int i = 0; i < g_sys.nprocs; i++) {
            if (used[i])
                continue;
            if (best < 0 || g_sys.procs[i].cpu_pm > g_sys.procs[best].cpu_pm ||
                (g_sys.procs[i].cpu_pm == g_sys.procs[best].cpu_pm && g_sys.procs[i].rss_kb > g_sys.procs[best].rss_kb))
                best = i;
        }
        if (best < 0)
            break;
        used[best] = true;
        out[n] = best;
    }
    return n;
}

static void card_cpu(struct pg_canvas *c, int x, int y, int w, int h)
{
    char right[64], big[24];
    snprintf(right, sizeof(right), "%d cores", g_sys.online_cores);
    struct pg_rect in = ui_card(c, x, y, w, h, "CPU", right);
    u_percent(big, sizeof(big), g_sys.cpu_pm);
    ui_text(c, PGF_TITLE, in.x, in.y - 2, big, grad_at(&g_theme.cpu, g_sys.cpu_pm));
    int gy = in.y + pgc_font_h(PGF_TITLE) + 4;
    struct series s = {&g_sys.cpu_hist, &g_theme.cpu};
    graph_draw(c, in.x, gy, in.w, in.y + in.h - gy, &s, 1, 1000, g_cfg.history);
}

static void card_mem(struct pg_canvas *c, int x, int y, int w, int h)
{
    char right[64], big[24], a[24], b[24];
    u_bytes(a, sizeof(a), g_sys.mem_used_kb * 1024, g_cfg.units);
    u_bytes(b, sizeof(b), g_sys.mem_total_kb * 1024, g_cfg.units);
    snprintf(right, sizeof(right), "%s of %s", a, b);
    struct pg_rect in = ui_card(c, x, y, w, h, "Memory", right);
    uint32_t pm = g_sys.mem_total_kb ? (uint32_t)(g_sys.mem_used_kb * 1000 / g_sys.mem_total_kb) : 0;
    u_percent(big, sizeof(big), pm);
    ui_text(c, PGF_TITLE, in.x, in.y - 2, big, grad_at(&g_theme.mem, pm));
    int gy = in.y + pgc_font_h(PGF_TITLE) + 4;
    struct series s = {&g_sys.mem_hist, &g_theme.mem};
    graph_draw(c, in.x, gy, in.w, in.y + in.h - gy, &s, 1, 1000, g_cfg.history);
}

static void card_net(struct pg_canvas *c, int x, int y, int w, int h)
{
    struct pg_rect in = ui_card(c, x, y, w, h, "Network", NULL);
    char a[32], b[32];
    u_rate(a, sizeof(a), g_sys.rx_rate, g_cfg.units);
    u_rate(b, sizeof(b), g_sys.tx_rate, g_cfg.units);
    int half = in.w / 2;
    ui_text(c, PGF_SMALL, in.x, in.y - 2, "Download", g_theme.foreground_dim);
    ui_text(c, PGF_MEDIUM, in.x, in.y + 12, a, grad_at(&g_theme.download, 600));
    ui_text(c, PGF_SMALL, in.x + half, in.y - 2, "Upload", g_theme.foreground_dim);
    ui_text(c, PGF_MEDIUM, in.x + half, in.y + 12, b, grad_at(&g_theme.upload, 600));
    int gy = in.y + 12 + pgc_font_h(PGF_MEDIUM) + 8;
    uint64_t mx = hist_max(&g_sys.rx_hist);
    uint64_t mt = hist_max(&g_sys.tx_hist);
    if (mt > mx)
        mx = mt;
    struct series s[2] = {{&g_sys.rx_hist, &g_theme.download}, {&g_sys.tx_hist, &g_theme.upload}};
    graph_draw(c, in.x, gy, in.w, in.y + in.h - gy, s, 2, graph_nice_max(mx), g_cfg.history);
}

static void row_kv(struct pg_canvas *c, struct pg_rect *in, int *y, const char *k, const char *v)
{
    if (*y + pgc_font_h(PGF_BODY) > in->y + in->h)
        return;
    ui_text(c, PGF_BODY, in->x, *y, k, g_theme.foreground_dim);
    int vx = in->x + 120;
    ui_text_fit(c, PGF_BODY, vx, *y, in->x + in->w - vx, v, g_theme.foreground);
    *y += pgc_font_h(PGF_BODY) + 8;
}

static void card_system(struct pg_canvas *c, int x, int y, int w, int h)
{
    struct pg_rect in = ui_card(c, x, y, w, h, "System", NULL);
    int cy = in.y;
    char v[96], a[32], b[32];
    u_duration(v, sizeof(v), g_sys.uptime_s);
    row_kv(c, &in, &cy, "Uptime", v);
    row_kv(c, &in, &cy, "Processor", g_sys.cpu_model[0] ? g_sys.cpu_model : "unknown");
    snprintf(v, sizeof(v), "%d online of %d%s%u MHz", g_sys.online_cores, g_sys.ncores, g_sys.cpu_mhz ? ", " : "", g_sys.cpu_mhz);
    if (!g_sys.cpu_mhz)
        snprintf(v, sizeof(v), "%d online of %d", g_sys.online_cores, g_sys.ncores);
    row_kv(c, &in, &cy, "Cores", v);
    snprintf(v, sizeof(v), "%u total, %u running", g_sys.tasks, g_sys.running);
    row_kv(c, &in, &cy, "Tasks", v);
    u_bytes(a, sizeof(a), g_sys.mem_free_kb * 1024, g_cfg.units);
    u_bytes(b, sizeof(b), g_sys.mem_total_kb * 1024, g_cfg.units);
    snprintf(v, sizeof(v), "%s free of %s", a, b);
    row_kv(c, &in, &cy, "Memory", v);
    if (g_sys.swap_total_kb) {
        u_bytes(a, sizeof(a), (g_sys.swap_total_kb - g_sys.swap_free_kb) * 1024, g_cfg.units);
        u_bytes(b, sizeof(b), g_sys.swap_total_kb * 1024, g_cfg.units);
        snprintf(v, sizeof(v), "%s of %s", a, b);
    } else
        snprintf(v, sizeof(v), "not enabled");
    row_kv(c, &in, &cy, "Swap", v);
    if (g_sys.thermal_ok) {
        snprintf(v, sizeof(v), "%d.%d C%s", g_sys.thermal_deci_c / 10, g_sys.thermal_deci_c % 10, g_sys.fan_rpm ? "" : "");
        if (g_sys.fan_rpm)
            snprintf(v + strlen(v), sizeof(v) - strlen(v), ", fan %u rpm", g_sys.fan_rpm);
        row_kv(c, &in, &cy, "Temperature", v);
    }
    if (g_sys.battery_present) {
        snprintf(v, sizeof(v), "%u%%%s", g_sys.battery_pct, g_sys.battery_charging ? ", charging" : "");
        row_kv(c, &in, &cy, "Battery", v);
    }
    row_kv(c, &in, &cy, "Kernel", g_sys.kernel);
}

static void card_top(struct pg_canvas *c, int x, int y, int w, int h)
{
    struct pg_rect in = ui_card(c, x, y, w, h, "Top processes", "by CPU");
    int rh = pgc_font_h(PGF_BODY) + 10;
    int max = in.h / rh;
    if (max > 12)
        max = 12;
    int idx[12];
    int n = top_cpu(idx, max);
    for (int i = 0; i < n; i++) {
        const struct proc_info *p = &g_sys.procs[idx[i]];
        int ry = in.y + i * rh;
        char cpu[16], mem[24];
        u_percent(cpu, sizeof(cpu), p->cpu_pm);
        u_bytes(mem, sizeof(mem), p->rss_kb * 1024, g_cfg.units);
        ui_text_right(c, PGF_BODY, in.x + in.w, ry, mem, g_theme.foreground_dim);
        ui_text_right(c, PGF_BODY, in.x + in.w - 100, ry, cpu, grad_at(&g_theme.cpu, p->cpu_pm * 4 > 1000 ? 1000 : p->cpu_pm * 4));
        ui_text_fit(c, PGF_BODY, in.x, ry, in.w - 200, p->name, g_theme.foreground);
    }
}

static void draw(struct pg_widget *w, struct pg_canvas *c, void *data)
{
    (void)data;
    struct pg_rect r = w->rect;
    pgc_fill(c, r.x, r.y, r.w, r.h, g_theme.background);
    int pad = 16, gap = 14;
    int x = r.x + pad, y = r.y + pad, W = r.w - 2 * pad;
    int cols = W >= 840 ? 3 : W >= 540 ? 2 : 1;
    int cw = (W - gap * (cols - 1)) / cols;
    int avail = r.h - 2 * pad;
    int rows = (3 + cols - 1) / cols;
    int ch = cols == 3 ? avail * 45 / 100 : avail * 32 / 100;
    if (ch < 170)
        ch = 170;
    if (ch > 260)
        ch = 260;
    for (int i = 0; i < 3; i++) {
        int cx = x + (i % cols) * (cw + gap), cy = y + (i / cols) * (ch + gap);
        if (i == 0)
            card_cpu(c, cx, cy, cw, ch);
        else if (i == 1)
            card_mem(c, cx, cy, cw, ch);
        else
            card_net(c, cx, cy, cw, ch);
    }
    y += rows * (ch + gap);
    int rest = r.y + r.h - pad - y;
    if (rest < 120)
        return;
    if (W >= 720) {
        int lw = (W - gap) / 2;
        card_system(c, x, y, lw, rest);
        card_top(c, x + lw + gap, y, W - lw - gap, rest);
    } else {
        int hh = (rest - gap) / 2;
        card_system(c, x, y, W, hh);
        card_top(c, x, y + hh + gap, W, hh);
    }
}

struct pg_widget *tab_overview_new(void)
{
    struct pg_widget *w = pg_drawing_new(200, 200, draw, NULL, NULL);
    pg_widget_set_expand(w, true, true);
    return w;
}
