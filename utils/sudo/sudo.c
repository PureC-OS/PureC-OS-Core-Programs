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

static const char *timestamp_directory(void)
{
    static char directory[64];
    static const char *const bases[] = {"/run", "/var/run", NULL};
    if (directory[0])
        return directory;
    for (int i = 0; bases[i]; i++) {
        struct stat info;
        if (stat(bases[i], &info) != 0 || !S_ISDIR(info.st_mode))
            continue;
        char step[64];
        snprintf(step, sizeof(step), "%s/sudo", bases[i]);
        if (mkdir(step, 0700) != 0 && errno != EEXIST)
            continue;
        snprintf(step, sizeof(step), "%s/sudo/ts", bases[i]);
        if (mkdir(step, 0700) != 0 && errno != EEXIST)
            continue;
        snprintf(directory, sizeof(directory), "%s", step);
        return directory;
    }
    return NULL;
}

static void terminal_id(char *out, size_t capacity)
{
    const char *name = ttyname(0);
    if (!name)
        name = ttyname(1);
    if (!name)
        name = ttyname(2);
    if (!name) {
        snprintf(out, capacity, "ppid%d", (int)getppid());
        return;
    }
    size_t n = 0;
    for (; *name && n + 1 < capacity; name++)
        out[n++] = *name == '/' ? '_' : *name;
    out[n] = 0;
}

static bool timestamp_path(const char *user, char *out, size_t capacity)
{
    const char *directory = timestamp_directory();
    if (!directory)
        return false;
    char tty[96];
    terminal_id(tty, sizeof(tty));
    snprintf(out, capacity, "%s/%s.%s", directory, user, tty);
    return true;
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
    char path[200];
    if (timeout_minutes <= 0 || !timestamp_path(user, path, sizeof(path)))
        return false;
    struct stat info;
    if (stat(path, &info) != 0 || info.st_uid != 0 || (info.st_mode & 077))
        return false;
    int fd = open(path, O_RDONLY);
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
    char path[200];
    if (!timestamp_path(user, path, sizeof(path)))
        return;
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return;
    char text[64];
    int length = snprintf(text, sizeof(text), TS_MAGIC "\n%ld\n", monotonic_seconds());
    if (write(fd, text, (size_t)length) < 0) {
        close(fd);
        return;
    }
    close(fd);
    chmod(path, 0600);
}

static void timestamp_remove(const char *user)
{
    char path[200];
    if (timestamp_path(user, path, sizeof(path)))
        unlink(path);
}

static void timestamp_remove_all(const char *user)
{
    const char *directory = timestamp_directory();
    if (!directory)
        return;
    DIR *dir = opendir(directory);
    if (!dir)
        return;
    size_t length = strlen(user);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, user, length) == 0 && entry->d_name[length] == '.') {
            char path[300];
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
            unlink(path);
        }
    }
    closedir(dir);
}

static bool shadow_hash(const char *user, char *out, size_t capacity)
{
    int fd = open("/etc/shadow", O_RDONLY);
    if (fd < 0)
        return false;
    static char buffer[16384];
    size_t length = 0;
    for (;;) {
        ssize_t got = read(fd, buffer + length, sizeof(buffer) - 1 - length);
        if (got <= 0)
            break;
        length += (size_t)got;
        if (length + 1 >= sizeof(buffer))
            break;
    }
    close(fd);
    buffer[length] = 0;
    size_t name_length = strlen(user);
    for (char *line = buffer; *line;) {
        char *next = strchr(line, '\n');
        if (strncmp(line, user, name_length) == 0 && line[name_length] == ':') {
            const char *hash = line + name_length + 1;
            size_t n = 0;
            while (hash[n] && hash[n] != ':' && hash[n] != '\n') {
                if (n + 1 >= capacity)
                    return false;
                out[n] = hash[n];
                n++;
            }
            out[n] = 0;
            return n > 0;
        }
        if (!next)
            break;
        line = next + 1;
    }
    return false;
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
    char hash[PURECRYPT_HASH_MAX];
    if (!shadow_hash(user, hash, sizeof(hash)))
        return false;
    if (hash[0] != '$' || hash[1] != '6' || hash[2] != '$')
        return false;
    return purecrypt_verify(password, hash) == 1;
}

