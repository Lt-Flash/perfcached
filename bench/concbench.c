/*
 * concbench.c — N CONCURRENT libperfd connections to one cluster.
 *
 * The mode matrix drives a single connection one operation at a time,
 * which is what an application's latency looks like but not what a
 * fleet of them costs.  pcbench's RTT arms use four raw connections and
 * report roughly four times as much.  This closes the gap honestly: N
 * real client handles, each on its own thread, against the same
 * cluster, so per-connection cost and aggregate throughput can be read
 * off the same run.
 *
 * One handle per thread, never shared: libperfd owns a socket, a cipher
 * state and a reply queue per handle, and nothing about that is
 * thread-safe by design.
 *
 * usage: concbench <host> <port> <secret> <col> <threads> <ops-each>
 *                  <getpct> <route 0|1> <binary 0|1>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "../lib/perfd.h"

#define MAXT 64
#define HIST 200000

struct arg {
	const char *host, *secret, *col;
	int port, ops, getpct, route, binary, id;
	unsigned int *hist;
	long done, missed;
	int err, fill_done;
	double mix_start, mix_end;     /* the MEASURED window: timing the
	                                * whole thread would charge the
	                                * unmeasured fill against
	                                * throughput and halve it */
};

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *worker(void *p)
{
	struct arg *a = p;
	/* "-" = connect PLAINTEXT, so the Noise channel's cost can be
	 * separated from the library's own */
	const char *sec[2] = { a->secret, NULL };
	perfd_opts o;
	perfd_t *h;
	char val[200];
	int i;

	memset(val, 'c', sizeof val);
	memset(&o, 0, sizeof o);
	o.secrets = strcmp(a->secret, "-") ? sec : NULL;
	o.spares = -1;
	o.route_keys = a->route;
	o.binary = a->binary;
	h = perfd_connect(a->host, a->port, &o);
	if (!h) {
		a->err = 1;
		return NULL;
	}
	/* FILL FIRST, unmeasured.  Without it the mix reads keys that were
	 * never written: every GET is a fleet-wide miss with a pull behind
	 * it, and the run measures a miss storm rather than the workload
	 * (measured: 18000 of 20000 reads missing, throughput less than
	 * half). */
	for (i = 0; i < a->ops; i++) {
		char k[40];

		snprintf(k, sizeof k, "cb%02d%08d", a->id, i);
		if (perfd_set(h, a->col, k, val, sizeof val, 0) != 0) {
			a->err = 1;
			perfd_free(h);
			return NULL;
		}
	}
	a->fill_done = 1;

	a->mix_start = now_s();
	for (i = 0; i < a->ops; i++) {
		char k[40];
		double t0 = now_s();
		long us;

		/* stride the keyspace so reads are not sequential */
		snprintf(k, sizeof k, "cb%02d%08d", a->id,
			(i * 7919) % a->ops);
		if (i % 100 < a->getpct) {
			char *v = NULL;
			size_t vl = 0;
			long long ttl = 0;
			int rc = perfd_get(h, a->col, k, (void **)&v, &vl,
				&ttl);

			if (rc < 0) {
				a->err = 1;
				break;
			}
			if (rc == 0)
				a->missed++;
			free(v);
		} else if (perfd_set(h, a->col, k, val, sizeof val, 0) != 0) {
			a->err = 1;
			break;
		}
		us = (long)((now_s() - t0) * 1e6);
		if (us < 0)
			us = 0;
		if (us >= HIST)
			us = HIST - 1;
		a->hist[us]++;
		a->done++;
	}
	a->mix_end = now_s();
	perfd_free(h);
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t th[MAXT];
	struct arg a[MAXT];
	unsigned int *hist;
	unsigned long total = 0, seen = 0;
	long done = 0, missed = 0;
	int nthreads, i, j, err = 0;
	long p50 = 0, p99 = 0;
	double t0, dt;

	if (argc < 10) {
		fprintf(stderr, "usage: %s host port secret col threads "
			"ops getpct route binary\n", argv[0]);
		return 2;
	}
	nthreads = atoi(argv[5]);
	if (nthreads > MAXT)
		nthreads = MAXT;
	hist = calloc((size_t)nthreads * HIST, sizeof *hist);
	if (!hist)
		return 2;

	for (i = 0; i < nthreads; i++) {
		memset(&a[i], 0, sizeof a[i]);
		a[i].host = argv[1];
		a[i].port = atoi(argv[2]);
		a[i].secret = argv[3];
		a[i].col = argv[4];
		a[i].ops = atoi(argv[6]);
		a[i].getpct = atoi(argv[7]);
		a[i].route = atoi(argv[8]);
		a[i].binary = atoi(argv[9]);
		a[i].id = i;
		a[i].hist = hist + (size_t)i * HIST;
	}
	t0 = now_s();
	for (i = 0; i < nthreads; i++)
		pthread_create(&th[i], NULL, worker, &a[i]);
	for (i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);
	(void)t0;
	/* the aggregate window is the union of the threads' MIX phases,
	 * not the process lifetime */
	{
		double first = 0, last = 0;

		for (i = 0; i < nthreads; i++) {
			if (!a[i].mix_start)
				continue;
			if (!first || a[i].mix_start < first)
				first = a[i].mix_start;
			if (a[i].mix_end > last)
				last = a[i].mix_end;
		}
		dt = last > first ? last - first : 1e-9;
	}

	for (i = 0; i < nthreads; i++) {
		done += a[i].done;
		missed += a[i].missed;
		err += a[i].err;
	}
	/* merge the per-thread histograms for a fleet-wide percentile */
	for (j = 0; j < HIST; j++) {
		unsigned long c = 0;

		for (i = 0; i < nthreads; i++)
			c += a[i].hist[j];
		total += c;
	}
	for (j = 0; j < HIST && total; j++) {
		unsigned long c = 0;

		for (i = 0; i < nthreads; i++)
			c += a[i].hist[j];
		seen += c;
		if (!p50 && seen >= total / 2)
			p50 = j;
		if (!p99 && seen >= (unsigned long)(total * 0.99)) {
			p99 = j;
			break;
		}
	}
	printf("threads=%d %s route=%s binary=%s ops_per_s=%.0f "
		"per_conn=%.0f p50=%ldus p99=%ldus missed=%ld errors=%d\n",
		nthreads, strcmp(argv[3], "-") ? "noise" : "plain",
		argv[8][0] == '1' ? "on" : "off",
		argv[9][0] == '1' ? "on" : "off",
		done / dt, done / dt / nthreads, p50, p99, missed, err);
	free(hist);
	return err ? 1 : 0;
}
