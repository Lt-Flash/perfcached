/* clepochtest.c - the cluster wire epoch (clepoch.c; the 0.7.2 split,
 * wave 1): the cluster's state through stubbed clstate.h accessors, a
 * real state directory for node-epoch.
 *
 *   A  epoch_ok: the epochs this build reads (PC_CL_EPOCH_MIN..max) pass;
 *      a peer that sends none, or a newer one, is refused by name;
 *      raising the max (the suites' flag) admits up to it
 *   B  a peer's facts: the epoch range it says it speaks, read off its
 *      settings; a build that says nothing is "unknown"
 *   C  the commit, refused for each reason - no cluster, not the master
 *      (naming it), an epoch this node does not speak, a live member
 *      that says nothing or cannot speak it, the epoch already committed
 *      - with a dead peer and this node itself not asked; then done:
 *      committed, persisted, counted, logged
 *   D  node-epoch: loaded when it names an epoch this build speaks,
 *      refused with a log when it does not, ignored when absent or 0;
 *      adopting the master's epoch persists it too
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "compat/dprint.h"
#include "cluster.h"
#include "clpeers.h"
#include "clstate.h"
#include "clepoch.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs: clstate.h, the log, an address ------------------------- */
static struct clpeers pt;
static char dir[256];
static int enabled = 1, role = PC_ROLE_MASTER, master = 1, commits, errs, notices;
static char last_log[512];

struct clpeers *cl_peers(void) { return &pt; }
int cl_enabled(void) { return enabled; }
int cl_node_id(void) { return 1; }
int cl_role(void) { return role; }
int cl_master_id(void) { return master; }
const char *cl_state_dir(void) { return dir; }
long long cl_now_ms(void) { return 100000; }
void cl_st_epoch_commits(void) { commits++; }
const char *pc_addr_str(const pc_addr *a) { (void)a; return "10.0.0.9:6480"; }
int compat_log_level = L_NOTICE;
/* CH-17: every line said - this test reads the refusals themselves */
int pc_logrl_ok(int reason, const pc_addr *src) { (void)reason; (void)src; return 1; }

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	va_list ap;

	(void)tag;
	va_start(ap, fmt);
	vsnprintf(last_log, sizeof last_log, fmt, ap);
	va_end(ap);
	if (level == L_ERR)
		errs++;
	else if (level == L_NOTICE)
		notices++;
}

/* ---- the harness ------------------------------------------------------- */
static int file_says(const char *want)
{
	char path[400], got[32] = "";
	FILE *f;

	snprintf(path, sizeof path, "%s/node-epoch", dir);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (!fgets(got, sizeof got, f))
		got[0] = 0;
	fclose(f);
	return !strcmp(got, want);
}

static void file_write(const char *s)
{
	char path[400];
	FILE *f;

	snprintf(path, sizeof path, "%s/node-epoch", dir);
	f = fopen(path, "w");
	if (f) {
		fputs(s, f);
		fclose(f);
	}
}

/* peer @i: node @node, live or not, saying @lo..@hi (lo < 0: nothing) */
static void peer(int i, int node, int live, int lo, int hi)
{
	struct peer *p = &pt.peers[i];

	memset(p, 0, sizeof *p);
	p->node = node;
	p->last_seen_ms = live ? 100000 : 100000 - PEER_UP_MS;
	p->ncfg = 0;
	if (lo >= 0) {
		p->cfg_name[0] = 0x12345u;              /* another fact first */
		p->cfg_val[0] = 7;
		p->cfg_name[1] = PC_FACT_EPOCHS;
		p->cfg_val[1] = (uint32_t)lo << 8 | (uint32_t)hi;
		p->ncfg = 2;
	}
	if (i >= pt.n_peers)
		pt.n_peers = i + 1;
}

