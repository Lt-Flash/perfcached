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
 *
 * S317: a batch is built as the request TREE (perfd_append_tree) and
 * its reply read as a tree (perfd_next_reply_tree): keys and values are
 * bulks, byte-exact, never through the JSON edge whose rendering is
 * lossy for bytes that are not UTF-8.  Each sent batch keeps its
 * encoded records (offset, length) so a misrouted one is re-added to
 * another batch as it was encoded.
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
#include "../src/ptree.h"

#define VERSION     "0.1"
#define MAXTOK      (1 << 17)
#define RECMAX      (1 << 16)          /* PCACHE_CELL_MAX: the largest key or value */
#define MAXMEM      64
#define MAXMAP      64
/* the batch is sent once its tree reaches this; the record that tipped
 * it is already in, so a batch runs over by at most one record (two
 * RECMAX cells and ~70 bytes of framing), and 880 KB + 128 KB stays
 * under the daemon's 1 MB frame payload (PC_MAX_REQ) - there is no
 * base64 inflation to leave room for any more */
#define BATCH_BYTES (880u << 10)
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
	const char *enable;            /* S129: [secrets] enable */
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
 * records are cut out of it (each record's encoded item, by offset and
 * length inside the batch tree) and re-sent.  tree NULL = a `set`. */
struct sent {
	unsigned char *tree;
	size_t *roff, *rlen;
	int rerouted;                  /* 1 = already a second hop: no third */
	int nrec;
};

struct handle {
	perfd_t *p;
	int mem;                       /* member index, -1 = the --to address */
	struct pc_tw w;                /* the batch under construction */
	size_t *roff, *rlen;           /* its records, as encoded */
	int rcap;
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
		unsigned long long *exp, unsigned long long *ver, unsigned char *fl)
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
	*fl = h[24];                       /* S280: bit 0 = a JSON document */
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
	if (!p) {
		fprintf(stderr, "perfload: %s:%d: %s\n", host, port, perfd_error(NULL));
		return p;
	}
	/* S129: `restore` is privileged, and this tool fans out across N
	 * connections - the bit is PER CONNECTION, so every one of them has
	 * to raise it, including any opened later for a retry. */
	if (sh->o->enable) {
		char params[512];
		char *res;
		int n = snprintf(params, sizeof params, "{\"secret\":\"%s\"}",
			sh->o->enable);

		res = n > 0 && (size_t)n < sizeof params
			? perfd_command(p, "enable", params) : NULL;
		if (!res || !strstr(res, "\"privileged\":true")) {
			fprintf(stderr, "perfload: %s:%d: enable failed - restore "
				"needs a privileged connection (--enable takes "
				"[secrets] enable, not the client secret)\n",
				host, port);
			free(res);
			perfd_free(p);
			return NULL;
		}
		free(res);
	}
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

/* the batch tree opens: {col, policy, records: [ ...  - the records are
 * appended until batch_send closes both containers */
static void batch_begin(struct job *j, struct handle *h)
{
	pc_tw_init(&h->w, NULL, 0);
	pc_tw_map(&h->w);
	pc_tw_key(&h->w, "col");
	pc_tw_str(&h->w, j->col);
	pc_tw_key(&h->w, "policy");
	pc_tw_str(&h->w, j->sh->o->policy);
	pc_tw_key(&h->w, "records");
	pc_tw_arr(&h->w);
	h->nrec = 0;
	h->rerouted = 0;
}

/* remember where the record just written sits in the batch tree */
static int batch_note(struct handle *h, size_t off, size_t len)
{
	if (h->nrec >= h->rcap) {
		int nc = h->rcap ? h->rcap * 2 : 256;
		size_t *no = realloc(h->roff, (size_t)nc * sizeof *no);
		size_t *nl = no ? realloc(h->rlen, (size_t)nc * sizeof *nl) : NULL;

		if (no)
			h->roff = no;
		if (!nl)
			return -1;
		h->rlen = nl;
		h->rcap = nc;
	}
	h->roff[h->nrec] = off;
	h->rlen[h->nrec] = len;
	return 0;
}

static void batch_send(struct job *j, struct handle *h)
{
	struct sent *s;

	if (!h->w.b || h->nrec == 0)
		return;
	pc_tw_end(&h->w);                  /* records */
	pc_tw_end(&h->w);                  /* the map */
	if (pc_tw_done(&h->w) != 0) {
		fprintf(stderr, "perfload: a batch did not fit its frame\n");
		j->failed = 1;
		pc_tw_free(&h->w);
		memset(&h->w, 0, sizeof h->w);
		return;
	}
	if (!handle_conn(j, h) ||
	    perfd_append_tree(h->p, "restore", h->w.b, h->w.n) != 0) {
		if (h->p)
			fprintf(stderr, "perfload: append: %s\n", perfd_error(h->p));
		j->failed = 1;
		pc_tw_free(&h->w);
		memset(&h->w, 0, sizeof h->w);
		return;
	}
	/* the sent batch keeps the tree and its record offsets until the
	 * reply names what was misrouted */
	s = &h->sent[h->nsent++];
	s->tree = h->w.b;
	s->roff = h->roff;
	s->rlen = h->rlen;
	s->rerouted = h->rerouted;
	s->nrec = h->nrec;
	memset(&h->w, 0, sizeof h->w);
	h->roff = h->rlen = NULL;
	h->rcap = 0;
	h->nrec = 0;
	if (h->nsent >= h->cap)
		drain(j, h);
}

