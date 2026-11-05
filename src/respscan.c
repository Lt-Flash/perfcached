/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * respscan.c - the RESP door's keyspace walk: KEYS, cooperative and
 * bounded by time per turn (pc_keys_chunk, S292; proto.c drives it), and
 * SCAN, about COUNT keys a reply (rh_scan), both over one walk callback.
 * Owns the KEYS turn budget.  Moved from verbs.c unchanged (the 0.7.2
 * split, wave 1 - doc/MODULARITY.md).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "json.h"
#include "store.h"
#include "core/pcache_htable.h"
#include "compat/timer.h"                /* get_ticks */
#include "config.h"                     /* PC_KEYS_TURN_US_DEFAULT */
#include "pcglob.h"
#include "verbs.h"
#include "respout.h"
#include "verbsint.h"

/* RESP walker callbacks: bulk keys into the element scratch.  The RESP
 * door's own walk state - its writer is the RESP text writer, where the
 * native door's (scan_ctx) is the tree's. */
struct resp_scan_ctx {
	struct pc_jw *w;
	const char *pat;
	int patlen;                        /* <0 = no pattern */
	int emitted;
	int scanned;                       /* keys VISITED, pre-filter: the
	                                    * SCAN yield loop bounds on this,
	                                    * never on emitted - a rare MATCH
	                                    * must not walk the table in one
	                                    * call (the stall S40 forbids) */
	int limit;
	int stopped;                       /* limit reached mid-walk */
	unsigned int now;
};

static int resp_keys_cb(const str *key, const str *val, unsigned int exp,
		void *p)
{
	struct resp_scan_ctx *sc = p;

	(void)val;
	sc->scanned++;
	if (exp && exp <= sc->now)
		return 0;
	if (sc->pat && !pc_glob(sc->pat, sc->patlen, key->s, key->len))
		return 0;
	if (sc->w->overflow)
		return -1;
	if (sc->emitted >= sc->limit) {
		sc->stopped = 1;
		return -1;
	}
	sc->emitted++;
	resp_bulk(sc->w, key->s, (size_t)key->len);
	return 0;
}

static long long keys_mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* S292: a turn walks by TIME, not by a bucket count.  1,024 buckets a
 * turn made a walk's turn count scale with the table's SIZE: a sparse
 * 131,072-bucket table took 128 turns, each as long as everything else
 * the worker served in it, so the KEYS caller waited 128 busy turns for
 * a walk that costs well under a millisecond.  Now a turn walks its
 * floor of 1,024 buckets and then goes on in steps of PC_KEYS_STEP while
 * the turn has spent less than [daemon] keys_turn_us - a sparse table ends
 * in a turn or two, and a dense one still yields after about the budget
 * (or the floor, whichever costs more), which is the stall bound the
 * cooperative walk exists for.  The step is 256 rather than 64 because
 * each pc_store_scan call also looks the table up in the registry. */
#define PC_KEYS_FLOOR      1024
#define PC_KEYS_STEP       256
static long long keys_budget_ns = PC_KEYS_TURN_US_DEFAULT * 1000LL;

void pc_keys_set_turn_us(int us)
{
	keys_budget_ns = us > 0 ? us * 1000LL : 0;
}

int pc_keys_chunk(void *htv, unsigned int *cursor, const char *pat,
		int patlen, unsigned int now, struct pc_jw *w, int *emitted,
		int limit, int *limit_hit)
{
	pcache_htable_t *ht = htv;
	struct resp_scan_ctx sc;
	long long t0 = keys_mono_ns();
	unsigned int step = PC_KEYS_FLOOR;

	memset(&sc, 0, sizeof sc);
	sc.w = w;
	sc.pat = patlen >= 0 ? pat : NULL;
	sc.patlen = patlen;
	sc.now = now;
	sc.emitted = *emitted;
	sc.limit = limit;
	for (;;) {
		pc_store_scan(ht, cursor, step, PCACHE_SCAN_NOVAL,  /* S257 */
			resp_keys_cb, &sc);
		if (!*cursor || sc.stopped || w->overflow ||
		        keys_mono_ns() - t0 >= keys_budget_ns)
			break;
		step = PC_KEYS_STEP;
	}
	*emitted = sc.emitted;
	*limit_hit = sc.stopped;
	return *cursor != 0;
}

