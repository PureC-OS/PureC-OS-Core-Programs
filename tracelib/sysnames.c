#include "sysnames.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>

#include "sysnames_table.inc"

struct sig_entry {
    const char *name;
    uint32_t value;
};

struct kind_table {
    const char *name;
    const char *kinds;
};

static const struct kind_table kinds[] = {
    {"fd_open", "sO"},
    {"fd_read", "dpz"},
    {"fd_write", "dpz"},
    {"fd_ctl", "dxx"},
    {"fd_stat", "dp"},
    {"fd_path", "dpz"},
    {"fd_seek", "diu"},
    {"fd_truncate", "dz"},
    {"file_open", "sa"},
    {"file_read", "dpz"},
    {"file_write", "dpz"},
    {"file_close", "d"},
    {"file_delete", "s"},
    {"file_rename", "ss"},
    {"file_move", "ss"},
    {"file_create", "sa"},
    {"dir_create", "s"},
    {"dir_list", "spu"},
    {"dir_list_long", "spu"},
    {"file_stat", "sp"},
    {"file_stat_ex", "sp"},
    {"rename_path", "ss"},
    {"chdir", "s"},
    {"getcwd", "pz"},
    {"chmod", "su"},
    {"chown", "sii"},
    {"kill", "iS"},
    {"sigaction", "Spp"},
    {"sigprocmask", "ipp"},
    {"sigreturn", ""},
    {"sigpending", "p"},
    {"sigaltstack", "pp"},
    {"pause", ""},
    {"exec", "s"},
    {"execve", "spp"},
    {"waitpid", "ipW"},
    {"wait", "p"},
    {"fork", ""},
    {"exit", "i"},
    {"getpid", ""},
    {"getppid", ""},
    {"gettid", ""},
    {"getuid", ""},
    {"getgid", ""},
    {"setuid", "u"},
    {"setgid", "u"},
    {"sched_yield", ""},
    {"sleep", "u"},
    {"pipe", "pu"},
    {"dup", "d"},
    {"dup2", "dd"},
    {"mmap", "p"},
    {"munmap", "pz"},
    {"mprotect", "pzP"},
    {"mremap", "pzzu"},
    {"madvise", "pzi"},
    {"poll", "pui"},
    {"socket", "iii"},
    {"connect", "dpu"},
    {"bind", "dpu"},
    {"listen", "di"},
    {"accept", "dpp"},
    {"sock_send", "dpz"},
    {"sock_recv", "dpz"},
    {"sendto", "dpz"},
    {"recvfrom", "dpz"},
    {"sendmsg", "dpu"},
    {"recvmsg", "dpu"},
    {"sock_shutdown", "di"},
    {"dns_resolve", "spp"},
    {"thread_exit", "i"},
    {"thread_create", "ppp"},
    {"futex", "pixxx"},
    {"set_fs_base", "p"},
    {"epoll_create", "u"},
    {"epoll_ctl", "diDp"},
    {"epoll_wait", "dpii"},
    {"eventfd", "uu"},
    {"memfd_create", "su"},
    {"shm_open", "sOu"},
    {"shm_unlink", "s"},
    {"utimes", "sp"},
    {"openpty", "ppp"},
    {"tty_ioctl", "dxx"},
    {"tty_read", "pz"},
    {"getrandom", "pzu"},
    {"time_get", "p"},
    {"env_get", "spz"},
    {"env_set", "ss"},
    {"env_unset", "s"},
    {"sync", ""},
    {"sudo_removed", ""},
};

static const struct sig_entry sigs[] = {
    {"SIGHUP", 1}, {"SIGINT", 2}, {"SIGQUIT", 3}, {"SIGILL", 4}, {"SIGTRAP", 5}, {"SIGABRT", 6},
    {"SIGBUS", 7}, {"SIGFPE", 8}, {"SIGKILL", 9}, {"SIGUSR1", 10}, {"SIGSEGV", 11}, {"SIGUSR2", 12},
    {"SIGPIPE", 13}, {"SIGALRM", 14}, {"SIGTERM", 15}, {"SIGCHLD", 17}, {"SIGCONT", 18},
    {"SIGSTOP", 19}, {"SIGTSTP", 20}, {"SIGTTIN", 21}, {"SIGTTOU", 22},
};

