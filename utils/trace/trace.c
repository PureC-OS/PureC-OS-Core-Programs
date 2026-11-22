#include "../lib/common.h"
#include "../../tracelib/sysnames.h"
#include "libc/include/purec.h"
#include <signal.h>
#include <sys/wait.h>

#define MAX_PROCS 512
#define MAX_PENDING 256
#define BATCH 64

struct proc {
    uint32_t pid;
    uint32_t ppid;
    int parent;
    bool alive;
    char name[24];
    char prefix[160];
    char node_prefix[160];
};

struct pending {
    bool used;
    bool unfinished;
    uint32_t tid;
    uint32_t pid;
    struct trace_record rec;
};

struct stat_entry {
    uint64_t calls;
    uint64_t errors;
    uint64_t total_ns;
};

static struct proc procs[MAX_PROCS];
static int nprocs;
static struct pending pending[MAX_PENDING];
static struct stat_entry stats[512];
static uint64_t filter[TRACE_MASK_WORDS];
static uint64_t breaks[TRACE_MASK_WORDS];
static bool have_filter, have_breaks;
static bool opt_follow = true, opt_color, opt_summary, opt_abs, opt_dur = true, opt_raw;
static FILE *out;
static int session = -1;
static volatile sig_atomic_t got_int;
static uint32_t root_pid;
static uint64_t last_dropped;
static uint32_t slow_ms = 100;

static void on_int(int sig)
{
    (void)sig;
    got_int++;
}

static bool mask_has(const uint64_t *m, uint32_t nr)
{
    return nr < TRACE_MASK_WORDS * 64 && (m[nr / 64] >> (nr % 64) & 1);
}

static const char *col(const char *c)
{
    return opt_color ? c : "";
}

static int parse_list(const char *text, uint64_t *mask)
{
    char *copy = strdup(text);
    if (!copy)
        return -1;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        int nr = tl_sysnr(tok);
        if (nr < 0 || nr >= TRACE_MASK_WORDS * 64) {
            util_err("unknown system call '%s'", tok);
            free(copy);
            return -1;
        }
        mask[nr / 64] |= 1ULL << (nr % 64);
    }
    free(copy);
    return 0;
}

static int find_proc(uint32_t pid)
{
    for (int i = 0; i < nprocs; i++)
        if (procs[i].pid == pid)
            return i;
    return -1;
}

static void put_line(const char *prefix, const char *body)
{
    fprintf(out, "%s%s\n", prefix, body);
}

static int add_proc(const struct trace_record *r)
{
    int slot = -1;
    if (nprocs < MAX_PROCS)
        slot = nprocs++;
    else
        for (int i = 0; i < MAX_PROCS; i++)
            if (!procs[i].alive) {
                slot = i;
                break;
            }
    if (slot < 0)
        slot = 0;
    struct proc *p = &procs[slot];
    memset(p, 0, sizeof(*p));
    p->pid = r->pid;
    p->ppid = r->ppid;
    p->alive = true;
    memcpy(p->name, r->comm, sizeof(p->name) - 1);
    p->parent = r->pid == root_pid ? -1 : find_proc(r->ppid);
    char line[256];
    if (p->parent >= 0 && r->pid != root_pid) {
        struct proc *q = &procs[p->parent];
        snprintf(p->prefix, sizeof(p->prefix), "%s%s", q->prefix, opt_color ? "\x1b[90m\xe2\x94\x82\x1b[0m  " : "\xe2\x94\x82  ");
        snprintf(p->node_prefix, sizeof(p->node_prefix), "%s", q->prefix);
        snprintf(line, sizeof(line), "%s%s\xe2\x94\x9c\xe2\x94\x80%s %s[%u %s]%s", q->prefix, col(C_GRAY), col(C_RESET),
                 col(C_BOLD C_MAGENTA), r->pid, r->comm, col(C_RESET));
        fprintf(out, "%s\n", line);
    } else {
        snprintf(line, sizeof(line), "%s[%u %s]%s", col(C_BOLD C_MAGENTA), r->pid, r->comm, col(C_RESET));
        fprintf(out, "%s\n", line);
    }
    return slot;
}

static struct proc *touch_proc(const struct trace_record *r)
{
    int i = find_proc(r->pid);
    if (i < 0)
        i = add_proc(r);
    memcpy(procs[i].name, r->comm, sizeof(procs[i].name) - 1);
    return &procs[i];
}

static void fmt_time(const struct trace_record *r, char *out_text, size_t cap)
{
    if (!opt_abs) {
        out_text[0] = 0;
        return;
    }
    snprintf(out_text, cap, "%s[%5llu.%06llu]%s ", col(C_GRAY), (unsigned long long)(r->time_ns / 1000000000ULL),
             (unsigned long long)(r->time_ns % 1000000000ULL / 1000), col(C_RESET));
}

