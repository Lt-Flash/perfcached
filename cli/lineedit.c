/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * lineedit.c — see lineedit.h.  One static editor instance; perfcli is
 * single-threaded by design.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "lineedit.h"

static char buf[LE_LINE_MAX];
static size_t len, pos, off;           /* text, cursor, display window */
static const char *le_prompt;
static size_t plen, cols;

static char *hist[LE_HIST_MAX];
static int hist_n;                     /* entries in the ring */
static int hist_idx;                   /* -1 = editing a fresh line */
static char stash[LE_LINE_MAX];        /* the fresh line while browsing */
static char *hist_path;

/* ---- terminal ---------------------------------------------------------- */

static struct termios t_saved;
static int t_raw;

static void tty_restore(void)
{
	if (t_raw) {
		tcsetattr(0, TCSAFLUSH, &t_saved);
		t_raw = 0;
	}
}

static int tty_raw(void)
{
	struct termios t;

	if (tcgetattr(0, &t_saved) != 0)
		return -1;
	t = t_saved;
	t.c_iflag &= ~(tcflag_t)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
	t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(0, TCSAFLUSH, &t) != 0)
		return -1;
	t_raw = 1;
	atexit(tty_restore);
	return 0;
}

static void tput(const char *s, size_t n)
{
	size_t o = 0;

	while (o < n) {
		ssize_t r = write(2, s + o, n - o);

		if (r <= 0)
			return;
		o += (size_t)r;
	}
}

static void tputs0(const char *s) { tput(s, strlen(s)); }

/* ---- reverse-i-search (Ctrl-R) ---------------------------------------- */

static char se_q[256];                 /* the search query */
static size_t se_qlen;
static int se_idx;                     /* current match, -1 = none */

static void refresh(void);

static void search_refresh(void)
{
	char head[300];
	size_t n;

	n = (size_t)snprintf(head, sizeof head,
		"\r\x1b[0K(reverse-i-search)`%.*s': ", (int)se_qlen, se_q);
	tput(head, n);
	if (se_idx >= 0)
		tputs0(hist[se_idx]);
}

/* newest match at or below @from containing the query; -1 = none */
static int search_back(int from)
{
	int i;

	se_q[se_qlen] = 0;
	for (i = from; i >= 0; i--)
		if (strstr(hist[i], se_q))
			return i;
	return -1;
}

/* adopt the current match into the edit buffer */
static void search_accept(void)
{
	if (se_idx >= 0) {
		snprintf(buf, sizeof buf, "%s", hist[se_idx]);
		len = pos = strlen(buf);
		off = 0;
	}
	refresh();
}

/* redraw: prompt + the visible window of the line + cursor placement */
static void refresh(void)
{
	char tail[48];
	size_t width = cols > plen + 2 ? cols - plen - 1 : 8, show;

	if (pos < off)
		off = pos;
	if (pos - off >= width)
		off = pos - width + 1;
	show = len - off > width ? width : len - off;

	tputs0("\r");
	tput(le_prompt, plen);
	tput(buf + off, show);
	snprintf(tail, sizeof tail, "\x1b[0K\r\x1b[%zuC",
		plen + (pos - off));
	tputs0(tail);
}

/* ---- history ----------------------------------------------------------- */

void le_history_add(const char *line)
{
	if (!*line)
		return;
	if (hist_n && !strcmp(hist[hist_n - 1], line))
		return;                        /* consecutive duplicate */
	if (hist_n == LE_HIST_MAX) {
		free(hist[0]);
		memmove(hist, hist + 1, sizeof hist[0] * (LE_HIST_MAX - 1));
		hist_n--;
	}
	hist[hist_n++] = strdup(line);
	if (hist_path) {
		int fd = open(hist_path, O_WRONLY | O_CREAT | O_APPEND, 0600);

		if (fd >= 0) {
			if (write(fd, line, strlen(line)) < 0 ||
			        write(fd, "\n", 1) < 0) { /* best effort */ }
			close(fd);
		}
	}
}

void le_history_load(const char *path)
{
	FILE *f = fopen(path, "r");
	char l[LE_LINE_MAX];

	hist_path = strdup(path);
	if (!f)
		return;
	while (fgets(l, sizeof l, f)) {
		char *nl = strchr(l, '\n');

		if (nl)
			*nl = 0;
		if (*l) {
			char *save = hist_path;

			hist_path = NULL;      /* load must not re-append */
			le_history_add(l);
			hist_path = save;
		}
	}
	fclose(f);
}

static void hist_show(int idx)
{
	if (hist_idx == -1 && idx != -1) {
		memcpy(stash, buf, len);       /* leaving the fresh line */
		stash[len] = 0;
	}
	hist_idx = idx;
	if (idx == -1)
		snprintf(buf, sizeof buf, "%s", stash);
	else
		snprintf(buf, sizeof buf, "%s", hist[idx]);
	len = pos = strlen(buf);
	off = 0;
	refresh();
}

/* ---- editing primitives ------------------------------------------------ */

static void ins(char c)
{
	if (len >= LE_LINE_MAX - 1)
		return;
	memmove(buf + pos + 1, buf + pos, len - pos);
	buf[pos++] = c;
	len++;
	refresh();
}

static void del_at(size_t at)
{
	if (at >= len)
		return;
	memmove(buf + at, buf + at + 1, len - at - 1);
	len--;
	refresh();
}

