#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "pg/pg.h"
#include "theme.h"
#include "sysdata.h"

typedef void (*ui_confirm_fn)(void *data);

struct pg_rect ui_card(struct pg_canvas *c, int x, int y, int w, int h, const char *title, const char *right);
int ui_text(struct pg_canvas *c, int font, int x, int y, const char *s, uint32_t color);
void ui_text_right(struct pg_canvas *c, int font, int right, int y, const char *s, uint32_t color);
void ui_text_fit(struct pg_canvas *c, int font, int x, int y, int maxw, const char *s, uint32_t color);
void ui_stat(struct pg_canvas *c, int x, int y, int w, const char *label, const char *value, uint32_t color);
void ui_legend(struct pg_canvas *c, int x, int y, const char *label, uint32_t color);

struct pg_widget *ui_overlay_new(struct pg_widget *content);
void ui_toast(const char *text, uint32_t color);
void ui_confirm(const char *title, const char *text, const char *action, bool danger, ui_confirm_fn fn, void *data);
bool ui_modal_active(void);
bool ui_modal_key(const struct pg_event *e);

struct pg_widget *ui_tabbar_new(const char *const *labels, const int *icons, int count, void (*changed)(int));
void ui_tabbar_select(struct pg_widget *bar, int index);
int ui_tabbar_selected(const struct pg_widget *bar);