#define ERR(n) {#n, n}
static const struct sig_entry errs[] = {
    ERR(EPERM), ERR(ENOENT), ERR(ESRCH), ERR(EINTR), ERR(EIO), ERR(ENXIO), ERR(E2BIG), ERR(ENOEXEC),
    ERR(EBADF), ERR(ECHILD), ERR(EAGAIN), ERR(ENOMEM), ERR(EACCES), ERR(EFAULT), ERR(ENOTBLK),
    ERR(EBUSY), ERR(EEXIST), ERR(EXDEV), ERR(ENODEV), ERR(ENOTDIR), ERR(EISDIR), ERR(EINVAL),
    ERR(ENFILE), ERR(EMFILE), ERR(ENOTTY), ERR(ETXTBSY), ERR(EFBIG), ERR(ENOSPC), ERR(ESPIPE),
    ERR(EROFS), ERR(EMLINK), ERR(EPIPE), ERR(EDOM), ERR(ERANGE), ERR(EDEADLK), ERR(ENAMETOOLONG),
    ERR(ENOLCK), ERR(ENOSYS), ERR(ENOTEMPTY), ERR(ELOOP), ERR(ENOMSG), ERR(EILSEQ), ERR(ENOTSOCK),
    ERR(EDESTADDRREQ), ERR(EPROTOTYPE), ERR(ENOPROTOOPT), ERR(EPROTONOSUPPORT), ERR(EOPNOTSUPP),
    ERR(EAFNOSUPPORT), ERR(EADDRINUSE), ERR(EADDRNOTAVAIL), ERR(ENETDOWN), ERR(ENETUNREACH),
    ERR(ECONNABORTED), ERR(ECONNRESET), ERR(ENOBUFS), ERR(EISCONN), ERR(ENOTCONN), ERR(ETIMEDOUT),
    ERR(EOVERFLOW), ERR(ECONNREFUSED), ERR(EHOSTUNREACH), ERR(EALREADY), ERR(EINPROGRESS),
};

struct flag_entry {
    uint64_t bit;
    const char *name;
};

static const struct flag_entry open_flags[] = {
    {O_CREAT, "O_CREAT"}, {O_EXCL, "O_EXCL"}, {O_TRUNC, "O_TRUNC"}, {O_APPEND, "O_APPEND"},
    {O_NONBLOCK, "O_NONBLOCK"}, {O_DIRECTORY, "O_DIRECTORY"}, {O_NOFOLLOW, "O_NOFOLLOW"},
    {O_CLOEXEC, "O_CLOEXEC"},
};

static const struct flag_entry map_flags[] = {
    {MAP_SHARED, "MAP_SHARED"}, {MAP_PRIVATE, "MAP_PRIVATE"}, {MAP_FIXED, "MAP_FIXED"},
    {MAP_ANONYMOUS, "MAP_ANONYMOUS"}, {MAP_NORESERVE, "MAP_NORESERVE"}, {MAP_POPULATE, "MAP_POPULATE"},
    {MAP_STACK, "MAP_STACK"},
};

static void put(char *out, size_t cap, size_t *len, const char *text)
{
    if (!cap)
        return;
    size_t n = strlen(text);
    if (*len + n >= cap)
        n = cap - 1 - *len;
    memcpy(out + *len, text, n);
    *len += n;
    out[*len] = 0;
}

static void put_flag(char *out, size_t cap, size_t *len, const char *name)
{
    if (*len)
        put(out, cap, len, "|");
    put(out, cap, len, name);
}

static void format_table(uint64_t v, const struct flag_entry *table, size_t n, char *out, size_t cap, size_t *len)
{
    for (size_t i = 0; i < n; i++)
        if (v & table[i].bit) {
            put_flag(out, cap, len, table[i].name);
            v &= ~table[i].bit;
        }
    if (v) {
        char tmp[24];
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)v);
        put_flag(out, cap, len, tmp);
    }
}

void tl_format_open_flags(uint64_t v, char *out, size_t cap)
{
    size_t len = 0;
    if (cap)
        out[0] = 0;
    uint64_t acc = v & O_ACCMODE;
    put_flag(out, cap, &len, acc == O_WRONLY ? "O_WRONLY" : acc == O_RDWR ? "O_RDWR" : "O_RDONLY");
    format_table(v & ~(uint64_t)O_ACCMODE, open_flags, sizeof(open_flags) / sizeof(open_flags[0]), out, cap, &len);
}

