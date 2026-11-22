#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "config.h"

struct pg_rect ui_card(struct pg_canvas *c, int x, int y, int w, int h, const char *title, const char *right)
{
    pgc_rrect(c, x, y, w, h, 10, g_theme.surface, 255);
    pgc_rrect_stroke(c, x, y, w, h, 10, g_theme.border, 255);
    int top = 12;
    if (title && *title) {
        pgc_text(c, PGF_MEDIUM, x + 14, y + 10, title, g_theme.foreground);
        if (right && *right) {
            int tw = pgc_text_w(PGF_SMALL, right);
            pgc_text(c, PGF_SMALL, x + w - 14 - tw, y + 12, right, g_theme.foreground_dim);
        }
        top = 10 + pgc_font_h(PGF_MEDIUM) + 8;
    }
    struct pg_rect in = {x + 14, y + top, w - 28, h - top - 12};
    if (in.h < 0)
        in.h = 0;
    return in;
}

int ui_text(struct pg_canvas *c, int font, int x, int y, const char *s, uint32_t color)
{
    return pgc_text(c, font, x, y, s, color);
}

void ui_text_right(struct pg_canvas *c, int font, int right, int y, const char *s, uint32_t color)
{
    pgc_text(c, font, right - pgc_text_w(font, s), y, s, color);
}

void ui_text_fit(struct pg_canvas *c, int font, int x, int y, int maxw, const char *s, uint32_t color)
{
    pgc_text_fit(c, font, x, y, maxw, s, color);
}

void ui_stat(struct pg_canvas *c, int x, int y, int w, const char *label, const char *value, uint32_t color)
{
    pgc_text(c, PGF_SMALL, x, y, label, g_theme.foreground_dim);
    pgc_text_fit(c, PGF_BODY, x, y + pgc_font_h(PGF_SMALL) + 2, w, value, color);
}

void ui_legend(struct pg_canvas *c, int x, int y, const char *label, uint32_t color)
{
    pgc_rrect(c, x, y + 3, 10, 10, 3, color, 255);
    pgc_text(c, PGF_SMALL, x + 16, y, label, g_theme.foreground_dim);
}

struct overlay {
    struct pg_widget base;
    struct pg_widget *content;
    struct pg_widget *modal;
    char toast_text[160];
    uint32_t toast_color;
    uint32_t toast_timer;
    bool toast_on;
};

static struct overlay *overlay;

static struct {
    bool active, danger;
    char title[64], text[200], action[32];
    ui_confirm_fn fn;
    void *data;
    int hover;
} dlg;

static void overlay_measure(struct pg_widget *w, int *width, int *height)
{
    pg_widget_measure(((struct overlay *)w)->content, width, height);
}

static void overlay_arrange(struct pg_widget *w)
{
    struct overlay *o = (struct overlay *)w;
    pg_widget_place(o->content, w->rect.x, w->rect.y, w->rect.w, w->rect.h);
    if (o->modal->visible)
        pg_widget_place(o->modal, w->rect.x, w->rect.y, w->rect.w, w->rect.h);
}

static void overlay_draw_after(struct pg_widget *w, struct pg_canvas *c)
{
    struct overlay *o = (struct overlay *)w;
    if (!o->toast_on)
        return;
    int tw = pgc_text_w(PGF_BODY, o->toast_text) + 36, th = pgc_font_h(PGF_BODY) + 20;
    if (tw > w->rect.w - 32)
        tw = w->rect.w - 32;
    int x = w->rect.x + (w->rect.w - tw) / 2, y = w->rect.y + w->rect.h - th - 24;
    pgc_shadow(c, x, y, tw, th, 10, 8, 90);
    pgc_rrect(c, x, y, tw, th, 10, g_theme.surface2, 255);
    pgc_rrect_stroke(c, x, y, tw, th, 10, o->toast_color, 255);
    pgc_text_fit(c, PGF_BODY, x + 18, y + 10, tw - 36, o->toast_text, g_theme.foreground);
}

static const struct pg_widget_class overlay_class = {
    .name = "overlay",
    .size = sizeof(struct overlay),
    .measure = overlay_measure,
    .arrange = overlay_arrange,
    .draw_after = overlay_draw_after,
};

static struct pg_rect dlg_box(const struct pg_rect *area)
{
    int w = 460, h = 190;
    if (w > area->w - 24)
        w = area->w - 24;
    struct pg_rect r = {area->x + (area->w - w) / 2, area->y + (area->h - h) / 2, w, h};
    return r;
}

static void dlg_buttons(const struct pg_rect *box, struct pg_rect *cancel, struct pg_rect *ok)
{
    int bw = 120, bh = 36;
    ok->w = bw;
    ok->h = bh;
    ok->x = box->x + box->w - 20 - bw;
    ok->y = box->y + box->h - 20 - bh;
    cancel->w = bw;
    cancel->h = bh;
    cancel->x = ok->x - 12 - bw;
    cancel->y = ok->y;
}

