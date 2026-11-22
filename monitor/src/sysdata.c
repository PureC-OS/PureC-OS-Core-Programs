#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pwd.h>
#include <unistd.h>
#include "sysdata.h"
#include "util.h"
#include "libc/include/purec.h"
#include "pg/app.h"

struct sysdata g_sys;

struct prev_cpu {
    uint32_t pid;
    uint64_t ms;
};

static struct prev_cpu prev_procs[MAX_PROCS];
static int prev_count;

struct uid_name {
    uint32_t uid;
    char name[32];
};

static struct uid_name uid_cache[32];
static int uid_cache_count;

void hist_init(struct history *h, int cap)
{
    free(h->v);
    h->v = calloc((size_t)cap, sizeof(uint64_t));
    h->cap = h->v ? cap : 0;
    h->count = 0;
    h->head = 0;
}

void hist_resize(struct history *h, int cap)
{
    uint64_t *n = calloc((size_t)cap, sizeof(uint64_t));
    if (!n)
        return;
    int keep = h->count < cap ? h->count : cap;
    for (int i = 0; i < keep; i++)
        n[i] = hist_at(h, h->count - keep + i);
    free(h->v);
    h->v = n;
    h->cap = cap;
    h->count = keep;
    h->head = keep % cap;
}

void hist_push(struct history *h, uint64_t value)
{
    if (!h->cap)
        return;
    h->v[h->head] = value;
    h->head = (h->head + 1) % h->cap;
    if (h->count < h->cap)
        h->count++;
}

uint64_t hist_at(const struct history *h, int index)
{
    if (index < 0 || index >= h->count)
        return 0;
    int start = (h->head - h->count + h->cap * 2) % h->cap;
    return h->v[(start + index) % h->cap];
}

uint64_t hist_max(const struct history *h)
{
    uint64_t m = 0;
    for (int i = 0; i < h->count; i++) {
        uint64_t v = hist_at(h, i);
        if (v > m)
            m = v;
    }
    return m;
}

static void all_hist(int cap, bool resize)
{
    struct history *list[] = {&g_sys.cpu_hist, &g_sys.mem_hist, &g_sys.swap_hist, &g_sys.rx_hist, &g_sys.tx_hist};
    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        if (resize)
            hist_resize(list[i], cap);
        else
            hist_init(list[i], cap);
    }
    for (int i = 0; i < MAX_CORES; i++) {
        if (resize)
            hist_resize(&g_sys.core_hist[i], cap);
        else
            hist_init(&g_sys.core_hist[i], cap);
    }
    for (int i = 0; i < MAX_IFACES; i++) {
        if (resize) {
            hist_resize(&g_sys.ifaces[i].rx, cap);
            hist_resize(&g_sys.ifaces[i].tx, cap);
        } else {
            hist_init(&g_sys.ifaces[i].rx, cap);
            hist_init(&g_sys.ifaces[i].tx, cap);
        }
    }
}

void sys_set_history(int history)
{
    all_hist(history, true);
}

static void read_static(void)
{
    static char text[2048];
    if (u_read_file("/proc/version", text, sizeof(text)))
        u_copy(g_sys.kernel, sizeof(g_sys.kernel), u_trim(text));
    if (u_read_file("/proc/cpuinfo", text, sizeof(text))) {
        char *m = strstr(text, "model name");
        if (m) {
            m = strchr(m, ':');
            if (m) {
                char line[64];
                u_copy(line, sizeof(line), m + 1);
                char *nl = strchr(line, '\n');
                if (nl)
                    *nl = 0;
                u_copy(g_sys.cpu_model, sizeof(g_sys.cpu_model), u_trim(line));
            }
        }
        char *f = strstr(text, "cpu MHz");
        if (f && (f = strchr(f, ':')))
            g_sys.cpu_mhz = (uint32_t)atoi(f + 1);
    }
    g_sys.my_uid = (uint32_t)getuid();
    g_sys.is_root = g_sys.my_uid == 0;
}

void sys_init(int history)
{
    memset(&g_sys, 0, sizeof(g_sys));
    all_hist(history, false);
    read_static();
}

