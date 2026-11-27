/* cfgkvtest.c - each config section's function on its own keys: a value taken, one out of range
 * or its words refused, another section's key answered CFG_KV_UNKNOWN ("unknown key"). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "config.h"
#include "configint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static struct pc_config cfg;
static struct pc_collection col;
static struct pstate ps;

/* @f on @key = @val: its answer */
static int kv(int (*f)(struct pstate *, char *, char *), const char *key, const char *val)
{
	char k[64], v[256];

	snprintf(k, sizeof k, "%s", key);
	snprintf(v, sizeof v, "%s", val);
	return f(&ps, k, v);
}

/* @f on @key = @val, its answer; what it logged in @out */
static int kv_logged_f(int (*f)(struct pstate *, char *, char *), const char *key,
		const char *val, char *out, size_t cap)
{
	char tmp[] = "/var/tmp/cfgkvtest.XXXXXX";
	int fd = mkstemp(tmp), save = dup(2), rc;
	ssize_t n = 0;

	fflush(stderr);
	if (fd >= 0)
		dup2(fd, 2);
	rc = kv(f, key, val);
	fflush(stderr);
	dup2(save, 2);
	close(save);
	if (fd >= 0) {
		n = pread(fd, out, cap - 1, 0);
		close(fd);
		unlink(tmp);
	}
	out[n > 0 ? n : 0] = 0;
	return rc;
}

/* @key = @val on [listen] */
static int kv_logged(const char *key, const char *val, char *out, size_t cap)
{
	return kv_logged_f(cfg_kv_listen, key, val, out, cap);
}

static int count_of(const char *s, const char *w)
{
	int n = 0;

	while ((s = strstr(s, w)) != NULL) {
		n++;
		s += strlen(w);
	}
	return n;
}

