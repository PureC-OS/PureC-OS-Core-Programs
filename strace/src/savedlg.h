#pragma once

typedef const char *(*savedlg_fn)(const char *path);

void savedlg_open(const char *default_path, savedlg_fn fn);
void savedlg_close(void);
