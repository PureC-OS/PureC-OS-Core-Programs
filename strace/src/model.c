#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include "model.h"

#define MAX_PENDING 256

struct model M;
struct proc P[MAX_PROCS];

static struct item *items;
static uint32_t items_cap;
static int release_fd = -1;

struct pend {
    bool used;
    uint32_t tid;
    uint32_t seq;
};

static struct pend pend[MAX_PENDING];

void model_init(void)
{
    memset(&M, 0, sizeof(M));
    M.session = -1;
    M.child_pid = -1;
    M.next_seq = 1;
    M.first_seq = 1;
}

bool model_active(void)
{
    return M.session >= 0;
}

bool item_valid(uint32_t seq)
{
    return seq && seq >= M.first_seq && seq < M.next_seq;
}

struct item *item_at(uint32_t seq)
{
    if (!item_valid(seq))
        return NULL;
    return &items[(seq - 1) % MAX_ITEMS];
}

void model_clear(void)
{
    free(M.command);
    M.command = NULL;
    for (int i = 0; i < MAX_PROCS; i++)
        free(P[i].st);
    memset(P, 0, sizeof(P));
    memset(pend, 0, sizeof(pend));
    M.nprocs = 0;
    M.events = 0;
    M.next_seq = 1;
    M.first_seq = 1;
    M.stop_changed = true;
}

static struct item *alloc_item(uint32_t *seq_out)
{
    uint32_t seq = M.next_seq;
    uint32_t slot = (seq - 1) % MAX_ITEMS;
    if (slot >= items_cap) {
        uint32_t cap = items_cap ? items_cap * 2 : 2048;
        if (cap > MAX_ITEMS)
            cap = MAX_ITEMS;
        struct item *n = realloc(items, (size_t)cap * sizeof(*items));
        if (!n)
            return NULL;
        items = n;
        items_cap = cap;
    }
    if (seq - M.first_seq >= MAX_ITEMS) {
        struct item *old = &items[slot];
        struct proc *p = &P[old->proc];
        if (p->used && p->first == M.first_seq)
            p->first = old->next;
        if (p->used && p->last == M.first_seq)
            p->last = 0;
        if (old->kind == IT_CHILD && P[old->child].used && P[old->child].parent == old->proc) {
            P[old->child].parent = -1;
            P[old->child].depth = 0;
        }
        M.first_seq++;
    }
    M.next_seq++;
    struct item *it = &items[slot];
    memset(it, 0, sizeof(*it));
    *seq_out = seq;
    return it;
}

static void link_item(int proc, uint32_t seq)
{
    struct proc *p = &P[proc];
    struct item *last = p->last ? item_at(p->last) : NULL;
    if (last)
        last->next = seq;
    else if (!p->first || !item_valid(p->first))
        p->first = seq;
    p->last = seq;
}

static int find_proc(uint32_t pid)
{
    for (int i = M.nprocs - 1; i >= 0; i--)
        if (P[i].used && P[i].pid == pid && P[i].alive)
            return i;
    return -1;
}

static int new_proc_slot(void)
{
    if (M.nprocs < MAX_PROCS)
        return M.nprocs++;
    for (int i = 0; i < MAX_PROCS; i++)
        if (!P[i].alive && (P[i].parent < 0 || !P[i].first || !item_valid(P[i].first))) {
            free(P[i].st);
            memset(&P[i], 0, sizeof(P[i]));
            return i;
        }
    return -1;
}

static int touch_proc(const struct trace_record *r)
{
    int i = find_proc(r->pid);
    if (i >= 0) {
        memcpy(P[i].name, r->comm, sizeof(P[i].name) - 1);
        return i;
    }
    i = new_proc_slot();
    if (i < 0)
        return -1;
    struct proc *p = &P[i];
    memset(p, 0, sizeof(*p));
    p->used = true;
    p->alive = true;
    p->expanded = true;
    p->pid = r->pid;
    p->ppid = r->ppid;
    p->parent = r->pid == M.root_pid ? -1 : find_proc(r->ppid);
    memcpy(p->name, r->comm, sizeof(p->name) - 1);
    if (p->parent >= 0) {
        p->depth = P[p->parent].depth + 1;
        uint32_t seq;
        struct item *it = alloc_item(&seq);
        if (it) {
            it->kind = IT_CHILD;
            it->proc = (uint16_t)p->parent;
            it->child = (uint16_t)i;
            link_item(p->parent, seq);
        }
    }
    return i;
}

