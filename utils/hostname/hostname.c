#include "../lib/common.h"

int main(int argc, char **argv)
{
    util_init(argc, argv);
    bool short_name = false;
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) { util_usage("[-s] [NAME]"); return 0; }
        if (!strcmp(argv[i], "-s")) short_name = true;
        else if (argv[i][0] == '-' && argv[i][1]) return util_err("invalid option '%s'", argv[i]);
        else if (!name) name = argv[i];
        else return util_err("extra operand '%s'", argv[i]);
    }
    if (name) {
        if (sethostname(name, strlen(name)) != 0) return util_perr("sethostname");
        return 0;
    }
    char host[256];
    if (gethostname(host, sizeof(host)) != 0) return util_perr("gethostname");
    if (short_name) {
        char *dot = strchr(host, '.');
        if (dot) *dot = 0;
    }
    puts(host);
    return 0;
}
