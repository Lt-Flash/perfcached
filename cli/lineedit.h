/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * lineedit.h — the in-house line editor for perfcli (no readline, no
 * linenoise: the zero-dependency decree covers the CLI too).
 *
 * Raw-termios single-line editing with horizontal scrolling, emacs-ish
 * keys and an in-memory history ring persisted to a 0600 file:
 *   arrows / Ctrl-B Ctrl-F   move        Up/Down / Ctrl-P Ctrl-N  history
 *   Home End Ctrl-A Ctrl-E   line ends   Delete / Ctrl-D          delete
 *   Backspace                delete left Ctrl-W                   word left
 *   Ctrl-U / Ctrl-K          kill line / kill to end
 *   Ctrl-L                   clear screen        Ctrl-C  discard the line
 *   Enter                    submit              Ctrl-D on empty = EOF
 *
 * Byte-based cursor math: ASCII is exact; multi-byte UTF-8 edits work
 * but wide glyphs may draw the cursor a column off - accepted for v1.
 *
 * The editor talks to the CONTROLLING terminal (stdin in, stderr out),
 * leaving stdout clean for redirected results.
 */
#ifndef PC_LINEEDIT_H
#define PC_LINEEDIT_H

#include <stddef.h>

#define LE_LINE_MAX  (256 * 1024)      /* pasted msets fit */
#define LE_HIST_MAX  256

/* read one edited line (without the newline).  Returns a pointer to an
 * internal buffer valid until the next call, or NULL on EOF (Ctrl-D on
 * an empty line, or read failure). */
char *le_readline(const char *prompt);

/* append an accepted line to the in-memory ring (consecutive
 * duplicates are dropped) and, if a history file was loaded, to it */
void le_history_add(const char *line);

/* load up to LE_HIST_MAX lines from @path (created 0600 on first add
 * if absent) and remember the path for appends */
void le_history_load(const char *path);

#endif /* PC_LINEEDIT_H */
