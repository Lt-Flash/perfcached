/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * alerts.c - S352b: the node's own alert conditions (alerts.h).
 *
 * Every condition is evaluated on the maintenance thread once a second
 * against figures the daemon already keeps; nothing here touches a
 * worker.  A condition must HOLD for its hold time before it is raised
 * (an fsync average above 4x the probe for five minutes, an upstream down
 * for a minute) and stay AWAY for its clear time before it is cleared, so
 * a flapping figure does not mail every second.  Events - records the WAL
 * lost, writes refused for want of memory - raise on the second they
 * happen and clear after five quiet minutes.
 *
 * One mutex guards the keys, the ring and the digest; the tick holds it
 * throughout (a few microseconds), /stats takes it to copy the ring out.
 *
 * S352c (DESIGN 12le): the fleet side.  A node that cannot mail - no
 * [mail] of its own - hands each alert it would mail to the ELECTED mailer
 * (pc_cluster_mailer: the lowest ready node id that can mail), which mails
 * it naming the origin; the cluster thread hands relays in through a small
 * inbox of their own, drained here.  FLEET events - a member silent, failed
 * or gone, one missing from the fleet's size, the fleet below a majority -
 * are raised by every member that sees them and mailed by the elected
 * mailer alone, so a fleet sends one mail per event, not one per member.
 */
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "compat/dprint.h"
#include "alerts.h"
#include "cluster.h"
#include "mail.h"
#include "ptree.h"
#include "recover.h"
#include "upstream.h"
#include "version.h"
#include "wal.h"
#include "core/pcache_arena.h"

#define KEYLEN 48
#define MAILW  40                     /* the ring's "what the mail did" */
#define INBOX  32                     /* relays waiting for the tick */
#define PEND_MS 10000                 /* a raise waits this long for a mailer */
#define PEND_WORD "waiting for a mailer"
#define S(x) ((long long)(x) * 1000)  /* seconds -> ms */

_Static_assert(KEYLEN == sizeof ((struct pc_alert_relay *)0)->key,
	"a relayed key must fit a key here");

struct akey {
	char key[KEYLEN];
	int level;                         /* of the active raise */
	int active;
	int raise_mailed;                  /* so its clear is mailed too */
	long long since_wall;              /* raised at (wall ms) */
	long long true_since, false_since; /* monotonic ms; 0 = not in that phase */
	long long last_mail;               /* monotonic ms of the last mailed raise */
	int fleet;                         /* S352c: a fleet event - the elected
	                                    * mailer's to send */
	int oneoff;                        /* an event (event()): nothing clears it */
	long long pend_since;              /* S352c: waiting for a mailer since */
	int pend_slot;                     /* and its ring entry */
	char text[PC_ALERT_TEXT];
};

struct aring {
	long long ts;                      /* wall ms */
	int level, cleared;
	int from;                          /* S352c: 0, or the node that relayed it */
	char key[KEYLEN];
	char text[PC_ALERT_TEXT];
	char mail[MAILW];                  /* what became of it, by mail */
};

static struct {
	pthread_mutex_t mx;
	struct akey k[PC_ALERT_KEYS];
	int nk;
	struct aring r[PC_ALERT_RING];
	int rnext, rn;
	/* the digest being collected */
	char *dbuf;
	size_t dlen, dcap;
	int dn, dlevel, dcleared_only;
	long long ddeadline;
	char dfirst[PC_ALERT_TEXT];
	int dfrom, drelayed;               /* S352c: the first line's origin; relayed lines */
	/* [mail]'s rules */
	int mail_level;
	long long min_interval, digest;
	int fast;                          /* PERFCACHED_ALERT_FAST */
	/* what the previous tick saw */
	unsigned long long wal_dropped, wal_overruns;
	unsigned long nomem;
	int primed, started_said;
	unsigned long long fs_n[61], fs_sum[61];  /* fsync totals, 1 per second */
	int fs_i, fs_have;
	char host[128];
	/* S352c */
	int mailer, mailer_self;           /* the elected mailer, this tick */
	unsigned int relay_seq;
	unsigned long long relayed_in, relayed_out, relay_dropped;
	unsigned long long base_in, base_out, base_drop;   /* S358 */
	long long nmr_said;                /* "no mailer reachable" last logged */
	unsigned long nmr_quiet;           /* and since then, unlogged */
	int fleet_seen;                    /* the most members listed */
	struct pc_member *mem;             /* pc_cluster_members' scratch */
} A = { .mx = PTHREAD_MUTEX_INITIALIZER };

