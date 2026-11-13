/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* lineedit.h - perfcli's own line editor (no readline: zero dependencies): raw termios, emacs keys,
 * a 0600 history file; it uses the controlling terminal (stdin, stderr), so stdout stays clean */
#ifndef PC_LINEEDIT_H
#define PC_LINEEDIT_H

#include <stddef.h>

#define LE_LINE_MAX  (256 * 1024)      /* pasted msets fit */
#define LE_HIST_MAX  256

/* one edited line without the newline, in a buffer valid until the next call; NULL on EOF
 * (Ctrl-D on an empty line) or a read failure */
char *le_readline(const char *prompt);

/* append an accepted line to the in-memory ring (consecutive
 * duplicates are dropped) and, if a history file was loaded, to it */
void le_history_add(const char *line);

/* load up to LE_HIST_MAX lines from @path (created 0600 on first add
 * if absent) and remember the path for appends */
void le_history_load(const char *path);

#endif /* PC_LINEEDIT_H */
