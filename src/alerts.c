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
#include <errno.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

#include "compat/dprint.h"
#include "alerts.h"
#include "winctr.h"            /* S385 */
#include "cluster.h"
#include "iostat.h"                   /* S379 */
#include "mail.h"
#include "ptree.h"
#include "recover.h"
#include "upstream.h"
#include "version.h"
#include "wal.h"
#include "rdb.h"                        /* S437 */
#include "store.h"                      /* S436 */
#include "core/pcache_arena.h"
#include "hostmon.h"                  /* S380: the host monitor (wave 1) */

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
	/* S377: the problem as a whole - when it began, its worst, the value
	 * when it cleared, and the mail its resolve answers */
	long long ep_wall, ep_mono;
	int worst_level;
	char worst[PC_ALERT_TEXT];
	char now[PC_ALERT_TEXT];
	char msgid[160];
	long long mailed_wall;
};

/* a digest entry: one raise or clear, its own or relayed */
#define DMAX 32
struct dent {
	int level, cleared, event, from;   /* from: 0 this node, else the origin */
	int again;                         /* a raise of a problem already mailed */
	char key[48], host[64];
	char text[PC_ALERT_TEXT];          /* a raise: what happened; a clear:
	                                    * what was wrong, at its worst */
	char now[PC_ALERT_TEXT];           /* a clear: the value now, "" unknown */
	long long started, at;             /* wall ms: the problem's start; this */
	char ref[160];                     /* a clear: the mail of its problem */
	long long ref_at;
};

/* the relayed problems this node mailed: a clear threads under its
 * problem's mail and says how long it lasted */
#define RMEM 64
struct rmem {
	int node, level, used;
	char key[48];
	long long started, mailed;
	char worst[PC_ALERT_TEXT];
	char msgid[160];
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
	/* the digest being collected (S377: entries, mailed per origin) */
	struct dent dq[DMAX];
	int dn, dmore;
	long long ddeadline;
	struct rmem rm[RMEM];              /* S377: relayed problems mailed */
	int fleet_size, fleet_ready;       /* S377: for the mail's header */
	char self_ip[48];
	char http_addr[64];                /* S377: the status page's link */
	int http_port;
	long long walloss_t[3];            /* S377: the last wal.loss starts */
	int test_said;
	/* [mail]'s rules */
	int mail_level;
	long long min_interval, digest;
	int fast;                          /* PERFCACHED_ALERT_FAST */
	struct pc_alerts_conf al;          /* S380: [alerts] */
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
	unsigned long long tomb_gaps;      /* AR15: cluster.peer_absent said */
} A = { .mx = PTHREAD_MUTEX_INITIALIZER };

/* S380, S377 (below) */
static struct akey *key_find(const char *key, int create);
static int hw_level(const char *key, int pct, int warn, int crit);

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
	return l >= PC_AL_CRITICAL ? "critical" : l >= PC_AL_WARNING ? "warning"
		: "info";
}

