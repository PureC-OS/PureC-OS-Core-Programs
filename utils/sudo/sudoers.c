#include "sudoers.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MATCH_NONE 2
#define MATCH_NO 0
#define MATCH_YES 1
#define ALIAS_DEPTH 16
#define INCLUDE_DEPTH 64

enum match_kind {
    KIND_USER,
    KIND_RUNAS_USER,
    KIND_RUNAS_GROUP,
    KIND_HOST,
    KIND_CMND
};

struct match_context {
    const struct sudoers_subject *who;
    const struct sudoers_request *request;
};

static const char *const ignored_defaults[] = {
    "lecture", "lecture_file", "insults", "mail_always", "mail_badpass",
    "mail_no_user", "mail_no_host", "mail_no_perms", "mailto", "mailfrom",
    "mailsub", "requiretty", "use_pty", "visiblepw", "always_set_home",
    "set_home", "env_editor", "umask", "logfile", "syslog", "log_input",
    "log_output", "authenticate", "fqdn", "noexec", "path_info", "pwfeedback",
    "tty_tickets", "passprompt", "badpass_message", "env_check", "env_delete",
    "umask_override", "ignore_dot", "ignore_local_sudoers", "shell_noargs",
    "stay_setuid", "setenv", "closefrom", "passwd_timeout", "timestampdir",
    "timestampowner", "runas_default", "iolog_dir", "log_host", "log_year",
    "sudoedit_checkdir", "sudoedit_follow", "rootpw_unused", "exempt_group",
    "verifypw", "listpw", "match_group_by_gid", "ignore_audit_errors",
    "mail_all_cmnds", "preserve_groups", "admin_flag", "secure_path_unused",
    NULL};

static void out_of_memory(void)
{
    fputs("sudo: out of memory\n", stderr);
    exit(1);
}

void *sudoers_xalloc(size_t size)
{
    void *pointer = calloc(1, size ? size : 1);
    if (!pointer)
        out_of_memory();
    return pointer;
}

void *sudoers_xrealloc(void *pointer, size_t size)
{
    void *grown = realloc(pointer, size ? size : 1);
    if (!grown)
        out_of_memory();
    return grown;
}

char *sudoers_xdup(const char *text)
{
    size_t length = strlen(text);
    char *copy = sudoers_xalloc(length + 1);
    memcpy(copy, text, length + 1);
    return copy;
}

char *sudoers_vformat(const char *format, va_list args)
{
    va_list again;
    va_copy(again, args);
    int length = vsnprintf(NULL, 0, format, args);
    if (length < 0)
        out_of_memory();
    char *text = sudoers_xalloc((size_t)length + 1);
    vsnprintf(text, (size_t)length + 1, format, again);
    va_end(again);
    return text;
}

char *sudoers_format(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *text = sudoers_vformat(format, args);
    va_end(args);
    return text;
}

#define xalloc sudoers_xalloc
#define xdup sudoers_xdup

static void set_error(struct sudoers *db, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *text = sudoers_vformat(format, args);
    va_end(args);
    free(db->error);
    db->error = text;
}

static void list_add(struct sudoers_list *list, const char *text)
{
    list->item = sudoers_xrealloc(list->item, sizeof(char *) * (size_t)(list->count + 1));
    list->item[list->count++] = xdup(text);
}

static void list_remove(struct sudoers_list *list, const char *text)
{
    int out = 0;
    for (int i = 0; i < list->count; i++)
        if (strcmp(list->item[i], text) != 0)
            list->item[out++] = list->item[i];
    list->count = out;
}

static char *trim(char *text)
{
    while (*text && isspace((unsigned char)*text))
        text++;
    size_t length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1]))
        text[--length] = 0;
    return text;
}

static void unescape(char *text)
{
    char *out = text;
    for (char *in = text; *in; in++) {
        if (*in == '\\' && in[1]) {
            in++;
        }
        *out++ = *in;
    }
    *out = 0;
}

static bool is_alias_name(const char *text)
{
    if (!isupper((unsigned char)text[0]))
        return false;
    for (const char *c = text; *c; c++)
        if (!isupper((unsigned char)*c) && !isdigit((unsigned char)*c) && *c != '_')
            return false;
    return true;
}