int rh_scan(struct resp_ctx *c)
{
	struct resp_scan_ctx sc;
	struct pc_jw ew;
	unsigned int cursor;
	long long cur, count = 128;
	char curbuf[16];
	int i, n;

	if (c->nargs < 2 || resp_ll(c->argv[1], c->argl[1], &cur) < 0 ||
	        cur < 0 || cur > 0xFFFFFFFFLL) {
		resp_err(c->out, "invalid cursor");
		return 0;
	}
	memset(&sc, 0, sizeof sc);
	pc_jw_init(&ew, c->scratch, c->scratch_cap);
	sc.w = &ew;
	sc.patlen = -1;
	sc.now = get_ticks();
	sc.limit = 0x7FFFFFFF;
	for (i = 2; i < c->nargs; i++) {
		if (resp_is(c->argv[i], c->argl[i], "MATCH") &&
		        i + 1 < c->nargs && c->argl[i + 1] < 256) {
			sc.pat = c->argv[i + 1];
			sc.patlen = (int)c->argl[i + 1];
			i++;
		} else if (resp_is(c->argv[i], c->argl[i], "COUNT") &&
		        i + 1 < c->nargs) {
			if (resp_ll(c->argv[i + 1], c->argl[i + 1],
			        &count) < 0 || count < 1 ||
			        count > 16384) {
				resp_err(c->out, "invalid COUNT");
				return 0;
			}
			i++;
		} else if (resp_is(c->argv[i], c->argl[i], "TYPE") &&
		        i + 1 < c->nargs) {
			i++;           /* strings only: accept, ignore */
		} else {
			resp_err(c->out, "syntax error");
			return 0;
		}
	}
	if (sc.patlen < 0)
		sc.pat = NULL;
	cursor = (unsigned int)cur;
	/* Yield like Redis: ~COUNT KEYS per reply, not COUNT buckets.
	 * Measured against redis 8 on an identical dataset: it
	 * returns ~COUNT keys per call (exactly 1000.0 at COUNT
	 * 1000), while a single bucket-budget walk returns COUNT x
	 * density - 27% more round trips at density 0.76, which was
	 * the whole remaining sweep deficit.  The loop bounds on
	 * keys SCANNED, never on keys matched, and on a hard bucket
	 * cap, so neither a sparse table nor a rare MATCH can turn
	 * one call into the full-table stall S40 exists to prevent.
	 * (Redis behaves the same way: its MATCH calls can return
	 * empty.) */
	{
		unsigned int visited = 0, budget = (unsigned int)count;
		unsigned int chunk = (unsigned int)count;

		while (visited < budget * 16) {
			pc_store_scan(c->ht, &cursor, chunk,       /* S257 */
				PCACHE_SCAN_NOVAL, resp_keys_cb, &sc);
			visited += chunk;
			if (!cursor || ew.overflow ||
			        sc.scanned >= (int)budget)
				break;
			/* Size the next chunk from the density this
			 * call just measured, so the reply lands NEAR
			 * count instead of overshooting by up to a
			 * whole chunk (~1515 keys for COUNT 1000 at
			 * density 0.76, and the fatter replies cost
			 * more than the saved round trips on some
			 * hosts).  An empty region so far means no
			 * estimate: keep the full chunk and let the
			 * 16x cap bound the walk. */
			if (sc.scanned > 0) {
				unsigned long long want =
					(unsigned long long)
					(budget - (unsigned)sc.scanned)
					* visited / (unsigned)sc.scanned;

				chunk = want ? (want > budget ?
					budget : (unsigned int)want) : 1;
			}
		}
	}
	if (ew.overflow) {
		resp_err(c->out, "reply too large");
		return 0;
	}
	n = snprintf(curbuf, sizeof curbuf, "%u", cursor);
	resp_arr(c->out, 2);
	resp_bulk(c->out, curbuf, (size_t)n);
	resp_arr(c->out, sc.emitted);
	pc_jw_raw(c->out, ew.buf, ew.len);
	return 0;
}