void pc_alerts_init(const struct pc_config *cfg)
{
	const char *f = getenv("PERFCACHED_ALERT_FAST");
	const char *th = getenv("PERFCACHED_ALERT_TEST_HOST");

	A.mail_level = cfg->mail.level;    /* S377: info is 0; config defaults it */
	{
		int li;

		for (li = 0; li < cfg->n_listen; li++)
			if (cfg->listen[li].http && cfg->listen[li].port) {
				snprintf(A.http_addr, sizeof A.http_addr, "%s",
					cfg->listen[li].addr ? cfg->listen[li].addr : "");
				A.http_port = cfg->listen[li].port;
				break;
			}
	}
	A.min_interval = S(cfg->mail.min_interval_s);
	A.digest = S(cfg->mail.digest_s);
	A.fast = f ? atoi(f) : 0;
	A.al = cfg->alerts;                /* S380 */
	hm_io_init(cfg->wal_dir ? cfg->wal_dir : cfg->state_dir);
	if (gethostname(A.host, sizeof A.host - 1) != 0)
		snprintf(A.host, sizeof A.host, "perfcached");
	/* S377 test hook: a host name as long as they come, for the subject's
	 * cut (alerttest D) */
	if (th && *th)
		snprintf(A.host, sizeof A.host, "%s", th);
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

/* ---- S377: the mail a person can read (DESIGN 12lu) --------------------- */

/* What each kind of alert is called in a subject, what the node does about
 * it by itself, what a person should do, and the alert that raises it.  A
 * key matches exactly, or by prefix where the entry ends in '.' or is
 * "fleet.node".  The docs list the same table. */
static const struct acat {
	const char *key;
	const char *title, *title_failed;  /* node.state: the title when its
	                                    * text says FAILED (a heal that
	                                    * stalled is critical, still a heal) */
	const char *node;                  /* "The node": NULL when nothing */
	const char *todo;                  /* "What to do" */
	const char *cause;                 /* raised by this key, NULL */
} CAT[] = {
	{ "node.start", "node started", NULL, NULL,
	  "Nothing, if the start was planned.  After an unclean stop, find out "
	  "why it stopped: its log before the stop, the system journal.", NULL },
	{ "node.reconcile", "reconcile without a witness", NULL,       /* S388 */
	  "Serves on; keeps the records it replayed at its start from the "
	  "peers' pulls and repairs until its reconcile can finish - which "
	  "it cannot: every live peer restarted after it went down, so none "
	  "can say what was deleted meanwhile.",
	  "Restart this node - its peers have been up long enough to witness "
	  "that.  Not while another node's reconcile has this one as its only "
	  "witness (its card says reconciling).", NULL },
	{ "node.state", "node healing", "node failed",
	  "Refuses writes and serves reads while it heals, then returns to "
	  "service by itself.",
	  "Nothing, if it is ready again within a few minutes.  If not, its "
	  "log names the reason.", "wal.loss" },
	{ "wal.loss", "WAL lost writes", NULL,
	  "Refuses writes and serves reads while a snapshot makes the lost "
	  "writes durable again, then returns to service by itself.",
	  "The disk under the WAL is slower than the write rate (iostat -x 1).  "
	  "To stop it recurring, raise [wal] ring_kb or move the log to faster "
	  "storage.", NULL },
	{ "wal.capacity", "WAL filling faster than snapshots free it", NULL, /* S437 */
	  "Serves normally, and asks for snapshots back to back.  If the log "
	  "fills before one frees it, acknowledged writes are lost and the "
	  "node refuses writes while it heals (wal.loss).",
	  "Give the log more room - [wal] segments or segment_mb, a restart "
	  "applies it - or lower the write load.  The WAL card shows its size "
	  "and how fast it fills; if the snapshot is the slow side, raise "
	  "rdb_mb_s.", NULL },
	{ "wal.fsync", "WAL fsync slow", NULL, NULL,
	  "Check the disk holding the WAL (iostat -x 1) and what else writes "
	  "to it.  If it stays this slow under load, the log can fall behind "
	  "and the node then refuses writes until a snapshot catches up.", NULL },
	{ "memory.nomem", "writes refused, arena full", NULL,
	  "Refuses each write that does not fit; reads are served.",
	  "Raise [memory] arena_mb, or find what is filling the arena "
	  "(/stats collections).", NULL },
	{ "memory.arena", "arena filling", NULL, NULL,
	  "Raise [memory] arena_mb before it is full, or find what is filling "
	  "it (/stats collections).  At the ceiling, writes are refused.", NULL },
	{ "host.cpu", "CPU busy", NULL, NULL,
	  "Find what keeps the CPUs busy - the alert names perfcached's own "
	  "share of it.", NULL },
	{ "host.io", "disk busy", NULL, NULL,
	  "Find what keeps the device busy (iostat -x 1, iotop).  The WAL needs "
	  "it to keep up with the write rate.", NULL },
	{ "host.memory", "host memory high", NULL, NULL,
	  "Find what uses the memory - the alert names perfcached's own share "
	  "of it.", NULL },
	{ "io.epoll_busy", "epoll workers busy", NULL,                /* S379 */
	  "Serves on; requests wait longer while its workers are busy.",
	  "Where io_uring is usable here (the alert says), change io = epoll "
	  "to io = io_uring in [daemon] and restart this node: it takes about "
	  "a third less CPU a request.  Where it was refused, clear what "
	  "refused it - a seccomp profile that allows io_uring, "
	  "kernel.io_uring_disabled = 0, a kernel 6.0 or later.  A slow disk, "
	  "memory pressure or CPU steal keeps a worker busy too: epoll is the "
	  "lever this node has, not necessarily the cause.", NULL },
	{ "write.", "write-through", NULL,                            /* S385 */
	  "Writes go to the upstream's master first and are read back; with "
	  "no master every write is refused (reads still answer), without "
	  "RedisJSON there every JSON write is.  The master and RedisJSON are "
	  "looked for again on their own - at once after a failure, then at "
	  "most every 10 s (RedisJSON: 60 s).",
	  "No master: check that the listed upstreams answer ROLE and that one "
	  "says master (redis-cli -h <host> -p <port> ROLE) - after a "
	  "failover, list the promoted server too; two masters and no replica "
	  "naming one is a split brain to settle in Redis.  No RedisJSON: load "
	  "the module on the master, or stop JSON writes to this collection.  "
	  "Passed-through writes: a client uses commands perfcached does not "
	  "know - see which on the page, and whether it should.", NULL },
	{ "upstream.", "read-through upstream", NULL,
	  "Answers what it holds; with several upstreams the others take the "
	  "fetches (S382), with none a miss answers from the negative cache "
	  "until one is back.",
	  "Check that the upstream answers (redis-cli -h <host> -p <port> PING "
	  "and ROLE) and that its user may track.  A replica refused as a "
	  "master was promoted: point upstream at the replicas again.  The "
	  "master serving (upstream_master_fallback) means no replica does: "
	  "bring one back.  A failover means the active upstream stopped "
	  "serving: bring it back - with upstream_preempt it takes over "
	  "again.  A perfcached as the upstream is a daisy-chain (refused, a "
	  "ring of them storms): point upstream at Redis, or set "
	  "upstream_allow_perfcached = yes for one deliberate hop.", NULL },
	{ "chain.", "read-through daisy-chain", NULL,                  /* S384 */
	  "Answers another perfcached's read-through from what it holds; "
	  "never fetches onward for it (one hop), so that node sees a miss "
	  "for whatever this one has not fetched itself.",
	  "Point the other perfcached's upstream at Redis itself, as this node "
	  "reads it - a daisy-chain of read-throughs serves its far end only "
	  "what the middle already holds.", NULL },
	{ "config.", "fleet settings differ", NULL,                    /* AR11 */
	  "Serves on, each member by its own settings - so the fleet behaves "
	  "unevenly: writes through a member on allow never reach Redis, a "
	  "member that fsyncs less loses more in a crash, a member without "
	  "the HTTP token serves its page to anyone allowed.",
	  "Make the setting the same on every member (the alert names it and "
	  "the member that differs), then restart that member - one at a "
	  "time, as any configuration change.", NULL },
	{ "cluster.autoscale", "autoscale modes differ across the fleet", /* S436 */
	  NULL,
	  "Serves on, each member sizing the collection by its own mode: "
	  "under auto a member shrinks a table the others keep, so one "
	  "collection holds a different table on each.",
	  "A member on a build before 0.7.2 does not take the fleet's modes: "
	  "upgrade it.  Otherwise the fleet re-sends them every few seconds, "
	  "so a difference that stays is a fault - compare autoscale and "
	  "autoscale_gen in each member's /stats collections, and set the "
	  "mode again with the autoscale verb.", NULL },
	{ "cluster.tombstones", "deletes or expiries forgotten early", NULL, /* AR15, S443 */
	  "Serves on.  A member away while the cap dropped deletes is not "
	  "told of them when it returns: a key deleted then may come back "
	  "from its copy.  A dropped expiry matters less: the copy expires "
	  "again unless it was written with a longer TTL.",
	  "Raise [cluster] tombstone_retain_max above the rate of deletes plus "
	  "expiries (an eager collection's) times tombstone_retain_s - the "
	  "GUIDE sizes it - or lower tombstone_retain_s.  The alert's text says "
	  "how many were expiries.", NULL },
	{ "cluster.acks_stuck", "a member confirms no more deletes", NULL,  /* 12ny AT-11 */
	  "Serves on.  Every delete of that origin since the member's mark "
	  "stopped is kept for it, on every node, until it confirms them or "
	  "the time rule and the cap drop them - then the member is asked to "
	  "rebuild when it is next away.",
	  "Look at that member: /members shows its mark of each origin and "
	  "how long it has stood; its /stats cluster.acks the holes it asks "
	  "for (gap_asked) and its WAL (a mark is confirmed when the WAL has "
	  "synced it).  A restart makes it rebuild its marks.", NULL },
	{ "cluster.peer_absent", "member back after a long absence", NULL,
	  "Replays to the member the deletes it still holds; older ones, or "
	  "ones the cap dropped, are not among them.  A member that confirms "
	  "deletes (tombstone_acks, the default) in an eager fleet is asked to "
	  "rebuild instead: it reconciles what it stored before the absence "
	  "with the fleet, and the text says so.",
	  "When the text says it was asked to rebuild: nothing - follow it in "
	  "that member's /stats (cluster.acks.rebuilds, reconcile_open).  "
	  "Otherwise re-issue the deletes that matter for keys written before "
	  "it left; to cover longer absences, raise tombstone_retain_s and "
	  "tombstone_retain_max with it.", NULL },
	{ "fleet.node", "fleet member down", NULL, NULL,
	  "Look at that member: its log and its /stats state.", NULL },
	{ "fleet.missing", "fleet members missing", NULL, NULL,
	  "Start the missing members, or take them out of the fleet if they "
	  "are gone for good.", NULL },
	{ "fleet.majority", "fleet below majority", NULL,
	  "Without a majority the fleet elects no master and changes no map "
	  "until members return.",
	  "Bring members back.", NULL },
	{ "test.event", "test event", NULL, NULL, NULL, NULL },
	{ "test.cond", "test condition", NULL, NULL, NULL, NULL },
};

static const struct acat *cat_of(const char *key)
{
	size_t i, n;

	for (i = 0; i < sizeof CAT / sizeof CAT[0]; i++) {
		n = strlen(CAT[i].key);
		if (!strcmp(key, CAT[i].key) ||
		    ((CAT[i].key[n - 1] == '.' || !strcmp(CAT[i].key, "fleet.node"))
		     && !strncmp(key, CAT[i].key, n)))
			return &CAT[i];
	}
	return NULL;
}

static const char *title_of(const char *key, const char *text)
{
	const struct acat *c = cat_of(key);

	if (!c)
		return key;
	return c->title_failed && !strncmp(text, "node FAILED", 11) ?
		c->title_failed : c->title;
}

static int key_is_event(const char *key)
{
	return !strcmp(key, "node.start") || !strcmp(key, "test.event") ||
		!strcmp(key, "cluster.peer_absent");             /* AR15 */
}

static void html_esc(FILE *f, const char *s)
{
	for (; *s; s++)
		switch (*s) {
		case '&': fputs("&amp;", f); break;
		case '<': fputs("&lt;", f); break;
		case '>': fputs("&gt;", f); break;
		case '"': fputs("&quot;", f); break;
		case '\'': fputs("&#39;", f); break;
		default: fputc(*s, f);
		}
}

static void when(char *b, size_t n, long long ms)
{
	time_t t = (time_t)(ms / 1000);
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(b, n, "%a %d %b %Y %H:%M:%S %Z", &tm);
}

static void hhmmss(char *b, size_t n, long long ms)
{
	time_t t = (time_t)(ms / 1000);
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(b, n, "%H:%M:%S", &tm);
}

static void lasted(char *b, size_t n, long long ms)
{
	long long s = ms < 0 ? 0 : ms / 1000;

	if (s < 60)
		snprintf(b, n, "%lld s", s);
	else if (s < 3600)
		snprintf(b, n, "%lld min %lld s", s / 60, s % 60);
	else
		snprintf(b, n, "%lld h %lld min", s / 3600, (s % 3600) / 60);
}

/* the state's colour: the bar and the title (HTML), the badge's border */
static void state_colours(int level, int resolved, const char **bar,
		const char **ink)
{
	if (resolved) {
		*bar = "#2f9e55"; *ink = "#2a8a4a";
	} else if (level >= PC_AL_CRITICAL) {
		*bar = "#d93a3a"; *ink = "#cc3333";
	} else if (level >= PC_AL_WARNING) {
		*bar = "#e8a317"; *ink = "#b8740a";
	} else {
		*bar = "#3478c8"; *ink = "#2a6bb5";
	}
}

/* one labelled field of the plain part, the value wrapped at ~70 */
static void tfield(FILE *f, const char *label, const char *v)
{
	const char *p = v;
	int first = 1;

	while (*p) {
		size_t n = strlen(p), cut = n;

		if (n > 56) {
			cut = 56;
			while (cut > 20 && p[cut] != ' ')
				cut--;
			/* one long word - a host name, a path: kept whole, the
			 * quoted-printable part's soft breaks carry the length */
			if (cut <= 20) {
				cut = 56;
				while (p[cut] && p[cut] != ' ')
					cut++;
			}
		}
		fprintf(f, "  %-14s %.*s\n", first ? label : "", (int)cut, p);
		first = 0;
		p += cut;
		while (*p == ' ')
			p++;
	}
}

static void hrow(FILE *f, const char *label, const char *v, int esc)
{
	fputs("<tr><td style=\"font-family:Arial,Helvetica,sans-serif;"
		"font-size:14px;color:#5f5e5a;padding:10px 14px 10px 0;"
		"vertical-align:top;white-space:nowrap;width:120px;"
		"border-top:1px solid #e4e2da;\">", f);
	fputs(label, f);
	fputs("</td>\n<td style=\"font-family:Arial,Helvetica,sans-serif;"
		"font-size:15px;color:#1f1e1d;padding:10px 0;vertical-align:top;"
		"line-height:1.5;border-top:1px solid #e4e2da;\">", f);
	if (esc)
		html_esc(f, v);
	else
		fputs(v, f);
	fputs("</td></tr>\n", f);
}

static const char *badge_word(const struct dent *e)
{
	return e->cleared ? "Resolved" : e->event ? "Event" : e->again ?
		"Problem, still open" : "Problem, new";
}

/* the status page's address, when a person elsewhere can reach it */
static void status_url(char *b, size_t n)
{
	const char *h = A.http_addr;

	b[0] = 0;
	if (!A.http_port)
		return;
	if (!h[0] || !strcmp(h, "0.0.0.0") || !strcmp(h, "*"))
		h = A.self_ip;
	if (!h[0] || !strncmp(h, "127.", 4) || !strcmp(h, "localhost"))
		return;
	snprintf(b, n, "http://%s:%d/", h, A.http_port);
}

/* one mail for one origin: @ix the entries, worst first */
static void mail_one(const int *ix, int n, int more)
{
	const struct dent *top = &A.dq[ix[0]];
	struct pc_cl_stats cs;
	struct pc_mail_msg m;
	char subj[200], plus[16] = "", title[96], host[64], msgid[160];
	char reply[160] = "", refs[900] = "", url[128], b1[64], b2[64], b3[64];
	char *text = NULL, *html = NULL, line[400];
	size_t tl = 0, hl = 0;
	const char *state, *bar, *ink, *headline_tail;
	FILE *t, *h;
	int i, j, problems = 0, ncrit = 0, nwarn = 0, ninfo = 0, nres = 0;
	int resolved_only, worst = PC_AL_INFO, own = !top->from;

	for (i = 0; i < n; i++) {
		const struct dent *e = &A.dq[ix[i]];

		if (e->cleared) {
			nres++;
			continue;
		}
		problems++;
		if (e->level >= PC_AL_CRITICAL)
			ncrit++;
		else if (e->level >= PC_AL_WARNING)
			nwarn++;
		else
			ninfo++;
		if (e->level > worst)
			worst = e->level;
	}
	resolved_only = !problems;
	state = resolved_only ? "RESOLVED" : worst >= PC_AL_CRITICAL ? "CRITICAL"
		: worst >= PC_AL_WARNING ? "WARNING" : "INFO";
	state_colours(worst, resolved_only, &bar, &ink);
	snprintf(host, sizeof host, "%.63s", own ? A.host : top->host);
	snprintf(title, sizeof title, "%s", title_of(top->key, top->text));
	if (n + more > 1)
		snprintf(plus, sizeof plus, " (+%d)", n + more - 1);
	/* "<STATE>: <title> on <host> (+N)", at most 78 characters: the
	 * title is cut first, then the host */
	{
		char st[96], sh[64];
		int room = 78 - (int)(strlen(state) + 2 + 4 + strlen(plus));
		int hl0 = (int)strlen(host), tl0 = (int)strlen(title), tmax, hmax;

		snprintf(st, sizeof st, "%s", title);
		snprintf(sh, sizeof sh, "%s", host);
		if (tl0 + hl0 > room) {
			tmax = room - hl0;
			if (tmax < 12) {
				tmax = 12;
				hmax = room - 12 < 8 ? 8 : room - 12;
				if (hl0 > hmax)
					snprintf(sh + hmax - 3, sizeof sh - (size_t)(hmax - 3),
						"...");
			}
			if (tl0 > tmax)
				snprintf(st + tmax - 3, sizeof st - (size_t)(tmax - 3), "...");
		}
		snprintf(subj, sizeof subj, "%s: %s on %s%s", state, st, sh, plus);
	}
	pc_mail_msgid(msgid, sizeof msgid);
	for (i = 0; i < n; i++) {
		const struct dent *e = &A.dq[ix[i]];

		if (!e->ref[0])
			continue;
		if (!reply[0])
			snprintf(reply, sizeof reply, "%s", e->ref);
		if (strlen(refs) + strlen(e->ref) + 2 < sizeof refs) {
			size_t rl = strlen(refs);

			snprintf(refs + rl, sizeof refs - rl, "%s%s", rl ? " " : "", e->ref);
		}
	}
	pc_cluster_get_stats(&cs);
	status_url(url, sizeof url);
	if (resolved_only)
		headline_tail = "recovered";
	else if (worst >= PC_AL_CRITICAL)
		headline_tail = "is critical";
	else if (worst >= PC_AL_WARNING)
		headline_tail = "needs attention";
	else
		headline_tail = NULL;

	t = open_memstream(&text, &tl);
	h = open_memstream(&html, &hl);
	if (!t || !h) {
		if (t)
			fclose(t);
		if (h)
			fclose(h);
		free(text);
		free(html);
		return;
	}
	/* ---- the banner ---- */
	if (headline_tail)
		snprintf(line, sizeof line, "%s %s", host, headline_tail);
	else
		snprintf(line, sizeof line, "%s: %s", host, title);
	fprintf(t, "%s - %s\n", state, line);
	fputs("<!doctype html><html><body style=\"margin:0;padding:0;"
		"background:#f4f3ef;\">\n<table role=\"presentation\" width=\"100%\" "
		"cellpadding=\"0\" cellspacing=\"0\" style=\"background:#f4f3ef;\">"
		"<tr><td align=\"center\" style=\"padding:12px 8px;\">\n"
		"<table role=\"presentation\" width=\"100%\" cellpadding=\"0\" "
		"cellspacing=\"0\" style=\"width:100%;max-width:640px;"
		"background:#ffffff;border:1px solid #d8d6cd;border-radius:10px;\">\n",
		h);
	fprintf(h, "<tr><td style=\"border-top:6px solid %s;padding:16px 20px "
		"12px;\">\n<div style=\"font-family:Arial,Helvetica,sans-serif;"
		"font-size:20px;font-weight:bold;color:%s;\">", bar, ink);
	html_esc(h, line);
	fputs("</div>\n", h);
	lasted(b1, sizeof b1, top->at - top->started);
	if (resolved_only && n == 1)
		snprintf(line, sizeof line, "%s is back to normal after %s.  No action "
			"needed.", title, b1);
	else if (resolved_only)
		snprintf(line, sizeof line, "%d problems resolved.  No action needed.",
			nres);
	else if (problems == 1 && worst >= PC_AL_CRITICAL)
		snprintf(line, sizeof line, "Act now: %s.", title);
	else if (problems == 1 && worst >= PC_AL_WARNING)
		snprintf(line, sizeof line, "Warning: look at it today.  %s.", title);
	else if (problems == 1)
		snprintf(line, sizeof line, "For your information - no action "
			"needed.");
	else {
		/* "3 alerts: 1 critical, 2 warning" - the counts there are */
		size_t ll;

		snprintf(line, sizeof line, "%s%d alerts:", worst >= PC_AL_CRITICAL ?
			"Act now.  " : "", n);
		ll = strlen(line);
		if (ncrit)
			ll += (size_t)snprintf(line + ll, sizeof line - ll, " %d critical,",
				ncrit);
		if (nwarn)
			ll += (size_t)snprintf(line + ll, sizeof line - ll, " %d warning,",
				nwarn);
		if (ninfo)
			ll += (size_t)snprintf(line + ll, sizeof line - ll, " %d info,",
				ninfo);
		if (nres)
			ll += (size_t)snprintf(line + ll, sizeof line - ll, " %d resolved,",
				nres);
		if (ll && line[ll - 1] == ',')
			line[ll - 1] = '.';
	}
	fprintf(t, "%s\n", line);
	fprintf(h, "<div style=\"font-family:Arial,Helvetica,sans-serif;"
		"font-size:15px;color:%s;padding-top:5px;\">", ink);
	html_esc(h, line);
	fputs("</div>\n</td></tr>\n", h);

	/* ---- the node ---- */
	fputs("<tr><td style=\"padding:14px 20px 4px;\"><table "
		"role=\"presentation\" cellpadding=\"0\" cellspacing=\"0\">\n<tr>", h);
#define HM_CELL "<td style=\"font-family:Arial,Helvetica,sans-serif;" \
	"font-size:14px;color:#5f5e5a;padding:3px 16px 3px 0;\">"
	if (own) {
		snprintf(line, sizeof line, "%s%s%s%s", host, A.self_ip[0] ? " (" : "",
			A.self_ip, A.self_ip[0] ? ")" : "");
		fputs("\n", t);
		tfield(t, "Host", line);
		fputs(HM_CELL "Host <b>", h);
		html_esc(h, host);
		fputs("</b>", h);
		if (A.self_ip[0]) {
			fputs(" (", h);
			html_esc(h, A.self_ip);
			fputs(")", h);
		}
		fputs("</td>", h);
		if (cs.enabled) {
			snprintf(line, sizeof line, "%d", (int)cs.node_id);
			tfield(t, "Node", line);
			fprintf(h, HM_CELL "Node <b>%d</b></td>", (int)cs.node_id);
		}
		snprintf(line, sizeof line, "perfcached %s (%s)", PC_VERSION,
			PC_BUILD_REV);
		tfield(t, "Version", line);
		fprintf(h, HM_CELL "Version <b>%s</b> (%s)</td></tr>\n<tr>",
			PC_VERSION, PC_BUILD_REV);
		snprintf(line, sizeof line, "%s", pc_node_state_name(pc_node_state()));
		tfield(t, "State now", line);
		fprintf(h, HM_CELL "State now <b>%s</b></td>",
			pc_node_state_name(pc_node_state()));
	} else {
		snprintf(line, sizeof line, "%s, node %d", host, top->from);
		fputs("\n", t);
		tfield(t, "Host", line);
		fputs(HM_CELL "Host <b>", h);
		html_esc(h, host);
		fprintf(h, "</b></td>" HM_CELL "Node <b>%d</b></td></tr>\n<tr>",
			top->from);
	}
	if (cs.enabled && A.fleet_size) {
		snprintf(line, sizeof line, "%d of %d ready", A.fleet_ready,
			A.fleet_size);
		tfield(t, "Fleet", line);
		fprintf(h, HM_CELL "Fleet <b>%d of %d ready</b></td>", A.fleet_ready,
			A.fleet_size);
	}
	fputs("</tr>\n</table></td></tr>\n", h);
#undef HM_CELL

	/* ---- the alerts, worst first ---- */
	for (i = 0; i < n; i++) {
		const struct dent *e = &A.dq[ix[i]];
		const struct acat *c = cat_of(e->key);
		const char *eb, *ei;
		char idl[200];
		int caused = 0;

		state_colours(e->level, e->cleared, &eb, &ei);
		fprintf(t, "\n%s - %s\n", e->cleared ? "RESOLVED" : e->level >=
			PC_AL_CRITICAL ? "CRITICAL" : e->level >= PC_AL_WARNING ?
			"WARNING" : "INFO", title_of(e->key, e->text));
		fprintf(h, "<tr><td style=\"padding:%s20px 2px;\"><span style=\""
			"font-family:Arial,Helvetica,sans-serif;font-size:13px;color:%s;"
			"border:1px solid %s;border-radius:4px;padding:2px 10px;\">%s"
			"</span> <span style=\"font-family:Arial,Helvetica,sans-serif;"
			"font-size:15px;font-weight:bold;color:#1f1e1d;\">", i ? "16px " :
			"10px ", ei, eb, e->cleared ? "Resolved" : e->level >=
			PC_AL_CRITICAL ? "Critical" : e->level >= PC_AL_WARNING ?
			"Warning" : "Info");
		html_esc(h, title_of(e->key, e->text));
		fputs("</span></td></tr>\n<tr><td style=\"padding:2px 20px 4px;\">"
			"<table role=\"presentation\" width=\"100%\" cellpadding=\"0\" "
			"cellspacing=\"0\">\n", h);
		tfield(t, e->cleared ? "What was wrong" : "What happened", e->text);
		hrow(h, e->cleared ? "What was wrong" : "What happened", e->text, 1);
		tfield(t, "Status", badge_word(e));
		snprintf(line, sizeof line, "<span style=\"color:%s;border:1px solid "
			"%s;border-radius:4px;padding:2px 10px;font-size:13px;\">%s</span>",
			ei, eb, badge_word(e));
		hrow(h, "Status", line, 0);
		if (e->cleared && e->now[0]) {
			tfield(t, "Now", e->now);
			hrow(h, "Now", e->now, 1);
		}
		when(b1, sizeof b1, e->started ? e->started : e->at);
		tfield(t, e->event ? "When" : "Started", b1);
		hrow(h, e->event ? "When" : "Started", b1, 1);
		if (e->cleared) {
			when(b2, sizeof b2, e->at);
			tfield(t, "Resolved", b2);
			hrow(h, "Resolved", b2, 1);
			if (e->started) {
				lasted(b3, sizeof b3, e->at - e->started);
				tfield(t, "Lasted", b3);
				hrow(h, "Lasted", b3, 1);
			}
		} else {
			if (c && c->node) {
				tfield(t, "The node", c->node);
				hrow(h, "The node", c->node, 1);
			}
			if (c && c->todo) {
				tfield(t, "What to do", c->todo);
				hrow(h, "What to do", c->todo, 1);
			}
		}
		if (c && c->cause)
			for (j = 0; j < n; j++)
				if (!A.dq[ix[j]].cleared && !strcmp(A.dq[ix[j]].key, c->cause))
					caused = 1;
		hhmmss(b1, sizeof b1, e->ref_at);
		if (e->cleared && e->ref_at)
			snprintf(idl, sizeof idl, "%s - closes the problem mailed at %s",
				e->key, b1);
		else if (e->again && e->ref_at)
			snprintf(idl, sizeof idl, "%s - first mailed at %s", e->key, b1);
		else if (!e->cleared && caused)
			snprintf(idl, sizeof idl, "%s - caused by %s above", e->key,
				c->cause);
		else
			snprintf(idl, sizeof idl, "%s", e->key);
		tfield(t, "ID", idl);
		hrow(h, "ID", idl, 1);
		fputs("</table></td></tr>\n", h);
	}
	if (more) {
		snprintf(line, sizeof line, "%d more in /stats alerts on %s.", more,
			A.host);
		fprintf(t, "\n%s\n", line);
		fprintf(h, "<tr><td style=\"font-family:Arial,Helvetica,sans-serif;"
			"font-size:14px;color:#5f5e5a;padding:10px 20px;\">%s</td></tr>\n",
			line);
	}

	/* ---- the footer ---- */
	if (problems && !resolved_only && worst >= PC_AL_WARNING)
		snprintf(line, sizeof line, "A resolved mail follows when %s.",
			problems == 1 ? "it clears" : "each clears");
	else
		line[0] = 0;
	if (line[0])
		fprintf(t, "\n%s\n", line);
	fputs("<tr><td style=\"font-family:Arial,Helvetica,sans-serif;"
		"font-size:13px;color:#5f5e5a;line-height:1.6;padding:14px 20px 18px;"
		"border-top:1px solid #e4e2da;\">", h);
	if (line[0]) {
		html_esc(h, line);
		fputs("<br>\n", h);
	}
	b2[0] = 0;
	if (cs.enabled)
		snprintf(b2, sizeof b2, " (node %d)", (int)cs.node_id);
	/* both nodes may well be on one host: the ids tell them apart */
	if (own)
		snprintf(line, sizeof line, "Sent by %s%s.", A.host, b2);
	else
		snprintf(line, sizeof line, "Sent by %s%s for %s (node %d), which has "
			"no [mail] of its own.", A.host, b2, host, top->from);
	fprintf(t, "%s", line);
	html_esc(h, line);
	if (url[0]) {
		fprintf(t, "  Status page: %s", url);
		fputs(" <a href=\"", h);
		html_esc(h, url);
		fputs("\" style=\"color:#185fa5;\">Status page</a>", h);
	}
	fprintf(t, "\nThe last %d alerts are in /stats alerts.\n", PC_ALERT_RING);
	fprintf(h, " &middot; the last %d alerts are in /stats alerts.</td></tr>\n"
		"</table>\n</td></tr></table></body></html>\n", PC_ALERT_RING);
	fclose(t);
	fclose(h);

	memset(&m, 0, sizeof m);
	m.subject = subj;
	m.from_name = host;
	m.msgid = msgid;
	m.in_reply_to = reply[0] ? reply : NULL;
	m.references = refs[0] ? refs : NULL;
	m.text = text;
	m.html = html;
	if (pc_mail_send_msg(&m) != 0) {
		LM_WARN("alerts: the mail '%s' could not be queued\n", subj);
	} else {
		/* a problem's mail is what its resolve answers */
		long long now = wall_ms();

		for (i = 0; i < n; i++) {
			const struct dent *e = &A.dq[ix[i]];
			struct akey *k;
			int r;

			if (e->cleared || e->event)
				continue;
			if (!e->from) {
				k = key_find(e->key, 0);
				if (k && !k->msgid[0]) {
					snprintf(k->msgid, sizeof k->msgid, "%s", msgid);
					k->mailed_wall = now;
				}
				continue;
			}
			for (r = 0; r < RMEM; r++)
				if (A.rm[r].used && A.rm[r].node == e->from &&
				    !strcmp(A.rm[r].key, e->key) && !A.rm[r].msgid[0]) {
					snprintf(A.rm[r].msgid, sizeof A.rm[r].msgid, "%s", msgid);
					A.rm[r].mailed = now;
				}
		}
	}
	free(text);
	free(html);
}

static void digest_add_entry(const struct dent *e)
{
	if (A.dn >= DMAX) {
		A.dmore++;                     /* the mail is full: the ring has it */
		return;
	}
	if (!A.dn)
		A.ddeadline = mono_ms() + A.digest;
	A.dq[A.dn++] = *e;
}

/* this node's own raise or clear */
static void digest_own(const struct akey *k, int cleared)
{
	struct dent e;

	memset(&e, 0, sizeof e);
	e.level = k->level;
	e.cleared = cleared;
	e.event = k->oneoff;
	snprintf(e.key, sizeof e.key, "%s", k->key);
	snprintf(e.host, sizeof e.host, "%.63s", A.host);
	snprintf(e.text, sizeof e.text, "%s", cleared && k->worst[0] ? k->worst
		: k->text);
	if (cleared) {
		snprintf(e.now, sizeof e.now, "%s", k->now);
		snprintf(e.ref, sizeof e.ref, "%s", k->msgid);
		e.ref_at = k->mailed_wall;
	} else if (!k->oneoff && k->msgid[0]) {
		e.again = 1;
		snprintf(e.ref, sizeof e.ref, "%s", k->msgid);
		e.ref_at = k->mailed_wall;
	}
	e.started = k->ep_wall;
	e.at = wall_ms();
	digest_add_entry(&e);
}

/* a relayed raise or clear: the problem is remembered here, so its clear
 * threads under its mail and says how long it lasted */
static void digest_relay(const struct pc_alert_relay *r, int lvl)
{
	struct dent e;
	int i, slot = -1, freeslot = -1;
	long long now = wall_ms();

	memset(&e, 0, sizeof e);
	e.level = lvl;
	e.cleared = r->cleared;
	e.event = key_is_event(r->key);
	e.from = r->node;
	snprintf(e.key, sizeof e.key, "%s", r->key);
	snprintf(e.host, sizeof e.host, "%s", r->host);
	snprintf(e.text, sizeof e.text, "%s", r->text);
	e.at = now;
	for (i = 0; i < RMEM; i++) {
		if (A.rm[i].used && A.rm[i].node == r->node &&
		    !strcmp(A.rm[i].key, r->key))
			slot = i;
		else if (!A.rm[i].used && freeslot < 0)
			freeslot = i;
	}
	if (!r->cleared && !e.event) {
		if (slot < 0 && freeslot >= 0) {
			slot = freeslot;
			memset(&A.rm[slot], 0, sizeof A.rm[slot]);
			A.rm[slot].used = 1;
			A.rm[slot].node = r->node;
			snprintf(A.rm[slot].key, sizeof A.rm[slot].key, "%s", r->key);
			A.rm[slot].started = now;
			A.rm[slot].level = -1;
		}
		if (slot >= 0) {
			if (lvl > A.rm[slot].level) {
				A.rm[slot].level = lvl;
				snprintf(A.rm[slot].worst, sizeof A.rm[slot].worst, "%s",
					r->text);
			}
			e.started = A.rm[slot].started;
			if (A.rm[slot].msgid[0]) {
				e.again = 1;
				snprintf(e.ref, sizeof e.ref, "%s", A.rm[slot].msgid);
				e.ref_at = A.rm[slot].mailed;
			}
		}
	} else if (r->cleared && slot >= 0) {
		e.started = A.rm[slot].started;
		snprintf(e.now, sizeof e.now, "%s", r->text);
		if (A.rm[slot].worst[0])
			snprintf(e.text, sizeof e.text, "%s", A.rm[slot].worst);
		snprintf(e.ref, sizeof e.ref, "%s", A.rm[slot].msgid);
		e.ref_at = A.rm[slot].mailed;
		A.rm[slot].used = 0;
	}
	digest_add_entry(&e);
}

/* worst first: critical, warning, info; a problem before an event of the
 * same level; resolves last */
static int dent_rank(const struct dent *e)
{
	return e->cleared ? -1 : e->level * 2 + !e->event;
}

static void digest_flush(int force)
{
	int ix[DMAX], done[DMAX] = { 0 };
	int i, j, n, a, b, tmp;

	if (!A.dn || (!force && mono_ms() < A.ddeadline))
		return;
	/* S377: one mail per origin node, its alerts worst first */
	for (i = 0; i < A.dn; i++) {
		if (done[i])
			continue;
		n = 0;
		for (j = i; j < A.dn; j++)
			if (!done[j] && A.dq[j].from == A.dq[i].from) {
				ix[n++] = j;
				done[j] = 1;
			}
		for (a = 1; a < n; a++)            /* insertion sort, stable */
			for (b = a; b > 0 && dent_rank(&A.dq[ix[b]]) >
			     dent_rank(&A.dq[ix[b - 1]]); b--) {
				tmp = ix[b];
				ix[b] = ix[b - 1];
				ix[b - 1] = tmp;
			}
		mail_one(ix, n, A.dq[i].from ? 0 : A.dmore);
	}
	A.dn = 0;
	A.dmore = 0;
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
		digest_own(k, cleared);                 /* S377 */
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

	/* S377: a new problem (or any one-off) starts its record */
	if (!k->active || k->oneoff) {
		k->ep_wall = wall_ms();
		k->ep_mono = now;
		k->worst_level = -1;
		k->worst[0] = 0;
		k->now[0] = 0;
		k->msgid[0] = 0;
		k->mailed_wall = 0;
		if (!strcmp(k->key, "wal.loss")) {
			A.walloss_t[2] = A.walloss_t[1];
			A.walloss_t[1] = A.walloss_t[0];
			A.walloss_t[0] = now;
		}
	}
	if (level > k->worst_level) {
		k->worst_level = level;
		snprintf(k->worst, sizeof k->worst, "%s", text);
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
	else if (level >= PC_AL_WARNING)
		LM_WARN("alert%s: %s\n", level >= PC_AL_CRITICAL ? " (critical)"
			: "", text);
	else
		LM_NOTICE("alert (info): %s\n", text);       /* S377 */
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
	/* S377: the value now, for its resolve - where the text IS a measure */
	if (text && text[0] && (!strncmp(key, "host.", 5) ||
	    !strncmp(key, "io.", 3) ||
	    !strcmp(key, "memory.arena") || !strcmp(key, "wal.fsync")))
		snprintf(k->now, sizeof k->now, "%s", text);
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
		/* S377: healing is the node's own repair - a warning, critical
		 * only when it has not healed in ten minutes */
		struct akey *hk = key_find("node.state", 0);
		int lv = hk && hk->active && mono_ms() - hk->ep_mono >= T(S(600)) ?
			PC_AL_CRITICAL : PC_AL_WARNING;

		snprintf(t, sizeof t, "node HEALING%s: %s", lv >= PC_AL_CRITICAL ?
			" for more than 10 minutes" : "",
			pc_node_reason_text(pc_node_reason()));
		cond("node.state", 1, lv, 0, 0, t);
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
			/* S377: it is up again - look today why it stopped */
			event("node.start", PC_AL_WARNING, t);
		} else {
			snprintf(t, sizeof t, "started (perfcached %s), ready",
				PC_VERSION);
			event("node.start", PC_AL_INFO, t);    /* S377: routine */
		}
	}
	/* S388: a reconcile with no witness waits for a restart of THIS node -
	 * the header said it and the log once; now the alerts (and the mail) */
	if (pc_cluster_enabled()) {
		struct pc_cl_stats cs;

		pc_cluster_get_stats(&cs);
		snprintf(t, sizeof t, "reconcile has had no witness for %llds - "
			"every live peer restarted after this node went down, so none "
			"can say what was deleted meanwhile: restart this node",
			(long long)cs.reconcile_no_witness_s);
		cond("node.reconcile", cs.reconcile_no_witness_s >= 30,
			PC_AL_WARNING, 0, S(30), t);
	}
	/* S377: PERFCACHED_ALERT_TEST_EVENT=<text> raises one test event
	 * (level PERFCACHED_ALERT_TEST_LEVEL, warning by default) once ready -
	 * for the suites: what a mail does with any text */
	if (!A.test_said && st == PC_NST_READY) {
		const char *te = getenv("PERFCACHED_ALERT_TEST_EVENT");
		const char *tl = getenv("PERFCACHED_ALERT_TEST_LEVEL");

		A.test_said = 1;
		if (te && te[0])
			event("test.event", tl && !strcmp(tl, "critical") ?
				PC_AL_CRITICAL : tl && !strcmp(tl, "info") ? PC_AL_INFO
				: PC_AL_WARNING, te);
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
		/* S377: the node heals a loss by itself (HEALING, a snapshot) -
		 * a warning; the third within an hour is critical, it will
		 * keep happening until someone changes the disk or the ring */
		struct akey *wk = key_find("wal.loss", 0);
		long long nm = mono_ms();
		int wi, inhour = wk && wk->active ? 0 : 1;   /* the one starting now */
		int third;

		for (wi = 0; wi < 3; wi++)
			if (A.walloss_t[wi] && nm - A.walloss_t[wi] < T(S(3600)))
				inhour++;
		third = inhour >= 3;

		snprintf(t, sizeof t, "the WAL lost records: %llu dropped, %llu "
			"overrun in the last second%s", dd, dov, third ?
			" - the third loss within an hour" : "");
		cond("wal.loss", dd || dov, third ? PC_AL_CRITICAL : PC_AL_WARNING,
			0, S(300), t);
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
		/* the ratio to one decimal: the resolved line of a mail read
		 * "0x the 3163 us measured at start" for 2506 us (operator,
		 * 2026-10-16) - an integer ratio has no "0.8x" */
		unsigned long long r10 = avg * 10 / ob.probe_p50_us;

		snprintf(t, sizeof t, "WAL fsync averages %llu us over the last "
			"minute - %llu.%llux the %llu us measured at start",
			avg, r10 / 10, r10 % 10, ob.probe_p50_us);
		cond("wal.fsync", n >= 5 && avg > 4 * ob.probe_p50_us,
			PC_AL_WARNING, S(300), S(60), t);
	}

	/* S437: the log's room against the snapshots that free it.  The WAL
	 * asks for a snapshot by itself and logs a line (S224's rate trigger,
	 * RV-13's sizing warning); this is what an operator is shown and
	 * mailed.  Either:
	 *  - the whole ring fills faster than one snapshot takes - undersized:
	 *    overruns and heals are certain while the load lasts.  Held a
	 *    minute; or
	 *  - the free segments last less than twice the last snapshot's
	 *    duration at the current fill rate - the trigger is firing back to
	 *    back, and one slower snapshot overruns the ring.  Firing alone is
	 *    only the WAL asking for the next snapshot (GUIDE); HOLDING is the
	 *    news, so five minutes - at a minute walsizetest's healthy 64 MB
	 *    ring raised it while snapshots ran back to back for a few
	 *    seconds (GitHub 39952740047).
	 * Cleared after five. */
	{
		struct pc_rdb_stats rs;
		int on = 0;
		long long hold = S(300);

		t[0] = '\0';
		pc_rdb_get_stats(&rs);
		if (rs.enabled && rs.last_dur_ms > 0 && ws.segments > 0) {
			double dur_s = (double)rs.last_dur_ms / 1000.0;
			unsigned long long mb = ws.segment_bytes >> 20;

			if (ws.ring_fill_s >= 0 && ws.ring_fill_s < dur_s) {
				on = 1;
				hold = S(60);
				snprintf(t, sizeof t, "the whole WAL (%d x %llu MB) "
					"fills in %.1f s at %.1f MB/s - less than the "
					"last snapshot's %.1f s, so it overruns before a "
					"snapshot can free it", ws.segments, mb,
					ws.ring_fill_s, ws.fill_mb_s, dur_s);
			} else if (ws.full_in_s >= 0 && ws.full_in_s < 2 * dur_s) {
				on = 1;
				snprintf(t, sizeof t, "the WAL's %d free segment(s) of "
					"%d (%llu MB each) last %.1f s at %.1f MB/s - "
					"under twice the last snapshot's %.1f s",
					ws.free_segments, ws.segments, mb, ws.full_in_s,
					ws.fill_mb_s, dur_s);
			}
		}
		cond("wal.capacity", on, PC_AL_WARNING, hold, S(300), t);
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
		/* S380: the operator's thresholds (0.6.0: under 10% left a
		 * warning, under 1% critical) */
		int used = held >= mx ? 100 : (int)(held * 100 / mx);
		int lv = hw_level("memory.arena", used, A.al.arena_warn,
			A.al.arena_crit);

		snprintf(t, sizeof t, "the arena holds %lu of its %lu MB - %d%% "
			"used%s - warning at %d%%, critical at %d%%", held >> 20,
			mx >> 20, used, used >= 100 ? ", at its ceiling" : "",
			A.al.arena_warn, A.al.arena_crit);
		cond("memory.arena", lv > 0, lv ? lv : PC_AL_WARNING, S(60), S(60),
			t);
	}
}