static void modal_draw(struct pg_widget *w, struct pg_canvas *c)
{
    pgc_fill_a(c, w->rect.x, w->rect.y, w->rect.w, w->rect.h, 0x000000, 150);
    struct pg_rect box = dlg_box(&w->rect), cancel, ok;
    pgc_shadow(c, box.x, box.y, box.w, box.h, 14, 14, 120);
    pgc_rrect(c, box.x, box.y, box.w, box.h, 14, g_theme.surface, 255);
    pgc_rrect_stroke(c, box.x, box.y, box.w, box.h, 14, g_theme.border, 255);
    pgc_text_fit(c, PGF_HEAD, box.x + 22, box.y + 20, box.w - 44, dlg.title, g_theme.foreground);
    pgc_text_wrap(c, PGF_BODY, box.x + 22, box.y + 20 + pgc_font_h(PGF_HEAD) + 12, box.w - 44, dlg.text,
                  g_theme.foreground_dim);
    dlg_buttons(&box, &cancel, &ok);
    uint32_t okc = dlg.danger ? g_theme.danger : g_theme.accent;
    pgc_rrect(c, cancel.x, cancel.y, cancel.w, cancel.h, 8, dlg.hover == 1 ? g_theme.border : g_theme.surface2, 255);
    pgc_text_center(c, PGF_BODY, cancel.x, cancel.y + (cancel.h - pgc_font_h(PGF_BODY)) / 2, cancel.w, "Cancel",
                    g_theme.foreground);
    pgc_rrect(c, ok.x, ok.y, ok.w, ok.h, 8, dlg.hover == 2 ? pgc_lighten(okc, 24) : okc, 255);
    pgc_text_center(c, PGF_BODY, ok.x, ok.y + (ok.h - pgc_font_h(PGF_BODY)) / 2, ok.w, dlg.action, 0xFFFFFF);
}

static void modal_close(void)
{
    dlg.active = false;
    pg_widget_set_visible(overlay->modal, false);
    pg_widget_queue_layout(&overlay->base);
    if (overlay->base.window)
        pg_window_queue_draw(overlay->base.window);
}

static void modal_accept(void)
{
    ui_confirm_fn fn = dlg.fn;
    void *d = dlg.data;
    modal_close();
    if (fn)
        fn(d);
}