static void update_cpu(void)
{
    static char text[1024];
    if (!u_read_file("/proc/stat", text, sizeof(text)))
        return;
    int cores = 0, online = 0;
    uint32_t sum_busy = 0, sum_cnt = 0;
    uint64_t sum_total = 0, sum_idle = 0;
    char *save = text;
    while (save && *save) {
        char *nl = strchr(save, '\n');
        char *line = save;
        if (nl) {
            *nl = 0;
            save = nl + 1;
        } else
            save = NULL;
        if (!strncmp(line, "tasks ", 6)) {
            unsigned t = 0, r = 0;
            sscanf(line + 6, "%u %u", &t, &r);
            g_sys.tasks = t;
            g_sys.running = r;
            continue;
        }
        if (strncmp(line, "cpu", 3) || line[3] < '0' || line[3] > '9')
            continue;
        unsigned id = 0, on = 1;
        unsigned long long total = 0, idle = 0;
        if (sscanf(line + 3, "%u %llu %llu %u", &id, &total, &idle, &on) < 3 || id >= MAX_CORES)
            continue;
        if ((int)id + 1 > cores)
            cores = (int)id + 1;
        g_sys.core_online[id] = on != 0;
        if (on)
            online++;
        if (g_sys.have_prev && on) {
            uint64_t dt = total - g_sys.prev_total[id], di = idle - g_sys.prev_idle[id];
            uint32_t pm = dt ? (uint32_t)((dt > di ? dt - di : 0) * 1000 / dt) : 0;
            g_sys.core_pm[id] = pm > 1000 ? 1000 : pm;
            sum_busy += g_sys.core_pm[id];
            sum_cnt++;
            sum_total += dt;
            sum_idle += di;
        } else if (!on)
            g_sys.core_pm[id] = 0;
        g_sys.prev_total[id] = total;
        g_sys.prev_idle[id] = idle;
    }
    (void)sum_total;
    (void)sum_idle;
    g_sys.ncores = cores;
    g_sys.online_cores = online;
    if (g_sys.have_prev && sum_cnt)
        g_sys.cpu_pm = sum_busy / sum_cnt;
    g_sys.have_prev = cores > 0;
    hist_push(&g_sys.cpu_hist, g_sys.cpu_pm);
    for (int i = 0; i < cores; i++)
        hist_push(&g_sys.core_hist[i], g_sys.core_pm[i]);
}

static void update_mem(void)
{
    static char text[1024];
    if (!u_read_file("/proc/meminfo", text, sizeof(text)))
        return;
    char *save = text;
    while (save && *save) {
        char *nl = strchr(save, '\n');
        char *line = save;
        if (nl) {
            *nl = 0;
            save = nl + 1;
        } else
            save = NULL;
        char key[32];
        unsigned long long v;
        if (sscanf(line, "%31[^:]: %llu", key, &v) != 2)
            continue;
        if (!strcmp(key, "MemTotal"))
            g_sys.mem_total_kb = v;
        else if (!strcmp(key, "MemFree"))
            g_sys.mem_free_kb = v;
        else if (!strcmp(key, "SwapTotal"))
            g_sys.swap_total_kb = v;
        else if (!strcmp(key, "SwapFree"))
            g_sys.swap_free_kb = v;
        else if (!strcmp(key, "SwapIn"))
            g_sys.swap_in = v;
        else if (!strcmp(key, "SwapOut"))
            g_sys.swap_out = v;
    }
    g_sys.mem_used_kb = g_sys.mem_total_kb > g_sys.mem_free_kb ? g_sys.mem_total_kb - g_sys.mem_free_kb : 0;
    hist_push(&g_sys.mem_hist, g_sys.mem_total_kb ? g_sys.mem_used_kb * 1000 / g_sys.mem_total_kb : 0);
    uint64_t su = g_sys.swap_total_kb > g_sys.swap_free_kb ? g_sys.swap_total_kb - g_sys.swap_free_kb : 0;
    hist_push(&g_sys.swap_hist, g_sys.swap_total_kb ? su * 1000 / g_sys.swap_total_kb : 0);
    if (u_read_file("/proc/uptime", text, sizeof(text)))
        g_sys.uptime_s = (uint64_t)atoll(text);
}

static const char *user_name(uint32_t uid)
{
    for (int i = 0; i < uid_cache_count; i++)
        if (uid_cache[i].uid == uid)
            return uid_cache[i].name;
    struct uid_name *e = &uid_cache[uid_cache_count < 32 ? uid_cache_count++ : 31];
    e->uid = uid;
    struct passwd *pw = getpwuid((uid_t)uid);
    if (pw && pw->pw_name)
        u_copy(e->name, sizeof(e->name), pw->pw_name);
    else
        snprintf(e->name, sizeof(e->name), "%u", uid);
    return e->name;
}

static uint64_t prev_ms_for(uint32_t pid, bool *found)
{
    for (int i = 0; i < prev_count; i++)
        if (prev_procs[i].pid == pid) {
            *found = true;
            return prev_procs[i].ms;
        }
    *found = false;
    return 0;
}

