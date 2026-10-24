/* walscantest.c - the WAL's reader (walscan.c; the 0.7.2 split, wave 1):
 * segments written byte by byte here in the format wal.h describes, read
 * back through pc_wal_scan and pc_wal_dump.
 *
 *   A  two segments read in generation order, not file order; each type's
 *      header - the version where it carries one, the upsert's flags byte
 *      - and the zeroed space after the last record read as the end; a
 *      file without the magic ignored
 *   B  the dump: one line a record, the delete's version from its value,
 *      the JSON flag, the summary with the order
 *   C  where reading stops, and what it says: a record cut short, a
 *      checksum that does not match, a length past the ceiling ("torn"),
 *      a recycled segment's leftover of another generation
 *      ("stale-gen"), lengths that do not add up ("torn")
 *   D  a newer build's record type, its framing sound - handed on
 *      unparsed, the records after it still read; the dump says so
 *   E  the callback stops the scan; no segment at all is 0 records; the
 *      dump names a sequence that went backwards
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "core/pcache_htable.h"
#include "wal.h"
#include "walfmt.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* -W's dump: main.c declares it where it calls it, as this file does */
int pc_wal_dump(const char *dir);

/* ---- the harness: records and segments by hand ------------------------ */
static char dir[64];

/* one record of @type into @out: [len][crc][gen][payload]; its size */
static size_t rec(unsigned char *out, uint32_t gen, int type, const char *col,
		const char *key, const void *val, int vlen, uint64_t exp,
		uint64_t seq, uint64_t ver, unsigned char flags)
{
	unsigned char *p = out + PC_WAL_RECHDR;
	unsigned int hdr = PC_WAL_HDRLEN(type);
	int collen = (int)strlen(col), klen = (int)strlen(key);
	uint32_t len = hdr + (uint32_t)(collen + klen + vlen);

	memset(p, 0, hdr);
	p[0] = (unsigned char)type;
	p[1] = (unsigned char)collen;
	w16(p + 2, (uint16_t)klen);
	w32(p + 4, (uint32_t)vlen);
	w64(p + 8, exp);
	w64(p + 16, seq);
	if (hdr >= 32)
		w64(p + 24, ver);
	if (hdr == 33)
		p[32] = flags;
	memcpy(p + hdr, col, (size_t)collen);
	memcpy(p + hdr + collen, key, (size_t)klen);
	memcpy(p + hdr + collen + klen, val, (size_t)vlen);
	w32(out, len);
	w32(out + 8, gen);
	w32(out + 4, pc_crc32c(0, out + 8, 4 + len));
	return PC_WAL_RECHDR + len;
}

/* segment @idx of generation @gen holding @n bytes of records, then the
 * zeroed space a provisioned segment has */
static void seg(int idx, uint32_t gen, const unsigned char *recs, size_t n)
{
	unsigned char h[PC_WAL_HDR], z[64];
	char path[128];
	int fd;

	snprintf(path, sizeof path, "%s/wal-%03d.seg", dir, idx);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return;
	w32(h, PC_WAL_MAGIC);
	w32(h + 4, gen);
	memset(z, 0, sizeof z);
	if (write(fd, h, sizeof h) < 0 || write(fd, recs, n) < 0 || write(fd, z, sizeof z) < 0)
		printf("  (write failed)\n");
	close(fd);
}

static void clear(void)
{
	char path[128];
	int i;

	for (i = 0; i < 4; i++) {
		snprintf(path, sizeof path, "%s/wal-%03d.seg", dir, i);
		unlink(path);
	}
}

struct got {
	int n, stop_at;
	int type[8], vlen[8];
	char col[8][16], key[8][16], val[8][16];
	unsigned long long exp[8], seq[8], ver[8];
	unsigned char flags[8];
};

static int cb(int type, const char *col, int collen, const char *key, int klen,
		const char *val, int vlen, unsigned long long exp, unsigned long long seq,
		unsigned long long ver, unsigned char flags, void *ctx)
{
	struct got *g = ctx;
	int i = g->n++;

	if (i < 8) {
		g->type[i] = type;
		snprintf(g->col[i], sizeof g->col[i], "%.*s", collen, col);
		snprintf(g->key[i], sizeof g->key[i], "%.*s", klen, key);
		snprintf(g->val[i], sizeof g->val[i], "%.*s", vlen < 15 ? vlen : 15, val);
		g->vlen[i] = vlen;
		g->exp[i] = exp;
		g->seq[i] = seq;
		g->ver[i] = ver;
		g->flags[i] = flags;
	}
	return g->stop_at && g->n >= g->stop_at ? -1 : 0;
}

