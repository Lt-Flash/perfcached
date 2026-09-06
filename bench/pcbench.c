/*
 * pcbench — dual-protocol cache bench client (task S10).
 *
 * ONE client, two wire protocols, so perfcached and Redis are measured
 * through the same code path:
 *   -P perf   perfcached text dialect (newline JSON-RPC, plaintext)
 *   -P bin    perfcached BINARY dialect (16B-header frames, raw values;
 *             layouts in src/proto.h) - what libperfd opts.binary and
 *             the OpenSIPS driver ride; measures the wire without the
 *             JSON codec
 *   -P resp   Redis RESP
 *
 * Each thread owns one connection and drives a fixed pipeline depth.
 * Depth 1 records per-op RTT into a 1us-granularity histogram
 * (p50/p99/p999); deeper pipelines measure throughput (batch RTTs are
 * not per-op latency and are not reported as such).  A fill phase
 * populates the keyspace before any timed GET arm; the first -w seconds
 * of each run are discarded (cold caches are not regressions, they are
 * warm-ups).  Replies are counted AND checked: a JSON reply carrying
 * "error" or a RESP -ERR counts as an error, and any error fails the
 * run loudly - a bench that cannot fail is not a bench.
 *
 * usage: pcbench -h host -p port -P perf|resp [-C col] [-c conns]
 *        [-d depth] [-n keys] [-v valsize] [-T secs] [-w warmup]
 *        [-M getpct] [-F] (fill only) [-N] (skip the pre-GET fill: the
 *        keyspace is known-filled - REQUIRED for reads through a
 *        proxy-mode NON-HOLDER, where a refill through an ingress with
 *        an empty locator would place duplicate copies)
 *        [-q] (machine-readable line only)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define HIST_US 100000                 /* 1us buckets up to 100ms, then clamp */

static const char *o_host = "127.0.0.1", *o_proto = "perf", *o_col = "b";
static int o_port = 6379, o_conns = 4, o_depth = 1, o_keys = 100000;
static int o_val = 64, o_secs = 10, o_warm = 2, o_getpct = 100;
static int o_fill_only = 0, o_quiet = 0, o_nofill = 0;

static volatile int t_stop;
static struct timespec t_warm_end;

struct th {
	pthread_t tid;
	int idx, fd;
	unsigned long long ops, errs, bytes_out;
	unsigned int *hist;               /* depth==1 only */
	unsigned long long hist_n;
	int failed;
};

static long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int dial(void)
{
	struct addrinfo hints, *res, *ai;
	char port[16];
	int fd = -1, one = 1;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(port, sizeof port, "%d", o_port);
	if (getaddrinfo(o_host, port, &hints, &res) != 0)
		return -1;
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, 0);
		if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		if (fd >= 0)
			close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd >= 0)
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return fd;
}

static int send_all(int fd, const char *b, size_t n)
{
	ssize_t w;

	while (n) {
		w = write(fd, b, n);
		if (w <= 0)
			return -1;
		b += w;
		n -= (size_t)w;
	}
	return 0;
}

/* ---- request builders -------------------------------------------------- */

static int is_perf, is_bin;

static void le16(unsigned char *b, unsigned int v)
{
	b[0] = (unsigned char)v;
	b[1] = (unsigned char)(v >> 8);
}

static void le32(unsigned char *b, unsigned int v)
{
	int i;

	for (i = 0; i < 4; i++)
		b[i] = (unsigned char)(v >> (8 * i));
}