/* one record into @h's batch; @raw is a ready-made record item (a
 * re-route), else the fields are encoded here: {k, v, exp, ver, t?} with
 * the key and the value as bulks, exact */
static void batch_add(struct job *j, struct handle *h, const unsigned char *raw,
		size_t rawlen, const char *key, uint32_t kl, const char *val,
		uint32_t vl, unsigned long long exp, unsigned long long ver,
		unsigned char fl, int rerouted)
{
	size_t at;

	if (!h->w.b)
		batch_begin(j, h);
	at = h->w.n;
	if (raw) {
		pc_tw_raw(&h->w, raw, rawlen);
	} else {
		pc_tw_map(&h->w);
		pc_tw_key(&h->w, "k");
		pc_tw_bulk(&h->w, key, kl);
		pc_tw_key(&h->w, "v");
		pc_tw_bulk(&h->w, val, vl);
		pc_tw_key(&h->w, "exp");
		pc_tw_i64(&h->w, (long long)exp);
		pc_tw_key(&h->w, "ver");
		pc_tw_i64(&h->w, (long long)ver);
		if (fl & 0x01) {               /* S280 */
			pc_tw_key(&h->w, "t");
			pc_tw_str(&h->w, "json");
		}
		pc_tw_end(&h->w);
	}
	if (h->w.over || batch_note(h, at, h->w.n - at) != 0) {
		fprintf(stderr, "perfload: out of memory building a batch\n");
		j->failed = 1;
		return;
	}
	h->nrec++;
	if (rerouted)
		h->rerouted = 1;
	if (h->nrec >= j->sh->o->batch || h->w.n >= BATCH_BYTES)
		batch_send(j, h);
}

/* --via-set: one plain `set` per record, the TTL re-based, the version
 * the daemon's own - the comparator */
static void set_add(struct job *j, struct handle *h, const char *key, uint32_t kl,
		const char *val, uint32_t vl, unsigned long long exp)
{
	struct pc_tw w;
	struct sent *s;

	if (exp && exp <= now_ms()) {          /* expired at load: skipped, as restore does */
		pthread_mutex_lock(&j->sh->mx);
		j->sh->expired++;
		pthread_mutex_unlock(&j->sh->mx);
		return;
	}
	pc_tw_init(&w, NULL, 0);
	pc_tw_map(&w);
	pc_tw_key(&w, "col");
	pc_tw_str(&w, j->col);
	pc_tw_key(&w, "key");
	pc_tw_bulk(&w, key, kl);
	pc_tw_key(&w, "value");
	pc_tw_bulk(&w, val, vl);
	if (exp) {
		unsigned long long now = now_ms(), ttl = exp > now ? (exp - now + 999) / 1000 : 1;

		pc_tw_key(&w, "ttl");
		pc_tw_i64(&w, (long long)ttl);
	}
	pc_tw_end(&w);
	if (pc_tw_done(&w) != 0 || !handle_conn(j, h) ||
	    perfd_append_tree(h->p, "set", w.b, w.n) != 0) {
		j->failed = 1;
		pc_tw_free(&w);
		return;
	}
	s = &h->sent[h->nsent++];
	s->tree = NULL;
	s->roff = s->rlen = NULL;
	s->rerouted = 0;
	s->nrec = 1;
	pc_tw_free(&w);
	if (h->nsent >= h->cap)
		drain(j, h);
}

static void sent_free(struct sent *s)
{
	free(s->tree);
	free(s->roff);
	free(s->rlen);
}

/* the reply map's integer @key, 0 when absent */
static long long tv_num(const struct pc_tv *v, int map, const char *key)
{
	long long n;

	return pc_tv_get_int(v, map, key, &n) == 0 ? n : 0;
}

/* read every reply @h has outstanding, tally it, re-send what came back
 * misrouted to the node the daemon named */
