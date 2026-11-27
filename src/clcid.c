/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clcid.c - the cluster id's lifecycle.  See clcid.h. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sodium.h>

#include "compat/dprint.h"
#include "clwire.h"
#include "statefile.h"
#include "clcid.h"

#define CID_HEADER "perfcached-cluster-id 1"
/* a third line: written at mint or adopt, cleared at promotion; older builds read two lines */
#define CID_PROV   "provisional"

static struct {
	char dir[512];
	unsigned char cid[16];
	int state;                     /* CLWIRE_CID_* */
	long long since_ms;            /* when it became provisional; -1 = at the next tick */
	int est_asked;                 /* the promotion's write is asked */
} K;

/* what the file should hold, written by pc-maint (pc_cid_persist_tick) in the order asked: the fsync
 * waits on storage, and the cluster thread answers every pull and forward */
static pthread_mutex_t pmx = PTHREAD_MUTEX_INITIALIZER;
static struct {
	unsigned char cid[16];
	int have;                      /* 0 = no file */
	int prov;                      /* written with the provisional line */
	const char *what;              /* "new", "adopted": for the log */
	unsigned int gen, tried;       /* asked, attempted */
} P;

static int is_zero(const unsigned char *b)
{
	int i;

	for (i = 0; i < 16; i++)
		if (b[i])
			return 0;
	return 1;
}

void pc_cid_str(const unsigned char cid[16], char out[37])
{
	static const char hx[] = "0123456789abcdef";
	int i, o = 0;

	if (!cid || is_zero(cid)) {
		out[0] = 0;
		return;
	}
	for (i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			out[o++] = '-';
		out[o++] = hx[cid[i] >> 4];
		out[o++] = hx[cid[i] & 15];
	}
	out[o] = 0;
}

const char *pc_cid_state_name(int state)
{
	return state == CLWIRE_CID_ESTABLISHED ? "established" :
		state == CLWIRE_CID_PROVISIONAL ? "provisional" : "none";
}

/* 1 and its bytes in @out when @s is exactly 32 hex digits, dashes anywhere; else 0 */
static int uuid_bytes(const char *s, unsigned char out[16])
{
	unsigned char b[16];
	int i = 0, hi = -1;
	const char *p;

	for (p = s; *p && i < 16; p++) {
		int v;

		if (*p == '-')
			continue;
		if (*p >= '0' && *p <= '9') v = *p - '0';
		else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
		else break;
		if (hi < 0)
			hi = v;
		else {
			b[i++] = (unsigned char)(hi << 4 | v);
			hi = -1;
		}
	}
	if (i != 16 || *p || hi >= 0)
		return 0;
	memcpy(out, b, 16);
	return 1;
}

/* a UUID (with or without dashes) parses to its bytes; anything else is a name
 * hashed into a v8 UUID, so `cluster_id = billing-prod` works too */
static void pin_bytes(const char *pin, unsigned char out[16])
{
	if (uuid_bytes(pin, out))
		return;
	crypto_generichash(out, 16, (const unsigned char *)pin, strlen(pin),
		(const unsigned char *)"perfcached-cid", 14);
	out[6] = (unsigned char)((out[6] & 0x0f) | 0x80);   /* a v8 UUID */
	out[8] = (unsigned char)((out[8] & 0x3f) | 0x80);   /* (RFC 9562) */
}

static int cid_store(const unsigned char cid[16], int prov)
{
	char buf[96], s[37];
	int n;

	if (!K.dir[0])
		return 1;                      /* nothing persists: said at init */
	pc_cid_str(cid, s);
	n = snprintf(buf, sizeof buf, CID_HEADER "\n%s\n%s", s,
		prov ? CID_PROV "\n" : "");
	return pc_state_write(K.dir, PC_CID_FILE, buf, (size_t)n) == 0;
}

/* @cid, or NULL for no file */
static void cid_store_later(const unsigned char *cid, int prov, const char *what)
{
	if (!K.dir[0])
		return;
	pthread_mutex_lock(&pmx);
	if (cid)
		memcpy(P.cid, cid, 16);
	P.have = cid != NULL;
	P.prov = prov;
	P.what = what;
	P.gen++;
	pthread_mutex_unlock(&pmx);
}

