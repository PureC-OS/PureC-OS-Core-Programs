#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "pg/pg.h"
#include "sysdata.h"
#include "theme.h"

struct series {
    const struct history *h;
    const struct grad *grad;
};

void graph_draw(struct pg_canvas *c, int x, int y, int w, int h, const struct series *s, int n,
                uint64_t max_value, int capacity);
void graph_grid(struct pg_canvas *c, int x, int y, int w, int h, int lines);
void bar_draw(struct pg_canvas *c, int x, int y, int w, int h, uint32_t permille, const struct grad *g);
void heat_draw(struct pg_canvas *c, int x, int y, int w, int h, const uint32_t *pm, const bool *online, int n);
uint64_t graph_nice_max(uint64_t value);
