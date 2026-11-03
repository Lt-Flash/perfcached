/* histtest.c - S183: a percentile must estimate the VALUE, not name a
 * power of two above it.
 *
 * The histogram was one bucket per octave and a percentile answered
 * with that bucket's upper bound, so /stats and the status page showed
 * 16, 32, 64, 128, 512 us and nothing else.  The operator's words: "if
 * the value is 262 us then obviously it would be less than 512 but more
 * than 256" - which is not a measurement, it is arithmetic the reader
 * could do without us.
 *
 * Eight buckets per octave and interpolation inside the bucket: this
 * feeds known distributions through the real recorder and holds the
 * answers to an error bound.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/json.h"
#include "../src/obs.h"

int pc_obs_init(int nworkers, long long slowlog_usec);
void pc_obs_cmd(const char *name, size_t nlen, unsigned long long usec);
int pc_obs_cmd_merge(struct pc_obs_cmdsum *out, int cap);

/* the percentiles are only reachable the way the page reads them: out
 * of the JSON the HTTP door renders from the stats tree (S317).  That
 * is the point - this asserts what a reader sees. */
static unsigned long long field(const char *name, const char *key)
{
	static char buf[1 << 20];
	static unsigned char tbuf[1 << 20];
	struct pc_tw tw;
	struct pc_jw w;
	char pat[64], *p;
	unsigned long long v = 0;

	pc_tw_init(&tw, tbuf, sizeof tbuf);
	pc_obs_commands_tree(&tw);
	pc_jw_init(&w, buf, sizeof buf);
	if (tw.over || pc_tree_json(&w, tw.b, tw.n) == 0)
		return 0;
	buf[w.len < sizeof buf ? w.len : sizeof buf - 1] = 0;
	snprintf(pat, sizeof pat, "\"name\":\"%s\"", name);
	p = strstr(buf, pat);
	if (!p)
		return 0;
	snprintf(pat, sizeof pat, "\"%s\":", key);
	p = strstr(p, pat);
	if (!p)
		return 0;
	sscanf(p + strlen(pat), "%llu", &v);
	return v;
}

/* obs.c reaches for the worker id; a histogram test has one worker.
 * (The unknown-command table it also reaches for is linked in as the
 * real src/clunk.o - obs.h now carries its prototypes.) */
int pc_worker_id(void) { return 0; }

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static struct pc_obs_cmdsum *row(const char *name)
{
	static struct pc_obs_cmdsum out[128];
	int n = pc_obs_cmd_merge(out, 128), i;

	for (i = 0; i < n; i++)
		if (out[i].nlen == strlen(name) &&
		        !memcmp(out[i].name, name, out[i].nlen))
			return &out[i];
	return NULL;
}

/* |a - b| as a percentage of b */
static double err_pct(unsigned long long a, unsigned long long b)
{
	double d = (double)a - (double)b;

	return (d < 0 ? -d : d) * 100.0 / (double)b;
}

