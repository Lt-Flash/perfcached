/* perfcli-upgrade.inc - UP7a (DESIGN 12ns): the roll driver.
 *
 *   perfcli -E <enable> -h <seed> upgrade plan <tarball> version=<v>
 *           [switch_cmd=<program>] [journal=<file>] [ready_s=<n>] [gate_s=<n>]
 *   perfcli ... upgrade start | resume | status | abort | rollback [journal=<file>]
 *
 * A stateless program with a journal: the roll's truth is the fleet, read
 * before every transition (which build each node runs, whether it is
 * ready, whether its peers kept their records); the journal says what was
 * intended and what happened.  Per node: staged -> switching -> up ->
 * ready -> gated.  At most one node past staged at a time; a node is never
 * skipped; the order (members by id, the backup, the master last) is
 * recomputed before each node; the driver stops at the first gate that
 * fails, reverts THAT node and prints the rollback command.
 *
 * switch_cmd replaces the upgrade_switch / upgrade_revert verbs with a
 * program (the CI roll's hook, 12ns "testability without root"):
 *   <program> switch <addr> <port> <node> <artifact> <sha256>
 *   <program> revert <addr> <port> <node>
 */
#include <stdarg.h>
#include <sys/wait.h>
#include <time.h>

#define UP_MAX_NODES 64

struct unode {
	int node, port;
	char addr[64];
	char ver[32], rev[32], state[24], role[16];
	long long pid, entries;
	int peers_up, safe, reach;
};

struct uplan {
	char artifact[512], sha[65], version[48], switch_cmd[512], journal[512];
	char seed[128];
	int ready_s, gate_s;
};

static int ujournal_line(const struct uplan *pl, const char *fmt, ...)
{
	FILE *f = fopen(pl->journal, "a");
	va_list ap;
	char line[1024];

	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if (!g_quiet)
		printf("%s\n", line);
	if (!f)
		return -1;
	fprintf(f, "%lld %s\n", (long long)time(NULL), line);
	fclose(f);
	return 0;
}

static long long jnumv(const char *res, const char *key)
{
	char pat[64];
	const char *s;

	snprintf(pat, sizeof pat, "\"%s\":", key);
	s = res ? strstr(res, pat) : NULL;
	return s ? strtoll(s + strlen(pat), NULL, 10) : -1;
}

/* the value of @key among the TOP-LEVEL keys of the JSON object that
 * starts at @obj ('{'): nested objects, arrays and strings are skipped, so
 * cluster.lane.peers_up is not taken for cluster.peers_up.  NULL: absent. */
static const char *jtop(const char *obj, const char *key)
{
	char pat[64];
	size_t pl;
	const char *p;
	int depth = 0;

	if (!obj || *obj != '{')
		return NULL;
	pl = (size_t)snprintf(pat, sizeof pat, "\"%s\":", key);
	for (p = obj; *p; p++) {
		if (*p == '"') {
			if (depth == 1 && !strncmp(p, pat, pl))
				return p + pl;
			for (p++; *p && *p != '"'; p++)   /* over the string */
				if (*p == '\\' && p[1])
					p++;
			if (!*p)
				return NULL;
		} else if (*p == '{' || *p == '[') {
			depth++;
		} else if (*p == '}' || *p == ']') {
			if (--depth == 0)
				return NULL;
		}
	}
	return NULL;
}

static const char *jobj(const char *res, const char *key)   /* "key":{ ... */
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof pat, "\"%s\":", key);
	p = res ? strstr(res, pat) : NULL;
	if (!p)
		return NULL;
	p += strlen(pat);
	while (*p == ' ')
		p++;
	return *p == '{' ? p : NULL;
}

/* the records a node holds: the top-level "entries" of each object in its
 * stats' "collections" array, summed - the same measure before and after a
 * peer's restart, so a fall says records went missing.  Every "entries" in
 * the text used to be summed, and that mixed in the fleet's total, the
 * node's total, the negative cache's and the shard map's: in shard mode a
 * peer "lost" 6039 -> 4999 across a restart while every key read back
 * (upgradeloadtest, 225, 2026-10-16) - its negative cache had emptied. */
