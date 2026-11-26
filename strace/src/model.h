#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "libc/include/purec.h"
#include "tracelib/sysnames.h"

#define MAX_ITEMS 50000
#define MAX_PROCS 256
#define NSYS 512

enum item_kind {
    IT_SYS = 1,
    IT_CHILD,
    IT_EXIT,
};

struct item {
    uint8_t kind;
    bool done;
    uint16_t proc;
    uint16_t child;
    uint32_t next;
    struct trace_record rec;
};

struct pstat {
    uint32_t calls[NSYS];
    uint32_t errors[NSYS];
    uint64_t ns[NSYS];
};

struct proc {
    bool used;
    bool alive;
    bool stopped;
    bool expanded;
    bool shown;
    int parent;
    int depth;
    uint32_t pid;
    uint32_t ppid;
    uint32_t first;
    uint32_t last;
    uint32_t stopped_seq;
    uint64_t calls;
    uint64_t errors;
    bool exited;
    int exit_code;
    int exit_signal;
    char name[24];
    char exit_text[48];
    struct pstat *st;
};

struct model {
    int session;
    uint32_t root_pid;
    int child_pid;
    uint64_t events;
    uint64_t dropped;
    bool stop_changed;
    bool stepping;
    uint64_t breaks[TRACE_MASK_WORDS];
    int nprocs;
    uint32_t next_seq;
    uint32_t first_seq;
    char *command;
};

extern struct model M;
extern struct proc P[MAX_PROCS];

void model_init(void);
void model_clear(void);
const char *model_start_command(const char *line);
const char *model_attach(uint32_t pid);
void model_detach(void);
bool model_poll(void);
void model_continue(void);
void model_step(void);
void model_apply_breaks(void);
bool model_any_stopped(void);
bool item_valid(uint32_t seq);
struct item *item_at(uint32_t seq);
bool model_active(void);
int model_roots(int *out, int max);
const char *model_export_json(const char *path);
