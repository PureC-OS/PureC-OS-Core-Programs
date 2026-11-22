#pragma once
#include <stdbool.h>
#include "pg/pg.h"

struct pg_widget *view_tree_new(void);
struct pg_widget *view_detail_new(void);
struct pg_widget *view_stats_new(void);
void view_rebuild(void);
void view_redraw(void);
void view_set_filter(const char *text);
void view_set_autoscroll(bool on);
bool view_autoscroll(void);
void view_reveal_stopped(void);
void view_clear_selection(void);
int view_row_count(void);
void view_on_autoscroll(void (*fn)(bool on));