static long long jentries_sum(const char *res)
{
	const char *p = res ? strstr(res, "\"collections\":[") : NULL;
	long long sum = 0;
	int depth = 0;

	if (!p)
		return 0;
	for (p += 14; *p; p++) {
		if (*p == '"') {
			for (p++; *p && *p != '"'; p++)
				if (*p == '\\' && p[1])
					p++;
			if (!*p)
				break;
		} else if (*p == '{') {
			if (depth == 0) {          /* one collection's object */
				const char *e = jtop(p, "entries");

				if (e)
					sum += strtoll(e, NULL, 10);
			}
			depth++;
		} else if (*p == '[') {
			depth++;
		} else if (*p == '}' || *p == ']') {
			if (depth == 0)
				break;                 /* the array's own ']' */
			depth--;
		}
	}
	return sum;
}

static perfd_t *udial(const char *addr, int port)
{
	perfd_t *h = g_dial_opts ? perfd_connect(addr, port, g_dial_opts) : NULL;

	if (h && g_enable_secret && perfd_enable(h, g_enable_secret) != 0) {
		perfd_free(h);
		return NULL;
	}
	return h;
}

/* one node's facts from its own /stats, over a fresh dial */
static int unode_read(struct unode *n)
{
	perfd_t *h = udial(n->addr, n->port);
	char *r;

	n->reach = 0;
	if (!h)
		return -1;
	r = perfd_stats(h, NULL);
	if (!r) {
		perfd_free(h);
		return -1;
	}
	{
		/* the cluster's own figures live in its block: a first-match scan of
		 * the whole text found an upstream's "role":"replica" and another
		 * block's "peers_up":0 (225, 2026-10-16: the ready gate never passed) */
		const char *cl = jobj(r, "cluster"), *v, *rs;

		jfield(r, "version", n->ver, sizeof n->ver);
		jfield(r, "rev", n->rev, sizeof n->rev);
		jfield(r, "state", n->state, sizeof n->state);      /* top level, first */
		n->pid = jnumv(r, "pid");                           /* process.pid, first */
		if (cl) {
			v = jtop(cl, "role");
			n->role[0] = 0;
			if (v && *v == '"') {
				size_t l = strcspn(v + 1, "\"");

				snprintf(n->role, sizeof n->role, "%.*s", (int)(l < 15 ? l : 15), v + 1);
			}
			v = jtop(cl, "peers_up");
			n->peers_up = v ? (int)strtoll(v, NULL, 10) : 0;
			rs = jtop(cl, "restart");
			while (rs && *rs == ' ')
				rs++;
			v = rs && *rs == '{' ? jtop(rs, "safe") : NULL;
			n->safe = v && !strncmp(v, "true", 4);
		} else {
			n->role[0] = 0;
			n->peers_up = 0;
			n->safe = 1;                                   /* standalone: nothing to wait for */
		}
	}
	n->entries = jentries_sum(r);
	n->reach = 1;
	perfd_free_value(r);
	perfd_free(h);
	return 0;
}

/* the fleet as the seed handle learned it, every member read */
static int ufleet_read(struct unode *ns, int max)
{
	int c = perfd_member_count(g_p), i, n = 0;

	if (c <= 0) {                       /* a fleet of one: the seed itself */
		struct unode *u = &ns[0];

		memset(u, 0, sizeof *u);
		snprintf(u->addr, sizeof u->addr, "%s", g_dial_host ? g_dial_host : "127.0.0.1");
		u->port = g_dial_port ? atoi(g_dial_port) : 6479;
		u->node = perfd_active_node(g_p);
		unode_read(u);
		return 1;
	}
	for (i = 0; i < c && n < max; i++) {
		struct unode *u = &ns[n];

		memset(u, 0, sizeof *u);
		if (perfd_member_info(g_p, i, u->addr, sizeof u->addr, &u->port,
		        &u->node, NULL) != 0)
			continue;
		unode_read(u);
		n++;
	}
	return n;
}

