#include "../lib/common.h"
#include <sched.h>

struct kind {
    char letter;
    const char *name;
    int flag;
    bool supported;
};

static const struct kind kinds[] = {
    {'u', "uts", CLONE_NEWUTS, true},
    {'m', "mount", CLONE_NEWNS, false},
    {'p', "pid", CLONE_NEWPID, false},
    {'i', "ipc", CLONE_NEWIPC, false},
    {'n', "net", CLONE_NEWNET, false},
    {'U', "user", CLONE_NEWUSER, false},
    {'C', "cgroup", CLONE_NEWCGROUP, false},
    {'T', "time", CLONE_NEWTIME, false},
};

#define KIND_COUNT (sizeof(kinds) / sizeof(kinds[0]))

static const struct kind *find_long(const char *name)
{
    for (size_t i = 0; i < KIND_COUNT; i++)
        if (!strcmp(kinds[i].name, name))
            return &kinds[i];
    return NULL;
}

static const struct kind *find_short(char letter)
{
    for (size_t i = 0; i < KIND_COUNT; i++)
        if (kinds[i].letter == letter)
            return &kinds[i];
    return NULL;
}

static int parse_pid(const char *text, long *pid)
{
    if (!util_parse_long(text, pid) || *pid <= 0)
        return util_err("invalid PID '%s'", text);
    return 0;
}

static int enter(long pid, const struct kind *kind)
{
    if (!kind->supported)
        return util_err("%s namespace is not supported yet", kind->name);
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/ns/%s", pid, kind->name);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return util_err("cannot open %s: %s", path, strerror(errno));
    int status = setns(fd, kind->flag);
    int saved = errno;
    close(fd);
    if (status != 0)
        return util_err("reassociate to namespace '%s' failed: %s", kind->name, strerror(saved));
    return 0;
}

int main(int argc, char **argv)
{
    util_init(argc, argv);
    long pid = 0;
    bool have_pid = false;
    bool wanted[KIND_COUNT] = {false};
    int i = 1;
    for (; i < argc; i++) {
        const char *word = argv[i];
        if (word[0] != '-' || !word[1])
            break;
        if (!strcmp(word, "--")) {
            i++;
            break;
        }
        if (!strcmp(word, "--help")) {
            util_usage("-t PID [-u|--uts] [-m] [-p] [-i] [-n] [-U] [-C] [-T] [--] [COMMAND [ARG]...]");
            return 0;
        }
        if (word[1] == '-') {
            const char *name = word + 2;
            if (!strncmp(name, "target", 6) && (name[6] == '=' || !name[6])) {
                const char *value = name[6] == '=' ? name + 7 : (i + 1 < argc ? argv[++i] : NULL);
                if (!value)
                    return util_err("option '--target' requires an argument");
                if (parse_pid(value, &pid))
                    return 1;
                have_pid = true;
                continue;
            }
            const struct kind *kind = find_long(name);
            if (!kind)
                return util_err("unrecognized option '%s'", word);
            wanted[kind - kinds] = true;
            continue;
        }
        for (const char *letter = word + 1; *letter; letter++) {
            if (*letter == 't') {
                const char *value = letter[1] ? letter + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!value)
                    return util_err("option requires an argument -- 't'");
                if (parse_pid(value, &pid))
                    return 1;
                have_pid = true;
                break;
            }
            const struct kind *kind = find_short(*letter);
            if (!kind)
                return util_err("invalid option -- '%c'", *letter);
            wanted[kind - kinds] = true;
        }
    }
    if (!have_pid)
        return util_err("no target PID specified (use -t PID)");
    for (size_t k = 0; k < KIND_COUNT; k++)
        if (wanted[k] && enter(pid, &kinds[k]))
            return 1;
    const char *fallback[2] = {NULL, NULL};
    char **command = argv + i;
    if (i >= argc) {
        const char *shell = getenv("SHELL");
        fallback[0] = shell && *shell ? shell : "/bin/sh";
        command = (char **)fallback;
    }
    execvp(command[0], command);
    util_err("'%s': %s", command[0], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}
