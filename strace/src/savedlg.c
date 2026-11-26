#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "pg/pg.h"
#include "savedlg.h"

struct ent {
    char *name;
    bool dir;
};

static struct {
    struct pg_window *win;
    struct pg_widget *entry, *list, *note, *button, *dirlabel;
    char *dir;
    struct ent *ents;
    int count;
    bool confirm;
    savedlg_fn fn;
} sd;

static int ent_cmp(const void *a, const void *b)
{
    const struct ent *x = a, *y = b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void free_ents(void)
{
    for (int i = 0; i < sd.count; i++)
        free(sd.ents[i].name);
    free(sd.ents);
    sd.ents = NULL;
    sd.count = 0;
}

static char *join(const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    char *p = malloc(dl + strlen(name) + 2);
    if (!p)
        return NULL;
    memcpy(p, dir, dl);
    if (dl && dir[dl - 1] != '/')
        p[dl++] = '/';
    strcpy(p + dl, name);
    return p;
}

static char *dir_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return strdup("/");
    size_t n = (size_t)(slash - path);
    char *d = malloc(n + 2);
    if (!d)
        return NULL;
    if (!n) {
        strcpy(d, "/");
        return d;
    }
    memcpy(d, path, n);
    d[n] = 0;
    return d;
}

static void set_note(const char *text, uint32_t color)
{
    pg_label_set_color(sd.note, color);
    pg_label_set_text(sd.note, text);
}

static void reset_confirm(void)
{
    if (sd.confirm) {
        sd.confirm = false;
        pg_button_set_label(sd.button, "Save");
    }
}

static void fill_list(void)
{
    free_ents();
    pg_list_clear(sd.list);
    pg_label_set_text(sd.dirlabel, sd.dir);
    int cap = 0;
    if (strcmp(sd.dir, "/")) {
        sd.ents = malloc(sizeof(*sd.ents));
        if (sd.ents) {
            sd.ents[0].name = strdup("..");
            sd.ents[0].dir = true;
            sd.count = 1;
            cap = 1;
        }
    }
    DIR *d = opendir(sd.dir);
    int first = sd.count;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            if (sd.count >= cap) {
                int nc = cap ? cap * 2 : 64;
                struct ent *n = realloc(sd.ents, sizeof(*n) * (size_t)nc);
                if (!n)
                    break;
                sd.ents = n;
                cap = nc;
            }
            char *full = join(sd.dir, e->d_name);
            struct stat st;
            bool isdir = full && stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            free(full);
            sd.ents[sd.count].name = strdup(e->d_name);
            sd.ents[sd.count].dir = isdir;
            if (sd.ents[sd.count].name)
                sd.count++;
        }
        closedir(d);
    }
    if (sd.count > first)
        qsort(sd.ents + first, (size_t)(sd.count - first), sizeof(*sd.ents), ent_cmp);
    for (int i = 0; i < sd.count; i++) {
        char *title = join("", sd.ents[i].name);
        if (sd.ents[i].dir && title) {
            char *t2 = realloc(title, strlen(title) + 2);
            if (t2) {
                title = t2;
                strcat(title, "/");
            }
        }
        pg_list_add(sd.list, PG_NO_ICON, title ? title : sd.ents[i].name, sd.ents[i].dir ? "folder" : "file");
        free(title);
    }
}

static void goto_dir(const char *path)
{
    char *np = NULL;
    const char *base = sd.dir;
    if (!strcmp(path, "..")) {
        np = dir_of(base);
    } else
        np = join(base, path);
    if (!np)
        return;
    const char *cur = pg_entry_text(sd.entry);
    const char *slash = strrchr(cur, '/');
    const char *name = slash ? slash + 1 : cur;
    char *full = join(np, name);
    free(sd.dir);
    sd.dir = np;
    fill_list();
    if (full) {
        pg_entry_set_text(sd.entry, full);
        free(full);
    }
    reset_confirm();
}

static void do_save(void)
{
    const char *path = pg_entry_text(sd.entry);
    if (!path[0]) {
        set_note("Enter a file name.", PG_COLOR_DANGER);
        return;
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            free(sd.dir);
            sd.dir = strdup(path);
            if (sd.dir)
                fill_list();
            set_note("That is a folder. Enter a file name after it.", PG_COLOR_WARNING);
            return;
        }
        if (!sd.confirm) {
            sd.confirm = true;
            pg_button_set_label(sd.button, "Overwrite");
            set_note("This file already exists. Click Overwrite to replace it.", PG_COLOR_WARNING);
            return;
        }
    }
    const char *err = sd.fn ? sd.fn(path) : NULL;
    if (err) {
        reset_confirm();
        set_note(err, PG_COLOR_DANGER);
        return;
    }
    pg_window_close(sd.win);
}

