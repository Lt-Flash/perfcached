/*
 * perfload - S113: a parallel loader over the client door.
 *
 * Reads a perfdump file set (doc/perfdump-format.md) and streams it back
 * through N pipelined connections with the `restore` batch verb: every
 * record with ITS version and ITS absolute expiry, installed through the
 * daemon's write path (WAL, the eager push), so a fleet of any size or
 * mode takes it as if a client had written it - only with the history
 * the dump held.
 *
 * Routing is the library's (S35 perfd_owner_of): a shard fleet's records
 * go straight to their owners; an eager or plain fleet takes the files
 * spread across its members.  The daemon never forwards a restore (the
 * forward frame carries no version): a record it does not own comes back
 * NAMED in the reply, with the node that owns or holds it, and the loader
 * re-sends it there once.
 *
 * Policy `newer` is the store's own rule (a copy older than, or as old
 * as, the one held loses), so a re-run over a finished load stores
 * nothing and counts everything as older; that is what makes a resume
 * safe.  A chunk file is marked done in DIR/perfload.done once its last
 * batch is acknowledged; the next run skips it.
 *
 * `--via-set` is the comparator and the fail-first control: the same
 * records through plain pipelined `set`, which re-bases the TTL and
 * assigns fresh versions - the two things a loader must NOT do.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../lib/perfd.h"
#include "../src/json.h"

#define VERSION     "0.1"
#define MAXTOK      (1 << 17)
#define RECMAX      (1 << 16)          /* PCACHE_CELL_MAX: the largest key or value */
#define MAXMEM      64
#define MAXMAP      64
#define BATCH_BYTES (600u << 10)       /* under the daemon's 1 MB request line, base64 and all */
#define BATCH_MAX   4096
#define DEPTH_MAX   64
#define SETTLE_MS   60000

struct file {
	char name[256], col[64];
	unsigned long long records, bytes;
	uint32_t crc;
	int zstd, complete, done, bad;
};

struct opts {
	const char *dir, *host, *secret, *cols, *map, *policy;
	int port, threads, batch, depth, rate, dry, verify, restart, settle,
	    via_set, route, verbose;
};

struct member {
	char addr[128];
	int port, node;
};

struct shared {
	struct opts *o;
	pthread_mutex_t mx;
	struct file *files;
	int nfiles, next, skipped;
	struct member mem[MAXMEM];
	int nmem;
	FILE *donef;
	char map_from[MAXMAP][64], map_to[MAXMAP][64];
	int nmap;
	/* the tally */
	unsigned long long records, bytes, batches;
	unsigned long long stored, older, existing, expired, refused, bad, rerouted;
	int failed;
};

/* one appended request, kept until its reply is read: the misrouted
 * records are cut out of it and re-sent */
struct sent {
	char *json;
	int rerouted;                  /* 1 = already a second hop: no third */
	int nrec;
};

struct handle {
	perfd_t *p;
	int mem;                       /* member index, -1 = the --to address */
	struct pc_jw w;                /* the batch under construction */
	int nrec, rerouted;
	struct sent *sent;
	int nsent, cap;
};

struct job {
	struct shared *sh;
	int idx;
	perfd_t *disc;                 /* the routing view (perfd_owner_of) */
	struct handle *hs;
	int nh;
	const char *col;               /* target collection of the file in progress */
	unsigned long long records, bytes, paced, t0;
	int failed;
};

/* ---- crc32 (IEEE), the same table perfdump writes with ---------------- */
static uint32_t crc_tab[256];
static void crc_init(void)
{
	uint32_t c;
	int n, k;

	for (n = 0; n < 256; n++) {
		c = (uint32_t)n;
		for (k = 0; k < 8; k++)
			c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_tab[n] = c;
	}
}
static uint32_t crc32_upd(uint32_t crc, const void *p, size_t n)
{
	const unsigned char *b = p;

	crc = ~crc;
	while (n--)
		crc = crc_tab[(crc ^ *b++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const unsigned char *p) { uint64_t v = 0; int i; for (i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

static unsigned long long now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)tv.tv_usec / 1000ULL;
}

static int jtrue(const char *b, const struct pc_jtok *t)
{
	return t->type == PC_J_PRIM && t->end - t->start == 4 && !memcmp(b + t->start, "true", 4);
}

static long long jnum(const char *b, const struct pc_jtok *toks, int ntok, int obj, const char *key)
{
	int t = pc_json_get(b, toks, ntok, obj, key);

	return t >= 0 && toks[t].type == PC_J_PRIM ? strtoll(b + toks[t].start, NULL, 10) : 0;
}

/* ---- the manifest and the done file ------------------------------------ */

static char *slurp(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)n + 1);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[n] = 0;
	*len = n;
	return buf;
}