int main(void)
{
	pc_addr from;
	char why[200];
	int lo, hi, e = PC_CL_EPOCH;
	uint32_t v;

	memset(&from, 0, sizeof from);
	snprintf(dir, sizeof dir, "/tmp/clepochtest.XXXXXX");
	if (!mkdtemp(dir))
		return 2;

	/* A */
	CHK(epoch_ok(PC_CL_EPOCH_MIN, &from, "join") && epoch_ok(PC_CL_EPOCH, &from, "join"),
		"A: the epochs this build reads (%d..%d) pass", PC_CL_EPOCH_MIN, PC_CL_EPOCH_MAX);
	errs = 0;
	CHK(!epoch_ok(0, &from, "keepalive") && errs == 1 &&
		strstr(last_log, "REFUSING keepalive from 10.0.0.9:6480"),
		"A: a peer that sends no epoch - refused by name");
	CHK(!epoch_ok(PC_CL_EPOCH_MAX + 1, &from, "join"), "A: a newer epoch - refused");
	epoch_set_max(PC_CL_EPOCH_MAX + 3);
	CHK(epoch_ok(PC_CL_EPOCH_MAX + 3, &from, "join") && !epoch_ok(PC_CL_EPOCH_MAX + 4, &from, "join") &&
		pc_cluster_epoch_max() == PC_CL_EPOCH_MAX + 3, "A: the max raised - read up to it, not past");

	/* B */
	peer(0, 2, 1, e, e + 2);
	CHK(peer_fact(&pt.peers[0], PC_FACT_EPOCHS, &v) && v == ((uint32_t)e << 8 | (uint32_t)(e + 2)) &&
		peer_fact(&pt.peers[0], 0x12345u, &v) && v == 7 && !peer_fact(&pt.peers[0], 0x99u, &v),
		"B: a peer's facts - found by key, the absent one not");
	CHK(peer_speaks(&pt.peers[0], e + 1, &lo, &hi) == 1 && lo == e && hi == e + 2,
		"B: speaks %d..%d - %d yes", e, e + 2, e + 1);
	CHK(peer_speaks(&pt.peers[0], e + 3, &lo, &hi) == 0, "B: %d no", e + 3);
	peer(1, 3, 1, -1, 0);
	CHK(peer_speaks(&pt.peers[1], e, &lo, &hi) == -1, "B: a build that says nothing - unknown");

	/* C */
	pt.n_peers = 0;
	peer(0, 2, 1, e, e + 3);           /* speaks it */
	peer(1, 4, 0, -1, 0);              /* dead: not asked */
	peer(2, 1, 1, -1, 0);              /* this node: not asked */
	enabled = 0;
	CHK(pc_cluster_epoch_commit(e + 1, why, sizeof why) == -1 && !strcmp(why, "no cluster here"),
		"C: no cluster - refused");
	enabled = 1;
	role = PC_ROLE_MASTER - 1;
	master = 7;
	CHK(pc_cluster_epoch_commit(e + 1, why, sizeof why) == -1 &&
		!strcmp(why, "not the master - ask node 7"), "C: not the master - refused, naming it");
	role = PC_ROLE_MASTER;
	CHK(pc_cluster_epoch_commit(PC_CL_EPOCH_MAX + 4, why, sizeof why) == -1 && strstr(why, "not "),
		"C: an epoch this node does not speak - refused (%s)", why);
	peer(3, 5, 1, -1, 0);              /* live, says nothing */
	CHK(pc_cluster_epoch_commit(e + 1, why, sizeof why) == -1 &&
		strstr(why, "node 5 says nothing"), "C: a live member that says nothing - refused (%s)", why);
	peer(3, 5, 1, e, e);               /* live, speaks only the current */
	CHK(pc_cluster_epoch_commit(e + 1, why, sizeof why) == -1 &&
		strstr(why, "node 5 speaks wire epochs"), "C: a live member that cannot speak it - refused (%s)", why);
	pt.n_peers = 3;
	CHK(pc_cluster_epoch_commit(e, why, sizeof why) == -1 && strstr(why, "already"),
		"C: the epoch already committed - refused");
	commits = notices = 0;
	CHK(pc_cluster_epoch_commit(e + 1, why, sizeof why) == 0 && pc_cluster_epoch() == e + 1 &&
		commits == 1 && notices == 1, "C: every live member speaks %d - committed, counted, logged", e + 1);
	CHK(file_says("3\n") || (e + 1 != 3 && file_says("4\n")), "C: and persisted to node-epoch");

	/* D */
	epoch_adopt(e + 2);
	{
		char want[16];

		snprintf(want, sizeof want, "%d\n", e + 2);
		CHK(pc_cluster_epoch() == e + 2 && file_says(want), "D: adopting the master's epoch persists it");
	}
	file_write("2\n");
	epoch_load();
	CHK(pc_cluster_epoch() == 2, "D: node-epoch 2 - loaded");
	errs = 0;
	file_write("200\n");
	epoch_load();
	CHK(pc_cluster_epoch() == 2 && errs == 1 && strstr(last_log, "does not speak"),
		"D: node-epoch 200 - this build does not speak it: logged, unchanged");
	errs = 0;
	file_write("0\n");
	epoch_load();
	CHK(pc_cluster_epoch() == 2 && errs == 0, "D: node-epoch 0 - ignored, quietly");
	{
		char path[400];

		snprintf(path, sizeof path, "%s/node-epoch", dir);
		unlink(path);
		epoch_load();
		CHK(pc_cluster_epoch() == 2, "D: no node-epoch - unchanged");
		rmdir(dir);
		dir[0] = 0;
		epoch_adopt(3);
		CHK(pc_cluster_epoch() == 3, "D: no state dir - adopted, nothing written");
	}

	printf("clepochtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