/* ---- S380: the host's high-water alarms --------------------------------- */

/* the level a high-water condition is at: critical from @crit, a warning
 * from @warn, and HM_HYST points of hysteresis under the level it already
 * holds, so a value sitting on a line does not flap.  0 = none. */
static int hw_level(const char *key, int pct, int warn, int crit)
{
	struct akey *k = key_find(key, 0);
	int cur = k && k->active ? k->level : 0;

	if (pct >= crit || (cur >= PC_AL_CRITICAL && pct >= crit - HM_HYST))
		return PC_AL_CRITICAL;
	if (pct >= warn || (cur && pct >= warn - HM_HYST))
		return PC_AL_WARNING;
	return 0;
}

/* S377 test hook: PERFCACHED_ALERT_TEST_COND=<file> is a condition the
 * suites drive - the file's first line "warning <text>" or "critical
 * <text>" raises it at that level (a change of level is mailed again),
 * anything else clears it.  Read before the alerts lock, as S380's files
 * are; @lvl -1 when the hook is off.  The arena cannot stand in: a freed
 * cell stays held until reclaim retires its chunk, so it does not clear
 * on a test's clock */
static int test_cond_read(char *t, size_t cap)
{
	const char *p = getenv("PERFCACHED_ALERT_TEST_COND");
	FILE *f;
	char line[PC_ALERT_TEXT + 16];
	int lvl = 0;

	t[0] = 0;
	if (!p || !*p)
		return -1;
	f = fopen(p, "r");
	if (!f)
		return 0;
	if (fgets(line, sizeof line, f)) {
		const char *src = NULL;

		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "warning ", 8)) {
			lvl = PC_AL_WARNING;
			src = line + 8;
		} else if (!strncmp(line, "critical ", 9)) {
			lvl = PC_AL_CRITICAL;
			src = line + 9;
		}
		if (src) {                     /* cut to fit, not a format */
			size_t n = strlen(src);

			if (n >= cap)
				n = cap - 1;
			memcpy(t, src, n);
			t[n] = 0;
		}
	}
	fclose(f);
	return lvl;
}