static void on_save(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    do_save();
}

static void on_cancel(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    pg_window_close(sd.win);
}

static void on_entry(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    reset_confirm();
}

static void on_select(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    int i = pg_list_selected(sd.list);
    if (i < 0 || i >= sd.count || sd.ents[i].dir)
        return;
    char *full = join(sd.dir, sd.ents[i].name);
    if (full) {
        pg_entry_set_text(sd.entry, full);
        free(full);
    }
    reset_confirm();
    set_note("", PG_COLOR_TEXT2);
}

static void on_activate(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    int i = pg_list_selected(sd.list);
    if (i < 0 || i >= sd.count)
        return;
    if (sd.ents[i].dir) {
        char *name = strdup(sd.ents[i].name);
        if (name) {
            goto_dir(name);
            free(name);
        }
    } else
        do_save();
}

static bool on_close(struct pg_window *win, void *data)
{
    (void)win;
    (void)data;
    sd.win = NULL;
    free_ents();
    free(sd.dir);
    sd.dir = NULL;
    return true;
}

void savedlg_close(void)
{
    if (sd.win)
        pg_window_close(sd.win);
}

void savedlg_open(const char *default_path, savedlg_fn fn)
{
    if (sd.win) {
        pg_window_raise(sd.win);
        return;
    }
    sd.fn = fn;
    sd.confirm = false;
    sd.win = pg_window_new("Export JSON", 560, 560, PG_WINDOW_RESIZABLE);
    if (!sd.win)
        return;
    pg_window_set_min_size(sd.win, 400, 360);
    pg_window_on_close(sd.win, on_close, NULL);
    struct pg_widget *root = pg_vbox(10);
    pg_box_set_padding(root, 16, 14, 16, 14);
    pg_widget_add(root, pg_label_new_font("Save the trace as a JSON file", PGF_SMALL, PG_COLOR_TEXT2));
    sd.entry = pg_entry_new("Path of the file");
    pg_entry_set_text(sd.entry, default_path);
    pg_widget_on(sd.entry, PG_SIGNAL_CHANGED, on_entry, NULL);
    pg_widget_on(sd.entry, PG_SIGNAL_ACTIVATE, on_save, NULL);
    pg_widget_add(root, sd.entry);
    sd.dirlabel = pg_label_new_font("", PGF_SMALL, PG_COLOR_TEXT2);
    pg_label_set_ellipsize(sd.dirlabel, true);
    pg_widget_add(root, sd.dirlabel);
    sd.list = pg_list_new();
    pg_widget_set_expand(sd.list, true, true);
    pg_widget_on(sd.list, PG_SIGNAL_CHANGED, on_select, NULL);
    pg_widget_on(sd.list, PG_SIGNAL_ACTIVATE, on_activate, NULL);
    struct pg_widget *scroll = pg_scroll_new();
    pg_widget_set_expand(scroll, true, true);
    pg_widget_add(scroll, sd.list);
    pg_widget_add(root, scroll);
    sd.note = pg_label_new_font("", PGF_SMALL, PG_COLOR_TEXT2);
    pg_label_set_wrap(sd.note, true);
    pg_widget_add(root, sd.note);
    struct pg_widget *row = pg_hbox(8);
    struct pg_widget *sp = pg_spacer_new();
    pg_widget_set_expand(sp, true, false);
    pg_widget_add(row, sp);
    struct pg_widget *cancel = pg_button_new("Cancel", PG_NO_ICON, PG_BUTTON_SECONDARY);
    pg_widget_on(cancel, PG_SIGNAL_CLICKED, on_cancel, NULL);
    pg_widget_add(row, cancel);
    sd.button = pg_button_new("Save", PG_NO_ICON, PG_BUTTON_PRIMARY);
    pg_widget_on(sd.button, PG_SIGNAL_CLICKED, on_save, NULL);
    pg_widget_add(row, sd.button);
    pg_widget_add(root, row);
    pg_window_set_child(sd.win, root);
    sd.dir = dir_of(default_path);
    if (!sd.dir)
        sd.dir = strdup("/");
    fill_list();
    pg_widget_grab_focus(sd.entry);
}
