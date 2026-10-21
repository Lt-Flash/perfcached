/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * walringtest - S226: the pump's two-memcpy ring copy is byte-identical to
 * the byte-at-a-time loop it replaced, for every start offset and every
 * length on rings of several sizes, including a record that straddles the
 * ring's end at every split point and one that fills the ring exactly.
 */
#include <stdio.h>
#include <string.h>
#include "walring.h"

int main(void)
{
	static const uint32_t sizes[] = { 1, 2, 7, 64, 257, 1024 };
	unsigned char buf[1024], want[1024], got[1024];
	unsigned long long cases = 0, bad = 0;
	size_t si;
	uint32_t pos, n, j;

	for (si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
		uint32_t size = sizes[si];

		for (j = 0; j < size; j++)
			buf[j] = (unsigned char)(j * 131u + size);
		for (pos = 0; pos < size; pos++)
			for (n = 0; n <= size; n++) {
				for (j = 0; j < n; j++)      /* the old loop */
					want[j] = buf[(pos + j) % size];
				memset(got, 0xA5, sizeof got);
				walring_copy(buf, size, pos, got, n);
				cases++;
				if (memcmp(got, want, n) || (n < sizeof got && got[n] != 0xA5)) {
					if (bad++ < 3)
						printf("  FAIL size %u pos %u n %u\n", size, pos, n);
				}
			}
	}
	if (!bad)
		printf("  ok   the two-memcpy copy matches the byte loop in all %llu cases "
		       "(every offset, every length, every wrap split, 6 ring sizes)\n", cases);
	printf("walringtest: %d passed, %d failed\n", bad ? 0 : 1, bad ? 1 : 0);
	return bad != 0;
}
