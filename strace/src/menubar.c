#include <stdint.h>
#include <stdlib.h>
#include "menubar.h"

#define BAR_H 26
#define PAD_X 10

struct mitem {
    const char *label;
    int act;
    const char *shortcut;
    bool check;
};

struct mdef {
    const char *title;
    int alt_key;
    const struct mitem *items;
    int count;
};

static const struct mitem m_file[] = {
    {"Run command...", A_RUN, NULL, false},
    {"Attach to PID...", A_ATTACH, NULL, false},
    {NULL, 0, NULL, false},
    {"Export JSON...", A_EXPORT, "Ctrl+S", false},
    {"Clear", A_CLEAR, NULL, false},
    {NULL, 0, NULL, false},
    {"Quit", A_QUIT, "Ctrl+Q", false},
};

static const struct mitem m_edit[] = {
    {"Copy selected row", A_COPY_ROW, NULL, false},
    {"Copy details", A_COPY_DETAILS, NULL, false},
    {NULL, 0, NULL, false},
    {"Focus filter", A_FOCUS_FILTER, "Ctrl+F", false},
};

static const struct mitem m_view[] = {
    {"Toggle details panel", A_TOGGLE_DETAILS, "Ctrl+J", true},
    {"Autoscroll", A_AUTOSCROLL, NULL, true},
    {NULL, 0, NULL, false},
    {"Expand all", A_EXPAND_ALL, NULL, false},
    {"Collapse all", A_COLLAPSE_ALL, NULL, false},
    {NULL, 0, NULL, false},
    {"Statistics", A_STATS, NULL, false},
};

static const struct mitem m_trace[] = {
    {"Start", A_START, NULL, false},
    {"Pause display", A_PAUSE, NULL, true},
    {"Continue", A_CONTINUE, "F5", false},
    {"Step", A_STEP, "F10", false},
    {"Detach", A_DETACH, NULL, false},
    {NULL, 0, NULL, false},
    {"Breakpoints...", A_BREAKS, NULL, false},
};