/* S352c: relays the cluster thread heard, for the tick.  A lock of their
 * own, never held while anything else is called, so the cluster thread
 * cannot wait on the tick and the tick never waits on it for long. */
static struct {
	pthread_mutex_t mx;
	struct pc_alert_relay q[INBOX];
	int n;
	unsigned long long dropped;
} IB = { .mx = PTHREAD_MUTEX_INITIALIZER };

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long wall_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* a hold or clear time, shortened under PERFCACHED_ALERT_FAST */
static long long T(long long ms)
{
	return A.fast > 1 ? ms / A.fast : ms;
}

static const char *lvl_name(int l)
{
	return l >= PC_AL_CRITICAL ? "critical" : "warning";
}

void pc_alerts_init(const struct pc_config *cfg)
{
	const char *f = getenv("PERFCACHED_ALERT_FAST");

	A.mail_level = cfg->mail.level ? cfg->mail.level : PC_AL_WARNING;
	A.min_interval = S(cfg->mail.min_interval_s);
	A.digest = S(cfg->mail.digest_s);
	A.fast = f ? atoi(f) : 0;
	if (gethostname(A.host, sizeof A.host - 1) != 0)
		snprintf(A.host, sizeof A.host, "perfcached");
	/* a relay's sequence starts somewhere a restart is unlikely to have
	 * been lately, so the mailer's duplicate check never takes a new
	 * relay for an old one */
	A.relay_seq = (unsigned int)wall_ms();
	if (A.fast > 1)
		LM_NOTICE("alerts: PERFCACHED_ALERT_FAST=%d - hold and clear "
			"times divided by %d (a test setting)\n", A.fast, A.fast);
}

static struct aring *ring_next(void)
{
	struct aring *e = &A.r[A.rnext];

	A.rnext = (A.rnext + 1) % PC_ALERT_RING;
	if (A.rn < PC_ALERT_RING)
		A.rn++;
	memset(e, 0, sizeof *e);
	e->ts = wall_ms();
	return e;
}

static void ring_add(const struct akey *k, int cleared, const char *mail)
{
	struct aring *e = ring_next();

	e->level = k->level;
	e->cleared = cleared;
	snprintf(e->key, sizeof e->key, "%s", k->key);
	snprintf(e->text, sizeof e->text, "%s", k->text);
	snprintf(e->mail, sizeof e->mail, "%s", mail);
}

/* @from: 0 for this node's own line, else the node that relayed it */
static void digest_add(int level, int cleared, const char *text, int from)
{
	char line[PC_ALERT_TEXT + 64], tb[16];
	time_t t = time(NULL);
	struct tm tm;
	int n;

	localtime_r(&t, &tm);
	strftime(tb, sizeof tb, "%H:%M:%S", &tm);
	n = snprintf(line, sizeof line, "%s  %-8s  %-7s  %s\n", tb,
		lvl_name(level), cleared ? "cleared" : "RAISED", text);
	if (n < 0)
		return;
	if (A.dlen + (size_t)n + 1 > A.dcap) {
		size_t nc = A.dcap ? A.dcap * 2 : 2048;
		char *nb;

		while (nc < A.dlen + (size_t)n + 1)
			nc *= 2;
		if (nc > PC_MAIL_BODY_MAX - 1024)
			return;                    /* the mail is full: the ring has it */
		nb = realloc(A.dbuf, nc);
		if (!nb)
			return;
		A.dbuf = nb;
		A.dcap = nc;
	}
	memcpy(A.dbuf + A.dlen, line, (size_t)n + 1);
	A.dlen += (size_t)n;
	if (from)
		A.drelayed++;
	if (!A.dn++) {
		A.ddeadline = mono_ms() + A.digest;
		snprintf(A.dfirst, sizeof A.dfirst, "%s", text);
		A.dlevel = level;
		A.dcleared_only = cleared;
		A.dfrom = from;
	} else {
		if (level > A.dlevel)
			A.dlevel = level;
		A.dcleared_only &= cleared;
	}
}

