/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* promlabel.h - a label value for the Prometheus text exposition; a header of its own so
 * test/promlabeltest.c checks it without linking everything metrics.c reads. */
#ifndef PC_PROMLABEL_H
#define PC_PROMLABEL_H

#include <stddef.h>

/* a quote, a backslash and a line feed escaped; any other byte outside printable ASCII (a
 * control byte, or one that may not be UTF-8) written as '?' */
static inline void pc_prom_label(const char *in, char *out, size_t cap)
{
	size_t i = 0;

	while (*in && i + 2 < cap) {
		unsigned char c = (unsigned char)*in++;

		if (c == '"' || c == '\\' || c == '\n') {
			out[i++] = '\\';
			out[i++] = c == '\n' ? 'n' : (char)c;
		} else {
			out[i++] = c < 0x20 || c > 0x7e ? '?' : (char)c;
		}
	}
	out[i] = 0;
}

#endif
