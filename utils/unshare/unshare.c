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

static const struct kind *find_long(const char *name, size_t length)
{
    for (size_t i = 0; i < KIND_COUNT; i++)
        if (strlen(kinds[i].name) == length && !strncmp(kinds[i].name, name, length))
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

static int request(const struct kind *kind, int *flags)
{
    if (!kind->supported)
        return util_err("%s namespace is not supported yet", kind->name);
    *flags |= kind->flag;
    return 0;
}

int main(int argc, char **argv)
{
    util_init(argc, argv);
    int flags = 0;
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
            util_usage("[-u|--uts] [-m] [-p] [-i] [-n] [-U] [-C] [-T] [--] [COMMAND [ARG]...]");
            return 0;
        }
        if (word[1] == '-') {
            const struct kind *kind = find_long(word + 2, strlen(word + 2));
            if (!kind)
                return util_err("unrecognized option '%s'", word);
            if (request(kind, &flags))
                return 1;
            continue;
        }
        for (const char *letter = word + 1; *letter; letter++) {
            const struct kind *kind = find_short(*letter);
            if (!kind)
                return util_err("invalid option -- '%c'", *letter);
            if (request(kind, &flags))
                return 1;
        }
    }
    if (flags && unshare(flags) != 0)
        return util_perr("unshare failed");
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
