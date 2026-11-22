#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include "util.h"

void u_copy(char *dst, size_t cap, const char *src)
{
    if (!cap)
        return;
    size_t n = 0;
    while (src && src[n] && n + 1 < cap) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
}

static void fixed(char *out, size_t cap, double v, const char *unit)
{
    if (v >= 100.0)
        snprintf(out, cap, "%.0f %s", v, unit);
    else if (v >= 10.0)
        snprintf(out, cap, "%.1f %s", v, unit);
    else
        snprintf(out, cap, "%.2f %s", v, unit);
}

void u_bytes(char *out, size_t cap, uint64_t bytes, int units)
{
    double v = (double)bytes;
    if (units == UNITS_KIB) {
        snprintf(out, cap, "%llu KiB", (unsigned long long)(bytes / 1024));
        return;
    }
    if (units == UNITS_MIB) {
        fixed(out, cap, v / 1048576.0, "MiB");
        return;
    }
    if (bytes < 1024)
        snprintf(out, cap, "%llu B", (unsigned long long)bytes);
    else if (bytes < 1048576ull)
        fixed(out, cap, v / 1024.0, "KiB");
    else if (bytes < 1073741824ull)
        fixed(out, cap, v / 1048576.0, "MiB");
    else if (bytes < 1099511627776ull)
        fixed(out, cap, v / 1073741824.0, "GiB");
    else
        fixed(out, cap, v / 1099511627776.0, "TiB");
}

void u_rate(char *out, size_t cap, uint64_t bps, int units)
{
    char tmp[32];
    u_bytes(tmp, sizeof(tmp), bps, units);
    snprintf(out, cap, "%s/s", tmp);
}

void u_percent(char *out, size_t cap, uint32_t permille)
{
    if (permille > 1000)
        permille = 1000;
    snprintf(out, cap, "%u.%u%%", permille / 10, permille % 10);
}

void u_duration(char *out, size_t cap, uint64_t s)
{
    uint64_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;
    if (d)
        snprintf(out, cap, "%llud %lluh %llum", (unsigned long long)d, (unsigned long long)h, (unsigned long long)m);
    else if (h)
        snprintf(out, cap, "%lluh %llum %llus", (unsigned long long)h, (unsigned long long)m, (unsigned long long)(s % 60));
    else
        snprintf(out, cap, "%llum %llus", (unsigned long long)m, (unsigned long long)(s % 60));
}

bool u_contains_nocase(const char *hay, const char *needle)
{
    if (!needle || !*needle)
        return true;
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl)
            return true;
    }
    return false;
}

char *u_trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
    return s;
}

bool u_read_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t n = read(fd, buf + total, cap - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    close(fd);
    buf[total] = 0;
    return true;
}

uint32_t u_parse_color(const char *s, bool *ok)
{
    if (*s == '#')
        s++;
    size_t n = strlen(s);
    if (ok)
        *ok = false;
    if (n != 6)
        return 0;
    uint32_t v = 0;
    for (size_t i = 0; i < 6; i++) {
        char ch = s[i];
        uint32_t d;
        if (ch >= '0' && ch <= '9')
            d = (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            d = (uint32_t)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F')
            d = (uint32_t)(ch - 'A' + 10);
        else
            return 0;
        v = v << 4 | d;
    }
    if (ok)
        *ok = true;
    return v;
}