static int manifest_read(struct shared *sh)
{
	char path[512], *buf;
	long n;
	struct pc_jtok *toks;
	int ntok, tf, i, cap = 64;

	snprintf(path, sizeof path, "%s/manifest.json", sh->o->dir);
	buf = slurp(path, &n);
	if (!buf) {
		fprintf(stderr, "perfload: no manifest in %s\n", sh->o->dir);
		return -1;
	}
	toks = malloc((size_t)MAXTOK * sizeof *toks);
	ntok = toks ? pc_json_parse(buf, (size_t)n, toks, MAXTOK) : -1;
	if (ntok < 0) {
		fprintf(stderr, "perfload: manifest unparsable\n");
		free(buf);
		free(toks);
		return -1;
	}
	sh->files = calloc((size_t)cap, sizeof *sh->files);
	tf = pc_json_get(buf, toks, ntok, 0, "files");
	for (i = tf + 1; tf >= 0 && i < ntok; i++) {
		struct file *f;
		int tn, tc, tcomp;

		if (toks[i].parent != tf)
			continue;
		tn = pc_json_get(buf, toks, ntok, i, "file");
		tc = pc_json_get(buf, toks, ntok, i, "collection");
		if (tn < 0 || tc < 0)
			continue;
		if (sh->nfiles == cap) {
			struct file *nf = realloc(sh->files, (size_t)cap * 2 * sizeof *sh->files);

			if (!nf) {
				fprintf(stderr, "perfload: out of memory reading the manifest\n");
				free(buf);
				free(toks);
				return -1;
			}
			sh->files = nf;
			cap *= 2;
		}
		f = &sh->files[sh->nfiles++];
		memset(f, 0, sizeof *f);
		snprintf(f->name, sizeof f->name, "%.*s", toks[tn].end - toks[tn].start, buf + toks[tn].start);
		snprintf(f->col, sizeof f->col, "%.*s", toks[tc].end - toks[tc].start, buf + toks[tc].start);
		f->records = (unsigned long long)jnum(buf, toks, ntok, i, "records");
		f->bytes = (unsigned long long)jnum(buf, toks, ntok, i, "bytes");
		f->crc = (uint32_t)strtoull(buf + toks[pc_json_get(buf, toks, ntok, i, "crc32") > 0 ? pc_json_get(buf, toks, ntok, i, "crc32") : 0].start, NULL, 10);
		f->zstd = (int)jnum(buf, toks, ntok, i, "zstd");
		tcomp = pc_json_get(buf, toks, ntok, i, "complete");
		f->complete = tcomp < 0 || jtrue(buf, &toks[tcomp]);
	}
	free(buf);
	free(toks);
	return 0;
}

/* DIR/perfload.done: one line per chunk file whose last batch was
 * acknowledged - "<file> <records> <target> <policy>" */
static void done_read(struct shared *sh)
{
	char path[512], line[512];
	FILE *f;

	snprintf(path, sizeof path, "%s/perfload.done", sh->o->dir);
	if (sh->o->restart) {
		unlink(path);
		return;
	}
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char name[256];
		int i;

		if (sscanf(line, "%255s", name) != 1)
			continue;
		for (i = 0; i < sh->nfiles; i++)
			if (!strcmp(sh->files[i].name, name) && !sh->files[i].done) {
				sh->files[i].done = 1;
				sh->skipped++;
			}
	}
	fclose(f);
}

static void done_mark(struct shared *sh, struct file *f)
{
	char path[512];

	pthread_mutex_lock(&sh->mx);
	if (!sh->donef) {
		snprintf(path, sizeof path, "%s/perfload.done", sh->o->dir);
		sh->donef = fopen(path, "a");
	}
	if (sh->donef) {
		fprintf(sh->donef, "%s %llu %s:%d %s\n", f->name, f->records,
			sh->o->host, sh->o->port, sh->o->policy);
		fflush(sh->donef);
		fsync(fileno(sh->donef));
	}
	f->done = 1;
	pthread_mutex_unlock(&sh->mx);
}

/* the collection a chunk loads into: --collection-map a=b, else itself */
static const char *map_col(struct shared *sh, const char *col)
{
	int i;

	for (i = 0; i < sh->nmap; i++)
		if (!strcmp(sh->map_from[i], col))
			return sh->map_to[i];
	return col;
}

static int col_wanted(struct shared *sh, const char *col)
{
	const char *s = sh->o->cols;

	if (!s)
		return 1;
	while (*s) {
		const char *e = strchr(s, ',');
		size_t l = e ? (size_t)(e - s) : strlen(s);

		if (l == strlen(col) && !strncmp(s, col, l))
			return 1;
		s = e ? e + 1 : s + l;
	}
	return 0;
}

/* ---- the chunk reader --------------------------------------------------- */

struct reader {
	FILE *pf;
	uint32_t crc;
	unsigned long long count, trailer;
	int eof, bad;
};

static int reader_open(struct reader *r, struct shared *sh, struct file *f)
{
	char cmd[1200];
	unsigned char h[4];

	memset(r, 0, sizeof *r);
	if (f->zstd > 0)
		snprintf(cmd, sizeof cmd, "zstd -dc -q '%s/%s'", sh->o->dir, f->name);
	else
		snprintf(cmd, sizeof cmd, "cat '%s/%s'", sh->o->dir, f->name);
	r->pf = popen(cmd, "r");
	if (!r->pf)
		return -1;
	if (fread(h, 1, 4, r->pf) != 4 || memcmp(h, "PCD1", 4) != 0) {
		pclose(r->pf);
		r->pf = NULL;
		return -1;
	}
	r->crc = crc32_upd(0, h, 4);
	return 0;
}

