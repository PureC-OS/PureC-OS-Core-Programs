#include "../lib/common.h"

static bool opt_symbolic, opt_force, opt_verbose, opt_no_deref, opt_no_target_dir;

static bool is_real_dir(const char *path)
{
    struct stat st;
    if (opt_no_deref) {
        if (lstat(path, &st) != 0) return false;
    } else if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

static int make_link(const char *target, const char *name)
{
    struct stat st;
    if (lstat(name, &st) == 0) {
        if (!opt_force)
            return util_err("failed to create %s link '%s': File exists",
                            opt_symbolic ? "symbolic" : "hard", name);
        if (S_ISDIR(st.st_mode))
            return util_err("cannot overwrite directory '%s'", name);
        if (unlink(name) != 0) {
            util_perr(name);
            return 1;
        }
    }
    int rc = opt_symbolic ? symlink(target, name) : link(target, name);
    if (rc != 0) {
        if (!opt_symbolic && errno == EPERM)
            return util_err("failed to create hard link '%s': hard links are not supported yet, use -s", name);
        return util_err("failed to create %s link '%s' -> '%s': %s",
                        opt_symbolic ? "symbolic" : "hard", name, target, strerror(errno));
    }
    if (opt_verbose) printf("'%s' -> '%s'\n", name, target);
    return 0;
}

int main(int argc, char **argv)
{
    util_init(argc, argv);
    struct opts o;
    opts_begin(&o, argc, argv);
    int c;
    while ((c = opts_next(&o, "sfvnT")) != -1) {
        if (c == 's') opt_symbolic = true;
        else if (c == 'f') opt_force = true;
        else if (c == 'v') opt_verbose = true;
        else if (c == 'n') opt_no_deref = true;
        else if (c == 'T') opt_no_target_dir = true;
        else if (c == 'L' && opts_long(&o, "symbolic")) opt_symbolic = true;
        else if (c == 'L' && opts_long(&o, "force")) opt_force = true;
        else if (c == 'L' && opts_long(&o, "verbose")) opt_verbose = true;
        else if (c == 'L' && opts_long(&o, "no-dereference")) opt_no_deref = true;
        else if (c == 'L' && opts_long(&o, "help")) {
            util_usage("[-sfvnT] TARGET [LINK_NAME]\n       ln [-sfvn] TARGET... DIRECTORY");
            return 0;
        }
        else return util_bad_option(&o, c);
    }
    if (!o.count) return util_err("missing file operand");
    if (o.count == 1)
        return make_link(o.operands[0], util_base(o.operands[0]));
    const char *last = o.operands[o.count - 1];
    if (!opt_no_target_dir && is_real_dir(last)) {
        int rc = 0;
        for (int i = 0; i < o.count - 1; i++) {
            char name[UTIL_PATH];
            util_join(name, sizeof(name), last, util_base(o.operands[i]));
            rc |= make_link(o.operands[i], name);
        }
        return rc;
    }
    if (o.count > 2) return util_err("target '%s' is not a directory", last);
    return make_link(o.operands[0], last);
}
