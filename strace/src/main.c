#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pwd.h>
#include <unistd.h>
#include "pg/pg.h"
#include "model.h"
#include "view.h"

static struct {
    struct pg_window *win;
    struct pg_widget *cmd, *start, *attach, *pause, *detach, *clear, *filter, *breaks, *cont, *step, *auto_cb, *status;
    bool paused;
    bool dirty;
    uint64_t last_rebuild;
    uint64_t last_status;
    char message[160];
    bool message_bad;
} app;

static struct {
    struct pg_window *win;
    struct pg_widget *list, *button, *note;
    uint32_t pid[512];
    bool ok[512];
    int count;
} att;

static struct {
    struct pg_window *win;
    struct pg_widget *filter;
    struct pg_widget *box;
    struct pg_widget *checks[NSYS];
    uint32_t nr[NSYS];
    int count;
} brk;

static void set_message(const char *text, bool bad)
{
    snprintf(app.message, sizeof(app.message), "%s", text);
    app.message_bad = bad;
    pg_label_set_color(app.status, bad ? PG_COLOR_DANGER : PG_COLOR_TEXT2);
    pg_label_set_text(app.status, app.message);
}

static void update_status(void)
{
    char text[200];
    if (app.message[0] && app.message_bad)
        return;
    if (M.session >= 0) {
        struct trace_status st;
        if (pc_trace_control(M.session, TRACE_OP_STATUS, &st) >= 0)
            M.dropped = st.dropped;
        snprintf(text, sizeof(text), "Tracing pid %u  |  %llu events  |  %d rows  |  %llu dropped by the kernel%s",
                 M.root_pid, (unsigned long long)M.events, view_row_count(), (unsigned long long)M.dropped,
                 app.paused ? "  |  display paused" : "");
    } else
        snprintf(text, sizeof(text), "Not tracing  |  %llu events  |  %d rows%s", (unsigned long long)M.events,
                 view_row_count(), app.paused ? "  |  display paused" : "");
    pg_label_set_color(app.status, PG_COLOR_TEXT2);
    pg_label_set_text(app.status, text);
}

static void update_buttons(void)
{
    bool active = M.session >= 0;
    bool stopped = model_any_stopped();
    pg_widget_set_enabled(app.detach, active);
    pg_widget_set_enabled(app.cont, active && stopped);
    pg_widget_set_enabled(app.step, active && stopped);
    pg_button_set_label(app.pause, app.paused ? "Resume" : "Pause");
    pg_widget_queue_draw(app.cont);
    pg_widget_queue_draw(app.step);
    pg_widget_queue_draw(app.detach);
}

static void rebuild_now(void)
{
    view_rebuild();
    app.last_rebuild = pg_time_ms();
    app.dirty = false;
}

static bool tick(void *data)
{
    (void)data;
    bool changed = model_poll();
    uint64_t now = pg_time_ms();
    if (changed)
        app.dirty = true;
    bool stop_changed = M.stop_changed;
    if (stop_changed) {
        M.stop_changed = false;
        update_buttons();
        app.dirty = true;
    }
    if (app.dirty && !app.paused && now - app.last_rebuild >= 90) {
        rebuild_now();
        if (stop_changed && model_any_stopped())
            view_reveal_stopped();
    }
    if (now - app.last_status >= 250) {
        app.last_status = now;
        update_status();
    }
    return true;
}

static void begin_ui(const char *error)
{
    if (error) {
        set_message(error, true);
    } else {
        app.message[0] = 0;
        app.message_bad = false;
    }
    app.dirty = true;
    update_buttons();
    rebuild_now();
    update_status();
}

static void on_start(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    begin_ui(model_start_command(pg_entry_text(app.cmd)));
}

static void on_pause(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    app.paused = !app.paused;
    update_buttons();
    if (!app.paused)
        rebuild_now();
    update_status();
}

static void on_detach(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    model_detach();
    app.message[0] = 0;
    app.message_bad = false;
    update_buttons();
    rebuild_now();
    update_status();
}

static void on_clear(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    bool active = M.session >= 0;
    if (active)
        model_detach();
    model_clear();
    view_clear_selection();
    if (active)
        set_message("Tracing stopped because the trace was cleared", false);
    update_buttons();
    rebuild_now();
    update_status();
}

