/* cfgchecktest.c - the config's rules (cfgcheck.c; the 0.7.2 split, wave
 * 1): cfg_validate on configs built here, each from one valid base with
 * one thing changed; the log stubbed, so what is refused and what is only
 * warned of is read back by its words.
 *
 *   A  the base - valid, nothing said; an alert warning not below its
 *      critical
 *   B  collections: none, a size missing, a name twice; standalone, a
 *      fleet mode and pull said and dropped
 *   C  secrets: none; the cluster's equal to a client's; a client's twice;
 *      a short one and allow_create with no enable secret - warned only
 *   D  listeners: none; RESP off-box with no allow-list; RESP on loopback,
 *      announced; its password equal to a client secret; HTTP off-box
 *      with no allow-list; an HTTP allow-list with no HTTP listener
 *   E  read-through: no prefixes; the no-fall companion made; failover's
 *      primary found in the list (user@ and /db stripped) or not; a
 *      refusing collection has no companion; write-through passes unknown
 *      commands
 *   F  memory: the ceiling below the arena, the floor above the ceiling,
 *      pin = require with a heap backing
 *   G  the cluster: no discovery; unicast's derived ports; unicast with no
 *      port; the set without a mode; spread without K, or K = 1; a local
 *      collection outside the set; the set's member with no block made,
 *      the mode stamped; a local mode contradicting; mode with no
 *      [cluster]
 *   H  [mail]: no server; a user with no password; complete; tls_verify
 *      off, said; a mail password with no [mail]
 *   I  the file world-readable - warned, by its path
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "compat/dprint.h"
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

	/* A */
	base();
	CHK(check() == 0 && !errs && !warns && !notices, "A: the base config - valid, nothing said");
	CHK(ps.line == 0, "A: the rules' errors carry no line number");
	base();
	cfg.alerts.io_warn = 75;
	CHK(refused("[alerts] io_warn_pct must be below io_crit_pct"), "A: io's warning at its critical - refused");

	/* B */
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

	/* C */
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

	/* D */
	base();
	cfg.n_listen = 0;
	CHK(refused("no listeners configured"), "D: no listeners - refused");
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

	/* E */
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

	/* F */
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

	/* G */
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

	/* H */
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

	/* I */
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

	printf("cfgchecktest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
