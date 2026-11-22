#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MAX_CORES 32
#define MAX_PROCS 256
#define MAX_IFACES 8
#define MAX_MOUNTS 16
#define MAX_PARTS 16

struct history {
    uint64_t *v;
    int cap, count, head;
};

void hist_init(struct history *h, int cap);
void hist_resize(struct history *h, int cap);
void hist_push(struct history *h, uint64_t value);
uint64_t hist_at(const struct history *h, int index);
uint64_t hist_max(const struct history *h);

struct proc_info {
    uint32_t pid, ppid, uid, threads;
    uint64_t rss_kb, cpu_ms;
    uint32_t cpu_pm;
    char state[12];
    char name[40];
    char user[32];
    bool locked;
};

struct iface_info {
    char name[16];
    bool present, link, has_ip;
    uint32_t ip;
    uint8_t mac[6];
    uint64_t rx_bytes, tx_bytes, rx_rate, tx_rate;
    struct history rx, tx;
};

struct mount_info {
    char source[48], path[48], type[16];
    bool ro, has_usage;
    uint64_t total, free;
};

struct part_info {
    char name[16], model[48];
    uint64_t bytes;
};

struct sysdata {
    bool is_root;
    uint32_t my_uid;
    int ncores, online_cores;
    uint32_t core_pm[MAX_CORES];
    bool core_online[MAX_CORES];
    uint32_t cpu_pm;
    struct history cpu_hist, core_hist[MAX_CORES];
    uint64_t prev_total[MAX_CORES], prev_idle[MAX_CORES];
    bool have_prev;

    uint64_t mem_total_kb, mem_free_kb, mem_used_kb;
    uint64_t swap_total_kb, swap_free_kb, swap_in, swap_out;
    struct history mem_hist, swap_hist;

    char cpu_model[64], kernel[64];
    uint32_t cpu_mhz;
    uint64_t uptime_s;
    uint32_t tasks, running;

    struct proc_info procs[MAX_PROCS];
    int nprocs;

    struct iface_info ifaces[MAX_IFACES];
    int nifaces;
    uint64_t rx_rate, tx_rate, rx_total, tx_total;
    struct history rx_hist, tx_hist;

    struct mount_info mounts[MAX_MOUNTS];
    int nmounts;
    struct part_info parts[MAX_PARTS];
    int nparts;
    uint64_t slow_ms;

    bool battery_present, battery_charging;
    uint32_t battery_pct;
    bool thermal_ok;
    int thermal_deci_c;
    uint32_t fan_rpm;

    uint64_t last_ms;
    uint32_t last_dt_ms;
};

extern struct sysdata g_sys;

void sys_init(int history);
void sys_set_history(int history);
void sys_update(void);
const struct proc_info *sys_find_proc(uint32_t pid);