static void digest_flush(int force)
{
	struct pc_cl_stats cs;
	char subj[PC_MAIL_SUBJ_MAX], node[32] = "", self[32] = "", *body;
	size_t bl;

	if (!A.dn || (!force && mono_ms() < A.ddeadline))
		return;
	pc_cluster_get_stats(&cs);
	if (cs.enabled) {
		snprintf(node, sizeof node, "node %d: ", (int)cs.node_id);
		snprintf(self, sizeof self, ", node %d", (int)cs.node_id);
	}
	/* S352c: one relayed alert is ABOUT its origin, which its line names
	 * ("node 3 (host): ..."), so the subject leads with that */
	if (A.dn == 1 && A.dfrom)
		node[0] = 0;
	if (A.dn == 1)
		snprintf(subj, sizeof subj, "[%s] %s%.120s", A.dcleared_only
			? "cleared" : lvl_name(A.dlevel), node, A.dfirst);
	else
		snprintf(subj, sizeof subj, "[%s] %s%d alerts - %.120s",
			A.dcleared_only ? "cleared" : lvl_name(A.dlevel), node,
			A.dn, A.dfirst);
	bl = A.dlen + 640;
	body = malloc(bl);
	if (body) {
		snprintf(body, bl, "perfcached %s (%s) on %s%s, state %s\n\n%s\n"
			"%s/stats `alerts` on this node keeps the last %d.\n",
			PC_VERSION, PC_BUILD_REV, A.host, self,
			pc_node_state_name(pc_node_state()), A.dbuf ? A.dbuf : "",
			A.drelayed ? "A line naming another node came from that node, "
			"which has no [mail] of its own.\n" : "", PC_ALERT_RING);
		if (pc_mail_send(subj, body) != 0)
			LM_WARN("alerts: the mail '%s' could not be queued\n", subj);
		free(body);
	}
	A.dn = 0;
	A.dlen = 0;
	A.dfrom = 0;
	A.drelayed = 0;
	if (A.dbuf)
		A.dbuf[0] = 0;
}

/* S352c: an alert that should go out and that no member can mail - said
 * in the log when it starts and at most every ten minutes while it lasts
 * (the alert's own line is there already), kept in the ring.  The tick
 * starts it again whenever a mailer is reachable. */
static void unreachable(const char *text, char *w, size_t wn)
{
	long long now = mono_ms();

	snprintf(w, wn, "no mailer reachable");
	if (A.nmr_said && now - A.nmr_said < S(600)) {
		A.nmr_quiet++;
		return;
	}
	if (A.nmr_quiet)
		LM_WARN("alerts: no mailer reachable - '%s' is kept in this "
			"node's ring only, as were %lu more since this was last "
			"said: no live, ready member has a [mail] section\n", text,
			A.nmr_quiet);
	else
		LM_WARN("alerts: no mailer reachable - '%s' is kept in this "
			"node's ring only: no live, ready member has a [mail] "
			"section\n", text);
	A.nmr_said = now;
	A.nmr_quiet = 0;
}

/* S352c: hand a raise or a clear to the elected mailer; 0 when it went */
static int relay_out(const struct akey *k, int cleared, char *w, size_t wn)
{
	struct pc_alert_relay r;

	memset(&r, 0, sizeof r);
	r.seq = ++A.relay_seq;
	r.level = k->level;
	r.cleared = cleared;
	snprintf(r.key, sizeof r.key, "%s", k->key);
	snprintf(r.host, sizeof r.host, "%.63s", A.host);
	snprintf(r.text, sizeof r.text, "%s", k->text);
	if (pc_cluster_alert_relay_send(A.mailer, &r) == 0) {
		A.relayed_out++;
		snprintf(w, wn, "relayed to node %d", A.mailer);
		return 0;
	}
	A.relay_dropped++;
	snprintf(w, wn, "relay to node %d failed", A.mailer);
	return -1;
}

/* S352c: one raise or clear that is due by mail, out the way this node
 * has: its own mailer, the elected mailer by relay, or - for a fleet event
 * on any node but the elected mailer - nothing, the mailer has it.  @w the
 * ring's words.  1 when it went, 0 when no mailer is reachable now, -1
 * when it cannot go (a lone node without [mail], a relay that failed). */
static int deliver(struct akey *k, int cleared, char *w, size_t wn)
{
	if (k->fleet && !(A.mailer_self && pc_mail_ready())) {
		if (!A.mailer)
			return 0;
		snprintf(w, wn, "mailed by node %d", A.mailer);
		return 1;
	}
	if (pc_mail_ready()) {
		digest_add(k->level, cleared, k->text, 0);
		snprintf(w, wn, "mailed");
		return 1;
	}
	if (!pc_cluster_enabled()) {
		snprintf(w, wn, "no mailer");          /* S352b: a lone node */
		return -1;
	}
	if (!A.mailer)
		return 0;
	return relay_out(k, cleared, w, wn) == 0 ? 1 : -1;
}