static const struct mitem m_help[] = {
    {"About", A_ABOUT, NULL, false},
    {"man strace", A_MAN, NULL, false},
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const struct mdef defs[] = {
    {"File", 0x09, m_file, COUNT(m_file)},
    {"Edit", 0x08, m_edit, COUNT(m_edit)},
    {"View", 0x19, m_view, COUNT(m_view)},
    {"Trace", 0x17, m_trace, COUNT(m_trace)},
    {"Help", 0x0B, m_help, COUNT(m_help)},
};

#define NDEFS COUNT(defs)

static struct pg_menu *cur;
static int cur_idx = -1;
static int hot = -1;
static struct pg_widget *bar;
static struct pg_window *owner;
static const struct menubar_hooks *hooks;

static void item_cb(int index, void *data)
{
    (void)index;
    cur_idx = -1;
    hooks->run((int)(intptr_t)data);
    pg_widget_queue_draw(bar);
}

static void open_menu(int idx);

static void nav_cb(int dir, void *data)
{
    (void)data;
    if (cur_idx < 0)
        return;
    open_menu((cur_idx + dir + (int)NDEFS) % (int)NDEFS);
}

static int title_x(int i)
{
    int x = bar->rect.x + 6;
    for (int k = 0; k < i; k++)
        x += pgc_text_w(PGF_BODY, defs[k].title) + PAD_X * 2;
    return x;
}

static int title_at(int px)
{
    for (int i = 0; i < (int)NDEFS; i++) {
        int x0 = title_x(i);
        int x1 = x0 + pgc_text_w(PGF_BODY, defs[i].title) + PAD_X * 2;
        if (px >= x0 && px < x1)
            return i;
    }
    return -1;
}

static void open_menu(int idx)
{
    if (cur) {
        pg_menu_destroy(cur);
        cur = NULL;
    }
    cur_idx = -1;
    pg_widget_queue_draw(bar);
    struct pg_menu *m = pg_menu_new();
    if (!m)
        return;
    const struct mdef *d = &defs[idx];
    for (int i = 0; i < d->count; i++) {
        const struct mitem *it = &d->items[i];
        if (!it->label) {
            pg_menu_add_separator(m);
            continue;
        }
        int k = pg_menu_add(m, it->label, PG_NO_ICON, item_cb, (void *)(intptr_t)it->act);
        if (it->shortcut)
            pg_menu_set_shortcut(m, k, it->shortcut);
        pg_menu_set_enabled(m, k, hooks->enabled(it->act));
        if (it->check)
            pg_menu_set_checked(m, k, hooks->checked(it->act));
    }
    pg_menu_on_nav(m, nav_cb, NULL);
    cur = m;
    cur_idx = idx;
    pg_menu_popup(m, owner, title_x(idx), bar->rect.y + bar->rect.h);
}

void menubar_close(void)
{
    if (cur)
        pg_menu_close(cur);
    cur_idx = -1;
    if (bar)
        pg_widget_queue_draw(bar);
}

static bool open_now(void)
{
    if (cur && cur_idx >= 0 && !pg_menu_is_open(cur))
        cur_idx = -1;
    return cur_idx >= 0;
}

static void bar_draw(struct pg_widget *w, struct pg_canvas *c, void *data)
{
    (void)data;
    const struct pg_theme *t = pg_theme();
    pgc_fill(c, w->rect.x, w->rect.y, w->rect.w, w->rect.h, t->surface);
    pgc_fill(c, w->rect.x, w->rect.y + w->rect.h - 1, w->rect.w, 1, t->border);
    bool open = open_now();
    int fh = pgc_font_h(PGF_BODY);
    for (int i = 0; i < (int)NDEFS; i++) {
        int x0 = title_x(i);
        int tw = pgc_text_w(PGF_BODY, defs[i].title);
        bool lit = (open && cur_idx == i) || (!open && hot == i);
        if (lit)
            pgc_rrect(c, x0, w->rect.y + 2, tw + PAD_X * 2, w->rect.h - 4, 5, t->surface3, 255);
        pgc_text(c, PGF_BODY, x0 + PAD_X, w->rect.y + (w->rect.h - fh) / 2, defs[i].title, lit ? t->text : t->text2);
    }
}

static bool bar_input(struct pg_widget *w, const struct pg_event *e, void *data)
{
    (void)data;
    (void)w;
    switch (e->type) {
    case PG_EVENT_POINTER_MOVE: {
        int i = title_at(e->x);
        if (open_now()) {
            if (i >= 0 && i != cur_idx)
                open_menu(i);
        } else if (i != hot) {
            hot = i;
            pg_widget_queue_draw(bar);
        }
        return true;
    }
    case PG_EVENT_LEAVE:
        if (hot != -1) {
            hot = -1;
            pg_widget_queue_draw(bar);
        }
        return false;
    case PG_EVENT_POINTER_DOWN: {
        if (e->button != PG_BUTTON_LEFT)
            return false;
        int i = title_at(e->x);
        if (i < 0)
            return true;
        if (open_now() && cur_idx == i)
            menubar_close();
        else
            open_menu(i);
        return true;
    }
    case PG_EVENT_POINTER_UP:
        return true;
    default:
        return false;
    }
}

struct pg_widget *menubar_new(struct pg_window *win, const struct menubar_hooks *h)
{
    owner = win;
    hooks = h;
    bar = pg_drawing_new(200, BAR_H, bar_draw, bar_input, NULL);
    if (bar)
        pg_widget_set_expand(bar, true, false);
    return bar;
}

bool menubar_key(const struct pg_event *e)
{
    if ((e->mods & PG_MOD_ALT) && !(e->mods & PG_MOD_CTRL))
        for (int i = 0; i < (int)NDEFS; i++)
            if (e->key == defs[i].alt_key) {
                open_menu(i);
                return true;
            }
    return false;
}