void tl_format_prot(uint64_t v, char *out, size_t cap)
{
    size_t len = 0;
    if (cap)
        out[0] = 0;
    if (!v) {
        put(out, cap, &len, "PROT_NONE");
        return;
    }
    static const struct flag_entry t[] = {{PROT_READ, "PROT_READ"}, {PROT_WRITE, "PROT_WRITE"}, {PROT_EXEC, "PROT_EXEC"}};
    format_table(v, t, 3, out, cap, &len);
}

void tl_format_map_flags(uint64_t v, char *out, size_t cap)
{
    size_t len = 0;
    if (cap)
        out[0] = 0;
    if (!v) {
        put(out, cap, &len, "0");
        return;
    }
    format_table(v, map_flags, sizeof(map_flags) / sizeof(map_flags[0]), out, cap, &len);
}

void tl_format_wait_flags(uint64_t v, char *out, size_t cap)
{
    size_t len = 0;
    if (cap)
        out[0] = 0;
    if (!v) {
        put(out, cap, &len, "0");
        return;
    }
    static const struct flag_entry t[] = {{WNOHANG, "WNOHANG"}};
    format_table(v, t, 1, out, cap, &len);
}

void tl_format_signal(int sig, char *out, size_t cap)
{
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        if ((int)sigs[i].value == sig) {
            snprintf(out, cap, "%s", sigs[i].name);
            return;
        }
    snprintf(out, cap, "%d", sig);
}

const char *tl_errname(int err)
{
    for (size_t i = 0; i < sizeof(errs) / sizeof(errs[0]); i++)
        if ((int)errs[i].value == err)
            return errs[i].name;
    return NULL;
}

size_t tl_sys_count(void)
{
    return sizeof(tl_sysnames) / sizeof(tl_sysnames[0]);
}

bool tl_sys_at(size_t index, uint32_t *nr, const char **name)
{
    if (index >= tl_sys_count())
        return false;
    *nr = tl_sysnames[index].nr;
    *name = tl_sysnames[index].name;
    return true;
}

