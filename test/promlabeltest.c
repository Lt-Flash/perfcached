/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* promlabeltest.c - a label value in the text exposition: a quote and a backslash escaped, a line
 * feed as \n, and no control or non-UTF-8 byte left raw (one fails the whole scrape). */
#include <stdio.h>
#include <string.h>
#include "promlabel.h"

static int pass, fail;

static void chk(int cond, const char *what)
{
	if (cond)
		pass++;
	else
		fail++;
	printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
}

/* printable ASCII only, and no quote that is not escaped */
static int clean(const char *s)
{
	size_t i;

	for (i = 0; s[i]; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c < 0x20 || c > 0x7e)
			return 0;
		if (c == '\\' && s[i + 1])
			i++;
		else if (c == '"')
			return 0;
	}
	return 1;
}

int main(void)
{
	char out[64];

	pc_prom_label("get", out, sizeof out);
	chk(!strcmp(out, "get"), "a plain name as it is");
	pc_prom_label("a\"b\\c", out, sizeof out);
	chk(!strcmp(out, "a\\\"b\\\\c"), "a quote and a backslash escaped");
	pc_prom_label("a\nb", out, sizeof out);
	chk(!strcmp(out, "a\\nb"), "a line feed written as \\n");
	pc_prom_label("a\rb\x01" "c\x7f", out, sizeof out);
	chk(clean(out) && strlen(out) >= 3, "CR, a control byte and DEL not left raw");
	pc_prom_label("\xff\xfe" "x\xc3", out, sizeof out);
	chk(clean(out), "bytes that are not UTF-8 not left raw");
	pc_prom_label("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n", out,
		sizeof out);
	chk(clean(out) && strlen(out) < sizeof out, "a long run of escapes stays inside the buffer, "
		"never half an escape");

	printf("promlabeltest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