/* S379: a node on epoll whose busiest worker runs out of time is told
 * the lever it has - io_uring where the host can run it, else what refused
 * it.  It says what it measured, not why: a slow disk, memory pressure or
 * CPU steal keeps a worker busy too.  Never on io_uring.  @busy: the
 * busiest worker's share of the last minute, read before the lock */
static void check_io(int busy)
{
	const struct pc_io_backend *b = &pc_io_backend;
	char t[PC_ALERT_TEXT];

	if (b->active != PC_IO_EPOLL || busy < 0)
		return;
	if (b->configured == PC_IO_EPOLL && b->uring_ok)
		snprintf(t, sizeof t, "the busiest worker %d%% busy over the last "
			"minute, on epoll - io_uring is usable here: change io = "
			"epoll to io = io_uring and restart", busy);
	else
		snprintf(t, sizeof t, "the busiest worker %d%% busy over the last "
			"minute, on epoll - io_uring %s: %.70s", busy,
			b->configured == PC_IO_EPOLL ? "is not usable here either"
			: "was refused", b->configured == PC_IO_EPOLL ? b->uring_why
			: b->why);
	cond("io.epoll_busy", busy >= A.al.epoll_busy, PC_AL_WARNING, S(300),
		S(60), t);
}

static void check_host(const struct hm_obs *o)
{
	char t[PC_ALERT_TEXT];
	int lv;

	hm_apply(o, A.fast);
	if (HM.cpu_pct >= 0) {
		lv = hw_level("host.cpu", HM.cpu_pct, A.al.cpu_warn, A.al.cpu_crit);
		snprintf(t, sizeof t, "CPU %d%% busy over the last minute, "
			"perfcached %d%% (%d CPUs) - warning at %d%%, critical at %d%%",
			HM.cpu_pct, HM.cpu_self_pct, HM.ncpu, A.al.cpu_warn,
			A.al.cpu_crit);
		cond("host.cpu", lv > 0, lv ? lv : PC_AL_WARNING, S(300), S(60), t);
	}
	if (HM.io_pct >= 0) {
		lv = hw_level("host.io", HM.io_pct, A.al.io_warn, A.al.io_crit);
		snprintf(t, sizeof t, "disk %.32s %d%% busy over the last minute (it "
			"holds %.60s) - warning at %d%%, critical at %d%%", HM.dev,
			HM.io_pct, HM.path, A.al.io_warn, A.al.io_crit);
		cond("host.io", lv > 0, lv ? lv : PC_AL_WARNING, S(300), S(60), t);
	}
	if (HM.mem_pct >= 0) {
		lv = hw_level("host.memory", HM.mem_pct, A.al.mem_warn,
			A.al.mem_crit);
		/* ~160 bytes in practice; the result is checked, as the relay's
		 * is, because six 64-bit figures can in theory outrun the
		 * 200-byte text and -Wformat-truncation says so */
		if (HM.huge_kb) {
			if (snprintf(t, sizeof t, "host memory %d%% used (%llu of %llu "
				"MB available beside %llu MB of huge pages), perfcached "
				"holds %llu MB + %llu MB of the huge pages - warning at "
				"%d%%, critical at %d%%", HM.mem_pct,
				HM.mem_avail_kb >> 10, HM.mem_total_kb >> 10,
				HM.huge_kb >> 10, HM.rss_mb, HM.huge_self_kb >> 10,
				A.al.mem_warn, A.al.mem_crit) < 0)
				t[0] = 0;
		} else
			snprintf(t, sizeof t, "host memory %d%% used (%llu of %llu "
				"MB available), perfcached holds %llu MB - warning at "
				"%d%%, critical at %d%%", HM.mem_pct,
				HM.mem_avail_kb >> 10, HM.mem_total_kb >> 10, HM.rss_mb,
				A.al.mem_warn, A.al.mem_crit);
		cond("host.memory", lv > 0, lv ? lv : PC_AL_WARNING, S(60), S(60), t);
	}
}