const char *tl_sysname(uint32_t nr, char *buf, size_t cap)
{
    size_t lo = 0, hi = tl_sys_count();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (tl_sysnames[mid].nr == nr)
            return tl_sysnames[mid].name;
        if (tl_sysnames[mid].nr < nr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (buf && cap)
        snprintf(buf, cap, "syscall_%u", nr);
    return buf;
}

int tl_sysnr(const char *name)
{
    if (!strncasecmp(name, "sys_", 4))
        name += 4;
    for (size_t i = 0; i < tl_sys_count(); i++)
        if (!strcasecmp(tl_sysnames[i].name, name))
            return (int)tl_sysnames[i].nr;
    char *end;
    long v = strtol(name, &end, 10);
    if (*name && !*end && v >= 0 && v < 512)
        return (int)v;
    return -1;
}

static const char *kinds_for(uint32_t nr)
{
    char tmp[24];
    const char *name = tl_sysname(nr, tmp, sizeof(tmp));
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        if (!strcmp(kinds[i].name, name))
            return kinds[i].kinds;
    return NULL;
}

int tl_nargs(uint32_t nr)
{
    const char *k = kinds_for(nr);
    return k ? (int)strlen(k) : 3;
}

char tl_argkind(uint32_t nr, int index)
{
    const char *k = kinds_for(nr);
    if (!k)
        return 'a';
    return index < (int)strlen(k) ? k[index] : 'a';
}

const char *tl_kind_label(char kind)
{
    switch (kind) {
    case 'd': return "fd";
    case 's': return "string";
    case 'p': return "pointer";
    case 'z': return "size";
    case 'O': return "open flags";
    case 'P': return "prot";
    case 'S': return "signal";
    case 'W': return "wait flags";
    case 'u': return "unsigned";
    case 'i': return "int";
    case 'x': return "hex";
    default: return "value";
    }
}

static void quote(char *out, size_t cap, const char *s)
{
    size_t len = 0;
    put(out, cap, &len, "\"");
    for (const char *p = s; *p; p++) {
        char c[3] = {*p, 0, 0};
        if (*p == '"' || *p == '\\') {
            c[0] = '\\';
            c[1] = *p;
        }
        put(out, cap, &len, c);
    }
    put(out, cap, &len, "\"");
    if (strlen(s) >= TRACE_STR_MAX - 1)
        put(out, cap, &len, "...");
}

void tl_format_arg(const struct trace_record *rec, int index, char *out, size_t cap, bool decode)
{
    uint64_t v = rec->args[index];
    char kind = tl_argkind(rec->nr, index);
    bool has_str = (index == 0 && (rec->flags & TRACE_REC_STR1)) || (index == 1 && (rec->flags & TRACE_REC_STR2));
    if (has_str && (kind == 's' || kind == 'a' || kind == 'p')) {
        quote(out, cap, rec->str);
        return;
    }
    if (!decode)
        kind = (kind == 'd' || kind == 'i' || kind == 'u' || kind == 'z') ? kind : 'x';
    switch (kind) {
    case 'd':
    case 'i':
        snprintf(out, cap, "%d", (int)(int64_t)v);
        break;
    case 'u':
    case 'z':
        snprintf(out, cap, "%llu", (unsigned long long)v);
        break;
    case 'O':
        tl_format_open_flags(v, out, cap);
        break;
    case 'P':
        tl_format_prot(v, out, cap);
        break;
    case 'S':
        tl_format_signal((int)v, out, cap);
        break;
    case 'W':
        tl_format_wait_flags(v, out, cap);
        break;
    case 'a':
        if (v < 65536)
            snprintf(out, cap, "%llu", (unsigned long long)v);
        else
            snprintf(out, cap, "0x%llx", (unsigned long long)v);
        break;
    case 's':
    case 'p':
    default:
        if (v == 0 && kind == 'p')
            snprintf(out, cap, "NULL");
        else
            snprintf(out, cap, "0x%llx", (unsigned long long)v);
        break;
    }
}

void tl_format_args(const struct trace_record *rec, char *out, size_t cap, bool decode)
{
    size_t len = 0;
    if (cap)
        out[0] = 0;
    int n = tl_nargs(rec->nr);
    for (int i = 0; i < n && i < 6; i++) {
        char tmp[160];
        tl_format_arg(rec, i, tmp, sizeof(tmp), decode);
        if (i)
            put(out, cap, &len, ", ");
        put(out, cap, &len, tmp);
    }
}

bool tl_ret_is_error(int64_t ret)
{
    return ret < 0 && ret >= -4095;
}

bool tl_format_ret(uint32_t nr, int64_t ret, char *out, size_t cap)
{
    if (tl_ret_is_error(ret)) {
        const char *name = tl_errname((int)-ret);
        if (name)
            snprintf(out, cap, "%lld %s", (long long)ret, name);
        else
            snprintf(out, cap, "%lld", (long long)ret);
        return true;
    }
    char tmp[24];
    const char *name = tl_sysname(nr, tmp, sizeof(tmp));
    if (ret > 0xffff && (!strcmp(name, "mmap") || !strcmp(name, "mremap") || !strcmp(name, "heap_grow")))
        snprintf(out, cap, "0x%llx", (unsigned long long)ret);
    else
        snprintf(out, cap, "%lld", (long long)ret);
    return false;
}

void tl_format_exit(const struct trace_record *rec, char *out, size_t cap)
{
    if (rec->args[0]) {
        char sig[24];
        tl_format_signal((int)rec->args[0], sig, sizeof(sig));
        snprintf(out, cap, "killed by %s", sig);
    } else
        snprintf(out, cap, "exited with %d", (int)rec->ret);
}

void tl_format_duration(uint64_t ns, char *out, size_t cap)
{
    if (ns < 1000)
        snprintf(out, cap, "%lluns", (unsigned long long)ns);
    else if (ns < 1000000)
        snprintf(out, cap, "%llu.%02lluus", (unsigned long long)(ns / 1000), (unsigned long long)(ns % 1000 / 10));
    else if (ns < 1000000000ULL)
        snprintf(out, cap, "%llu.%02llums", (unsigned long long)(ns / 1000000), (unsigned long long)(ns % 1000000 / 10000));
    else
        snprintf(out, cap, "%llu.%02llus", (unsigned long long)(ns / 1000000000ULL), (unsigned long long)(ns % 1000000000ULL / 10000000));
}
