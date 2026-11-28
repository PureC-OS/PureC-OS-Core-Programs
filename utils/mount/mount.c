#include "../lib/common.h"
#include <sys/mount.h>

struct option_flag {
    const char *name;
    unsigned long set;
    unsigned long clear;
};

static const struct option_flag option_flags[] = {
    {"ro", MS_RDONLY, 0},
    {"rw", 0, MS_RDONLY},
    {"remount", MS_REMOUNT, 0},
    {"bind", MS_BIND, 0},
    {"rbind", MS_BIND | MS_REC, 0},
    {"nosuid", MS_NOSUID, 0},
    {"nodev", MS_NODEV, 0},
    {"noexec", MS_NOEXEC, 0},
    {"sync", MS_SYNCHRONOUS, 0},
    {"dirsync", MS_DIRSYNC, 0},
    {"noatime", MS_NOATIME, 0},
    {"nodiratime", MS_NODIRATIME, 0},
    {"relatime", MS_RELATIME, 0},
    {"strictatime", MS_STRICTATIME, 0},
    {"silent", MS_SILENT, 0},
    {"private", MS_PRIVATE, 0},
    {"rprivate", MS_PRIVATE | MS_REC, 0},
    {"shared", MS_SHARED, 0},
    {"rshared", MS_SHARED | MS_REC, 0},
    {"slave", MS_SLAVE, 0},
    {"rslave", MS_SLAVE | MS_REC, 0},
    {"unbindable", MS_UNBINDABLE, 0},
    {"runbindable", MS_UNBINDABLE | MS_REC, 0},
    {"defaults", 0, 0},
    {"auto", 0, 0},
    {"noauto", 0, 0},
    {"user", 0, 0},
    {"nouser", 0, 0},
    {"dev", 0, 0},
    {"suid", 0, 0},
    {"exec", 0, 0},
    {"async", 0, 0},
    {"atime", 0, 0},
};

#define OPTION_COUNT (sizeof(option_flags) / sizeof(option_flags[0]))

static unsigned long flags;
static char *data;
static size_t data_length;

static int add_data(const char *token, size_t length)
{
    char *grown = realloc(data, data_length + length + 2);
    if (!grown)
        return util_err("out of memory");
    data = grown;
    if (data_length)
        data[data_length++] = ',';
    memcpy(data + data_length, token, length);
    data_length += length;
    data[data_length] = '\0';
    return 0;
}

static int parse_options(const char *list)
{
    while (*list) {
        const char *end = strchr(list, ',');
        size_t length = end ? (size_t)(end - list) : strlen(list);
        if (length) {
            bool known = false;
            for (size_t i = 0; i < OPTION_COUNT; i++) {
                if (strlen(option_flags[i].name) == length && !strncmp(option_flags[i].name, list, length)) {
                    flags |= option_flags[i].set;
                    flags &= ~option_flags[i].clear;
                    known = true;
                    break;
                }
            }
            if (!known && add_data(list, length))
                return 1;
        }
        if (!end)
            break;
        list = end + 1;
    }
    return 0;
}

static int show_mounts(void)
{
    int fd = open("/proc/self/mounts", O_RDONLY);
    if (fd < 0)
        return util_perr("/proc/self/mounts");
    char block[4096];
    ssize_t got;
    while ((got = read(fd, block, sizeof(block))) > 0)
        fwrite(block, 1, (size_t)got, stdout);
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    util_init(argc, argv);
    const char *type = NULL;
    const char *operands_storage[2] = {NULL, NULL};
    int operands = 0;
    bool stop = false;
    for (int i = 1; i < argc; i++) {
        const char *word = argv[i];
        if (stop || word[0] != '-' || !word[1]) {
            if (operands == 2)
                return util_err("too many arguments");
            operands_storage[operands++] = word;
            continue;
        }
        if (!strcmp(word, "--")) {
            stop = true;
            continue;
        }
        if (!strcmp(word, "--help") || !strcmp(word, "-h")) {
            util_usage("[-t TYPE] [-o OPTIONS] [-r] SOURCE TARGET\n       mount --bind|--rbind SOURCE TARGET\n       mount -o remount[,ro|rw] TARGET\n       mount --make-[r]private|shared|slave TARGET\n       mount");
            return 0;
        }
        if (!strcmp(word, "--bind") || !strcmp(word, "-B")) {
            flags |= MS_BIND;
        } else if (!strcmp(word, "--rbind") || !strcmp(word, "-R")) {
            flags |= MS_BIND | MS_REC;
        } else if (!strcmp(word, "--read-only") || !strcmp(word, "-r")) {
            flags |= MS_RDONLY;
        } else if (!strcmp(word, "--read-write") || !strcmp(word, "-w")) {
            flags &= ~MS_RDONLY;
        } else if (!strncmp(word, "--make-", 7)) {
            if (parse_options(word + 7))
                return 1;
        } else if (!strcmp(word, "-t") || !strcmp(word, "--types") || !strcmp(word, "-o") || !strcmp(word, "--options")) {
            if (i + 1 >= argc)
                return util_err("option '%s' requires an argument", word);
            const char *value = argv[++i];
            if (word[1] == 't' || word[2] == 't') {
                type = value;
            } else if (parse_options(value)) {
                return 1;
            }
        } else if (!strncmp(word, "-t", 2)) {
            type = word + 2;
        } else if (!strncmp(word, "-o", 2)) {
            if (parse_options(word + 2))
                return 1;
        } else if (!strncmp(word, "--types=", 8)) {
            type = word + 8;
        } else if (!strncmp(word, "--options=", 10)) {
            if (parse_options(word + 10))
                return 1;
        } else if (!strcmp(word, "-v") || !strcmp(word, "-n") || !strcmp(word, "-f") || !strcmp(word, "--verbose")) {
        } else {
            return util_err("invalid option '%s'", word);
        }
    }
    if (operands == 0 && !flags && !type && !data)
        return show_mounts();
    const char *source = NULL;
    const char *target = NULL;
    bool modifies = flags & (MS_REMOUNT | MS_PRIVATE | MS_SHARED | MS_SLAVE | MS_UNBINDABLE);
    if (operands == 2) {
        source = operands_storage[0];
        target = operands_storage[1];
    } else if (operands == 1 && (modifies || type)) {
        target = operands_storage[0];
        source = type ? type : "none";
    } else {
        return util_err("usage: mount [-t TYPE] [-o OPTIONS] SOURCE TARGET");
    }
    if (!type && !(flags & MS_BIND) && !modifies)
        return util_err("you must specify the filesystem type");
    if (flags & (MS_BIND | MS_REMOUNT | MS_PRIVATE | MS_SHARED | MS_SLAVE | MS_UNBINDABLE))
        type = NULL;
    if (mount(source, target, type, flags, data) != 0)
        return util_err("%s: %s", target, strerror(errno));
    return 0;
}
