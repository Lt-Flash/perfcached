/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* respdoor.c - RESP door framing: one multibulk or inline command from the input,
 * run through pc_verb_resp, timed and counted, parked, or begun as a walk */
#include <string.h>

#include "compat/timer.h"                /* get_ticks */
#include "json.h"
#include "obs.h"
#include "verbs.h"                      /* pc_verb_resp */
#include "proto.h"
#include "protoint.h"
#include "mgetfan.h"

/* ---- RESP dialect --------------------------------------------------------
 * same consumed/0/-1 contract as bin_frame; compaction is the caller's */

int resp_ll_span(const char *p, size_t n, long long *out)
{
	long long v = 0;
	size_t i = 0;
	int neg = 0;

	if (n && (p[0] == '-' || p[0] == '+')) {
		neg = p[0] == '-';
		i = 1;
	}
	if (i == n || n > 17)
		return -1;
	for (; i < n; i++) {
		if (p[i] < '0' || p[i] > '9')
			return -1;
		v = v * 10 + (p[i] - '0');
	}
	*out = neg ? -v : v;
	return 0;
}

static int resp_execute_inner(struct pc_conn *c, char **argv,
		size_t *argl, int nargs)
{
	char *resbuf = scratch_buf(0), *scratch = scratch_buf(1);
	struct pc_jw rw;
	const char *col = NULL, *key = NULL;
	size_t collen = 0, klen = 0;
	unsigned int park_req = 0;
	struct pc_enum_start es;
	int quit = 0;

	if (!resbuf || !scratch)
		return -1;
	pc_jw_init(&rw, resbuf, JW_REPLY_CAP);
	memset(&es, 0, sizeof es);
	ev_origin(c, "resp");
	if (pc_verb_resp(argv, argl, nargs, &rw, scratch, JW_REPLY_CAP,
	        &park_req, &col, &collen, &key, &klen,
	        c->resp_col, &c->resp_collen, &quit, &c->resp_authed,
	        &es, c->obs, c) < 0) {
		pc_mget_free(pc_mget_take());  /* never the next command's */
		return -1;
	}
	if (es.start) {
		if (enum_begin(c, &es) != 0) {
			static const char msg[] =
				"-ERR out of memory starting the walk\r\n";

			return out_append(c, msg, sizeof msg - 1);
		}
		return 0;                      /* reply comes chunk by chunk */
	}
	{
		/* an MGET across nodes replies when the lookups answer, and what was
		 * pipelined behind it waits (RESP replies are ordered) */
		struct pc_mget *mg = pc_mget_take();

		if (mg) {
			static const char oom[] = "-ERR out of memory\r\n";

			pc_jw_free(&rw);
			if (pc_mget_attach(mg, c, PC_MGET_DOOR_RESP, NULL) == 0) {
				c->park_hold = 1;
				return 0;
			}
			pc_mget_free(mg);
			return out_append(c, oom, sizeof oom - 1);
		}
	}
	if (park_req && col && key &&
	        park_add(c, park_req, NULL, col, collen, key, klen,
	                PARK_RESP, 0) == 0) {
		/* the reply comes on completion; what was pipelined behind it waits */
		c->park_hold = 1;
		return 0;
	}
	if (rw.overflow) {
		static const char msg[] = "-ERR reply too large\r\n";

		pc_jw_free(&rw);               /* no-op unless it grew one */
		if (out_append(c, msg, sizeof msg - 1) < 0)
			return -1;
		return 0;
	}
	if (quit)
		c->close_after = 1;
	/* a verb may have swapped in a heap buffer of its own (CLUSTER
	 * SLOTS); out_append copies, so it can be released right after */
	{
		int rc = out_append(c, rw.buf, rw.len);

		pc_jw_free(&rw);
		return rc;
	}
}


/* a row is named only by a command the door has: anything else is the one row "unknown" */
static void *resp_obs_row(const char *name, size_t nlen)
{
	if (!pc_verb_resp_known(name, nlen)) {
		name = "unknown";
		nlen = 7;
	}
	return pc_obs_cmd_slot(name, nlen);
}

/* the stats row from the last four names this worker ran, matched on the bytes
 * as sent, so pc_obs_cmd_slot's fold and hash run only for a new name or case */
static void *resp_obs_slot(const char *name, size_t nlen)
{
	static __thread char nm[4][16];
	static __thread unsigned char nl[4], next;
	static __thread void *slot[4];
	unsigned int i;

	if (nlen >= sizeof nm[0])
		return resp_obs_row(name, nlen);
	for (i = 0; i < 4; i++)
		if (nl[i] == nlen && slot[i] && memcmp(nm[i], name, nlen) == 0)
			return slot[i];
	i = next++ & 3;
	memcpy(nm[i], name, nlen);
	nl[i] = (unsigned char)nlen;
	slot[i] = resp_obs_row(name, nlen);
	return slot[i];
}