static int build_req(char *dst, int get, unsigned int key, const char *val)
{
	char kb[24];
	int kl = snprintf(kb, sizeof kb, "k%08u", key);

	if (is_bin) {
		/* frames per src/proto.h: [9E 01 01 00][plen u32][id u64]
		 * then [verb][cn][klen u16][(ttl i64 on set)][col][key][val] */
		unsigned char *d = (unsigned char *)dst;
		size_t cn = strlen(o_col);
		unsigned int plen = get ? 4 + (unsigned int)cn + (unsigned int)kl
			: 4 + 8 + (unsigned int)cn + (unsigned int)kl +
			  (unsigned int)o_val;
		unsigned char *q;

		d[0] = 0x9E; d[1] = 1; d[2] = 1; d[3] = 0;
		le32(d + 4, plen);
		memset(d + 8, 0, 8);
		d[8] = 1;                      /* id: matched per-conn in order */
		q = d + 16;
		*q++ = get ? 2 : 3;            /* PC_VERB_GET / PC_VERB_SET */
		*q++ = (unsigned char)cn;
		le16(q, (unsigned int)kl);
		q += 2;
		if (!get) {
			memset(q, 0, 8);       /* ttl 0 = never */
			q += 8;
		}
		memcpy(q, o_col, cn);
		q += cn;
		memcpy(q, kb, (size_t)kl);
		q += kl;
		if (!get) {
			memcpy(q, val, (size_t)o_val);
			q += o_val;
		}
		return (int)(16 + plen);
	}
	if (is_perf) {
		if (get)
			return sprintf(dst, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
				"\"get\",\"params\":{\"col\":\"%s\",\"key\":\"%s\"}}\n",
				o_col, kb);
		return sprintf(dst, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
			"\"set\",\"params\":{\"col\":\"%s\",\"key\":\"%s\","
			"\"value\":\"%s\"}}\n", o_col, kb, val);
	}
	if (get)
		return sprintf(dst, "*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n", kl, kb);
	return sprintf(dst, "*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
		kl, kb, o_val, val);
}

/* ---- reply counting ---------------------------------------------------- */

/* portable memmem-lite: no _GNU_SOURCE, so no GNU memmem needed */
static const char *memfind(const char *h, size_t hn, const char *nd,
		size_t nn)
{
	size_t i;

	if (nn > hn)
		return NULL;
	for (i = 0; i + nn <= hn; i++)
		if (h[i] == nd[0] && !memcmp(h + i, nd, nn))
			return h + i;
	return NULL;
}

/* count complete replies in buf[0..n); *used = bytes consumed.
 * Returns replies counted; *errs incremented per error reply. */
static int count_perf(const char *buf, size_t n, size_t *used,
		unsigned long long *errs)
{
	const char *p = buf, *e = buf + n, *nl;
	int cnt = 0;

	while ((nl = memchr(p, '\n', (size_t)(e - p)))) {
		if (memfind(p, (size_t)(nl - p), "\"error\"", 7))
			(*errs)++;
		cnt++;
		p = nl + 1;
	}
	*used = (size_t)(p - buf);
	return cnt;
}

/* binary frames: [magic ver type flags][plen u32][id u64][payload].
 * type 2 = response (flags bit0 = error), type 3 = notification
 * (skipped, not counted - replies must pair with requests). */
