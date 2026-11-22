#include <stdio.h>
#include <string.h>
#include "tabs.h"
#include "ui.h"
#include "graph.h"
#include "config.h"
#include "util.h"

static void card_fs(struct pg_canvas *c, int x, int y, int w, int h)
{
    char right[32];
    snprintf(right, sizeof(right), "%d mounted", g_sys.nmounts);
    struct pg_rect in = ui_card(c, x, y, w, h, "Filesystems", right);
    int rh = 62;
    for (int i = 0; i < g_sys.nmounts; i++) {
        const struct mount_info *m = &g_sys.mounts[i];
        int ry = in.y + i * rh;
        if (ry + rh - 6 > in.y + in.h)
            break;
        pgc_rrect(c, in.x, ry, in.w, rh - 6, 8, g_theme.surface2, 255);
        char line[120];
        ui_text_fit(c, PGF_MEDIUM, in.x + 12, ry + 6, in.w / 2 - 24, m->path, g_theme.foreground);
        snprintf(line, sizeof(line), "%s  %s  %s", m->source, m->type, m->ro ? "read-only" : "read-write");
        ui_text_fit(c, PGF_SMALL, in.x + 12, ry + 8 + pgc_font_h(PGF_MEDIUM), in.w / 2 - 24, line, g_theme.foreground_dim);
        int bx = in.x + in.w / 2, bw = in.w / 2 - 12;
        if (m->has_usage && m->total) {
            uint64_t used = m->total > m->free ? m->total - m->free : 0;
            uint32_t pm = (uint32_t)(used * 1000 / m->total);
            char a[32], b[32];
            u_bytes(a, sizeof(a), used, g_cfg.units);
            u_bytes(b, sizeof(b), m->total, g_cfg.units);
            snprintf(line, sizeof(line), "%s of %s  (%u%%)", a, b, pm / 10);
            ui_text_right(c, PGF_BODY, bx + bw, ry + 6, line, pm > 900 ? g_theme.danger : pm > 750 ? g_theme.warning : g_theme.foreground);
            struct grad g = {g_theme.success, g_theme.warning, g_theme.danger};
            bar_draw(c, bx, ry + 6 + pgc_font_h(PGF_BODY) + 8, bw, 10, pm, &g);
        } else
            ui_text_right(c, PGF_SMALL, bx + bw, ry + 14, "usage is not reported for this filesystem", g_theme.foreground_dim);
    }
}

static void card_parts(struct pg_canvas *c, int x, int y, int w, int h)
{
    char right[48];
    snprintf(right, sizeof(right), "%d device%s", g_sys.nparts, g_sys.nparts == 1 ? "" : "s");
    struct pg_rect in = ui_card(c, x, y, w, h, "Block devices", right);
    int rh = 34;
    for (int i = 0; i < g_sys.nparts; i++) {
        const struct part_info *p = &g_sys.parts[i];
        int ry = in.y + i * rh;
        if (ry + rh > in.y + in.h)
            break;
        char sz[32];
        u_bytes(sz, sizeof(sz), p->bytes, UNITS_AUTO);
        ui_text(c, PGF_BODY, in.x, ry + 4, p->name, g_theme.foreground);
        ui_text_fit(c, PGF_BODY, in.x + 120, ry + 4, in.w - 240, p->model, g_theme.foreground_dim);
        ui_text_right(c, PGF_BODY, in.x + in.w, ry + 4, sz, g_theme.foreground);
        pgc_fill_a(c, in.x, ry + rh - 4, in.w, 1, g_theme.border, 160);
    }
    int ny = in.y + g_sys.nparts * rh + 6;
    if (ny + pgc_font_h(PGF_SMALL) <= in.y + in.h)
        ui_text(c, PGF_SMALL, in.x, ny, "Disk read and write rates are not reported by the kernel.", g_theme.foreground_dim);
}

static void draw(struct pg_widget *w, struct pg_canvas *c, void *data)
{
    (void)data;
    struct pg_rect r = w->rect;
    pgc_fill(c, r.x, r.y, r.w, r.h, g_theme.background);
    int pad = 16, gap = 14;
    int x = r.x + pad, y = r.y + pad, W = r.w - 2 * pad, H = r.h - 2 * pad;
    int fh = 40 + g_sys.nmounts * 62 + 8;
    if (fh > H * 60 / 100)
        fh = H * 60 / 100;
    if (fh < 120)
        fh = 120;
    card_fs(c, x, y, W, fh);
    int ph = H - fh - gap;
    if (ph > 80)
        card_parts(c, x, y + fh + gap, W, ph);
}

struct pg_widget *tab_disks_new(void)
{
    struct pg_widget *w = pg_drawing_new(200, 200, draw, NULL, NULL);
    pg_widget_set_expand(w, true, true);
    return w;
}