/* where a raise goes by mail; @w gets the ring's words for it */
static void mail_raise(struct akey *k, long long now, int worse, char *w,
		size_t wn)
{
	int r;

	/* a fleet event is the elected mailer's to send, by ITS rules - on
	 * every other member only whose it is is recorded */
	if (!k->fleet || (A.mailer_self && pc_mail_ready())) {
		if (k->level < A.mail_level) {
			snprintf(w, wn, "below level");
			return;
		}
		/* min_interval quiets a REPEAT; a condition that got worse
		 * (healing -> failed) is news and goes out whatever the
		 * window says */
		if (!worse && k->last_mail && now - k->last_mail < A.min_interval) {
			snprintf(w, wn, "suppressed");
			return;
		}
	}
	r = deliver(k, 0, w, wn);
	if (r == 0) {
		/* S352c: a node just started, or just joined, has not heard
		 * the fleet's beats yet - its own start is raised the second
		 * it is ready.  The raise waits PEND_MS for a mailer, and the
		 * ring entry says what became of it then (pend_tick). */
		k->pend_since = now;
		snprintf(w, wn, "%s", PEND_WORD);
	} else if (r > 0) {
		k->last_mail = now;
		k->raise_mailed = 1;
	}
}

/* the clear of a raise that went out goes the same way - at once */
static void mail_clear(struct akey *k, char *w, size_t wn)
{
	if (deliver(k, 1, w, wn) == 0)
		unreachable(k->text, w, wn);
}

/* S352c: the ring entry of a raise that waited, told what became of it */
static void pend_word(const struct akey *k, const char *w)
{
	struct aring *e = &A.r[k->pend_slot];

	if (!e->cleared && !e->from && !strcmp(e->key, k->key) &&
	        !strcmp(e->mail, PEND_WORD))
		snprintf(e->mail, sizeof e->mail, "%s", w);
}

/* S352c: the raises waiting for a mailer - out as soon as one is heard,
 * "no mailer reachable" once PEND_MS has passed without one */
static void pend_tick(void)
{
	long long now = mono_ms();
	char w[MAILW];
	int i, r;

	for (i = 0; i < A.nk; i++) {
		struct akey *k = &A.k[i];

		if (!k->pend_since)
			continue;
		if (!k->active && !k->oneoff) {
			/* cleared while it waited: nothing left to send */
			pend_word(k, "no mailer reachable");
			k->pend_since = 0;
			continue;
		}
		r = deliver(k, 0, w, sizeof w);
		if (r == 0) {
			if (now - k->pend_since < PEND_MS)
				continue;
			unreachable(k->text, w, sizeof w);
		} else if (r > 0) {
			k->last_mail = now;
			k->raise_mailed = 1;
		}
		pend_word(k, w);
		k->pend_since = 0;
	}
}

static struct akey *key_find(const char *key, int create)
{
	int i;

	for (i = 0; i < A.nk; i++)
		if (!strcmp(A.k[i].key, key))
			return &A.k[i];
	if (!create)
		return NULL;
	for (i = 0; i < A.nk; i++)             /* reuse a long-quiet slot */
		if (!A.k[i].active && !A.k[i].true_since && !A.k[i].pend_since &&
		    (!A.k[i].last_mail || mono_ms() - A.k[i].last_mail > A.min_interval))
			break;
	if (i == A.nk) {
		if (A.nk == PC_ALERT_KEYS)
			return NULL;
		A.nk++;
	}
	memset(&A.k[i], 0, sizeof A.k[i]);
	snprintf(A.k[i].key, sizeof A.k[i].key, "%s", key);
	A.k[i].fleet = !strncmp(key, "fleet.", 6);     /* S352c */
	return &A.k[i];
}

static void raise_now(struct akey *k, int level, const char *text, long long now)
{
	int worse = k->active && level > k->level;
	char w[MAILW];

	if (k->pend_since) {                   /* said again before it went */
		pend_word(k, "superseded");
		k->pend_since = 0;
	}

	k->active = 1;
	k->level = level;
	k->since_wall = wall_ms();
	snprintf(k->text, sizeof k->text, "%s", text);
	/* a one-off tells of something past - the node's own start after an
	 * unclean stop, which it has recovered from: WARNING in the log, its
	 * level in the text, so a CRITICAL line still means a failure NOW */
	if (level >= PC_AL_CRITICAL && !k->oneoff)
		LM_CRIT("alert: %s\n", text);
	else
		LM_WARN("alert%s: %s\n", level >= PC_AL_CRITICAL ? " (critical)"
			: "", text);
	mail_raise(k, now, worse, w, sizeof w);
	ring_add(k, 0, w);
	if (k->pend_since)
		k->pend_slot = (A.rnext - 1 + PC_ALERT_RING) % PC_ALERT_RING;
}