/* next record into @key/@val (RECMAX each); 1 = a record, 0 = the
 * trailer (r->trailer set), -1 = a framing error */
static int reader_next(struct reader *r, char *key, uint32_t *kl, char *val, uint32_t *vl,
		unsigned long long *exp, unsigned long long *ver)
{
	unsigned char h[25];

	if (fread(h, 1, 4, r->pf) != 4)
		return -1;
	*kl = get32(h);
	if (*kl == 0xFFFFFFFFu) {
		unsigned char tt[8];

		if (fread(tt, 1, 8, r->pf) != 8)
			return -1;
		r->crc = crc32_upd(r->crc, h, 4);
		r->crc = crc32_upd(r->crc, tt, 8);
		r->trailer = get64(tt);
		r->eof = 1;
		return 0;
	}
	if (fread(h + 4, 1, 21, r->pf) != 21)
		return -1;
	*vl = get32(h + 4);
	*exp = get64(h + 8);
	*ver = get64(h + 16);
	if (*kl > RECMAX || *vl > RECMAX)
		return -1;
	if (fread(key, 1, *kl, r->pf) != *kl || fread(val, 1, *vl, r->pf) != *vl)
		return -1;
	r->crc = crc32_upd(r->crc, h, 25);
	r->crc = crc32_upd(r->crc, key, *kl);
	r->crc = crc32_upd(r->crc, val, *vl);
	r->count++;
	return 1;
}

static void reader_close(struct reader *r)
{
	if (r->pf)
		pclose(r->pf);
	r->pf = NULL;
}

/* ---- the connections ------------------------------------------------- */

static perfd_t *dial(struct shared *sh, const char *host, int port, int learn)
{
	perfd_opts po;
	const char *secrets[2];
	perfd_t *p;

	memset(&po, 0, sizeof po);
	po.io_timeout_ms = 30000;
	po.spares = learn ? 0 : PERFD_SPARES_NONE;
	po.route_keys = learn ? 1 : 0;
	secrets[0] = sh->o->secret;
	secrets[1] = NULL;
	if (sh->o->secret)
		po.secrets = secrets;
	p = perfd_connect(host, port, &po);
	if (!p)
		fprintf(stderr, "perfload: %s:%d: %s\n", host, port, perfd_error(NULL));
	return p;
}

static perfd_t *handle_conn(struct job *j, struct handle *h)
{
	struct shared *sh = j->sh;

	if (h->p)
		return h->p;
	if (h->mem < 0)
		h->p = dial(sh, sh->o->host, sh->o->port, 0);
	else
		h->p = dial(sh, sh->mem[h->mem].addr, sh->mem[h->mem].port, 0);
	if (!h->p)
		j->failed = 1;
	return h->p;
}

static int handle_by_node(struct shared *sh, int node)
{
	int i;

	for (i = 0; i < sh->nmem; i++)
		if (sh->mem[i].node == node)
			return i;
	return -1;
}

/* which handle takes @key: the owner when the library routes it, else
 * this thread's member, else the --to address */
static struct handle *pick(struct job *j, const char *col, const char *key, uint32_t kl)
{
	struct shared *sh = j->sh;
	int idx = -1;

	if (sh->nmem == 0)
		return &j->hs[0];
	if (j->disc && sh->o->route) {
		char kz[RECMAX + 1];

		memcpy(kz, key, kl);
		kz[kl] = 0;
		idx = perfd_owner_of(j->disc, col, kz);
	}
	if (idx < 0 || idx >= j->nh)
		idx = sh->o->route ? j->idx % j->nh : 0;
	return &j->hs[idx];
}

static void drain(struct job *j, struct handle *h);

static void batch_begin(struct job *j, struct handle *h)
{
	memset(&h->w, 0, sizeof h->w);
	(void)pc_jw_init_heap(&h->w, 1 << 16);
	pc_jw_lit(&h->w, "{\"col\":");
	pc_jw_str(&h->w, j->col, strlen(j->col));
	pc_jw_lit(&h->w, ",\"policy\":");
	pc_jw_str(&h->w, j->sh->o->policy, strlen(j->sh->o->policy));
	pc_jw_lit(&h->w, ",\"records\":[");
	h->nrec = 0;
	h->rerouted = 0;
}

static void batch_send(struct job *j, struct handle *h)
{
	struct sent *s;
	char *json;

	if (!h->w.buf || h->nrec == 0)
		return;
	pc_jw_lit(&h->w, "]}");
	json = malloc(h->w.len + 1);
	memcpy(json, h->w.buf, h->w.len);
	json[h->w.len] = 0;
	pc_jw_free(&h->w);
	memset(&h->w, 0, sizeof h->w);
	if (!handle_conn(j, h)) {
		free(json);
		return;
	}
	if (perfd_append(h->p, "restore", json) != 0) {
		fprintf(stderr, "perfload: append: %s\n", perfd_error(h->p));
		j->failed = 1;
		free(json);
		return;
	}
	s = &h->sent[h->nsent++];
	s->json = json;
	s->rerouted = h->rerouted;
	s->nrec = h->nrec;
	h->nrec = 0;
	if (h->nsent >= h->cap)
		drain(j, h);
}

