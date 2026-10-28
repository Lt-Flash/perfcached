/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * enumwalk.c - the cooperative KEYS walk (S40): a RESP KEYS runs as one
 * bounded chunk per event-loop turn (pc_keys_chunk, S292's time budget),
 * the reply built on the connection's own heap writer and answered whole
 * when the walk ends - then the input that arrived behind it is served.
 * Owns this worker's list of walking connections and the walks' counters.
 * Moved from proto.c unchanged (the 0.7.2 split, wave 1 -
 * doc/MODULARITY.md); the connection's life is proto.c's (protoint.h).
 */
#include <stdio.h>
#include <string.h>

#include "compat/timer.h"                /* get_ticks */
#include "json.h"
#include "store.h"                      /* the slot's publish generation */
#include "verbs.h"                      /* pc_keys_chunk, struct pc_enum_start */
#include "proto.h"
#include "protoint.h"

/* S40: the conns on THIS worker with a cooperative walk in flight */
static __thread struct pc_conn *enum_head;

static void enum_unlink(struct pc_conn *c)
{
	struct pc_conn **pp = &enum_head;

	while (*pp && *pp != c)
		pp = &(*pp)->enum_next;
	if (*pp)
		*pp = c->enum_next;
	c->enum_next = NULL;
}

void enum_abort(struct pc_conn *c)
{
	if (!c->enum_active)
		return;
	pc_jw_free(&c->enum_w);
	c->enum_active = 0;
	enum_unlink(c);
}

/* a walk the verb asked for (@es) begins on @c, joining this worker's
 * list; -1 when its reply buffer cannot be had */
int enum_begin(struct pc_conn *c, const struct pc_enum_start *es)
{
	memset(&c->enum_w, 0, sizeof c->enum_w);
	if (pc_jw_init_heap(&c->enum_w, 64L * 1024) != 0)
		return -1;
	c->enum_active = 1;
	c->enum_cursor = 0;
	c->enum_emitted = 0;
	c->enum_limit = es->limit;
	c->enum_patlen = es->patlen;
	if (es->patlen > 0)
		memcpy(c->enum_pat, es->pat, (size_t)es->patlen);
	c->enum_ht = es->ht;
	/* the slot and its generation as ONE pair (see store.c): a
	 * table swapped out since the verb resolved it gives -1, and
	 * the first step ends the walk as a swap - counted */
	c->enum_pub = 0;
	c->enum_col = pc_store_slot_pub(es->ht, &c->enum_pub);
	c->enum_next = enum_head;
	enum_head = c;
	return 0;
}

int pc_enum_pending(void)
{
	return enum_head != NULL;
}

/* S150 B: walks that ended because the registry moved their table */
static unsigned long long enum_ended_on_swap;
static unsigned long long enum_walks, enum_turns;   /* S292 */

unsigned long long pc_enum_ended_on_swap(void)
{
	return __atomic_load_n(&enum_ended_on_swap, __ATOMIC_RELAXED);
}

void pc_enum_figures(unsigned long long *walks, unsigned long long *turns)
{
	*walks = __atomic_load_n(&enum_walks, __ATOMIC_RELAXED);
	*turns = __atomic_load_n(&enum_turns, __ATOMIC_RELAXED);
}

void pc_enum_step(void)
{
	struct pc_conn *c = enum_head, *next;
	int more, limit_hit;

	/* one chunk per active conn per turn: fairness between walks, and
	 * a bounded stall (a time budget, S292 - see pc_keys_chunk) for
	 * everything else this worker serves */
	for (; c; c = next) {
		next = c->enum_next;
		limit_hit = 0;
		/* S150 B: the pointer was taken on an earlier turn.  If the
		 * registry has published the slot since - a resize swap, a
		 * drop - the table under this walk is retired or retiring:
		 * what was emitted stands, and the walk ends here rather than
		 * read on from a table that may be reused. */
		if (c->enum_col < 0 ||
		        pc_store_pub_gen(c->enum_col) != c->enum_pub) {
			__atomic_fetch_add(&enum_ended_on_swap, 1,
				__ATOMIC_RELAXED);
			more = 0;
		} else {
			more = pc_keys_chunk(c->enum_ht, &c->enum_cursor,
				c->enum_pat, c->enum_patlen, get_ticks(),
				&c->enum_w, &c->enum_emitted, c->enum_limit,
				&limit_hit);
			__atomic_fetch_add(&enum_turns, 1, __ATOMIC_RELAXED);
		}
		if (more && !limit_hit && !c->enum_w.overflow)
			continue;              /* walk on next turn */
		__atomic_fetch_add(&enum_walks, 1, __ATOMIC_RELAXED);

		/* done (or stopped): assemble the one ordered reply */
		if (c->enum_w.overflow) {
			static const char msg[] = "-ERR reply too large\r\n";

			if (out_append(c, msg, sizeof msg - 1) < 0)
				goto drop;
		} else {
			char hdr[24];
			int hn = snprintf(hdr, sizeof hdr, "*%d\r\n",
				c->enum_emitted);

			if (out_append(c, hdr, (size_t)hn) < 0 ||
			        out_append(c, c->enum_w.buf,
			                c->enum_w.len) < 0)
				goto drop;
		}
		enum_abort(c);                 /* frees the writer, unlinks */
		/* input that arrived behind the KEYS is still buffered:
		 * serve it now, in order, then flush the lot */
		if (process_plaintext(c) < 0) {
			c->why = "protocol error";
			goto drop;
		}
		if (conn_flush(c) < 0) {
			c->why = "write error";
			goto drop;
		}
		if (c->close_after && !c->out_len && !c->wire_len)
			goto drop;
		continue;
drop:
		conn_destroy(c);
	}
}
