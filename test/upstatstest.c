/* upstatstest.c - the read-through's figures (upstats.c; the 0.7.2 split,
 * wave 1): the collection table, its two upstreams and the helpers
 * upstream.c owns are stubbed here, and the tree is rendered as JSON.
 *
 *   A  an unknown collection writes nothing
 *   B  each upstream: its address, role, state and why, connections, its
 *      tracking, lag (null when not known) and shadow entries; the
 *      collection's connections and shadow entries summed over both; its
 *      tracking off while one in use is untracked, on once that one is
 *      not used
 *   C  the counters as /stats shows them (S358: since the last reset) and
 *      the average fetch; the write-through block - its target, the queue
 *      (queued plus the writer's batch), the broadcast and upfill states,
 *      a row per command, the last errors newest first; with no target,
 *      the state's name and why
 *   D  pc_upstream_stats_reset: every counter reads 0 and then counts from
 *      there, the per-command rows too, the error ring empties; gauges
 *      (connections, shadow entries), the per-upstream shares and the
 *      drain run's figures are left as they were
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "json.h"
#include "ptree.h"
#include "upstream.h"
#include "upstreamint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- what upstream.c owns, stubbed ------------------------------------- */
struct up_col up_cols[UP_MAX_COLS];
const char *const upws_name[] = { "ok", "starting", "no master",
	"two masters, no replica names one", "the master is not answering" };
const char *const updr_name[] = { "idle", "running", "done",
	"stopped", "failed" };
static int sstate[2] = { PC_UPS_SERVE, PC_UPS_UNKNOWN };

struct up_col *col_of(const char *col, size_t collen)
{
	struct up_col *uc = &up_cols[0];

	return uc->nlen == collen && !memcmp(uc->name, col, collen) ? uc : NULL;
}

int srv_state(const struct up_srv *s) { return sstate[s->idx]; }
const char *srv_why(const struct up_srv *s) { return s->idx ? "not reached" : ""; }
int bcast_state(const struct up_col *uc) { (void)uc; return 1; }
unsigned long pcache_ht_entries(pcache_htable_t *ht) { (void)ht; return 21; }

/* ---- the harness ------------------------------------------------------- */
static char js[1 << 16];

/* the collection's block as JSON, in an object of its own */
static void render(const char *col)
{
	static unsigned char tb[1 << 16];
	struct pc_tw t;
	struct pc_jw w;

	pc_tw_init(&t, tb, sizeof tb);
	pc_tw_map(&t);
	pc_upstream_stats_tree(&t, col, strlen(col));
	pc_tw_end(&t);
	pc_jw_init(&w, js, sizeof js);
	if (t.over || pc_tree_json(&w, t.b, t.n) == 0 || w.overflow) {
		strcpy(js, "(render failed)");
		return;
	}
	js[w.len] = 0;
}

static int has(const char *s) { return strstr(js, s) != NULL; }

static void setup(void)
{
	struct up_col *uc = &up_cols[0];
	int i;

	memcpy(uc->name, "c", 2);
	uc->nlen = 1;
	pthread_mutex_init(&uc->mx, NULL);
	uc->nsrv = 2;
	for (i = 0; i < 2; i++) {
		struct up_srv *s = &uc->srv[i];

		s->uc = uc;
		s->idx = i;
		pthread_mutex_init(&s->run_mx, NULL);
		snprintf(s->host, sizeof s->host, "10.0.0.%d", i + 1);
		s->port = 6379;
		snprintf(s->addr, sizeof s->addr, "10.0.0.%d:6379", i + 1);
		s->role = i ? UPR_REPLICA : UPR_MASTER;
		s->conns_up = i ? 3 : 2;
		s->inv_id = i ? 0 : 7;
		s->lag_ms = i ? 15 : -1;
		s->fetches = i ? 4 : 6;
	}
	memcpy(uc->srv[0].inv_run, "abc", 4);
	uc->held = 7;
	uc->joined = 3;
	uc->fetches = 10;
	uc->read_lost = 2;
	uc->invalidations = 5;
	uc->flushes = 1;
	uc->lat_us_sum = 1000;
	uc->lat_n = 4;
	uc->dr_scanned = 9;
	uc->drain_state = UPDR_RUNNING;
	/* write-through */
	uc->through = 1;
	uc->w_master = 0;
	uc->w_state = UPWS_OK;
	uc->w_json = 1;
	uc->wq_n = 2;
	uc->w_inflight = 1;
	uc->wt_owned = -1;
	uc->upf_state = UPDR_IDLE;
	uc->wt_fwd = 4;
	uc->nwcmd = 1;
	memcpy(uc->wcmd[0].name, "set", 4);
	uc->wcmd[0].c[WC_OK] = 5;
	for (i = 0; i < 3; i++) {
		uc->werr[i].at = 1000 + i;
		snprintf(uc->werr[i].msg, sizeof uc->werr[i].msg, "e%d", i);
	}
	uc->werr_n = 3;
}

