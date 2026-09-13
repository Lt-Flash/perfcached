/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * clinit.c - the node identity file format.  See clinit.h.
 */
#include <stdio.h>
#include <string.h>

#include "clinit.h"
#include "fnv1a.h"

uint64_t clinit_fnv1a64(const unsigned char *p, size_t n)
{
	return fnv1a64(p, n);
}



static int hexval(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

void clinit_ident_hex(const unsigned char *id, char *out)
{
	static const char hx[] = "0123456789abcdef";
	int i;

	for (i = 0; i < CLINIT_IDENT_LEN; i++) {
		out[2 * i] = hx[id[i] >> 4];
		out[2 * i + 1] = hx[id[i] & 15];
	}
	out[2 * CLINIT_IDENT_LEN] = 0;
}

int clinit_hex2bin(const char *s, unsigned char *out, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		int hi = hexval((unsigned char)s[2 * i]);
		int lo = hexval((unsigned char)s[2 * i + 1]);

		if (hi < 0 || lo < 0)
			return -1;
		out[i] = (unsigned char)(hi << 4 | lo);
	}
	return 0;
}

size_t clinit_ident_render(char *buf, size_t cap, const unsigned char *id)
{
	char hex[2 * CLINIT_IDENT_LEN + 1];
	int n;

	clinit_ident_hex(id, hex);
	n = snprintf(buf, cap, CLINIT_IDENT_HEADER
		"\nidentity %s\ncheck %016llx\n", hex,
		(unsigned long long)clinit_fnv1a64(id, CLINIT_IDENT_LEN));
	if (n < 0 || (size_t)n >= cap)
		return 0;
	return (size_t)n;
}

int clinit_ident_parse(const unsigned char *buf, size_t len,
		unsigned char *id, char *why, size_t wlen)
{
	const char *s = (const char *)buf;
	size_t hl = strlen(CLINIT_IDENT_HEADER);
	unsigned char chk[8];
	uint64_t want = 0;
	int i;

	/* the legacy shape: sixteen raw bytes and nothing else */
	if (len == CLINIT_IDENT_LEN) {
		memcpy(id, buf, CLINIT_IDENT_LEN);
		return 0;
	}
	if (len == 0) {
		snprintf(why, wlen, "the file is empty");
		return -1;
	}
	if (len < hl + 1 || memcmp(s, CLINIT_IDENT_HEADER, hl) != 0 ||
	        s[hl] != '\n') {
		snprintf(why, wlen, "the first line is not \""
			CLINIT_IDENT_HEADER "\"");
		return -1;
	}
	s += hl + 1;
	len -= hl + 1;
	if (len < 9 + 32 + 1 || memcmp(s, "identity ", 9) != 0 ||
	        s[9 + 32] != '\n' ||
	        clinit_hex2bin(s + 9, id, CLINIT_IDENT_LEN) != 0) {
		snprintf(why, wlen, "the second line is not "
			"\"identity <32 hex digits>\"");
		return -1;
	}
	s += 9 + 32 + 1;
	len -= 9 + 32 + 1;
	if (len < 6 + 16 + 1 || memcmp(s, "check ", 6) != 0 ||
	        s[6 + 16] != '\n' || clinit_hex2bin(s + 6, chk, 8) != 0) {
		snprintf(why, wlen, "the third line is not "
			"\"check <16 hex digits>\"");
		return -1;
	}
	if (len != 6 + 16 + 1) {
		snprintf(why, wlen, "there is more after the check line");
		return -1;
	}
	for (i = 0; i < 8; i++)
		want = want << 8 | chk[i];
	if (want != clinit_fnv1a64(id, CLINIT_IDENT_LEN)) {
		snprintf(why, wlen, "the check value does not match the "
			"identity (edited, or a torn write)");
		return -1;
	}
	return 1;
}

int clinit_proposed_id(const unsigned char *id)
{
	return 1 + (int)(clinit_fnv1a64(id, CLINIT_IDENT_LEN) % 1023);
}

/* ---- the config digest ------------------------------------------------- */

uint64_t clinit_config_digest(const struct clinit_digest_in *in)
{
	uint64_t h = FNV1A64_BASIS;
	const char *names[CLINIT_MAX_NAMES];
	int i, j, n = in->n_names;

	#define DIG(b) do { h = fnv1a64_byte(h, (unsigned char)(b)); \
		} while (0)

	DIG(in->mode);
	DIG(in->eager);
	/* folded only when ON, so a fleet without a WAL keeps the digest it
	 * had before the flag existed and a rolling upgrade does not split
	 * it (S73b) */
	if (in->wal) {
		DIG('W');
		DIG(1);
	}
	/* PLACEMENT is interchange-relevant, and more so than anything else
	 * here: two nodes that hash keys differently agree about every
	 * collection and still put the same key in two places. */
	{
		const char *q;

		for (q = in->route_algo; *q; q++)
			DIG(*q);
		DIG(0);
	}
	/* the SET, order-independent: sort the names before folding */
	if (n > CLINIT_MAX_NAMES)
		n = CLINIT_MAX_NAMES;
	for (i = 0; i < n; i++)
		names[i] = in->names[i];
	for (i = 1; i < n; i++) {
		const char *tmp = names[i];

		for (j = i; j > 0 && strcmp(names[j - 1], tmp) > 0; j--)
			names[j] = names[j - 1];
		names[j] = tmp;
	}
	for (i = 0; i < n; i++) {
		const char *q;

		for (q = names[i]; *q; q++)
			DIG(*q);
		DIG(0);
	}
	#undef DIG
	return h;
}
