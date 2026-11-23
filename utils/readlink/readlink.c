#include "../lib/common.h"

int main(int argc, char **argv)
{
    util_init(argc, argv);
    struct opts o;
    opts_begin(&o, argc, argv);
    bool canonical = false, no_newline = false, quiet = false;
    int c;
    while ((c = opts_next(&o, "femnqsvz")) != -1) {
        if (c == 'f' || c == 'e' || c == 'm') canonical = true;
        else if (c == 'n') no_newline = true;
        else if (c == 'q' || c == 's') quiet = true;
        else if (c == 'v') quiet = false;
        else if (c == 'z') continue;
        else if (c == 'L' && opts_long(&o, "canonicalize")) canonical = true;
        else if (c == 'L' && opts_long(&o, "help")) { util_usage("[-fn] FILE..."); return 0; }
        else return util_bad_option(&o, c);
    }
    if (!o.count) return util_err("missing operand");
    int rc = 0;
    for (int i = 0; i < o.count; i++) {
        char out[UTIL_PATH];
        if (canonical) {
            if (!realpath(o.operands[i], out)) {
                if (!quiet) util_perr(o.operands[i]);
                rc = 1;
                continue;
            }
        } else {
            ssize_t n = readlink(o.operands[i], out, sizeof(out) - 1);
            if (n < 0) {
                if (!quiet) util_perr(o.operands[i]);
                rc = 1;
                continue;
            }
            out[n] = 0;
        }
        fputs(out, stdout);
        if (!no_newline || o.count > 1) putchar('\n');
    }
    return rc;
}