static void fmt_duration_tag(uint64_t ns, char *tag, size_t cap)
{
    if (!opt_dur || !ns) {
        tag[0] = 0;
        return;
    }
    char d[32];
    tl_format_duration(ns, d, sizeof(d));
    const char *c = ns / 1000000ULL >= slow_ms ? C_YELLOW : C_GRAY;
    snprintf(tag, cap, "  %s[%s]%s", col(c), d, col(C_RESET));
}

static void print_call(struct proc *p, const struct trace_record *enter, const struct trace_record *exit_rec, bool resumed)
{
    char tmp[24], args[512], ret[64], dur[64], ts[64], line[1200];
    const char *name = tl_sysname(enter->nr, tmp, sizeof(tmp));
    tl_format_args(enter, args, sizeof(args), !opt_raw);
    fmt_time(enter, ts, sizeof(ts));
    const char *pre = p ? p->prefix : "";
    if (!exit_rec) {
        snprintf(line, sizeof(line), "%s%s%s%s(%s) %s<unfinished ...>%s", pre, ts, col(C_CYAN), name, args, col(C_GRAY),
                 col(C_RESET));
        fprintf(out, "%s\n", line);
        return;
    }
    bool err = tl_format_ret(enter->nr, exit_rec->ret, ret, sizeof(ret));
    fmt_duration_tag(exit_rec->duration_ns, dur, sizeof(dur));
    if (resumed)
        snprintf(line, sizeof(line), "%s%s%s<... %s resumed>%s = %s%s%s%s", pre, ts, col(C_GRAY), name, col(C_RESET),
                 err ? col(C_RED) : col(C_GREEN), ret, col(C_RESET), dur);
    else
        snprintf(line, sizeof(line), "%s%s%s%s%s(%s) = %s%s%s%s", pre, ts, col(C_CYAN), name, col(C_RESET), args,
                 err ? col(C_RED) : col(C_GREEN), ret, col(C_RESET), dur);
    fprintf(out, "%s\n", line);
}

static void account(const struct trace_record *enter, const struct trace_record *exit_rec)
{
    if (enter->nr >= 512)
        return;
    struct stat_entry *s = &stats[enter->nr];
    s->calls++;
    if (exit_rec) {
        if (tl_ret_is_error(exit_rec->ret))
            s->errors++;
        s->total_ns += exit_rec->duration_ns;
    }
}

static struct pending *find_pending(uint32_t tid)
{
    for (int i = 0; i < MAX_PENDING; i++)
        if (pending[i].used && pending[i].tid == tid)
            return &pending[i];
    return NULL;
}

static void flush_pid(uint32_t pid, uint32_t except_tid, bool final)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        struct pending *q = &pending[i];
        if (!q->used || q->pid != pid || q->tid == except_tid)
            continue;
        if (final) {
            if (!opt_summary) {
                struct proc *p = find_proc(pid) >= 0 ? &procs[find_proc(pid)] : NULL;
                char tmp[24], args[512], ts[64];
                tl_format_args(&q->rec, args, sizeof(args), !opt_raw);
                fmt_time(&q->rec, ts, sizeof(ts));
                fprintf(out, "%s%s%s%s%s(%s) = ?\n", p ? p->prefix : "", ts, col(C_CYAN),
                        tl_sysname(q->rec.nr, tmp, sizeof(tmp)), col(C_RESET), args);
            }
            account(&q->rec, NULL);
            q->used = false;
        } else if (!q->unfinished) {
            if (!opt_summary)
                print_call(find_proc(pid) >= 0 ? &procs[find_proc(pid)] : NULL, &q->rec, NULL, false);
            q->unfinished = true;
        }
    }
}

static void do_continue(void)
{
    pc_trace_control(session, TRACE_OP_CONTINUE, NULL);
}

static bool prompt_stop(const struct trace_record *r)
{
    char tmp[24], args[512];
    tl_format_args(r, args, sizeof(args), !opt_raw);
    fflush(out);
    if (!isatty(0)) {
        do_continue();
        return true;
    }
    fprintf(stderr, "%s[stopped: pid %u %s(%s)]%s Enter=continue, q=detach, c=continue all\n> ", col(C_BOLD C_YELLOW),
            r->pid, tl_sysname(r->nr, tmp, sizeof(tmp)), args, col(C_RESET));
    fflush(stderr);
    char buf[64];
    if (!fgets(buf, sizeof(buf), stdin)) {
        do_continue();
        return true;
    }
    if (buf[0] == 'q' || buf[0] == 'Q')
        return false;
    if (buf[0] == 'c' || buf[0] == 'C') {
        uint64_t zero[TRACE_MASK_WORDS] = {0};
        pc_trace_control(session, TRACE_OP_SET_STOP_MASK, zero);
    }
    do_continue();
    return true;
}

