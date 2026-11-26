#include "../sudo/sudoers.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void usage(FILE *out)
{
    fputs("usage: visudo [-chqsV] [[-f] sudoers_file]\n", out);
}

static bool read_all(const char *path, char **data, size_t *size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    size_t capacity = 8192;
    size_t length = 0;
    char *buffer = malloc(capacity);
    if (!buffer) {
        close(fd);
        return false;
    }
    for (;;) {
        if (length == capacity) {
            capacity *= 2;
            char *grown = realloc(buffer, capacity);
            if (!grown) {
                free(buffer);
                close(fd);
                return false;
            }
            buffer = grown;
        }
        ssize_t got = read(fd, buffer + length, capacity - length);
        if (got < 0) {
            free(buffer);
            close(fd);
            return false;
        }
        if (got == 0)
            break;
        length += (size_t)got;
    }
    close(fd);
    *data = buffer;
    *size = length;
    return true;
}

static bool write_all(int fd, const char *data, size_t size)
{
    while (size) {
        ssize_t put = write(fd, data, size);
        if (put <= 0)
            return false;
        data += put;
        size -= (size_t)put;
    }
    return true;
}

static int run_editor(const char *path)
{
    const char *editor = getenv("SUDO_EDITOR");
    if (!editor || !*editor)
        editor = getenv("VISUAL");
    if (!editor || !*editor)
        editor = getenv("EDITOR");
    if (!editor || !*editor)
        editor = "vim";
    char *copy = sudoers_xdup(editor);
    char **argv = NULL;
    int count = 0;
    for (char *c = copy; *c;) {
        while (*c == ' ' || *c == '\t')
            c++;
        if (!*c)
            break;
        argv = sudoers_xrealloc(argv, sizeof(char *) * (size_t)(count + 1));
        argv[count++] = c;
        while (*c && *c != ' ' && *c != '\t')
            c++;
        if (*c)
            *c++ = 0;
    }
    argv = sudoers_xrealloc(argv, sizeof(char *) * (size_t)(count + 2));
    argv[count++] = (char *)path;
    argv[count] = NULL;
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "visudo: fork: %s\n", strerror(errno));
        return -1;
    }
    if (child == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "visudo: unable to run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    int waited = waitpid(child, &status, 0);
    free(argv);
    free(copy);
    return waited < 0 ? -1 : status;
}

static int check_only(const char *path, bool strict, bool quiet)
{
    struct sudoers db;
    sudoers_init(&db, true, strict);
    if (sudoers_load(&db, path) != 0) {
        fprintf(stderr, "visudo: %s\n", db.error);
        return 1;
    }
    for (int i = 0; i < db.warning_count; i++)
        fprintf(stderr, "visudo: warning: %s\n", db.warning[i]);
    struct stat info;
    int status = 0;
    if (stat(path, &info) == 0 && (info.st_mode & 0777) != 0440) {
        fprintf(stderr, "visudo: %s: bad permissions, should be mode 0440\n", path);
        status = 1;
    }
    if (!quiet)
        for (int i = 0; i < db.file_count; i++)
            printf("%s: parsed OK\n", db.files[i]);
    return status;
}

int main(int argc, char **argv)
{
    bool check = false;
    bool strict = false;
    bool quiet = false;
    const char *target = SUDOERS_PATH;
    int i = 1;
    for (; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] != '-' || !arg[1])
            break;
        if (strcmp(arg, "--") == 0) {
            i++;
            break;
        }
        for (const char *c = arg + 1; *c; c++) {
            if (*c == 'c')
                check = true;
            else if (*c == 's')
                strict = true;
            else if (*c == 'q')
                quiet = true;
            else if (*c == 'f') {
                if (i + 1 >= argc) {
                    usage(stderr);
                    return 1;
                }
                target = argv[++i];
                break;
            } else if (*c == 'h') {
                usage(stdout);
                return 0;
            } else if (*c == 'V') {
                puts("visudo version 1.0");
                return 0;
            } else {
                usage(stderr);
                return 1;
            }
        }
    }
    if (i < argc)
        target = argv[i];
    if (geteuid() != 0) {
        fputs("visudo: you must be root to run this program\n", stderr);
        return 1;
    }
    if (check)
        return check_only(target, strict, quiet);

    char *temp = sudoers_format("%s.tmp", target);
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        if (errno == EEXIST)
            fprintf(stderr, "visudo: %s busy, try again later\n", temp);
        else
            fprintf(stderr, "visudo: %s: %s\n", temp, strerror(errno));
        return 1;
    }
    char *original = NULL;
    size_t original_size = 0;
    if (!read_all(target, &original, &original_size)) {
        original = sudoers_xalloc(1);
        original_size = 0;
    }
    if (!write_all(fd, original, original_size)) {
        fprintf(stderr, "visudo: unable to write %s\n", temp);
        close(fd);
        unlink(temp);
        return 1;
    }
    close(fd);
    for (;;) {
        int status = run_editor(temp);
        if (status != 0) {
            fputs("visudo: editor did not exit cleanly\n", stderr);
            unlink(temp);
            return 1;
        }
        char *edited = NULL;
        size_t edited_size = 0;
        if (!read_all(temp, &edited, &edited_size)) {
            fprintf(stderr, "visudo: unable to read %s\n", temp);
            unlink(temp);
            return 1;
        }
        if (edited_size == original_size && memcmp(edited, original, original_size) == 0) {
            fprintf(stderr, "visudo: %s unchanged\n", target);
            unlink(temp);
            return 0;
        }
        struct sudoers db;
        sudoers_init(&db, false, strict);
        bool good = sudoers_load(&db, temp) == 0;
        for (int w = 0; w < db.warning_count; w++)
            fprintf(stderr, "visudo: warning: %s\n", db.warning[w]);
        if (good) {
            chown(temp, 0, 0);
            chmod(temp, 0440);
            if (rename(temp, target) != 0) {
                fprintf(stderr, "visudo: unable to install %s: %s\n", target, strerror(errno));
                unlink(temp);
                return 1;
            }
            return 0;
        }
        fprintf(stderr, "%s: syntax error\n", db.error);
        for (;;) {
            fputs("What now? ", stderr);
            fflush(stderr);
            char choice = 0;
            char c;
            ssize_t got = read(0, &c, 1);
            if (got > 0) {
                choice = c;
                while (c != '\n' && read(0, &c, 1) > 0)
                    ;
            }
            if (got <= 0 || choice == 'x') {
                unlink(temp);
                fputc('\n', stderr);
                return 1;
            }
            if (choice == 'Q') {
                chown(temp, 0, 0);
                chmod(temp, 0440);
                if (rename(temp, target) != 0) {
                    fprintf(stderr, "visudo: unable to install %s: %s\n", target, strerror(errno));
                    unlink(temp);
                    return 1;
                }
                return 0;
            }
            if (choice == 'e')
                break;
            fputs("Options are:\n  (e)dit sudoers file again\n"
                  "  e(x)it without saving changes to sudoers file\n"
                  "  (Q)uit and save changes to sudoers file (DANGEROUS!)\n\n",
                  stderr);
        }
    }
}
