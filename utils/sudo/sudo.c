#include "sudoers.h"
#include "purecrypt.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SUDO_VERSION "1.0"
#define DEFAULT_PATH "/usr/bin:/bin:/usr/sbin:/sbin"
#define PASSWORD_BUFFER 512
#define TS_MAGIC "PCSUDO1"

struct options {
    const char *user;
    const char *group;
    bool login;
    bool shell;
    bool list;
    bool invalidate;
    bool remove_all;
    bool validate;
    bool non_interactive;
    bool preserve_env;
    int first_command;
};

static const char *const default_keep[] = {
    "TERM", "COLORTERM", "DISPLAY", "HOSTNAME", "KRB5CCNAME", "LS_COLORS", "PATH",
    "PS1", "PS2", "XAUTHORITY", "XAUTHORIZATION", "LANG", "LANGUAGE", "LC_*", "TZ",
    NULL};

static const char *const always_delete[] = {"LD_*", "ENV", "BASH_ENV", "IFS", "CDPATH",
                                            "SHELLOPTS", "PS4", NULL};

static void usage(FILE *out)
{
    fputs("usage: sudo -h | -K | -k | -V\n"
          "usage: sudo -v [-kn]\n"
          "usage: sudo -l [-kn] [-u user] [-g group] [command]\n"
          "usage: sudo [-EiknS] [-g group] [-u user] [-s | -i | command [arg ...]]\n",
          out);
}

static void wipe(char *p, size_t n)
{
    volatile char *v = p;
    while (n--)
        *v++ = 0;
}

static char *timestamp_directory(void)
{
    static const char *const bases[] = {"/run", "/var/run", NULL};
    for (int i = 0; bases[i]; i++) {
        struct stat info;
        if (stat(bases[i], &info) != 0 || !S_ISDIR(info.st_mode))
            continue;
        char *step = sudoers_format("%s/sudo", bases[i]);
        int made = mkdir(step, 0700) != 0 && errno != EEXIST;
        free(step);
        if (made)
            continue;
        step = sudoers_format("%s/sudo/ts", bases[i]);
        made = mkdir(step, 0700) != 0 && errno != EEXIST;
        if (made) {
            free(step);
            continue;
        }
        return step;
    }
    return NULL;
}

static char *terminal_id(void)
{
    const char *name = ttyname(0);
    if (!name)
        name = ttyname(1);
    if (!name)
        name = ttyname(2);
    if (!name)
        return sudoers_format("ppid%d", (int)getppid());
    char *id = sudoers_xdup(name);
    for (char *c = id; *c; c++)
        if (*c == '/')
            *c = '_';
    return id;
}

static char *timestamp_path(const char *user)
{
    char *directory = timestamp_directory();
    if (!directory)
        return NULL;
    char *tty = terminal_id();
    char *path = sudoers_format("%s/%s.%s", directory, user, tty);
    free(directory);
    free(tty);
    return path;
}

static long monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return (long)time(NULL);
    return (long)now.tv_sec;
}

static bool timestamp_valid(const char *user, int timeout_minutes)
{
    if (timeout_minutes <= 0)
        return false;
    char *path = timestamp_path(user);
    if (!path)
        return false;
    struct stat info;
    int fd = -1;
    if (stat(path, &info) == 0 && info.st_uid == 0 && !(info.st_mode & 077))
        fd = open(path, O_RDONLY);
    free(path);
    if (fd < 0)
        return false;
    char text[64];
    ssize_t got = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (got <= 0)
        return false;
    text[got] = 0;
    if (strncmp(text, TS_MAGIC "\n", sizeof(TS_MAGIC)) != 0)
        return false;
    long stamp = atol(text + sizeof(TS_MAGIC));
    long now = monotonic_seconds();
    return stamp <= now && now - stamp < (long)timeout_minutes * 60;
}

static void timestamp_update(const char *user)
{
    char *path = timestamp_path(user);
    if (!path)
        return;
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
        char text[64];
        int length = snprintf(text, sizeof(text), TS_MAGIC "\n%ld\n", monotonic_seconds());
        if (write(fd, text, (size_t)length) < 0)
            unlink(path);
        close(fd);
        chmod(path, 0600);
    }
    free(path);
}

static void timestamp_remove(const char *user)
{
    char *path = timestamp_path(user);
    if (path)
        unlink(path);
    free(path);
}

