#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "model.h"

#ifndef STRACE_VERSION
#define STRACE_VERSION "unknown"
#endif

static int utf8_len(const unsigned char *p)
{
    if (p[0] < 0x80)
        return 1;
    int n = p[0] >= 0xf0 && p[0] < 0xf8 ? 4 : p[0] >= 0xe0 ? (p[0] < 0xf0 ? 3 : 0) : p[0] >= 0xc2 ? 2 : 0;
    if (!n || n > 4)
        return 0;
    for (int i = 1; i < n; i++)
        if ((p[i] & 0xc0) != 0x80)
            return 0;
    return n;
}

static void js(FILE *f, const char *s)
{
    fputc('"', f);
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    while (*p) {
        unsigned char c = *p;
        switch (c) {
        case '"': fputs("\\\"", f); p++; continue;
        case '\\': fputs("\\\\", f); p++; continue;
        case '\n': fputs("\\n", f); p++; continue;
        case '\r': fputs("\\r", f); p++; continue;
        case '\t': fputs("\\t", f); p++; continue;
        case '\b': fputs("\\b", f); p++; continue;
        case '\f': fputs("\\f", f); p++; continue;
        default: break;
        }
        if (c < 0x20 || c == 0x7f) {
            fprintf(f, "\\u%04x", c);
            p++;
            continue;
        }
        int n = utf8_len(p);
        if (!n) {
            fprintf(f, "\\u%04x", c);
            p++;
            continue;
        }
        fwrite(p, 1, (size_t)n, f);
        p += n;
    }
    fputc('"', f);
}

static void stat_entries(FILE *f, const uint32_t *calls, const uint32_t *errors, const uint64_t *ns, const char *indent)
{
    bool first = true;
    fputs("[", f);
    for (int i = 0; i < NSYS; i++) {
        if (!calls[i])
            continue;
        char tmp[24];
        fprintf(f, "%s\n%s{\"syscall_nr\": %d, \"syscall\": ", first ? "" : ",", indent, i);
        js(f, tl_sysname((uint32_t)i, tmp, sizeof(tmp)));
        fprintf(f, ", \"calls\": %u, \"errors\": %u, \"total_ns\": %llu}", calls[i], errors[i], (unsigned long long)ns[i]);
        first = false;
    }
    fputs(first ? "]" : "\n      ]", f);
}

static void write_args(FILE *f, const struct item *it, char *buf, size_t cap)
{
    fputs("\"args_raw\": [", f);
    for (int i = 0; i < 6; i++)
        fprintf(f, "%s\"0x%llx\"", i ? ", " : "", (unsigned long long)it->rec.args[i]);
    fputs("], \"args_decoded\": [", f);
    int n = tl_nargs(it->rec.nr);
    if (n > 6)
        n = 6;
    for (int i = 0; i < n; i++) {
        tl_format_arg(&it->rec, i, buf, cap, true);
        if (i)
            fputs(", ", f);
        js(f, buf);
    }
    fputs("]", f);
}