/* one condition, this second: @on with @level and @text; raised once it
 * has held @hold ms, cleared once it has been away @clear ms */
static void cond(const char *key, int on, int level, long long hold,
		long long clear, const char *text)
{
	long long now = mono_ms();
	struct akey *k = key_find(key, on);

	if (!k)
		return;
	if (on) {
		k->false_since = 0;
		if (!k->true_since)
			k->true_since = now;
		if (k->active) {
			if (level != k->level)     /* worse, or better: said again */
				raise_now(k, level, text, now);
		} else if (now - k->true_since >= T(hold)) {
			raise_now(k, level, text, now);
		}
		return;
	}
	k->true_since = 0;
	if (!k->active)
		return;
	if (!k->false_since)
		k->false_since = now;
	if (now - k->false_since < T(clear))
		return;
	k->active = 0;
	LM_NOTICE("alert cleared: %s\n", k->text);
	if (k->raise_mailed) {                  /* its clear goes the same way */
		char w[MAILW];

		mail_clear(k, w, sizeof w);
		ring_add(k, 1, w);
	} else {
		ring_add(k, 1, "-");
	}
	k->raise_mailed = 0;
}

/* a one-off: raised, recorded, mailed - nothing to clear */
static void event(const char *key, int level, const char *text)
{
	struct akey *k = key_find(key, 1);

	if (!k)
		return;
	k->oneoff = 1;
	raise_now(k, level, text, mono_ms());
	k->active = 0;
	k->raise_mailed = 0;
}

static void check_node(void)
{
	int st = pc_node_state();
	char t[PC_ALERT_TEXT];

	if (st == PC_NST_FAILED) {
		snprintf(t, sizeof t, "node FAILED: %s",
			pc_node_reason_text(pc_node_reason()));
		cond("node.state", 1, PC_AL_CRITICAL, 0, 0, t);
	} else if (st == PC_NST_HEALING) {
		snprintf(t, sizeof t, "node HEALING: %s",
			pc_node_reason_text(pc_node_reason()));
		cond("node.state", 1, PC_AL_WARNING, 0, 0, t);
	} else {
		cond("node.state", 0, 0, 0, 0, "");
	}
	/* the node's own start, once it is ready to serve */
	if (!A.started_said && st == PC_NST_READY) {
		long w = pc_recovered_wal_applied();

		A.started_said = 1;
		if (w > 0) {
			snprintf(t, sizeof t, "started (perfcached %s) after an "
				"UNCLEAN stop - %ld WAL records were replayed above "
				"the last snapshot", PC_VERSION, w);
			event("node.start", PC_AL_CRITICAL, t);
		} else {
			snprintf(t, sizeof t, "started (perfcached %s), ready",
				PC_VERSION);
			event("node.start", PC_AL_WARNING, t);
		}
	}
}

static void check_wal(void)
{
	struct pc_wal_stats ws;
	struct pc_wal_fsync_obs ob;
	char t[PC_ALERT_TEXT];
	unsigned long long dd, dov;
	int j;

	if (!pc_wal_enabled())
		return;
	pc_wal_get_stats(&ws);
	dd = ws.dropped - A.wal_dropped;
	dov = ws.overruns - A.wal_overruns;
	if (A.primed) {
		snprintf(t, sizeof t, "the WAL lost records: %llu dropped, %llu "
			"overrun in the last second", dd, dov);
		cond("wal.loss", dd || dov, PC_AL_CRITICAL, 0, S(300), t);
	}
	A.wal_dropped = ws.dropped;
	A.wal_overruns = ws.overruns;

	/* the fsync mean over the last minute against what the start-up
	 * probe measured - S351's window is five minutes; a minute here,
	 * held for five, says "it has been slow for five minutes" */
	pc_wal_fsync_observed(&ob);
	A.fs_n[A.fs_i] = ob.fsync_n;
	A.fs_sum[A.fs_i] = ob.sum_us;
	j = (A.fs_i + 1) % 61;                 /* the sample 60 s ago */
	A.fs_i = j;
	if (A.fs_have < 61)
		A.fs_have++;
	if (A.fs_have == 61 && ob.probe_p50_us) {
		unsigned long long n = ob.fsync_n - A.fs_n[j];
		unsigned long long avg = n ? (ob.sum_us - A.fs_sum[j]) / n : 0;

		snprintf(t, sizeof t, "WAL fsync averages %llu us over the last "
			"minute - %llux the %llu us measured at start",
			avg, ob.probe_p50_us ? avg / ob.probe_p50_us : 0,
			ob.probe_p50_us);
		cond("wal.fsync", n >= 5 && avg > 4 * ob.probe_p50_us,
			PC_AL_WARNING, S(300), S(60), t);
	}
}