/* one record into @h's batch; @raw is a ready-made record object (a
 * re-route), else the fields are encoded here */
static void batch_add(struct job *j, struct handle *h, const char *raw, size_t rawlen,
		const char *key, uint32_t kl, const char *val, uint32_t vl,
		unsigned long long exp, unsigned long long ver, int rerouted)
{
	char num[32];

	if (!h->w.buf)
		batch_begin(j, h);
	if (h->nrec)
		pc_jw_lit(&h->w, ",");
	if (raw) {
		pc_jw_raw(&h->w, raw, rawlen);
	} else {
		pc_jw_lit(&h->w, "{");
		pc_jw_value(&h->w, "k", "k_enc", key, kl);
		pc_jw_lit(&h->w, ",");
		pc_jw_value(&h->w, "v", "enc", val, vl);
		pc_jw_lit(&h->w, ",\"exp\":");
		snprintf(num, sizeof num, "%llu", exp);
		pc_jw_raw(&h->w, num, strlen(num));
		pc_jw_lit(&h->w, ",\"ver\":");
		snprintf(num, sizeof num, "%llu", ver);
		pc_jw_raw(&h->w, num, strlen(num));
		pc_jw_lit(&h->w, "}");
	}
	h->nrec++;
	if (rerouted)
		h->rerouted = 1;
	if (h->nrec >= j->sh->o->batch || h->w.len >= BATCH_BYTES)
		batch_send(j, h);
}

/* --via-set: one plain `set` per record, the TTL re-based, the version
 * the daemon's own - the comparator */
static void set_add(struct job *j, struct handle *h, const char *key, uint32_t kl,
		const char *val, uint32_t vl, unsigned long long exp)
{
	struct pc_jw w;
	struct sent *s;
	char num[32];

	if (exp && exp <= now_ms()) {          /* expired at load: skipped, as restore does */
		pthread_mutex_lock(&j->sh->mx);
		j->sh->expired++;
		pthread_mutex_unlock(&j->sh->mx);
		return;
	}
	memset(&w, 0, sizeof w);
	(void)pc_jw_init_heap(&w, 4096);
	pc_jw_lit(&w, "{\"col\":");
	pc_jw_str(&w, j->col, strlen(j->col));
	pc_jw_lit(&w, ",\"key\":");
	pc_jw_str(&w, key, kl);
	pc_jw_lit(&w, ",");
	pc_jw_value(&w, "value", "enc", val, vl);
	if (exp) {
		unsigned long long now = now_ms(), ttl = exp > now ? (exp - now + 999) / 1000 : 1;

		pc_jw_lit(&w, ",\"ttl\":");
		snprintf(num, sizeof num, "%llu", ttl);
		pc_jw_raw(&w, num, strlen(num));
	}
	pc_jw_lit(&w, "}");
	pc_jw_raw(&w, "", 1);              /* the writer does not NUL-terminate */
	if (!handle_conn(j, h) || perfd_append(h->p, "set", w.buf) != 0) {
		j->failed = 1;
		pc_jw_free(&w);
		return;
	}
	s = &h->sent[h->nsent++];
	s->json = NULL;
	s->rerouted = 0;
	s->nrec = 1;
	pc_jw_free(&w);
	if (h->nsent >= h->cap)
		drain(j, h);
}

/* the i-th record object of a sent batch, verbatim */
static int record_raw(const char *json, struct pc_jtok *toks, int ntok, int want,
		const char **raw, size_t *rawlen)
{
	int tr = pc_json_get(json, toks, ntok, 0, "records"), i, n = 0;

	for (i = tr + 1; tr >= 0 && i < ntok; i++) {
		if (toks[i].parent != tr)
			continue;
		if (n++ == want) {
			*raw = json + toks[i].start;
			*rawlen = (size_t)(toks[i].end - toks[i].start);
			return 0;
		}
	}
	return -1;
}

/* read every reply @h has outstanding, tally it, re-send what came back
 * misrouted to the node the daemon named */
