#include "../lib/common.h"
#include <sys/mount.h>

int main(int argc, char **argv)
{
    util_init(argc, argv);
    int flags = 0;
    int status = 0;
    int targets = 0;
    bool stop = false;
    for (int i = 1; i < argc; i++) {
        const char *word = argv[i];
        if (!stop && !strcmp(word, "--")) {
            stop = true;
            continue;
        }
        if (!stop && word[0] == '-' && word[1]) {
            if (!strcmp(word, "--help")) {
                util_usage("[-lf] TARGET...");
                return 0;
            }
            if (!strcmp(word, "--lazy")) {
                flags |= MNT_DETACH;
                continue;
            }
            if (!strcmp(word, "--force")) {
                flags |= MNT_FORCE;
                continue;
            }
            if (word[1] == '-')
                return util_err("unrecognized option '%s'", word);
            for (const char *letter = word + 1; *letter; letter++) {
                if (*letter == 'l')
                    flags |= MNT_DETACH;
                else if (*letter == 'f')
                    flags |= MNT_FORCE;
                else if (*letter != 'v' && *letter != 'n')
                    return util_err("invalid option -- '%c'", *letter);
            }
            continue;
        }
        targets++;
        if (umount2(word, flags) != 0)
            status = util_err("%s: %s", word, errno == EINVAL ? "not mounted" : strerror(errno));
    }
    if (!targets)
        return util_err("usage: umount [-lf] TARGET...");
    return status ? 1 : 0;
}