static void drain(struct job *j, struct handle *h)
{
	struct shared *sh = j->sh;
	struct sent *sent;
	unsigned char **res;
	size_t *rn;
	int n = h->nsent, k;

	if (n == 0 || !h->p)
		return;
	sent = malloc((size_t)n * sizeof *sent);
	res = calloc((size_t)n, sizeof *res);
	rn = calloc((size_t)n, sizeof *rn);
	memcpy(sent, h->sent, (size_t)n * sizeof *sent);
	h->nsent = 0;
	if (perfd_flush(h->p) != 0) {
		fprintf(stderr, "perfload: flush: %s\n", perfd_error(h->p));
		j->failed = 1;
	}
	for (k = 0; k < n; k++) {
		res[k] = perfd_next_reply_tree(h->p, &rn[k]);
		if (!res[k]) {
			if (!j->failed || sh->o->verbose)
				fprintf(stderr, "perfload: %s\n", perfd_error(h->p));
			j->failed = 1;
		}
	}
	for (k = 0; k < n; k++) {
		struct pc_tv v;
		int tm, resent = 0;
		unsigned long long refused;

		memset(&v, 0, sizeof v);
		if (!res[k])
			goto next;
		if (!sent[k].tree) {           /* a set's reply */
			pthread_mutex_lock(&sh->mx);
			sh->stored++;
			pthread_mutex_unlock(&sh->mx);
			goto next;
		}
		if (pc_tv_parse(&v, res[k], rn[k]) != 0 || v.n[0].type != 'm')
			goto next;
		refused = (unsigned long long)tv_num(&v, 0, "refused");
		tm = pc_tv_get(&v, 0, "misrouted");
		if (tm >= 0 && v.n[tm].type == 'a' && !sent[k].rerouted) {
			unsigned int i;

			/* [index, node] pairs: the record at index goes to the
			 * handle of the node named, as it was encoded */
			for (i = 0; i < v.n[tm].len; i++) {
				int pair = pc_tv_at(&v, tm, i), t1, t2, midx;
				long long idx, node;

				if (pair < 0 || v.n[pair].type != 'a' || v.n[pair].len < 2)
					continue;
				t1 = pc_tv_at(&v, pair, 0);
				t2 = pc_tv_at(&v, pair, 1);
				if (t1 < 0 || t2 < 0 || v.n[t1].type != 'i' ||
				    v.n[t2].type != 'i')
					continue;
				idx = v.n[t1].i;
				node = v.n[t2].i;
				midx = handle_by_node(sh, (int)node);
				if (midx < 0 || midx >= j->nh || idx < 0 || idx >= sent[k].nrec)
					continue;
				batch_add(j, &j->hs[midx], sent[k].tree + sent[k].roff[idx],
					sent[k].rlen[idx], NULL, 0, NULL, 0, 0, 0, 0, 1);
				resent++;
			}
		}
		pthread_mutex_lock(&sh->mx);
		sh->batches++;
		sh->stored += (unsigned long long)tv_num(&v, 0, "stored");
		sh->older += (unsigned long long)tv_num(&v, 0, "older");
		sh->existing += (unsigned long long)tv_num(&v, 0, "existing");
		sh->expired += (unsigned long long)tv_num(&v, 0, "expired");
		sh->bad += (unsigned long long)tv_num(&v, 0, "bad");
		sh->refused += refused - (unsigned long long)resent;
		sh->rerouted += (unsigned long long)resent;
		pthread_mutex_unlock(&sh->mx);
next:
		pc_tv_free(&v);
		free(res[k]);
		sent_free(&sent[k]);
	}
	free(res);
	free(rn);
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
		unsigned char fl;
		int rc = -1;

		if (reader_open(&r, sh, f) == 0) {
			while ((rc = reader_next(&r, key, &kl, val, &vl, &exp, &ver, &fl)) == 1)
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
		unsigned char fl = 0;
		int rc = 0;

		snprintf(colbuf, sizeof colbuf, "%s", map_col(sh, f->col));
		j->col = colbuf;
		if (reader_open(&r, sh, f) != 0) {
			fprintf(stderr, "perfload: cannot read %s\n", f->name);
			j->failed = 1;
			break;
		}
		while (!j->failed && (rc = reader_next(&r, key, &kl, val, &vl, &exp, &ver, &fl)) == 1) {
			struct handle *h;

			j->records++;
			j->bytes += kl + vl;
			if (o->dry)
				continue;
			h = o->via_set ? &j->hs[j->idx % j->nh] : pick(j, colbuf, key, kl);
			if (o->via_set)
				set_add(j, h, key, kl, val, vl, exp);
			else
				batch_add(j, h, NULL, 0, key, kl, val, vl, exp, ver, fl, 0);
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
		int k;

		pc_tw_free(&j->hs[i].w);
		free(j->hs[i].roff);
		free(j->hs[i].rlen);
		if (j->hs[i].p)
			perfd_free(j->hs[i].p);
		/* a failed run may leave batches sent and never drained */
		for (k = 0; k < j->hs[i].nsent; k++)
			sent_free(&j->hs[i].sent[k]);
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
		else if (!strcmp(a, "--enable") && v) { o.enable = v; i++; }
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
