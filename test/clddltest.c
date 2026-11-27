/* clddltest.c - the collection DDL across the fleet (clddl.c): announces, resize repeats, the set
 * for a newly seen peer and the receive side, over stubbed clstate.h, store and RESP map. */
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "compat/dprint.h"
#include "store.h"
#include "respmap.h"
#include "cluster.h"
#include "clcol.h"
#include "clmemb.h"
#include "clmsg.h"
#include "clpeers.h"
#include "clstate.h"
#include "clddl.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs: clstate.h, the clock, the sends, the store, the map, the log ---------- */
static struct clpeers PT;
static long long now = 1000000;
static int enabled = 1, no_autoscale, as_deaf;
static unsigned long long lamport = 500;
static char logbuf[4096];

struct clpeers *cl_peers(void) { return &PT; }
long long cl_now_ms(void) { return now; }
int cl_enabled(void) { return enabled; }
int cl_self_no_autoscale(void) { return no_autoscale; }
int cl_self_as_deaf(void) { return as_deaf; }
unsigned long long pc_lamport_tick(void) { return ++lamport; }
unsigned long long pc_lamport_now(void) { return lamport; }
void pc_lamport_observe(unsigned long long seen) { if (seen > lamport) lamport = seen; }
int compat_log_level = L_NOTICE;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	size_t w = strlen(logbuf);
	va_list ap;

	(void)level; (void)tag;
	if (w + 256 > sizeof logbuf)
		return;
	va_start(ap, fmt);
	vsnprintf(logbuf + w, sizeof logbuf - w, fmt, ap);
	va_end(ap);
}

/* every frame sent: to whom (0 = every live peer), and what it said */
static struct sent { int to; struct clcol_ann a; char name[80], to_name[80]; } sent[64];
static int n_sent;

static void note_sent(int to, const unsigned char *msg, size_t n)
{
	struct sent *s = &sent[n_sent < 64 ? n_sent++ : 63];

	memset(s, 0, sizeof *s);
	s->to = to;
	if (n < 1 || msg[0] != M_COL_SET || !clcol_parse(msg, n, &s->a)) {
		s->a.op = -1;
		return;
	}
	memcpy(s->name, s->a.name, s->a.nlen);
	if (s->a.to)
		memcpy(s->to_name, s->a.to, s->a.tlen);
}

int clsend_live(struct clpeers *pt, const unsigned char *msg, size_t n, long long t)
{
	int i, k = 0;

	for (i = 0; i < pt->n_peers; i++)
		k += clpeers_live(&pt->peers[i], t);
	note_sent(0, msg, n);
	return k;
}

int clsend_seal(const pc_addr *to, const unsigned char *pt, size_t n)
{
	note_sent((int)(ntohl(to->in4.sin_addr.s_addr) & 0xff), pt, n);
	return 0;
}

/* the store: names held, with their size, generation and mode */
static struct col { char name[64]; int bl, live, mode; unsigned long long gen, mgen; } cols[8];
static int n_cols, resize_rc;
static char calls[1024];

static void call(const char *fmt, ...)
{
	size_t w = strlen(calls);
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(calls + w, sizeof calls - w, fmt, ap);
	va_end(ap);
}

static struct col *col_find(const char *name, size_t nlen)
{
	int i;

	for (i = 0; i < n_cols; i++)
		if (cols[i].live && strlen(cols[i].name) == nlen && !memcmp(cols[i].name, name, nlen))
			return &cols[i];
	return NULL;
}

pcache_htable_t *pc_store_find(const char *name, size_t nlen)
{
	return (pcache_htable_t *)(void *)col_find(name, nlen);
}

int pc_store_create_gen(const char *name, size_t nlen, int bl, unsigned long long gen)
{
	call("create %.*s %d %llu;", (int)nlen, name, bl, gen);
	return 0;
}

long pc_store_drop_gen(const char *name, size_t nlen, unsigned long long gen)
{
	call("drop %.*s %llu;", (int)nlen, name, gen);
	return 0;
}

void pc_store_note_drop(const char *name, size_t nlen, unsigned long long gen)
{
	call("note_drop %.*s %llu;", (int)nlen, name, gen);
}

int pc_store_resize_request(const char *name, size_t nlen, int bl, unsigned long long gen)
{
	call("resize %.*s %d %llu;", (int)nlen, name, bl, gen);
	return resize_rc;
}

