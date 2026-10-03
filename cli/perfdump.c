/*
 * perfdump - S112: a parallel dumper over the client door.
 *
 * One node's collections are walked with the `dump` verb by N
 * connections, each owning a range of buckets (`end`), the last one the
 * tail; the records go to a directory of chunk files with a manifest.
 * Shaped like mydumper: one file per collection per chunk, a manifest
 * naming every file with its record and byte counts and a checksum, a
 * chunk complete only once its trailer and its manifest entry are
 * written, so a restart re-dumps what is not complete.
 *
 * File format (perfdump-format v1, doc/perfdump-format.md):
 *   "PCD1" magic, then records: u32 klen, u32 vlen, u64 expiry (absolute
 *   unix ms, 0 = never), u64 version, u8 flags, key bytes, value bytes;
 *   trailer: u32 0xFFFFFFFF, u64 record count.  Little-endian throughout.
 *   Compressed chunks are the same bytes through zstd (the binary).
 *
 * Built on libperfd: learns nothing it does not need, holds one plain
 * handle per connection (no spares).  The walk rides the tree-level
 * entry (perfd_command_tree): a dump chunk's keys and values are bulks
 * read byte-exact off the reply tree - never through the JSON edge,
 * whose rendering is lossy for bytes that are not UTF-8 (S317).  Only
 * the one `stats` call at startup reads rendered JSON, for names and
 * integers.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../lib/perfd.h"
#include "../src/json.h"
#include "../src/ptree.h"

#define VERSION "0.1"
#define MAXTOK   (1 << 17)
#define RECMAX   (1 << 20)          /* PCACHE_CELL_MAX-sized key/value scratch */

struct opts {
	const char *host, *out, *cols, *secret;
	int port, threads, count, zstd, chunk_mb, rate, inspect, verbose;
};