static void timestamp_remove_all(const char *user)
{
    char *directory = timestamp_directory();
    if (!directory)
        return;
    DIR *dir = opendir(directory);
    if (dir) {
        size_t length = strlen(user);
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, user, length) == 0 && entry->d_name[length] == '.') {
                char *path = sudoers_format("%s/%s", directory, entry->d_name);
                unlink(path);
                free(path);
            }
        }
        closedir(dir);
    }
    free(directory);
}

static char *shadow_hash(const char *user)
{
    int fd = open("/etc/shadow", O_RDONLY);
    if (fd < 0)
        return NULL;
    size_t capacity = 4096;
    size_t length = 0;
    char *buffer = sudoers_xalloc(capacity);
    for (;;) {
        if (length + 1 >= capacity) {
            capacity *= 2;
            buffer = sudoers_xrealloc(buffer, capacity);
        }
        ssize_t got = read(fd, buffer + length, capacity - length - 1);
        if (got <= 0)
            break;
        length += (size_t)got;
    }
    close(fd);
    buffer[length] = 0;
    size_t name_length = strlen(user);
    char *result = NULL;
    for (char *line = buffer; *line;) {
        char *next = strchr(line, '\n');
        if (strncmp(line, user, name_length) == 0 && line[name_length] == ':') {
            const char *hash = line + name_length + 1;
            size_t n = 0;
            while (hash[n] && hash[n] != ':' && hash[n] != '\n')
                n++;
            if (n) {
                result = sudoers_xalloc(n + 1);
                memcpy(result, hash, n);
            }
            break;
        }
        if (!next)
            break;
        line = next + 1;
    }
    wipe(buffer, capacity);
    free(buffer);
    return result;
}

static int read_password(const char *prompt, char *out, size_t capacity)
{
    struct termios saved, quiet;
    bool tty = tcgetattr(0, &saved) == 0;
    if (tty) {
        quiet = saved;
        quiet.c_lflag &= ~(tcflag_t)(ECHO | ECHOE | ECHOK);
        tcsetattr(0, TCSAFLUSH, &quiet);
    }
    fputs(prompt, stderr);
    fflush(stderr);
    size_t length = 0;
    int status = -1;
    for (;;) {
        char c;
        ssize_t got = read(0, &c, 1);
        if (got <= 0)
            break;
        if (c == '\n' || c == '\r') {
            status = 0;
            break;
        }
        if ((c == 127 || c == '\b') && length) {
            length--;
            continue;
        }
        if (length + 1 < capacity)
            out[length++] = c;
    }
    out[length] = 0;
    if (tty)
        tcsetattr(0, TCSAFLUSH, &saved);
    fputc('\n', stderr);
    return status;
}

static bool password_ok(const char *user, const char *password)
{
    char *hash = shadow_hash(user);
    if (!hash)
        return false;
    bool ok = hash[0] == '$' && hash[1] == '6' && hash[2] == '$' &&
              purecrypt_verify(password, hash) == 1;
    free(hash);
    return ok;
}

static int authenticate(const char *auth_user, int tries)
{
    for (int attempt = 0; attempt < tries; attempt++) {
        char password[PASSWORD_BUFFER];
        char *prompt = sudoers_format("[sudo] password for %s: ", auth_user);
        int read_status = read_password(prompt, password, sizeof(password));
        free(prompt);
        if (read_status != 0) {
            wipe(password, sizeof(password));
            fputs("sudo: no password was provided\n", stderr);
            fprintf(stderr, "sudo: %d incorrect password attempt%s\n", attempt + 1,
                    attempt ? "s" : "");
            return -1;
        }
        bool ok = password_ok(auth_user, password);
        wipe(password, sizeof(password));
        if (ok)
            return 0;
        if (attempt + 1 < tries)
            fputs("Sorry, try again.\n", stderr);
    }
    fprintf(stderr, "sudo: %d incorrect password attempt%s\n", tries, tries == 1 ? "" : "s");
    return -1;
}

static bool pattern_in(const char *const *patterns, const char *name)
{
    for (int i = 0; patterns[i]; i++)
        if (fnmatch(patterns[i], name, 0) == 0)
            return true;
    return false;
}