static void check_memory(void)
{
	struct pcache_arena_pressure pr;
	unsigned long mx = pcache_arena_max_bytes, held, d;
	char t[PC_ALERT_TEXT];

	pcache_arena_pressure(&pr);
	d = pr.refused - A.nomem;
	if (A.primed) {
		snprintf(t, sizeof t, "writes refused, the arena is full: %lu in "
			"the last second", d);
		cond("memory.nomem", d > 0, PC_AL_CRITICAL, 0, S(300), t);
	}
	A.nomem = pr.refused;
	if (!mx)
		return;
	held = pcache_arena_held_bytes();
	{
		unsigned long freep = held >= mx ? 0 : (mx - held) * 100 / mx;

		snprintf(t, sizeof t, "the arena holds %lu of its %lu MB - %lu%% "
			"left%s", held >> 20, mx >> 20, freep,
			freep < 1 ? ", at its ceiling" : "");
		cond("memory.arena", freep < 10, freep < 1 ? PC_AL_CRITICAL
			: PC_AL_WARNING, S(30), S(60), t);
	}
}

static void check_upstreams(void)
{
	struct pc_up_health h;
	char key[KEYLEN], t[PC_ALERT_TEXT];
	int i, n = pc_upstream_count();

	for (i = 0; i < n; i++) {
		int down, trk;

		if (pc_upstream_health(i, &h) != 0)
			continue;
		down = h.conns_up == 0;
		trk = h.tracking_wanted && !h.tracking_on;
		snprintf(key, sizeof key, "upstream.%s", h.col);
		if (down)
			snprintf(t, sizeof t, "the read-through upstream %s:%d of "
				"collection %s is unreachable - misses are answered "
				"as its on_error says", h.host, h.port, h.col);
		else
			snprintf(t, sizeof t, "tracking with the read-through "
				"upstream %s:%d of collection %s is off - copies are "
				"believed for upstream_negative_ms only",
				h.host, h.port, h.col);
		cond(key, down || trk, PC_AL_WARNING, S(60), S(60), t);
	}
}

/* S352c: the FLEET as this node sees it - pc_cluster_members(), the list
 * /members and the page's fleet grid read.  Each member that went silent,
 * failed, stalled, left or was dropped is an alert of its own, a warning;
 * members missing from the fleet's size are one; fewer ready than a
 * majority of it is critical.  The fleet's size is S341's, the header's:
 * [cluster] expect when set, else the most members this node has listed.
 * Raised on every member that sees it, mailed by the elected mailer only. */
