/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * pcglob.c - the key glob (pcglob.h).  Moved from verbs.c unchanged (the
 * 0.7.2 split, wave 1 - doc/MODULARITY.md); test/pcglobtest.c.
 */
#include "pcglob.h"

/* S331: the same matcher for the read-through allow-list */
int pc_glob_match(const char *p, int pn, const char *s, int sn)
{
	return pc_glob(p, pn, s, sn);
}

int pc_glob(const char *p, int pn, const char *s, int sn)
{
	int pi = 0, si = 0, star_p = -1, star_s = 0;

	while (si < sn) {
		if (pi < pn) {
			char pc = p[pi];

			if (pc == '*') {
				star_p = ++pi;         /* remember the backtrack point */
				star_s = si;
				continue;
			}
			if (pc == '?') {
				pi++; si++;
				continue;
			}
			if (pc == '[') {
				int j = pi + 1, neg = 0, matched = 0, first;

				if (j < pn && (p[j] == '^' || p[j] == '!')) {
					neg = 1; j++;
				}
				first = j;
				while (j < pn && (p[j] != ']' || j == first)) {
					char lo = p[j];

					if (lo == '\\' && j + 1 < pn)
						lo = p[++j];
					if (j + 2 < pn && p[j + 1] == '-' && p[j + 2] != ']') {
						if (s[si] >= lo && s[si] <= p[j + 2])
							matched = 1;
						j += 3;
					} else {
						if (s[si] == lo)
							matched = 1;
						j++;
					}
				}
				if (j >= pn)
					return 0;          /* unterminated class */
				if (matched != neg) {
					pi = j + 1; si++;
					continue;
				}
				/* class mismatch: fall through to the backtrack */
			} else if (pc == '\\' && pi + 1 < pn) {
				if (p[pi + 1] == s[si]) {
					pi += 2; si++;
					continue;
				}
			} else if (pc == s[si]) {
				pi++; si++;
				continue;
			}
		}
		if (star_p < 0)
			return 0;
		pi = star_p;                   /* the last '*' eats one more byte */
		si = ++star_s;
	}
	while (pi < pn && p[pi] == '*')
		pi++;
	return pi == pn;
}