static void on_filter(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    view_set_filter(pg_entry_text(app.filter));
}

static void on_continue(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    model_continue();
    update_buttons();
    rebuild_now();
}

static void on_step(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    model_step();
    update_buttons();
    rebuild_now();
}

static void on_autoscroll(struct pg_widget *w, void *data)
{
    (void)data;
    view_set_autoscroll(pg_checkbox_active(w));
}

static void autoscroll_off(bool on)
{
    pg_checkbox_set_active(app.auto_cb, on);
}

static bool dialog_closed_att(struct pg_window *win, void *data)
{
    (void)win;
    (void)data;
    att.win = NULL;
    return true;
}

static const char *user_of(uid_t uid, char *buf, size_t cap)
{
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name)
        snprintf(buf, cap, "%s", pw->pw_name);
    else
        snprintf(buf, cap, "%u", (unsigned)uid);
    return buf;
}

struct pinfo {
    uint32_t pid;
    uint32_t uid;
    bool kernel;
    char name[40];
    char state[24];
};

static int pcmp(const void *a, const void *b)
{
    const struct pinfo *x = a, *y = b;
    return x->pid < y->pid ? -1 : x->pid > y->pid;
}

static bool read_status(const char *pid, struct pinfo *pi)
{
    char path[48], text[1024];
    snprintf(path, sizeof(path), "/proc/%s/status", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    memset(pi, 0, sizeof(*pi));
    for (char *save = text; save && *save;) {
        char *nl = strchr(save, '\n');
        char *ln = save;
        if (nl) {
            *nl = 0;
            save = nl + 1;
        } else
            save = NULL;
        char *colon = strchr(ln, ':');
        if (!colon)
            continue;
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        if (!strcmp(ln, "Name"))
            snprintf(pi->name, sizeof(pi->name), "%s", v);
        else if (!strcmp(ln, "State"))
            snprintf(pi->state, sizeof(pi->state), "%s", v);
        else if (!strcmp(ln, "Pid"))
            pi->pid = (uint32_t)atoi(v);
        else if (!strcmp(ln, "Uid"))
            pi->uid = (uint32_t)atoi(v);
        else if (!strcmp(ln, "Kernel"))
            pi->kernel = atoi(v) != 0;
    }
    return pi->pid && strcmp(pi->state, "exited");
}

static void att_fill(void)
{
    static struct pinfo infos[512];
    int n = 0;
    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < 512) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9')
                continue;
            if (read_status(e->d_name, &infos[n]))
                n++;
        }
        closedir(d);
    }
    qsort(infos, (size_t)n, sizeof(infos[0]), pcmp);
    pg_list_clear(att.list);
    uid_t me = getuid();
    pid_t self = getpid();
    att.count = n;
    for (int i = 0; i < n; i++) {
        struct pinfo *pi = &infos[i];
        char user[32], sub[96], title[80];
        user_of((uid_t)pi->uid, user, sizeof(user));
        snprintf(title, sizeof(title), "%s", pi->name);
        snprintf(sub, sizeof(sub), "PID %u  |  %s  |  %s", pi->pid, pi->kernel ? "kernel" : user, pi->state);
        bool ok = !pi->kernel && pi->pid > 1 && pi->pid != (uint32_t)self && (me == 0 || pi->uid == me);
        att.pid[i] = pi->pid;
        att.ok[i] = ok;
        int idx = pg_list_add(att.list, PG_NO_ICON, title, sub);
        if (!ok)
            pg_list_set_trailing(att.list, idx, pi->kernel ? "kernel thread" : pi->pid <= 1 ? "init" : pi->pid == (uint32_t)self ? "this program" : "not yours");
    }
    pg_widget_set_enabled(att.button, false);
    pg_label_set_text(att.note, "Select a process to trace. Kernel threads and PID 1 cannot be traced.");
}

static void att_changed(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    int i = pg_list_selected(att.list);
    pg_widget_set_enabled(att.button, i >= 0 && i < att.count && att.ok[i]);
}