static long scan(struct got *g, const char **why)
{
	memset(g, 0, sizeof *g);
	return pc_wal_scan(dir, cb, g, why);
}

/* pc_wal_dump's output, captured */
static char dumped[4096];

static int dump(void)
{
	char path[128];
	int fd, out, rc;
	ssize_t n;

	snprintf(path, sizeof path, "%s/dump.txt", dir);
	fflush(stdout);
	out = dup(1);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	dup2(fd, 1);
	rc = pc_wal_dump(dir);
	fflush(stdout);
	dup2(out, 1);
	close(out);
	lseek(fd, 0, SEEK_SET);
	n = read(fd, dumped, sizeof dumped - 1);
	dumped[n > 0 ? n : 0] = 0;
	close(fd);
	unlink(path);
	return rc;
}

int main(void)
{
	unsigned char a[1024], b[1024], v8[8];
	const char *why = "?";
	struct got g;
	size_t na, nb, r1;
	long n;

	snprintf(dir, sizeof dir, "/tmp/walscantest.XXXXXX");
	if (!mkdtemp(dir))
		return 2;

	/* A */
	w64(v8, 42);
	na = rec(a, 5, PC_WAL_T_UPSERT, "c", "k1", "v1", 2, 1000, 1, 0, 0);
	na += rec(a + na, 5, PC_WAL_T_DEL, "c", "k1", v8, 8, 0, 2, 0, 0);
	nb = rec(b, 7, PC_WAL_T_UPSERT_V, "c", "k2", "{}", 2, 0, 3, 99, PCACHE_F_JSON);
	nb += rec(b + nb, 7, PC_WAL_T_TOUCH_V, "c", "k2", "", 0, 2000, 4, 100, 0);
	seg(0, 7, b, nb);                      /* the newer generation first on disk */
	seg(1, 5, a, na);
	{
		char path[128];
		int fd;

		snprintf(path, sizeof path, "%s/wal-002.seg", dir);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd >= 0) {
			if (write(fd, "NOTAWALSEGMENT..", 16) < 0)
				printf("  (write failed)\n");
			close(fd);
		}
	}
	n = scan(&g, &why);
	CHK(n == 4 && g.n == 4 && !strcmp(why, "end"),
		"A: 4 records, then the zeroed space - the end (the file without the magic ignored)");
	CHK(g.seq[0] == 1 && g.seq[1] == 2 && g.seq[2] == 3 && g.seq[3] == 4,
		"A: generation 5 (wal-001) before 7 (wal-000)");
	CHK(g.type[0] == PC_WAL_T_UPSERT && !strcmp(g.col[0], "c") && !strcmp(g.key[0], "k1") &&
		!strcmp(g.val[0], "v1") && g.exp[0] == 1000 && g.ver[0] == 0 && g.flags[0] == 0,
		"A: an upsert - its strings and expiry, no version in its header");
	CHK(g.type[1] == PC_WAL_T_DEL && g.vlen[1] == PC_WAL_DEL_VERLEN, "A: a delete - its version as its 8-byte value");
	CHK(g.type[2] == PC_WAL_T_UPSERT_V && g.ver[2] == 99 && g.flags[2] == PCACHE_F_JSON && !strcmp(g.val[2], "{}"),
		"A: an upsert with its version and its flags byte (JSON)");
	CHK(g.type[3] == PC_WAL_T_TOUCH_V && g.ver[3] == 100 && g.exp[3] == 2000 && g.vlen[3] == 0,
		"A: a touch with its version");

	/* B */
	CHK(dump() == 0 && strstr(dumped, "seq=1 upsert c/k1 vlen=2 exp=1000\n   val=v1\n") &&
		strstr(dumped, "seq=2 del c/k1 vlen=8 exp=0 ver=42\n") &&
		strstr(dumped, "seq=3 upsert c/k2 vlen=2 exp=0 ver=99 type=json\n   val={}\n") &&
		strstr(dumped, "seq=4 touch c/k2 vlen=0 exp=2000 ver=100\n") &&
		strstr(dumped, "wal-dump: 4 records, stop=end, seq-order=ascending\n"),
		"B: the dump - a line a record, the delete's version, the JSON flag, the summary");
	clear();

	/* C */
	r1 = rec(a, 1, PC_WAL_T_UPSERT, "c", "k1", "v1", 2, 0, 1, 0, 0);
	na = r1 + rec(a + r1, 1, PC_WAL_T_UPSERT, "c", "k2", "v2", 2, 0, 2, 0, 0);
	seg(0, 1, a, na - 3);
	n = scan(&g, &why);
	CHK(n == 1 && !strcmp(why, "torn"), "C: a record cut short - the one before it read, torn");
	memcpy(b, a, na);
	b[na - 1] ^= 1;
	seg(0, 1, b, na);
	n = scan(&g, &why);
	CHK(n == 1 && !strcmp(why, "torn"), "C: a byte of it flipped - the checksum: torn");
	memcpy(b, a, r1);
	w32(b + r1, (uint32_t)MAX_REC + 1);
	memset(b + r1 + 4, 0xAB, 8);
	seg(0, 1, b, r1 + 12);
	n = scan(&g, &why);
	CHK(n == 1 && !strcmp(why, "torn"), "C: a length past the ceiling (%ld) - torn", (long)MAX_REC);
	nb = r1 + rec(a + r1, 2, PC_WAL_T_UPSERT, "c", "old", "x", 1, 0, 9, 0, 0);
	seg(0, 1, a, nb);
	n = scan(&g, &why);
	CHK(n == 1 && !strcmp(why, "stale-gen"), "C: a leftover of generation 2 in generation 1's segment - stale-gen");
	{
		size_t m = rec(b, 1, PC_WAL_T_UPSERT, "c", "k1", "v1", 2, 0, 1, 0, 0);
		uint32_t len = r32(b) + 1;

		b[m] = 'Z';                    /* a byte the lengths do not count */
		w32(b, len);
		w32(b + 4, pc_crc32c(0, b + 8, 4 + len));
		seg(0, 1, b, m + 1);
		n = scan(&g, &why);
		CHK(n == 0 && !strcmp(why, "torn"), "C: a sound frame whose lengths do not add up - torn");
	}

	/* D */
	na = rec(a, 3, PC_WAL_T_UPSERT, "c", "k1", "v1", 2, 0, 1, 0, 0);
	na += rec(a + na, 3, 9, "zz", "future", "!", 1, 0, 2, 0, 0);
	na += rec(a + na, 3, PC_WAL_T_UPSERT, "c", "k3", "v3", 2, 0, 3, 0, 0);
	seg(0, 3, a, na);
	n = scan(&g, &why);
	CHK(n == 3 && !strcmp(why, "end") && g.type[1] == 9 && !g.col[1][0] && !g.key[1][0] && g.seq[1] == 0 &&
		g.seq[2] == 3, "D: type 9 - handed on unparsed, the record after it read");
	CHK(dump() == 0 && strstr(dumped, "type=9 unknown: a newer build's record, not parsed\n") &&
		strstr(dumped, "wal-dump: 3 records, stop=end, seq-order=ascending, unknown-type=1 (a newer build wrote them)\n"),
		"D: the dump says so, and the order holds around it");

	/* E */
	memset(&g, 0, sizeof g);
	g.stop_at = 2;
	n = pc_wal_scan(dir, cb, &g, &why);
	CHK(n == 2 && g.n == 2, "E: the callback stops it at the second record - 2");
	clear();
	n = scan(&g, &why);
	CHK(n == 0 && g.n == 0, "E: no segment - 0 records");
	na = rec(a, 4, PC_WAL_T_UPSERT, "c", "k1", "v1", 2, 0, 5, 0, 0);
	na += rec(a + na, 4, PC_WAL_T_UPSERT, "c", "k2", "v2", 2, 0, 3, 0, 0);
	seg(0, 4, a, na);
	CHK(dump() == 0 && strstr(dumped, "wal-dump: 2 records, stop=end, seq-order=VIOLATED\n"),
		"E: seq 5 then 3 - the dump says VIOLATED");
	clear();
	{
		char path[128];

		snprintf(path, sizeof path, "%s/wal-002.seg", dir);
		unlink(path);
		rmdir(dir);
	}

	printf("walscantest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