static struct pend *pend_find(uint32_t tid)
{
    for (int i = 0; i < MAX_PENDING; i++)
        if (pend[i].used && pend[i].tid == tid)
            return &pend[i];
    return NULL;
}

static struct pend *pend_slot(uint32_t tid)
{
    struct pend *q = pend_find(tid);
    if (q)
        return q;
    for (int i = 0; i < MAX_PENDING; i++)
        if (!pend[i].used || !item_valid(pend[i].seq))
            return &pend[i];
    return &pend[0];
}

static void handle(const struct trace_record *r)
{
    M.events++;
    int pi = touch_proc(r);
    if (pi < 0)
        return;
    struct proc *p = &P[pi];
    uint32_t seq;
    if (r->type == TRACE_EV_ENTER) {
        struct item *it = alloc_item(&seq);
        if (!it)
            return;
        it->kind = IT_SYS;
        it->proc = (uint16_t)pi;
        it->rec = *r;
        link_item(pi, seq);
        struct pend *q = pend_slot(r->tid);
        q->used = true;
        q->tid = r->tid;
        q->seq = seq;
    } else if (r->type == TRACE_EV_EXIT) {
        struct pend *q = pend_find(r->tid);
        if (!q)
            return;
        struct item *it = item_at(q->seq);
        q->used = false;
        if (!it || it->rec.nr != r->nr)
            return;
        it->done = true;
        it->rec.ret = r->ret;
        it->rec.duration_ns = r->duration_ns;
        p->calls++;
        bool err = tl_ret_is_error(r->ret);
        if (err)
            p->errors++;
        if (!p->st)
            p->st = calloc(1, sizeof(*p->st));
        if (p->st && r->nr < NSYS) {
            p->st->calls[r->nr]++;
            p->st->ns[r->nr] += r->duration_ns;
            if (err)
                p->st->errors[r->nr]++;
        }
        if (p->stopped && p->stopped_seq == q->seq) {
            p->stopped = false;
            M.stop_changed = true;
        }
    } else if (r->type == TRACE_EV_STOPPED) {
        struct pend *q = pend_find(r->tid);
        p->stopped = true;
        p->stopped_seq = q ? q->seq : 0;
        M.stop_changed = true;
    } else if (r->type == TRACE_EV_PROC_EXIT) {
        struct item *it = alloc_item(&seq);
        if (it) {
            it->kind = IT_EXIT;
            it->proc = (uint16_t)pi;
            it->rec = *r;
            link_item(pi, seq);
        }
        p->alive = false;
        if (p->stopped) {
            p->stopped = false;
            M.stop_changed = true;
        }
        p->exited = true;
        p->exit_signal = (int)r->args[0];
        p->exit_code = (int)r->ret;
        tl_format_exit(r, p->exit_text, sizeof(p->exit_text));
        for (int i = 0; i < MAX_PENDING; i++)
            if (pend[i].used && item_valid(pend[i].seq)) {
                struct item *pi2 = item_at(pend[i].seq);
                if (pi2 && pi2->proc == pi)
                    pend[i].used = false;
            }
    }
}

bool model_any_stopped(void)
{
    for (int i = 0; i < M.nprocs; i++)
        if (P[i].used && P[i].alive && P[i].stopped)
            return true;
    return false;
}

static bool any_alive(void)
{
    for (int i = 0; i < M.nprocs; i++)
        if (P[i].used && P[i].alive)
            return true;
    return false;
}

bool model_poll(void)
{
    if (M.session < 0)
        return false;
    static struct trace_record batch[64];
    bool changed = false;
    for (int round = 0; round < 8; round++) {
        int n = pc_trace_read(M.session, batch, 64, 0);
        if (n < 0) {
            M.session = -1;
            M.stop_changed = true;
            return true;
        }
        if (n == 0)
            break;
        changed = true;
        for (int i = 0; i < n; i++)
            handle(&batch[i]);
    }
    if (M.child_pid > 0) {
        int st;
        if (waitpid(M.child_pid, &st, WNOHANG) > 0)
            M.child_pid = -1;
    }
    static int idle;
    if (changed)
        idle = 0;
    else if (M.nprocs && !any_alive() && ++idle > 20) {
        model_detach();
        idle = 0;
        return true;
    }
    return changed;
}

