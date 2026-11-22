#include <stdio.h>
#include "graph.h"
#include "config.h"
#include "util.h"

uint64_t graph_nice_max(uint64_t v)
{
    uint64_t floor_v = 16 * 1024;
    if (v < floor_v)
        return floor_v;
    uint64_t p = floor_v;
    while (p < v)
        p *= 2;
    return p;
}

void graph_grid(struct pg_canvas *c, int x, int y, int w, int h, int lines)
{
    for (int i = 0; i <= lines; i++) {
        int gy = y + (h - 1) * i / lines;
        pgc_fill_a(c, x, gy, w, 1, g_theme.border, i == lines ? 200 : 110);
    }
}

static uint64_t sample_at(const struct history *hs, float fi)
{
    if (hs->count == 0)
        return 0;
    if (fi <= 0.0f)
        return hist_at(hs, 0);
    if (fi >= (float)(hs->count - 1))
        return hist_at(hs, hs->count - 1);
    int i0 = (int)fi;
    float f = fi - (float)i0;
    float a = (float)hist_at(hs, i0), b = (float)hist_at(hs, i0 + 1);
    return (uint64_t)(a + (b - a) * f);
}

void graph_draw(struct pg_canvas *c, int x, int y, int w, int h, const struct series *s, int n,
                uint64_t max_value, int capacity)
{
    if (w < 8 || h < 8 || capacity < 2)
        return;
    if (!max_value)
        max_value = 1;
    graph_grid(c, x, y, w, h, 4);
    float step = (float)w / (float)(capacity - 1);
    float phase = 1.0f;
    if (g_cfg.smooth && g_sys.last_ms) {
        uint64_t now = pg_time_ms();
        float p = (float)(now - g_sys.last_ms) / (float)g_cfg.interval_ms;
        phase = p > 1.0f ? 1.0f : p < 0.0f ? 0.0f : p;
    }
    for (int si = 0; si < n; si++) {
        const struct history *hs = s[si].h;
        if (!hs->count)
            continue;
        int count = hs->count;
        float lastx = 0, lasty = 0;
        bool have = false;
        for (int px = 0; px < w; px += 2) {
            float fi = ((float)px - (float)w - (1.0f - phase) * step) / step + (float)(count - 1);
            if (fi < -0.5f)
                continue;
            uint64_t v = sample_at(hs, fi);
            uint64_t vv = v > max_value ? max_value : v;
            int hh = (int)((uint64_t)(h - 2) * vv / max_value);
            uint32_t ratio = (uint32_t)(vv * 1000 / max_value);
            uint32_t col = grad_at(s[si].grad, ratio);
            if (hh > 0)
                pgc_fill_a(c, x + px, y + h - 1 - hh, 2, hh, col, n > 1 ? 60 : 85);
            float fx = (float)(x + px), fy = (float)(y + h - 1 - hh);
            if (have)
                pgc_line(c, lastx, lasty, fx, fy, 1.6f, col);
            lastx = fx;
            lasty = fy;
            have = true;
        }
    }
}

void bar_draw(struct pg_canvas *c, int x, int y, int w, int h, uint32_t pm, const struct grad *g)
{
    int r = h / 2;
    pgc_rrect(c, x, y, w, h, r, g_theme.surface2, 255);
    if (pm > 1000)
        pm = 1000;
    int fw = (int)((uint64_t)w * pm / 1000);
    if (fw < h && pm > 0)
        fw = h;
    if (fw > 0)
        pgc_rrect_grad(c, x, y, fw, h, r, g->low, grad_at(g, pm), true);
}

void heat_draw(struct pg_canvas *c, int x, int y, int w, int h, const uint32_t *pm, const bool *online, int n)
{
    if (n <= 0 || w < 20 || h < 12)
        return;
    int cols = 1;
    while (cols < n && (w / cols) * (h / ((n + cols - 1) / cols)) > 0 && (w / cols) > 2 * (h / ((n + cols - 1) / cols)))
        cols++;
    int rows = (n + cols - 1) / cols;
    int gap = 4;
    int cw = (w - gap * (cols - 1)) / cols, ch = (h - gap * (rows - 1)) / rows;
    for (int i = 0; i < n; i++) {
        int cx = x + (i % cols) * (cw + gap), cy = y + (i / cols) * (ch + gap);
        uint32_t base = online[i] ? grad_at(&g_theme.cpu, pm[i]) : g_theme.surface2;
        pgc_rrect(c, cx, cy, cw, ch, 6, g_theme.surface2, 255);
        if (online[i])
            pgc_rrect(c, cx, cy, cw, ch, 6, base, 60 + pm[i] * 195 / 1000);
        char label[24];
        if (online[i])
            snprintf(label, sizeof(label), "%u  %u%%", (unsigned)i, pm[i] / 10);
        else
            snprintf(label, sizeof(label), "%u  off", (unsigned)i);
        int tw = pgc_text_w(PGF_SMALL, label);
        if (tw + 8 <= cw && pgc_font_h(PGF_SMALL) + 4 <= ch)
            pgc_text(c, PGF_SMALL, cx + (cw - tw) / 2, cy + (ch - pgc_font_h(PGF_SMALL)) / 2, label,
                     online[i] && pm[i] > 450 ? 0x101010 : g_theme.foreground);
    }
}