int main(void)
{
	struct up_col *uc = &up_cols[0];

	setup();

	/* A */
	render("nope");
	CHK(!strcmp(js, "{}"), "A: an unknown collection writes nothing (%s)", js);

	/* B */
	render("c");
	CHK(has("{\"addr\":\"10.0.0.1:6379\",\"role\":\"master\",\"state\":\"serving\",\"why\":\"\","
		"\"connections_up\":2,"), "B: the first upstream - master, serving");
	CHK(has("{\"addr\":\"10.0.0.2:6379\",\"role\":\"replica\",\"state\":\"not reached yet\","
		"\"why\":\"not reached\",\"connections_up\":3,"), "B: the second - replica, not reached, why");
	CHK(has("\"tracking\":true,\"fetches\":6,") && has("\"tracking\":false,\"fetches\":4,"),
		"B: each one's tracking and its share of the fetches");
	CHK(has("\"lag_ms\":null,\"shadow_entries\":21,") && has("\"lag_ms\":15,\"shadow_entries\":21,"),
		"B: lag null when not known, a replica's in ms; each shadow's entries");
	CHK(has("\"connections_up\":5,\"connects\":"), "B: the collection's connections - 2 + 3");
	CHK(has("\"shadow_entries\":42,\"fetch_us_avg\":"), "B: its shadow entries - 21 + 21");
	CHK(has("\"tracking\":false,\"server\":\"abc\""),
		"B: tracking off while an upstream in use is untracked");
	sstate[1] = PC_UPS_OFF;
	render("c");
	CHK(has("\"tracking\":true,\"server\":\"abc\""), "B: on once that one is not used");
	sstate[1] = PC_UPS_UNKNOWN;

	/* C */
	render("c");
	CHK(has("\"held\":7,\"joined\":3,\"fetches\":10,\"win_fetches\":"), "C: held, joined, fetches");
	CHK(has("\"read_lost\":2,\"win_read_lost\":"), "C: read_lost (S435)");
	CHK(has("\"invalidations\":5,\"flushes\":1,\"untracked_fills\":0"), "C: invalidations, flushes");
	CHK(has("\"fetch_us_avg\":250,"), "C: the average fetch - 1000 us over 4");
	CHK(has("\"write_through\":{\"queued\":3,\"target\":\"10.0.0.1:6379\",\"state\":\"ok\","
		"\"why\":\"\",\"json\":\"yes\","), "C: the writer - queued 2 + 1 held, its target, RedisJSON");
	CHK(has("\"bcast\":\"on\",\"fleet_owned\":null,") && has("\"upfill\":{\"state\":\"idle\","),
		"C: broadcast tracking, fleet_owned not counted yet, the upfill idle");
	CHK(has("\"forwarded\":4,\"win_forwarded\":"), "C: forwarded");
	CHK(has("{\"name\":\"set\",\"passed\":false,\"applied\":5,\"win_applied\":"),
		"C: a row per command");
	{
		const char *e2 = strstr(js, "\"message\":\"e2\""), *e1 = strstr(js, "\"message\":\"e1\""),
			*e0 = strstr(js, "\"message\":\"e0\"");

		CHK(e2 && e1 && e0 && e2 < e1 && e1 < e0, "C: the last errors, newest first");
	}
	uc->w_master = -1;
	uc->w_state = UPWS_DOWN;
	memcpy(uc->w_why, "lost it", 8);
	render("c");
	CHK(has("\"target\":null,\"state\":\"the master is not answering\",\"why\":\"lost it\""),
		"C: no target - the state's name and why");
	uc->w_master = 0;
	uc->w_state = UPWS_OK;

	/* D */
	pc_upstream_stats_reset();
	render("c");
	CHK(has("\"held\":0,\"joined\":0,\"fetches\":0,\"win_fetches\":"), "D: the counters read 0");
	CHK(has("\"invalidations\":0,\"flushes\":0,") && has("\"read_lost\":0,") &&
		has("\"forwarded\":0,") && has("\"fetch_us_avg\":0,"),
		"D: the rest too, and the average with nothing to average");
	CHK(has("{\"name\":\"set\",\"passed\":false,\"applied\":0,") && has("\"last_errors\":[]"),
		"D: the per-command rows, and the error ring empties");
	CHK(has("\"connections_up\":5,\"connects\":") && has("\"shadow_entries\":42,"),
		"D: the gauges as they were");
	CHK(has("\"tracking\":true,\"fetches\":6,") && has("\"read\":9,"),
		"D: the per-upstream shares and the drain run's figures are not reset");
	uc->fetches = 13;
	uc->lat_us_sum = 1600;
	uc->lat_n = 6;
	uc->wcmd[0].c[WC_OK] = 7;
	render("c");
	CHK(has("\"joined\":0,\"fetches\":3,") && has("\"fetch_us_avg\":300,") &&
		has("{\"name\":\"set\",\"passed\":false,\"applied\":2,"),
		"D: then they count from the reset - 3 fetches, 600 us over 2, 2 applied");

	printf("upstatstest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