static bool handle(const struct trace_record *r)
{
    if (r->type == TRACE_EV_STOPPED)
        return prompt_stop(r);
    struct proc *p = touch_proc(r);
    if (r->type == TRACE_EV_PROC_EXIT) {
        flush_pid(r->pid, 0xffffffffu, true);
        p->alive = false;
        if (!opt_summary) {
            char what[64], line[200];
            tl_format_exit(r, what, sizeof(what));
            bool bad = r->args[0] != 0 || r->ret != 0;
            if (p->parent >= 0 && r->pid != root_pid)
                snprintf(line, sizeof(line), "%s%s\xe2\x94\x94\xe2\x94\x80%s %s+++ [%u %s] %s +++%s", p->node_prefix,
                         col(C_GRAY), col(C_RESET), bad ? col(C_RED) : col(C_GRAY), r->pid, p->name, what, col(C_RESET));
            else
                snprintf(line, sizeof(line), "%s+++ [%u %s] %s +++%s", bad ? col(C_RED) : col(C_GRAY), r->pid, p->name,
                         what, col(C_RESET));
            fprintf(out, "%s\n", line);
        }
        return true;
    }
    if (r->type == TRACE_EV_ENTER) {
        flush_pid(r->pid, r->tid, false);
        if (have_filter && !mask_has(filter, r->nr))
            return true;
        struct pending *q = find_pending(r->tid);
        if (!q)
            for (int i = 0; i < MAX_PENDING; i++)
                if (!pending[i].used) {
                    q = &pending[i];
                    break;
                }
        if (!q)
            return true;
        q->used = true;
        q->unfinished = false;
        q->tid = r->tid;
        q->pid = r->pid;
        q->rec = *r;
        return true;
    }
    if (r->type == TRACE_EV_EXIT) {
        flush_pid(r->pid, r->tid, false);
        struct pending *q = find_pending(r->tid);
        if (!q || q->rec.nr != r->nr)
            return true;
        account(&q->rec, r);
        if (!opt_summary)
            print_call(p, &q->rec, r, q->unfinished);
        q->used = false;
    }
    return true;
}

static int stat_cmp(const void *a, const void *b)
{
    const struct stat_entry *x = &stats[*(const int *)a], *y = &stats[*(const int *)b];
    if (x->total_ns != y->total_ns)
        return x->total_ns < y->total_ns ? 1 : -1;
    return x->calls < y->calls ? 1 : x->calls > y->calls ? -1 : 0;
}

static void print_summary(void)
{
    int order[512], n = 0;
    uint64_t total = 0, calls = 0, errors = 0;
    for (int i = 0; i < 512; i++)
        if (stats[i].calls) {
            order[n++] = i;
            total += stats[i].total_ns;
            calls += stats[i].calls;
            errors += stats[i].errors;
        }
    if (!n)
        return;
    qsort(order, (size_t)n, sizeof(int), stat_cmp);
    fprintf(out, "%s%7s %12s %10s %8s %7s  %s%s\n", col(C_BOLD), "% time", "total", "avg", "calls", "errors", "syscall",
            col(C_RESET));
    for (int i = 0; i < n; i++) {
        struct stat_entry *s = &stats[order[i]];
        char tot[32], avg[32], tmp[24];
        tl_format_duration(s->total_ns, tot, sizeof(tot));
        tl_format_duration(s->calls ? s->total_ns / s->calls : 0, avg, sizeof(avg));
        unsigned pct = total ? (unsigned)(s->total_ns * 1000 / total) : 0;
        fprintf(out, "%4u.%u%% %12s %10s %8llu %s%7llu%s  %s\n", pct / 10, pct % 10, tot, avg,
                (unsigned long long)s->calls, s->errors ? col(C_RED) : "", (unsigned long long)s->errors, col(C_RESET),
                tl_sysname((uint32_t)order[i], tmp, sizeof(tmp)));
    }
    char tot[32];
    tl_format_duration(total, tot, sizeof(tot));
    fprintf(out, "%s%7s %12s %10s %8llu %7llu  total%s\n", col(C_BOLD), "100%", tot, "", (unsigned long long)calls,
            (unsigned long long)errors, col(C_RESET));
}

static void check_dropped(void)
{
    struct trace_status st;
    if (pc_trace_control(session, TRACE_OP_STATUS, &st) < 0)
        return;
    if (st.dropped != last_dropped) {
        fprintf(out, "%s+++ %llu records dropped by the kernel +++%s\n", col(C_RED),
                (unsigned long long)(st.dropped - last_dropped), col(C_RESET));
        last_dropped = st.dropped;
    }
}

static int live_count(void)
{
    int n = 0;
    for (int i = 0; i < nprocs; i++)
        n += procs[i].alive;
    return n;
}