static void check_fleet(void)
{
	char key[KEYLEN], t[PC_ALERT_TEXT], ab[INET_ADDRSTRLEN];
	int i, j, n, size, ready = 0, expect;

	/* a node that is not ready sees a fleet it is still joining */
	if (!pc_cluster_enabled() || pc_node_state() != PC_NST_READY)
		return;
	if (!A.mem && !(A.mem = malloc(sizeof *A.mem * PC_CL_MAXMEMBERS)))
		return;
	n = pc_cluster_members(A.mem, PC_CL_MAXMEMBERS);
	if (n < 1 || A.mem[0].node <= 0)
		return;                            /* no id yet */
	if (n > A.fleet_seen)
		A.fleet_seen = n;
	expect = pc_cluster_expect();
	size = expect > A.fleet_seen ? expect : A.fleet_seen;
	for (i = 0; i < n; i++) {
		const struct pc_member *m = &A.mem[i];
		const char *why;
		long long hold = S(30);
		int bad = 1;

		if (m->is_self) {
			ready++;                       /* READY, by the gate above */
			continue;
		}
		if (!inet_ntop(AF_INET, &m->addr, ab, sizeof ab))
			snprintf(ab, sizeof ab, "?");
		if (m->gone_s >= 0) {
			why = pc_cluster_gone_reason(m->gone_why);
			if (!strcmp(why, "goodbye")) {
				/* a stop someone made: a restart is back inside
				 * the minute and says so itself (node.start) */
				hold = S(60);
				snprintf(t, sizeof t, "member node %d (%s) left - "
					"it said goodbye", m->node, ab);
			} else if (!strcmp(why, "forgotten")) {
				snprintf(t, sizeof t, "member node %d (%s) was "
					"dropped - refused by this fleet", m->node, ab);
			} else if (!strcmp(why, "silent")) {
				snprintf(t, sizeof t, "member node %d (%s) went "
					"silent and was purged - it did not say goodbye",
					m->node, ab);
			} else {
				snprintf(t, sizeof t, "member node %d (%s) left",
					m->node, ab);
			}
		} else if (!m->live) {
			snprintf(t, sizeof t, "member node %d (%s) is silent - "
				"no heartbeat heard", m->node, ab);
		} else if (m->state == PC_NST_FAILED || m->state == PC_NST_STALLED) {
			snprintf(t, sizeof t, "member node %d (%s) %s: %s", m->node,
				ab, m->state == PC_NST_FAILED ? "FAILED" : "STALLED",
				pc_node_reason_text(m->reason));
		} else {
			bad = 0;
			if (m->state == PC_NST_READY)
				ready++;
		}
		snprintf(key, sizeof key, "fleet.node%d", m->node);
		cond(key, bad, PC_AL_WARNING, hold, S(30), bad ? t : "");
	}
	/* a member no longer listed at all - its id released - is counted
	 * as missing below; its own alert is over */
	for (i = 0; i < A.nk; i++) {
		int id;

		if ((!A.k[i].active && !A.k[i].true_since) ||
		        sscanf(A.k[i].key, "fleet.node%d", &id) != 1)
			continue;
		for (j = 0; j < n && A.mem[j].node != id; j++)
			;
		if (j == n) {
			snprintf(key, sizeof key, "%s", A.k[i].key);
			cond(key, 0, 0, 0, S(30), "");
		}
	}
	snprintf(t, sizeof t, "%d member(s) missing - %d listed of the fleet's "
		"%d (%s)", size - n, n, size, expect >= size ? "[cluster] expect"
		: "the most this node has listed");
	cond("fleet.missing", size > n, PC_AL_WARNING, S(60), S(30), t);
	if (size >= 2) {
		snprintf(t, sizeof t, "only %d of the fleet's %d members are "
			"ready - below a majority", ready, size);
		cond("fleet.majority", ready < size / 2 + 1, PC_AL_CRITICAL,
			S(60), S(30), t);
	}
}

/* S352c: one relay from another node - this node mails it, by its own
 * [mail] level, naming the origin, and keeps it in its ring */
static void relay_in(const struct pc_alert_relay *r)
{
	struct aring *e = ring_next();
	int lvl = r->level >= PC_AL_CRITICAL ? PC_AL_CRITICAL : PC_AL_WARNING;

	if (snprintf(e->text, sizeof e->text, "node %d (%s): %s", r->node,
	        r->host, r->text) < 0)
		e->text[0] = 0;
	e->level = lvl;
	e->cleared = r->cleared;
	e->from = r->node;
	snprintf(e->key, sizeof e->key, "%s", r->key);
	A.relayed_in++;
	LM_NOTICE("alert %s on node %d (%s), relayed to this node: %s\n",
		r->cleared ? "cleared" : "raised", r->node, r->host, r->text);
	if (!pc_mail_ready()) {
		snprintf(e->mail, sizeof e->mail, "no mailer");
	} else if (lvl < A.mail_level) {
		snprintf(e->mail, sizeof e->mail, "below level");
	} else {
		digest_add(lvl, r->cleared, e->text, r->node);
		snprintf(e->mail, sizeof e->mail, "mailed");
	}
}

/* the cluster thread: a relay heard, the duplicate copy already dropped */
void pc_alerts_relayed(const struct pc_alert_relay *r)
{
	pthread_mutex_lock(&IB.mx);
	if (IB.n < INBOX)
		IB.q[IB.n++] = *r;
	else
		IB.dropped++;
	pthread_mutex_unlock(&IB.mx);
}

