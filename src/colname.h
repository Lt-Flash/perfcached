/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* colname.h - the one collection-name rule, for whatever originates a name: the config,
 * a client's create/rename, a peer's announce.  Only the daemon makes <name>.nofall. */
#ifndef PC_COLNAME_H
#define PC_COLNAME_H

#include <stddef.h>
#include <string.h>

#define PC_COL_NAME_RULE "collection names are 1-32 characters of " \
	"A-Z a-z 0-9 _ - and do not start with -"
#define PC_COL_NOFALL ".nofall"

/* a name a person, a client or a peer may give */
static inline int pc_col_name_ok(const char *s, size_t n)
{
	size_t i;

	if (!s || n == 0 || n > 32 || s[0] == '-')
		return 0;
	for (i = 0; i < n; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') ||
		      (s[i] >= 'A' && s[i] <= 'Z') ||
		      (s[i] >= '0' && s[i] <= '9') ||
		      s[i] == '_' || s[i] == '-'))
			return 0;
	return 1;
}

/* a name the daemon may hold: a given one, or its .nofall companion */
static inline int pc_col_name_held_ok(const char *s, size_t n)
{
	size_t k = sizeof PC_COL_NOFALL - 1;

	if (pc_col_name_ok(s, n))
		return 1;
	return s && n > k && !memcmp(s + n - k, PC_COL_NOFALL, k) &&
		pc_col_name_ok(s, n - k);
}

/* @s as a log line may carry it: printable ASCII as is, every other byte as \xHH, so a
 *  name that predates the rule cannot forge a log line.  NUL-terminates @out (@cap > 0). */
static inline const char *pc_col_name_print(char *out, size_t cap,
		const char *s, size_t n)
{
	static const char hx[] = "0123456789abcdef";
	size_t i, o = 0;

	for (i = 0; i < n && o + 5 < cap; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c >= 0x20 && c < 0x7f && c != '\\') {
			out[o++] = (char)c;
		} else {
			out[o++] = '\\';
			out[o++] = 'x';
			out[o++] = hx[c >> 4];
			out[o++] = hx[c & 15];
		}
	}
	out[o] = 0;
	return out;
}

#endif /* PC_COLNAME_H */