int pc_store_rename(const char *from, size_t flen, const char *to, size_t tlen,
		unsigned long long gen)
{
	call("rename %.*s %.*s %llu;", (int)flen, from, (int)tlen, to, gen);
	return 0;
}

/* the receive side marks the files due: a write there would hold the cluster thread (T585) */
static int sync_writes;
void pc_store_persist_later(void) { call("persist;"); }
int pc_store_persist(void) { sync_writes++; return 0; }
void pc_wal_col_rename(const char *from, const char *to) { call("wal_rename %s %s;", from, to); }
int pc_store_count(void) { return n_cols; }

int pc_store_entry(int i, const char **name, int *bl, unsigned long long *gen, int *live)
{
	if (i < 0 || i >= n_cols)
		return -1;
	*name = cols[i].name;
	*bl = cols[i].bl;
	*gen = cols[i].gen;
	*live = cols[i].live;
	return 0;
}

int pc_store_autoscale_entry(int i, const char **name, int *m, unsigned long long *gen)
{
	if (i < 0 || i >= n_cols || !cols[i].live || !cols[i].mgen)
		return -1;
	*name = cols[i].name;
	*m = cols[i].mode;
	*gen = cols[i].mgen;
	return 0;
}

int pc_store_autoscale_apply(const char *name, size_t nlen, int m, unsigned long long gen,
		int *was)
{
	call("mode %.*s %d %llu;", (int)nlen, name, m, gen);
	*was = 0;
	return 1;
}

const char *pc_autoscale_name(int m) { return m ? "on" : "off"; }

int pc_respmap_apply(int index, const char *col, size_t cl, uint64_t gen, int set, char *was,
		size_t wcap)
{
	call("map %d %.*s %llu %d;", index, (int)cl, col, (unsigned long long)gen, set);
	snprintf(was, wcap, "%s", "");
	return 1;
}

void pc_respmap_persist_later(void) { call("map_persist;"); }
int pc_respmap_persist(void) { sync_writes++; return 0; }

void pc_respmap_each(pc_respmap_cb cb, void *ctx)
{
	cb(2, "users", 77, 1, ctx);
}

/* ---- the fleet: peers 11, 12 (both features) and 13 (none); 14 silent ---------------- */
static struct peer *add_peer(int last, unsigned features, int live)
{
	struct peer *p = &PT.peers[PT.n_peers++];

	memset(p, 0, sizeof *p);
	pc_addr_from_v4(&p->addr, htonl(0x0a000000u | (unsigned)last), 6480);
	p->node = last;
	p->features = features;
	p->last_seen_ms = live ? now : now - 2 * PEER_UP_MS;
	return p;
}

/* 11-13 heard just now, 14 still silent */
static void beat(void)
{
	int i;

	for (i = 0; i < 3; i++)
		PT.peers[i].last_seen_ms = now;
}

static void reset(void)
{
	n_sent = 0;
	calls[0] = logbuf[0] = 0;
}

static int sent_to(int to, int op)
{
	int i, k = 0;

	for (i = 0; i < n_sent; i++)
		k += sent[i].to == to && sent[i].a.op == op;
	return k;
}

static void feed(int op, int bl, unsigned long long gen, const char *name, const char *to)
{
	unsigned char msg[256];
	struct clcol_ann a;

	memset(&a, 0, sizeof a);
	a.op = op;
	a.buckets_log2 = bl;
	a.gen = gen;
	a.name = name;
	a.nlen = (unsigned)strlen(name);
	if (to) {
		a.to = to;
		a.tlen = (unsigned)strlen(to);
	}
	handle_col_set(msg, clcol_write(msg, sizeof msg, M_COL_SET, &a));
}