static int authenticate(const char *auth_user, int tries)
{
    for (int attempt = 0; attempt < tries; attempt++) {
        char prompt[128];
        char password[256];
        snprintf(prompt, sizeof(prompt), "[sudo] password for %s: ", auth_user);
        if (read_password(prompt, password, sizeof(password)) != 0) {
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
    for (int i = 0; i < *count; i++) {
        if (strncmp((*env)[i], name, name_length) == 0 && (*env)[i][name_length] == '=') {
            free((*env)[i]);
            goto set;
        }
    }
    *env = realloc(*env, sizeof(char *) * (size_t)(*count + 2));
    if (!*env) {
        fputs("sudo: out of memory\n", stderr);
        exit(1);
    }
    (*env)[(*count)++] = NULL;
set:;
    int index = 0;
    for (; index < *count; index++)
        if (!(*env)[index] ||
            (strncmp((*env)[index], name, name_length) == 0 && (*env)[index][name_length] == '='))
            break;
    char *entry = malloc(name_length + strlen(value) + 2);
    if (!entry) {
        fputs("sudo: out of memory\n", stderr);
        exit(1);
    }
    sprintf(entry, "%s=%s", name, value);
    (*env)[index] = entry;
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
    env = malloc(sizeof(char *));
    if (!env)
        exit(1);
    env[0] = NULL;
    for (char **e = environ; e && *e; e++) {
        char *equals = strchr(*e, '=');
        if (!equals)
            continue;
        char name[256];
        size_t length = (size_t)(equals - *e);
        if (length == 0 || length >= sizeof(name))
            continue;
        memcpy(name, *e, length);
        name[length] = 0;
        if (pattern_in(always_delete, name))
            continue;
        if (settings->env_reset && !preserve && !keep_by_settings(settings, name))
            continue;
        env_put(&env, &count, name, equals + 1);
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

static bool resolve_command(const char *name, const char *search, char *out, size_t capacity)
{
    if (strchr(name, '/')) {
        char *real = realpath(name, NULL);
        if (real && executable_file(real)) {
            snprintf(out, capacity, "%s", real);
            free(real);
            return true;
        }
        free(real);
        snprintf(out, capacity, "%s", name);
        return false;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s", search && *search ? search : DEFAULT_PATH);
    char *cursor = path;
    while (cursor) {
        char *end = strchr(cursor, ':');
        if (end)
            *end = 0;
        if (*cursor == '/') {
            char candidate[1100];
            snprintf(candidate, sizeof(candidate), "%s/%s", cursor, name);
            if (executable_file(candidate)) {
                snprintf(out, capacity, "%s", candidate);
                return true;
            }
        }
        cursor = end ? end + 1 : NULL;
    }
    snprintf(out, capacity, "%s", name);
    return false;
}

static void join_arguments(char **argv, int count, char *out, size_t capacity)
{
    out[0] = 0;
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        int n = snprintf(out + used, capacity - used, "%s%s", i ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= capacity - used)
            break;
        used += (size_t)n;
    }
}

static void log_event(const char *invoker, const char *message, const char *runas,
                      const char *command)
{
    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd)))
        snprintf(cwd, sizeof(cwd), "?");
    char tty[96];
    terminal_id(tty, sizeof(tty));
    syslog(LOG_NOTICE, "%s : %sTTY=%s ; PWD=%s ; USER=%s ; COMMAND=%s", invoker, message, tty,
           cwd, runas, command);
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

int main(int argc, char **argv)
{
    struct options opt;
    parse_options(argc, argv, &opt);
    openlog("sudo", 0, LOG_AUTH);
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
    static struct passwd invoker;
    static char invoker_name[64], invoker_dir[128], invoker_shell[128];
    snprintf(invoker_name, sizeof(invoker_name), "%s", found->pw_name);
    snprintf(invoker_dir, sizeof(invoker_dir), "%s", found->pw_dir ? found->pw_dir : "/");
    snprintf(invoker_shell, sizeof(invoker_shell), "%s", found->pw_shell ? found->pw_shell : "/bin/sh");
    invoker = *found;
    invoker.pw_name = invoker_name;
    invoker.pw_dir = invoker_dir;
    invoker.pw_shell = invoker_shell;

    if (opt.remove_all) {
        timestamp_remove_all(invoker.pw_name);
        if (opt.first_command >= argc && !opt.shell && !opt.login && !opt.list && !opt.validate)
            return 0;
    }
    if (opt.invalidate) {
        timestamp_remove(invoker.pw_name);
        if (opt.first_command >= argc && !opt.shell && !opt.login && !opt.list && !opt.validate)
            return 0;
    }
    bool has_command = opt.first_command < argc;
    if (!has_command && !opt.shell && !opt.login && !opt.list && !opt.validate) {
        usage(stderr);
        return 1;
    }
    if ((opt.shell || opt.login) && (opt.shell && opt.login)) {
        fputs("sudo: you may not specify both the '-i' and '-s' options\n", stderr);
        return 1;
    }

    static struct passwd target;
    static char target_name[64], target_dir[128], target_shell[128];
    struct passwd *target_found = NULL;
    if (opt.user) {
        target_found = opt.user[0] == '#' && isdigit((unsigned char)opt.user[1])
                           ? getpwuid((uid_t)atol(opt.user + 1))
                           : getpwnam(opt.user);
        if (!target_found && opt.user[0] == '#' && isdigit((unsigned char)opt.user[1])) {
            static struct passwd synthetic;
            synthetic.pw_name = (char *)opt.user;
            synthetic.pw_uid = (uid_t)atol(opt.user + 1);
            synthetic.pw_gid = synthetic.pw_uid;
            synthetic.pw_dir = (char *)"/";
            synthetic.pw_shell = (char *)"/bin/sh";
            target_found = &synthetic;
        }
    } else {
        target_found = getpwuid(0);
    }
    if (!target_found) {
        fprintf(stderr, "sudo: unknown user: %s\n", opt.user ? opt.user : "root");
        return 1;
    }
    snprintf(target_name, sizeof(target_name), "%s", target_found->pw_name);
    snprintf(target_dir, sizeof(target_dir), "%s", target_found->pw_dir ? target_found->pw_dir : "/");
    snprintf(target_shell, sizeof(target_shell), "%s",
             target_found->pw_shell && *target_found->pw_shell ? target_found->pw_shell : "/bin/sh");
    target = *target_found;
    target.pw_name = target_name;
    target.pw_dir = target_dir;
    target.pw_shell = target_shell;

    gid_t target_gid = target.pw_gid;
    static char group_name[64];
    const char *request_group = NULL;
    if (opt.group) {
        struct group *group = opt.group[0] == '#' && isdigit((unsigned char)opt.group[1])
                                  ? getgrgid((gid_t)atol(opt.group + 1))
                                  : getgrnam(opt.group);
        if (group) {
            snprintf(group_name, sizeof(group_name), "%s", group->gr_name);
            target_gid = group->gr_gid;
        } else if (opt.group[0] == '#' && isdigit((unsigned char)opt.group[1])) {
            snprintf(group_name, sizeof(group_name), "%s", opt.group);
            target_gid = (gid_t)atol(opt.group + 1);
        } else {
            fprintf(stderr, "sudo: unknown group: %s\n", opt.group);
            return 1;
        }
        request_group = group_name;
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

    struct sudoers_subject subject = {invoker.pw_name, invoker.pw_uid, invoker.pw_gid};
    struct sudoers_request request = {target.pw_name, target.pw_uid, request_group,
                                      target_gid, "", ""};
    struct sudoers_settings settings;
    sudoers_settings(&db, &subject, &request, &settings);

    char command_path[1024] = "";
    char command_line[2048] = "";
    char arguments[1536] = "";
    char shell_arguments[1600] = "";
    char **exec_argv = NULL;
    const char *shell_path = NULL;
    bool found_command = true;

    if (opt.shell || opt.login) {
        const char *shell_env = getenv("SHELL");
        shell_path = opt.login ? target.pw_shell
                               : (shell_env && shell_env[0] == '/' ? shell_env : target.pw_shell);
        snprintf(command_path, sizeof(command_path), "%s", shell_path);
        exec_argv = calloc(4, sizeof(char *));
        if (!exec_argv)
            return 1;
        const char *base = strrchr(shell_path, '/');
        exec_argv[0] = (char *)(base ? base + 1 : shell_path);
        if (has_command) {
            join_arguments(argv + opt.first_command, argc - opt.first_command, arguments,
                           sizeof(arguments));
            exec_argv[1] = (char *)"-c";
            exec_argv[2] = arguments;
            snprintf(shell_arguments, sizeof(shell_arguments), "-c %s", arguments);
            snprintf(command_line, sizeof(command_line), "%s %s", shell_path, shell_arguments);
        } else {
            snprintf(command_line, sizeof(command_line), "%s", shell_path);
        }
    } else if (has_command) {
        found_command = resolve_command(argv[opt.first_command], settings.secure_path ?
                                        settings.secure_path : getenv("PATH"), command_path,
                                        sizeof(command_path));
        join_arguments(argv + opt.first_command + 1, argc - opt.first_command - 1, arguments,
                       sizeof(arguments));
        if (arguments[0])
            snprintf(command_line, sizeof(command_line), "%s %s", command_path, arguments);
        else
            snprintf(command_line, sizeof(command_line), "%s", command_path);
        exec_argv = calloc((size_t)(argc - opt.first_command + 1), sizeof(char *));
        if (!exec_argv)
            return 1;
        for (int i = opt.first_command; i < argc; i++)
            exec_argv[i - opt.first_command] = argv[i];
    }

    request.command = command_path;
    request.arguments = (opt.shell || opt.login) ? shell_arguments : arguments;
    sudoers_settings(&db, &subject, &request, &settings);

    struct sudoers_verdict verdict;
    sudoers_check(&db, &subject, &request, &verdict);
    bool needs_command_check = has_command || opt.shell || opt.login;

    if ((opt.list || opt.validate) && !has_command && !opt.shell && !opt.login)
        needs_command_check = false;

    char host[256];
    snprintf(host, sizeof(host), "%s", db.hostname[0] ? db.hostname : "localhost");

    if (needs_command_check && !verdict.allowed) {
        if (!verdict.listed) {
            fprintf(stderr, "%s is not in the sudoers file.\n", invoker.pw_name);
            log_event(invoker.pw_name, "user NOT in sudoers ; ", target.pw_name, command_line);
        } else {
            fprintf(stderr, "Sorry, user %s is not allowed to execute '%s' as %s on %s.\n",
                    invoker.pw_name, command_line, target.pw_name, host);
            log_event(invoker.pw_name, "command not allowed ; ", target.pw_name, command_line);
        }
        return 1;
    }
    if (!needs_command_check) {
        if (!verdict.listed) {
            if (opt.list)
                printf("User %s is not allowed to run sudo on %s.\n", invoker.pw_name, host);
            else
                fprintf(stderr, "%s is not in the sudoers file.\n", invoker.pw_name);
            log_event(invoker.pw_name, "user NOT in sudoers ; ", target.pw_name, "");
            return 1;
        }
    }

    if (opt.preserve_env && !verdict.setenv && needs_command_check) {
        fputs("sudo: sorry, you are not allowed to preserve the environment\n", stderr);
        return 1;
    }

    const char *auth_user = settings.rootpw ? "root" : settings.targetpw ? target.pw_name
                                                                          : invoker.pw_name;
    bool needs_password = real_uid != 0 || settings.rootpw || settings.targetpw;
    if (needs_command_check && verdict.passwd == 2)
        needs_password = false;
    if (!needs_command_check && sudoers_user_has_nopasswd(&db, &subject))
        needs_password = false;
    if (needs_password && !opt.invalidate && timestamp_valid(invoker.pw_name, settings.timestamp_timeout))
        needs_password = false;
    if (needs_password) {
        if (opt.non_interactive) {
            fputs("sudo: a password is required\n", stderr);
            return 1;
        }
        if (authenticate(auth_user, settings.passwd_tries) != 0) {
            log_event(invoker.pw_name, "incorrect password attempts ; ", target.pw_name,
                      command_line);
            return 1;
        }
        timestamp_update(invoker.pw_name);
    } else if (real_uid != 0 && settings.timestamp_timeout > 0 &&
               timestamp_valid(invoker.pw_name, settings.timestamp_timeout)) {
        timestamp_update(invoker.pw_name);
    }

    if (opt.validate && !has_command && !opt.shell && !opt.login)
        return 0;

    if (opt.list) {
        printf("Matching Defaults entries for %s on %s:\n    ", invoker.pw_name, host);
        print_defaults(&settings);
        printf("\nUser %s may run the following commands on %s:\n", invoker.pw_name, host);
        if (has_command) {
            printf("%s\n", command_line);
        } else {
            sudoers_list_rules(&db, &subject, stdout);
        }
        return 0;
    }

    if (!found_command && !(opt.shell || opt.login)) {
        fprintf(stderr, "sudo: %s: command not found\n", argv[opt.first_command]);
        return 1;
    }

    const char *run_shell = shell_path ? shell_path : target.pw_shell;
    char **env = build_environment(&settings, opt.preserve_env, &target, &invoker, command_line,
                                   run_shell);

    log_event(invoker.pw_name, "", target.pw_name, command_line);

    if (setresgid(target_gid, target_gid, target_gid) != 0 ||
        setresuid(target.pw_uid, target.pw_uid, target.pw_uid) != 0) {
        fputs("sudo: unable to change to runas uid or gid\n", stderr);
        return 1;
    }
    if (opt.login && chdir(target.pw_dir) != 0) {
        fprintf(stderr, "sudo: unable to change directory to %s: %s\n", target.pw_dir,
                strerror(errno));
        return 1;
    }
    execve(command_path, exec_argv, env);
    fprintf(stderr, "sudo: %s: %s\n", command_path, strerror(errno));
    return errno == ENOENT ? 127 : 126;
}
