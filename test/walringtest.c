/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* walringtest.c - the pump's two-memcpy ring copy equals a byte-at-a-time copy for every offset and
 * length, including records straddling the ring's end and one that fills it exactly; and a ring's
 * places stay continuous across its counters' 2^32 wrap. */
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
				for (j = 0; j < n; j++)      /* the reference copy */
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

	/* the producer's and the pump's places come from free-running uint32 byte counters: a
	 * record that starts where the last one ended must stay there across the 2^32 wrap, for
	 * any ring_kb the config takes (4..16384) and fsync = always's 1024 x mult */
	{
		static const int kbs[] = { 4, 5, 1000, 1024, 3072, 5120, 7168, 12288, 16384 };
		int wrapbad = 0, ki;

		for (ki = 0; ki < (int)(sizeof kbs / sizeof kbs[0]); ki++) {
			uint32_t size = walring_size(kbs[ki]), c0 = 0xFFFFFFFFu - 200, d;

			if (size < (uint32_t)kbs[ki] << 10) {
				printf("  FAIL ring_kb %d: a ring of %u bytes, less than asked\n",
					kbs[ki], size);
				wrapbad++;
				continue;
			}
			for (d = 0; d <= 400; d++)
				if (walring_pos(c0 + d, size) !=
				        (walring_pos(c0, size) + d) % size) {
					printf("  FAIL ring_kb %d: the place jumps from %u to %u "
						"as the counter wraps\n", kbs[ki],
						walring_pos(c0 + d - 1, size),
						walring_pos(c0 + d, size));
					wrapbad++;
					break;
				}
		}
		if (!wrapbad)
			printf("  ok   ring places run on across the counters' 2^32 wrap for "
			       "every ring size tried\n");
		bad += (unsigned long long)wrapbad;
	}
	printf("walringtest: %d passed, %d failed\n", bad ? 0 : 1, bad ? 1 : 0);
	return bad != 0;
}