static void drain(struct job *j, struct handle *h)
{
	struct shared *sh = j->sh;
	struct sent *sent;
	char **res;
	int n = h->nsent, k;

	if (n == 0 || !h->p)
		return;
	sent = malloc((size_t)n * sizeof *sent);
	res = calloc((size_t)n, sizeof *res);
	memcpy(sent, h->sent, (size_t)n * sizeof *sent);
	h->nsent = 0;
	if (perfd_flush(h->p) != 0) {
		fprintf(stderr, "perfload: flush: %s\n", perfd_error(h->p));
		j->failed = 1;
	}
	for (k = 0; k < n; k++) {
		res[k] = perfd_next_reply(h->p);
		if (!res[k]) {
			if (!j->failed || sh->o->verbose)
				fprintf(stderr, "perfload: %s\n", perfd_error(h->p));
			j->failed = 1;
		}
	}
	for (k = 0; k < n; k++) {
		struct pc_jtok *toks = NULL;
		int ntok = 0, tm, resent = 0;
		unsigned long long refused;

		if (!res[k])
			goto next;
		if (!sent[k].json) {           /* a set's reply */
			pthread_mutex_lock(&sh->mx);
			sh->stored++;
			pthread_mutex_unlock(&sh->mx);
			goto next;
		}
		toks = malloc((size_t)MAXTOK * sizeof *toks);
		ntok = pc_json_parse(res[k], strlen(res[k]), toks, MAXTOK);
		if (ntok < 0)
			goto next;
		refused = (unsigned long long)jnum(res[k], toks, ntok, 0, "refused");
		tm = pc_json_get(res[k], toks, ntok, 0, "misrouted");
		if (tm >= 0 && !sent[k].rerouted) {
			struct pc_jtok *btoks = NULL;
			int bntok = -1, i;

			for (i = tm + 1; i < ntok; i++) {
				int t1, t2, idx, node, midx;
				const char *raw;
				size_t rawlen;

				if (toks[i].parent != tm)
					continue;
				t1 = i + 1;
				t2 = i + 2;
				if (t2 >= ntok || toks[t1].parent != i || toks[t2].parent != i)
					continue;
				idx = (int)strtol(res[k] + toks[t1].start, NULL, 10);
				node = (int)strtol(res[k] + toks[t2].start, NULL, 10);
				midx = handle_by_node(sh, node);
				if (midx < 0 || midx >= j->nh)
					continue;
				if (!btoks) {
					btoks = malloc((size_t)MAXTOK * sizeof *btoks);
					bntok = pc_json_parse(sent[k].json, strlen(sent[k].json), btoks, MAXTOK);
				}
				if (bntok < 0 || record_raw(sent[k].json, btoks, bntok, idx, &raw, &rawlen) != 0)
					continue;
				batch_add(j, &j->hs[midx], raw, rawlen, NULL, 0, NULL, 0, 0, 0, 1);
				resent++;
			}
			free(btoks);
		}
		pthread_mutex_lock(&sh->mx);
		sh->batches++;
		sh->stored += (unsigned long long)jnum(res[k], toks, ntok, 0, "stored");
		sh->older += (unsigned long long)jnum(res[k], toks, ntok, 0, "older");
		sh->existing += (unsigned long long)jnum(res[k], toks, ntok, 0, "existing");
		sh->expired += (unsigned long long)jnum(res[k], toks, ntok, 0, "expired");
		sh->bad += (unsigned long long)jnum(res[k], toks, ntok, 0, "bad");
		sh->refused += refused - (unsigned long long)resent;
		sh->rerouted += (unsigned long long)resent;
		pthread_mutex_unlock(&sh->mx);
next:
		free(toks);
		free(res[k]);
		free(sent[k].json);
	}
	free(res);
	free(sent);
}

static void drain_all(struct job *j)
{
	int i, again;

	/* a drain can re-route into another handle: go round until quiet */
	do {
		again = 0;
		for (i = 0; i < j->nh; i++) {
			batch_send(j, &j->hs[i]);
			if (j->hs[i].nsent)
				drain(j, &j->hs[i]);
		}
		for (i = 0; i < j->nh; i++)
			if (j->hs[i].nrec || j->hs[i].nsent)
				again = 1;
	} while (again && !j->failed);
}

/* ---- the two passes ---------------------------------------------------- */

static struct file *take(struct shared *sh)
{
	struct file *f = NULL;

	pthread_mutex_lock(&sh->mx);
	while (sh->next < sh->nfiles) {
		struct file *c = &sh->files[sh->next++];

		if (c->done || c->bad || !c->complete || !col_wanted(sh, c->col))
			continue;
		f = c;
		break;
	}
	pthread_mutex_unlock(&sh->mx);
	return f;
}

static void *verifier(void *arg)
{
	struct job *j = arg;
	struct shared *sh = j->sh;
	struct file *f;
	char *key = malloc(RECMAX), *val = malloc(RECMAX);

	while ((f = take(sh)) != NULL) {
		struct reader r;
		uint32_t kl, vl;
		unsigned long long exp, ver;
		int rc = -1;

		if (reader_open(&r, sh, f) == 0) {
			while ((rc = reader_next(&r, key, &kl, val, &vl, &exp, &ver)) == 1)
				;
			reader_close(&r);
		}
		if (rc != 0 || r.count != f->records || r.trailer != f->records || r.crc != f->crc) {
			fprintf(stderr, "perfload: %s BAD: read %llu records, trailer %llu, manifest %llu; crc %08x vs manifest %08x\n",
				f->name, r.count, r.trailer, f->records, r.crc, f->crc);
			f->bad = 1;
			j->failed = 1;
		}
	}
	free(key);
	free(val);
	return NULL;
}