int pc_cid_persist_tick(void)
{
	static unsigned int said;
	static int failed;
	unsigned char c[16];
	char path[600];
	const char *what;
	unsigned int g;
	int have, prov, ok;

	pthread_mutex_lock(&pmx);
	g = P.gen;
	have = P.have;
	prov = P.prov;
	what = P.what;
	memcpy(c, P.cid, 16);
	pthread_mutex_unlock(&pmx);
	if (g == __atomic_load_n(&P.tried, __ATOMIC_ACQUIRE) && !failed)
		return 0;
	if (have)
		ok = cid_store(c, prov);
	else {
		snprintf(path, sizeof path, "%s/" PC_CID_FILE, K.dir);
		ok = unlink(path) == 0 || errno == ENOENT;
	}
	__atomic_store_n(&P.tried, g, __ATOMIC_RELEASE);
	failed = !ok;
	if (ok)
		return 1;
	if (said != g && have)
		LM_WARN("cluster: could not persist the %s cluster id (%s) - retrying\n",
			what, strerror(errno));
	else if (said != g)
		LM_WARN("cluster: could not remove the given-up cluster id from %s (%s) - "
			"retrying\n", K.dir, strerror(errno));
	said = g;
	return -1;
}

/* 1 established, 2 provisional, 0 none or unreadable, -1 a newer build's */
static int cid_load(unsigned char out[16])
{
	char path[600], buf[128];
	FILE *f;
	int ok = 0;

	if (!K.dir[0])
		return 0;
	snprintf(path, sizeof path, "%s/" PC_CID_FILE, K.dir);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (!fgets(buf, sizeof buf, f))
		buf[0] = 0;
	if (!strncmp(buf, "perfcached-cluster-id ", 22) && atoi(buf + 22) > 1) {
		/* a newer header is a downgrade, not damage: "join or found as if new" would
		 * put this node in ANOTHER cluster */
		LM_CRIT("cluster: %s is version %d, written by a NEWER build (this "
			"one reads version 1) - refusing to start rather than join "
			"or found a cluster as if new; run the newer build\n", path,
			atoi(buf + 22));
		fclose(f);
		return -1;
	}
	if (!strncmp(buf, CID_HEADER, strlen(CID_HEADER)) &&
	        fgets(buf, sizeof buf, f)) {
		buf[strcspn(buf, "\r\n")] = 0;
		ok = uuid_bytes(buf, out) && !is_zero(out);
		if (ok && fgets(buf, sizeof buf, f)) {
			buf[strcspn(buf, "\r\n")] = 0;
			if (!strcmp(buf, CID_PROV))
				ok = 2;
		}
	}
	fclose(f);
	if (!ok)
		LM_WARN("cluster: %s is unreadable - ignored, this node will "
			"join or found as if new\n", path);
	return ok;
}

static void set(const unsigned char cid[16], int state, long long now)
{
	memcpy(K.cid, cid, 16);
	K.state = state;
	K.since_ms = now;
	K.est_asked = 0;
	clwire_set_cid(cid, state);
}

