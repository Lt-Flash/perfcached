/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clcid.c - S302: the cluster id's lifecycle.  See clcid.h.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sodium.h>

#include "compat/dprint.h"
#include "clwire.h"
#include "clcid.h"

#define CID_HEADER "perfcached-cluster-id 1"

static struct {
	char dir[512];
	unsigned char cid[16];
	int state;                     /* CLWIRE_CID_* */
	long long since_ms;            /* when it became provisional */
} K;

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

/* a UUID (with or without dashes) parses to its bytes; anything else is
 * a name, hashed into a v8 UUID - so `cluster_id = billing-prod` works as
 * well, and every id is a well-formed UUID (a minted one is v7) */
static void pin_bytes(const char *pin, unsigned char out[16])
{
	unsigned char b[16];
	int i = 0, hi = -1;
	const char *p;

	for (p = pin; *p && i < 16; p++) {
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
	if (i == 16 && !*p && hi < 0) {
		memcpy(out, b, 16);
		return;
	}
	crypto_generichash(out, 16, (const unsigned char *)pin, strlen(pin),
		(const unsigned char *)"perfcached-cid", 14);
	out[6] = (unsigned char)((out[6] & 0x0f) | 0x80);   /* a v8 UUID */
	out[8] = (unsigned char)((out[8] & 0x3f) | 0x80);   /* (RFC 9562) */
}

static int cid_store(const unsigned char cid[16])
{
	char tmp[600], fin[600], buf[96], s[37];
	int fd, dfd, n, ok = 0;

	if (!K.dir[0])
		return 1;                      /* nothing persists: said at init */
	pc_cid_str(cid, s);
	snprintf(tmp, sizeof tmp, "%s/" PC_CID_FILE ".tmp.%d", K.dir, (int)getpid());
	snprintf(fin, sizeof fin, "%s/" PC_CID_FILE, K.dir);
	n = snprintf(buf, sizeof buf, CID_HEADER "\n%s\n", s);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return 0;
	if (write(fd, buf, (size_t)n) == (ssize_t)n && fsync(fd) == 0)
		ok = 1;
	close(fd);
	if (!ok || rename(tmp, fin) != 0) {
		unlink(tmp);
		return 0;
	}
	dfd = open(K.dir, O_RDONLY);
	if (dfd >= 0) {
		fsync(dfd);
		close(dfd);
	}
	return 1;
}

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
	if (fgets(buf, sizeof buf, f) && !strncmp(buf, CID_HEADER, strlen(CID_HEADER)) &&
	        fgets(buf, sizeof buf, f)) {
		buf[strcspn(buf, "\r\n")] = 0;
		pin_bytes(buf, out);
		ok = !is_zero(out);
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
		if (!cid_store(want))
			LM_WARN("cluster: could not persist the pinned cluster "
				"id\n");
		set(want, CLWIRE_CID_ESTABLISHED, 0);
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
		LM_NOTICE("cluster: cluster id %s (%s)\n", a,
			pin && *pin ? "pinned" : "from the state directory");
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
	if (!cid_store(c))
		LM_WARN("cluster: could not persist the new cluster id\n");
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
	if (!cid_store(cid))
		LM_WARN("cluster: could not persist the adopted cluster id\n");
	set(cid, CLWIRE_CID_PROVISIONAL, now_ms);
	pc_cid_str(cid, s);
	LM_NOTICE("cluster: joined cluster %s\n", s);
}

void pc_cid_drop(void)
{
	char path[600];

	if (K.state != CLWIRE_CID_PROVISIONAL)
		return;
	if (K.dir[0]) {
		snprintf(path, sizeof path, "%s/" PC_CID_FILE, K.dir);
		unlink(path);
	}
	memset(K.cid, 0, sizeof K.cid);
	K.state = CLWIRE_CID_NONE;
	clwire_set_cid(NULL, CLWIRE_CID_NONE);
}

void pc_cid_tick(long long now_ms)
{
	if (K.state == CLWIRE_CID_PROVISIONAL &&
	        now_ms - K.since_ms >= PC_CID_GRACE_MS) {
		K.state = CLWIRE_CID_ESTABLISHED;
		clwire_set_cid(K.cid, CLWIRE_CID_ESTABLISHED);
	}
}

int pc_cid_get(unsigned char out[16])
{
	memcpy(out, K.cid, 16);
	return K.state;
}
