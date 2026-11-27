/* cfgchecktest.c - the config rules (cfg_validate): each case is one valid base with one thing
 * changed, and what is refused or only warned of is read back from the stubbed log by its words. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include "compat/dprint.h"
#include "clwire.h"
#include "config.h"
#include "configint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stub: the log, kept ------------------------------------------- */
int compat_log_level = L_NOTICE;
static char logbuf[16384];
static int errs, warns, notices;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	size_t n = strlen(logbuf);
	va_list ap;

	(void)tag;
	va_start(ap, fmt);
	vsnprintf(logbuf + n, sizeof logbuf - n, fmt, ap);
	va_end(ap);
	if (level == L_ERR)
		errs++;
	else if (level == L_WARN)
		warns++;
	else if (level == L_NOTICE)
		notices++;
}

/* ---- the harness ------------------------------------------------------- */
static struct pc_config cfg;
static struct pstate ps;
static const char *path = "/nonexistent/cfgchecktest.conf";

/* the base: one collection, one loopback native listener, one client
 * secret, the alert defaults - valid */
static void base(void)
{
	memset(&cfg, 0, sizeof cfg);
	logbuf[0] = 0;
	errs = warns = notices = 0;
	cfg.alerts.cpu_warn = cfg.alerts.io_warn = 65;
	cfg.alerts.mem_warn = cfg.alerts.arena_warn = 65;
	cfg.alerts.cpu_crit = cfg.alerts.io_crit = 75;
	cfg.alerts.mem_crit = cfg.alerts.arena_crit = 75;
	cfg.alerts.link_warn = 80;
	cfg.alerts.link_crit = 95;
	cfg.col[0].name = "c";
	cfg.col[0].buckets_log2 = 16;
	cfg.n_col = 1;
	cfg.listen[0].type = PC_LISTEN_TCP;
	cfg.listen[0].addr = "127.0.0.1";
	cfg.listen[0].port = 6480;
	cfg.listen[0].loopback = 1;
	cfg.n_listen = 1;
	cfg.client_secret[0] = "client-secret-1";
	cfg.n_client_secrets = 1;
	memset(&ps, 0, sizeof ps);
	ps.path = path;
	ps.line = 5;                       /* validate's errors are not line errors */
	ps.cfg = &cfg;
}

static int check(void)
{
	return cfg_validate(&ps, path, &cfg);
}

/* refused, the log saying @what */
static int refused(const char *what)
{
	return check() == -1 && errs == 1 && strstr(logbuf, what) != NULL;
}

/* @body as a config file, through pc_config_load into @lc: its answer, the log in logbuf */
static int load_text(const char *body, struct pc_config *lc)
{
	char tmp[64];
	FILE *f;
	int fd, rc;

	snprintf(tmp, sizeof tmp, "/tmp/cfgchecktest.XXXXXX");
	if ((fd = mkstemp(tmp)) < 0 || !(f = fdopen(fd, "w")))
		return -2;
	fputs(body, f);
	fclose(f);
	chmod(tmp, 0640);
	logbuf[0] = 0;
	errs = warns = notices = 0;
	rc = pc_config_load(tmp, lc);
	unlink(tmp);
	return rc;
}

static void cluster(void)
{
	cfg.cl_enabled = 1;
	cfg.cluster_secret = "cluster-secret-1";
	cfg.cl_unicast = 1;
	cfg.cl_port = 7000;
	cfg.cl_seeds = "10.0.0.2";
}