static void update_procs(uint32_t dt_ms)
{
    DIR *d = opendir("/proc");
    if (!d)
        return;
    static struct prev_cpu next[MAX_PROCS];
    int count = 0;
    struct dirent *e;
    static char text[1024];
    int cores = g_sys.online_cores > 0 ? g_sys.online_cores : 1;
    while ((e = readdir(d)) && count < MAX_PROCS) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char path[48];
        snprintf(path, sizeof(path), "/proc/%s/status", e->d_name);
        if (!u_read_file(path, text, sizeof(text)))
            continue;
        struct proc_info *p = &g_sys.procs[count];
        memset(p, 0, sizeof(*p));
        p->threads = 1;
        char *save = text;
        while (save && *save) {
            char *nl = strchr(save, '\n');
            char *line = save;
            if (nl) {
                *nl = 0;
                save = nl + 1;
            } else
                save = NULL;
            char *colon = strchr(line, ':');
            if (!colon)
                continue;
            *colon = 0;
            char *val = u_trim(colon + 1);
            if (!strcmp(line, "Name"))
                u_copy(p->name, sizeof(p->name), val);
            else if (!strcmp(line, "State"))
                u_copy(p->state, sizeof(p->state), val);
            else if (!strcmp(line, "Pid"))
                p->pid = (uint32_t)atoi(val);
            else if (!strcmp(line, "PPid"))
                p->ppid = (uint32_t)atoi(val);
            else if (!strcmp(line, "Uid"))
                p->uid = (uint32_t)atoi(val);
            else if (!strcmp(line, "Threads"))
                p->threads = (uint32_t)atoi(val);
            else if (!strcmp(line, "VmRSS"))
                p->rss_kb = (uint64_t)atoll(val);
            else if (!strcmp(line, "CpuTime"))
                p->cpu_ms = (uint64_t)atoll(val);
            else if (!strcmp(line, "Kernel"))
                p->kernel = atoi(val) != 0;
        }
        if (!p->pid || !strcmp(p->state, "exited"))
            continue;
        u_copy(p->user, sizeof(p->user), p->kernel ? "kernel" : user_name(p->uid));
        p->locked = p->kernel || p->uid == 0 || p->pid <= 1;
        bool found;
        uint64_t before = prev_ms_for(p->pid, &found);
        if (found && dt_ms && p->cpu_ms >= before) {
            uint64_t pm = (p->cpu_ms - before) * 1000 / dt_ms / (uint64_t)cores;
            p->cpu_pm = pm > 1000 ? 1000 : (uint32_t)pm;
        }
        next[count].pid = p->pid;
        next[count].ms = p->cpu_ms;
        count++;
    }
    closedir(d);
    memcpy(prev_procs, next, sizeof(struct prev_cpu) * (size_t)count);
    prev_count = count;
    g_sys.nprocs = count;
}

static struct iface_info *iface_slot(const char *name)
{
    for (int i = 0; i < g_sys.nifaces; i++)
        if (!strcmp(g_sys.ifaces[i].name, name))
            return &g_sys.ifaces[i];
    if (g_sys.nifaces >= MAX_IFACES)
        return NULL;
    struct iface_info *s = &g_sys.ifaces[g_sys.nifaces++];
    u_copy(s->name, sizeof(s->name), name);
    s->rx_bytes = s->tx_bytes = 0;
    return s;
}

static void update_net(uint32_t dt_ms)
{
    static struct net_if_info list[MAX_IFACES];
    int n = pc_net_if_list(list, MAX_IFACES);
    for (int i = 0; i < g_sys.nifaces; i++)
        g_sys.ifaces[i].present = false;
    uint64_t rx = 0, tx = 0, rxr = 0, txr = 0;
    for (int i = 0; i < n; i++) {
        struct iface_info *s = iface_slot(list[i].name);
        if (!s)
            continue;
        bool first = !s->present && s->rx_bytes == 0 && s->tx_bytes == 0;
        uint64_t r = list[i].rx_bytes, t = list[i].tx_bytes;
        if (!first && dt_ms && r >= s->rx_bytes && t >= s->tx_bytes) {
            s->rx_rate = (r - s->rx_bytes) * 1000 / dt_ms;
            s->tx_rate = (t - s->tx_bytes) * 1000 / dt_ms;
        } else {
            s->rx_rate = s->tx_rate = 0;
        }
        s->rx_bytes = r;
        s->tx_bytes = t;
        s->present = true;
        s->link = list[i].link_up != 0;
        s->has_ip = list[i].has_ip != 0;
        s->ip = list[i].ip_address;
        memcpy(s->mac, list[i].mac, 6);
        hist_push(&s->rx, s->rx_rate);
        hist_push(&s->tx, s->tx_rate);
        rx += r;
        tx += t;
        rxr += s->rx_rate;
        txr += s->tx_rate;
    }
    g_sys.rx_total = rx;
    g_sys.tx_total = tx;
    g_sys.rx_rate = rxr;
    g_sys.tx_rate = txr;
    hist_push(&g_sys.rx_hist, rxr);
    hist_push(&g_sys.tx_hist, txr);
}