static int urole_rank(const struct unode *u)
{
	return !strcmp(u->role, "master") ? 2 : !strcmp(u->role, "backup") ? 1 : 0;
}

static int unode_cmp(const void *a, const void *b)
{
	const struct unode *x = a, *y = b;
	int rx = urole_rank(x), ry = urole_rank(y);

	return rx != ry ? rx - ry : x->node - y->node;
}

static struct unode *ufind(struct unode *ns, int n, int node)
{
	int i;

	for (i = 0; i < n; i++)
		if (ns[i].node == node)
			return &ns[i];
	return NULL;
}

/* ---- the journal: the plan and what has happened to each node ---------- */

static int ujournal_plan(struct uplan *pl)
{
	FILE *f = fopen(pl->journal, "r");
	char line[1200];
	int found = 0;

	if (!f)
		return -1;
	while (fgets(line, sizeof line, f)) {
		char *p = strchr(line, ' ');

		if (!p || strncmp(p + 1, "plan ", 5))
			continue;
		p += 6;
		line[strcspn(line, "\r\n")] = 0;
		sscanf(p, "artifact=%511s sha256=%64s version=%47s ready_s=%d gate_s=%d seed=%127s",
			pl->artifact, pl->sha, pl->version, &pl->ready_s, &pl->gate_s, pl->seed);
		pl->switch_cmd[0] = 0;
		{
			char *s = strstr(p, "switch_cmd=");

			if (s)
				snprintf(pl->switch_cmd, sizeof pl->switch_cmd, "%s", s + 11);
		}
		found = 1;
	}
	fclose(f);
	return found ? 0 : -1;
}

/* the nodes the journal says reached "gated" since the last plan line, in
 * the order they did; and whether the journal's last word was "done" */
static int ujournal_gated(const struct uplan *pl, int *nodes, int max, int *done)
{
	FILE *f = fopen(pl->journal, "r");
	char line[1200];
	int n = 0;

	*done = 0;
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		char *p = strchr(line, ' ');
		int node;

		if (!p)
			continue;
		p++;
		if (!strncmp(p, "plan ", 5)) {
			n = 0;
			*done = 0;
		} else if (sscanf(p, "node=%d", &node) == 1 && strstr(p, "event=gated")) {
			if (n < max)
				nodes[n++] = node;
		} else if (sscanf(p, "node=%d", &node) == 1 && strstr(p, "event=reverted")) {
			int i, j;

			for (i = 0; i < n; i++)
				if (nodes[i] == node) {
					for (j = i; j + 1 < n; j++)
						nodes[j] = nodes[j + 1];
					n--;
					break;
				}
		} else if (!strncmp(p, "done", 4)) {
			*done = 1;
		}
	}
	fclose(f);
	return n;
}

static int uabort_wanted(const struct uplan *pl)
{
	char p[560];
	struct stat st;

	snprintf(p, sizeof p, "%s.abort", pl->journal);
	return stat(p, &st) == 0;
}

static void uabort_clear(const struct uplan *pl)
{
	char p[560];

	snprintf(p, sizeof p, "%s.abort", pl->journal);
	unlink(p);
}

static void udefault_journal(struct uplan *pl)
{
	const char *home = getenv("HOME");
	char seed[160];

	snprintf(seed, sizeof seed, "%s_%s", g_dial_host ? g_dial_host : "local",
		g_dial_port ? g_dial_port : "6479");
	snprintf(pl->journal, sizeof pl->journal, "%s/.perfcached/upgrade-%s.journal",
		home ? home : ".", seed);
}