static char *split_next(char **cursor, char separator, bool quotes)
{
    char *start = *cursor;
    if (!start)
        return NULL;
    bool quoted = false;
    char *c = start;
    for (; *c; c++) {
        if (*c == '\\' && c[1]) {
            c++;
            continue;
        }
        if (quotes && *c == '"')
            quoted = !quoted;
        if (!quoted && *c == separator)
            break;
    }
    if (*c) {
        *c = 0;
        *cursor = c + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

static void split_into(char *text, char separator, struct sudoers_list *out)
{
    char *cursor = text;
    char *piece;
    while ((piece = split_next(&cursor, separator, false)) != NULL) {
        piece = trim(piece);
        if (!*piece)
            continue;
        char *copy = xdup(piece);
        unescape(copy);
        list_add(out, copy);
    }
}

static int fail(struct sudoers *db, const char *file, int line, const char *message)
{
    set_error(db, "%s:%d: %s", file, line, message);
    return -1;
}

static void warn(struct sudoers *db, const char *file, int line, const char *message,
                 const char *detail)
{
    db->warning = sudoers_xrealloc(db->warning, sizeof(char *) * (size_t)(db->warning_count + 1));
    db->warning[db->warning_count++] = sudoers_format("%s:%d: %s \"%s\"", file, line, message, detail);
}

void sudoers_init(struct sudoers *db, bool check_permissions, bool strict)
{
    memset(db, 0, sizeof(*db));
    db->check_permissions = check_permissions;
    db->strict = strict;
    size_t capacity = 64;
    for (;;) {
        char *name = sudoers_xalloc(capacity);
        if (gethostname(name, capacity) != 0) {
            free(name);
            db->hostname = xdup("");
            return;
        }
        if (strlen(name) + 1 < capacity) {
            db->hostname = name;
            return;
        }
        free(name);
        capacity *= 2;
    }
}

static const struct sudoers_alias *find_alias(const struct sudoers *db,
                                              enum sudoers_alias_kind kind,
                                              const char *name)
{
    for (int i = 0; i < db->alias_count; i++)
        if (db->alias[i].kind == kind && strcmp(db->alias[i].name, name) == 0)
            return &db->alias[i];
    return NULL;
}

static bool user_in_group(const char *user, gid_t primary, const struct group *group)
{
    if (!group)
        return false;
    if (group->gr_gid == primary)
        return true;
    for (char **member = group->gr_mem; member && *member; member++)
        if (strcmp(*member, user) == 0)
            return true;
    return false;
}

static int match_list(const struct sudoers *db, const struct sudoers_list *list,
                      enum match_kind kind, const struct match_context *context,
                      int depth);

static int match_command_text(char *text, const struct sudoers_request *request);

static int match_command(const char *item, const struct sudoers_request *request)
{
    char *text = xdup(item);
    int result = match_command_text(text, request);
    free(text);
    return result;
}

static int match_command_text(char *text, const struct sudoers_request *request)
{
    size_t length = strlen(text);
    if (length > 1 && text[length - 1] == '/') {
        text[length - 1] = 0;
        const char *slash = strrchr(request->command, '/');
        if (!slash)
            return MATCH_NONE;
        size_t directory = (size_t)(slash - request->command);
        if (directory == 0)
            directory = 1;
        return strlen(text) == directory && strncmp(text, request->command, directory) == 0
                   ? MATCH_YES
                   : MATCH_NONE;
    }
    char *arguments = NULL;
    for (char *c = text; *c; c++) {
        if (isspace((unsigned char)*c)) {
            *c = 0;
            arguments = trim(c + 1);
            break;
        }
    }
    if (strcmp(text, "sudoedit") == 0)
        return MATCH_NONE;
    bool path_ok;
    if (strpbrk(text, "*?["))
        path_ok = fnmatch(text, request->command, FNM_PATHNAME) == 0;
    else
        path_ok = strcmp(text, request->command) == 0;
    if (!path_ok)
        return MATCH_NONE;
    if (!arguments || !*arguments)
        return MATCH_YES;
    const char *given = request->arguments ? request->arguments : "";
    if (strcmp(arguments, "\"\"") == 0)
        return *given ? MATCH_NONE : MATCH_YES;
    return fnmatch(arguments, given, 0) == 0 ? MATCH_YES : MATCH_NONE;
}

static int match_user_atom(const char *item, const char *name, uid_t uid, gid_t gid)
{
    if (item[0] == '%') {
        struct group *group = item[1] == '#' ? getgrgid((gid_t)atoi(item + 2))
                                             : getgrnam(item + 1);
        return user_in_group(name, gid, group) ? MATCH_YES : MATCH_NONE;
    }
    if (item[0] == '#' && isdigit((unsigned char)item[1]))
        return (uid_t)atol(item + 1) == uid ? MATCH_YES : MATCH_NONE;
    return strcmp(item, name) == 0 ? MATCH_YES : MATCH_NONE;
}

static int match_item(const struct sudoers *db, const char *item, enum match_kind kind,
                      const struct match_context *context, int depth)
{
    if (strcmp(item, "ALL") == 0)
        return MATCH_YES;
    if (is_alias_name(item) && depth < ALIAS_DEPTH) {
        enum sudoers_alias_kind akind =
            kind == KIND_USER ? ALIAS_USER
            : (kind == KIND_RUNAS_USER || kind == KIND_RUNAS_GROUP) ? ALIAS_RUNAS
            : kind == KIND_HOST ? ALIAS_HOST : ALIAS_CMND;
        const struct sudoers_alias *alias = find_alias(db, akind, item);
        if (alias)
            return match_list(db, &alias->members, kind, context, depth + 1);
    }
    switch (kind) {
    case KIND_USER:
        return match_user_atom(item, context->who->user, context->who->uid,
                               context->who->gid);
    case KIND_RUNAS_USER:
        return match_user_atom(item, context->request->runas_user,
                               context->request->runas_uid, context->request->runas_gid);
    case KIND_RUNAS_GROUP: {
        if (!context->request->runas_group)
            return MATCH_NONE;
        if (item[0] == '#' && isdigit((unsigned char)item[1]))
            return (gid_t)atol(item + 1) == context->request->runas_gid ? MATCH_YES
                                                                         : MATCH_NONE;
        return strcmp(item, context->request->runas_group) == 0 ? MATCH_YES : MATCH_NONE;
    }
    case KIND_HOST:
        if (!db->hostname || !db->hostname[0])
            return MATCH_NONE;
        return fnmatch(item, db->hostname, 0) == 0 ? MATCH_YES : MATCH_NONE;
    case KIND_CMND:
        return match_command(item, context->request);
    }
    return MATCH_NONE;
}

static int match_list_raw(const struct sudoers *db, const struct sudoers_list *list,
                          enum match_kind kind, const struct match_context *context,
                          int depth)
{
    int result = MATCH_NONE;
    for (int i = 0; i < list->count; i++) {
        const char *item = list->item[i];
        bool negated = false;
        while (*item == '!' || isspace((unsigned char)*item)) {
            if (*item == '!')
                negated = !negated;
            item++;
        }
        int r = match_item(db, item, kind, context, depth);
        if (r == MATCH_NONE)
            continue;
        if (negated)
            r = r == MATCH_YES ? MATCH_NO : MATCH_YES;
        result = r;
    }
    return result;
}

static int match_list(const struct sudoers *db, const struct sudoers_list *list,
                      enum match_kind kind, const struct match_context *context,
                      int depth)
{
    return match_list_raw(db, list, kind, context, depth);
}

static bool list_yes(const struct sudoers *db, const struct sudoers_list *list,
                     enum match_kind kind, const struct match_context *context)
{
    return match_list_raw(db, list, kind, context, 0) == MATCH_YES;
}

static bool rule_runas_matches(const struct sudoers *db, const struct sudoers_rule *rule,
                               const struct match_context *context)
{
    const struct sudoers_request *request = context->request;
    if (!rule->has_runas) {
        return request->runas_uid == 0 && !request->runas_group;
    }
    if (rule->runas_users.count == 0) {
        if (strcmp(request->runas_user, context->who->user) != 0)
            return false;
    } else if (!list_yes(db, &rule->runas_users, KIND_RUNAS_USER, context)) {
        return false;
    }
    if (request->runas_group) {
        if (rule->runas_groups.count == 0)
            return false;
        return list_yes(db, &rule->runas_groups, KIND_RUNAS_GROUP, context);
    }
    return true;
}

void sudoers_check(const struct sudoers *db, const struct sudoers_subject *who,
                   const struct sudoers_request *request, struct sudoers_verdict *out)
{
    struct match_context context = {who, request};
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < db->rule_count; i++) {
        const struct sudoers_rule *rule = &db->rule[i];
        if (!list_yes(db, &rule->who, KIND_USER, &context) ||
            !list_yes(db, &rule->hosts, KIND_HOST, &context))
            continue;
        out->listed = true;
        if (!rule_runas_matches(db, rule, &context))
            continue;
        char *single[1] = {rule->command};
        struct sudoers_list one = {single, 1};
        int r = match_list_raw(db, &one, KIND_CMND, &context, 0);
        if (r == MATCH_NONE)
            continue;
        out->allowed = r == MATCH_YES;
        out->passwd = rule->passwd;
        out->setenv = rule->setenv;
    }
}

bool sudoers_user_has_nopasswd(const struct sudoers *db, const struct sudoers_subject *who)
{
    struct sudoers_request request = {"root", 0, NULL, 0, "", ""};
    struct match_context context = {who, &request};
    for (int i = 0; i < db->rule_count; i++) {
        const struct sudoers_rule *rule = &db->rule[i];
        if (list_yes(db, &rule->who, KIND_USER, &context) &&
            list_yes(db, &rule->hosts, KIND_HOST, &context) && rule->passwd == 2)
            return true;
    }
    return false;
}

static void print_list(const struct sudoers_list *list, FILE *out)
{
    for (int i = 0; i < list->count; i++)
        fprintf(out, "%s%s", i ? ", " : "", list->item[i]);
}

void sudoers_list_rules(const struct sudoers *db, const struct sudoers_subject *who, FILE *out)
{
    struct sudoers_request request = {"root", 0, NULL, 0, "", ""};
    struct match_context context = {who, &request};
    for (int i = 0; i < db->rule_count; i++) {
        const struct sudoers_rule *rule = &db->rule[i];
        if (!list_yes(db, &rule->who, KIND_USER, &context) ||
            !list_yes(db, &rule->hosts, KIND_HOST, &context))
            continue;
        fputs("    (", out);
        if (!rule->has_runas) {
            fputs("root", out);
        } else {
            print_list(&rule->runas_users, out);
            if (rule->runas_groups.count) {
                fputs(" : ", out);
                print_list(&rule->runas_groups, out);
            }
        }
        fputs(") ", out);
        if (rule->passwd == 2)
            fputs("NOPASSWD: ", out);
        else if (rule->passwd == 1)
            fputs("PASSWD: ", out);
        if (rule->setenv)
            fputs("SETENV: ", out);
        fprintf(out, "%s\n", rule->command);
    }
}

bool sudoers_name_in_list(const struct sudoers_list *list, const char *name)
{
    for (int i = 0; i < list->count; i++)
        if (strcmp(list->item[i], name) == 0)
            return true;
    return false;
}

static int parse_bool(const char *text)
{
    return text && *text ? atoi(text) : 0;
}

static void apply_setting(struct sudoers_settings *out, const struct sudoers_setting *s)
{
    const char *name = s->name;
    if (strcmp(name, "secure_path") == 0) {
        out->secure_path = s->negate || !s->value ? NULL : s->value;
    } else if (strcmp(name, "env_keep") == 0) {
        if (s->negate) {
            out->env_keep.count = 0;
        } else if (s->value) {
            char *copy = xdup(s->value);
            for (char *c = copy; *c; c++)
                if (*c == ',')
                    *c = ' ';
            char *cursor = copy;
            char *word;
            if (s->op == '=')
                out->env_keep.count = 0;
            while ((word = split_next(&cursor, ' ', false)) != NULL) {
                word = trim(word);
                if (!*word)
                    continue;
                if (s->op == '-')
                    list_remove(&out->env_keep, word);
                else
                    list_add(&out->env_keep, word);
            }
        }
    } else if (strcmp(name, "env_reset") == 0) {
        out->env_reset = !s->negate;
    } else if (strcmp(name, "timestamp_timeout") == 0) {
        out->timestamp_timeout = s->negate ? 0 : parse_bool(s->value);
    } else if (strcmp(name, "passwd_tries") == 0) {
        out->passwd_tries = s->negate ? 0 : parse_bool(s->value);
        if (out->passwd_tries < 1)
            out->passwd_tries = 1;
    } else if (strcmp(name, "targetpw") == 0) {
        out->targetpw = !s->negate;
    } else if (strcmp(name, "rootpw") == 0) {
        out->rootpw = !s->negate;
    }
}

void sudoers_settings(const struct sudoers *db, const struct sudoers_subject *who,
                      const struct sudoers_request *request, struct sudoers_settings *out)
{
    struct match_context context = {who, request};
    memset(out, 0, sizeof(*out));
    out->env_reset = true;
    out->timestamp_timeout = 5;
    out->passwd_tries = 3;
    static const char order[] = "GHURC";
    for (const char *scope = order; *scope; scope++) {
        for (int i = 0; i < db->defaults_count; i++) {
            const struct sudoers_defaults *d = &db->defaults[i];
            if (d->scope != *scope)
                continue;
            bool applies = true;
            if (d->scope == 'U')
                applies = list_yes(db, &d->who, KIND_USER, &context);
            else if (d->scope == 'R')
                applies = list_yes(db, &d->who, KIND_RUNAS_USER, &context);
            else if (d->scope == 'H')
                applies = list_yes(db, &d->who, KIND_HOST, &context);
            else if (d->scope == 'C')
                applies = list_yes(db, &d->who, KIND_CMND, &context);
            if (!applies)
                continue;
            for (int j = 0; j < d->count; j++)
                apply_setting(out, &d->setting[j]);
        }
    }
}

static char *lex_list(char *text, struct sudoers_list *out)
{
    for (;;) {
        while (*text && isspace((unsigned char)*text))
            text++;
        char *start = text;
        while (*text && !isspace((unsigned char)*text) && *text != ',' && *text != '=') {
            if (*text == '\\' && text[1])
                text++;
            text++;
        }
        if (text == start)
            return NULL;
        char *peek = text;
        while (*peek && isspace((unsigned char)*peek))
            peek++;
        char saved = *text;
        *text = 0;
        char *copy = xdup(start);
        *text = saved;
        unescape(copy);
        list_add(out, copy);
        if (*peek == ',') {
            text = peek + 1;
            continue;
        }
        return text;
    }
}

static bool known_ignored(const char *name)
{
    for (int i = 0; ignored_defaults[i]; i++)
        if (strcmp(ignored_defaults[i], name) == 0)
            return true;
    return false;
}

static bool known_default(const char *name)
{
    static const char *const known[] = {"secure_path", "env_keep", "env_reset",
                                        "timestamp_timeout", "passwd_tries",
                                        "targetpw", "rootpw", NULL};
    for (int i = 0; known[i]; i++)
        if (strcmp(known[i], name) == 0)
            return true;
    return false;
}

static int parse_defaults(struct sudoers *db, char *text, const char *file, int line)
{
    char *cursor = text + 8;
    struct sudoers_defaults entry;
    memset(&entry, 0, sizeof(entry));
    entry.scope = 'G';
    if (*cursor == ':' || *cursor == '@' || *cursor == '!' || *cursor == '>') {
        entry.scope = *cursor == ':' ? 'U' : *cursor == '@' ? 'H' : *cursor == '!' ? 'C' : 'R';
        cursor = lex_list(cursor + 1, &entry.who);
        if (!cursor)
            return fail(db, file, line, "syntax error");
    } else if (*cursor && !isspace((unsigned char)*cursor)) {
        return fail(db, file, line, "syntax error");
    }
    char *settings = trim(cursor);
    if (!*settings)
        return fail(db, file, line, "syntax error");
    char *walk = settings;
    char *piece;
    while ((piece = split_next(&walk, ',', true)) != NULL) {
        piece = trim(piece);
        if (!*piece)
            return fail(db, file, line, "syntax error");
        struct sudoers_setting setting;
        memset(&setting, 0, sizeof(setting));
        setting.op = 0;
        char *name = piece;
        while (*name == '!') {
            setting.negate = !setting.negate;
            name++;
        }
        char *end = name;
        while (*end && *end != '=' && *end != '+' && *end != '-' && !isspace((unsigned char)*end))
            end++;
        char *value = NULL;
        char *after = end;
        while (isspace((unsigned char)*after))
            after++;
        if (*after == '=' || ((*after == '+' || *after == '-') && after[1] == '=')) {
            setting.op = *after == '=' ? '=' : *after;
            value = trim(after + (*after == '=' ? 1 : 2));
            if (*value == '"') {
                value++;
                size_t length = strlen(value);
                if (length && value[length - 1] == '"')
                    value[length - 1] = 0;
            }
        } else if (*after) {
            return fail(db, file, line, "syntax error");
        }
        *end = 0;
        if (!*name)
            return fail(db, file, line, "syntax error");
        setting.name = xdup(name);
        setting.value = value ? xdup(value) : NULL;
        if (!known_default(name)) {
            if (!known_ignored(name)) {
                if (db->strict)
                    return fail(db, file, line, "unknown defaults entry");
                warn(db, file, line, "unknown defaults entry", name);
            }
            continue;
        }
        entry.setting = sudoers_xrealloc(entry.setting, sizeof(*entry.setting) * (size_t)(entry.count + 1));
        entry.setting[entry.count++] = setting;
    }
    db->defaults = sudoers_xrealloc(db->defaults, sizeof(*db->defaults) * (size_t)(db->defaults_count + 1));
    db->defaults[db->defaults_count++] = entry;
    return 0;
}

static int parse_alias(struct sudoers *db, char *text, enum sudoers_alias_kind kind,
                       const char *file, int line)
{
    char *cursor = strchr(text, ' ');
    if (!cursor)
        cursor = strchr(text, '\t');
    if (!cursor)
        return fail(db, file, line, "syntax error");
    char *definitions = trim(cursor);
    char *walk = definitions;
    char *piece;
    while ((piece = split_next(&walk, ':', false)) != NULL) {
        piece = trim(piece);
        char *equals = strchr(piece, '=');
        if (!equals)
            return fail(db, file, line, "syntax error");
        *equals = 0;
        char *name = trim(piece);
        if (!is_alias_name(name))
            return fail(db, file, line, "invalid alias name");
        struct sudoers_alias alias;
        memset(&alias, 0, sizeof(alias));
        alias.kind = kind;
        alias.name = xdup(name);
        split_into(equals + 1, ',', &alias.members);
        if (!alias.members.count)
            return fail(db, file, line, "syntax error");
        db->alias = sudoers_xrealloc(db->alias, sizeof(*db->alias) * (size_t)(db->alias_count + 1));
        db->alias[db->alias_count++] = alias;
    }
    return 0;
}

static bool tag_word(const char *text, const char *word)
{
    size_t length = strlen(word);
    return strncmp(text, word, length) == 0 && text[length] == ':';
}

static int add_rule(struct sudoers *db, const struct sudoers_rule *rule)
{
    db->rule = sudoers_xrealloc(db->rule, sizeof(*db->rule) * (size_t)(db->rule_count + 1));
    db->rule[db->rule_count++] = *rule;
    return 0;
}

static int parse_userspec(struct sudoers *db, char *text, const char *file, int line)
{
    char *equals = text;
    while (*equals && *equals != '=') {
        if (*equals == '\\' && equals[1])
            equals++;
        equals++;
    }
    if (*equals != '=')
        return fail(db, file, line, "syntax error");
    *equals = 0;
    struct sudoers_list who;
    struct sudoers_list hosts;
    memset(&who, 0, sizeof(who));
    memset(&hosts, 0, sizeof(hosts));
    char *cursor = lex_list(text, &who);
    if (!cursor)
        return fail(db, file, line, "syntax error");
    char *after_who = cursor;
    while (isspace((unsigned char)*after_who))
        after_who++;
    if (!*after_who)
        return fail(db, file, line, "syntax error");
    cursor = lex_list(after_who, &hosts);
    if (!cursor)
        return fail(db, file, line, "syntax error");
    while (isspace((unsigned char)*cursor))
        cursor++;
    if (*cursor)
        return fail(db, file, line, "syntax error");
    char *rest = equals + 1;
    struct sudoers_list runas_users;
    struct sudoers_list runas_groups;
    memset(&runas_users, 0, sizeof(runas_users));
    memset(&runas_groups, 0, sizeof(runas_groups));
    bool has_runas = false;
    int passwd = 0;
    bool setenv_tag = false;
    int produced = 0;
    for (;;) {
        while (*rest && isspace((unsigned char)*rest))
            rest++;
        if (!*rest)
            break;
        if (*rest == '(') {
            char *close = strchr(rest, ')');
            if (!close)
                return fail(db, file, line, "syntax error");
            *close = 0;
            char *inside = rest + 1;
            char *colon = strchr(inside, ':');
            memset(&runas_users, 0, sizeof(runas_users));
            memset(&runas_groups, 0, sizeof(runas_groups));
            if (colon) {
                *colon = 0;
                split_into(colon + 1, ',', &runas_groups);
            }
            split_into(inside, ',', &runas_users);
            has_runas = true;
            rest = close + 1;
            continue;
        }
        if (isupper((unsigned char)*rest)) {
            if (tag_word(rest, "NOPASSWD")) {
                passwd = 2;
                rest += 9;
                continue;
            }
            if (tag_word(rest, "PASSWD")) {
                passwd = 1;
                rest += 7;
                continue;
            }
            if (tag_word(rest, "SETENV")) {
                setenv_tag = true;
                rest += 7;
                continue;
            }
            if (tag_word(rest, "NOSETENV")) {
                setenv_tag = false;
                rest += 9;
                continue;
            }
            static const char *const other[] = {"EXEC", "NOEXEC", "LOG_INPUT", "NOLOG_INPUT",
                                                "LOG_OUTPUT", "NOLOG_OUTPUT", "MAIL", "NOMAIL",
                                                "FOLLOW", "NOFOLLOW", NULL};
            bool skipped = false;
            for (int i = 0; other[i]; i++) {
                if (tag_word(rest, other[i])) {
                    rest += strlen(other[i]) + 1;
                    skipped = true;
                    break;
                }
            }
            if (skipped)
                continue;
        }
        char *piece = split_next(&rest, ',', false);
        if (!piece)
            break;
        piece = trim(piece);
        if (!*piece)
            return fail(db, file, line, "syntax error");
        char *command = xdup(piece);
        unescape(command);
        struct sudoers_rule rule;
        memset(&rule, 0, sizeof(rule));
        rule.who = who;
        rule.hosts = hosts;
        rule.runas_users = runas_users;
        rule.runas_groups = runas_groups;
        rule.has_runas = has_runas;
        rule.passwd = passwd;
        rule.setenv = setenv_tag;
        rule.command = command;
        add_rule(db, &rule);
        produced++;
        if (!rest)
            break;
    }
    if (!produced)
        return fail(db, file, line, "syntax error");
    return 0;
}

static int load_file(struct sudoers *db, const char *path, int depth);

static int compare_names(const void *left, const void *right)
{
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static char *join_path(const char *base_file, const char *name)
{
    if (name[0] == '/')
        return xdup(name);
    const char *slash = strrchr(base_file, '/');
    if (!slash)
        return xdup(name);
    return sudoers_format("%.*s/%s", (int)(slash - base_file), base_file, name);
}

static char *include_argument(char *text)
{
    char *arg = trim(text);
    if (*arg == '"') {
        arg++;
        size_t length = strlen(arg);
        if (length && arg[length - 1] == '"')
            arg[length - 1] = 0;
    }
    return arg;
}

static int load_directory(struct sudoers *db, const char *directory, int depth,
                          const char *file, int line)
{
    struct stat info;
    if (stat(directory, &info) != 0 || !S_ISDIR(info.st_mode))
        return 0;
    if (db->check_permissions) {
        if (info.st_uid != 0) {
            set_error(db, "%s is owned by uid %u, should be 0", directory, (unsigned)info.st_uid);
            return -1;
        }
        if (info.st_mode & 002) {
            set_error(db, "%s is world writable", directory);
            return -1;
        }
    }
    DIR *dir = opendir(directory);
    if (!dir)
        return fail(db, file, line, "cannot open include directory");
    char **names = NULL;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t length = strlen(name);
        if (strchr(name, '.') || (length && name[length - 1] == '~'))
            continue;
        names = sudoers_xrealloc(names, sizeof(char *) * (size_t)(count + 1));
        names[count++] = xdup(name);
    }
    closedir(dir);
    if (count > 1)
        qsort(names, (size_t)count, sizeof(char *), compare_names);
    int result = 0;
    for (int i = 0; i < count; i++) {
        char *full = sudoers_format("%s/%s", directory, names[i]);
        if (result == 0 && load_file(db, full, depth + 1) != 0)
            result = -1;
        free(full);
        free(names[i]);
    }
    free(names);
    return result;
}

static int process_line(struct sudoers *db, char *text, const char *file, int line, int depth)
{
    char *body = trim(text);
    if (!*body)
        return 0;
    if (strncmp(body, "#includedir", 11) == 0 || strncmp(body, "@includedir", 11) == 0) {
        char *full = join_path(file, include_argument(body + 11));
        int result = load_directory(db, full, depth, file, line);
        free(full);
        return result;
    }
    if (strncmp(body, "#include", 8) == 0 || strncmp(body, "@include", 8) == 0) {
        char *full = join_path(file, include_argument(body + 8));
        int result = load_file(db, full, depth + 1) != 0 ? -1 : 0;
        free(full);
        return result;
    }
    char *comment = NULL;
    bool quoted = false;
    for (char *c = body; *c; c++) {
        if (*c == '\\' && c[1]) {
            c++;
            continue;
        }
        if (*c == '"')
            quoted = !quoted;
        if (*c == '#' && !quoted && !isdigit((unsigned char)c[1])) {
            comment = c;
            break;
        }
    }
    if (comment)
        *comment = 0;
    body = trim(body);
    if (!*body)
        return 0;
    if (strncmp(body, "Defaults", 8) == 0 &&
        (isspace((unsigned char)body[8]) || strchr(":@!>", body[8]) != NULL) && body[8])
        return parse_defaults(db, body, file, line);
    static const struct {
        const char *word;
        enum sudoers_alias_kind kind;
    } words[] = {{"User_Alias", ALIAS_USER}, {"Runas_Alias", ALIAS_RUNAS},
                 {"Host_Alias", ALIAS_HOST}, {"Cmnd_Alias", ALIAS_CMND}};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        size_t length = strlen(words[i].word);
        if (strncmp(body, words[i].word, length) == 0 && isspace((unsigned char)body[length]))
            return parse_alias(db, body, words[i].kind, file, line);
    }
    return parse_userspec(db, body, file, line);
}