static void att_do(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    int i = pg_list_selected(att.list);
    if (i < 0 || i >= att.count || !att.ok[i])
        return;
    const char *err = model_attach(att.pid[i]);
    if (err) {
        pg_label_set_color(att.note, PG_COLOR_DANGER);
        pg_label_set_text(att.note, err);
        return;
    }
    begin_ui(NULL);
    pg_window_close(att.win);
}

static void att_refresh(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    pg_label_set_color(att.note, PG_COLOR_TEXT2);
    att_fill();
}

static void att_cancel(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    pg_window_close(att.win);
}

static void on_attach(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    if (att.win) {
        pg_window_raise(att.win);
        return;
    }
    att.win = pg_window_new("Attach to process", 520, 560, PG_WINDOW_RESIZABLE);
    if (!att.win)
        return;
    pg_window_set_min_size(att.win, 360, 360);
    pg_window_on_close(att.win, dialog_closed_att, NULL);
    struct pg_widget *root = pg_vbox(10);
    pg_box_set_padding(root, 16, 14, 16, 14);
    att.note = pg_label_new_font("", PGF_SMALL, PG_COLOR_TEXT2);
    pg_label_set_wrap(att.note, true);
    pg_widget_add(root, att.note);
    att.list = pg_list_new();
    pg_widget_set_expand(att.list, true, true);
    pg_widget_on(att.list, PG_SIGNAL_CHANGED, att_changed, NULL);
    pg_widget_on(att.list, PG_SIGNAL_ACTIVATE, att_do, NULL);
    struct pg_widget *scroll = pg_scroll_new();
    pg_widget_set_expand(scroll, true, true);
    pg_widget_add(scroll, att.list);
    pg_widget_add(root, scroll);
    struct pg_widget *row = pg_hbox(8);
    struct pg_widget *refresh = pg_button_new("Refresh", PG_NO_ICON, PG_BUTTON_SECONDARY);
    pg_widget_on(refresh, PG_SIGNAL_CLICKED, att_refresh, NULL);
    pg_widget_add(row, refresh);
    struct pg_widget *sp = pg_spacer_new();
    pg_widget_set_expand(sp, true, false);
    pg_widget_add(row, sp);
    struct pg_widget *cancel = pg_button_new("Cancel", PG_NO_ICON, PG_BUTTON_SECONDARY);
    pg_widget_on(cancel, PG_SIGNAL_CLICKED, att_cancel, NULL);
    pg_widget_add(row, cancel);
    att.button = pg_button_new("Attach", PG_NO_ICON, PG_BUTTON_PRIMARY);
    pg_widget_on(att.button, PG_SIGNAL_CLICKED, att_do, NULL);
    pg_widget_add(row, att.button);
    pg_widget_add(root, row);
    pg_window_set_child(att.win, root);
    att_fill();
}

static bool dialog_closed_brk(struct pg_window *win, void *data)
{
    (void)win;
    (void)data;
    brk.win = NULL;
    return true;
}

static void brk_filter(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    const char *f = pg_entry_text(brk.filter);
    for (int i = 0; i < brk.count; i++) {
        bool show = !f[0];
        if (!show) {
            char tmp[24];
            const char *name = tl_sysname(brk.nr[i], tmp, sizeof(tmp));
            show = strstr(name, f) != NULL;
        }
        pg_widget_set_visible(brk.checks[i], show);
    }
    pg_widget_queue_layout(brk.box);
}

static void brk_clear(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    for (int i = 0; i < brk.count; i++)
        pg_checkbox_set_active(brk.checks[i], false);
}

static void brk_apply(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    memset(M.breaks, 0, sizeof(M.breaks));
    for (int i = 0; i < brk.count; i++)
        if (pg_checkbox_active(brk.checks[i]) && brk.nr[i] < TRACE_MASK_WORDS * 64)
            M.breaks[brk.nr[i] / 64] |= 1ULL << (brk.nr[i] % 64);
    model_apply_breaks();
    pg_window_close(brk.win);
}

static void brk_cancel(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    pg_window_close(brk.win);
}

