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
 * handle per connection (no spares), speaks the JSON dialect - the walk
 * measured client-bound at 165k records/s with a Python parser; this
 * parser is the daemon's own.
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
	/* per collection */
	char cols[64][64];
	unsigned int nb[64];
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
};

/* a JSON primitive `true`: the tokenizer's string compare is for string
 * tokens only, so a bare literal needs its own test */
static int jtrue(const char *b, const struct pc_jtok *t)
{
	return t->type == PC_J_PRIM && t->end > t->start && b[t->start] == 't';
}

/* the fields of one record object, by walking ITS children only: the
 * tokenizer's pc_json_get() scans from the object to the end of the
 * token array, which made per-record lookups quadratic in the chunk */
struct recf { int k, kenc, v, venc, ttl, ver; };
static void rec_fields(const char *b, const struct pc_jtok *t, int ntok, int obj,
		struct recf *f)
{
	int j, child = 0, want = t[obj].size * 2;   /* children alternate key, value */

	f->k = f->kenc = f->v = f->venc = f->ttl = f->ver = -1;
	for (j = obj + 1; j < ntok && child < want; j++) {
		int len;

		if (t[j].parent != obj)
			continue;
		if ((child++ & 1) != 0)
			continue;                  /* a value: only even children are keys */
		if (t[j].type != PC_J_STR || j + 1 >= ntok)
			continue;
		len = t[j].end - t[j].start;
		if (len == 1 && b[t[j].start] == 'k')
			f->k = j + 1;
		else if (len == 1 && b[t[j].start] == 'v')
			f->v = j + 1;
		else if (len == 3 && !memcmp(b + t[j].start, "ttl", 3))
			f->ttl = j + 1;
		else if (len == 3 && !memcmp(b + t[j].start, "ver", 3))
			f->ver = j + 1;
		else if (len == 3 && !memcmp(b + t[j].start, "enc", 3))
			f->venc = j + 1;
		else if (len == 5 && !memcmp(b + t[j].start, "k_enc", 5))
			f->kenc = j + 1;
	}
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
	struct pc_jtok *toks;
	char *kbuf, *vbuf, *raw;
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
	toks = malloc((size_t)MAXTOK * sizeof *toks);
	kbuf = malloc(RECMAX);
	vbuf = malloc(RECMAX);
	raw = malloc(RECMAX);
	if (!toks || !kbuf || !vbuf || !raw) {
		fprintf(stderr, "perfdump: out of memory\n");
		j->failed = 1;
		return NULL;
	}
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
		char params[512];
		char *res;
		int ntok, tr, tc, tm, i;
		long long ccur;
		int more;
		/* the walker's range: buckets [lo, hi); hi == nb means the tail is
		 * mine and the verb's own end (nb - 1 then the tail) applies */
		if (j->hi < sh->nb[j->col])
			snprintf(params, sizeof params,
				"{\"col\":\"%s\",\"cursor\":%u,\"count\":%d,\"end\":%u}",
				sh->cols[j->col], cursor, count, j->hi);
		else
			snprintf(params, sizeof params,
				"{\"col\":\"%s\",\"cursor\":%u,\"count\":%d}",
				sh->cols[j->col], cursor, count);
		res = perfd_command(p, "dump", params);
		if (!res) {
			const char *e = perfd_error(p);

			if (e && strstr(e, "halve") && count > 1) {
				count /= 2;    /* a chunk that did not fit: smaller buckets */
				continue;
			}
			fprintf(stderr, "perfdump: connection %d: dump failed at cursor %u: %s\n",
				j->idx, cursor, e ? e : "?");
			j->failed = 1;
			break;
		}
		ntok = pc_json_parse(res, strlen(res), toks, MAXTOK);
		if (ntok < 0) {
			fprintf(stderr, "perfdump: connection %d: unparsable reply\n", j->idx);
			free(res);
			j->failed = 1;
			break;
		}
		tr = pc_json_get(res, toks, ntok, 0, "records");
		tc = pc_json_get(res, toks, ntok, 0, "cursor");
		tm = pc_json_get(res, toks, ntok, 0, "more");
		if (tr < 0 || tc < 0 || tm < 0) {
			fprintf(stderr, "perfdump: connection %d: reply without records/cursor/more\n", j->idx);
			free(res);
			j->failed = 1;
			break;
		}
		ccur = strtoll(res + toks[tc].start, NULL, 10);
		more = jtrue(res, &toks[tm]);
		/* every element of the records array: the tokens whose parent is it */
		for (i = tr + 1; i < ntok; i++) {
			struct recf rf;
			int tk, tv, tt, tvr, te, kl, vl;
			long long ttl, ver;
			unsigned long long exp_ms;

			if (toks[i].parent != tr)
				continue;
			rec_fields(res, toks, ntok, i, &rf);
			tk = rf.k; tv = rf.v; tt = rf.ttl; tvr = rf.ver;
			if (tk < 0 || tv < 0 || tt < 0 || tvr < 0)
				continue;
			kl = pc_json_unescape(res, &toks[tk], raw, RECMAX);
			if (kl < 0)
				continue;
			te = rf.kenc;
			if (te >= 0 && pc_json_streq(res, &toks[te], "b64")) {
				kl = pc_b64_dec(raw, (size_t)kl, kbuf, RECMAX);
				if (kl < 0)
					continue;
			} else
				memcpy(kbuf, raw, (size_t)kl);
			vl = pc_json_unescape(res, &toks[tv], raw, RECMAX);
			if (vl < 0)
				continue;
			te = rf.venc;
			if (te >= 0 && pc_json_streq(res, &toks[te], "b64")) {
				vl = pc_b64_dec(raw, (size_t)vl, vbuf, RECMAX);
				if (vl < 0)
					continue;
			} else
				memcpy(vbuf, raw, (size_t)vl);
			ttl = strtoll(res + toks[tt].start, NULL, 10);
			ver = strtoll(res + toks[tvr].start, NULL, 10);
			exp_ms = ttl < 0 ? 0 : now_ms() + (unsigned long long)ttl * 1000ULL;
			if (chunk_put(&ch, kbuf, (size_t)kl, vbuf, (size_t)vl, exp_ms,
			        (unsigned long long)ver, 0) != 0) {
				fprintf(stderr, "perfdump: write failed on %s: %s\n", ch.path, strerror(errno));
				j->failed = 1;
				break;
			}
			j->records++;
			j->bytes += (unsigned long long)(kl + vl);
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
	free(toks);
	free(kbuf);
	free(vbuf);
	free(raw);
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

int main(int argc, char **argv)
{
	struct opts o;
	struct shared sh;
	perfd_opts po;
	const char *secrets[2];
	perfd_t *p;
	char *res, hostbuf[256];
	struct pc_jtok *toks;
	int ntok, i, c, failed = 0;
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

	/* one job per collection per thread: bucket ranges, the last owns the tail */
	jobs = calloc((size_t)sh.ncols * (size_t)o.threads, sizeof *jobs);
	th = calloc((size_t)sh.ncols * (size_t)o.threads, sizeof *th);
	t0 = now_ms();
	for (c = 0; c < sh.ncols; c++) {
		unsigned int nb = sh.nb[c];
		int n = o.threads;

		if ((unsigned int)n > nb)
			n = (int)nb;
		for (i = 0; i < n; i++) {
			struct job *j = &jobs[c * o.threads + i];

			j->sh = &sh;
			j->idx = c * o.threads + i;
			j->col = c;
			j->lo = (unsigned int)((unsigned long long)nb * (unsigned long long)i / (unsigned long long)n);
			j->hi = i == n - 1 ? nb : (unsigned int)((unsigned long long)nb * (unsigned long long)(i + 1) / (unsigned long long)n);
			pthread_create(&th[j->idx], NULL, walker, j);
		}
	}
	tot_bytes = 0;
	for (c = 0; c < sh.ncols; c++) {
		unsigned int nb = sh.nb[c];
		int n = o.threads;

		if ((unsigned int)n > nb)
			n = (int)nb;
		for (i = 0; i < n; i++) {
			struct job *j = &jobs[c * o.threads + i];

			pthread_join(th[j->idx], NULL);
			failed |= j->failed;
			tot_bytes += j->bytes;
		}
	}
	t1 = now_ms();
	printf("perfdump: %llu records, %.1f MB of keys and values, %d file(s), %d collection(s), %.2f s, %.0f records/s%s\n",
		sh.records, (double)tot_bytes / 1048576.0, sh.nchunks, sh.ncols,
		(double)(t1 - t0) / 1000.0,
		t1 > t0 ? (double)sh.records * 1000.0 / (double)(t1 - t0) : 0.0,
		failed ? " - INCOMPLETE, a connection failed" : "");
	free(jobs);
	free(th);
	free(sh.chunks);
	pthread_mutex_destroy(&sh.mx);
	return failed ? 1 : 0;
}