static int load_file(struct sudoers *db, const char *path, int depth)
{
    if (depth > INCLUDE_DEPTH) {
        set_error(db, "%s: too many levels of includes", path);
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_error(db, "unable to open %s: %s", path, strerror(errno));
        return -1;
    }
    struct stat info;
    if (fstat(fd, &info) != 0) {
        close(fd);
        set_error(db, "unable to stat %s: %s", path, strerror(errno));
        return -1;
    }
    if (db->check_permissions) {
        if (info.st_uid != 0) {
            close(fd);
            set_error(db, "%s is owned by uid %u, should be 0", path,
                     (unsigned)info.st_uid);
            return -1;
        }
        if (info.st_mode & 002) {
            close(fd);
            set_error(db, "%s is world writable", path);
            return -1;
        }
        if (info.st_mode & 020) {
            close(fd);
            set_error(db, "%s is writable by group", path);
            return -1;
        }
    }
    size_t capacity = 4096;
    size_t length = 0;
    char *buffer = xalloc(capacity);
    for (;;) {
        if (length + 1024 >= capacity) {
            char *grown = xalloc(capacity * 2);
            memcpy(grown, buffer, length);
            free(buffer);
            buffer = grown;
            capacity *= 2;
        }
        ssize_t got = read(fd, buffer + length, capacity - length - 1);
        if (got < 0) {
            close(fd);
            set_error(db, "unable to read %s: %s", path, strerror(errno));
            return -1;
        }
        if (got == 0)
            break;
        length += (size_t)got;
    }
    close(fd);
    buffer[length] = 0;
    db->files = sudoers_xrealloc(db->files, sizeof(char *) * (size_t)(db->file_count + 1));
    db->files[db->file_count++] = xdup(path);
    char *joined = xalloc(length + 2);
    int line = 1;
    int status = 0;
    char *cursor = buffer;
    while (*cursor) {
        int start_line = line;
        size_t joined_length = 0;
        for (;;) {
            char *newline = strchr(cursor, '\n');
            size_t piece = newline ? (size_t)(newline - cursor) : strlen(cursor);
            size_t trimmed = piece;
            if (trimmed && cursor[trimmed - 1] == '\r')
                trimmed--;
            bool continued = trimmed && cursor[trimmed - 1] == '\\';
            size_t take = continued ? trimmed - 1 : trimmed;
            memcpy(joined + joined_length, cursor, take);
            joined_length += take;
            cursor = newline ? newline + 1 : cursor + piece;
            line++;
            if (!continued || !*cursor)
                break;
        }
        joined[joined_length] = 0;
        status = process_line(db, joined, path, start_line, depth);
        if (status != 0)
            break;
    }
    free(joined);
    free(buffer);
    return status;
}

int sudoers_load(struct sudoers *db, const char *path)
{
    return load_file(db, path, 0);
}
