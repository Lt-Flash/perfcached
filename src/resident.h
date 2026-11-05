/*
 * resident.h - S265: make a long-lived buffer resident when it is made.
 *
 * A block of 128 KB or more from malloc/calloc is its own lazily faulted
 * mapping: its pages become resident only as they are first written.  For
 * the daemon's fixed tables - the WAL rings, the pump's stage buffer, the
 * pending table, the reply scratch - that meant RSS climbed with use for
 * hours after every restart until each table had been swept once (~0.7 MB
 * an hour on the test fleet, 10-30 MB in all), and read as a leak.
 *
 * One volatile write per page, not a memset: the compiler folds
 * malloc + memset(0) into calloc, and calloc hands back untouched zero
 * pages - the thing this exists to prevent.  A write to a zero page is a
 * fault the kernel must back, on any kernel (no MADV_POPULATE_WRITE).
 * Callers own the memory; the bytes written are zeros, so a calloc'd
 * buffer stays zeroed and a malloc'd one carries no promise either way.
 */
#ifndef PC_RESIDENT_H
#define PC_RESIDENT_H

#include <stddef.h>
#include <unistd.h>

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