void pc_alerts_tick(void)
{
	struct pc_alert_relay in[INBOX];
	unsigned long long dropped;
	int i, nin;

	/* the ALIVE bit first: what this node can do, then who mails */
	pc_cluster_set_can_mail(pc_mail_ready());
	pthread_mutex_lock(&IB.mx);
	nin = IB.n;
	memcpy(in, IB.q, (size_t)nin * sizeof in[0]);
	IB.n = 0;
	dropped = IB.dropped;
	IB.dropped = 0;
	pthread_mutex_unlock(&IB.mx);

	pthread_mutex_lock(&A.mx);
	A.mailer = pc_cluster_mailer(&A.mailer_self);
	if (A.mailer || pc_mail_ready())
		A.nmr_said = A.nmr_quiet = 0;      /* reachable: said afresh next time */
	A.relay_dropped += dropped;
	for (i = 0; i < nin; i++)
		relay_in(&in[i]);
	pend_tick();
	check_node();
	check_wal();
	check_memory();
	check_upstreams();
	check_fleet();
	A.primed = 1;
	digest_flush(0);
	pthread_mutex_unlock(&A.mx);
}

void pc_alerts_relay_stats(unsigned long long *in, unsigned long long *out,
		unsigned long long *dropped)
{
	pthread_mutex_lock(&A.mx);
	*in = A.relayed_in - A.base_in;
	*out = A.relayed_out - A.base_out;
	*dropped = A.relay_dropped - A.base_drop;
	pthread_mutex_unlock(&A.mx);
}

void pc_alerts_metrics(unsigned long long *in, unsigned long long *out,
		unsigned long long *dropped, int *warning, int *critical)
{
	int i;

	pthread_mutex_lock(&A.mx);
	*in = A.relayed_in;
	*out = A.relayed_out;
	*dropped = A.relay_dropped;
	*warning = *critical = 0;
	for (i = 0; i < A.nk; i++)
		if (A.k[i].active) {
			if (A.k[i].level >= PC_AL_CRITICAL)
				(*critical)++;
			else
				(*warning)++;
		}
	pthread_mutex_unlock(&A.mx);
}

void pc_alerts_stats_reset(void)
{
	pthread_mutex_lock(&A.mx);
	A.base_in = A.relayed_in;
	A.base_out = A.relayed_out;
	A.base_drop = A.relay_dropped;
	pthread_mutex_unlock(&A.mx);
}

void pc_alerts_stats_tree(struct pc_tw *out)
{
	char mw[24];
	int i, j;

	pthread_mutex_lock(&A.mx);
	pc_tw_key(out, "alerts");
	pc_tw_map(out);
	/* S352c: where this node's own alerts go - "this node", "node N"
	 * (relayed to the elected mailer) or "none"; and the node that mails
	 * the fleet's events, 0 = none */
	if (pc_mail_ready())
		snprintf(mw, sizeof mw, "this node");
	else if (A.mailer > 0 && !A.mailer_self)
		snprintf(mw, sizeof mw, "node %d", A.mailer);
	else
		snprintf(mw, sizeof mw, "none");
	pc_tw_key(out, "mailer");
	pc_tw_str(out, mw);
	pc_tw_key(out, "fleet_mailer");
	pc_tw_i64(out, A.mailer);
	pc_tw_key(out, "active");
	pc_tw_arr(out);
	for (i = 0; i < A.nk; i++) {
		const struct akey *k = &A.k[i];

		if (!k->active)
			continue;
		pc_tw_map(out);
		pc_tw_key(out, "key");
		pc_tw_str(out, k->key);
		pc_tw_key(out, "level");
		pc_tw_str(out, lvl_name(k->level));
		pc_tw_key(out, "since_ms");
		pc_tw_i64(out, k->since_wall);
		pc_tw_key(out, "text");
		pc_tw_str(out, k->text);
		pc_tw_end(out);
	}
	pc_tw_end(out);
	pc_tw_key(out, "recent");                  /* newest first */
	pc_tw_arr(out);
	for (i = 0; i < A.rn; i++) {
		const struct aring *e;

		j = (A.rnext - 1 - i + PC_ALERT_RING) % PC_ALERT_RING;
		e = &A.r[j];
		pc_tw_map(out);
		pc_tw_key(out, "ts_ms");
		pc_tw_i64(out, e->ts);
		pc_tw_key(out, "level");
		pc_tw_str(out, lvl_name(e->level));
		pc_tw_key(out, "key");
		pc_tw_str(out, e->key);
		pc_tw_key(out, "event");
		pc_tw_str(out, e->cleared ? "cleared" : "raised");
		pc_tw_key(out, "text");
		pc_tw_str(out, e->text);
		pc_tw_key(out, "mail");
		pc_tw_str(out, e->mail[0] ? e->mail : "-");
		pc_tw_key(out, "from_node");           /* S352c: 0 = this node's */
		pc_tw_i64(out, e->from);
		pc_tw_end(out);
	}
	pc_tw_end(out);
	pc_tw_end(out);
	pthread_mutex_unlock(&A.mx);
}
