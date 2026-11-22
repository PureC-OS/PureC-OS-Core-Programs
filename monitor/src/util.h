#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum { UNITS_AUTO = 0, UNITS_KIB, UNITS_MIB, UNITS_COUNT };

void u_copy(char *dst, size_t cap, const char *src);
void u_bytes(char *out, size_t cap, uint64_t bytes, int units);
void u_rate(char *out, size_t cap, uint64_t bytes_per_s, int units);
void u_percent(char *out, size_t cap, uint32_t permille);
void u_duration(char *out, size_t cap, uint64_t seconds);
bool u_contains_nocase(const char *hay, const char *needle);
char *u_trim(char *s);
bool u_read_file(const char *path, char *buf, size_t cap);
uint32_t u_parse_color(const char *s, bool *ok);