int main(void)
{
	const unsigned feats = CLMEMB_FEAT_RESPMAP | CLMEMB_FEAT_AUTOSCALE;
	unsigned long long g, g2;
	char long_name[PC_COL_NAME_MAX + 1];
	int i;

	add_peer(11, feats, 1);
	add_peer(12, feats, 1);
	add_peer(13, 0, 1);
	add_peer(14, feats, 0);

	printf("A. the announces\n");
	enabled = 0;
	reset();
	CHK(pc_cluster_col_announce("users", 5, 10, CLCOL_OP_SET) == 0 && !n_sent,
		"A. the cluster off: nothing sent");
	enabled = 1;
	memset(long_name, 'x', sizeof long_name);
	CHK(pc_cluster_col_announce("", 0, 10, CLCOL_OP_SET) == 0 &&
		pc_cluster_col_announce(long_name, PC_COL_NAME_MAX, 10, CLCOL_OP_SET) == 0 && !n_sent,
		"A. an empty name, or one of PC_COL_NAME_MAX: nothing sent");
	CHK(pc_cluster_col_announce("users", 5, 10, CLCOL_OP_RENAME) == 0 && !n_sent &&
		strstr(logbuf, "refusing to announce collection 'users' with op 2"),
		"A. a rename through the one-name call: refused and logged, nothing sent");
	reset();
	g = pc_cluster_col_announce("users", 5, 10, CLCOL_OP_SET);
	CHK(g == 500 && n_sent == 1 && sent[0].to == 0 && sent[0].a.op == CLCOL_OP_SET &&
		sent[0].a.gen == 500 && sent[0].a.buckets_log2 == 10 && !strcmp(sent[0].name, "users"),
		"A. a create: one frame to every live peer, at the clock's value (%llu)", g);
	reset();
	g = pc_cluster_col_announce("users", 5, 12, CLCOL_OP_RESIZE);
	CHK(g == 501 && n_sent == 1 && sent[0].a.op == CLCOL_OP_RESIZE && sent[0].a.gen == 501,
		"A. a resize: at a fresh tick (%llu)", g);
	for (i = 0; i < 9; i++) {
		now += 1000;
		rz_again_tick(now);
	}
	CHK(n_sent == 4 && sent[1].a.gen == 501 && sent[3].a.op == CLCOL_OP_RESIZE &&
		sent[3].a.buckets_log2 == 12,
		"A. repeated at +1, +3 and +7 s, then no more (%d frames in 9 s)", n_sent);
	reset();
	g = pc_cluster_col_announce("users", 5, 13, CLCOL_OP_RESIZE);
	now += 500;
	g2 = pc_cluster_col_announce("users", 5, 14, CLCOL_OP_RESIZE);
	for (i = 0; i < 9; i++) {
		now += 1000;
		rz_again_tick(now);
	}
	CHK(n_sent == 5 && sent[2].a.gen == g2 && sent[4].a.gen == g2 && sent[4].a.buckets_log2 == 14,
		"A. a newer resize of the name takes the older's place: 2 + 3 frames, all repeats "
		"at %llu (not %llu)", g2, g);
	reset();
	g = pc_cluster_col_announce_gen("orders", 6, 9, CLCOL_OP_SET, 480);
	g2 = pc_cluster_col_announce_gen("orders", 6, 0, CLCOL_OP_DROP, 481);
	CHK(g == 480 && g2 == 481 && n_sent == 2 && sent[0].a.gen == 480 && sent[1].a.gen == 481 &&
		sent[1].a.op == CLCOL_OP_DROP,
		"A. a create and a drop go out under the generation the store stamped, not the clock's");
	reset();
	pc_cluster_col_announce2("users", 5, "people", 6);
	CHK(n_sent == 1 && sent[0].a.op == CLCOL_OP_RENAME && !strcmp(sent[0].name, "users") &&
		!strcmp(sent[0].to_name, "people"), "A. a rename: both names in one frame");

	printf("B. the map and the modes, only where they are read\n");
	beat();
	reset();
	pc_cluster_respmap_announce(3, "users", 90, 1);
	CHK(n_sent == 2 && sent_to(11, CLCOL_OP_MAP) == 1 && sent_to(12, CLCOL_OP_MAP) == 1 &&
		!strcmp(sent[0].name, "3") && !strcmp(sent[0].to_name, "users"),
		"B. a map override to 11 and 12; not 13 (does not read it), not 14 (silent)");
	reset();
	pc_cluster_autoscale_announce("users", 5, 1, 91);
	CHK(n_sent == 2 && sent_to(11, CLCOL_OP_AUTOSCALE) == 1 && sent_to(12, CLCOL_OP_AUTOSCALE) == 1,
		"B. a mode to 11 and 12 only");
	no_autoscale = 1;
	reset();
	pc_cluster_autoscale_announce("users", 5, 1, 92);
	CHK(!n_sent, "B. from a member with the S436 test flag: no mode sent");
	no_autoscale = 0;

	printf("C. the set to a peer seen for the first time\n");
	strcpy(cols[0].name, "users");
	cols[0].bl = 10, cols[0].gen = 40, cols[0].live = 1, cols[0].mode = 1, cols[0].mgen = 41;
	strcpy(cols[1].name, "gone");
	cols[1].bl = 8, cols[1].gen = 42, cols[1].live = 0;
	n_cols = 2;
	reset();
	col_sync_peer(&PT.peers[0]);
	CHK(n_sent == 4 && sent[0].a.op == CLCOL_OP_SET && sent[0].a.gen == 40 &&
		sent[1].a.op == CLCOL_OP_DROP && !strcmp(sent[1].name, "gone") && sent[1].a.gen == 42 &&
		sent[2].a.op == CLCOL_OP_MAP && sent[3].a.op == CLCOL_OP_AUTOSCALE && sent[3].a.gen == 41,
		"C. to 11: users (SET 40), gone (DROP 42), the map, the mode (41)");
	reset();
	col_sync_peer(&PT.peers[2]);
	CHK(n_sent == 2 && sent_to(13, CLCOL_OP_SET) == 1 && sent_to(13, CLCOL_OP_DROP) == 1,
		"C. to 13, which reads neither: the two names only");

	printf("D. the receive side\n");
	reset();
	feed(CLCOL_OP_SET, 9, 700, "orders", NULL);
	CHK(strstr(calls, "create orders 9 700;persist;") && lamport == 700 &&
		strstr(logbuf, "collection 'orders' created by the fleet: 2^9 buckets"),
		"D. a create: made, persisted, the clock moved to its generation");
	reset();
	feed(CLCOL_OP_SET, 9, 701, "bad name", NULL);
	CHK(!strstr(calls, "create") && pc_cluster_bad_col_names() == 1 &&
		strstr(logbuf, "by a peer refused"), "D. a name that breaks the rule: counted, never made");
	reset();
	feed(CLCOL_OP_SET, 10, 702, "users", NULL);
	CHK(!strstr(calls, "create"), "D. a create of a name held: nothing made");
	reset();
	feed(CLCOL_OP_DROP, 0, 703, "users", NULL);
	feed(CLCOL_OP_DROP, 0, 704, "nosuch", NULL);
	CHK(strstr(calls, "drop users 703;") && strstr(calls, "note_drop nosuch 704;"),
		"D. a drop: of a held name dropped, of an unknown one noted (%s)", calls);
	reset();
	resize_rc = 2;
	feed(CLCOL_OP_RESIZE, 12, 705, "users", NULL);
	CHK(strstr(calls, "resize users 12 705;") &&
		strstr(logbuf, "the fleet's resize to 2^12 is held"), "D. a resize: requested, its hold logged");
	resize_rc = 0;
	reset();
	feed(CLCOL_OP_RENAME, 0, 706, "users", "people");
	feed(CLCOL_OP_RENAME, 0, 707, "users", "bad name");
	CHK(strstr(calls, "rename users people 706;") && !strstr(calls, "707") &&
		pc_cluster_bad_col_names() == 2, "D. a rename made; one to a name that breaks the rule "
		"refused and counted");
	/* a member's restart maps its snapshot's older name by this record and its file */
	CHK(strstr(calls, "rename users people 706;wal_rename users people;persist;") != NULL,
		"D. a fleet rename is logged to the WAL, then persisted (%s)", calls);
	reset();
	feed(CLCOL_OP_MAP, 1, 708, "3", "users");
	feed(CLCOL_OP_MAP, 1, 709, "64", "users");
	CHK(strstr(calls, "map 3 users 708 1;map_persist;") && !strstr(calls, "709"),
		"D. a map override applied and persisted; index 64 dropped (%s)", calls);
	reset();
	feed(CLCOL_OP_AUTOSCALE, 1, 710, "users", NULL);
	as_deaf = 1;
	feed(CLCOL_OP_AUTOSCALE, 0, 711, "users", NULL);
	CHK(strstr(calls, "mode users 1 710;persist;") && !strstr(calls, "711"),
		"D. a mode applied and persisted; ignored by a deaf member (%s)", calls);
	reset();
	feed(CLCOL_OP_AUTOSCALE + 1, 9, 712, "newcol", NULL);
	CHK(!strstr(calls, "create") && !strstr(calls, "persist") &&
		strstr(logbuf, "a peer announced op 6, which this build does not know"),
		"D. an op this build does not know: ignored and logged, not made (%s)", calls);
	CHK(sync_writes == 0, "D. the receive side wrote no file itself, %d times: pc-maint writes "
		"what it marks due", sync_writes);

	printf("%s\n", fails ? "FAIL" : "PASS");
	return fails ? 1 : 0;
}