/* S381: the CPUs this process may use - its affinity, capped by a cgroup
 * cpu.max quota on its own cgroup or an ancestor (a container's limit) */
double pc_process_cpus(void)
{
	unsigned long mask[64];
	char line[512], path[600], *p;
	double cpus = 0, q;
	long n, i;
	FILE *f;

	n = syscall(SYS_sched_getaffinity, 0, sizeof mask, mask);
	for (i = 0; i < n / (long)sizeof mask[0]; i++)
		cpus += __builtin_popcountl(mask[i]);
	if (cpus <= 0)
		cpus = (double)sysconf(_SC_NPROCESSORS_ONLN);
	f = fopen("/proc/self/cgroup", "r");
	if (!f)
		return cpus;
	path[0] = 0;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, "0::", 3)) {
			line[strcspn(line, "\n")] = 0;
			snprintf(path, sizeof path, "/sys/fs/cgroup%s", line + 3);
			break;
		}
	fclose(f);
	while (path[0] && strcmp(path, "/sys/fs/cgroup") != 0) {
		char cm[640];
		long long quota, period;

		snprintf(cm, sizeof cm, "%s/cpu.max", path);
		f = fopen(cm, "r");
		if (f) {
			if (fscanf(f, "%lld %lld", &quota, &period) == 2 &&
			    quota > 0 && period > 0) {
				long long hundredths = quota * 100 / period;

				q = (double)hundredths / 100.0;
				if (q < cpus)
					cpus = q;
			}
			fclose(f);
		}
		p = strrchr(path, '/');
		if (!p)
			break;
		*p = 0;
	}
	return cpus;
}