static void *loader(void *arg)
{
	struct job *j = arg;
	struct shared *sh = j->sh;
	struct opts *o = sh->o;
	struct file *f;
	char *key = malloc(RECMAX), *val = malloc(RECMAX);
	char colbuf[64];
	int i;

	j->nh = sh->nmem ? sh->nmem : 1;
	j->hs = calloc((size_t)j->nh, sizeof *j->hs);
	for (i = 0; i < j->nh; i++) {
		j->hs[i].mem = sh->nmem ? i : -1;
		j->hs[i].cap = o->depth * (o->via_set ? o->batch : 1);
		j->hs[i].sent = calloc((size_t)j->hs[i].cap, sizeof *j->hs[i].sent);
	}
	if (sh->nmem && o->route && !o->via_set) {
		j->disc = dial(sh, o->host, o->port, 1);
		if (!j->disc)
			j->failed = 1;
	}
	j->t0 = now_ms();
	while (!j->failed && (f = take(sh)) != NULL) {
		struct reader r;
		uint32_t kl, vl;
		unsigned long long exp, ver;
		int rc = 0;

		snprintf(colbuf, sizeof colbuf, "%s", map_col(sh, f->col));
		j->col = colbuf;
		if (reader_open(&r, sh, f) != 0) {
			fprintf(stderr, "perfload: cannot read %s\n", f->name);
			j->failed = 1;
			break;
		}
		while (!j->failed && (rc = reader_next(&r, key, &kl, val, &vl, &exp, &ver)) == 1) {
			struct handle *h;

			j->records++;
			j->bytes += kl + vl;
			if (o->dry)
				continue;
			h = o->via_set ? &j->hs[j->idx % j->nh] : pick(j, colbuf, key, kl);
			if (o->via_set)
				set_add(j, h, key, kl, val, vl, exp);
			else
				batch_add(j, h, NULL, 0, key, kl, val, vl, exp, ver, 0);
			if (o->rate > 0) {
				unsigned long long per = (unsigned long long)o->rate / (unsigned long long)o->threads + 1, due, now;

				j->paced++;
				due = j->t0 + j->paced * 1000ULL / per;
				now = now_ms();
				if (now < due)
					usleep((useconds_t)((due - now) * 1000ULL));
			}
		}
		reader_close(&r);
		if (rc < 0) {
			fprintf(stderr, "perfload: %s: framing error after %llu records\n", f->name, r.count);
			j->failed = 1;
		}
		if (!o->dry)
			drain_all(j);
		if (!j->failed && !o->dry)
			done_mark(sh, f);
	}
	for (i = 0; i < j->nh; i++) {
		if (j->hs[i].w.buf)
			pc_jw_free(&j->hs[i].w);
		if (j->hs[i].p)
			perfd_free(j->hs[i].p);
		free(j->hs[i].sent);
	}
	free(j->hs);
	if (j->disc)
		perfd_free(j->disc);
	free(key);
	free(val);
	return NULL;
}

/* ---- the fleet ----------------------------------------------------------- */

/* the target's collections (a chunk mapped to one the target lacks is
 * refused before anything is loaded) and its members */
static int learn(struct shared *sh, char cols[][64], int *ncols)
{
	perfd_t *p = dial(sh, sh->o->host, sh->o->port, 1);
	char *res;
	struct pc_jtok *toks;
	int ntok, i, n;

	if (!p)
		return -1;
	n = perfd_member_count(p);
	for (i = 0; i < n && i < MAXMEM; i++) {
		struct member *m = &sh->mem[sh->nmem];

		if (perfd_member_info(p, i, m->addr, sizeof m->addr, &m->port, &m->node, NULL) == 0)
			sh->nmem++;
	}
	res = perfd_command(p, "stats", NULL);
	if (!res) {
		fprintf(stderr, "perfload: stats: %s\n", perfd_error(p));
		perfd_free(p);
		return -1;
	}
	toks = malloc((size_t)MAXTOK * sizeof *toks);
	ntok = pc_json_parse(res, strlen(res), toks, MAXTOK);
	*ncols = 0;
	if (ntok >= 0) {
		int tc = pc_json_get(res, toks, ntok, 0, "collections");

		for (i = tc + 1; tc >= 0 && i < ntok && *ncols < 64; i++) {
			int tn;

			if (toks[i].parent != tc)
				continue;
			tn = pc_json_get(res, toks, ntok, i, "name");
			if (tn < 0)
				continue;
			snprintf(cols[*ncols], 64, "%.*s", toks[tn].end - toks[tn].start, res + toks[tn].start);
			(*ncols)++;
		}
	}
	free(res);
	free(toks);
	perfd_free(p);
	return 0;
}

/* entries of @col on every member, summed over the collections loaded */
static long long entries_of(perfd_t *p, char cols[][64], int ncols)
{
	char *res = perfd_command(p, "stats", NULL);
	struct pc_jtok *toks;
	int ntok, i, c;
	long long sum = -1;

	if (!res)
		return -1;
	toks = malloc((size_t)MAXTOK * sizeof *toks);
	ntok = pc_json_parse(res, strlen(res), toks, MAXTOK);
	if (ntok >= 0) {
		int tc = pc_json_get(res, toks, ntok, 0, "collections");

		sum = 0;
		for (i = tc + 1; tc >= 0 && i < ntok; i++) {
			int tn;

			if (toks[i].parent != tc)
				continue;
			tn = pc_json_get(res, toks, ntok, i, "name");
			for (c = 0; tn >= 0 && c < ncols; c++)
				if (pc_json_streq(res, &toks[tn], cols[c]))
					sum += jnum(res, toks, ntok, i, "entries");
		}
	}
	free(toks);
	free(res);
	return sum;
}

/* after the load: poll every member until its entry counts have stood
 * still for a second (an eager fleet's push catching up), or give up */