static void update_slow(void)
{
    static char text[2048];
    g_sys.nmounts = 0;
    if (u_read_file("/proc/mounts", text, sizeof(text))) {
        char *save = text;
        while (save && *save && g_sys.nmounts < MAX_MOUNTS) {
            char *nl = strchr(save, '\n');
            char *line = save;
            if (nl) {
                *nl = 0;
                save = nl + 1;
            } else
                save = NULL;
            char src[48], path[48], type[16], mode[8];
            if (sscanf(line, "%47s %47s %15s %7s", src, path, type, mode) < 4)
                continue;
            struct mount_info *m = &g_sys.mounts[g_sys.nmounts++];
            memset(m, 0, sizeof(*m));
            u_copy(m->source, sizeof(m->source), src);
            u_copy(m->path, sizeof(m->path), path);
            u_copy(m->type, sizeof(m->type), type);
            m->ro = !strcmp(mode, "ro");
            if (!strcmp(path, "/")) {
                struct ext2_super_info sb;
                if (pc_ext2_super(&sb) == 0 && sb.block_size) {
                    m->has_usage = true;
                    m->total = (uint64_t)sb.total_blocks * sb.block_size;
                    m->free = (uint64_t)sb.free_blocks * sb.block_size;
                }
            }
        }
    }
    g_sys.nparts = 0;
    if (u_read_file("/proc/partitions", text, sizeof(text))) {
        char *save = strchr(text, '\n');
        if (save)
            save++;
        while (save && *save && g_sys.nparts < MAX_PARTS) {
            char *nl = strchr(save, '\n');
            char *line = save;
            if (nl) {
                *nl = 0;
                save = nl + 1;
            } else
                save = NULL;
            char name[16], model[48] = "";
            unsigned long long sectors;
            unsigned ss;
            if (sscanf(line, "%15s %llu %u %47[^\n]", name, &sectors, &ss, model) < 3)
                continue;
            struct part_info *p = &g_sys.parts[g_sys.nparts++];
            u_copy(p->name, sizeof(p->name), name);
            u_copy(p->model, sizeof(p->model), u_trim(model));
            p->bytes = sectors * ss;
        }
    }
    struct battery_info bi;
    memset(&bi, 0, sizeof(bi));
    if (pc_syscall(SYS_BATTERY_INFO, (uint64_t)(uintptr_t)&bi, 0, 0) >= 0 && bi.present) {
        g_sys.battery_present = true;
        g_sys.battery_pct = bi.percent > 100 ? 100 : bi.percent;
        g_sys.battery_charging = bi.charging != 0;
    } else
        g_sys.battery_present = false;
    struct thermal_info ti;
    memset(&ti, 0, sizeof(ti));
    if (pc_syscall(SYS_THERMAL_INFO, (uint64_t)(uintptr_t)&ti, 0, 0) >= 0 && ti.available) {
        g_sys.thermal_ok = true;
        g_sys.thermal_deci_c = ti.temperature_deci_c;
        g_sys.fan_rpm = ti.fan_rpm;
    } else
        g_sys.thermal_ok = false;
}

void sys_update(void)
{
    uint64_t now = pg_time_ms();
    uint32_t dt = g_sys.last_ms ? (uint32_t)(now - g_sys.last_ms) : 0;
    g_sys.last_dt_ms = dt;
    g_sys.last_ms = now;
    update_cpu();
    update_mem();
    update_procs(dt);
    update_net(dt);
    if (!g_sys.slow_ms || now - g_sys.slow_ms >= 3000) {
        g_sys.slow_ms = now;
        update_slow();
    }
}

const struct proc_info *sys_find_proc(uint32_t pid)
{
    for (int i = 0; i < g_sys.nprocs; i++)
        if (g_sys.procs[i].pid == pid)
            return &g_sys.procs[i];
    return NULL;
}
