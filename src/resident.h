/* resident.h - fault a long-lived buffer in when it is made, so RSS does not climb
 * for hours as each fixed table is first swept (it read as a leak). */
#ifndef PC_RESIDENT_H
#define PC_RESIDENT_H

#include <stddef.h>
#include <unistd.h>

/* one volatile write per page, not memset: malloc + memset(0) folds into calloc,
 * whose zero pages stay untouched; the bytes written are zeros */
static inline void pc_touch_pages(void *p, size_t n)
{
	volatile unsigned char *c = p;
	long pg = sysconf(_SC_PAGESIZE);
	size_t i, step = pg > 0 ? (size_t)pg : 4096;

	if (!p)
		return;
	for (i = 0; i < n; i += step)
		c[i] = 0;
	if (n)
		c[n - 1] = 0;                  /* the last, partial page */
}

#endif /* PC_RESIDENT_H */