void model_apply_breaks(void)
{
    if (M.session >= 0)
        pc_trace_control(M.session, TRACE_OP_SET_STOP_MASK, M.breaks);
}

static void release_stops(void)
{
    for (int i = 0; i < M.nprocs; i++)
        P[i].stopped = false;
    M.stop_changed = true;
}

void model_continue(void)
{
    if (M.session < 0)
        return;
    if (M.stepping) {
        M.stepping = false;
        model_apply_breaks();
    }
    pc_trace_control(M.session, TRACE_OP_CONTINUE, NULL);
    release_stops();
}

void model_step(void)
{
    if (M.session < 0)
        return;
    uint64_t all[TRACE_MASK_WORDS];
    memset(all, 0xff, sizeof(all));
    M.stepping = true;
    pc_trace_control(M.session, TRACE_OP_SET_STOP_MASK, all);
    pc_trace_control(M.session, TRACE_OP_CONTINUE, NULL);
    release_stops();
}

void model_detach(void)
{
    if (M.session >= 0)
        pc_trace_control(M.session, TRACE_OP_DETACH, NULL);
    M.session = -1;
    M.stepping = false;
    M.stop_changed = true;
    for (int i = 0; i < MAX_PROCS; i++)
        if (P[i].used) {
            P[i].stopped = false;
            if (P[i].alive)
                strcpy(P[i].exit_text, "detached");
            P[i].alive = false;
        }
    memset(pend, 0, sizeof(pend));
    if (release_fd >= 0) {
        close(release_fd);
        release_fd = -1;
    }
}

static const char *begin_session(uint32_t pid)
{
    if (M.session >= 0)
        model_detach();
    int id = pc_trace_attach(pid, TRACE_FOLLOW);
    if (id < 0)
        return "cannot attach: not permitted, PID 1, kernel thread, this program or already traced";
    M.session = id;
    M.root_pid = pid;
    M.dropped = 0;
    M.stepping = false;
    M.stop_changed = true;
    model_apply_breaks();
    return NULL;
}

const char *model_attach(uint32_t pid)
{
    if (pid <= 1 || pid == (uint32_t)getpid())
        return "this process cannot be traced";
    const char *err = begin_session(pid);
    if (!err) {
        free(M.command);
        M.command = NULL;
    }
    return err;
}

static int split(char *line, char **argv, int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max - 1) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        char quote = 0;
        if (*p == '"' || *p == '\'')
            quote = *p++;
        argv[n++] = p;
        char *w = p;
        while (*p && (quote ? *p != quote : (*p != ' ' && *p != '\t'))) {
            if (*p == '\\' && quote == '"' && p[1])
                p++;
            *w++ = *p++;
        }
        if (*p)
            p++;
        *w = 0;
    }
    argv[n] = NULL;
    return n;
}

const char *model_start_command(const char *line)
{
    static char buf[512];
    char *argv[32];
    snprintf(buf, sizeof(buf), "%s", line ? line : "");
    if (!split(buf, argv, 32))
        return "enter a command to run";
    int fds[2];
    if (pipe(fds) < 0)
        return "cannot create a pipe";
    pid_t child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return "cannot fork";
    }
    if (child == 0) {
        char b;
        for (int fd = 3; fd < 256; fd++)
            if (fd != fds[0])
                close(fd);
        signal(SIGINT, SIG_DFL);
        if (read(fds[0], &b, 1) != 1 || b != 'g')
            _exit(126);
        close(fds[0]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[0]);
    const char *err = begin_session((uint32_t)child);
    if (err) {
        close(fds[1]);
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        return err;
    }
    M.child_pid = child;
    free(M.command);
    M.command = strdup(line);
    char go = 'g';
    write(fds[1], &go, 1);
    close(fds[1]);
    return NULL;
}

int model_roots(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < M.nprocs && n < max; i++)
        if (P[i].used && P[i].parent < 0)
            out[n++] = i;
    return n;
}
