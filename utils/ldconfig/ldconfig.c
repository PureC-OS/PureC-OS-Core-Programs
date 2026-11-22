#include "../lib/common.h"
#include "../lib/elfdyn.h"
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define CACHE_PATH "/etc/ld.so.cache"
#define CACHE_MAGIC "PCLDCACHE1"
#define CONF_PATH "/etc/ld.so.conf"
#define MAX_DIRS 128
#define MAX_ENTRIES 4096

struct entry {
    char name[128];
    char path[384];
};

static char *dirs[MAX_DIRS];
static int ndirs;
static struct entry *entries;
static int nentries;
static bool verbose, no_links, no_cache;

static void add_dir(const char *d)
{
    size_t n = strlen(d);
    while (n > 1 && d[n - 1] == '/')
        n--;
    if (!n || ndirs >= MAX_DIRS)
        return;
    for (int i = 0; i < ndirs; i++)
        if (strlen(dirs[i]) == n && !strncmp(dirs[i], d, n))
            return;
    dirs[ndirs++] = strndup(d, n);
}

static bool suffix_match(const char *name, const char *pattern)
{
    const char *star = strchr(pattern, '*');
    if (!star)
        return !strcmp(name, pattern);
    size_t pre = (size_t)(star - pattern), suf = strlen(star + 1), nl = strlen(name);
    return nl >= pre + suf && !strncmp(name, pattern, pre) && !strcmp(name + nl - suf, star + 1);
}

static void read_conf(const char *path, int depth)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        char *e = p + strlen(p);
        while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        char *hash = strchr(p, '#');
        if (hash)
            *hash = 0;
        if (!*p)
            continue;
        if (!strncmp(p, "include", 7) && (p[7] == ' ' || p[7] == '\t')) {
            if (depth > 3)
                continue;
            char *pat = p + 8;
            while (*pat == ' ' || *pat == '\t')
                pat++;
            char dir[512];
            const char *slash = strrchr(pat, '/');
            if (!slash)
                continue;
            snprintf(dir, sizeof(dir), "%.*s", (int)(slash - pat), pat);
            DIR *d = opendir(dir);
            if (!d)
                continue;
            struct dirent *de;
            while ((de = readdir(d))) {
                if (de->d_name[0] == '.' || !suffix_match(de->d_name, slash + 1))
                    continue;
                char full[1024];
                snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
                read_conf(full, depth + 1);
            }
            closedir(d);
            continue;
        }
        add_dir(p);
    }
    fclose(f);
}

static bool has_entry(const char *name)
{
    for (int i = 0; i < nentries; i++)
        if (!strcmp(entries[i].name, name))
            return true;
    return false;
}

static void add_entry(const char *name, const char *path)
{
    if (nentries >= MAX_ENTRIES || strlen(name) >= sizeof(entries[0].name) ||
        strlen(path) >= sizeof(entries[0].path) || has_entry(name))
        return;
    snprintf(entries[nentries].name, sizeof(entries[0].name), "%s", name);
    snprintf(entries[nentries].path, sizeof(entries[0].path), "%s", path);
    nentries++;
}

struct cand {
    char file[128];
    char soname[128];
};

static int cand_cmp(const void *a, const void *b)
{
    const struct cand *x = a, *y = b;
    int c = strcmp(x->soname, y->soname);
    if (c)
        return c;
    size_t lx = strlen(x->file), ly = strlen(y->file);
    if (lx != ly)
        return lx > ly ? -1 : 1;
    return strcmp(y->file, x->file);
}