/* a command's clock starts where the previous one in the drain stopped: one clock
 * read per command (costly on a VM); a parked command is counted on its re-run */
static int resp_execute(struct pc_conn *c, char **argv, size_t *argl,
		int nargs)
{
	unsigned long long t0 = obs_carry ? obs_carry : pc_obs_usec_now(), dt;
	size_t o0 = c->out_len;            /* where this reply starts */
	int rc = resp_execute_inner(c, argv, argl, nargs);

	if (nargs > 0) {
		obs_carry = pc_obs_usec_now();
		dt = obs_carry - t0;
		if (c->up_hold)
			return rc;                     /* parked: counted on its re-run */
		pc_obs_cmd_at(c->resp_authed ? resp_obs_slot(argv[0], argl[0])
			: resp_obs_slot("unknown", 7), dt);
		pc_obs_conn_touch(c->obs, argv[0], argl[0], get_ticks());
		pc_obs_slow(argv, argl, nargs, dt, c->obs);
		if (pc_obs_qlog_on())
			pc_obs_qlog("resp", c->obs, argv[0], argl[0], NULL, 0,
				nargs > 1 ? argv[1] : NULL, nargs > 1 ? argl[1] : 0,
				rc < 0 || (c->out_len > o0 && c->out[o0] == '-') ? "err"
				: c->out_len - o0 >= 3 && (memcmp(c->out + o0, "$-1", 3) == 0
				        || memcmp(c->out + o0, "*-1", 3) == 0) ? "miss"
				: c->out_len > o0 && c->out[o0] == '$' ? "hit" : "ok", dt);
	}
	return rc;
}

int resp_one(struct pc_conn *c, size_t off)
{
	char *base = c->in + off;
	size_t avail = c->in_len - off, pos;
	char *argv[PC_RESP_MAXARGS];
	size_t argl[PC_RESP_MAXARGS];
	long long nargs, blen;
	int i;

	if (base[0] != '*') {
		/* inline command: one line, split on blanks */
		char *nl = memchr(base, '\n', avail);
		size_t n, s = 0;
		int na = 0;

		if (!nl) {
			if (avail > 65536)
				return -1;     /* runaway inline line */
			return 0;
		}
		n = (size_t)(nl - base);
		if (n && base[n - 1] == '\r')
			n--;
		while (s < n && na < PC_RESP_MAXARGS) {
			size_t e;

			while (s < n && (base[s] == ' ' || base[s] == '\t'))
				s++;
			if (s == n)
				break;
			e = s;
			while (e < n && base[e] != ' ' && base[e] != '\t')
				e++;
			argv[na] = base + s;
			argl[na] = e - s;
			na++;
			s = e;
		}
		if (na && resp_execute(c, argv, argl, na) < 0)
			return -1;
		if (c->up_hold)
			return 0;          /* parked - left in the buffer */
		c->up_verdict = 0;
		c->up_lost = 0;
		return (int)((size_t)(nl - base) + 1);
	}

	/* multibulk: *N\r\n then N x ($len\r\n bytes \r\n) */
	{
		char *nl = memchr(base, '\n', avail < 16 ? avail : 16);

		if (!nl)
			return avail >= 16 ? -1 : 0;
		if (resp_ll_span(base + 1, (size_t)(nl - base) -
		        (nl[-1] == '\r' ? 2 : 1), &nargs) < 0 ||
		        nargs < 0 || nargs > PC_RESP_MAXARGS)
			return -1;
		pos = (size_t)(nl - base) + 1;
	}
	for (i = 0; i < (int)nargs; i++) {
		char *nl;

		if (pos >= avail)
			return 0;
		if (base[pos] != '$')
			return -1;
		nl = memchr(base + pos, '\n',
			(avail - pos) < 16 ? (avail - pos) : 16);
		if (!nl)
			return (avail - pos) >= 16 ? -1 : 0;
		if (resp_ll_span(base + pos + 1, (size_t)(nl - (base + pos)) -
		        (nl[-1] == '\r' ? 2 : 1), &blen) < 0 ||
		        blen < 0 || blen > (long long)PC_MAX_REQ)
			return -1;
		pos = (size_t)(nl - base) + 1;
		if (avail - pos < (size_t)blen + 2)
			return 0;              /* body + CRLF not here yet */
		argv[i] = base + pos;
		argl[i] = (size_t)blen;
		pos += (size_t)blen;
		if (base[pos] == '\r')
			pos++;
		if (base[pos] != '\n')
			return -1;
		pos++;
	}
	if (nargs && resp_execute(c, argv, argl, (int)nargs) < 0)
		return -1;
	if (c->up_hold)
		return 0;                      /* parked - left in the buffer */
	c->up_verdict = 0;
	c->up_lost = 0;
	return (int)pos;
}