static int uopts(struct uplan *pl, int argc, char **argv)
{
	int i;

	for (i = 0; i < argc; i++) {
		const char *a = argv[i];

		if (!strncmp(a, "journal=", 8))
			snprintf(pl->journal, sizeof pl->journal, "%s", a + 8);
		else if (!strncmp(a, "version=", 8))
			snprintf(pl->version, sizeof pl->version, "%s", a + 8);
		else if (!strncmp(a, "switch_cmd=", 11))
			snprintf(pl->switch_cmd, sizeof pl->switch_cmd, "%s", a + 11);
		else if (!strncmp(a, "ready_s=", 8))
			pl->ready_s = atoi(a + 8);
		else if (!strncmp(a, "gate_s=", 7))
			pl->gate_s = atoi(a + 7);
		else {
			fprintf(stderr, "upgrade: unknown word '%s'\n", a);
			return -1;
		}
	}
	return 0;
}

static int ufile_sha256(const char *path, char *hex, size_t cap)
{
	FILE *f = fopen(path, "rb");
	crypto_hash_sha256_state st;
	unsigned char buf[65536], h[32];
	size_t n;

	if (!f)
		return -1;
	crypto_hash_sha256_init(&st);
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		crypto_hash_sha256_update(&st, buf, n);
	fclose(f);
	crypto_hash_sha256_final(&st, h);
	sodium_bin2hex(hex, cap, h, sizeof h);
	return 0;
}

/* ---- the steps ------------------------------------------------------------ */