static int count_bin(const char *buf, size_t n, size_t *used,
		unsigned long long *errs)
{
	const unsigned char *p = (const unsigned char *)buf;
	const unsigned char *e = p + n;
	int cnt = 0;

	while ((size_t)(e - p) >= 16) {
		uint32_t plen = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
			((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);

		if (p[0] != 0x9E) {            /* stream desync: fail loudly */
			(*errs)++;
			p = e;
			break;
		}
		if ((size_t)(e - p) < 16 + (size_t)plen)
			break;
		if (p[2] == 2) {
			if (p[3] & 1)
				(*errs)++;
			cnt++;
		}
		p += 16 + plen;
	}
	*used = (size_t)((const char *)p - buf);
	return cnt;
}

static int count_resp(const char *buf, size_t n, size_t *used,
		unsigned long long *errs)
{
	const char *p = buf, *e = buf + n, *nl;
	long len;
	int cnt = 0;

	*used = 0;
	while (p < e) {
		nl = memchr(p, '\n', (size_t)(e - p));
		if (!nl)
			break;
		if (*p == '$') {               /* bulk: $len\r\n[data\r\n] */
			len = strtol(p + 1, NULL, 10);
			if (len >= 0) {
				if (nl + 1 + len + 2 > e)
					break;
				p = nl + 1 + len + 2;
			} else {
				p = nl + 1;            /* $-1 null bulk */
			}
		} else {                       /* + - : simple line replies */
			if (*p == '-')
				(*errs)++;
			p = nl + 1;
		}
		cnt++;
		*used = (size_t)(p - buf);
	}
	return cnt;
}

/* ---- worker ------------------------------------------------------------ */

static unsigned int xr(unsigned int *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 17;
	*s ^= *s << 5;
	return *s;
}

static void *worker(void *arg)
{
	struct th *t = arg;
	char *out, *in, *val;
	size_t out_len, in_len = 0, used;
	unsigned int seed = (unsigned int)(t->idx * 2654435761u + 7);
	int i, get, warm_done = 0;
	long long t0;
	ssize_t r;

	out = malloc((size_t)o_depth * (size_t)(o_val + 256));
	in = malloc(1 << 20);
	val = malloc((size_t)o_val + 1);
	if (!out || !in || !val) {
		t->failed = 1;
		return NULL;
	}
	memset(val, 'x', (size_t)o_val);
	val[o_val] = 0;

	while (!t_stop) {
		out_len = 0;
		for (i = 0; i < o_depth; i++) {
			get = (int)(xr(&seed) % 100) < o_getpct;
			out_len += (size_t)build_req(out + out_len, get,
				xr(&seed) % (unsigned int)o_keys, val);
		}
		t0 = now_us();
		if (send_all(t->fd, out, out_len) < 0) {
			t->failed = 1;
			break;
		}
		t->bytes_out += out_len;

		for (i = 0; i < o_depth; ) {
			r = read(t->fd, in + in_len, (1 << 20) - in_len);
			if (r <= 0) {
				t->failed = 1;
				goto done;
			}
			in_len += (size_t)r;
			i += is_bin ? count_bin(in, in_len, &used, &t->errs)
			   : is_perf ? count_perf(in, in_len, &used, &t->errs)
			             : count_resp(in, in_len, &used, &t->errs);
			memmove(in, in + used, in_len - used);
			in_len -= used;
		}

		if (!warm_done) {
			struct timespec nowts;

			clock_gettime(CLOCK_MONOTONIC, &nowts);
			if (nowts.tv_sec < t_warm_end.tv_sec ||
			    (nowts.tv_sec == t_warm_end.tv_sec &&
			     nowts.tv_nsec < t_warm_end.tv_nsec))
				continue;              /* discard the cold segment */
			warm_done = 1;
		}
		t->ops += (unsigned long long)o_depth;
		if (t->hist) {
			long long d = now_us() - t0;

			if (d < 0)
				d = 0;
			if (d >= HIST_US)
				d = HIST_US - 1;
			t->hist[d]++;
			t->hist_n++;
		}
	}
done:
	free(out);
	free(in);
	free(val);
	return NULL;
}

/* ---- fill (sequential SET of the whole keyspace, all threads) ---------- */

static int fill(struct th *ts)
{
	char *out, *in, *val;
	size_t out_len, in_len, used;
	int per = o_keys / o_conns, c, k, i, batch;
	ssize_t r;

	out = malloc(64 * (size_t)(o_val + 256));
	in = malloc(1 << 20);
	val = malloc((size_t)o_val + 1);
	if (!out || !in || !val)
		return -1;
	memset(val, 'x', (size_t)o_val);
	val[o_val] = 0;

	for (c = 0; c < o_conns; c++) {
		int lo = c * per, hi = c == o_conns - 1 ? o_keys : lo + per;

		for (k = lo; k < hi; k += 64) {
			batch = k + 64 > hi ? hi - k : 64;
			out_len = 0;
			for (i = 0; i < batch; i++)
				out_len += (size_t)build_req(out + out_len, 0,
					(unsigned int)(k + i), val);
			if (send_all(ts[c].fd, out, out_len) < 0)
				return -1;
			in_len = 0;
			for (i = 0; i < batch; ) {
				r = read(ts[c].fd, in + in_len, (1 << 20) - in_len);
				if (r <= 0)
					return -1;
				in_len += (size_t)r;
				i += is_bin
					? count_bin(in, in_len, &used, &ts[c].errs)
					: is_perf
					? count_perf(in, in_len, &used, &ts[c].errs)
					: count_resp(in, in_len, &used, &ts[c].errs);
				memmove(in, in + used, in_len - used);
				in_len -= used;
			}
		}
	}
	free(out);
	free(in);
	free(val);
	return 0;
}

static long long pct(unsigned int *h, unsigned long long n, double p)
{
	unsigned long long want = (unsigned long long)(p * (double)n), acc = 0;
	int i;

	for (i = 0; i < HIST_US; i++) {
		acc += h[i];
		if (acc > want)
			return i;
	}
	return HIST_US - 1;
}

int main(int argc, char **argv)
{
	struct th *ts;
	struct timespec ts0;
	unsigned int *hist = NULL;
	unsigned long long ops = 0, errs = 0, hn = 0, mb = 0;
	long long t_start, t_end;
	int opt, c, failed = 0;
	double secs;

	while ((opt = getopt(argc, argv, "h:p:P:C:c:d:n:v:T:w:M:FqN")) != -1) {
		switch (opt) {
		case 'h': o_host = optarg; break;
		case 'p': o_port = atoi(optarg); break;
		case 'P': o_proto = optarg; break;
		case 'C': o_col = optarg; break;
		case 'c': o_conns = atoi(optarg); break;
		case 'd': o_depth = atoi(optarg); break;
		case 'n': o_keys = atoi(optarg); break;
		case 'v': o_val = atoi(optarg); break;
		case 'T': o_secs = atoi(optarg); break;
		case 'w': o_warm = atoi(optarg); break;
		case 'M': o_getpct = atoi(optarg); break;
		case 'F': o_fill_only = 1; break;
		case 'N': o_nofill = 1; break;
		case 'q': o_quiet = 1; break;
		default:
			fprintf(stderr, "bad usage\n");
			return 2;
		}
	}
	is_perf = !strcmp(o_proto, "perf");
	is_bin = !strcmp(o_proto, "bin");
	if (o_conns < 1 || o_conns > 256 || o_depth < 1 || o_depth > 1024 ||
	        o_keys < 1 || o_val < 1 || o_val > 60000) {
		fprintf(stderr, "bad parameters\n");
		return 2;
	}

	ts = calloc((size_t)o_conns, sizeof *ts);
	for (c = 0; c < o_conns; c++) {
		ts[c].idx = c;
		ts[c].fd = dial();
		if (ts[c].fd < 0) {
			fprintf(stderr, "connect failed\n");
			return 1;
		}
		if (o_depth == 1 && !o_fill_only)
			ts[c].hist = calloc(HIST_US, sizeof *ts[c].hist);
	}

	if ((o_getpct > 0 && !o_nofill) || o_fill_only) {
		if (fill(ts) < 0) {
			fprintf(stderr, "fill failed\n");
			return 1;
		}
	}
	if (o_fill_only) {
		printf("filled %d keys\n", o_keys);
		return 0;
	}

	clock_gettime(CLOCK_MONOTONIC, &ts0);
	t_warm_end = ts0;
	t_warm_end.tv_sec += o_warm;
	t_start = now_us() + (long long)o_warm * 1000000;

	for (c = 0; c < o_conns; c++)
		pthread_create(&ts[c].tid, NULL, worker, &ts[c]);
	sleep((unsigned int)(o_warm + o_secs));
	t_stop = 1;
	t_end = now_us();
	for (c = 0; c < o_conns; c++)
		pthread_join(ts[c].tid, NULL);

	if (o_depth == 1)
		hist = calloc(HIST_US, sizeof *hist);
	for (c = 0; c < o_conns; c++) {
		ops += ts[c].ops;
		errs += ts[c].errs;
		mb += ts[c].bytes_out;
		failed |= ts[c].failed;
		if (hist && ts[c].hist) {
			int i;

			for (i = 0; i < HIST_US; i++)
				hist[i] += ts[c].hist[i];
			hn += ts[c].hist_n;
		}
	}
	secs = (double)(t_end - t_start) / 1e6;
	if (failed || errs) {
		fprintf(stderr, "RUN FAILED: conn-failure=%d errors=%llu\n",
			failed, errs);
		return 1;
	}

	if (o_depth == 1 && hn) {
		printf("%s d=%d c=%d M=%d: %.0f ops/s  p50=%lldus p99=%lldus "
			"p999=%lldus (n=%llu)\n", o_proto, o_depth, o_conns,
			o_getpct, (double)ops / secs, pct(hist, hn, 0.50),
			pct(hist, hn, 0.99), pct(hist, hn, 0.999), hn);
	} else {
		printf("%s d=%d c=%d M=%d: %.0f ops/s  (%.1f MB/s out)\n",
			o_proto, o_depth, o_conns, o_getpct,
			(double)ops / secs, (double)mb / secs / 1e6);
	}
	(void)o_quiet;
	return 0;
}