static void on_breaks(struct pg_widget *w, void *data)
{
    (void)w;
    (void)data;
    if (brk.win) {
        pg_window_raise(brk.win);
        return;
    }
    brk.win = pg_window_new("Breakpoints", 420, 580, PG_WINDOW_RESIZABLE);
    if (!brk.win)
        return;
    pg_window_set_min_size(brk.win, 320, 320);
    pg_window_on_close(brk.win, dialog_closed_brk, NULL);
    struct pg_widget *root = pg_vbox(10);
    pg_box_set_padding(root, 16, 14, 16, 14);
    struct pg_widget *hint = pg_label_new_font("The traced process stops at the entry of the checked system calls.", PGF_SMALL, PG_COLOR_TEXT2);
    pg_label_set_wrap(hint, true);
    pg_widget_add(root, hint);
    brk.filter = pg_entry_new("Filter system calls");
    pg_widget_on(brk.filter, PG_SIGNAL_CHANGED, brk_filter, NULL);
    pg_widget_add(root, brk.filter);
    brk.box = pg_vbox(2);
    brk.count = 0;
    size_t total = tl_sys_count();
    for (size_t i = 0; i < total && brk.count < NSYS; i++) {
        uint32_t nr;
        const char *name;
        if (!tl_sys_at(i, &nr, &name) || nr >= TRACE_MASK_WORDS * 64)
            continue;
        char label[64];
        snprintf(label, sizeof(label), "%s  (%u)", name, nr);
        bool on = M.breaks[nr / 64] >> (nr % 64) & 1;
        struct pg_widget *cb = pg_checkbox_new(label, on);
        brk.checks[brk.count] = cb;
        brk.nr[brk.count] = nr;
        brk.count++;
        pg_widget_add(brk.box, cb);
    }
    struct pg_widget *scroll = pg_scroll_new();
    pg_widget_set_expand(scroll, true, true);
    pg_widget_add(scroll, brk.box);
    pg_widget_add(root, scroll);
    struct pg_widget *row = pg_hbox(8);
    struct pg_widget *clear = pg_button_new("Clear all", PG_NO_ICON, PG_BUTTON_SECONDARY);
    pg_widget_on(clear, PG_SIGNAL_CLICKED, brk_clear, NULL);
    pg_widget_add(row, clear);
    struct pg_widget *sp = pg_spacer_new();
    pg_widget_set_expand(sp, true, false);
    pg_widget_add(row, sp);
    struct pg_widget *cancel = pg_button_new("Cancel", PG_NO_ICON, PG_BUTTON_SECONDARY);
    pg_widget_on(cancel, PG_SIGNAL_CLICKED, brk_cancel, NULL);
    pg_widget_add(row, cancel);
    struct pg_widget *ok = pg_button_new("Apply", PG_NO_ICON, PG_BUTTON_PRIMARY);
    pg_widget_on(ok, PG_SIGNAL_CLICKED, brk_apply, NULL);
    pg_widget_add(row, ok);
    pg_widget_add(root, row);
    pg_window_set_child(brk.win, root);
}

static bool on_close(struct pg_window *win, void *data)
{
    (void)win;
    (void)data;
    model_detach();
    if (att.win)
        pg_window_close(att.win);
    if (brk.win)
        pg_window_close(brk.win);
    return true;
}

static bool on_key(struct pg_window *win, const struct pg_event *e, void *data)
{
    (void)data;
    if (e->type != PG_EVENT_KEY_DOWN)
        return false;
    if ((e->mods & PG_MOD_CTRL) && (e->key == PG_KEY_Q || e->key == PG_KEY_W)) {
        pg_window_close(win);
        return true;
    }
    if (e->key == PG_KEY_F1 + 4 && pg_widget_is_enabled(app.cont)) {
        on_continue(NULL, NULL);
        return true;
    }
    if (e->key == PG_KEY_F1 + 9 && pg_widget_is_enabled(app.step)) {
        on_step(NULL, NULL);
        return true;
    }
    return false;
}

static struct pg_widget *button(struct pg_widget *row, const char *label, enum pg_button_style style, pg_callback fn)
{
    struct pg_widget *b = pg_button_new(label, PG_NO_ICON, style);
    pg_widget_on(b, PG_SIGNAL_CLICKED, fn, NULL);
    pg_widget_add(row, b);
    return b;
}