static void env_put(char ***env, int *count, const char *name, const char *value)
{
    size_t name_length = strlen(name);
    char *entry = sudoers_format("%s=%s", name, value);
    for (int i = 0; i < *count; i++) {
        if (strncmp((*env)[i], name, name_length) == 0 && (*env)[i][name_length] == '=') {
            free((*env)[i]);
            (*env)[i] = entry;
            return;
        }
    }
    *env = sudoers_xrealloc(*env, sizeof(char *) * (size_t)(*count + 2));
    (*env)[(*count)++] = entry;
    (*env)[*count] = NULL;
}

static bool keep_by_settings(const struct sudoers_settings *settings, const char *name)
{
    if (pattern_in(default_keep, name))
        return true;
    for (int i = 0; i < settings->env_keep.count; i++)
        if (fnmatch(settings->env_keep.item[i], name, 0) == 0)
            return true;
    return false;
}

static char **build_environment(const struct sudoers_settings *settings, bool preserve,
                                const struct passwd *target, const struct passwd *invoker,
                                const char *command_line, const char *shell)
{
    char **env = NULL;
    int count = 0;
    env = sudoers_xalloc(sizeof(char *));
    for (char **e = environ; e && *e; e++) {
        char *equals = strchr(*e, '=');
        if (!equals)
            continue;
        size_t length = (size_t)(equals - *e);
        if (length == 0)
            continue;
        char *name = sudoers_xalloc(length + 1);
        memcpy(name, *e, length);
        bool skip = pattern_in(always_delete, name) ||
                    (settings->env_reset && !preserve && !keep_by_settings(settings, name));
        if (!skip)
            env_put(&env, &count, name, equals + 1);
        free(name);
    }
    char number[32];
    env_put(&env, &count, "HOME", target->pw_dir ? target->pw_dir : "/");
    env_put(&env, &count, "USER", target->pw_name);
    env_put(&env, &count, "LOGNAME", target->pw_name);
    env_put(&env, &count, "SHELL", shell);
    env_put(&env, &count, "SUDO_USER", invoker->pw_name);
    snprintf(number, sizeof(number), "%u", (unsigned)invoker->pw_uid);
    env_put(&env, &count, "SUDO_UID", number);
    snprintf(number, sizeof(number), "%u", (unsigned)invoker->pw_gid);
    env_put(&env, &count, "SUDO_GID", number);
    env_put(&env, &count, "SUDO_COMMAND", command_line);
    if (settings->secure_path)
        env_put(&env, &count, "PATH", settings->secure_path);
    else if (!getenv("PATH") || (settings->env_reset && !preserve && !keep_by_settings(settings, "PATH")))
        env_put(&env, &count, "PATH", DEFAULT_PATH);
    return env;
}

static bool executable_file(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode) && (info.st_mode & 0111);
}

static bool resolve_command(const char *name, const char *search, char **out)
{
    if (strchr(name, '/')) {
        char *real = realpath(name, NULL);
        if (real && executable_file(real)) {
            *out = real;
            return true;
        }
        free(real);
        *out = sudoers_xdup(name);
        return false;
    }
    char *path = sudoers_xdup(search && *search ? search : DEFAULT_PATH);
    char *cursor = path;
    while (cursor) {
        char *end = strchr(cursor, ':');
        if (end)
            *end = 0;
        if (*cursor == '/') {
            char *candidate = sudoers_format("%s/%s", cursor, name);
            if (executable_file(candidate)) {
                *out = candidate;
                free(path);
                return true;
            }
            free(candidate);
        }
        cursor = end ? end + 1 : NULL;
    }
    free(path);
    *out = sudoers_xdup(name);
    return false;
}

static char *join_arguments(char **argv, int count)
{
    size_t total = 1;
    for (int i = 0; i < count; i++)
        total += strlen(argv[i]) + 1;
    char *out = sudoers_xalloc(total);
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        size_t length = strlen(argv[i]);
        if (i)
            out[used++] = ' ';
        memcpy(out + used, argv[i], length);
        used += length;
    }
    out[used] = 0;
    return out;
}

static char *current_directory(void)
{
    size_t capacity = 128;
    for (;;) {
        char *buffer = sudoers_xalloc(capacity);
        if (getcwd(buffer, capacity))
            return buffer;
        free(buffer);
        if (errno != ERANGE)
            return sudoers_xdup("?");
        capacity *= 2;
    }
}

static void log_event(const char *invoker, const char *message, const char *runas,
                      const char *command)
{
    char *cwd = current_directory();
    char *tty = terminal_id();
    syslog(LOG_NOTICE, "%s : %sTTY=%s ; PWD=%s ; USER=%s ; COMMAND=%s", invoker, message, tty,
           cwd, runas, command);
    free(cwd);
    free(tty);
}