static void fix_link(const char *dir, const char *soname, const char *file)
{
    char link[512], cur[512];
    snprintf(link, sizeof(link), "%s/%s", dir, soname);
    struct stat st;
    if (lstat(link, &st) == 0) {
        if (!S_ISLNK(st.st_mode))
            return;
        ssize_t n = readlink(link, cur, sizeof(cur) - 1);
        if (n > 0) {
            cur[n] = 0;
            if (!strcmp(cur, file))
                return;
        }
        unlink(link);
    }
    if (symlink(file, link) == 0) {
        if (verbose)
            printf("\t%s -> %s\n", soname, file);
    } else
        util_err("cannot create symbolic link %s: %s", link, strerror(errno));
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        if (verbose)
            util_err("cannot open directory %s: %s", dir, strerror(errno));
        return;
    }
    if (verbose)
        printf("%s:\n", dir);
    struct cand *c = calloc(1024, sizeof(*c));
    int nc = 0;
    struct dirent *de;
    while (c && nc < 1024 && (de = readdir(d))) {
        const char *n = de->d_name;
        if (n[0] == '.' || !strstr(n, ".so") || strlen(n) >= sizeof(c->file))
            continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, n);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        struct elfd e;
        if (elfd_load(full, &e) != 0)
            continue;
        const char *so = e.type == 3 && e.dyn ? elfd_tag_str(&e, ELFD_SONAME, 0) : NULL;
        bool is_lib = e.type == 3 && e.dyn && !e.has_interp;
        if (!is_lib && e.type == 3 && e.dyn && so)
            is_lib = true;
        if (is_lib) {
            snprintf(c[nc].file, sizeof(c[nc].file), "%s", n);
            snprintf(c[nc].soname, sizeof(c[nc].soname), "%s", so && strlen(so) < sizeof(c->soname) ? so : n);
            nc++;
        }
        elfd_free(&e);
    }
    closedir(d);
    if (!c)
        return;
    qsort(c, (size_t)nc, sizeof(*c), cand_cmp);
    for (int i = 0; i < nc; i++) {
        if (i && !strcmp(c[i].soname, c[i - 1].soname))
            continue;
        if (strcmp(c[i].soname, c[i].file) != 0 && !no_links)
            fix_link(dir, c[i].soname, c[i].file);
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", dir, c[i].soname);
        struct stat st;
        if (stat(p, &st) != 0)
            snprintf(p, sizeof(p), "%s/%s", dir, c[i].file);
        add_entry(c[i].soname, p);
        if (strcmp(c[i].soname, c[i].file) != 0 && verbose)
            printf("\t%s (%s) => %s\n", c[i].soname, c[i].file, p);
    }
    free(c);
}

static int write_cache(void)
{
    char tmp[] = CACHE_PATH ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f)
        return util_err("cannot write %s: %s", tmp, strerror(errno));
    fprintf(f, "%s\n", CACHE_MAGIC);
    for (int i = 0; i < nentries; i++)
        fprintf(f, "%s\t%s\n", entries[i].name, entries[i].path);
    if (fflush(f) != 0 || ferror(f)) {
        fclose(f);
        unlink(tmp);
        return util_err("write error on %s", tmp);
    }
    fclose(f);
    if (rename(tmp, CACHE_PATH) != 0) {
        unlink(tmp);
        return util_err("cannot rename to %s: %s", CACHE_PATH, strerror(errno));
    }
    return 0;
}

static int print_cache(void)
{
    FILE *f = fopen(CACHE_PATH, "r");
    if (!f)
        return util_err("cannot open cache file %s", CACHE_PATH);
    char line[768];
    if (!fgets(line, sizeof(line), f) || strncmp(line, CACHE_MAGIC, strlen(CACHE_MAGIC)) != 0) {
        fclose(f);
        return util_err("%s: bad cache format", CACHE_PATH);
    }
    int n = 0;
    char **rows = calloc(MAX_ENTRIES, sizeof(char *));
    while (rows && n < MAX_ENTRIES && fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        char row[800];
        snprintf(row, sizeof(row), "\t%s => %s", line, tab + 1);
        rows[n++] = strdup(row);
    }
    fclose(f);
    printf("%d libs found in cache `%s'\n", n, CACHE_PATH);
    for (int i = 0; i < n; i++) {
        puts(rows[i]);
        free(rows[i]);
    }
    free(rows);
    return 0;
}

int main(int argc, char **argv)
{
    util_init(argc, argv);
    bool only_args = false, print = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "--help")) { util_usage("[-nNXvp] [DIR]..."); return 0; }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'n') only_args = true;
            else if (*o == 'N') no_cache = true;
            else if (*o == 'X') no_links = true;
            else if (*o == 'v') verbose = true;
            else if (*o == 'p') print = true;
            else return util_err("invalid option '-%c'", *o);
        }
    }
    if (print)
        return print_cache();
    if (only_args)
        no_cache = true;
    if (!no_cache || !no_links) {
        if (geteuid() != 0)
            return util_err("must be run as root");
    }
    entries = calloc(MAX_ENTRIES, sizeof(*entries));
    if (!entries)
        return util_err("out of memory");
    for (int k = i; k < argc; k++)
        add_dir(argv[k]);
    if (!only_args) {
        read_conf(CONF_PATH, 0);
        add_dir("/lib");
        add_dir("/usr/lib");
    }
    for (int k = 0; k < ndirs; k++)
        scan_dir(dirs[k]);
    if (no_cache)
        return 0;
    return write_cache() ? 1 : 0;
}
