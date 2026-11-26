#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#define SUDOERS_PATH "/etc/sudoers"
#define SUDOERS_MAX_FILES 256

struct sudoers_list {
    char **item;
    int count;
};

enum sudoers_alias_kind {
    ALIAS_USER,
    ALIAS_RUNAS,
    ALIAS_HOST,
    ALIAS_CMND
};

struct sudoers_alias {
    enum sudoers_alias_kind kind;
    char *name;
    struct sudoers_list members;
};

struct sudoers_setting {
    char *name;
    char op;
    bool negate;
    char *value;
};

struct sudoers_defaults {
    char scope;
    struct sudoers_list who;
    struct sudoers_setting *setting;
    int count;
};

struct sudoers_rule {
    struct sudoers_list who;
    struct sudoers_list hosts;
    struct sudoers_list runas_users;
    struct sudoers_list runas_groups;
    bool has_runas;
    int passwd;
    bool setenv;
    char *command;
};

struct sudoers_settings {
    char *secure_path;
    struct sudoers_list env_keep;
    bool env_reset;
    int timestamp_timeout;
    int passwd_tries;
    bool targetpw;
    bool rootpw;
};

struct sudoers {
    struct sudoers_alias *alias;
    int alias_count;
    struct sudoers_defaults *defaults;
    int defaults_count;
    struct sudoers_rule *rule;
    int rule_count;
    char *files[SUDOERS_MAX_FILES];
    int file_count;
    char error[512];
    char warning[8][256];
    int warning_count;
    char hostname[256];
    bool check_permissions;
    bool strict;
};

struct sudoers_subject {
    const char *user;
    uid_t uid;
    gid_t gid;
};

struct sudoers_request {
    const char *runas_user;
    uid_t runas_uid;
    const char *runas_group;
    gid_t runas_gid;
    const char *command;
    const char *arguments;
};

struct sudoers_verdict {
    bool listed;
    bool allowed;
    int passwd;
    bool setenv;
};

void sudoers_init(struct sudoers *db, bool check_permissions, bool strict);
int sudoers_load(struct sudoers *db, const char *path);
void sudoers_settings(const struct sudoers *db, const struct sudoers_subject *who,
                      const struct sudoers_request *request,
                      struct sudoers_settings *out);
void sudoers_check(const struct sudoers *db, const struct sudoers_subject *who,
                   const struct sudoers_request *request,
                   struct sudoers_verdict *out);
bool sudoers_user_has_nopasswd(const struct sudoers *db,
                               const struct sudoers_subject *who);
void sudoers_list_rules(const struct sudoers *db,
                        const struct sudoers_subject *who, FILE *out);
bool sudoers_name_in_list(const struct sudoers_list *list, const char *name);
