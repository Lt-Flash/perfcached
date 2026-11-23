/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* walscan.c - the WAL's reader: segments in generation order, each record's length, checksum and
 * type checked, handed to a callback (replay at start, the suites, perfcached -W's dump). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "core/pcache_htable.h"         /* the record flags */
#include "wal.h"
#include "walfmt.h"

/* ---- scan (tests, -W, replay) ------------------------------------------ */

struct seg_meta {
	int idx;
	uint32_t gen;
};

static int cmp_gen(const void *a, const void *b)
{
	const struct seg_meta *x = a, *y = b;

	return x->gen < y->gen ? -1 : x->gen > y->gen;
}

long pc_wal_scan(const char *dir, pc_wal_scan_cb cb, void *ctx,
		const char **why)
{
	struct seg_meta segs[256];
	char path[512];
	unsigned char h[PC_WAL_HDR], *buf = NULL;
	long count = 0;
	int i, n = 0, fd;

	(void)pc_crc32c_select();          /* the table; hw_ok() picks the same each time */
	*why = "end";

	for (i = 0; i < 256; i++) {
		snprintf(path, sizeof path, "%s/wal-%03d.seg", dir, i);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		if (read(fd, h, sizeof h) == (ssize_t)sizeof h &&
		        r32(h) == PC_WAL_MAGIC) {
			segs[n].idx = i;
			segs[n].gen = r32(h + 4);
			n++;
		}
		close(fd);
	}
	if (!n)
		return 0;
	qsort(segs, (size_t)n, sizeof segs[0], cmp_gen);

	buf = malloc(MAX_REC + 16);
	if (!buf)
		return -1;
	for (i = 0; i < n; i++) {
		long long off = PC_WAL_HDR;

		snprintf(path, sizeof path, "%s/wal-%03d.seg", dir, segs[i].idx);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		for (;;) {
			unsigned char rh[PC_WAL_RECHDR];
			uint32_t len, crc, gen;

			if (pread(fd, rh, sizeof rh, off) != (ssize_t)sizeof rh)
				break;
			len = r32(rh);
			if (len == 0 || len > MAX_REC) {
				if (len != 0)
					*why = "torn";
				break;
			}
			crc = r32(rh + 4);
			gen = r32(rh + 8);
			if (gen != segs[i].gen) {
				*why = "stale-gen";
				break;
			}
			memcpy(buf, rh + 8, 4);        /* gen is crc-covered */
			if (pread(fd, buf + 4, len, off + PC_WAL_RECHDR) !=
			        (ssize_t)len) {
				*why = "torn";
				break;
			}
			if (pc_crc32c(0, buf, 4 + len) != crc) {
				*why = "torn";
				break;
			}
			if (!PC_WAL_T_KNOWN(buf[4])) {
				/* a newer build's type (framing sound): handed on unparsed for the
				 * replay to refuse, not called torn and the rest dropped */
				count++;
				if (cb && cb(buf[4], "", 0, "", 0, "", 0, 0, 0, 0, 0,
				        ctx) < 0) {
					close(fd);
					free(buf);
					return count;
				}
				off += PC_WAL_RECHDR + len;
				continue;
			}
			{
				const unsigned char *p = buf + 4;
				int type = p[0], collen = p[1], klen = r16(p + 2);
				uint32_t vlen = r32(p + 4);
				uint64_t exp = r64(p + 8), seq = r64(p + 16);
				/* type 4 carries the version between seq and the strings; older
				 * types do not, and still replay (numbered 0) */
				unsigned int hdr = PC_WAL_HDRLEN(type);
				uint64_t ver = hdr >= 32u ? r64(p + 24) : 0;
				/* an upsert's type */
				unsigned char flags = hdr == 33u ? p[32] : 0;
				const char *col = (const char *)p + hdr;
				const char *key = col + collen;
				const char *val = key + klen;

				if (len < hdr || (uint32_t)(collen + klen) > len - hdr ||
				        vlen != len - hdr - (uint32_t)(collen + klen)) {
					*why = "torn";
					break;
				}
				count++;
				if (cb && cb(type, col, collen, key, klen, val,
				        (int)vlen, exp, seq, ver, flags, ctx) < 0) {
					close(fd);
					free(buf);
					return count;
				}
			}
			off += PC_WAL_RECHDR + len;
		}
		close(fd);
	}
	free(buf);
	return count;
}

/* ---- -W: human dump (also the test surface) ---------------------------- */

struct dump_ctx {
	long n;
	unsigned long long last_seq;
	int order_ok;
	long unknown;                  /* a newer build's records */
};

static int dump_cb(int type, const char *col, int collen, const char *key,
		int klen, const char *val, int vlen, unsigned long long exp,
		unsigned long long seq, unsigned long long ver,
		unsigned char flags, void *ctx)
{
	struct dump_ctx *d = ctx;
	static const char *tn[] = { "?", "upsert", "del", "touch", "upsert",
		"touch", "col-create", "col-drop", "col-rename", "del-n" };
	int isup = type == PC_WAL_T_UPSERT || type == PC_WAL_T_UPSERT_V;

	(void)val;
	if (!PC_WAL_T_KNOWN(type)) {
		/* not parsed - its header is a newer build's, its seq
		 * unknown, so it neither orders nor breaks the order */
		printf("type=%d unknown: a newer build's record, not parsed\n",
			type);
		d->unknown++;
		return 0;
	}
	if (seq <= d->last_seq && d->n)
		d->order_ok = 0;
	d->last_seq = seq;
	d->n++;
	/* ver= only where the record carries one, so an older log reads as silent about versions
	 * rather than as full of records versioned zero */
	printf("seq=%llu %s %.*s/%.*s vlen=%d exp=%llu", seq,
		tn[type >= 1 && type <= PC_WAL_T_DEL_N ? type : 0], collen, col, klen, key,
		vlen, exp);
	if (PC_WAL_T_HASVER(type))
		printf(" ver=%llu", ver);
	if (flags & PCACHE_F_JSON)
		printf(" type=json");
	else if (flags & PCACHE_F_RL)
		printf(" type=ratelimit");
	else if (flags & PCACHE_F_HASH)
		printf(" type=hash");
	else if (type == PC_WAL_T_DEL && vlen == PC_WAL_DEL_VERLEN)
		printf(" ver=%llu", (unsigned long long)r64((const unsigned char *)val));
	else if (type == PC_WAL_T_DEL_N && vlen == PC_WAL_DELN_VLEN) {
		const unsigned char *u = (const unsigned char *)val;

		printf(" ver=%llu origin=%016llx incarn=%u num=%llu",
			(unsigned long long)r64(u), (unsigned long long)r64(u + 8),
			(unsigned)r32(u + 16),
			(unsigned long long)r64(u + 20));
	}
	printf("\n");
	if (isup && vlen <= 40)
		printf("   val=%.*s\n", vlen, val);
	return 0;
}

int pc_wal_dump(const char *dir)
{
	struct dump_ctx d = { 0, 0, 1, 0 };
	const char *why = "?";
	long n = pc_wal_scan(dir, dump_cb, &d, &why);

	printf("wal-dump: %ld records, stop=%s, seq-order=%s", n, why,
		d.order_ok ? "ascending" : "VIOLATED");
	if (d.unknown)
		printf(", unknown-type=%ld (a newer build wrote them)", d.unknown);
	printf("\n");
	return n < 0 ? 1 : 0;
}