static bool in_rect(const struct pg_rect *r, int x, int y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static bool modal_event(struct pg_widget *w, const struct pg_event *e)
{
    struct pg_rect box = dlg_box(&w->rect), cancel, ok;
    dlg_buttons(&box, &cancel, &ok);
    if (e->type == PG_EVENT_POINTER_MOVE) {
        int h = in_rect(&cancel, e->x, e->y) ? 1 : in_rect(&ok, e->x, e->y) ? 2 : 0;
        if (h != dlg.hover) {
            dlg.hover = h;
            pg_widget_queue_draw(w);
        }
        return true;
    }
    if (e->type == PG_EVENT_POINTER_UP && e->button == PG_BUTTON_LEFT) {
        if (in_rect(&ok, e->x, e->y))
            modal_accept();
        else if (in_rect(&cancel, e->x, e->y) || !in_rect(&box, e->x, e->y))
            modal_close();
        return true;
    }
    return e->type == PG_EVENT_POINTER_DOWN || e->type == PG_EVENT_WHEEL;
}

static const struct pg_widget_class modal_class = {
    .name = "modal",
    .draw = modal_draw,
    .event = modal_event,
};

struct pg_widget *ui_overlay_new(struct pg_widget *content)
{
    overlay = (struct overlay *)pg_widget_new(&overlay_class);
    overlay->content = content;
    pg_widget_add(&overlay->base, content);
    overlay->modal = pg_widget_new(&modal_class);
    overlay->modal->visible = false;
    pg_widget_add(&overlay->base, overlay->modal);
    return &overlay->base;
}

static bool toast_hide(void *data)
{
    (void)data;
    overlay->toast_timer = 0;
    overlay->toast_on = false;
    if (overlay->base.window)
        pg_window_queue_draw(overlay->base.window);
    return false;
}

void ui_toast(const char *text, uint32_t color)
{
    if (!overlay)
        return;
    snprintf(overlay->toast_text, sizeof(overlay->toast_text), "%s", text);
    overlay->toast_color = color;
    overlay->toast_on = true;
    if (overlay->toast_timer)
        pg_timer_remove(overlay->toast_timer);
    overlay->toast_timer = pg_timer_add(3200, toast_hide, NULL);
    if (overlay->base.window)
        pg_window_queue_draw(overlay->base.window);
}

void ui_confirm(const char *title, const char *text, const char *action, bool danger, ui_confirm_fn fn, void *data)
{
    snprintf(dlg.title, sizeof(dlg.title), "%s", title);
    snprintf(dlg.text, sizeof(dlg.text), "%s", text);
    snprintf(dlg.action, sizeof(dlg.action), "%s", action);
    dlg.danger = danger;
    dlg.fn = fn;
    dlg.data = data;
    dlg.hover = 0;
    dlg.active = true;
    pg_widget_set_visible(overlay->modal, true);
    pg_widget_queue_layout(&overlay->base);
    if (overlay->base.window)
        pg_window_queue_draw(overlay->base.window);
}

bool ui_modal_active(void)
{
    return dlg.active;
}

bool ui_modal_key(const struct pg_event *e)
{
    if (!dlg.active)
        return false;
    if (e->key == PG_KEY_ESCAPE)
        modal_close();
    else if (e->key == PG_KEY_ENTER || e->key == PG_KEY_KP_ENTER)
        modal_accept();
    return true;
}

struct tabbar {
    struct pg_widget base;
    const char *const *labels;
    const int *icons;
    int count, selected, hover;
    void (*changed)(int);
};

static void tabbar_measure(struct pg_widget *w, int *width, int *height)
{
    (void)w;
    *width = 200;
    *height = 52;
}

static int tab_width(const struct tabbar *t, int i)
{
    return pgc_text_w(PGF_MEDIUM, t->labels[i]) + 28 + (t->icons ? 26 : 0);
}

static int tab_at(const struct tabbar *t, int x)
{
    int cx = t->base.rect.x + 12;
    for (int i = 0; i < t->count; i++) {
        int tw = tab_width(t, i);
        if (x >= cx && x < cx + tw)
            return i;
        cx += tw + 4;
    }
    return -1;
}

static void tabbar_draw(struct pg_widget *w, struct pg_canvas *c)
{
    struct tabbar *t = (struct tabbar *)w;
    struct pg_rect r = w->rect;
    pgc_fill(c, r.x, r.y, r.w, r.h, g_theme.surface);
    pgc_fill(c, r.x, r.y + r.h - 1, r.w, 1, g_theme.border);
    int cx = r.x + 12;
    for (int i = 0; i < t->count; i++) {
        int tw = tab_width(t, i);
        bool sel = i == t->selected;
        if (sel)
            pgc_rrect(c, cx, r.y + 8, tw, r.h - 16, 8, g_theme.selection, 255);
        else if (i == t->hover)
            pgc_rrect(c, cx, r.y + 8, tw, r.h - 16, 8, g_theme.surface2, 255);
        uint32_t col = sel ? g_theme.selection_text : g_theme.foreground_dim;
        int tx = cx + 14;
        if (t->icons) {
            int px = pgc_icon_px(PG_ICON_S);
            pgc_icon(c, t->icons[i], PG_ICON_S, tx, r.y + (r.h - px) / 2, sel ? g_theme.accent : col);
            tx += 26;
        }
        pgc_text(c, PGF_MEDIUM, tx, r.y + (r.h - pgc_font_h(PGF_MEDIUM)) / 2, t->labels[i], col);
        if (sel)
            pgc_rrect(c, cx + 10, r.y + r.h - 4, tw - 20, 3, 1, g_theme.accent, 255);
        cx += tw + 4;
    }
}

static bool tabbar_event(struct pg_widget *w, const struct pg_event *e)
{
    struct tabbar *t = (struct tabbar *)w;
    if (e->type == PG_EVENT_POINTER_MOVE || e->type == PG_EVENT_ENTER) {
        int h = tab_at(t, e->x);
        if (h != t->hover) {
            t->hover = h;
            pg_widget_queue_draw(w);
        }
        return false;
    }
    if (e->type == PG_EVENT_LEAVE) {
        t->hover = -1;
        pg_widget_queue_draw(w);
        return false;
    }
    if (e->type == PG_EVENT_POINTER_DOWN && e->button == PG_BUTTON_LEFT) {
        int i = tab_at(t, e->x);
        if (i >= 0 && i != t->selected) {
            ui_tabbar_select(w, i);
            if (t->changed)
                t->changed(i);
        }
        return true;
    }
    return false;
}

static const struct pg_widget_class tabbar_class = {
    .name = "tabbar",
    .size = sizeof(struct tabbar),
    .measure = tabbar_measure,
    .draw = tabbar_draw,
    .event = tabbar_event,
};

struct pg_widget *ui_tabbar_new(const char *const *labels, const int *icons, int count, void (*changed)(int))
{
    struct tabbar *t = (struct tabbar *)pg_widget_new(&tabbar_class);
    t->labels = labels;
    t->icons = icons;
    t->count = count;
    t->hover = -1;
    t->changed = changed;
    t->base.hexpand = true;
    return &t->base;
}

void ui_tabbar_select(struct pg_widget *bar, int index)
{
    struct tabbar *t = (struct tabbar *)bar;
    t->selected = index;
    pg_widget_queue_draw(bar);
}

int ui_tabbar_selected(const struct pg_widget *bar)
{
    return ((const struct tabbar *)bar)->selected;
}