int main(void)
{
	struct pc_obs_cmdsum *s;
	int i;

	if (pc_obs_init(1, -1) != 0) {
		printf("obs init failed\n");
		return 2;
	}

	/* 1. every call the same 262 us - the operator's own example */
	for (i = 0; i < 10000; i++)
		pc_obs_cmd("flat", 4, 262);
	s = row("flat");
	if (!s) {
		printf("no row\n");
		return 2;
	}
	CHK(s->calls == 10000, "10,000 calls recorded (%llu)", s->calls);
	{
		unsigned long long p50 = field("flat", "p50_us");
		unsigned long long p99 = field("flat", "p99_us");
		unsigned long long mx = field("flat", "max_us");

		CHK(mx == 262, "the slowest call is EXACT: %llu us", mx);
		CHK(err_pct(p50, 262) <= 12.5,
			"p50 of a flat 262 us reads %llu (%.1f%% off, bucket is 12.5%% wide)",
			p50, err_pct(p50, 262));
		CHK(err_pct(p99, 262) <= 12.5,
			"p99 reads %llu (%.1f%% off)", p99, err_pct(p99, 262));
		/* the complaint in one assertion: 262 us must not report as
		 * 512, and 256 and 512 are the only powers of two it could
		 * have collapsed to */
		CHK(p50 != 512 && p50 != 256,
			"and it is not a power of two (%llu)", p50);
	}

	/* 2. a spread: 200..299 us, ten calls each.  The median is 249-250
	 * and p99 is ~298 - both must land inside the bucket width, and
	 * NEITHER may be a power of two, which is the whole complaint. */
	for (i = 200; i < 300; i++) {
		int j;

		for (j = 0; j < 10; j++)
			pc_obs_cmd("spread", 6, (unsigned long long)i);
	}
	s = row("spread");
	CHK(s && s->calls == 1000, "1,000 calls across 200..299 us");
	CHK(s && s->max == 299, "max is exactly the slowest: %llu", s ? s->max : 0);
	{
		unsigned long long p50 = field("spread", "p50_us");
		unsigned long long p99 = field("spread", "p99_us");

		CHK(err_pct(p50, 249) <= 12.5,
			"p50 of a flat spread 200..299 reads %llu, true 249 (%.1f%% off)",
			p50, err_pct(p50, 249));
		CHK(err_pct(p99, 298) <= 12.5,
			"p99 reads %llu, true 298 (%.1f%% off)", p99, err_pct(p99, 298));
		CHK(p99 <= s->max,
			"and no percentile exceeds the slowest call seen (%llu <= %llu)",
			p99, s->max);
		CHK(p50 < p99, "and p50 < p99, which one bucket per octave could not show");
	}

	/* 3. the fast path: a 2 us call must read 2 us, not "<= 2" from a
	 * bucket that also holds 1 - below PC_OBS_SUB every value has its
	 * own slot precisely so a GET is not rounded */
	for (i = 0; i < 1000; i++)
		pc_obs_cmd("fast", 4, 2);
	{
		unsigned long long p50 = field("fast", "p50_us");

		CHK(p50 == 2, "a 2 us call reports p50 = %llu us, exactly", p50);
	}

	/* 4. S259: a call faster than a microsecond is recorded as 0 us, and
	 * a percentile of such calls read 0 - rc51 GitHub cmdstatstest, 40
	 * JSON gets in 26 us on a fast runner, "p50=0.000".  A measured call
	 * took AT MOST 1 us, not none: Redis 7 reports 0.001 ms there (its
	 * histogram's floor), so the shape its readers expect is >= 1 us -
	 * and the max beside it must not sit below its own percentiles. */
	for (i = 0; i < 1000; i++)
		pc_obs_cmd("sub", 3, 0);
	{
		unsigned long long p50 = field("sub", "p50_us");
		unsigned long long p999 = field("sub", "p999_us");
		unsigned long long mx = field("sub", "max_us");

		CHK(p50 == 1 && p999 == 1,
			"sub-microsecond calls report p50 %llu, p99.9 %llu us - "
			"1, Redis's floor, never 0", p50, p999);
		CHK(mx == 1, "and max_us %llu, not below its percentiles", mx);
	}

	/* S194: the five-minute window rides beside the totals.  No roll
	 * has happened in this test - a roll is a minute of wall clock -
	 * so the window IS everything, which is the honest answer for a
	 * row younger than one roll and is what the merge must report. */
	{
		unsigned long long c = field("flat", "calls");
		unsigned long long wc = field("flat", "win_calls");
		unsigned long long mx = field("flat", "max_us");
		unsigned long long wmx = field("flat", "win_max_us");

		CHK(wc == c, "the window carries every call so far (%llu of %llu)",
			wc, c);
		CHK(wmx == mx, "and the same slowest call (%llu of %llu)", wmx, mx);
		CHK(field("flat", "win_p50_us") == field("flat", "p50_us"),
			"and the same p50, from the same percentile code");
	}

	printf("histtest: %s\n", fails ? "FAILED" : "ok");
	return fails ? 1 : 0;
}