static void tw_pct(struct pc_tw *out, const char *k, int v)
{
	pc_tw_key(out, k);
	if (v < 0)
		pc_tw_nil(out);
	else
		pc_tw_i64(out, v);
}

/* S382: why an upstream of a read-through collection is not used now */
static const char *up_why(const struct pc_up_srv_health *s)
{
	if (s->state != PC_UPS_OFF)
		return s->conns_up ? NULL : "unreachable";
	if (s->perfcached)                     /* S384 */
		return "a perfcached: daisy-chaining off (upstream_allow_perfcached)";
	if (s->role == 1)
		return "a master - not used (upstream_role = replica)";
	if (s->role == 2 && s->link_down)
		return "a replica cut off from its master - skipped";
	if (s->role == 2)
		return "a replica lagging past upstream_max_lag_ms - skipped";
	if (s->conns_up || s->role == 3)
		return "not a replica (ROLE) - not used (upstream_role = replica)";
	return "unreachable";
}

static void check_upstreams(void)
{
	struct pc_up_health h;
	char key[KEYLEN], t[PC_ALERT_TEXT];
	int i, j, n = pc_upstream_count();

	static unsigned long long chain_seen[8];    /* S384: per collection */

	for (i = 0; i < n; i++) {
		int down, trk, by_design;
		const char *why;

		if (pc_upstream_health(i, &h) != 0)
			continue;
		/* S384: another perfcached read this read-through collection
		 * through us - a daisy-chain, cut here: its misses got what we
		 * hold, nothing fetched onward.  Raised by any such read, cleared
		 * after five quiet minutes */
		if (i < 8) {
			unsigned long long d = h.chained - chain_seen[i];

			chain_seen[i] = h.chained;
			snprintf(key, sizeof key, "chain.%s", h.col);
			snprintf(t, sizeof t, "another perfcached reads collection %.20s "
				"through this node, which reads it through its own "
				"upstream: %llu reads in the last check, answered from "
				"what this node holds", h.col, d);
			cond(key, d > 0, PC_AL_WARNING, 0, S(300), t);
		}
		if (h.through) {
			/* S385: write-through.  No target: every write is refused
			 * - critical, clients are getting errors */
			static const char *const wsn[] = { "", "starting",
				"no listed upstream says master",
				"two say master and no replica names one",
				"the master stopped answering" };
			int nt = h.w_state > 1 && h.w_state <= 4;
			char wk[160];
			long long pw, fw;

			snprintf(key, sizeof key, "write.%s", h.col);
			snprintf(t, sizeof t, nt ? "collection %.20s writes through to "
				"its master and has none - %s; every write is refused" :
				"collection %.20s writes through to its master again%s",
				h.col, nt ? wsn[h.w_state] : "");
			cond(key, nt, PC_AL_CRITICAL, S(10), S(10), t);
			/* no RedisJSON there: JSON writes are refused */
			snprintf(key, sizeof key, "write.%s.json", h.col);
			snprintf(t, sizeof t, "the master of collection %.20s has no "
				"RedisJSON - JSON writes are refused, not forwarded", h.col);
			cond(key, h.w_json == 0 && h.w_target >= 0, PC_AL_WARNING,
				S(10), S(60), t);
			/* passed-through writes over a tenth of the forwarded ones in
			 * five minutes: a client relies on commands this node only
			 * relays - worth knowing which */
			snprintf(wk, sizeof wk, "col.%.64s.upw.passed_writes", h.col);
			pw = pc_win_delta(wk, h.w_passed, 0, NULL);
			snprintf(wk, sizeof wk, "col.%.64s.upw.forwarded", h.col);
			fw = pc_win_delta(wk, h.w_forwarded, 0, NULL);
			snprintf(key, sizeof key, "write.%s.passed", h.col);
			snprintf(t, sizeof t, "collection %.20s: %lld of %lld writes in "
				"5 min were commands perfcached does not know, passed "
				"through to Redis - a client relies on them",
				h.col, pw < 0 ? 0 : pw, fw < 0 ? 0 : fw);
			cond(key, pw >= 0 && fw >= 20 && pw * 10 > fw, PC_AL_WARNING,
				S(60), S(300), t);
			/* AR2: writes refused at a full write-through queue, or
			 * answered "not applied" after waiting out
			 * upstream_timeout_ms - the master is slower than the
			 * writes arriving at it */
			{
				long long qf, qe;

				/* counters that start with the process: from 0,
				 * or a node up under a minute could never say it */
				snprintf(wk, sizeof wk, "col.%.64s.upw.queue_full", h.col);
				qf = pc_win_delta(wk, h.w_qfull, 1, NULL);
				snprintf(wk, sizeof wk, "col.%.64s.upw.queue_expired",
					h.col);
				qe = pc_win_delta(wk, h.w_qexpired, 1, NULL);
				snprintf(key, sizeof key, "write.%s.queue", h.col);
				snprintf(t, sizeof t, "collection %.20s, 5 min: %lld writes "
					"refused (queue full), %lld not applied (waited too "
					"long) - the master is slower than the writes",
					h.col, qf < 0 ? 0 : qf, qe < 0 ? 0 : qe);
				cond(key, qf > 0 || qe > 0, PC_AL_WARNING, S(10), S(300), t);
			}
			/* AR1: without broadcast tracking a change another Redis
			 * client makes to a key written through here is not seen */
			snprintf(key, sizeof key, "write.%s.tracking", h.col);
			snprintf(t, sizeof t, "collection %.20s writes through but "
				"does not follow other clients' changes in Redis "
				"(broadcast tracking %s)", h.col,
				h.w_bcast < 0 ? "refused" : "off");
			cond(key, h.w_bcast != 1, PC_AL_WARNING, S(60), S(60), t);
		}
		if (h.nsrv <= 1) {
			/* one upstream: 0.6.0's alert, its reason said (S382) */
			why = up_why(&h.srv[0]);
			down = h.conns_up == 0 || h.srv[0].state == PC_UPS_OFF;
			trk = h.tracking_wanted && !h.tracking_on;
			snprintf(key, sizeof key, "upstream.%s", h.col);
			if (down)
				snprintf(t, sizeof t, "the read-through upstream %.32s:%d "
					"of collection %.20s is %.60s - misses answer per "
					"on_error", h.host, h.port, h.col,
					why ? why : "unreachable");
			else
				snprintf(t, sizeof t, "tracking with the read-through "
					"upstream %.60s:%d of collection %s is off - copies "
					"are believed for upstream_negative_ms only",
					h.host, h.port, h.col);
			cond(key, down || trk, PC_AL_WARNING, S(60), S(60), t);
			continue;
		}
		/* S382: several - each its own warning; none serving is
		 * critical; the master serving because no replica does is a
		 * warning for as long as it lasts */
		for (j = 0; j < h.nsrv; j++) {
			const struct pc_up_srv_health *u = &h.srv[j];

			if (u->state == PC_UPS_UNKNOWN)
				continue;              /* not reached yet: not judged */
			/* a master under upstream_role = replica is skipped by
			 * design - listed so the collection follows the upstream's
			 * failover.  The page shows it; no alert (.master below
			 * covers it serving as the fallback) */
			by_design = u->state == PC_UPS_OFF && u->role == 1 &&
				!u->perfcached;
			why = up_why(u);
			trk = h.tracking_wanted && !u->tracking_on &&
				u->state != PC_UPS_OFF;
			snprintf(key, sizeof key, "upstream.%s.%d", h.col, j + 1);
			if (why)
				snprintf(t, sizeof t, "upstream %.32s:%d of read-through "
					"collection %.20s is %.60s - the others take its "
					"fetches", u->host, u->port, h.col, why);
			else
				snprintf(t, sizeof t, "tracking with upstream %.60s:%d "
					"of read-through collection %s is off - its copies are "
					"believed for upstream_negative_ms only", u->host,
					u->port, h.col);
			cond(key, (why != NULL && !by_design) || trk, PC_AL_WARNING,
				S(60), S(60), t);
		}
		snprintf(key, sizeof key, "upstream.%s", h.col);
		snprintf(t, sizeof t, "no upstream of read-through collection %s "
			"serves (%d configured) - misses are answered as its on_error "
			"says", h.col, h.nsrv);
		cond(key, !h.serving && !h.fallback, PC_AL_CRITICAL, S(60), S(60), t);
		/* S382 part 2: failed over - the primary not serving and
		 * another active; cleared once the primary serves again
		 * (with upstream_preempt it then takes over) */
		snprintf(key, sizeof key, "upstream.%s.failover", h.col);
		if (h.failover && h.active >= 0 && h.active != h.primary) {
			const struct pc_up_srv_health *pp = &h.srv[h.primary],
				*aa = &h.srv[h.active];

			why = up_why(pp);
			snprintf(t, sizeof t, "collection %.20s failed over to "
				"%.24s:%d - its primary %.24s:%d is %.60s", h.col,
				aa->host, aa->port, pp->host, pp->port,
				why ? why : "not serving");
			cond(key, pp->state != PC_UPS_SERVE, PC_AL_WARNING, S(10),
				S(10), t);
		} else
			cond(key, 0, PC_AL_WARNING, S(10), S(10), "");
		snprintf(key, sizeof key, "upstream.%s.master", h.col);
		if (h.fallback)
			snprintf(t, sizeof t, "read-through collection %s is reading "
				"from the master %.60s:%d: no replica serves "
				"(upstream_master_fallback = yes)", h.col,
				h.fallback->host, h.fallback->port);
		else
			snprintf(t, sizeof t, "read-through collection %s reads from "
				"its replicas again", h.col);
		cond(key, h.fallback != NULL, PC_AL_WARNING, S(10), S(10), t);
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
			if (!inet_ntop(AF_INET, &m->addr, A.self_ip, sizeof A.self_ip))
				A.self_ip[0] = 0;          /* S377: the mail's header */
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
	A.fleet_size = size;                   /* S377: the mail's header */
	A.fleet_ready = ready;
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

/* AR11 (DESIGN 12ms, AR6): a setting the fleet must agree on that a member
 * holds differently.  Per setting, the value most live members hold is the
 * fleet's (a tie: this node's); every member holding another is named, on
 * every node.  Members that report no settings (before 0.6.5) are left out */
static void check_config(void)
{
	const struct pc_cfgsig *e;
	char key[KEYLEN], t[PC_ALERT_TEXT], who[96], ab[INET_ADDRSTRLEN];
	int ne, n, i, j, k;

	if (!pc_cluster_enabled() || pc_node_state() != PC_NST_READY)
		return;
	ne = pc_cluster_cfgsig(&e);
	if (ne <= 0)
		return;
	if (!A.mem && !(A.mem = malloc(sizeof *A.mem * PC_CL_MAXMEMBERS)))
		return;
	n = pc_cluster_members(A.mem, PC_CL_MAXMEMBERS);
	for (i = 0; i < ne; i++) {
		uint32_t maj = e[i].val_h;
		int best = 0, total = 0, ndiff = 0, self_differs = 0;
		size_t wl = 0;

		/* the most held value; a tie keeps this node's */
		for (j = 0; j < n; j++) {
			const struct pc_member *m = &A.mem[j];
			uint32_t v = 0;
			int has = 0, c = 0;

			for (k = 0; k < m->ncfg; k++)
				if (m->cfg_name[k] == e[i].name_h) {
					v = m->cfg_val[k];
					has = 1;
					break;
				}
			if (!has || !m->live || m->gone_s >= 0)
				continue;
			total++;
			for (k = 0; k < n; k++) {
				const struct pc_member *o = &A.mem[k];
				int q;

				if (!o->live || o->gone_s >= 0)
					continue;
				for (q = 0; q < o->ncfg; q++)
					if (o->cfg_name[q] == e[i].name_h) {
						c += o->cfg_val[q] == v;
						break;
					}
			}
			if (c > best || (c == best && v == e[i].val_h)) {
				best = c;
				maj = v;
			}
		}
		who[0] = 0;
		for (j = 0; j < n; j++) {
			const struct pc_member *m = &A.mem[j];

			if (!m->live || m->gone_s >= 0)
				continue;
			for (k = 0; k < m->ncfg; k++)
				if (m->cfg_name[k] == e[i].name_h)
					break;
			if (k == m->ncfg || m->cfg_val[k] == maj)
				continue;
			ndiff++;
			if (m->is_self) {
				self_differs = 1;
				continue;
			}
			if (!inet_ntop(AF_INET, &m->addr, ab, sizeof ab))
				snprintf(ab, sizeof ab, "?");
			if (wl < sizeof who - 1)
				wl += (size_t)snprintf(who + wl, sizeof who - wl, "%snode %d "
					"(%s)", wl ? ", " : "", m->node, ab);
			if (wl > sizeof who - 1)
				wl = sizeof who - 1;
		}
		snprintf(key, sizeof key, "config.%.40s", e[i].name);
		if (!ndiff) {
			cond(key, 0, 0, 0, S(60), "");
			continue;
		}
		/* bounded so the longest fits PC_ALERT_TEXT whole */
		if (self_differs)
			snprintf(t, sizeof t, "%.48s differs here (%.40s) from %d of "
				"the fleet's %d members", e[i].what, e[i].val, best, total);
		else
			snprintf(t, sizeof t, "%.48s differs on %.60s - this node and "
				"%d other(s) hold %.30s", e[i].what, who, best - 1,
				e[i].val);
		cond(key, 1, PC_AL_WARNING, S(60), S(60), t);
	}
}

/* S436: the autoscale modes set at runtime are fleet state - announced, and
 * re-sent to every member every few seconds.  Raised while a live member
 * reports another digest of them for longer than that heals (held 30 s), or
 * while modes are set here and a member too old to take them is in the
 * fleet.  A member that does not report the digest (its settings take
 * every keepalive slot) is not compared. */
static void check_autoscale(void)
{
	char t[PC_ALERT_TEXT], dif[96], old[96], ab[INET_ADDRSTRLEN];
	size_t dl = 0, ol = 0;
	uint32_t mine, v, f;
	int n, j, nset = 0, nd = 0, no = 0;

	if (!pc_cluster_enabled() || pc_node_state() != PC_NST_READY)
		return;
	if (!A.mem && !(A.mem = malloc(sizeof *A.mem * PC_CL_MAXMEMBERS)))
		return;
	mine = pc_store_autoscale_sig(&nset);
	n = pc_cluster_members(A.mem, PC_CL_MAXMEMBERS);
	dif[0] = old[0] = 0;
	for (j = 0; j < n; j++) {
		const struct pc_member *m = &A.mem[j];
		char *w;
		size_t *wl;

		if (m->is_self || !m->live || m->gone_s >= 0)
			continue;
		if (!pc_member_fact(m, PC_FACT_FEATURES, &f) ||
		        !(f & PC_FEAT_AUTOSCALE)) {
			if (!nset)
				continue;          /* nothing here it would miss */
			no++;
			w = old;
			wl = &ol;
		} else if (pc_member_fact(m, PC_FACT_AUTOSCALE, &v) && v != mine) {
			nd++;
			w = dif;
			wl = &dl;
		} else
			continue;
		if (!inet_ntop(AF_INET, &m->addr, ab, sizeof ab))
			snprintf(ab, sizeof ab, "?");
		if (*wl < sizeof dif - 1)
			*wl += (size_t)snprintf(w + *wl, sizeof dif - *wl, "%snode %d "
				"(%s)", *wl ? ", " : "", m->node, ab);
		if (*wl > sizeof dif - 1)
			*wl = sizeof dif - 1;
	}
	/* bounded so the longest fits PC_ALERT_TEXT whole */
	if (nd)
		snprintf(t, sizeof t, "the autoscale modes set at runtime differ "
			"on %.95s from this node's (%d set here)", dif, nset);
	else
		snprintf(t, sizeof t, "%.95s cannot take the autoscale modes set "
			"at runtime (a build before 0.7.2) - %d set here", old, nset);
	cond("cluster.autoscale", nd || no, PC_AL_WARNING, S(30), S(60),
		nd || no ? t : "");
}

/* AR15: the tombstone table's limits - deletes its cap dropped before
 * tombstone_retain_s, and a member back after a silence longer than the
 * table covers.  Either way a delete may come back from a returning copy */
/* S443: a count clamped into 32 bits, for a text that must fit */
static unsigned int u32c(long long v)
{
	return v < 0 ? 0u : v > 4294967295LL ? 4294967295u : (unsigned int)v;
}

static void check_tombstones(void)
{
	struct pc_tomb_alert ta;
	char t[PC_ALERT_TEXT];
	long long de, nd, dx;

	if (!pc_cluster_enabled() || !pc_cluster_tomb_alert(&ta))
		return;
	/* counters that start with the process: from 0 (re-cut 1's lesson -
	 * otherwise a node up under a minute never says it) */
	de = pc_win_delta("cluster.tomb_dropped_early", ta.dropped_early, 1, NULL);
	nd = pc_win_delta("cluster.tomb_noted", ta.noted, 1, NULL);
	/* S443: how many of them were EXPIRIES (an eager collection's reaped
	 * keys) - it said "deletes" for all of them, and called the 5-minute
	 * count of notes the level the cap was "full at" */
	dx = pc_win_delta("cluster.tomb_dropped_early_exp",
		ta.dropped_early_expiries, 1, NULL);
	if (de < 0)
		de = 0;
	if (dx < 0 || dx > de)
		dx = dx < 0 ? 0 : de;
	if (nd < 0)
		nd = 0;
	/* 5-minute counts, as 32-bit figures so the longest text fits
	 * PC_ALERT_TEXT whole (4 x 20 digits did not) */
	snprintf(t, sizeof t, "5 min: %u forgotten before tombstone_retain_s "
		"(%u s) - %u deletes, %u expiries; %u noted in those 5 min "
		"against tombstone_retain_max %u", u32c(de), ta.retain_s,
		u32c(de - dx), u32c(dx), u32c(nd), ta.max);
	cond("cluster.tombstones", de > 0, PC_AL_WARNING, S(10), S(300), t);
	/* 12ny AT-11: a member whose mark of an origin's deletes stands still
	 * while the others confirm - what this alert used to show only once
	 * the cap bit, by its symptom */
	{
		struct pc_ack_hold hv[PC_ACK_HOLD_MAX];
		int hn = pc_cluster_acks_holds(hv, PC_ACK_HOLD_MAX), j, w = -1;

		for (j = 0; j < hn; j++)
			if (hv[j].stuck_node && (w < 0 || hv[j].stuck_s > hv[w].stuck_s))
				w = j;
		if (w >= 0)
			snprintf(t, sizeof t, "node %d has confirmed none of origin "
				"%016llx's deletes for %u s while the others have - "
				"%llu behind, every one kept for it", hv[w].stuck_node,
				(unsigned long long)hv[w].oid, hv[w].stuck_s,
				(unsigned long long)(hv[w].head - hv[w].stuck_mark));
		else
			t[0] = 0;
		cond("cluster.acks_stuck", w >= 0, PC_AL_WARNING, S(10), S(60), t);
	}
	if (ta.gaps > A.tomb_gaps) {
		A.tomb_gaps = ta.gaps;
		event("cluster.peer_absent", PC_AL_WARNING, ta.gap_text);
	}
}

/* S352c: one relay from another node - this node mails it, by its own
 * [mail] level, naming the origin, and keeps it in its ring */
static void relay_in(const struct pc_alert_relay *r)
{
	struct aring *e = ring_next();
	int lvl = r->level >= PC_AL_CRITICAL ? PC_AL_CRITICAL : r->level >=
		PC_AL_WARNING ? PC_AL_WARNING : PC_AL_INFO;

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
		digest_relay(r, lvl);                   /* S377 */
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
	struct hm_obs ho;
	unsigned long long dropped;
	int i, nin, busy, tcl;
	char tct[PC_ALERT_TEXT];

	/* the ALIVE bit first: what this node can do, then who mails */
	pc_cluster_set_can_mail(pc_mail_ready());
	pthread_mutex_lock(&IB.mx);
	nin = IB.n;
	memcpy(in, IB.q, (size_t)nin * sizeof in[0]);
	IB.n = 0;
	dropped = IB.dropped;
	IB.dropped = 0;
	pthread_mutex_unlock(&IB.mx);

	hm_read(&ho, mono_ms());               /* S380: the files, unlocked */
	tcl = test_cond_read(tct, sizeof tct); /* S377: a suite's, unlocked too */
	/* S379: its own lock, not ours; the minute shrinks under
	 * PERFCACHED_ALERT_FAST like S380's, to 3 samples at the least */
	busy = pc_iobusy_max_pct(A.fast > 1 ? (60 / A.fast > 3 ? 60 / A.fast : 3)
		: 60);
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
	check_host(&ho);                       /* S380 */
	check_io(busy);                        /* S379 */
	if (tcl >= 0)
		cond("test.cond", tcl > 0, tcl > 0 ? tcl : PC_AL_WARNING, 0, S(60),
			tct[0] ? tct : "the test condition");
	check_upstreams();
	check_fleet();
	check_tombstones();                       /* AR15 */
	check_config();                           /* AR11 */
	check_autoscale();                        /* S436 */
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
	/* S380: what the high-water alarms measure, and their thresholds */
	pc_tw_key(out, "host");
	pc_tw_map(out);
	tw_pct(out, "cpu_busy_pct", HM.cpu_pct);
	tw_pct(out, "cpu_perfcached_pct", HM.cpu_self_pct);
	tw_pct(out, "cpus", HM.ncpu ? HM.ncpu : -1);
	tw_pct(out, "disk_busy_pct", HM.io_pct);
	pc_tw_key(out, "disk");
	pc_tw_str(out, HM.io_ok ? HM.dev : "");
	pc_tw_key(out, "disk_path");
	pc_tw_str(out, HM.path);
	pc_tw_key(out, "disk_unwatched");
	pc_tw_str(out, HM.io_ok ? "" : HM.io_why);
	tw_pct(out, "memory_used_pct", HM.mem_pct);
	pc_tw_key(out, "thresholds");
	pc_tw_map(out);
	tw_pct(out, "cpu_warn_pct", A.al.cpu_warn);
	tw_pct(out, "cpu_crit_pct", A.al.cpu_crit);
	tw_pct(out, "io_warn_pct", A.al.io_warn);
	tw_pct(out, "io_crit_pct", A.al.io_crit);
	tw_pct(out, "memory_warn_pct", A.al.mem_warn);
	tw_pct(out, "memory_crit_pct", A.al.mem_crit);
	tw_pct(out, "arena_warn_pct", A.al.arena_warn);
	tw_pct(out, "arena_crit_pct", A.al.arena_crit);
	tw_pct(out, "epoll_busy_pct", A.al.epoll_busy);     /* S379 */
	pc_tw_end(out);
	pc_tw_end(out);
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
