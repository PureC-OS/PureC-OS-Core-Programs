#include <stdio.h>
#include <string.h>
#include "tabs.h"
#include "ui.h"
#include "graph.h"
#include "config.h"
#include "util.h"

static void card_through(struct pg_canvas *c, int x, int y, int w, int h)
{
    struct pg_rect in = ui_card(c, x, y, w, h, "Throughput", NULL);
    char a[32], b[32], t[32], u[32];
    u_rate(a, sizeof(a), g_sys.rx_rate, g_cfg.units);
    u_rate(b, sizeof(b), g_sys.tx_rate, g_cfg.units);
    u_bytes(t, sizeof(t), g_sys.rx_total, g_cfg.units);
    u_bytes(u, sizeof(u), g_sys.tx_total, g_cfg.units);
    int q = in.w / 4;
    ui_stat(c, in.x, in.y - 4, q - 8, "Download", a, grad_at(&g_theme.download, 600));
    ui_stat(c, in.x + q, in.y - 4, q - 8, "Upload", b, grad_at(&g_theme.upload, 600));
    ui_stat(c, in.x + 2 * q, in.y - 4, q - 8, "Received", t, g_theme.foreground);
    ui_stat(c, in.x + 3 * q, in.y - 4, q - 8, "Sent", u, g_theme.foreground);
    int gy = in.y + 2 * pgc_font_h(PGF_BODY) + 18;
    uint64_t mx = hist_max(&g_sys.rx_hist), mt = hist_max(&g_sys.tx_hist);
    if (mt > mx)
        mx = mt;
    uint64_t scale = graph_nice_max(mx);
    char sc[40];
    u_rate(sc, sizeof(sc), scale, g_cfg.units);
    int gh = in.y + in.h - gy;
    struct series s[2] = {{&g_sys.rx_hist, &g_theme.download}, {&g_sys.tx_hist, &g_theme.upload}};
    graph_draw(c, in.x, gy, in.w, gh, s, 2, scale, g_cfg.history);
    ui_text(c, PGF_SMALL, in.x + 6, gy + 4, sc, g_theme.foreground_dim);
    ui_legend(c, in.x + in.w - 170, gy + 4, "Down", grad_at(&g_theme.download, 600));
    ui_legend(c, in.x + in.w - 90, gy + 4, "Up", grad_at(&g_theme.upload, 600));
}

static void card_ifaces(struct pg_canvas *c, int x, int y, int w, int h)
{
    char right[32];
    snprintf(right, sizeof(right), "%d interface%s", g_sys.nifaces, g_sys.nifaces == 1 ? "" : "s");
    struct pg_rect in = ui_card(c, x, y, w, h, "Interfaces", right);
    if (!g_sys.nifaces) {
        ui_text(c, PGF_BODY, in.x, in.y, "No network interfaces", g_theme.foreground_dim);
        return;
    }
    int rh = in.h / g_sys.nifaces;
    if (rh > 72)
        rh = 72;
    if (rh < 40)
        rh = 40;
    bool spark = in.w >= 640;
    for (int i = 0; i < g_sys.nifaces; i++) {
        const struct iface_info *f = &g_sys.ifaces[i];
        int ry = in.y + i * rh;
        if (ry + rh > in.y + in.h + 4)
            break;
        pgc_rrect(c, in.x, ry, in.w, rh - 6, 8, g_theme.surface2, 255);
        char line[80], rate[40], tot[64], a[32], b[32];
        ui_text(c, PGF_MEDIUM, in.x + 12, ry + 6, f->name, g_theme.foreground);
        if (f->has_ip)
            snprintf(line, sizeof(line), "%s   %u.%u.%u.%u", f->link ? "Up" : "Down", (f->ip >> 24) & 255u,
                     (f->ip >> 16) & 255u, (f->ip >> 8) & 255u, f->ip & 255u);
        else
            snprintf(line, sizeof(line), "%s   no address", f->link ? "Up" : "Down");
        ui_text(c, PGF_SMALL, in.x + 12, ry + 8 + pgc_font_h(PGF_MEDIUM), line, f->link ? g_theme.success : g_theme.foreground_dim);
        int mx = in.x + 210;
        u_rate(a, sizeof(a), f->rx_rate, g_cfg.units);
        u_rate(b, sizeof(b), f->tx_rate, g_cfg.units);
        snprintf(rate, sizeof(rate), "Down %s", a);
        ui_text(c, PGF_BODY, mx, ry + 6, rate, grad_at(&g_theme.download, 600));
        snprintf(rate, sizeof(rate), "Up %s", b);
        ui_text(c, PGF_BODY, mx, ry + 8 + pgc_font_h(PGF_BODY), rate, grad_at(&g_theme.upload, 600));
        u_bytes(a, sizeof(a), f->rx_bytes, g_cfg.units);
        u_bytes(b, sizeof(b), f->tx_bytes, g_cfg.units);
        snprintf(tot, sizeof(tot), "%s in / %s out", a, b);
        int tx = mx + 190;
        if (tx + 120 < in.x + in.w)
            ui_text_fit(c, PGF_SMALL, tx, ry + 6, in.x + in.w - tx - (spark ? 250 : 12), tot, g_theme.foreground_dim);
        if (spark) {
            int sx = in.x + in.w - 240, sw = 228;
            uint64_t m1 = hist_max(&f->rx), m2 = hist_max(&f->tx);
            struct series s[2] = {{&f->rx, &g_theme.download}, {&f->tx, &g_theme.upload}};
            struct pg_clip saved = pgc_clip_push(c, sx, ry + 4, sw, rh - 14);
            graph_draw(c, sx, ry + 4, sw, rh - 14, s, 2, graph_nice_max(m1 > m2 ? m1 : m2), g_cfg.history);
            pgc_clip_pop(c, saved);
        }
    }
}

static void draw(struct pg_widget *w, struct pg_canvas *c, void *data)
{
    (void)data;
    struct pg_rect r = w->rect;
    pgc_fill(c, r.x, r.y, r.w, r.h, g_theme.background);
    int pad = 16, gap = 14;
    int x = r.x + pad, y = r.y + pad, W = r.w - 2 * pad, H = r.h - 2 * pad;
    int th = H * 48 / 100;
    if (th < 220)
        th = 220;
    card_through(c, x, y, W, th);
    int ih = H - th - gap;
    if (ih > 100)
        card_ifaces(c, x, y + th + gap, W, ih);
}

struct pg_widget *tab_network_new(void)
{
    struct pg_widget *w = pg_drawing_new(200, 200, draw, NULL, NULL);
    pg_widget_set_expand(w, true, true);
    return w;
}
