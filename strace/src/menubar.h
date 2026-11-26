#pragma once
#include <stdbool.h>
#include "pg/pg.h"

enum action {
    A_RUN = 1,
    A_ATTACH,
    A_EXPORT,
    A_CLEAR,
    A_QUIT,
    A_COPY_ROW,
    A_COPY_DETAILS,
    A_FOCUS_FILTER,
    A_TOGGLE_DETAILS,
    A_AUTOSCROLL,
    A_EXPAND_ALL,
    A_COLLAPSE_ALL,
    A_STATS,
    A_START,
    A_PAUSE,
    A_CONTINUE,
    A_STEP,
    A_DETACH,
    A_BREAKS,
    A_ABOUT,
    A_MAN,
};

struct menubar_hooks {
    bool (*enabled)(int action);
    bool (*checked)(int action);
    void (*run)(int action);
};

struct pg_widget *menubar_new(struct pg_window *win, const struct menubar_hooks *hooks);
void menubar_close(void);
bool menubar_key(const struct pg_event *e);