static void settle(struct shared *sh, char cols[][64], int ncols)
{
	perfd_t *ps[MAXMEM];
	long long last[MAXMEM], cur[MAXMEM];
	unsigned long long t0 = now_ms(), quiet_since = 0;
	int i, n = sh->nmem, ok = 0;

	for (i = 0; i < n; i++) {
		ps[i] = dial(sh, sh->mem[i].addr, sh->mem[i].port, 0);
		last[i] = -2;
	}
	for (;;) {
		int same = 1;
		unsigned long long now;

		for (i = 0; i < n; i++) {
			cur[i] = ps[i] ? entries_of(ps[i], cols, ncols) : -1;
			if (cur[i] != last[i])
				same = 0;
			last[i] = cur[i];
		}
		now = now_ms();
		if (!same)
			quiet_since = now;
		else if (now - quiet_since >= 1000) {
			ok = 1;
			break;
		}
		if (now - t0 > SETTLE_MS)
			break;
		usleep(200000);
	}
	printf("perfload: %s after %.1f s; entries", ok ? "peers settled" : "peers did NOT settle",
		(double)(now_ms() - t0 - (ok ? 1000 : 0)) / 1000.0);
	for (i = 0; i < n; i++)
		printf(" %s:%d=%lld", sh->mem[i].addr, sh->mem[i].port, cur[i]);
	printf("\n");
	for (i = 0; i < n; i++)
		if (ps[i])
			perfd_free(ps[i]);
}

static void usage(FILE *f)
{
	fprintf(f, "perfload " VERSION " - load a perfdump file set into a perfcached node or fleet\n"
		"usage: perfload DIR --to HOST:PORT [options]\n"
		"  -a SECRET            client secret (default: $PERFCLI_AUTH; none = plaintext)\n"
		"  --threads N          loader connections (default 4)\n"
		"  --batch K            records per restore call (default 256, max 4096)\n"
		"  --depth D            restore calls in flight per connection (default 8, max 64)\n"
		"  --policy P           newer (default) | skip | overwrite\n"
		"  --collections a,b    load only these collections\n"
		"  --collection-map a=b load collection a's chunks into collection b\n"
		"  --rate R             records per second, all threads together\n"
		"  --dry-run            read, verify and count; load nothing\n"
		"  --no-verify          skip the checksum pass\n"
		"  --restart            ignore DIR/perfload.done: load every chunk again\n"
		"  --no-settle          do not wait for the members' entry counts to stand still\n"
		"  --no-route           send everything to --to; the daemon names what belongs elsewhere\n"
		"  --via-set            the comparator: plain pipelined set (TTL re-based, fresh versions)\n"
		"  -v                   verbose\n");
}