static int run_loop(bool command_mode)
{
    static struct trace_record batch[BATCH];
    int grace = 0;
    for (;;) {
        if (got_int >= (command_mode ? 2 : 1))
            break;
        int n = pc_trace_read(session, batch, BATCH, 100);
        if (n < 0)
            break;
        bool keep = true;
        for (int i = 0; i < n && keep; i++)
            keep = handle(&batch[i]);
        fflush(out);
        if (!keep)
            break;
        if (n > 0) {
            grace = 0;
            check_dropped();
            continue;
        }
        if (nprocs && live_count() == 0) {
            if (++grace >= 2)
                break;
        }
    }
    check_dropped();
    for (int i = 0; i < MAX_PENDING; i++)
        if (pending[i].used)
            flush_pid(pending[i].pid, 0xffffffffu, true);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: %s [-cnrtTdfF] [-e LIST] [-b LIST] [-o FILE] COMMAND [ARGS...]\n"
            "       %s [options] -p PID\n",
            prog, prog);
}

int main(int argc, char **argv)
{
    util_init(argc, argv);
    int pid_arg = 0;
    const char *outfile = NULL;
    bool no_color = false;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1])
            break;
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (!strcmp(a, "--help")) {
            usage();
            return 0;
        }
        for (int k = 1; a[k]; k++) {
            char c = a[k];
            const char *val = NULL;
            if (c == 'e' || c == 'b' || c == 'o' || c == 'p') {
                if (a[k + 1])
                    val = a + k + 1;
                else if (i + 1 < argc)
                    val = argv[++i];
                else
                    return util_err("option requires an argument -- '%c'", c);
            }
            switch (c) {
            case 'e':
                if (parse_list(val, filter))
                    return 2;
                have_filter = true;
                break;
            case 'b':
                if (parse_list(val, breaks))
                    return 2;
                have_breaks = true;
                break;
            case 'o': outfile = val; break;
            case 'p': pid_arg = atoi(val); if (pid_arg <= 0) return util_err("invalid PID '%s'", val); break;
            case 'c': opt_summary = true; break;
            case 'n': no_color = true; break;
            case 'r': opt_raw = true; break;
            case 't': opt_abs = true; break;
            case 'T': opt_dur = true; break;
            case 'd': opt_dur = false; break;
            case 'f': opt_follow = true; break;
            case 'F': opt_follow = false; break;
            default:
                util_err("invalid option -- '%c'", c);
                usage();
                return 2;
            }
            if (val)
                break;
        }
    }
    if (!pid_arg && i >= argc) {
        usage();
        return 2;
    }
    out = stdout;
    if (outfile) {
        out = fopen(outfile, "w");
        if (!out)
            return util_perr(outfile);
        no_color = true;
    }
    opt_color = !no_color && isatty(fileno(out));

    signal(SIGINT, on_int);
    uint32_t flags = opt_follow ? TRACE_FOLLOW : 0;
    int status_fd[2] = {-1, -1};
    pid_t child = 0;
    uint32_t target;

    if (pid_arg) {
        target = (uint32_t)pid_arg;
    } else {
        if (pipe(status_fd) < 0)
            return util_perr("pipe");
        child = fork();
        if (child < 0)
            return util_perr("fork");
        if (child == 0) {
            close(status_fd[1]);
            char b;
            signal(SIGINT, SIG_DFL);
            if (read(status_fd[0], &b, 1) != 1 || b != 'g')
                _exit(126);
            close(status_fd[0]);
            execvp(argv[i], argv + i);
            fprintf(stderr, "%s: cannot run '%s': %s\n", prog, argv[i], strerror(errno));
            _exit(127);
        }
        close(status_fd[0]);
        target = (uint32_t)child;
    }
    root_pid = target;
    session = pc_trace_attach(target, flags);
    if (session < 0) {
        util_err("cannot attach to pid %u (code %d)%s", target, session,
                 session == -1 ? ": not permitted, PID 1, kernel thread, itself or already traced" : "");
        if (child) {
            kill(child, SIGKILL);
            waitpid(child, NULL, 0);
        }
        return 1;
    }
    if (have_breaks && pc_trace_control(session, TRACE_OP_SET_STOP_MASK, breaks) < 0)
        util_err("cannot set breakpoints");
    if (child) {
        char go = 'g';
        write(status_fd[1], &go, 1);
        close(status_fd[1]);
    }
    run_loop(child != 0);
    pc_trace_control(session, TRACE_OP_DETACH, NULL);
    if (opt_summary)
        print_summary();
    int rc = 0;
    if (child) {
        int st = 0;
        if (got_int >= 2)
            kill(child, SIGTERM);
        if (waitpid(child, &st, got_int >= 2 ? WNOHANG : 0) > 0)
            rc = (st & 0x7f) ? 128 + (st & 0x7f) : (st >> 8) & 0xff;
    }
    fflush(out);
    if (out != stdout)
        fclose(out);
    return rc;
}