static void kill_word(void)
{
	size_t e = pos;

	while (pos > 0 && buf[pos - 1] == ' ')
		pos--;
	while (pos > 0 && buf[pos - 1] != ' ')
		pos--;
	memmove(buf + pos, buf + e, len - e);
	len -= e - pos;
	refresh();
}

/* ---- the read loop ----------------------------------------------------- */

char *le_readline(const char *prompt)
{
	struct winsize ws;

	le_prompt = prompt;
	plen = strlen(prompt);
	cols = 80;
	if (ioctl(2, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		cols = ws.ws_col;
	if (tty_raw() != 0) {              /* not a tty after all: plain read */
		if (!fgets(buf, sizeof buf, stdin))
			return NULL;
		len = strlen(buf);
		while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
			buf[--len] = 0;
		return buf;
	}

	len = pos = off = 0;
	buf[0] = 0;
	hist_idx = -1;
	refresh();

	for (;;) {
		unsigned char c;
		ssize_t r = read(0, &c, 1);

		if (r <= 0) {
			tty_restore();
			return NULL;
		}
		switch (c) {
		case 13: case 10:              /* Enter */
			buf[len] = 0;
			tty_restore();
			tputs0("\r\n");
			return buf;
		case 3:                        /* Ctrl-C: discard the line */
			tputs0("^C\r\n");
			len = pos = off = 0;
			buf[0] = 0;
			hist_idx = -1;
			refresh();
			break;
		case 4:                        /* Ctrl-D */
			if (len == 0) {
				tty_restore();
				tputs0("\r\n");
				return NULL;
			}
			del_at(pos);
			break;
		case 127: case 8:              /* Backspace */
			if (pos > 0) {
				pos--;
				del_at(pos);
			}
			break;
		case 1:  pos = 0; refresh(); break;             /* Ctrl-A */
		case 5:  pos = len; refresh(); break;           /* Ctrl-E */
		case 2:  if (pos) pos--; refresh(); break;      /* Ctrl-B */
		case 6:  if (pos < len) pos++; refresh(); break;/* Ctrl-F */
		case 11: len = pos; buf[len] = 0; refresh(); break; /* ^K */
		case 21:                       /* Ctrl-U: kill the whole line */
			memmove(buf, buf + pos, len - pos);
			len -= pos;
			pos = 0;
			refresh();
			break;
		case 23: kill_word(); break;   /* Ctrl-W */
		case 12:                       /* Ctrl-L: clear screen */
			tputs0("\x1b[H\x1b[2J");
			refresh();
			break;
		case 18: {                     /* Ctrl-R: reverse-i-search */
			se_qlen = 0;
			se_idx = hist_n ? search_back(hist_n - 1) : -1;
			search_refresh();
			for (;;) {
				unsigned char sc;

				if (read(0, &sc, 1) != 1)
					break;
				if (sc == 18) {        /* next older match */
					if (se_idx > 0) {
						int m = search_back(se_idx - 1);

						if (m >= 0)
							se_idx = m;
					}
				} else if (sc == 127 || sc == 8) {
					if (se_qlen) {
						se_qlen--;
						se_idx = search_back(hist_n - 1);
					}
				} else if (sc == 27 || sc == 7) {
					/* ESC / Ctrl-G: cancel, keep the line */
					refresh();
					goto search_done;
				} else if (sc == 13 || sc == 10) {
					/* accept AND submit */
					search_accept();
					buf[len] = 0;
					tty_restore();
					tputs0("\r\n");
					return buf;
				} else if (sc >= 32 && se_qlen < sizeof se_q - 1) {
					se_q[se_qlen++] = (char)sc;
					se_idx = search_back(hist_n - 1);
				} else {
					/* any other key: accept into the buffer,
					 * back to normal editing */
					search_accept();
					goto search_done;
				}
				search_refresh();
			}
search_done:
			break; }
		case 16:                       /* Ctrl-P: history prev */
			if (hist_n && hist_idx != 0)
				hist_show(hist_idx == -1 ? hist_n - 1
					: hist_idx - 1);
			break;
		case 14:                       /* Ctrl-N: history next */
			if (hist_idx != -1)
				hist_show(hist_idx + 1 < hist_n ?
					hist_idx + 1 : -1);
			break;
		case 27: {                     /* ESC sequences */
			unsigned char s0, s1;

			if (read(0, &s0, 1) != 1)
				break;
			if (s0 != '[' && s0 != 'O')
				break;
			if (read(0, &s1, 1) != 1)
				break;
			switch (s1) {
			case 'A':                  /* Up */
				if (hist_n && hist_idx != 0)
					hist_show(hist_idx == -1 ?
						hist_n - 1 : hist_idx - 1);
				break;
			case 'B':                  /* Down */
				if (hist_idx != -1)
					hist_show(hist_idx + 1 < hist_n ?
						hist_idx + 1 : -1);
				break;
			case 'C': if (pos < len) pos++; refresh(); break;
			case 'D': if (pos) pos--; refresh(); break;
			case 'H': pos = 0; refresh(); break;
			case 'F': pos = len; refresh(); break;
			default:
				if (s1 >= '0' && s1 <= '9') {
					unsigned char t2;

					if (read(0, &t2, 1) != 1)
						break;
					if (t2 != '~')
						break;
					if (s1 == '3')
						del_at(pos);
					else if (s1 == '1' || s1 == '7') {
						pos = 0;
						refresh();
					} else if (s1 == '4' || s1 == '8') {
						pos = len;
						refresh();
					}
				}
			}
			break; }
		default:
			if (c >= 32 || c >= 0x80)
				ins((char)c);
		}
	}
}