const char *model_export_json(const char *path)
{
    static char err[160];
    const size_t cap = 4096;
    char *buf = malloc(cap);
    uint64_t *tns = calloc(NSYS, sizeof(uint64_t));
    uint32_t *tcalls = calloc(NSYS, sizeof(uint32_t));
    uint32_t *terr = calloc(NSYS, sizeof(uint32_t));
    FILE *f = fopen(path, "w");
    if (!buf || !tns || !tcalls || !terr || !f) {
        snprintf(err, sizeof(err), "cannot write %s: %s", path, f ? "out of memory" : strerror(errno));
        if (f)
            fclose(f);
        free(buf);
        free(tns);
        free(tcalls);
        free(terr);
        return err;
    }
    char stamp[40];
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02dZ", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    fputs("{\n  \"tool\": \"strace\",\n  \"version\": ", f);
    js(f, STRACE_VERSION);
    fputs(",\n  \"exported_at\": ", f);
    js(f, stamp);
    fputs(",\n  \"traced\": {\"command\": ", f);
    if (M.command)
        js(f, M.command);
    else
        fputs("null", f);
    fprintf(f, ", \"pid\": %u},\n  \"processes\": [", M.root_pid);
    bool first = true;
    uint64_t total_calls = 0, total_errors = 0;
    for (int i = 0; i < M.nprocs; i++) {
        struct proc *p = &P[i];
        if (!p->used)
            continue;
        fprintf(f, "%s\n    {\"pid\": %u, \"parent\": ", first ? "" : ",", p->pid);
        if (p->parent >= 0 && P[p->parent].used)
            fprintf(f, "%u", P[p->parent].pid);
        else
            fprintf(f, "%u", p->ppid);
        fputs(", \"name\": ", f);
        js(f, p->name);
        fprintf(f, ", \"alive\": %s, \"exit_status\": ", p->alive ? "true" : "false");
        if (p->exited)
            fprintf(f, "{\"code\": %d, \"signal\": %d, \"text\": ", p->exit_signal ? -1 : p->exit_code, p->exit_signal);
        else if (!p->alive && p->exit_text[0])
            fputs("{\"code\": null, \"signal\": 0, \"text\": ", f);
        else
            fputs("null", f);
        if (p->exited || (!p->alive && p->exit_text[0])) {
            js(f, p->exit_text);
            fputs("}", f);
        }
        fprintf(f, ", \"calls\": %llu, \"errors\": %llu}", (unsigned long long)p->calls, (unsigned long long)p->errors);
        total_calls += p->calls;
        total_errors += p->errors;
        first = false;
    }
    fputs(first ? "],\n  \"events\": [" : "\n  ],\n  \"events\": [", f);
    first = true;
    uint64_t written = 0;
    for (uint32_t seq = M.first_seq; seq < M.next_seq; seq++) {
        struct item *it = item_at(seq);
        if (!it || it->kind != IT_SYS)
            continue;
        char tmp[24];
        fprintf(f, "%s\n    {\"index\": %u, \"pid\": %u, \"tid\": %u, \"syscall_nr\": %u, \"syscall\": ", first ? "" : ",", seq,
                it->rec.pid, it->rec.tid, it->rec.nr);
        js(f, tl_sysname(it->rec.nr, tmp, sizeof(tmp)));
        fputs(", ", f);
        write_args(f, it, buf, cap);
        fputs(", \"string\": ", f);
        if (it->rec.flags & (TRACE_REC_STR1 | TRACE_REC_STR2))
            js(f, it->rec.str);
        else
            fputs("null", f);
        if (it->done) {
            bool bad = tl_ret_is_error(it->rec.ret);
            const char *en = bad ? tl_errname((int)-it->rec.ret) : NULL;
            tl_format_ret(it->rec.nr, it->rec.ret, buf, cap);
            fprintf(f, ", \"result\": {\"raw\": %lld, \"text\": ", (long long)it->rec.ret);
            js(f, buf);
            fputs(", \"error\": ", f);
            fputs(bad ? "true" : "false", f);
            fputs(", \"errno\": ", f);
            if (en)
                js(f, en);
            else
                fputs("null", f);
            fputs("}", f);
        } else
            fputs(", \"result\": null", f);
        fprintf(f, ", \"entry_time_ns\": %llu, \"duration_ns\": ", (unsigned long long)it->rec.time_ns);
        if (it->done)
            fprintf(f, "%llu", (unsigned long long)it->rec.duration_ns);
        else
            fputs("null", f);
        fputs("}", f);
        first = false;
        written++;
    }
    fprintf(f, "%s],\n  \"statistics\": {\n    \"events\": %llu,\n    \"events_exported\": %llu,\n", first ? "" : "\n  ",
            (unsigned long long)M.events, (unsigned long long)written);
    fprintf(f, "    \"completed_calls\": %llu,\n    \"failed_calls\": %llu,\n    \"dropped_by_kernel\": %llu,\n",
            (unsigned long long)total_calls, (unsigned long long)total_errors, (unsigned long long)M.dropped);
    fprintf(f, "    \"rows_discarded_by_viewer\": %u,\n    \"per_process\": [", M.first_seq - 1);
    first = true;
    for (int i = 0; i < M.nprocs; i++) {
        struct proc *p = &P[i];
        if (!p->used || !p->st)
            continue;
        fprintf(f, "%s\n      {\"pid\": %u, \"syscalls\": ", first ? "" : ",", p->pid);
        stat_entries(f, p->st->calls, p->st->errors, p->st->ns, "        ");
        fputs("}", f);
        for (int k = 0; k < NSYS; k++) {
            tcalls[k] += p->st->calls[k];
            terr[k] += p->st->errors[k];
            tns[k] += p->st->ns[k];
        }
        first = false;
    }
    fputs(first ? "],\n    \"per_syscall\": " : "\n    ],\n    \"per_syscall\": ", f);
    stat_entries(f, tcalls, terr, tns, "      ");
    fputs("\n  }\n}\n", f);
    bool bad = ferror(f) != 0;
    if (fclose(f) != 0)
        bad = true;
    if (bad)
        snprintf(err, sizeof(err), "write to %s failed: %s", path, strerror(errno));
    free(buf);
    free(tns);
    free(tcalls);
    free(terr);
    return bad ? err : NULL;
}
