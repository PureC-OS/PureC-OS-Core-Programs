#include "../lib/common.h"
#include "../lib/elfdyn.h"
#include <unistd.h>
#include <sys/wait.h>

#define CACHE_PATH "/etc/ld.so.cache"
#define CACHE_MAGIC "PCLDCACHE1"
#define MAX_SEEN 256

static char *seen[MAX_SEEN];
static int nseen;
static int missing;
static const char *lib_path_env;

static bool cache_find(const char *name, char *out, size_t cap)
{
    FILE *f = fopen(CACHE_PATH, "r");
    if (!f)
        return false;
    char line[768];
    bool ok = false;
    if (!fgets(line, sizeof(line), f) || strncmp(line, CACHE_MAGIC, strlen(CACHE_MAGIC)) != 0) {
        fclose(f);
        return false;
    }
    size_t nl = strlen(name);
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (l > nl + 2 && !strncmp(line, name, nl) && line[nl] == '\t' && line[nl + 1] == '/' &&
            access(line + nl + 1, 0) == 0) {
            snprintf(out, cap, "%s", line + nl + 1);
            ok = true;
            break;
        }
    }
    fclose(f);
    return ok;
}

static bool search_list(const char *list, const char *name, char *out, size_t cap)
{
    while (list && *list) {
        const char *end = strchr(list, ':');
        size_t n = end ? (size_t)(end - list) : strlen(list);
        if (n) {
            snprintf(out, cap, "%.*s/%s", (int)n, list, name);
            if (access(out, 0) == 0)
                return true;
        }
        list = end ? end + 1 : NULL;
    }
    return false;
}

static bool resolve(const struct elfd *req, const struct elfd *root, const char *name, char *out, size_t cap)
{
    if (strchr(name, '/')) {
        snprintf(out, cap, "%s", name);
        return access(out, 0) == 0;
    }
    const char *runpath = req ? elfd_tag_str(req, ELFD_RUNPATH, 0) : NULL;
    const char *rpath = req && !runpath ? elfd_tag_str(req, ELFD_RPATH, 0) : NULL;
    if (rpath && search_list(rpath, name, out, cap))
        return true;
    if (root && root != req && !elfd_tag_str(root, ELFD_RUNPATH, 0) &&
        search_list(elfd_tag_str(root, ELFD_RPATH, 0), name, out, cap))
        return true;
    if (search_list(lib_path_env, name, out, cap))
        return true;
    if (runpath && search_list(runpath, name, out, cap))
        return true;
    if (cache_find(name, out, cap))
        return true;
    return search_list("/lib:/usr/lib:/lib64", name, out, cap);
}

static void walk(const struct elfd *e, const struct elfd *root)
{
    for (size_t i = 0;; i++) {
        const char *name = elfd_tag_str(e, ELFD_NEEDED, i);
        if (!name)
            break;
        bool dup = false;
        for (int k = 0; k < nseen; k++)
            if (!strcmp(seen[k], name))
                dup = true;
        if (dup)
            continue;
        if (nseen < MAX_SEEN)
            seen[nseen++] = strdup(name);
        char path[512];
        if (!resolve(e, root, name, path, sizeof(path))) {
            printf("\t%s => not found\n", name);
            missing = 1;
            continue;
        }
        printf("\t%s => %s\n", name, path);
        struct elfd dep;
        if (elfd_load(path, &dep) == 0) {
            walk(&dep, root);
            elfd_free(&dep);
        }
    }
}

static int run_traced(const char *path)
{
    extern char **environ;
    size_t n = 0;
    while (environ && environ[n])
        n++;
    char **env = malloc((n + 2) * sizeof(char *));
    if (!env)
        return 1;
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (strncmp(environ[i], "LD_TRACE_LOADED_OBJECTS=", 24) != 0)
            env[k++] = environ[i];
    env[k++] = "LD_TRACE_LOADED_OBJECTS=1";
    env[k] = NULL;
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        free(env);
        return util_perr("fork");
    }
    if (pid == 0) {
        char *av[2] = {(char *)path, NULL};
        execve(path, av, env);
        _exit(127);
    }
    free(env);
    int st = 0;
    waitpid(pid, &st, 0);
    return (st & 0x7f) == 0 ? (st >> 8) & 0xff : 1;
}

static int do_one(const char *path, bool header)
{
    struct elfd e;
    int r = elfd_load(path, &e);
    if (r == -1)
        return util_perr(path);
    if (header)
        printf("%s:\n", path);
    if (r < 0) {
        printf("\tnot a dynamic executable\n");
        return 1;
    }
    int rc = 0;
    if (e.has_interp && (e.type == 2 || e.type == 3)) {
        elfd_free(&e);
        return run_traced(path);
    }
    if (!e.dyn) {
        printf("\tstatically linked\n");
        elfd_free(&e);
        return 0;
    }
    nseen = 0;
    missing = 0;
    walk(&e, &e);
    rc = missing;
    elfd_free(&e);
    return rc;
}

int main(int argc, char **argv)
{
    util_init(argc, argv);
    lib_path_env = getenv("LD_LIBRARY_PATH");
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "--help")) { util_usage("FILE..."); return 0; }
        return util_err("invalid option '%s'", argv[i]);
    }
    if (i >= argc)
        return util_err("missing file operand");
    bool header = argc - i > 1;
    int rc = 0;
    for (; i < argc; i++)
        if (do_one(argv[i], header))
            rc = 1;
    return rc;
}