int main(void)
{
	char tmp[64];

	/* A: the base is valid and silent; an alert warning not below its critical */
	base();
	CHK(check() == 0 && !errs && !warns && !notices, "A: the base config - valid, nothing said");
	CHK(ps.line == 0, "A: the rules' errors carry no line number");
	base();
	cfg.alerts.io_warn = 75;
	CHK(refused("[alerts] io_warn_pct must be below io_crit_pct"), "A: io's warning at its critical - refused");
	base();
	cfg.alerts.link_warn = 95;
	CHK(refused("[alerts] link_warn_pct must be below link_crit_pct"),
		"A: the link's warning at its critical - refused (S465)");

	/* B: collections - none, a size missing, a name twice; standalone with fleet settings */
	base();
	cfg.n_col = 0;
	CHK(refused("no collections configured"), "B: no collections - refused");
	base();
	cfg.col[0].buckets_log2 = -1;
	CHK(refused("collection 'c': buckets_log2 missing"), "B: a collection with no size - refused");
	base();
	cfg.col[1] = cfg.col[0];
	cfg.n_col = 2;
	CHK(refused("duplicate collection 'c'"), "B: a name twice - refused");
	base();
	cfg.col[0].mode = PC_MODE_PROXY;
	cfg.col[0].pull = 1;
	CHK(check() == 0 && warns == 2 && strstr(logbuf, "mode = proxy ignored") &&
		strstr(logbuf, "pull = 1 ignored") && cfg.col[0].mode == PC_MODE_STORE && !cfg.col[0].pull,
		"B: standalone, mode = proxy and pull = 1 - dropped, each said, running as store");

	/* C: secrets - none, the cluster's equal to a client's, one twice; weak ones only warned */
	base();
	cfg.n_client_secrets = 0;
	CHK(refused("no client secret configured"), "C: no client secret - refused");
	base();
	{
		char same[] = "client-secret-1";

		cfg.cluster_secret = same;
		CHK(refused("the cluster secret EQUALS client secret #1"),
			"C: the cluster's secret equal to a client's - refused (two principals)");
	}
	base();
	cfg.client_secret[1] = "client-secret-1";
	cfg.n_client_secrets = 2;
	CHK(refused("duplicate client secret (#2 = #1)"), "C: a client secret twice - refused");
	base();
	cfg.client_secret[0] = "short";
	cfg.allow_create = 1;
	CHK(check() == 0 && warns == 2 && strstr(logbuf, "client secret #1 is under 8 characters") &&
		strstr(logbuf, "allow_create is ON and no [secrets] enable is set"),
		"C: a short secret, allow_create with no enable secret - warned, not refused");

	/* D: listeners - none, RESP and HTTP off-box without an allow-list, RESP password reuse */
	base();
	cfg.n_listen = 0;
	CHK(refused("no listeners configured"), "D: no listeners - refused");
	base();
	cfg.listen[1].type = PC_LISTEN_UNIX;
	cfg.listen[1].addr = "/run/pc/a.sock";
	cfg.listen[1].loopback = 1;
	cfg.listen[2] = cfg.listen[1];
	cfg.listen[2].addr = "/run/pc/b.sock";
	cfg.n_listen = 3;
	CHK(refused("more than one 'unix =' listener"), "D: two unix listeners - refused");
	cfg.n_listen = 2;
	errs = 0;
	logbuf[0] = 0;
	CHK(check() == 0, "D: one unix listener beside a TCP one - valid");
	base();
	cfg.listen[0].resp = 1;
	cfg.listen[0].loopback = 0;
	CHK(refused("a non-loopback RESP listener needs resp_allow"),
		"D: RESP off-box with no allow-list - refused");
	base();
	cfg.listen[0].resp = 1;
	CHK(check() == 0 && notices == 1 &&
		strstr(logbuf, "RESP listener on 127.0.0.1:6480 - 0 allow-list entries, password none, collections all"),
		"D: RESP on loopback - announced, with what guards it");
	base();
	{
		char same[] = "client-secret-1";

		cfg.listen[0].resp = 1;
		cfg.resp_password = same;
		CHK(refused("the resp password EQUALS client secret #1"),
			"D: the RESP password equal to a client secret - refused");
	}
	base();
	cfg.listen[0].http = 1;
	cfg.listen[0].loopback = 0;
	CHK(refused("a non-loopback http listener needs http_allow"),
		"D: HTTP off-box with no allow-list - refused");
	base();
	cfg.n_http_allow = 1;
	CHK(refused("http_allow is set but no 'http = addr:port' listener exists"),
		"D: an HTTP allow-list with no HTTP listener - refused");

	/* E: read-through - prefixes, the no-fall companion, failover's primary, write-through */
	base();
	cfg.col[0].upstream = "redis://10.0.0.1:6379";
	CHK(refused("[collection c] upstream needs upstream_prefixes"), "E: an upstream with no prefixes - refused");
	base();
	cfg.col[0].upstream = "redis://10.0.0.1:6379";
	cfg.col[0].up_prefixes = "*";
	CHK(check() == 0 && cfg.n_col == 2 && !strcmp(cfg.col[1].name, "c.nofall") &&
		cfg.col[1].buckets_log2 == 16 && !cfg.col[1].upstream,
		"E: a read-through collection - its companion c.nofall made, sized like it");
	free(cfg.col[1].name);
	base();
	cfg.col[0].upstream = "redis://u@10.0.0.1:6379/0, 10.0.0.2:6379";
	cfg.col[0].up_prefixes = "*";
	cfg.col[0].up_mode = PC_UP_MODE_FAILOVER;
	cfg.col[0].has_up_mode = 1;
	cfg.col[0].up_primary = "10.0.0.2:6379";
	CHK(check() == 0 && !errs, "E: failover's primary, the second listed - found");
	free(cfg.col[1].name);
	base();
	cfg.col[0].upstream = "redis://u@10.0.0.1:6379/0, 10.0.0.2:6379";
	cfg.col[0].up_prefixes = "*";
	cfg.col[0].up_mode = PC_UP_MODE_FAILOVER;
	cfg.col[0].has_up_mode = 1;
	cfg.col[0].up_primary = "10.0.0.3:6379";
	CHK(refused("upstream_primary = 10.0.0.3:6379 is not one of its upstreams"),
		"E: a primary not in the list - refused");
	base();
	cfg.col[0].upstream = "redis://u@10.0.0.1:6379/0, 10.0.0.2:6379";
	cfg.col[0].up_prefixes = "*";
	cfg.col[0].up_mode = PC_UP_MODE_FAILOVER;
	cfg.col[0].has_up_mode = 1;
	cfg.col[0].up_primary = "10.0.0.1:6379";
	CHK(check() == 0, "E: the first listed, behind user@ and before /db - found");
	free(cfg.col[1].name);
	base();
	cfg.col[0].upstream = "redis://10.0.0.1:6379";
	cfg.col[0].up_prefixes = "*";
	cfg.col[0].up_refuse_writes = 1;
	CHK(check() == 0 && cfg.n_col == 1, "E: a collection that refuses writes - no companion");
	base();
	cfg.col[0].upstream = "redis://10.0.0.1:6379";
	cfg.col[0].up_prefixes = "*";
	cfg.col[0].up_write_through = 1;
	CHK(check() == 0 && cfg.col[0].up_unknown == PC_UP_UNKNOWN_PASS && cfg.n_col == 2,
		"E: write-through - unknown commands pass to the upstream by default");
	free(cfg.col[1].name);
	base();
	cfg.col[0].up_write_through = 1;
	CHK(refused("[collection c] upstream_writes needs upstream ="),
		"E: upstream_writes with no upstream - refused");

	/* F: memory - ceiling below the arena, floor above the ceiling, pin with a heap backing */
	base();
	cfg.arena_mb = 1024;
	cfg.arena_cap_mb = 512;
	CHK(refused("arena_cap_mb 512 below arena_mb 1024"), "F: the ceiling below the arena - refused");
	base();
	cfg.arena_mb = 1024;
	cfg.reclaim_floor_mb = 2048;
	CHK(refused("reclaim_floor_mb 2048 above the arena ceiling 1024 MB"),
		"F: the floor above the ceiling (the arena, no cap) - refused");
	base();
	cfg.pin_require = 1;
	cfg.backing_heap = 1;
	CHK(refused("pin = require needs backing = own"), "F: pin = require on a heap backing - refused");

	/* G: the cluster - discovery, unicast ports, the set, spread's K, local collection modes */
	base();
	cluster();
	cfg.cl_unicast = 0;
	CHK(refused("[cluster] needs multicast = group:port"), "G: a cluster with no discovery - refused");
	base();
	cluster();
	CHK(check() == 0 && cfg.cl_lane_port == 7002 && cfg.cl_ps_port == 7001 && warns == 1 &&
		strstr(logbuf, "[cluster] without `collections`"),
		"G: unicast on 7000 - lane 7002, pub/sub 7001; no set declared, warned");
	base();
	cluster();
	cfg.cl_port = 0;
	CHK(refused("discovery = unicast needs port = <cluster port>"), "G: unicast with no port - refused");
	base();
	cluster();
	cfg.cl_port = 65534;
	cfg.cl_max_dgram = 1232;
	CHK(refused("the cluster port is 65534, so lane_port must be set"),
		"G: unicast on 65534 with a lane and no lane_port - refused, not a lane on port 0");
	cfg.cl_lane_port = 7002;
	errs = 0;
	logbuf[0] = 0;
	CHK(check() == 0 && cfg.cl_lane_port == 7002, "G: ... and with lane_port = 7002 - valid");
	base();
	cluster();
	cfg.cl_colname[0] = "c";
	cfg.n_cl_col = 1;
	CHK(refused("[cluster] collections = ... needs mode ="), "G: the set with no mode - refused");
	base();
	cluster();
	cfg.cl_colname[0] = "c";
	cfg.n_cl_col = 1;
	cfg.cl_has_mode = 1;
	cfg.cl_mode = PC_MODE_SPREAD;
	CHK(refused("[cluster] mode = spread needs replicas = N"), "G: spread with no K - refused");
	cfg.cl_replicas = 1;
	errs = 0;
	logbuf[0] = 0;
	CHK(refused("[cluster] replicas = 1 is shard mode"), "G: spread with K = 1 - refused, not aliased");
	base();
	cluster();
	cfg.cl_colname[0] = "d";
	cfg.n_cl_col = 1;
	cfg.cl_has_mode = 1;
	cfg.cl_mode = PC_MODE_STORE;
	CHK(refused("[collection c] is not in the cluster's collections list"),
		"G: a local collection outside the set - refused");
	base();
	cluster();
	cfg.cl_colname[0] = "c";
	cfg.cl_colname[1] = "d";
	cfg.n_cl_col = 2;
	cfg.cl_has_mode = 1;
	cfg.cl_mode = PC_MODE_STORE;
	CHK(check() == 0 && cfg.n_col == 2 && !strcmp(cfg.col[1].name, "d") &&
		cfg.col[1].buckets_log2 == PC_DEFAULT_BUCKETS_LOG2 && notices == 1 &&
		strstr(logbuf, "collection 'd' comes from the cluster"),
		"G: the set's d with no local block - made, default size, said");
	CHK(cfg.col[0].has_mode && cfg.col[0].pull && cfg.col[1].has_mode && cfg.col[1].pull &&
		cfg.col[1].mode == PC_MODE_STORE && !cfg.col[1].eager,
		"G: the cluster's mode stamped on both - store pulls on a miss");
	free(cfg.col[1].name);
	base();
	cluster();
	cfg.cl_colname[0] = "c";
	cfg.n_cl_col = 1;
	cfg.cl_has_mode = 1;
	cfg.cl_mode = PC_MODE_STORE;
	cfg.col[0].has_mode = 1;
	cfg.col[0].mode = PC_MODE_PROXY;
	CHK(refused("[collection c] mode = proxy contradicts [cluster] mode = store"),
		"G: a local mode contradicting the cluster's - refused, both named");
	base();
	cfg.cl_has_mode = 1;
	CHK(refused("[cluster] mode/collections without a [cluster] section"),
		"G: a cluster mode with no [cluster] - refused");

	/* H: [mail] - no server, a user without password, tls_verify off, a stray password */
	base();
	cfg.mail_on = 1;
	CHK(refused("[mail] needs smtp = smtp://host:port or smtps://host:port"), "H: [mail] with no server - refused");
	base();
	cfg.mail_on = 1;
	cfg.mail.url = "smtp://10.0.0.5:25";
	cfg.mail.from = "pc@example.org";
	cfg.mail.to[0] = "ops@example.org";
	cfg.mail.nto = 1;
	cfg.mail.user = "alerts";
	CHK(refused("[mail] user = alerts, but no [secrets] mail"), "H: a user with no password - refused");
	cfg.mail.pass = "mail-pass";
	cfg.mail.tls_verify = 1;
	errs = 0;
	logbuf[0] = 0;
	CHK(check() == 0 && !warns, "H: complete, verified - valid, nothing said");
	cfg.mail.tls_verify = 0;
	CHK(check() == 0 && warns == 1 && strstr(logbuf, "tls_verify = no") &&
		strstr(logbuf, "anyone on the path to smtp://10.0.0.5:25"), "H: tls_verify = no - warned, naming the server");
	base();
	cfg.mail.pass = "mail-pass";
	CHK(check() == 0 && warns == 1 && strstr(logbuf, "[secrets] mail is set but there is no [mail] section"),
		"H: a mail password with no [mail] - warned, unused");

	/* I: a world-readable config file is warned of by its path */
	snprintf(tmp, sizeof tmp, "/tmp/cfgchecktest.XXXXXX");
	{
		int fd = mkstemp(tmp);

		if (fd < 0)
			return 2;
		close(fd);
	}
	path = tmp;
	chmod(tmp, 0644);
	base();
	CHK(check() == 0 && warns == 1 && strstr(logbuf, "is world-readable and holds secrets") &&
		strstr(logbuf, tmp), "I: the file world-readable - warned, by its path");
	chmod(tmp, 0640);
	base();
	CHK(check() == 0 && !warns, "I: mode 640 - nothing said");
	unlink(tmp);

	/* J: relay threads - auto = half the workers rounded up, at most one a lane, or a number */
	{
		static const int wk[][2] = { { 12, 6 }, { 7, 4 }, { 2, 1 }, { 1, 1 },
			{ 64, CLWIRE_LANES } };
		static const struct { const char *line; int n, autom; } rx[] = {
			{ "", 6, 1 }, { "pubsub_rx_threads = 5\n", 5, 0 },
			{ "pubsub_rx_threads = auto\n", 6, 1 } };
		static struct pc_config lc;
		unsigned int i;
		FILE *f;
		int fd, rc;

		for (i = 0; i < sizeof wk / sizeof wk[0]; i++) {
			base();
			cluster();
			cfg.workers = wk[i][0];
			cfg.cl_ps_rx_threads = -1;
			CHK(check() == 0 && cfg.cl_ps_rx_threads == wk[i][1] && cfg.cl_ps_rx_auto,
				"J: auto with %d workers - %d relay threads (%d)", wk[i][0], wk[i][1],
				cfg.cl_ps_rx_threads);
		}
		base();
		cluster();
		cfg.workers = 12;
		cfg.cl_ps_rx_threads = 3;
		CHK(check() == 0 && cfg.cl_ps_rx_threads == 3 && !cfg.cl_ps_rx_auto,
			"J: a number is kept, not marked auto");
		for (i = 0; i < sizeof rx / sizeof rx[0]; i++) {
			snprintf(tmp, sizeof tmp, "/tmp/cfgchecktest.XXXXXX");
			if ((fd = mkstemp(tmp)) < 0 || !(f = fdopen(fd, "w")))
				return 2;
			fprintf(f, "[daemon]\nworkers = 12\n[secrets]\nclient = cfgcheck-client-secret-1\n"
				"cluster = cfgcheck-cluster-secret-1\n[listen]\ntcp = 127.0.0.1:6480\n"
				"plaintext = loopback\n[cluster]\ndiscovery = unicast\nport = 7000\n"
				"seeds = 10.0.0.2\n%s[collection c]\nbuckets_log2 = 9\n", rx[i].line);
			fclose(f);
			chmod(tmp, 0640);
			rc = pc_config_load(tmp, &lc);
			CHK(rc == 0 && lc.cl_ps_rx_threads == rx[i].n && lc.cl_ps_rx_auto == rx[i].autom,
				"J: 12 workers, the file says \"%.*s\" - %d relay threads (rc %d, %d)",
				(int)strcspn(rx[i].line, "\n"), rx[i].line, rx[i].n, rc, lc.cl_ps_rx_threads);
			unlink(tmp);
		}
	}

	/* K: the WAL's segments_max below its floor is refused, at or above it kept */
	base();
	cfg.wal_segments = 8;
	cfg.wal_segments_max = 4;
	CHK(refused("[wal] segments_max 4 is below segments (the floor) 8"),
		"K: segments_max below segments - refused");
	base();
	cfg.wal_segments = 8;
	cfg.wal_segments_max = 32;
	CHK(check() == 0 && cfg.wal_segments_max == 32, "K: segments_max above segments - kept");
	{
		static const struct { const char *wal; int warned; } wk[] = {
			{ "[wal]\nfsync = always\nsave = 900 1\n", 1 },
			{ "[wal]\ndir = /var/tmp/cfgcheck-wal\nfsync = always\n", 0 } };
		static struct pc_config lc;
		unsigned int i;
		FILE *f;
		int fd, rc;

		for (i = 0; i < sizeof wk / sizeof wk[0]; i++) {
			snprintf(tmp, sizeof tmp, "/tmp/cfgchecktest.XXXXXX");
			if ((fd = mkstemp(tmp)) < 0 || !(f = fdopen(fd, "w")))
				return 2;
			fprintf(f, "[secrets]\nclient = cfgcheck-client-secret-1\n[listen]\n"
				"tcp = 127.0.0.1:6480\nplaintext = loopback\n%s"
				"[collection c]\nbuckets_log2 = 9\n", wk[i].wal);
			fclose(f);
			chmod(tmp, 0640);
			logbuf[0] = 0;
			warns = 0;
			rc = pc_config_load(tmp, &lc);
			CHK(rc == 0 && (strstr(logbuf, "[wal] fsync is set but [wal] has no dir") != NULL) ==
				wk[i].warned, "K: [wal] fsync %s dir - %s (rc %d)",
				wk[i].warned ? "without" : "with", wk[i].warned ? "warned" : "nothing said", rc);
			if (rc == 0)
				pc_config_free(&lc);
			unlink(tmp);
		}
	}

	/* L: an empty HTTP token is refused, as every other secret is; a set one is kept */
	base();
	cfg.listen[0].http = 1;
	cfg.http_token = "";
	CHK(refused("empty http secret"), "L: [secrets] http = (empty) - refused");
	base();
	cfg.listen[0].http = 1;
	cfg.http_token = "http-token-1";
	CHK(check() == 0, "L: [secrets] http = http-token-1 - kept");

	/* M: no secret shared across principals - cluster_accept and enable against the clients,
	 * the clear-text RESP password and HTTP token against every principal's */
	base();
	{
		char same[] = "client-secret-1";

		cluster();
		cfg.cluster_accept_secret = same;
		CHK(refused("the cluster_accept secret EQUALS client secret #1"),
			"M: cluster_accept equal to a client secret - refused");
	}
	base();
	{
		char same[] = "client-secret-1";

		cfg.enable_secret[0] = same;
		cfg.n_enable_secrets = 1;
		CHK(refused("enable secret #1 EQUALS client secret #1"),
			"M: the enable secret equal to a client secret - refused");
	}
	base();
	{
		char same[] = "enable-secret-1";

		cfg.enable_secret[0] = "enable-secret-1";
		cfg.n_enable_secrets = 1;
		cfg.listen[0].http = 1;
		cfg.http_token = same;
		CHK(refused("the http token EQUALS enable secret #1"),
			"M: the HTTP token equal to the enable secret - refused");
	}
	base();
	{
		char same[] = "client-secret-1";

		cfg.listen[0].http = 1;
		cfg.http_token = same;
		CHK(refused("the http token EQUALS client secret #1"),
			"M: the HTTP token equal to a client secret - refused");
	}
	base();
	{
		char same[] = "accept-secret-1";

		cluster();
		cfg.cluster_accept_secret = "accept-secret-1";
		cfg.listen[0].resp = 1;
		cfg.resp_password = same;
		CHK(refused("the resp password EQUALS the cluster_accept secret"),
			"M: the RESP password equal to cluster_accept - refused");
	}
	base();
	cfg.enable_secret[0] = "enable-secret-1";
	cfg.n_enable_secrets = 1;
	cfg.listen[0].http = 1;
	cfg.http_token = "http-token-1";
	CHK(check() == 0 && !errs, "M: every secret its own - valid");

	/* N: an unquoted '#' inside a secret is refused, not a cut-short secret; after a space it
	 * starts a comment, and quoted it is part of the secret */
	{
		static struct pc_config lc;
		static const char *const tail = "[listen]\ntcp = 127.0.0.1:6480\nplaintext = loopback\n"
			"[collection c]\nbuckets_log2 = 9\n";
		char body[512];
		int rc;

		snprintf(body, sizeof body, "[secrets]\nclient = Tr0ub4dor#3xtra\n%s", tail);
		rc = load_text(body, &lc);
		CHK(rc == -1 && strstr(logbuf, "must be quoted"),
			"N: client = Tr0ub4dor#3xtra - refused, quote it (rc %d)", rc);
		snprintf(body, sizeof body, "[secrets]\nclient = Tr0ub4dor-3xtra   # rotated 11-26\n%s", tail);
		rc = load_text(body, &lc);
		CHK(rc == 0 && lc.n_client_secrets == 1 && !strcmp(lc.client_secret[0], "Tr0ub4dor-3xtra"),
			"N: a comment after a space - the secret without it (rc %d)", rc);
		if (rc == 0)
			pc_config_free(&lc);
		snprintf(body, sizeof body, "[secrets]\nclient = \"Tr0ub4dor#3xtra\"\n%s", tail);
		rc = load_text(body, &lc);
		CHK(rc == 0 && lc.n_client_secrets == 1 && !strcmp(lc.client_secret[0], "Tr0ub4dor#3xtra"),
			"N: quoted - the whole secret (rc %d)", rc);
		if (rc == 0)
			pc_config_free(&lc);
	}

	/* O: the reload's keymap folds a section's word, not a collection's name */
	{
		static struct pc_config lc;
		static struct pc_keymap km;
		char tmp2[64];
		FILE *f;
		int fd, rc, i, foo = 0, Foo = 0, lst = 0;

		snprintf(tmp2, sizeof tmp2, "/tmp/cfgchecktest.XXXXXX");
		if ((fd = mkstemp(tmp2)) < 0 || !(f = fdopen(fd, "w")))
			return 2;
		fputs("[secrets]\nclient = cfgcheck-client-secret-1\n[LISTEN]\ntcp = 127.0.0.1:6480\n"
			"plaintext = loopback\n[collection Foo]\nbuckets_log2 = 9\n"
			"[collection foo]\nbuckets_log2 = 9\n", f);
		fclose(f);
		chmod(tmp2, 0640);
		rc = pc_config_load_km(tmp2, &lc, &km);
		unlink(tmp2);
		for (i = 0; i < km.n; i++) {
			foo += !strcmp(km.e[i].sect, "collection foo");
			Foo += !strcmp(km.e[i].sect, "collection Foo");
			lst += !strcmp(km.e[i].sect, "listen");
		}
		CHK(rc == 0 && foo == 1 && Foo == 1 && lst == 2,
			"O: [collection Foo] and [collection foo] - two keymap sections; [LISTEN] is listen "
			"(rc %d, %d %d %d)", rc, foo, Foo, lst);
		if (rc == 0)
			pc_config_free(&lc);
	}

	/* P: the -E dump names an HTTP door as HTTP and a TLS door as TLS, with the HTTP allow-list
	 * and every secret masked */
	{
		char tmp3[] = "/var/tmp/cfgchecktest.XXXXXX", out[16384];
		int fd = mkstemp(tmp3), save;
		ssize_t n = 0;

		if (fd < 0)
			return 2;
		base();
		cfg.listen[1].type = PC_LISTEN_TCP;
		cfg.listen[1].addr = "0.0.0.0";
		cfg.listen[1].port = 9100;
		cfg.listen[1].http = 1;
		cfg.listen[2].type = PC_LISTEN_TCP;
		cfg.listen[2].addr = "127.0.0.1";
		cfg.listen[2].port = 6479;
		cfg.listen[2].resp = 1;
		cfg.listen[2].tls = 1;
		cfg.n_listen = 3;
		cfg.http_allow[0].family = AF_INET;
		cfg.http_allow[0].bits = 8;
		cfg.http_allow[0].net[0] = 10;
		cfg.n_http_allow = 1;
		cfg.enable_secret[0] = "enable-secret-1";
		cfg.n_enable_secrets = 1;
		cfg.http_token = "http-token-1";
		cfg.resp_password = "resp-password-1";
		fflush(stdout);
		save = dup(1);
		dup2(fd, 1);
		pc_config_dump(&cfg);
		fflush(stdout);
		dup2(save, 1);
		close(save);
		n = pread(fd, out, sizeof out - 1, 0);
		close(fd);
		unlink(tmp3);
		out[n > 0 ? n : 0] = 0;
		CHK(strstr(out, "http = 0.0.0.0:9100") && !strstr(out, "tcp = 0.0.0.0:9100"),
			"P: http = 0.0.0.0:9100 dumped as http, not as a native door");
		CHK(strstr(out, "resp = 127.0.0.1:6479 tls") && !strstr(out, "6479   # RESP2 only, plaintext"),
			"P: a TLS RESP door dumped with tls, not as plaintext");
		CHK(strstr(out, "http_allow = 10.0.0.0/8") != NULL, "P: http_allow dumped");
		CHK(strstr(out, "enable = ******** (len 15)") && strstr(out, "http = ******** (len 12)") &&
			strstr(out, "resp = ******** (len 15)") && !strstr(out, "enable-secret-1") &&
			!strstr(out, "http-token-1") && !strstr(out, "resp-password-1"),
			"P: the enable secret, HTTP token and RESP password - said, masked");
	}

	/* Q: the descriptor reserve counts every worker's socket on each TCP listener */
	base();
	cluster();
	cfg.workers = 64;
	{
		int k;

		for (k = 1; k < 4; k++) {
			cfg.listen[k] = cfg.listen[0];
			cfg.listen[k].port = 6480 + k;
		}
	}
	cfg.listen[4].type = PC_LISTEN_UNIX;
	cfg.listen[4].addr = "/run/pc/a.sock";
	cfg.n_listen = 5;
	CHK(pc_config_fd_reserve(&cfg) >= 64L * (2 + 4) + 1 + 16,
		"Q: 64 workers, 4 TCP listeners and a unix one - at least 64 x (2 + 4) + 1 + 16 held "
		"apart (%ld)", pc_config_fd_reserve(&cfg));

	printf("cfgchecktest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