static void print_defaults(const struct sudoers_settings *s)
{
    bool first = true;
    if (s->env_reset) {
        fputs("env_reset", stdout);
        first = false;
    }
    if (s->secure_path) {
        printf("%ssecure_path=", first ? "" : ", ");
        for (const char *c = s->secure_path; *c; c++) {
            if (*c == ':')
                fputc('\\', stdout);
            fputc(*c, stdout);
        }
        first = false;
    }
    if (s->timestamp_timeout != 5) {
        printf("%stimestamp_timeout=%d", first ? "" : ", ", s->timestamp_timeout);
        first = false;
    }
    if (s->passwd_tries != 3) {
        printf("%spasswd_tries=%d", first ? "" : ", ", s->passwd_tries);
        first = false;
    }
    if (!first)
        fputc('\n', stdout);
}

static int parse_options(int argc, char **argv, struct options *o)
{
    memset(o, 0, sizeof(*o));
    int i = 1;
    for (; i < argc; i++) {
        char *arg = argv[i];
        if (arg[0] != '-' || !arg[1])
            break;
        if (strcmp(arg, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(arg, "--help") == 0) {
            usage(stdout);
            exit(0);
        }
        if (strcmp(arg, "--version") == 0) {
            printf("sudo version %s\n", SUDO_VERSION);
            exit(0);
        }
        bool stop_cluster = false;
        for (char *c = arg + 1; *c && !stop_cluster; c++) {
            switch (*c) {
            case 'u':
            case 'g': {
                const char *value = c[1] ? c + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!value) {
                    fprintf(stderr, "sudo: option requires an argument -- '%c'\n", *c);
                    usage(stderr);
                    exit(1);
                }
                if (*c == 'u')
                    o->user = value;
                else
                    o->group = value;
                stop_cluster = true;
                break;
            }
            case 'i':
                o->login = true;
                break;
            case 's':
                o->shell = true;
                break;
            case 'l':
                o->list = true;
                break;
            case 'k':
                o->invalidate = true;
                break;
            case 'K':
                o->remove_all = true;
                break;
            case 'v':
                o->validate = true;
                break;
            case 'n':
                o->non_interactive = true;
                break;
            case 'E':
                o->preserve_env = true;
                break;
            case 'S':
                break;
            case 'h':
                usage(stdout);
                exit(0);
            case 'V':
                printf("sudo version %s\n", SUDO_VERSION);
                exit(0);
            default:
                fprintf(stderr, "sudo: invalid option -- '%c'\n", *c);
                usage(stderr);
                exit(1);
            }
        }
    }
    o->first_command = i;
    return 0;
}

static struct passwd *copy_passwd(const struct passwd *source)
{
    struct passwd *copy = sudoers_xalloc(sizeof(*copy));
    *copy = *source;
    copy->pw_name = sudoers_xdup(source->pw_name);
    copy->pw_dir = sudoers_xdup(source->pw_dir ? source->pw_dir : "/");
    copy->pw_shell = sudoers_xdup(source->pw_shell && *source->pw_shell ? source->pw_shell : "/bin/sh");
    return copy;
}

static bool numeric_id(const char *text)
{
    return text[0] == '#' && isdigit((unsigned char)text[1]);
}