int main(void)
{
	char lg[2048];
	int i, n;

	ps.path = "cfgkvtest.conf";
	ps.line = 0;                       /* no file line: errors to the log */
	ps.cfg = &cfg;
	ps.col = &col;

	/* A: [daemon] allow_create's words, log_events_rate and workers by their ranges */
	CHK(kv(cfg_kv_daemon, "allow_create", "yes") == 0 && cfg.allow_create == 1 &&
		kv(cfg_kv_daemon, "allow_create", "off") == 0 && cfg.allow_create == 0,
		"A: allow_create yes / off");
	CHK(kv(cfg_kv_daemon, "allow_create", "maybe") == -1 && cfg.allow_create == 0,
		"A: allow_create maybe - refused, unchanged");
	CHK(kv(cfg_kv_daemon, "log_events_rate", "100000") == 0 && cfg.log_events_rate == 100000 &&
		kv(cfg_kv_daemon, "log_events_rate", "0") == -1 &&
		kv(cfg_kv_daemon, "log_events_rate", "100001") == -1 && cfg.log_events_rate == 100000,
		"A: log_events_rate in 1..100000");
	CHK(kv(cfg_kv_daemon, "workers", "512") == 0 && cfg.workers == 512 &&
		kv(cfg_kv_daemon, "workers", "513") == -1, "A: workers in 1..512");

	/* B: [memory], [secrets] client, [listen] http_rate/cache_ms, [wal] fsync/segment_mb */
	CHK(kv(cfg_kv_memory, "arena_mb", "16") == 0 && cfg.arena_mb == 16 &&
		kv(cfg_kv_memory, "arena_mb", "15") == -1, "B: arena_mb from 16");
	for (n = 0, i = 0; i < PC_MAX_CLIENT_SECRETS; i++)
		n += kv(cfg_kv_secrets, "client", "s3cret") == 0;
	CHK(n == PC_MAX_CLIENT_SECRETS && cfg.n_client_secrets == PC_MAX_CLIENT_SECRETS &&
		cfg.client_secret[0] && !strcmp(cfg.client_secret[0], "s3cret"),
		"B: client secrets - %d taken", PC_MAX_CLIENT_SECRETS);
	CHK(kv(cfg_kv_secrets, "client", "one-too-many") == -1 &&
		cfg.n_client_secrets == PC_MAX_CLIENT_SECRETS, "B: one more - refused");
	for (i = 0; i < cfg.n_client_secrets; i++)
		free(cfg.client_secret[i]);
	CHK(kv(cfg_kv_listen, "http_rate", "0") == 0 && cfg.http_rate == 0 &&
		kv(cfg_kv_listen, "http_rate", "100001") == -1, "B: http_rate in 0..100000 (0 = off)");
	CHK(kv(cfg_kv_listen, "http_cache_ms", "60000") == 0 && cfg.http_cache_ms == 60000 &&
		kv(cfg_kv_listen, "http_cache_ms", "60001") == -1, "B: http_cache_ms in 0..60000");
	CHK(kv(cfg_kv_wal, "fsync", "always") == 0 && cfg.wal_fsync == 1 &&
		kv(cfg_kv_wal, "fsync", "NO") == 0 && cfg.wal_fsync == 2 &&
		kv(cfg_kv_wal, "fsync", "everysec") == 0 && cfg.wal_fsync == 0,
		"B: fsync always / NO / everysec (words in any case)");
	CHK(kv(cfg_kv_wal, "fsync", "sometimes") == -1 && cfg.wal_fsync == 0, "B: fsync sometimes - refused");
	CHK(kv(cfg_kv_wal, "segment_mb", "1024") == 0 && cfg.wal_segment_mb == 1024 &&
		kv(cfg_kv_wal, "segment_mb", "1025") == -1, "B: segment_mb in 0..1024");

	/* C: [cluster] port; [collection X] buckets_log2 and durable */
	CHK(kv(cfg_kv_cluster, "port", "65535") == 0 && cfg.cl_port == 65535 &&
		kv(cfg_kv_cluster, "port", "0") == -1 && kv(cfg_kv_cluster, "port", "65536") == -1,
		"C: port in 1..65535");
	CHK(kv(cfg_kv_collection, "buckets_log2", "24") == 0 && col.buckets_log2 == 24 &&
		kv(cfg_kv_collection, "buckets_log2", "3") == -1 && col.buckets_log2 == 24,
		"C: [collection X] buckets_log2 in 4..24, on the section's collection");
	CHK(kv(cfg_kv_collection, "durable", "no") == 0 && col.nondurable == 1 &&
		kv(cfg_kv_collection, "durable", "YES") == 0 && col.nondurable == 0 &&
		kv(cfg_kv_collection, "durable", "sometimes") == -1 && col.nondurable == 0,
		"C: [collection X] durable yes|no (G5), a third word refused");

	/* D: [mail] timeout_s; [alerts] cpu_warn_pct; [upgrade] stage_mb */
	CHK(kv(cfg_kv_mail, "timeout_s", "300") == 0 && cfg.mail.timeout_s == 300 &&
		kv(cfg_kv_mail, "timeout_s", "301") == -1, "D: [mail] timeout_s in 1..300");
	CHK(kv(cfg_kv_alerts, "cpu_warn_pct", "80") == 0 && cfg.alerts.cpu_warn == 80 &&
		kv(cfg_kv_alerts, "cpu_warn_pct", "101") == -1 && cfg.alerts.cpu_warn == 80,
		"D: [alerts] cpu_warn_pct in 1..100");
	CHK(kv(cfg_kv_upgrade, "stage_mb", "4096") == 0 && cfg.upgrade.stage_mb == 4096 &&
		kv(cfg_kv_upgrade, "stage_mb", "15") == -1, "D: [upgrade] stage_mb in 16..4096");

	/* E: every section's unknown key - CFG_KV_UNKNOWN, config untouched; junk after a number */
	{
		static int (*const f[])(struct pstate *, char *, char *) = {
			cfg_kv_daemon, cfg_kv_memory, cfg_kv_secrets, cfg_kv_listen,
			cfg_kv_wal, cfg_kv_cluster, cfg_kv_collection, cfg_kv_mail,
			cfg_kv_alerts, cfg_kv_upgrade };
		static struct pc_config before;

		memcpy(&before, &cfg, sizeof cfg);
		for (n = 0, i = 0; i < (int)(sizeof f / sizeof f[0]); i++)
			n += kv(f[i], "no_such_key", "1") == CFG_KV_UNKNOWN;
		CHK(n == 10 && !memcmp(&before, &cfg, sizeof cfg),
			"E: a key no section knows - CFG_KV_UNKNOWN from all ten, the config untouched");
	}
	CHK(kv(cfg_kv_daemon, "workers", "4x") == -1 && kv(cfg_kv_daemon, "workers", "") == -1 &&
		cfg.workers == 512, "E: junk after the number, or no number - refused");

	/* F: [listen] resp_allow / http_allow take a full dotted quad only - inet_aton's short and
	 * octal forms name another network than the one written */
	n = cfg.n_resp_allow;
	CHK(kv(cfg_kv_listen, "resp_allow", "192.168.0.0/16") == 0 && cfg.n_resp_allow == n + 1 &&
		cfg.resp_allow[n].net[0] == 192 && cfg.resp_allow[n].net[1] == 168 &&
		cfg.resp_allow[n].bits == 16, "F: resp_allow 192.168.0.0/16 - taken");
	n = cfg.n_resp_allow;
	CHK(kv(cfg_kv_listen, "resp_allow", "192.168/16") == -1 && cfg.n_resp_allow == n,
		"F: resp_allow 192.168/16 (inet_aton: 192.0.0.168) - refused");
	CHK(kv(cfg_kv_listen, "resp_allow", "10/8") == -1 && cfg.n_resp_allow == n,
		"F: resp_allow 10/8 (inet_aton: 0.0.0.10) - refused");
	CHK(kv(cfg_kv_listen, "resp_allow", "010.0.0.0/8") == -1 && cfg.n_resp_allow == n,
		"F: resp_allow 010.0.0.0/8 (inet_aton: octal, 8.0.0.0) - refused");
	CHK(kv(cfg_kv_listen, "resp_allow", "10.0.0.1 10.0.0.2") == -1 && cfg.n_resp_allow == n,
		"F: resp_allow \"10.0.0.1 10.0.0.2\" (inet_aton: the first alone) - refused");
	n = cfg.n_http_allow;
	CHK(kv(cfg_kv_listen, "http_allow", "172.16/12") == -1 && cfg.n_http_allow == n,
		"F: http_allow 172.16/12 (inet_aton: 172.0.0.16) - refused");
	CHK(kv(cfg_kv_listen, "http_allow", "[::1]/128") == 0 && cfg.n_http_allow == n + 1,
		"F: http_allow [::1]/128 - IPv6 still taken");

	/* G: host bits past the prefix - taken as the network, with one WARNING naming the entry */
	n = cfg.n_resp_allow;
	CHK(kv_logged("resp_allow", "10.1.2.3/8", lg, sizeof lg) == 0 && cfg.n_resp_allow == n + 1 &&
		cfg.resp_allow[n].net[0] == 10 && cfg.resp_allow[n].net[1] == 0 &&
		cfg.resp_allow[n].net[3] == 0 && cfg.resp_allow[n].bits == 8,
		"G: resp_allow 10.1.2.3/8 - taken as 10.0.0.0/8");
	CHK(count_of(lg, "WARNING") == 1 && strstr(lg, "10.1.2.3/8") && strstr(lg, "10.0.0.0/8"),
		"G: and one WARNING names the entry and the network (log: %.200s)", lg);
	n = cfg.n_http_allow;
	CHK(kv_logged("http_allow", "[2001:db8::1]/32", lg, sizeof lg) == 0 && cfg.n_http_allow == n + 1 &&
		cfg.http_allow[n].net[4] == 0 && cfg.http_allow[n].net[15] == 0 &&
		count_of(lg, "WARNING") == 1 && strstr(lg, "2001:db8::1"),
		"G: http_allow [2001:db8::1]/32 - taken masked, one WARNING (log: %.200s)", lg);
	CHK(kv_logged("resp_allow", "10.0.0.0/8", lg, sizeof lg) == 0 && !strstr(lg, "WARNING") &&
		kv_logged("resp_allow", "10.1.2.3", lg, sizeof lg) == 0 && !strstr(lg, "WARNING"),
		"G: 10.0.0.0/8 and a bare 10.1.2.3 - no warning (log: %.200s)", lg);

	/* H: a key told apart from its twin by its whole name, in any case */
	CHK(kv(cfg_kv_listen, "TLS_CERT", "a.pem") == 0 && cfg.tls_cert &&
		!strcmp(cfg.tls_cert, "a.pem") && !cfg.tls_key,
		"H: TLS_CERT = a.pem - stored as the certificate, not the key");
	CHK(kv(cfg_kv_collection, "Upstream_Sentinel_Master", "mymaster") == 0 && col.up_smaster &&
		!strcmp(col.up_smaster, "mymaster") && !col.up_spassword,
		"H: Upstream_Sentinel_Master = mymaster - the master's name, not the sentinel password");

	/* I: keepalive_s within what TCP_KEEPIDLE takes */
	CHK(kv(cfg_kv_listen, "keepalive_s", "32767") == 0 && cfg.keepalive_s == 32767 &&
		kv(cfg_kv_listen, "keepalive_s", "32768") == -1 && cfg.keepalive_s == 32767,
		"I: keepalive_s in 0..32767 - 32768 refused, the socket option would not take it");

	/* J: save - every pair of a token or a refusal, one save line; sampled:N a whole number */
	cfg.n_rdb_rules = -1;
	CHK(kv(cfg_kv_wal, "save", "3600 1 300 100 60 10000") == -1,
		"J: save = 3600 1 300 100 60 10000 (Redis's one-line form) - refused, not cut to 3600 1");
	cfg.n_rdb_rules = -1;
	CHK(kv(cfg_kv_wal, "save", "900 1, 300 10 ") == 0 && cfg.n_rdb_rules == 2 &&
		cfg.rdb_rules[1].secs == 300 && cfg.rdb_rules[1].changes == 10,
		"J: save = 900 1, 300 10 - two rules");
	CHK(kv(cfg_kv_wal, "save", "60 10000") == -1 && cfg.n_rdb_rules == 2,
		"J: a second save line - refused, the first kept");
	cfg.query_log = 0;
	CHK(kv(cfg_kv_daemon, "query_log", "sampled:100x") == -1 && cfg.query_log == 0 &&
		kv(cfg_kv_daemon, "query_log", "sampled:100") == 0 && cfg.query_log == 100,
		"J: query_log = sampled:100x - refused; sampled:100 taken");

	/* K: [cluster] collections - every name under the collection-name rule */
	CHK(kv(cfg_kv_cluster, "collections", "sessions; carts") == -1 && cfg.n_cl_col == 0,
		"K: collections = sessions; carts - refused, no collection of that name");
	free(cfg.cl_collections);
	cfg.cl_collections = NULL;
	CHK(kv(cfg_kv_cluster, "collections", "a234567890123456789012345678901234") == -1 &&
		cfg.n_cl_col == 0, "K: a 34-character name - refused");
	free(cfg.cl_collections);
	cfg.cl_collections = NULL;
	CHK(kv(cfg_kv_cluster, "collections", "sessions, carts_2") == 0 && cfg.n_cl_col == 2 &&
		!strcmp(cfg.cl_colname[1], "carts_2"), "K: collections = sessions, carts_2 - taken");

	/* L: a [secrets] line typed under [listen] or a URL with credentials - refused, not echoed */
	n = cfg.n_listen;
	CHK(kv_logged("http", "tok:en-part2", lg, sizeof lg) == -1 && cfg.n_listen == n &&
		!strstr(lg, "en-part2") && strstr(lg, "ERROR"),
		"L: [listen] http = tok:en-part2 - refused, the value not in the log (log: %.200s)", lg);
	CHK(kv_logged("resp", "s3cret pass-word", lg, sizeof lg) == -1 && cfg.n_listen == n &&
		!strstr(lg, "pass-word") && strstr(lg, "ERROR"),
		"L: [listen] resp = s3cret pass-word - refused, the value not in the log (log: %.200s)", lg);
	CHK(kv_logged_f(cfg_kv_mail, "smtp", "smpt://alerts:mail-pw@10.0.0.5:25", lg, sizeof lg) == -1 &&
		!strstr(lg, "mail-pw") && strstr(lg, "no credentials"),
		"L: smtp = smpt://alerts:mail-pw@... - refused for its credentials, not printed (log: %.200s)", lg);

	/* M: loopback is a numeric loopback address (or localhost), never a name that starts 127. */
	n = cfg.n_listen;
	CHK(kv(cfg_kv_listen, "resp", "127.cache.example.net:6379") == 0 && cfg.n_listen == n + 1 &&
		!cfg.listen[n].loopback, "M: resp = 127.cache.example.net:6379 - a name, not loopback");
	CHK(kv(cfg_kv_listen, "resp", "127.0.0.2:6379") == 0 && cfg.n_listen == n + 2 &&
		cfg.listen[n + 1].loopback, "M: resp = 127.0.0.2:6379 - loopback");
	CHK(kv(cfg_kv_listen, "resp", "10.0.0.1:6379") == 0 && cfg.n_listen == n + 3 &&
		!cfg.listen[n + 2].loopback, "M: resp = 10.0.0.1:6379 - not loopback");

	printf("cfgkvtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