static int uhook(const struct uplan *pl, const char *op, const struct unode *u)
{
	char cmd[1400];
	int rc;

	snprintf(cmd, sizeof cmd, "%s %s %s %d %d '%s' %s", pl->switch_cmd, op,
		u->addr, u->port, u->node, pl->artifact, pl->sha);
	rc = system(cmd);
	return rc == -1 ? -1 : WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/* the artifact on every member: staged on the seed, relayed by the daemon,
 * verified by each with its own keys */
static int ustage(const struct uplan *pl, struct unode *ns, int n)
{
	int i, signed_ = 0, t;
	char *r;

	r = perfd_upgrade_status(g_p);
	if (!(r && strstr(r, pl->sha))) {
		perfd_free_value(r);
		r = perfd_upgrade_stage_file(g_p, pl->artifact, pl->version, NULL, NULL,
			&signed_);
		if (!r) {
			ujournal_line(pl, "stop reason=stage-refused seed=%s what=\"%s\"",
				pl->seed, perfd_error(g_p));
			return -1;
		}
		ujournal_line(pl, "staged seed=%s signed=%s", pl->seed, signed_ ? "yes" : "no");
	}
	perfd_free_value(r);
	for (t = 0; t < 120; t++) {
		int missing = 0;

		for (i = 0; i < n; i++) {
			perfd_t *h = udial(ns[i].addr, ns[i].port);

			if (!h) {
				missing++;
				continue;
			}
			r = perfd_upgrade_status(h);
			if (!(r && strstr(r, pl->sha)))
				missing++;
			perfd_free_value(r);
			perfd_free(h);
		}
		if (!missing) {
			ujournal_line(pl, "staged on every member sha256=%s", pl->sha);
			return 0;
		}
		sleep(1);
	}
	for (i = 0; i < n; i++) {
		perfd_t *h = udial(ns[i].addr, ns[i].port);
		int has = 0;

		if (h) {
			r = perfd_upgrade_status(h);
			has = r && strstr(r, pl->sha) != NULL;
			perfd_free_value(r);
			perfd_free(h);
		}
		if (!has)
			ujournal_line(pl, "node=%d addr=%s:%d event=failed reason=not-staged "
				"(the relay refused it, or the member is unreachable - "
				"upgrade_status.relay on the seed says which)", ns[i].node,
				ns[i].addr, ns[i].port);
	}
	ujournal_line(pl, "stop reason=not-staged-everywhere - nothing was switched");
	return -1;
}

static int uwait_safe(const struct uplan *pl, struct unode *ns, int n, int before)
{
	int t, i;

	for (t = 0; t < 180; t++) {
		int all = 1;

		n = ufleet_read(ns, n > 0 ? n : UP_MAX_NODES);
		for (i = 0; i < n; i++)
			if (!ns[i].reach || !ns[i].safe)
				all = 0;
		if (all)
			return n;
		sleep(2);
	}
	ujournal_line(pl, "stop reason=not-restart-safe before=%d (a reconcile pass is open "
		"on a node that needs a peer to finish it)", before);
	return -1;
}

/* the node is up on the new build (a new pid, the version asked), then
 * ready with its peers up */
static int uwait_up(const struct uplan *pl, struct unode *u, long long old_pid, int peers)
{
	int t;

	for (t = 0; t < pl->ready_s; t++) {
		unode_read(u);
		if (u->reach && u->pid != old_pid && !strcmp(u->ver, pl->version)) {
			ujournal_line(pl, "node=%d addr=%s:%d event=up version=%s pid=%lld",
				u->node, u->addr, u->port, u->ver, u->pid);
			break;
		}
		sleep(1);
	}
	if (t >= pl->ready_s)
		return -1;
	for (t = 0; t < pl->gate_s; t++) {
		unode_read(u);
		if (u->reach && !strcmp(u->state, "ready") && u->peers_up >= peers) {
			ujournal_line(pl, "node=%d addr=%s:%d event=ready peers_up=%d",
				u->node, u->addr, u->port, u->peers_up);
			return 0;
		}
		sleep(1);
	}
	return -2;
}

static void uprint_rollback(const struct uplan *pl, const struct unode *u)
{
	printf("rollback this node:  perfcli -h %s -p %d -E <enable> upgrade-revert\n",
		u->addr, u->port);
	printf("rollback the roll:   perfcli -h %s -E <enable> upgrade rollback journal=%s\n",
		pl->seed, pl->journal);
	printf("continue:            perfcli -h %s -E <enable> upgrade resume journal=%s\n",
		pl->seed, pl->journal);
}

/* revert one node (the hook, or the daemon's upgrade_revert) and wait for the
 * old build to be ready again */
static int urevert_node(const struct uplan *pl, struct unode *u, int peers)
{
	long long old_pid = u->pid;
	int rc;

	if (pl->switch_cmd[0]) {
		rc = uhook(pl, "revert", u);
	} else {
		perfd_t *h = udial(u->addr, u->port);
		char *r = h ? perfd_upgrade_revert(h, 0) : NULL;

		rc = r ? 0 : -1;
		if (!r)
			ujournal_line(pl, "node=%d addr=%s:%d revert refused: %s", u->node,
				u->addr, u->port, h ? perfd_error(h) : "unreachable");
		perfd_free_value(r);
		if (h)
			perfd_free(h);
	}
	if (rc != 0)
		return -1;
	{
		int t;

		for (t = 0; t < pl->ready_s + pl->gate_s; t++) {
			unode_read(u);
			if (u->reach && u->pid != old_pid && !strcmp(u->state, "ready") &&
			        u->peers_up >= peers) {
				ujournal_line(pl, "node=%d addr=%s:%d event=reverted version=%s pid=%lld",
					u->node, u->addr, u->port, u->ver, u->pid);
				return 0;
			}
			sleep(1);
		}
	}
	return -1;
}

/* the roll: every node not yet gated, in the live order, one at a time */
static int uroll(struct uplan *pl, int resume)
{
	struct unode ns[UP_MAX_NODES], before[UP_MAX_NODES];
	int gated[UP_MAX_NODES], ng, done, n, i, k, peers;

	uabort_clear(pl);
	ng = ujournal_gated(pl, gated, UP_MAX_NODES, &done);
	ujournal_line(pl, "%s version=%s gated_before=%d", resume ? "resume" : "start",
		pl->version, ng);
	n = ufleet_read(ns, UP_MAX_NODES);
	if (n <= 0) {
		ujournal_line(pl, "stop reason=no-fleet (the seed answered no members)");
		return 1;
	}
	for (i = 0; i < n; i++)
		if (!ns[i].reach) {
			ujournal_line(pl, "stop reason=unreachable node=%d addr=%s:%d - never skipped; "
				"resume when it is back", ns[i].node, ns[i].addr, ns[i].port);
			return 1;
		}
	if (ustage(pl, ns, n) != 0)
		return 1;
	peers = n - 1;
	for (;;) {
		struct unode *u = NULL, cur;
		long long old_pid;
		int rc, is_gated;

		if (uabort_wanted(pl)) {
			ujournal_line(pl, "stop reason=abort (asked; the roll stopped between nodes)");
			uabort_clear(pl);
			return 1;
		}
		n = uwait_safe(pl, ns, n, 0);
		if (n < 0)
			return 1;
		qsort(ns, (size_t)n, sizeof ns[0], unode_cmp);   /* the master last, live */
		for (i = 0; i < n && !u; i++) {
			is_gated = 0;
			for (k = 0; k < ng; k++)
				if (gated[k] == ns[i].node)
					is_gated = 1;
			if (!is_gated)
				u = &ns[i];
		}
		if (!u) {
			ujournal_line(pl, "done version=%s nodes=%d", pl->version, n);
			printf("every node runs %s - the roll is done\n", pl->version);
			return 0;
		}
		memcpy(before, ns, sizeof ns);
		/* u points into ns, which every fleet re-read below overwrites in
		 * member order: the node being rolled is copied out first (225,
		 * 2026-10-16: the gated line named another node and the same node
		 * was switched again and again) */
		cur = *u;
		u = &cur;
		old_pid = u->pid;
		ujournal_line(pl, "node=%d addr=%s:%d event=switching from=%s role=%s pid=%lld",
			u->node, u->addr, u->port, u->ver, u->role, old_pid);
		if (pl->switch_cmd[0]) {
			rc = uhook(pl, "switch", u);
			if (rc != 0) {
				ujournal_line(pl, "node=%d addr=%s:%d event=failed reason=switch-cmd-rc-%d",
					u->node, u->addr, u->port, rc);
				uprint_rollback(pl, u);
				return 1;
			}
		} else {
			perfd_t *h = udial(u->addr, u->port);
			char *r = h ? perfd_upgrade_switch(h, pl->sha, 0) : NULL;

			if (!r) {
				ujournal_line(pl, "node=%d addr=%s:%d event=failed reason=switch-refused "
					"what=\"%s\"", u->node, u->addr, u->port,
					h ? perfd_error(h) : "unreachable");
				if (h)
					perfd_free(h);
				uprint_rollback(pl, u);
				return 1;
			}
			perfd_free_value(r);
			perfd_free(h);
		}
		rc = uwait_up(pl, u, old_pid, peers);
		if (rc != 0) {
			ujournal_line(pl, "node=%d addr=%s:%d event=failed reason=%s", u->node,
				u->addr, u->port, rc == -1 ? "not-up-in-time" : "not-ready-in-time");
			/* 12ns case 2/3: revert THAT node, verify the old build, stop */
			if (urevert_node(pl, u, peers) == 0)
				ujournal_line(pl, "stop reason=node-%d-reverted - the nodes already "
					"rolled stay on %s", u->node, pl->version);
			else
				ujournal_line(pl, "stop reason=node-%d-not-reverted - look at it now",
					u->node);
			uprint_rollback(pl, u);
			return 1;
		}
		/* the post-gate: the peers kept their records, every node restart-safe */
		{
			struct unode after[UP_MAX_NODES];
			int na = ufleet_read(after, UP_MAX_NODES), lost = 0;

			for (i = 0; i < n; i++) {
				struct unode *b = &before[i], *a;

				if (b->node == u->node)
					continue;
				a = ufind(after, na, b->node);
				if (!a || !a->reach) {
					lost = 1;
					ujournal_line(pl, "node=%d peer=%d unreachable after the restart",
						u->node, b->node);
				} else if (b->entries >= 50 && a->entries < b->entries * 9 / 10) {
					lost = 1;
					ujournal_line(pl, "node=%d peer=%d entries %lld -> %lld across the "
						"restart (S405's shape)", u->node, b->node, b->entries,
						a->entries);
				}
			}
			if (lost) {
				ujournal_line(pl, "node=%d addr=%s:%d event=failed reason=peers-lost-records",
					u->node, u->addr, u->port);
				if (urevert_node(pl, u, peers) == 0)
					ujournal_line(pl, "stop reason=node-%d-reverted after the gate", u->node);
				else
					ujournal_line(pl, "stop reason=node-%d-not-reverted - look at it now",
						u->node);
				uprint_rollback(pl, u);
				return 1;
			}
		}
		n = uwait_safe(pl, ns, n, u->node);
		if (n < 0)
			return 1;
		ujournal_line(pl, "node=%d addr=%s:%d event=gated version=%s", u->node,
			u->addr, u->port, pl->version);
		if (ng < UP_MAX_NODES)
			gated[ng++] = u->node;
		sleep(2);
	}
}

static int urollback(struct uplan *pl)
{
	struct unode ns[UP_MAX_NODES];
	int gated[UP_MAX_NODES], ng, done, n, i, peers;

	uabort_clear(pl);
	ng = ujournal_gated(pl, gated, UP_MAX_NODES, &done);
	n = ufleet_read(ns, UP_MAX_NODES);
	if (n <= 0) {
		ujournal_line(pl, "stop reason=no-fleet");
		return 1;
	}
	peers = n - 1;
	ujournal_line(pl, "rollback nodes=%d (the last rolled first)", ng);
	for (i = ng - 1; i >= 0; i--) {
		struct unode *u = ufind(ns, n, gated[i]);

		if (!u || !u->reach) {
			ujournal_line(pl, "stop reason=unreachable node=%d - resume the rollback when it is back",
				gated[i]);
			return 1;
		}
		if (uwait_safe(pl, ns, n, u->node) < 0)
			return 1;
		u = ufind(ns, n, gated[i]);
		ujournal_line(pl, "node=%d addr=%s:%d event=reverting from=%s", u->node, u->addr,
			u->port, u->ver);
		if (urevert_node(pl, u, peers) != 0) {
			ujournal_line(pl, "stop reason=node-%d-not-reverted - look at it now", u->node);
			return 1;
		}
	}
	ujournal_line(pl, "rollback done");
	return 0;
}

static int ustatus(struct uplan *pl)
{
	struct unode ns[UP_MAX_NODES];
	int gated[UP_MAX_NODES], ng, done, n, i, k;

	ng = ujournal_gated(pl, gated, UP_MAX_NODES, &done);
	printf("plan: %s version %s sha256 %.12s... journal %s%s\n", pl->artifact,
		pl->version, pl->sha, pl->journal, pl->switch_cmd[0] ? " (switch hook)" : "");
	printf("journal: %d node(s) gated%s%s\n", ng, done ? ", done" : "",
		uabort_wanted(pl) ? ", abort asked" : "");
	n = ufleet_read(ns, UP_MAX_NODES);
	qsort(ns, (size_t)n, sizeof ns[0], unode_cmp);
	printf("%-5s %-22s %-9s %-10s %-7s %-5s %s\n", "node", "member", "version",
		"state", "role", "safe", "roll");
	for (i = 0; i < n; i++) {
		char m[80];
		const char *w = "pending";

		for (k = 0; k < ng; k++)
			if (gated[k] == ns[i].node)
				w = "gated";
		if (!strcmp(w, "pending") && !strcmp(ns[i].ver, pl->version))
			w = "on it (not gated)";
		snprintf(m, sizeof m, "%s:%d", ns[i].addr, ns[i].port);
		printf("%-5d %-22s %-9s %-10s %-7s %-5s %s\n", ns[i].node, m,
			ns[i].reach ? ns[i].ver : "?", ns[i].reach ? ns[i].state : "unreachable",
			ns[i].role, ns[i].reach ? (ns[i].safe ? "yes" : "no") : "?", w);
	}
	return 0;
}

static int upgrade_cmd(int argc, char **argv)
{
	struct uplan pl;
	const char *sub = argc > 0 ? argv[0] : "";

	memset(&pl, 0, sizeof pl);
	pl.ready_s = 600;
	pl.gate_s = 90;
	udefault_journal(&pl);
	snprintf(pl.seed, sizeof pl.seed, "%s:%s", g_dial_host ? g_dial_host : "127.0.0.1",
		g_dial_port ? g_dial_port : "6479");
	if (!strcmp(sub, "plan")) {
		char dir[520];

		if (argc < 2 || uopts(&pl, argc - 2, argv + 2) != 0 || !pl.version[0]) {
			fprintf(stderr, "usage: upgrade plan <tarball> version=<v> [switch_cmd=<program>] "
				"[journal=<file>] [ready_s=<n>] [gate_s=<n>]\n");
			return 2;
		}
		snprintf(pl.artifact, sizeof pl.artifact, "%s", argv[1]);
		if (ufile_sha256(pl.artifact, pl.sha, sizeof pl.sha) != 0) {
			fprintf(stderr, "upgrade plan: cannot read %s\n", pl.artifact);
			return 2;
		}
		snprintf(dir, sizeof dir, "%s", pl.journal);
		{
			char *s = strrchr(dir, '/');

			if (s) {
				*s = 0;
				mkdir(dir, 0700);
			}
		}
		if (ujournal_line(&pl, "plan artifact=%s sha256=%s version=%s ready_s=%d gate_s=%d seed=%s switch_cmd=%s",
		        pl.artifact, pl.sha, pl.version, pl.ready_s, pl.gate_s, pl.seed,
		        pl.switch_cmd) != 0) {
			fprintf(stderr, "upgrade plan: cannot write %s\n", pl.journal);
			return 2;
		}
		printf("planned: %s (%s) -> every member of %s; journal %s\n", pl.version,
			pl.sha, pl.seed, pl.journal);
		printf("next: perfcli -h %s -E <enable> upgrade start journal=%s\n", pl.seed,
			pl.journal);
		return 0;
	}
	if (uopts(&pl, argc > 0 ? argc - 1 : 0, argv + 1) != 0)
		return 2;
	if (!strcmp(sub, "abort")) {
		char p[560];
		FILE *f;

		snprintf(p, sizeof p, "%s.abort", pl.journal);
		f = fopen(p, "w");
		if (!f) {
			fprintf(stderr, "upgrade abort: cannot write %s\n", p);
			return 2;
		}
		fclose(f);
		if (ujournal_plan(&pl) == 0)
			ujournal_line(&pl, "abort asked (the running driver stops between nodes)");
		printf("abort asked: the driver finishes the node it is on and stops\n");
		return 0;
	}
	if (ujournal_plan(&pl) != 0) {
		fprintf(stderr, "upgrade %s: no plan in %s - run 'upgrade plan' first\n", sub,
			pl.journal);
		return 2;
	}
	if (!strcmp(sub, "start"))
		return uroll(&pl, 0);
	if (!strcmp(sub, "resume"))
		return uroll(&pl, 1);
	if (!strcmp(sub, "status"))
		return ustatus(&pl);
	if (!strcmp(sub, "rollback"))
		return urollback(&pl);
	fprintf(stderr, "usage: upgrade plan|start|resume|status|abort|rollback\n");
	return 2;
}
