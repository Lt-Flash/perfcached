/*
 * clplattest.c - platform-derived identity (clplat).
 *
 * The point of this module is that a CLONE gets a different identity.
 * So the cases that matter are the ones where two inputs must NOT
 * collide, and the ones where a re-derivation must reproduce exactly.
 *
 * The /sys probe is not tested here - it reads the machine it runs on,
 * which is not a fixture.  Everything it feeds is, and that split is why
 * the composition and derivation are separate functions.
 *
 * Build: cc -o clplattest test/clplattest.c src/clplat.o -lsodium
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../src/clplat.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

int main(void)
{
	const unsigned char SALT[] = "site-salt-alpha";
	const unsigned char SALT2[] = "site-salt-beta";
	unsigned char a[16], b[16], c[16];
	char k[CLPLAT_KEY_MAX], k2[CLPLAT_KEY_MAX], s[37];
	size_t n;

	printf("=== clplattest ===\n");

	/* ---- A. the key, and the newline /sys hands back --------------- */
	n = clplat_key_build(CLPLAT_DMI, "68a85ae7-7837-4a67-a811-ddf700bef911\n",
		k, sizeof k);
	chk(n && strcmp(k, "pve-uuid:68a85ae7-7837-4a67-a811-ddf700bef911") == 0,
		"A1 the DMI key drops the trailing newline");
	n = clplat_key_build(CLPLAT_MAC, "bc:24:11:57:f3:7b\n", k2, sizeof k2);
	chk(n && strcmp(k2, "pve-lxc-mac:bc:24:11:57:f3:7b") == 0,
		"A2 the MAC key is prefixed distinctly");
	chk(strcmp(k, k2) != 0, "A3 the two sources cannot collide");
	chk(!clplat_key_build(CLPLAT_DMI, "\n", k, sizeof k),
		"A4 a raw value that is only whitespace is refused");
	chk(!clplat_key_build(CLPLAT_DMI, "x", k, 4),
		"A5 a key that will not fit is refused, not truncated");
	chk(!clplat_key_build(CLPLAT_NONE, "x", k, sizeof k),
		"A6 an unknown source produces nothing");

	/* ---- B. derivation: same in, same out; different in, different -- */
	clplat_key_build(CLPLAT_DMI, "uuid-one", k, sizeof k);
	clplat_derive(SALT, sizeof SALT - 1, k, a);
	clplat_derive(SALT, sizeof SALT - 1, k, b);
	chk(memcmp(a, b, 16) == 0, "B1 derivation is deterministic");
	clplat_key_build(CLPLAT_DMI, "uuid-two", k2, sizeof k2);
	clplat_derive(SALT, sizeof SALT - 1, k2, c);
	chk(memcmp(a, c, 16) != 0,
		"B2 a DIFFERENT platform id gives a different identity"
		" - this is the whole point");
	clplat_derive(SALT2, sizeof SALT2 - 1, k, c);
	chk(memcmp(a, c, 16) != 0,
		"B3 a different SITE SALT gives a different identity");

	/* the two sources must not alias: a MAC that happens to read like a
	 * uuid must still derive differently, which the prefix guarantees */
	clplat_key_build(CLPLAT_MAC, "uuid-one", k2, sizeof k2);
	clplat_derive(SALT, sizeof SALT - 1, k2, c);
	chk(memcmp(a, c, 16) != 0, "B4 same raw value, different source, "
		"different identity");

	/* ---- C. the version nibble IS the provenance ------------------- */
	chk(clplat_uuid_version(a) == 8, "C1 a derived identity is v8");
	clplat_random_v7(b, 0x0192a1b2c3d4ULL);
	chk(clplat_uuid_version(b) == 7, "C2 a minted identity is v7");
	chk(clplat_uuid_v7_ms(b) == 0x0192a1b2c3d4ULL,
		"C3 and it carries the time it was minted");
	chk(clplat_uuid_v7_ms(a) == 0,
		"C4 a v8 has no timestamp to report");
	{
		unsigned char x[16], y[16];

		clplat_random_v7(x, 1000);
		clplat_random_v7(y, 2000);
		chk(memcmp(x, y, 6) < 0,
			"C5 v7 identities sort by when they were minted");
		chk(memcmp(x + 6, y + 6, 10) != 0,
			"C6 and are not otherwise identical");
	}
	/* RFC 9562 variant bits, both flavours */
	chk((a[8] & 0xc0) == 0x80, "C7 v8 carries the RFC variant");
	chk((b[8] & 0xc0) == 0x80, "C8 v7 carries the RFC variant");

	/* ---- D. the canonical rendering operators will read ------------ */
	memset(a, 0, 16);
	a[0] = 0x01; a[15] = 0xef;
	clplat_uuid_str(a, s);
	chk(strlen(s) == 36, "D1 36 characters");
	chk(s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-',
		"D2 dashes at 8-4-4-4-12");
	chk(strncmp(s, "01000000-", 9) == 0 && strcmp(s + 24, "0000000000ef") == 0,
		"D3 the bytes land where a reader expects them");

	printf("clplattest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