/* ---- crc32 (IEEE, table-driven; no zlib dependency) -------------------- */
static uint32_t crc_tab[256];
static void crc_init(void)
{
	uint32_t c;
	int i, k;

	for (i = 0; i < 256; i++) {
		c = (uint32_t)i;
		for (k = 0; k < 8; k++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_tab[i] = c;
	}
}
static uint32_t crc32_upd(uint32_t crc, const void *p, size_t n)
{
	const unsigned char *b = p;
	size_t i;

	crc = ~crc;
	for (i = 0; i < n; i++)
		crc = crc_tab[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

/* ---- shared state ------------------------------------------------------ */
struct chunkrec {
	char col[64];
	char file[128];
	unsigned int lo, hi;           /* bucket range the chunk's walker owns */
	unsigned long long records, bytes;
	uint32_t crc;
	int complete;
	int zstd;
};

struct shared {
	struct opts *o;
	pthread_mutex_t mx;
	struct chunkrec *chunks;
	int nchunks, capchunks;
	unsigned int chunkseq;
	unsigned long long records, bytes, errors;
	int resized;                   /* collections dumped again after a resize */
	/* per collection */
	char cols[64][64];
	unsigned int nb[64];
	/* the table each collection's walkers are pinned to: the daemon
	 * refuses a chunk the moment the collection has been republished
	 * (a resize swap), and the collection is dumped again */
	unsigned long long tbl[64];
	int ncols;
	char fleet[128];
	time_t started;
};

struct job {
	struct shared *sh;
	int idx;                       /* connection index 0..threads-1 */
	int col;                       /* collection index */
	unsigned int lo, hi;           /* buckets [lo, hi); hi == nb means the tail is mine */
	unsigned long long records, bytes;
	int failed;
	int resized;                   /* the table moved under this walker */
};

/* a JSON primitive `true` in the manifest: the tokenizer's string
 * compare is for string tokens only, so a bare literal needs its own test */
static int jtrue(const char *b, const struct pc_jtok *t)
{
	return t->type == PC_J_PRIM && t->end > t->start && b[t->start] == 't';
}

/* the `dump` request: {col, cursor, count[, end], table?} as a tree.
 * @end < 0 = the verb's own end; @table 0 = not pinned (the probe). */
static size_t dump_params(unsigned char *buf, size_t cap, const char *col,
		unsigned int cursor, int count, long long end,
		unsigned long long table)
{
	struct pc_tw w;

	pc_tw_init(&w, buf, cap);
	pc_tw_map(&w);
	pc_tw_key(&w, "col");
	pc_tw_str(&w, col);
	pc_tw_key(&w, "cursor");
	pc_tw_i64(&w, cursor);
	pc_tw_key(&w, "count");
	pc_tw_i64(&w, count);
	if (end >= 0) {
		pc_tw_key(&w, "end");
		pc_tw_i64(&w, end);
	}
	if (table) {
		pc_tw_key(&w, "table");
		pc_tw_i64(&w, (long long)table);
	}
	pc_tw_end(&w);
	return pc_tw_done(&w) == 0 ? w.n : 0;
}

static unsigned long long now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)tv.tv_usec / 1000ULL;
}

static void put32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(unsigned char *p, uint64_t v) { int i; for (i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const unsigned char *p) { uint64_t v = 0; int i; for (i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

/* ---- the manifest: rewritten whole and renamed into place ------------- */
static int manifest_write(struct shared *sh)
{
	struct pc_jw w;
	char tmp[512], path[512];
	FILE *f;
	int i;

	memset(&w, 0, sizeof w);           /* init_heap wants an empty writer */
	if (pc_jw_init_heap(&w, 1 << 16) != 0)
		return -1;
	pc_jw_lit(&w, "{\"format\":\"perfdump-format\",\"format_version\":1,\"tool\":\"perfdump " VERSION "\"");
	pc_jw_lit(&w, ",\"source\":");
	pc_jw_str(&w, sh->fleet, strlen(sh->fleet));
	pc_jw_lit(&w, ",\"started\":");
	pc_jw_i64(&w, (long long)sh->started);
	pc_jw_lit(&w, ",\"threads\":");
	pc_jw_i64(&w, sh->o->threads);
	pc_jw_lit(&w, ",\"collections\":[");
	for (i = 0; i < sh->ncols; i++) {
		if (i)
			pc_jw_lit(&w, ",");
		pc_jw_lit(&w, "{\"name\":");
		pc_jw_str(&w, sh->cols[i], strlen(sh->cols[i]));
		pc_jw_lit(&w, ",\"buckets\":");
		pc_jw_i64(&w, sh->nb[i]);
		pc_jw_lit(&w, "}");
	}
	pc_jw_lit(&w, "],\"files\":[");
	for (i = 0; i < sh->nchunks; i++) {
		struct chunkrec *c = &sh->chunks[i];

		if (i)
			pc_jw_lit(&w, ",");
		pc_jw_lit(&w, "{\"file\":");
		pc_jw_str(&w, c->file, strlen(c->file));
		pc_jw_lit(&w, ",\"collection\":");
		pc_jw_str(&w, c->col, strlen(c->col));
		pc_jw_lit(&w, ",\"bucket_lo\":");
		pc_jw_i64(&w, c->lo);
		pc_jw_lit(&w, ",\"bucket_hi\":");
		pc_jw_i64(&w, c->hi);
		pc_jw_lit(&w, ",\"records\":");
		pc_jw_i64(&w, (long long)c->records);
		pc_jw_lit(&w, ",\"bytes\":");
		pc_jw_i64(&w, (long long)c->bytes);
		pc_jw_lit(&w, ",\"crc32\":");
		pc_jw_i64(&w, (long long)c->crc);
		pc_jw_lit(&w, ",\"zstd\":");
		pc_jw_i64(&w, c->zstd);
		pc_jw_lit(&w, ",\"complete\":");
		pc_jw_lit(&w, c->complete ? "true" : "false");
		pc_jw_lit(&w, "}");
	}
	pc_jw_lit(&w, "],\"records\":");
	pc_jw_i64(&w, (long long)sh->records);
	pc_jw_lit(&w, ",\"bytes\":");
	pc_jw_i64(&w, (long long)sh->bytes);
	pc_jw_lit(&w, "}\n");
	if (w.overflow) {
		pc_jw_free(&w);
		return -1;
	}
	snprintf(tmp, sizeof tmp, "%s/manifest.json.tmp", sh->o->out);
	snprintf(path, sizeof path, "%s/manifest.json", sh->o->out);
	f = fopen(tmp, "w");
	if (!f || fwrite(w.buf, 1, w.len, f) != w.len || fclose(f) != 0) {
		pc_jw_free(&w);
		return -1;
	}
	pc_jw_free(&w);
	return rename(tmp, path);
}

/* ---- one chunk file ----------------------------------------------------- */
struct chunk {
	FILE *f;
	char path[512];
	int slot;                      /* index into sh->chunks */
	unsigned long long records, bytes;
	uint32_t crc;
};

static int chunk_open(struct shared *sh, struct job *j, struct chunk *c)
{
	unsigned int seq;
	struct chunkrec *r;

	pthread_mutex_lock(&sh->mx);
	seq = sh->chunkseq++;
	if (sh->nchunks == sh->capchunks) {
		int nc = sh->capchunks ? sh->capchunks * 2 : 64;
		struct chunkrec *n = realloc(sh->chunks, (size_t)nc * sizeof *n);

		if (!n) {
			pthread_mutex_unlock(&sh->mx);
			return -1;
		}
		sh->chunks = n;
		sh->capchunks = nc;
	}
	r = &sh->chunks[sh->nchunks];
	memset(r, 0, sizeof *r);
	snprintf(r->col, sizeof r->col, "%s", sh->cols[j->col]);
	snprintf(r->file, sizeof r->file, "%s.%04u.pcd", sh->cols[j->col], seq);
	r->lo = j->lo;
	r->hi = j->hi;
	r->zstd = sh->o->zstd;
	c->slot = sh->nchunks++;
	pthread_mutex_unlock(&sh->mx);

	snprintf(c->path, sizeof c->path, "%s/%s", sh->o->out, r->file);
	c->f = fopen(c->path, "wb");
	if (!c->f)
		return -1;
	c->records = c->bytes = 0;
	c->crc = 0;
	if (fwrite("PCD1", 1, 4, c->f) != 4)
		return -1;
	c->bytes = 4;
	c->crc = crc32_upd(c->crc, "PCD1", 4);
	return 0;
}

/* the record's flags byte: bit 0 = a JSON document (S280) */
#define PCD_F_JSON 0x01

static int chunk_put(struct chunk *c, const char *k, size_t kl, const char *v, size_t vl,
		unsigned long long exp_ms, unsigned long long ver, unsigned char flags)
{
	unsigned char h[25];

	put32(h, (uint32_t)kl);
	put32(h + 4, (uint32_t)vl);
	put64(h + 8, exp_ms);
	put64(h + 16, ver);
	h[24] = flags;
	if (fwrite(h, 1, 25, c->f) != 25 || fwrite(k, 1, kl, c->f) != kl ||
	    (vl && fwrite(v, 1, vl, c->f) != vl))
		return -1;
	c->crc = crc32_upd(c->crc, h, 25);
	c->crc = crc32_upd(c->crc, k, kl);
	c->crc = crc32_upd(c->crc, v, vl);
	c->records++;
	c->bytes += 25 + kl + vl;
	return 0;
}

/* close: trailer, checksum, optional zstd (synchronous, the binary), then
 * the manifest entry - the chunk is complete only once the manifest says so */
static int chunk_close(struct shared *sh, struct chunk *c)
{
	unsigned char t[12];
	struct chunkrec *r;

	if (!c->f)
		return 0;
	put32(t, 0xFFFFFFFFu);
	put64(t + 4, c->records);
	if (fwrite(t, 1, 12, c->f) != 12 || fclose(c->f) != 0) {
		c->f = NULL;
		return -1;
	}
	c->f = NULL;
	c->crc = crc32_upd(c->crc, t, 12);
	c->bytes += 12;
	if (sh->o->zstd > 0) {
		char cmd[1200];
		int rc;

		snprintf(cmd, sizeof cmd, "zstd -q -f --rm -T1 -%d '%s' -o '%s.zst'",
			sh->o->zstd, c->path, c->path);
		rc = system(cmd);
		if (rc != 0) {
			fprintf(stderr, "perfdump: zstd failed on %s (rc %d)\n", c->path, rc);
			return -1;
		}
	}
	pthread_mutex_lock(&sh->mx);
	r = &sh->chunks[c->slot];
	if (sh->o->zstd > 0) {
		size_t n = strlen(r->file);

		if (n + 5 < sizeof r->file)
			memcpy(r->file + n, ".zst", 5);
	}
	r->records = c->records;
	r->bytes = c->bytes;
	r->crc = c->crc;
	r->complete = 1;
	sh->records += c->records;
	sh->bytes += c->bytes;
	{
		int mrc = manifest_write(sh);

		pthread_mutex_unlock(&sh->mx);
		return mrc;
	}
}

/* ---- the walker --------------------------------------------------------- */
static void *walker(void *arg)
{
	struct job *j = arg;
	struct shared *sh = j->sh;
	struct opts *o = sh->o;
	const char *secrets[2] = { o->secret, NULL };
	perfd_opts po;
	perfd_t *p;
	struct pc_tv v;
	struct chunk ch;
	unsigned int cursor = j->lo;
	int count = o->count;
	unsigned long long chunk_limit = (unsigned long long)o->chunk_mb << 20;
	unsigned long long t0 = now_ms(), paced = 0;

	memset(&po, 0, sizeof po);
	po.io_timeout_ms = 60000;
	po.spares = PERFD_SPARES_NONE;
	if (o->secret)
		po.secrets = secrets;
	memset(&ch, 0, sizeof ch);
	memset(&v, 0, sizeof v);
	p = perfd_connect(o->host, o->port, &po);
	if (!p) {
		fprintf(stderr, "perfdump: connection %d: %s\n", j->idx, perfd_error(NULL));
		j->failed = 1;
		return NULL;
	}
	if (chunk_open(sh, j, &ch) != 0) {
		fprintf(stderr, "perfdump: cannot open a chunk file in %s: %s\n", o->out, strerror(errno));
		j->failed = 1;
		perfd_free(p);
		return NULL;
	}
	for (;;) {
		unsigned char params[512];
		unsigned char *res;
		size_t pn, rn;
		int tr, tc, tm;
		unsigned int i;
		long long ccur;
		int more;
		/* the walker's range: buckets [lo, hi); hi == nb means the tail is
		 * mine and the verb's own end (nb - 1 then the tail) applies */
		pn = dump_params(params, sizeof params, sh->cols[j->col], cursor,
			count, j->hi < sh->nb[j->col] ? (long long)j->hi : -1,
			sh->tbl[j->col]);
		res = pn ? perfd_command_tree(p, "dump", params, pn, &rn) : NULL;
		if (!res) {
			const char *e = pn ? perfd_error(p) : "request too large";

			if (e && strstr(e, "halve") && count > 1) {
				count /= 2;    /* a chunk that did not fit: smaller buckets */
				continue;
			}
			/* the collection was resized since it was planned: every
			 * range and cursor of it now names another table's
			 * buckets.  Not a failure - main dumps it again. */
			if (e && strstr(e, "table resized")) {
				j->resized = 1;
				break;
			}
			fprintf(stderr, "perfdump: connection %d: dump failed at cursor %u: %s\n",
				j->idx, cursor, e ? e : "?");
			j->failed = 1;
			break;
		}
		if (pc_tv_parse(&v, res, rn) != 0 || v.n[0].type != 'm') {
			fprintf(stderr, "perfdump: connection %d: unparsable reply (%s)\n",
				j->idx, v.err ? v.err : "not a map");
			free(res);
			j->failed = 1;
			break;
		}
		tr = pc_tv_get(&v, 0, "records");
		tc = pc_tv_get(&v, 0, "cursor");
		tm = pc_tv_get(&v, 0, "more");
		if (tr < 0 || v.n[tr].type != 'a' || tc < 0 || tm < 0 ||
		    pc_tv_get_int(&v, 0, "cursor", &ccur) != 0) {
			fprintf(stderr, "perfdump: connection %d: reply without records/cursor/more\n", j->idx);
			free(res);
			j->failed = 1;
			break;
		}
		more = pc_tv_get_bool(&v, 0, "more");
		/* every record of the array: {k, v, ttl, ver, t?} - the key and
		 * the value are bulks, written to the chunk byte for byte */
		for (i = 0; i < v.n[tr].len; i++) {
			int rec = pc_tv_at(&v, tr, i), tk, tvv, tt;
			long long ttl, ver;
			unsigned long long exp_ms;

			if (rec < 0 || v.n[rec].type != 'm')
				continue;
			tk = pc_tv_get(&v, rec, "k");
			tvv = pc_tv_get(&v, rec, "v");
			if (tk < 0 || v.n[tk].type != 'b' || tvv < 0 ||
			    v.n[tvv].type != 'b' ||
			    pc_tv_get_int(&v, rec, "ttl", &ttl) != 0 ||
			    pc_tv_get_int(&v, rec, "ver", &ver) != 0)
				continue;
			if (v.n[tk].len > RECMAX || v.n[tvv].len > RECMAX)
				continue;
			tt = pc_tv_get(&v, rec, "t");
			exp_ms = ttl < 0 ? 0 : now_ms() + (unsigned long long)ttl * 1000ULL;
			if (chunk_put(&ch, (const char *)v.n[tk].p, v.n[tk].len,
			        (const char *)v.n[tvv].p, v.n[tvv].len, exp_ms,
			        (unsigned long long)ver,
			        pc_tv_streq(&v, tt, "json") ? PCD_F_JSON : 0) != 0) {
				fprintf(stderr, "perfdump: write failed on %s: %s\n", ch.path, strerror(errno));
				j->failed = 1;
				break;
			}
			j->records++;
			j->bytes += (unsigned long long)(v.n[tk].len + v.n[tvv].len);
			paced++;
		}
		free(res);
		if (j->failed)
			break;
		if (ch.bytes >= chunk_limit) {
			if (chunk_close(sh, &ch) != 0 || chunk_open(sh, j, &ch) != 0) {
				fprintf(stderr, "perfdump: chunk rotation failed: %s\n", strerror(errno));
				j->failed = 1;
				break;
			}
		}
		if (o->rate > 0) {
			/* pace: sleep until this connection is back under rate */
			unsigned long long due = t0 + paced * 1000ULL / (unsigned long long)o->rate, now = now_ms();

			if (due > now)
				usleep((useconds_t)((due - now) * 1000ULL));
		}
		if (!more)
			break;
		cursor = (unsigned int)ccur;
	}
	if (!j->failed && chunk_close(sh, &ch) != 0) {
		fprintf(stderr, "perfdump: closing the last chunk failed: %s\n", strerror(errno));
		j->failed = 1;
	}
	if (j->failed && ch.f) {
		fclose(ch.f);
		ch.f = NULL;
	}
	perfd_free(p);
	pc_tv_free(&v);
	return NULL;
}

/* ---- inspect: the manifest, and every file's checksum ------------------- */
static int inspect(const char *dir)
{
	char path[512], *buf;
	FILE *f;
	long n;
	struct pc_jtok *toks;
	int ntok, tf, i, bad = 0, files = 0;
	unsigned long long recs = 0;

	snprintf(path, sizeof path, "%s/manifest.json", dir);
	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "perfdump: no manifest in %s\n", dir);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n + 1);
	toks = malloc((size_t)MAXTOK * sizeof *toks);
	if (!buf || !toks || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		return 1;
	}
	fclose(f);
	buf[n] = 0;
	ntok = pc_json_parse(buf, (size_t)n, toks, MAXTOK);
	if (ntok < 0) {
		fprintf(stderr, "perfdump: manifest unparsable\n");
		return 1;
	}
	i = pc_json_get(buf, toks, ntok, 0, "source");
	printf("source: %.*s\n", i >= 0 ? toks[i].end - toks[i].start : 1, i >= 0 ? buf + toks[i].start : "?");
	tf = pc_json_get(buf, toks, ntok, 0, "files");
	for (i = tf + 1; tf >= 0 && i < ntok; i++) {
		int tn, tc, tr, tz, tcomp;
		char fname[256], cmd[1200];
		unsigned long long want, got = 0, count = 0;
		uint32_t crc = 0, wantcrc;
		FILE *pf;
		unsigned char h[25], *rec;
		size_t got_n;

		if (toks[i].parent != tf)
			continue;
		tn = pc_json_get(buf, toks, ntok, i, "file");
		tc = pc_json_get(buf, toks, ntok, i, "crc32");
		tr = pc_json_get(buf, toks, ntok, i, "records");
		tz = pc_json_get(buf, toks, ntok, i, "zstd");
		tcomp = pc_json_get(buf, toks, ntok, i, "complete");
		if (tn < 0 || tc < 0 || tr < 0)
			continue;
		snprintf(fname, sizeof fname, "%.*s", toks[tn].end - toks[tn].start, buf + toks[tn].start);
		wantcrc = (uint32_t)strtoull(buf + toks[tc].start, NULL, 10);
		want = strtoull(buf + toks[tr].start, NULL, 10);
		files++;
		if (tcomp >= 0 && !jtrue(buf, &toks[tcomp])) {
			printf("  %-32s INCOMPLETE\n", fname);
			bad++;
			continue;
		}
		if (tz >= 0 && strtol(buf + toks[tz].start, NULL, 10) > 0)
			snprintf(cmd, sizeof cmd, "zstd -dc -q '%s/%s'", dir, fname);
		else
			snprintf(cmd, sizeof cmd, "cat '%s/%s'", dir, fname);
		pf = popen(cmd, "r");
		if (!pf) {
			printf("  %-32s cannot read\n", fname);
			bad++;
			continue;
		}
		rec = malloc(RECMAX * 2 + 32);
		if (fread(h, 1, 4, pf) == 4 && memcmp(h, "PCD1", 4) == 0) {
			crc = crc32_upd(crc, h, 4);
			for (;;) {
				uint32_t kl, vl;

				if (fread(h, 1, 4, pf) != 4)
					break;
				kl = get32(h);
				if (kl == 0xFFFFFFFFu) {
					unsigned char tt[8];

					if (fread(tt, 1, 8, pf) != 8)
						break;
					crc = crc32_upd(crc, h, 4);
					crc = crc32_upd(crc, tt, 8);
					got = get64(tt);
					break;
				}
				if (fread(h + 4, 1, 21, pf) != 21)
					break;
				vl = get32(h + 4);
				if (kl + vl > RECMAX * 2)
					break;
				got_n = fread(rec, 1, kl + vl, pf);
				if (got_n != kl + vl)
					break;
				crc = crc32_upd(crc, h, 25);
				crc = crc32_upd(crc, rec, kl + vl);
				count++;
			}
		}
		pclose(pf);
		free(rec);
		if (count == want && got == want && crc == wantcrc) {
			printf("  %-32s ok: %llu records, crc %08x\n", fname, count, crc);
			recs += count;
		} else {
			printf("  %-32s BAD: read %llu records, trailer says %llu, manifest says %llu; crc %08x vs manifest %08x\n",
				fname, count, got, want, crc, wantcrc);
			bad++;
		}
	}
	printf("%d file(s), %llu records verified, %d bad\n", files, recs, bad);
	free(buf);
	free(toks);
	return bad ? 1 : 0;
}

static void usage(FILE *f)
{
	fputs("usage: perfdump --from HOST:PORT --out DIR [-a SECRET] [--collections a,b] [--threads N]\n"
	      "                [--count BUCKETS] [--chunk-mb MB] [--zstd LEVEL] [--rate RECORDS/S] [-v]\n"
	      "       perfdump --inspect DIR\n"
	      "  --from        one node's client door (eager: any READY node holds everything)\n"
	      "  --out         directory for the chunk files and manifest.json (created)\n"
	      "  -a            client secret (or PERFCLI_AUTH)\n"
	      "  --collections comma list; default: every collection the node reports\n"
	      "  --threads     connections per collection, each owning a bucket range (default 4)\n"
	      "  --count       buckets per dump call (default 1024; halved on the daemon's refusal)\n"
	      "  --chunk-mb    chunk file size before compression (default 64)\n"
	      "  --zstd        0 = raw .pcd files; N = zstd -N through the zstd binary (default 3)\n"
	      "  --rate        records per second per connection, 0 = unpaced\n"
	      "  --inspect     print a dump's manifest summary and verify every file's checksum\n", f);
}

/* ---- a collection resized under the dump (see the walker) ------------ */
static perfd_t *dconnect(struct opts *o)
{
	static const char *secrets[2];
	perfd_opts po;

	memset(&po, 0, sizeof po);
	po.io_timeout_ms = 30000;
	po.spares = PERFD_SPARES_NONE;
	secrets[0] = o->secret;
	secrets[1] = NULL;
	if (o->secret)
		po.secrets = secrets;
	return perfd_connect(o->host, o->port, &po);
}

/* the table a collection's walkers will be pinned to, and ITS bucket
 * count: one bucket's dump, whose records are not kept.  The bucket
 * count in `stats` is not the same read - a swap between the two would
 * plan ranges for one table and pin them to another. */
static int probe_table(struct shared *sh, int c)
{
	struct pc_tv v;
	unsigned char params[160], *res;
	size_t pn, rn;
	perfd_t *p = dconnect(sh->o);
	long long tbl, nb;
	int rc = -1;

	if (!p) {
		fprintf(stderr, "perfdump: %s\n", perfd_error(NULL));
		return -1;
	}
	memset(&v, 0, sizeof v);
	pn = dump_params(params, sizeof params, sh->cols[c], 0, 1, -1, 0);
	res = pn ? perfd_command_tree(p, "dump", params, pn, &rn) : NULL;
	if (res && pc_tv_parse(&v, res, rn) == 0 && v.n[0].type == 'm') {
		if (pc_tv_get_int(&v, 0, "table", &tbl) == 0 &&
		    pc_tv_get_int(&v, 0, "buckets", &nb) == 0) {
			sh->tbl[c] = (unsigned long long)tbl;
			sh->nb[c] = (unsigned int)nb;
			rc = 0;
		} else
			fprintf(stderr, "perfdump: the node's dump names no table - it predates "
				"the resize guard (0.4.0-rc33); a resize during this dump would "
				"repeat or lose records\n");
	} else
		fprintf(stderr, "perfdump: dump probe of '%s' failed: %s\n", sh->cols[c],
			res ? "unparsable reply" : perfd_error(p));
	free(res);
	pc_tv_free(&v);
	perfd_free(p);
	return rc;
}

/* every file of collection @c goes, with its manifest entries and its
 * share of the totals - it is dumped again from scratch */
static int discard_collection(struct shared *sh, int c)
{
	char path[768];
	int i, k = 0, n = 0, rc;

	pthread_mutex_lock(&sh->mx);
	for (i = 0; i < sh->nchunks; i++) {
		struct chunkrec *r = &sh->chunks[i];

		if (strcmp(r->col, sh->cols[c]) != 0) {
			sh->chunks[k++] = *r;
			continue;
		}
		snprintf(path, sizeof path, "%s/%s", sh->o->out, r->file);
		unlink(path);
		if (!r->complete && sh->o->zstd > 0) {
			/* an incomplete chunk was never compressed */
			snprintf(path, sizeof path, "%s/%s.zst", sh->o->out, r->file);
			unlink(path);
		}
		if (r->complete) {
			sh->records -= r->records;
			sh->bytes -= r->bytes;
		}
		n++;
	}
	sh->nchunks = k;
	rc = manifest_write(sh);
	pthread_mutex_unlock(&sh->mx);
	return rc < 0 ? -1 : n;
}

/* one collection's walkers: bucket ranges over its PINNED table, the
 * last range owning the tail.  Returns how many were started. */
static int start_collection(struct shared *sh, int c, struct job *jobs, pthread_t *th)
{
	unsigned int nb = sh->nb[c];
	int i, n = sh->o->threads;

	if ((unsigned int)n > nb)
		n = (int)nb;
	for (i = 0; i < n; i++) {
		struct job *j = &jobs[c * sh->o->threads + i];

		memset(j, 0, sizeof *j);
		j->sh = sh;
		j->idx = c * sh->o->threads + i;
		j->col = c;
		j->lo = (unsigned int)((unsigned long long)nb * (unsigned long long)i / (unsigned long long)n);
		j->hi = i == n - 1 ? nb : (unsigned int)((unsigned long long)nb * (unsigned long long)(i + 1) / (unsigned long long)n);
		pthread_create(&th[j->idx], NULL, walker, j);
	}
	return n;
}

#define PERFDUMP_RESIZE_ATTEMPTS 5

int main(int argc, char **argv)
{
	struct opts o;
	struct shared sh;
	perfd_opts po;
	const char *secrets[2];
	perfd_t *p;
	char *res, hostbuf[256];
	struct pc_jtok *toks;
	int ntok, i, c, failed = 0, attempt, *nstarted, *redo;
	pthread_t *th;
	struct job *jobs;
	unsigned long long t0, t1, tot_bytes;

	memset(&o, 0, sizeof o);
	o.threads = 4;
	o.count = 1024;
	o.chunk_mb = 64;
	o.zstd = 3;
	o.secret = getenv("PERFCLI_AUTH");
	for (i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "--from") && v) { o.host = v; i++; }
		else if (!strcmp(a, "--out") && v) { o.out = v; i++; }
		else if (!strcmp(a, "-a") && v) { o.secret = v; i++; }
		else if (!strcmp(a, "--collections") && v) { o.cols = v; i++; }
		else if (!strcmp(a, "--threads") && v) { o.threads = atoi(v); i++; }
		else if (!strcmp(a, "--count") && v) { o.count = atoi(v); i++; }
		else if (!strcmp(a, "--chunk-mb") && v) { o.chunk_mb = atoi(v); i++; }
		else if (!strcmp(a, "--zstd") && v) { o.zstd = atoi(v); i++; }
		else if (!strcmp(a, "--rate") && v) { o.rate = atoi(v); i++; }
		else if (!strcmp(a, "--inspect") && v) { o.inspect = 1; o.out = v; i++; }
		else if (!strcmp(a, "-v")) o.verbose = 1;
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
		else if (!strcmp(a, "-V")) { printf("perfdump " VERSION " (libperfd %s)\n", PERFD_VERSION); return 0; }
		else { usage(stderr); return 2; }
	}
	crc_init();
	if (o.inspect)
		return inspect(o.out);
	if (!o.host || !o.out || o.threads < 1 || o.threads > 64 || o.count < 1 || o.chunk_mb < 1) {
		usage(stderr);
		return 2;
	}
	{
		const char *colon = strrchr(o.host, ':');

		if (!colon) {
			fprintf(stderr, "perfdump: --from needs HOST:PORT\n");
			return 2;
		}
		snprintf(hostbuf, sizeof hostbuf, "%.*s", (int)(colon - o.host), o.host);
		o.host = hostbuf;
		o.port = atoi(colon + 1);
	}
	if (mkdir(o.out, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "perfdump: cannot create %s: %s\n", o.out, strerror(errno));
		return 1;
	}

	/* the collections and their bucket counts, from the node itself */
	memset(&sh, 0, sizeof sh);
	sh.o = &o;
	pthread_mutex_init(&sh.mx, NULL);
	sh.started = time(NULL);
	snprintf(sh.fleet, sizeof sh.fleet, "%s:%d", o.host, o.port);
	memset(&po, 0, sizeof po);
	po.io_timeout_ms = 30000;
	po.spares = PERFD_SPARES_NONE;
	secrets[0] = o.secret;
	secrets[1] = NULL;
	if (o.secret)
		po.secrets = secrets;
	p = perfd_connect(o.host, o.port, &po);
	if (!p) {
		fprintf(stderr, "perfdump: %s (%s:%d)\n", perfd_error(NULL), o.host, o.port);
		return 1;
	}
	res = perfd_command(p, "stats", NULL);
	if (!res) {
		fprintf(stderr, "perfdump: stats: %s\n", perfd_error(p));
		perfd_free(p);
		return 1;
	}
	toks = malloc((size_t)MAXTOK * sizeof *toks);
	ntok = pc_json_parse(res, strlen(res), toks, MAXTOK);
	if (ntok < 0) {
		fprintf(stderr, "perfdump: stats unparsable\n");
		return 1;
	}
	{
		int tcols = pc_json_get(res, toks, ntok, 0, "collections");
		int tst = pc_json_get(res, toks, ntok, 0, "cluster");

		if (tst >= 0) {
			int ts = pc_json_get(res, toks, ntok, tst, "state");

			if (ts >= 0 && !pc_json_streq(res, &toks[ts], "ready") &&
			    !pc_json_streq(res, &toks[ts], "null")) {
				fprintf(stderr, "perfdump: the node is not READY (%.*s) - it may still be pulling; pick another\n",
					toks[ts].end - toks[ts].start, res + toks[ts].start);
				return 1;
			}
		}
		for (i = tcols + 1; tcols >= 0 && i < ntok && sh.ncols < 64; i++) {
			int tn, tb;
			char name[64];

			if (toks[i].parent != tcols)
				continue;
			tn = pc_json_get(res, toks, ntok, i, "name");
			tb = pc_json_get(res, toks, ntok, i, "buckets");
			if (tn < 0 || tb < 0)
				continue;
			snprintf(name, sizeof name, "%.*s", toks[tn].end - toks[tn].start, res + toks[tn].start);
			if (o.cols) {
				/* comma list membership */
				const char *s = o.cols;
				int hit = 0;

				while (*s) {
					const char *e = strchr(s, ',');
					size_t l = e ? (size_t)(e - s) : strlen(s);

					if (l == strlen(name) && !strncmp(s, name, l))
						hit = 1;
					s = e ? e + 1 : s + l;
				}
				if (!hit)
					continue;
			}
			snprintf(sh.cols[sh.ncols], sizeof sh.cols[0], "%s", name);
			sh.nb[sh.ncols] = (unsigned int)strtoul(res + toks[tb].start, NULL, 10);
			sh.ncols++;
		}
	}
	free(res);
	free(toks);
	perfd_free(p);
	if (!sh.ncols) {
		fprintf(stderr, "perfdump: no collection to dump\n");
		return 1;
	}

	/* pin every collection to one table, and plan its ranges over THAT
	 * table's buckets */
	for (c = 0; c < sh.ncols; c++)
		if (probe_table(&sh, c) != 0)
			return 1;

	/* one job per collection per thread: bucket ranges, the last owns the tail */
	jobs = calloc((size_t)sh.ncols * (size_t)o.threads, sizeof *jobs);
	th = calloc((size_t)sh.ncols * (size_t)o.threads, sizeof *th);
	nstarted = calloc((size_t)sh.ncols, sizeof *nstarted);
	redo = calloc((size_t)sh.ncols, sizeof *redo);
	t0 = now_ms();
	for (c = 0; c < sh.ncols; c++)
		nstarted[c] = start_collection(&sh, c, jobs, th);
	tot_bytes = 0;
	for (attempt = 1; ; attempt++) {
		int again = 0;

		for (c = 0; c < sh.ncols; c++) {
			unsigned long long cb = 0;

			redo[c] = 0;
			for (i = 0; i < nstarted[c]; i++) {
				struct job *j = &jobs[c * o.threads + i];

				pthread_join(th[j->idx], NULL);
				failed |= j->failed;
				redo[c] |= j->resized;
				cb += j->bytes;
			}
			nstarted[c] = 0;
			if (!redo[c])
				tot_bytes += cb;
		}
		for (c = 0; c < sh.ncols; c++) {
			int nf;

			if (!redo[c] || failed)
				continue;
			nf = discard_collection(&sh, c);
			if (nf < 0) {
				fprintf(stderr, "perfdump: could not rewrite the manifest\n");
				failed = 1;
				continue;
			}
			if (attempt >= PERFDUMP_RESIZE_ATTEMPTS) {
				fprintf(stderr, "perfdump: collection '%s' was resized during each of %d "
					"attempts - dump it when it is not growing or shrinking\n",
					sh.cols[c], attempt);
				failed = 1;
				continue;
			}
			if (probe_table(&sh, c) != 0) {
				failed = 1;
				continue;
			}
			fprintf(stderr, "perfdump: collection '%s' was resized during the dump - "
				"%d file(s) discarded, dumping it again (%u buckets, attempt %d)\n",
				sh.cols[c], nf, sh.nb[c], attempt + 1);
			sh.resized++;
			nstarted[c] = start_collection(&sh, c, jobs, th);
			again = 1;
		}
		if (!again)
			break;
	}
	t1 = now_ms();
	printf("perfdump: %llu records, %.1f MB of keys and values, %d file(s), %d collection(s), %.2f s, %.0f records/s%s\n",
		sh.records, (double)tot_bytes / 1048576.0, sh.nchunks, sh.ncols,
		(double)(t1 - t0) / 1000.0,
		t1 > t0 ? (double)sh.records * 1000.0 / (double)(t1 - t0) : 0.0,
		failed ? " - INCOMPLETE, a connection failed" : "");
	if (sh.resized)
		printf("perfdump: %d collection dump(s) restarted after a resize\n", sh.resized);
	free(jobs);
	free(th);
	free(nstarted);
	free(redo);
	free(sh.chunks);
	pthread_mutex_destroy(&sh.mx);
	return failed ? 1 : 0;
}
