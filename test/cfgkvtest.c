/* cfgkvtest.c - the config sections (cfgcore.c, cfgcluster.c, cfgops.c;
 * the 0.7.2 split, wave 1): each section's function on its own keys - a
 * value taken into the config, one out of its range or its words
 * refused - and a key of another section answered CFG_KV_UNKNOWN, which
 * config.c turns into "unknown key".
 *
 *   A  [daemon]: allow_create's words, log_events_rate and workers by
 *      their ranges
 *   B  [memory] arena_mb; [secrets] client, up to PC_MAX_CLIENT_SECRETS;
 *      [listen] http_rate and http_cache_ms; [wal] fsync's words and
 *      segment_mb
 *   C  [cluster] port; [collection X] buckets_log2, on the section's
 *      collection, and durable (G5)
 *   D  [mail] timeout_s; [alerts] cpu_warn_pct; [upgrade] stage_mb
 *   E  every section: a key it does not know - CFG_KV_UNKNOWN, the config
 *      untouched; and parse_int's own rule - junk after the number refused
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int main(void)
{
	int i, n;

	ps.path = "cfgkvtest.conf";
	ps.line = 0;                       /* no file line: errors to the log */
	ps.cfg = &cfg;
	ps.col = &col;

	/* A */
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

	/* B */
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

	/* C */
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

	/* D */
	CHK(kv(cfg_kv_mail, "timeout_s", "300") == 0 && cfg.mail.timeout_s == 300 &&
		kv(cfg_kv_mail, "timeout_s", "301") == -1, "D: [mail] timeout_s in 1..300");
	CHK(kv(cfg_kv_alerts, "cpu_warn_pct", "80") == 0 && cfg.alerts.cpu_warn == 80 &&
		kv(cfg_kv_alerts, "cpu_warn_pct", "101") == -1 && cfg.alerts.cpu_warn == 80,
		"D: [alerts] cpu_warn_pct in 1..100");
	CHK(kv(cfg_kv_upgrade, "stage_mb", "4096") == 0 && cfg.upgrade.stage_mb == 4096 &&
		kv(cfg_kv_upgrade, "stage_mb", "15") == -1, "D: [upgrade] stage_mb in 16..4096");

	/* E */
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

	printf("cfgkvtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