int main(int argc, char **argv)
{
	struct opts o;
	struct shared sh;
	struct job *jobs;
	pthread_t *th;
	char hostbuf[256], tcols[64][64], loaded[64][64];
	int ntcols = 0, nloaded = 0, i, failed = 0;
	unsigned long long t0, t1, records = 0, bytes = 0;

	memset(&o, 0, sizeof o);
	o.threads = 4;
	o.batch = 256;
	o.depth = 8;
	o.policy = "newer";
	o.verify = 1;
	o.settle = 1;
	o.route = 1;
	o.secret = getenv("PERFCLI_AUTH");
	for (i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!a)
			break;
		if (!strcmp(a, "--to") && v) { o.host = v; i++; }
		else if (!strcmp(a, "-a") && v) { o.secret = v; i++; }
		else if (!strcmp(a, "--threads") && v) { o.threads = atoi(v); i++; }
		else if (!strcmp(a, "--batch") && v) { o.batch = atoi(v); i++; }
		else if (!strcmp(a, "--depth") && v) { o.depth = atoi(v); i++; }
		else if (!strcmp(a, "--policy") && v) { o.policy = v; i++; }
		else if (!strcmp(a, "--collections") && v) { o.cols = v; i++; }
		else if (!strcmp(a, "--collection-map") && v) { o.map = v; i++; }
		else if (!strcmp(a, "--rate") && v) { o.rate = atoi(v); i++; }
		else if (!strcmp(a, "--dry-run")) o.dry = 1;
		else if (!strcmp(a, "--no-verify")) o.verify = 0;
		else if (!strcmp(a, "--restart")) o.restart = 1;
		else if (!strcmp(a, "--no-settle")) o.settle = 0;
		else if (!strcmp(a, "--no-route")) o.route = 0;
		else if (!strcmp(a, "--via-set")) o.via_set = 1;
		else if (!strcmp(a, "-v")) o.verbose = 1;
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
		else if (!strcmp(a, "-V")) { printf("perfload " VERSION " (libperfd %s)\n", PERFD_VERSION); return 0; }
		else if (a[0] != '-' && !o.dir) o.dir = a;
		else { usage(stderr); return 2; }
	}
	if (!o.dir || !o.host || o.threads < 1 || o.threads > 64 || o.batch < 1 || o.batch > BATCH_MAX ||
	    o.depth < 1 || o.depth > DEPTH_MAX ||
	    (strcmp(o.policy, "newer") && strcmp(o.policy, "skip") && strcmp(o.policy, "overwrite"))) {
		usage(stderr);
		return 2;
	}
	{
		const char *colon = strrchr(o.host, ':');

		if (!colon) {
			fprintf(stderr, "perfload: --to needs HOST:PORT\n");
			return 2;
		}
		snprintf(hostbuf, sizeof hostbuf, "%.*s", (int)(colon - o.host), o.host);
		o.host = hostbuf;
		o.port = atoi(colon + 1);
	}
	crc_init();
	memset(&sh, 0, sizeof sh);
	sh.o = &o;
	pthread_mutex_init(&sh.mx, NULL);
	if (o.map) {
		const char *s = o.map;

		while (*s && sh.nmap < MAXMAP) {
			const char *e = strchr(s, ','), *eq = strchr(s, '=');
			size_t l = e ? (size_t)(e - s) : strlen(s);

			if (!eq || eq > s + l) {
				fprintf(stderr, "perfload: --collection-map wants a=b[,c=d]\n");
				return 2;
			}
			snprintf(sh.map_from[sh.nmap], 64, "%.*s", (int)(eq - s), s);
			snprintf(sh.map_to[sh.nmap], 64, "%.*s", (int)(s + l - eq - 1), eq + 1);
			sh.nmap++;
			s = e ? e + 1 : s + l;
		}
	}
	if (manifest_read(&sh) != 0)
		return 1;
	done_read(&sh);
	if (learn(&sh, tcols, &ntcols) != 0)
		return 1;
	/* the collections this run loads, checked against the target */
	for (i = 0; i < sh.nfiles; i++) {
		const char *tc = map_col(&sh, sh.files[i].col);
		int k, have = 0;

		if (!col_wanted(&sh, sh.files[i].col) || sh.files[i].done)
			continue;
		for (k = 0; k < nloaded; k++)
			if (!strcmp(loaded[k], tc))
				have = 1;
		if (!have && nloaded < 64)
			snprintf(loaded[nloaded++], 64, "%s", tc);
		for (k = 0, have = 0; k < ntcols; k++)
			if (!strcmp(tcols[k], tc))
				have = 1;
		if (!have) {
			fprintf(stderr, "perfload: the target has no collection '%s' (chunk %s)\n", tc, sh.files[i].name);
			return 1;
		}
	}
	jobs = calloc((size_t)o.threads, sizeof *jobs);
	th = calloc((size_t)o.threads, sizeof *th);
	for (i = 0; i < o.threads; i++) {
		jobs[i].sh = &sh;
		jobs[i].idx = i;
	}
	if (o.verify) {
		sh.next = 0;
		for (i = 0; i < o.threads; i++)
			pthread_create(&th[i], NULL, verifier, &jobs[i]);
		for (i = 0; i < o.threads; i++) {
			pthread_join(th[i], NULL);
			failed |= jobs[i].failed;
			jobs[i].failed = 0;
		}
		if (failed) {
			fprintf(stderr, "perfload: a chunk failed verification; nothing loaded\n");
			return 1;
		}
	}
	sh.next = 0;
	t0 = now_ms();
	for (i = 0; i < o.threads; i++)
		pthread_create(&th[i], NULL, loader, &jobs[i]);
	for (i = 0; i < o.threads; i++) {
		pthread_join(th[i], NULL);
		failed |= jobs[i].failed;
		records += jobs[i].records;
		bytes += jobs[i].bytes;
	}
	t1 = now_ms();
	if (sh.donef)
		fclose(sh.donef);
	if (o.dry) {
		int c;

		printf("perfload: dry run: %llu records, %.1f MB in %d chunk(s) would load into %s:%d as",
			records, (double)bytes / 1048576.0, sh.nfiles - sh.skipped, o.host, o.port);
		for (c = 0; c < nloaded; c++)
			printf(" %s", loaded[c]);
		printf("%s; %d member(s), policy %s%s\n", nloaded ? "" : " (nothing)", sh.nmem, o.policy,
			sh.skipped ? " - some chunks already loaded (resume)" : "");
	} else {
		printf("perfload: %llu records, %.1f MB, %d chunk(s), %d collection(s), %.2f s, %.0f records/s; "
			"stored %llu, older %llu, existing %llu, expired %llu, refused %llu, bad %llu, rerouted %llu%s%s\n",
			records, (double)bytes / 1048576.0, sh.nfiles - sh.skipped, nloaded,
			(double)(t1 - t0) / 1000.0,
			t1 > t0 ? (double)records * 1000.0 / (double)(t1 - t0) : 0.0,
			sh.stored, sh.older, sh.existing, sh.expired, sh.refused, sh.bad, sh.rerouted,
			sh.skipped ? " (resumed: some chunks were already loaded)" : "",
			failed ? " - INCOMPLETE, a connection failed" : "");
		if (o.settle && sh.nmem > 1 && !failed)
			settle(&sh, loaded, nloaded);
	}
	if (sh.skipped)
		printf("perfload: %d chunk(s) skipped as already loaded (perfload.done; --restart reloads them)\n", sh.skipped);
	free(jobs);
	free(th);
	free(sh.files);
	pthread_mutex_destroy(&sh.mx);
	return failed ? 1 : 0;
}