int pc_cid_init(const char *state_dir, const char *pin)
{
	unsigned char disk[16], want[16];
	char a[37], b[37];
	int have;

	memset(&K, 0, sizeof K);
	if (state_dir && *state_dir)
		snprintf(K.dir, sizeof K.dir, "%s", state_dir);
	have = cid_load(disk);
	if (have < 0)
		return -1;                 /* a newer build's file */
	if (pin && *pin) {
		pin_bytes(pin, want);
		if (have && memcmp(disk, want, 16) != 0) {
			pc_cid_str(disk, a);
			pc_cid_str(want, b);
			LM_CRIT("cluster: [cluster] cluster_id pins %s but this "
				"node's state directory holds cluster %s - it "
				"belongs to another cluster.  Refusing to start: "
				"delete %s/" PC_CID_FILE " to move it, or fix the "
				"pin\n", b, a, K.dir);
			return -1;
		}
		if (!cid_store(want, 0))
			LM_WARN("cluster: could not persist the pinned cluster "
				"id\n");
		set(want, CLWIRE_CID_ESTABLISHED, 0);
	} else if (have == 2) {
		/* founded or joined inside its grace: it still yields, its window started again */
		set(disk, CLWIRE_CID_PROVISIONAL, -1);
	} else if (have) {
		set(disk, CLWIRE_CID_ESTABLISHED, 0);
	} else {
		clwire_set_cid(NULL, CLWIRE_CID_NONE);
	}
	if (!K.dir[0])
		LM_WARN("cluster: no state directory - the cluster id is not "
			"persisted, so a restart joins or founds as if new\n");
	if (K.state) {
		pc_cid_str(K.cid, a);
		LM_NOTICE("cluster: cluster id %s (%s%s)\n", a,
			pin && *pin ? "pinned" : "from the state directory",
			K.state == CLWIRE_CID_PROVISIONAL ? ", provisional" : "");
	}
	return 0;
}

void pc_cid_mint(long long now_ms)
{
	unsigned char c[16];
	char s[37];
	struct timespec ts;
	unsigned long long ms;
	int i;

	if (K.state != CLWIRE_CID_NONE)
		return;
	/* a v7 UUID (RFC 9562): 48 bits of Unix milliseconds, then 74
	 * random - the id says when its cluster was founded */
	clock_gettime(CLOCK_REALTIME, &ts);
	ms = (unsigned long long)ts.tv_sec * 1000ULL +
		(unsigned long long)(ts.tv_nsec / 1000000);
	randombytes_buf(c, sizeof c);
	for (i = 0; i < 6; i++)
		c[i] = (unsigned char)(ms >> (40 - 8 * i));
	c[6] = (unsigned char)((c[6] & 0x0f) | 0x70);
	c[8] = (unsigned char)((c[8] & 0x3f) | 0x80);
	cid_store_later(c, 1, "new");
	set(c, CLWIRE_CID_PROVISIONAL, now_ms);
	pc_cid_str(c, s);
	LM_NOTICE("cluster: founded cluster %s (provisional for %d s - a "
		"founder that hears another in that time yields to it)\n", s,
		PC_CID_GRACE_MS / 1000);
}

void pc_cid_adopt(const unsigned char cid[16], long long now_ms)
{
	char s[37];

	if (!cid || is_zero(cid) || K.state == CLWIRE_CID_ESTABLISHED)
		return;
	if (K.state != CLWIRE_CID_NONE && !memcmp(K.cid, cid, 16))
		return;
	cid_store_later(cid, 1, "adopted");
	set(cid, CLWIRE_CID_PROVISIONAL, now_ms);
	pc_cid_str(cid, s);
	LM_NOTICE("cluster: joined cluster %s\n", s);
}

void pc_cid_drop(void)
{
	if (K.state != CLWIRE_CID_PROVISIONAL)
		return;
	cid_store_later(NULL, 0, NULL);             /* after any write still due, never before it */
	memset(K.cid, 0, sizeof K.cid);
	K.state = CLWIRE_CID_NONE;
	clwire_set_cid(NULL, CLWIRE_CID_NONE);
}

void pc_cid_tick(long long now_ms)
{
	if (K.state != CLWIRE_CID_PROVISIONAL)
		return;
	if (K.since_ms < 0)
		K.since_ms = now_ms;
	if (now_ms - K.since_ms < PC_CID_GRACE_MS)
		return;
	/* established only once the file without the provisional line is written */
	if (K.dir[0] && !K.est_asked) {
		cid_store_later(K.cid, 0, "established");
		K.est_asked = 1;
	}
	if (!K.dir[0] || __atomic_load_n(&P.tried, __ATOMIC_ACQUIRE) == P.gen) {
		K.state = CLWIRE_CID_ESTABLISHED;
		clwire_set_cid(K.cid, CLWIRE_CID_ESTABLISHED);
	}
}

int pc_cid_get(unsigned char out[16])
{
	memcpy(out, K.cid, 16);
	return K.state;
}