int main(int argc, char **argv)
{
    struct options opt;
    parse_options(argc, argv, &opt);
    openlog("sudo", 0, LOG_USER);
    if (geteuid() != 0) {
        fputs("sudo: effective uid is not 0, is /usr/bin/sudo on a filesystem with the 'nosuid' option set?\n",
              stderr);
        return 1;
    }
    uid_t real_uid = getuid();
    struct passwd *found = getpwuid(real_uid);
    if (!found) {
        fputs("sudo: you do not exist in the passwd database\n", stderr);
        return 1;
    }
    struct passwd *invoker = copy_passwd(found);

    bool bare = opt.first_command >= argc && !opt.shell && !opt.login && !opt.list && !opt.validate;
    if (opt.remove_all) {
        timestamp_remove_all(invoker->pw_name);
        if (bare)
            return 0;
    }
    if (opt.invalidate) {
        timestamp_remove(invoker->pw_name);
        if (bare)
            return 0;
    }
    bool has_command = opt.first_command < argc;
    if (!has_command && !opt.shell && !opt.login && !opt.list && !opt.validate) {
        usage(stderr);
        return 1;
    }
    if (opt.shell && opt.login) {
        fputs("sudo: you may not specify both the '-i' and '-s' options\n", stderr);
        return 1;
    }

    struct passwd *target_found;
    if (opt.user) {
        target_found = numeric_id(opt.user) ? getpwuid((uid_t)atol(opt.user + 1)) : getpwnam(opt.user);
    } else {
        target_found = getpwuid(0);
    }
    struct passwd *target;
    if (target_found) {
        target = copy_passwd(target_found);
    } else if (opt.user && numeric_id(opt.user)) {
        struct passwd synthetic;
        memset(&synthetic, 0, sizeof(synthetic));
        synthetic.pw_name = (char *)opt.user;
        synthetic.pw_uid = (uid_t)atol(opt.user + 1);
        synthetic.pw_gid = synthetic.pw_uid;
        synthetic.pw_dir = (char *)"/";
        synthetic.pw_shell = (char *)"/bin/sh";
        target = copy_passwd(&synthetic);
    } else {
        fprintf(stderr, "sudo: unknown user: %s\n", opt.user ? opt.user : "root");
        return 1;
    }

    gid_t target_gid = target->pw_gid;
    char *request_group = NULL;
    if (opt.group) {
        struct group *group = numeric_id(opt.group) ? getgrgid((gid_t)atol(opt.group + 1))
                                                    : getgrnam(opt.group);
        if (group) {
            request_group = sudoers_xdup(group->gr_name);
            target_gid = group->gr_gid;
        } else if (numeric_id(opt.group)) {
            request_group = sudoers_xdup(opt.group);
            target_gid = (gid_t)atol(opt.group + 1);
        } else {
            fprintf(stderr, "sudo: unknown group: %s\n", opt.group);
            return 1;
        }
    }

    struct sudoers db;
    sudoers_init(&db, true, false);
    if (access(SUDOERS_PATH, F_OK) != 0) {
        fprintf(stderr, "sudo: unable to open %s: %s\n", SUDOERS_PATH, strerror(errno));
        return 1;
    }
    if (sudoers_load(&db, SUDOERS_PATH) != 0) {
        fprintf(stderr, "sudo: %s\n", db.error);
        return 1;
    }

    struct sudoers_subject subject = {invoker->pw_name, invoker->pw_uid, invoker->pw_gid};
    struct sudoers_request request = {target->pw_name, target->pw_uid, request_group,
                                      target_gid, "", ""};
    struct sudoers_settings settings;
    sudoers_settings(&db, &subject, &request, &settings);

    char *command_path = NULL;
    char *command_line = NULL;
    char *arguments = sudoers_xdup("");
    char *shell_arguments = sudoers_xdup("");
    char **exec_argv = NULL;
    const char *shell_path = NULL;
    bool found_command = true;
    bool use_shell = opt.shell || opt.login;

    if (use_shell) {
        const char *shell_env = getenv("SHELL");
        shell_path = opt.login ? target->pw_shell
                               : (shell_env && shell_env[0] == '/' ? shell_env : target->pw_shell);
        command_path = sudoers_xdup(shell_path);
        exec_argv = sudoers_xalloc(4 * sizeof(char *));
        const char *base = strrchr(shell_path, '/');
        exec_argv[0] = (char *)(base ? base + 1 : shell_path);
        if (has_command) {
            free(arguments);
            arguments = join_arguments(argv + opt.first_command, argc - opt.first_command);
            exec_argv[1] = (char *)"-c";
            exec_argv[2] = arguments;
            free(shell_arguments);
            shell_arguments = sudoers_format("-c %s", arguments);
            command_line = sudoers_format("%s %s", shell_path, shell_arguments);
        } else {
            command_line = sudoers_xdup(shell_path);
        }
    } else if (has_command) {
        found_command = resolve_command(argv[opt.first_command],
                                        settings.secure_path ? settings.secure_path : getenv("PATH"),
                                        &command_path);
        free(arguments);
        arguments = join_arguments(argv + opt.first_command + 1, argc - opt.first_command - 1);
        command_line = arguments[0] ? sudoers_format("%s %s", command_path, arguments)
                                    : sudoers_xdup(command_path);
        exec_argv = sudoers_xalloc((size_t)(argc - opt.first_command + 1) * sizeof(char *));
        for (int i = opt.first_command; i < argc; i++)
            exec_argv[i - opt.first_command] = argv[i];
    } else {
        command_path = sudoers_xdup("");
        command_line = sudoers_xdup("");
    }

    request.command = command_path;
    request.arguments = use_shell ? shell_arguments : arguments;
    sudoers_settings(&db, &subject, &request, &settings);

    struct sudoers_verdict verdict;
    sudoers_check(&db, &subject, &request, &verdict);
    bool needs_command_check = !((opt.list || opt.validate) && !has_command && !use_shell);

    const char *host = db.hostname && db.hostname[0] ? db.hostname : "localhost";

    if (needs_command_check && !verdict.allowed) {
        if (!verdict.listed) {
            fprintf(stderr, "%s is not in the sudoers file.\n", invoker->pw_name);
            log_event(invoker->pw_name, "user NOT in sudoers ; ", target->pw_name, command_line);
        } else {
            fprintf(stderr, "Sorry, user %s is not allowed to execute '%s' as %s on %s.\n",
                    invoker->pw_name, command_line, target->pw_name, host);
            log_event(invoker->pw_name, "command not allowed ; ", target->pw_name, command_line);
        }
        return 1;
    }
    if (!needs_command_check && !verdict.listed) {
        if (opt.list)
            printf("User %s is not allowed to run sudo on %s.\n", invoker->pw_name, host);
        else
            fprintf(stderr, "%s is not in the sudoers file.\n", invoker->pw_name);
        log_event(invoker->pw_name, "user NOT in sudoers ; ", target->pw_name, "");
        return 1;
    }

    if (opt.preserve_env && !verdict.setenv && needs_command_check) {
        fputs("sudo: sorry, you are not allowed to preserve the environment\n", stderr);
        return 1;
    }

    const char *auth_user = settings.rootpw ? "root" : settings.targetpw ? target->pw_name
                                                                         : invoker->pw_name;
    bool needs_password = real_uid != 0 || settings.rootpw || settings.targetpw;
    if (needs_command_check && verdict.passwd == 2)
        needs_password = false;
    if (!needs_command_check && sudoers_user_has_nopasswd(&db, &subject))
        needs_password = false;
    bool fresh = !opt.invalidate && timestamp_valid(invoker->pw_name, settings.timestamp_timeout);
    if (needs_password && fresh)
        needs_password = false;
    if (needs_password) {
        if (opt.non_interactive) {
            fputs("sudo: a password is required\n", stderr);
            return 1;
        }
        if (authenticate(auth_user, settings.passwd_tries) != 0) {
            log_event(invoker->pw_name, "incorrect password attempts ; ", target->pw_name,
                      command_line);
            return 1;
        }
        timestamp_update(invoker->pw_name);
    } else if (real_uid != 0 && fresh) {
        timestamp_update(invoker->pw_name);
    }

    if (opt.validate && !has_command && !use_shell)
        return 0;

    if (opt.list) {
        printf("Matching Defaults entries for %s on %s:\n    ", invoker->pw_name, host);
        print_defaults(&settings);
        printf("\nUser %s may run the following commands on %s:\n", invoker->pw_name, host);
        if (has_command)
            printf("%s\n", command_line);
        else
            sudoers_list_rules(&db, &subject, stdout);
        return 0;
    }

    if (!found_command && !use_shell) {
        fprintf(stderr, "sudo: %s: command not found\n", argv[opt.first_command]);
        return 1;
    }

    const char *run_shell = shell_path ? shell_path : target->pw_shell;
    char **env = build_environment(&settings, opt.preserve_env, target, invoker, command_line,
                                   run_shell);

    log_event(invoker->pw_name, "", target->pw_name, command_line);

    if (setresgid(target_gid, target_gid, target_gid) != 0 ||
        setresuid(target->pw_uid, target->pw_uid, target->pw_uid) != 0) {
        fputs("sudo: unable to change to runas uid or gid\n", stderr);
        return 1;
    }
    if (opt.login && chdir(target->pw_dir) != 0) {
        fprintf(stderr, "sudo: unable to change directory to %s: %s\n", target->pw_dir,
                strerror(errno));
        return 1;
    }
    execve(command_path, exec_argv, env);
    fprintf(stderr, "sudo: %s: %s\n", command_path, strerror(errno));
    return errno == ENOENT ? 127 : 126;
}