int main(int argc, char **argv)
{
    if (!pg_init(argc, argv)) {
        fprintf(stderr, "strace: cannot connect to the display server\n");
        return 1;
    }
    model_init();
    app.win = pg_window_new("Syscall Tracer", 1100, 720, PG_WINDOW_RESIZABLE);
    if (!app.win) {
        fprintf(stderr, "strace: cannot create a window\n");
        return 1;
    }
    pg_window_set_min_size(app.win, 760, 480);
    pg_window_on_key(app.win, on_key, NULL);
    pg_window_on_close(app.win, on_close, NULL);

    struct pg_widget *body = pg_vbox(0);
    pg_box_set_background(body, PG_COLOR_BG, 0);

    struct pg_widget *bar1 = pg_hbox(8);
    pg_box_set_padding(bar1, 14, 12, 14, 4);
    app.cmd = pg_entry_new("Command to run, for example: ls /etc");
    pg_widget_set_expand(app.cmd, true, false);
    pg_widget_on(app.cmd, PG_SIGNAL_ACTIVATE, on_start, NULL);
    pg_widget_add(bar1, app.cmd);
    app.start = button(bar1, "Start", PG_BUTTON_PRIMARY, on_start);
    app.attach = button(bar1, "Attach to PID...", PG_BUTTON_SECONDARY, on_attach);
    app.pause = button(bar1, "Pause", PG_BUTTON_SECONDARY, on_pause);
    app.detach = button(bar1, "Detach", PG_BUTTON_SECONDARY, on_detach);
    app.clear = button(bar1, "Clear", PG_BUTTON_SECONDARY, on_clear);
    pg_widget_add(body, bar1);

    struct pg_widget *bar2 = pg_hbox(8);
    pg_box_set_padding(bar2, 14, 4, 14, 8);
    app.filter = pg_entry_new("Filter by system call or string");
    pg_widget_set_size(app.filter, 280, -1);
    pg_widget_on(app.filter, PG_SIGNAL_CHANGED, on_filter, NULL);
    pg_widget_add(bar2, app.filter);
    app.breaks = button(bar2, "Breakpoints...", PG_BUTTON_SECONDARY, on_breaks);
    app.cont = button(bar2, "Continue", PG_BUTTON_PRIMARY, on_continue);
    app.step = button(bar2, "Step", PG_BUTTON_SECONDARY, on_step);
    struct pg_widget *sp = pg_spacer_new();
    pg_widget_set_expand(sp, true, false);
    pg_widget_add(bar2, sp);
    app.auto_cb = pg_checkbox_new("Autoscroll", true);
    pg_widget_set_align(app.auto_cb, PG_ALIGN_FILL, PG_ALIGN_CENTER);
    pg_widget_on(app.auto_cb, PG_SIGNAL_CHANGED, on_autoscroll, NULL);
    pg_widget_add(bar2, app.auto_cb);
    pg_widget_add(body, bar2);

    pg_widget_add(body, view_tree_new());

    static const char *const tab_labels[2] = {"Details", "Statistics"};
    struct pg_widget *tabs = pg_tabs_new(tab_labels, 2);
    pg_widget_add(body, tabs);
    struct pg_widget *stack = pg_stack_new();
    pg_widget_set_size(stack, -1, 190);
    pg_widget_set_expand(stack, true, false);
    pg_widget_add(stack, view_detail_new());
    pg_widget_add(stack, view_stats_new());
    pg_tabs_bind_stack(tabs, stack);
    pg_widget_add(body, stack);

    app.status = pg_label_new_font("", PGF_SMALL, PG_COLOR_TEXT2);
    pg_widget_set_margins(app.status, 14, 6, 14, 8);
    pg_label_set_ellipsize(app.status, true);
    pg_widget_add(body, app.status);

    pg_window_set_child(app.win, body);
    view_on_autoscroll(autoscroll_off);
    update_buttons();
    rebuild_now();
    update_status();
    pg_timer_add(30, tick, NULL);
    int code = pg_run();
    model_detach();
    pg_shutdown();
    return code;
}
