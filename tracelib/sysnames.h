#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../libc/include/purec/abi/trace.h"

struct tl_sysname {
    uint32_t nr;
    const char *name;
};

const char *tl_sysname(uint32_t nr, char *buf, size_t cap);
int tl_sysnr(const char *name);
size_t tl_sys_count(void);
bool tl_sys_at(size_t index, uint32_t *nr, const char **name);

const char *tl_errname(int err);
int tl_nargs(uint32_t nr);
char tl_argkind(uint32_t nr, int index);
const char *tl_kind_label(char kind);

void tl_format_arg(const struct trace_record *rec, int index, char *out, size_t cap, bool decode);
void tl_format_args(const struct trace_record *rec, char *out, size_t cap, bool decode);
bool tl_format_ret(uint32_t nr, int64_t ret, char *out, size_t cap);
bool tl_ret_is_error(int64_t ret);
void tl_format_exit(const struct trace_record *rec, char *out, size_t cap);
void tl_format_duration(uint64_t ns, char *out, size_t cap);
void tl_format_signal(int sig, char *out, size_t cap);
void tl_format_open_flags(uint64_t v, char *out, size_t cap);
void tl_format_prot(uint64_t v, char *out, size_t cap);
void tl_format_map_flags(uint64_t v, char *out, size_t cap);
void tl_format_wait_flags(uint64_t v, char *out, size_t cap);
