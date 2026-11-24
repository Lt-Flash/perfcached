/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* perfcli - the command-line client (the redis-cli analogue): libperfd is the transport, perfcli
 * owns words -> params, display and the line editor; JSON is only its input syntax and display */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sodium.h>

#include "../src/json.h"
#include "../src/pcaddr.h"
#include "../src/version.h"
#include "../lib/perfd.h"
#include "lineedit.h"

#define REQ_CAP   (1u << 20)

static perfd_t *g_p;
static int g_encrypted;
static int g_quiet;
static int g_pretty;

/* ---- pretty printing --------------------------------------------------- */

/* streaming re-indenter for VALID compact JSON (which replies are):
 * no token array, any size, strings copied verbatim */
static void pretty_print(const char *s, size_t n)
{
	size_t i = 0;
	int depth = 0, in_str = 0, esc = 0, j;

	for (i = 0; i < n; i++) {
		char c = s[i];

		if (in_str) {
			putchar(c);
			if (esc)
				esc = 0;
			else if (c == '\\')
				esc = 1;
			else if (c == '"')
				in_str = 0;
			continue;
		}
		switch (c) {
		case '"':
			in_str = 1;
			putchar(c);
			break;
		case '{': case '[': {
			char close = c == '{' ? '}' : ']';
			size_t k = i + 1;

			while (k < n && (s[k] == ' ' || s[k] == '\t'))
				k++;
			if (k < n && s[k] == close) {
				putchar(c);
				putchar(close);
				i = k;
				break;
			}
			putchar(c);
			depth++;
			putchar('\n');
			for (j = 0; j < depth; j++)
				fputs("  ", stdout);
			break; }
		case '}': case ']':
			depth--;
			putchar('\n');
			for (j = 0; j < depth; j++)
				fputs("  ", stdout);
			putchar(c);
			break;
		case ',':
			putchar(c);
			putchar('\n');
			for (j = 0; j < depth; j++)
				fputs("  ", stdout);
			break;
		case ':':
			fputs(": ", stdout);
			break;
		case ' ': case '\t': case '\n': case '\r':
			break;                 /* re-flowed */
		default:
			putchar(c);
		}
	}
	putchar('\n');
}

/* ---- request building -------------------------------------------------- */

/* what we dialled with, so a session that outlived its server can be redialled */
static const char *g_dial_host, *g_dial_port, *g_dial_unix;
static perfd_opts *g_dial_opts;

/* redial once after the connection died: libperfd never reconnects by itself, and an
 * interactive session must not stay attached to a dead handle; 1 = a fresh connection */
static int redial(void)
{
	perfd_t *n;

	if (!g_dial_opts)
		return 0;
	n = g_dial_unix ? perfd_connect_unix(g_dial_unix, g_dial_opts)
	                : perfd_connect(g_dial_host, atoi(g_dial_port),
	                                g_dial_opts);
	if (!n)
		return 0;
	perfd_free(g_p);
	g_p = n;
	fprintf(stderr, "perfcli: connection was lost - reconnected to %s%s%s\n",
		g_dial_unix ? g_dial_unix : g_dial_host,
		g_dial_unix ? "" : ":", g_dial_unix ? "" : g_dial_port);
	return 1;
}

/* the privilege secret this session started with, so a redial raises privilege again
 * rather than the next DDL being refused */
static const char *g_enable_secret;

static int raise_privilege(void)
{
	char params[512];
	char *res;
	int n, ok;

	if (!g_enable_secret)
		return 0;
	n = snprintf(params, sizeof params, "{\"secret\":\"%s\"}",
		g_enable_secret);
	if (n < 0 || (size_t)n >= sizeof params)
		return -1;
	res = perfd_command(g_p, "enable", params);
	if (!res)
		return -1;
	ok = strstr(res, "\"privileged\":true") != NULL;
	free(res);
	return ok ? 0 : -1;
}

/* verbs a redial must NOT send again: a request lost with its connection may have run, and
 * these are not safe twice (the privileged verbs, less enable/disable) */
static int unsafe_twice(const char *m)
{
	static const char *const once[] = { "fleetstop", "create", "drop",
		"rename", "resize", "restore", "mailtest", "map", "unmap", "forget",
		NULL };
	int i;

	for (i = 0; once[i]; i++)
		if (!strcmp(m, once[i]))
			return 1;
	return 0;
}

/* a jget's fragment arrives as JSON text rendered as an escaped string: put it back raw for
 * the reader - only the "value" member moves, the rest is copied as rendered */
static char *splice_document(char *res)
{
	size_t n = strlen(res);
	int maxt = (int)(n / 2) + 8, ntok, tv, i, child = 0;
	struct pc_jtok *t = malloc(sizeof *t * (size_t)maxt);
	char *doc, *out;
	struct pc_jw w;
	int dl;

	if (!t)
		return res;
	ntok = pc_json_parse(res, n, t, maxt);
	tv = ntok > 0 && t[0].type == PC_J_OBJ ? pc_json_get(res, t, ntok, 0, "value") : -1;
	if (tv < 0 || t[tv].type != PC_J_STR) {
		free(t);
		return res;
	}
	doc = malloc(n + 1);
	out = malloc(2 * n + 16);
	if (!doc || !out) {
		free(t); free(doc); free(out);
		return res;
	}
	dl = pc_json_unescape(res, &t[tv], doc, n + 1);
	if (dl < 0) {
		free(t); free(doc); free(out);
		return res;
	}
	pc_jw_init(&w, out, 2 * n + 16);
	pc_jw_lit(&w, "{");
	for (i = 1; i < ntok; i++) {
		int tk = i, tval = i + 1;

		if (t[tk].parent != 0)
			continue;              /* inside a member's subtree */
		if (child++ & 1)
			continue;              /* a value: the members alternate key, value */
		if (w.len > 1)
			pc_jw_lit(&w, ",");
		pc_jw_raw(&w, res + t[tk].start - 1, (size_t)(t[tk].end - t[tk].start) + 2);
		pc_jw_lit(&w, ":");
		if (tval == tv) {
			pc_jw_raw(&w, doc, (size_t)dl);
		} else {
			int a = t[tval].type == PC_J_STR ? t[tval].start - 1 : t[tval].start;
			int b = t[tval].type == PC_J_STR ? t[tval].end + 1 : t[tval].end;

			pc_jw_raw(&w, res + a, (size_t)(b - a));
		}
	}
	pc_jw_lit(&w, "}");
	free(t);
	free(doc);
	if (w.overflow) {
		free(out);
		return res;
	}
	out[w.len] = 0;
	free(res);
	return out;
}

static int do_command(const char *method, const char *params_json)
{
	char *res = perfd_command(g_p, method, params_json);

	if (!res && perfd_state(g_p) == PERFD_ST_FAILED && redial()) {
		(void)raise_privilege();   /* a new link starts unprivileged */
		if (unsafe_twice(method)) {
			fprintf(stderr, "perfcli: '%s' was sent before the "
				"connection was lost, so it may already have "
				"run - not sending it again; check the fleet "
				"before repeating it\n", method);
			return 1;
		}
		res = perfd_command(g_p, method, params_json);
	}

	if (!res) {
		fprintf(stderr, "%s\n", perfd_error(g_p));
		return 1;
	}
	if (!strcmp(method, "jget"))
		res = splice_document(res);
	if (g_pretty)
		pretty_print(res, strlen(res));
	else
		printf("%s\n", res);
	free(res);
	return 0;
}

/* a raw {"method":..,"params":..} request (-j / a '{' line): the method and params text ride
 * the library like any other command */
static int raw_command(const char *json)
{
	static struct pc_jtok toks[4096];
	char method[256], *params = NULL;
	int ntok, tm, tp, ml, rc;

	ntok = pc_json_parse(json, strlen(json), toks, 4096);
	if (ntok < 1 || toks[0].type != PC_J_OBJ) {
		fprintf(stderr, "perfcli: not a JSON object\n");
		return 1;
	}
	tm = pc_json_get(json, toks, ntok, 0, "method");
	if (tm < 0 || toks[tm].type != PC_J_STR) {
		fprintf(stderr, "perfcli: no \"method\" in request\n");
		return 1;
	}
	ml = pc_json_unescape(json, &toks[tm], method, sizeof method - 1);
	if (ml < 0) {
		fprintf(stderr, "perfcli: bad method string\n");
		return 1;
	}
	method[ml] = 0;
	tp = pc_json_get(json, toks, ntok, 0, "params");
	if (tp >= 0) {
		int a = toks[tp].start - (toks[tp].type == PC_J_STR ? 1 : 0);
		int b = toks[tp].end + (toks[tp].type == PC_J_STR ? 1 : 0);

		params = malloc((size_t)(b - a) + 1);
		if (!params)
			return 1;
		memcpy(params, json + a, (size_t)(b - a));
		params[b - a] = 0;
	}
	rc = do_command(method, params);
	free(params);
	return rc;
}

/* tokenize a command line: whitespace-separated, double quotes group,
 * backslash escapes inside quotes; -1 when it has more than @max words */
static int split_words(char *s, char **argv, int max)
{
	int n = 0;

	while (*s) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		if (n >= max)
			return -1;
		if (*s == '"') {
			char *w = ++s, *o = s;

			while (*s && *s != '"') {
				if (*s == '\\' && s[1])
					s++;
				*o++ = *s++;
			}
			if (*s)
				s++;
			*o = 0;
			argv[n++] = w;
		} else {
			argv[n++] = s;
			while (*s && *s != ' ' && *s != '\t')
				s++;
			if (*s)
				*s++ = 0;
		}
	}
	return n;
}

static void jw_kv_str(struct pc_jw *w, const char *k, const char *v)
{
	pc_jw_lit(w, ",\"");
	pc_jw_lit(w, k);
	pc_jw_lit(w, "\":");
	pc_jw_str(w, v, strlen(v));
}

static void jw_kv_ll(struct pc_jw *w, const char *k, long long v)
{
	pc_jw_lit(w, ",\"");
	pc_jw_lit(w, k);
	pc_jw_lit(w, "\":");
	pc_jw_i64(w, v);
}

/* upgrade-stage: the tarball and <tarball>.sig go to the node's upgrade/staged/ 192 KB at a
 * time; the node verifies the whole file, this only reports progress */
static void stage_progress(void *ctx, size_t done, size_t total)
{
	(void)ctx;
	if (!g_quiet)
		fprintf(stderr, "\rupgrade-stage: %zu of %zu bytes%s", done, total,
			done == total ? "\n" : "");
}

/* a string field of a flat reply: "key":"value" - enough for the few
 * fields fleet-status reads (version, rev, state, install_mode) */
static int jfield(const char *res, const char *key, char *out, size_t cap)
{
	char pat[64];
	const char *s, *e;
	size_t n;

	snprintf(pat, sizeof pat, "\"%s\":\"", key);
	s = res ? strstr(res, pat) : NULL;
	if (!s)
		return -1;
	s += strlen(pat);
	e = strchr(s, '"');
	if (!e || (n = (size_t)(e - s)) >= cap)
		return -1;
	memcpy(out, s, n);
	out[n] = 0;
	return 0;
}

/* the staged builds' versions, newest first, as "0.6.6 0.6.5" */
static void staged_versions(const char *res, char *out, size_t cap)
{
	const char *p = res ? strstr(res, "\"staged\":[") : NULL;
	/* the array ends at the key after it: a signer's text ("trusted_keys[0] ...")
	 * carries a ']' of its own, which cut the list to one entry */
	const char *end = p ? strstr(p, "\"upload\":") : NULL;
	size_t o = 0;

	out[0] = 0;
	if (!p || !end)
		return;
	while ((p = strstr(p, "\"version\":\"")) && p < end) {
		const char *e;
		size_t n;

		p += 11;
		e = strchr(p, '"');
		if (!e)
			break;
		n = (size_t)(e - p);
		if (o + n + 2 >= cap)
			break;
		if (o)
			out[o++] = ' ';
		memcpy(out + o, p, n);
		o += n;
		out[o] = 0;
		p = e;
	}
}

/* fleet-status: this node and every member it knows, each dialled as the upgrade leader sees
 * it; unprivileged, needs a handle that learned the fleet (the default; -u learns too) */
static int fleet_status_cmd(void)
{
	int n = perfd_member_count(g_p), i, bad = 0;

	printf("%-5s %-24s %-9s %-10s %-8s %s\n", "node", "member", "version",
		"state", "install", "staged (newest first)");
	for (i = -1; i < n; i++) {
		char addr[64] = "", why[160] = "", ver[32] = "?", mode[16] = "?";
		char stg[200] = "", line[160];
		const char *state;
		perfd_t *h;
		int port = 0, node = 0, own = i < 0;
		char *r;

		if (own) {
			snprintf(addr, sizeof addr, "%s", g_dial_unix ? g_dial_unix
				: g_dial_host ? g_dial_host : "?");
			port = g_dial_port ? atoi(g_dial_port) : 0;
			node = perfd_active_node(g_p);
			state = "this";
			h = g_p;
		} else {
			if (perfd_member_info(g_p, i, addr, sizeof addr, &port, &node,
			        NULL) != 0)
				continue;
			if (node && node == perfd_active_node(g_p))
				continue;          /* the node this handle sits on: shown above */
			state = perfd_member_state(g_p, i, why, sizeof why);
			h = g_dial_opts ? perfd_connect(addr, port, g_dial_opts) : NULL;
		}
		if (h) {
			r = perfd_stats(h, NULL);
			if (r) {
				jfield(r, "version", ver, sizeof ver);
				perfd_free_value(r);
			}
			r = perfd_upgrade_status(h);
			if (r) {
				jfield(r, "install_mode", mode, sizeof mode);
				staged_versions(r, stg, sizeof stg);
				perfd_free_value(r);
			} else {
				snprintf(stg, sizeof stg, "(%s)", perfd_error(h));
			}
			if (!own)
				perfd_free(h);
		} else if (!own) {
			bad++;
			snprintf(stg, sizeof stg, "unreachable: %s", perfd_error(NULL));
		}
		snprintf(line, sizeof line, "%s:%d", addr, port);
		printf("%-5d %-24s %-9s %-10s %-8s %s\n", node, line, ver, state, mode,
			stg[0] ? stg : "-");
	}
	return bad ? 1 : 0;
}

#include "perfcli-upgrade.inc"          /* perfcli upgrade - the roll driver */

static int upgrade_stage_cmd(int argc, char **argv)
{
	char sp[1100];
	char *res;
	int have_sig = 0;

	if (argc < 3) {
		fprintf(stderr, "usage: upgrade-stage <tarball> <version>\n");
		return 2;
	}
	/* the staging is libperfd's (perfd_upgrade_stage_file); perfcli warns about an
	 * unsigned artifact before the bytes go */
	snprintf(sp, sizeof sp, "%s.sig", argv[1]);
	if (access(sp, R_OK) != 0)
		fprintf(stderr, "perfcli: no %s - sending it UNSIGNED (the node "
			"refuses that unless [upgrade] allow_unsigned = yes)\n", sp);
	res = perfd_upgrade_stage_file(g_p, argv[1], argv[2], stage_progress,
		NULL, &have_sig);
	if (!res) {
		fprintf(stderr, "perfcli: %s\n", perfd_error(g_p));
		return 1;
	}
	printf("%s\n", res);
	perfd_free_value(res);
	return 0;
}

/* client-side commands that never reach the daemon.  Returns 1 when
 * @argv was handled locally. */
static int local_cmd(int argc, char **argv)
{
	if (strcmp(argv[0], "pretty"))
		return 0;
	if (argc >= 2 && !strcmp(argv[1], "off"))
		g_pretty = 0;
	else if (argc >= 2 && !strcmp(argv[1], "on"))
		g_pretty = 1;
	else
		g_pretty = !g_pretty;
	fprintf(stderr, "pretty: %s\n", g_pretty ? "on" : "off");
	return 1;
}

/* the command word as typed, the method as the daemon spells it */
static const char *canon(const char *m)
{
	return strcmp(m, "reset-stats") ? m : "reset_stats";
}

/* the PARAMS object ("{...}") for a word command, the method being argv[0]: its length,
 * 0 for no params, -1 on a usage error, -2 handled locally */
static int build(char *out, size_t cap, int argc, char **argv)
{
	struct pc_jw w;
	const char *m = argv[0];
	int i;

	pc_jw_init(&w, out, cap);

#define NEED(n) do { if (argc < (n) + 1) goto usage; } while (0)
#define P_OPEN()  pc_jw_lit(&w, "{\"col\":"), \
	pc_jw_str(&w, argv[1], strlen(argv[1]))
#define P_KEY()   jw_kv_str(&w, "key", argv[2])

	if (!strcmp(m, "ping") || !strcmp(m, "save") ||
	    !strcmp(m, "load") || !strcmp(m, "sync") ||
	    !strcmp(m, "collections") || !strcmp(m, "reset_stats") ||
	    !strcmp(m, "reset-stats") || !strcmp(m, "mailtest") ||
	    !strcmp(m, "upgrade_status") || !strcmp(m, "reload") ||
	    !strcmp(m, "maps") ||
	    !strcmp(m, "master_auto")) {
		return 0;                      /* no params */
	} else if (!strcmp(m, "updrain") ||    /* needs privilege */
	    !strcmp(m, "upfill")) {
		NEED(1);
		P_OPEN();
		if (argc > 2 && !strcmp(argv[2], "stop"))
			pc_jw_lit(&w, ",\"stop\":true");
		else if (argc > 2 && !strcmp(m, "upfill") &&
		    !strcmp(argv[2], "check"))
			pc_jw_lit(&w, ",\"check\":true");
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "fleetstop")) {  /* needs privilege */
		if (argc < 2)
			return 0;
		/* how long to wait for the peers, ms (the daemon
		 * clamps it to 1-120 s) */
		pc_jw_lit(&w, "{\"timeout_ms\":");
		pc_jw_i64(&w, atoll(argv[1]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "forget")) {
		NEED(1);
		pc_jw_lit(&w, "{\"node\":");
		pc_jw_i64(&w, atoll(argv[1]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "master_prefer")) {
		NEED(1);
		pc_jw_lit(&w, "{\"node\":");
		pc_jw_i64(&w, atoll(argv[1]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "probe")) {
		if (argc < 2)
			return 0;
		pc_jw_lit(&w, "{\"secs\":");
		pc_jw_i64(&w, atoll(argv[1]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "create")) {
		/* create <col> [buckets_log2] */
		NEED(1);
		pc_jw_lit(&w, "{\"col\":");
		pc_jw_str(&w, argv[1], strlen(argv[1]));
		if (argc >= 3) {
			pc_jw_lit(&w, ",\"buckets_log2\":");
			pc_jw_i64(&w, atoll(argv[2]));
		}
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "drop")) {
		/* drop <col> [force] */
		NEED(1);
		pc_jw_lit(&w, "{\"col\":");
		pc_jw_str(&w, argv[1], strlen(argv[1]));
		if (argc >= 3 && (!strcmp(argv[2], "force") ||
		        !strcmp(argv[2], "yes") || !strcmp(argv[2], "1")))
			pc_jw_lit(&w, ",\"force\":true");
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "rename")) {
		/* rename <col> <to> */
		NEED(2);
		pc_jw_lit(&w, "{\"col\":");
		pc_jw_str(&w, argv[1], strlen(argv[1]));
		pc_jw_lit(&w, ",\"to\":");
		pc_jw_str(&w, argv[2], strlen(argv[2]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "map") || !strcmp(m, "unmap")) {
		/* map <index> <col> / unmap <index>: the index a whole decimal, so swapped
		 * words (`map mycol 0`) are refused here rather than sent as index 0 */
		const char *d;

		NEED(!strcmp(m, "map") ? 2 : 1);
		for (d = argv[1]; *d >= '0' && *d <= '9'; d++)
			;
		if (!argv[1][0] || *d || d - argv[1] > 4)
			goto usage;
		pc_jw_lit(&w, "{\"index\":");
		pc_jw_i64(&w, atoll(argv[1]));
		if (!strcmp(m, "map")) {
			pc_jw_lit(&w, ",\"col\":");
			pc_jw_str(&w, argv[2], strlen(argv[2]));
		}
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "autoscale")) {
		/* autoscale <col> <auto|floor|warn|off> */
		NEED(2);
		pc_jw_lit(&w, "{\"col\":");
		pc_jw_str(&w, argv[1], strlen(argv[1]));
		pc_jw_lit(&w, ",\"mode\":");
		pc_jw_str(&w, argv[2], strlen(argv[2]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "resize")) {
		/* resize <col> <buckets_log2> */
		NEED(2);
		pc_jw_lit(&w, "{\"col\":");
		pc_jw_str(&w, argv[1], strlen(argv[1]));
		pc_jw_lit(&w, ",\"buckets_log2\":");
		pc_jw_i64(&w, atoll(argv[2]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "name")) {
		NEED(1);
		pc_jw_lit(&w, "{\"name\":");
		pc_jw_str(&w, argv[1], strlen(argv[1]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "stats")) {
		if (argc < 2)
			return 0;
		pc_jw_lit(&w, "{\"col\":");
		pc_jw_str(&w, argv[1], strlen(argv[1]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "get") || !strcmp(m, "del") ||
	           !strcmp(m, "exists") || !strcmp(m, "ttl")) {
		NEED(2);
		P_OPEN(); P_KEY();
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "set")) {
		NEED(3);
		P_OPEN(); P_KEY();
		jw_kv_str(&w, "value", argv[3]);
		if (argc >= 5)
			jw_kv_ll(&w, "ttl", atoll(argv[4]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "expire")) {
		NEED(3);
		P_OPEN(); P_KEY();
		jw_kv_ll(&w, "ttl", atoll(argv[3]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "add") || !strcmp(m, "sub")) {
		NEED(2);
		P_OPEN(); P_KEY();
		if (argc >= 4)
			jw_kv_ll(&w, "by", atoll(argv[3]));
		if (argc >= 5)
			jw_kv_ll(&w, "ttl", atoll(argv[4]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "keys")) {
		/* the collection is a glob, absent = every collection, grouped; no cursor on
		 * purpose (`scan` is the cursored walk); the limit is PER COLLECTION */
		if (argc < 2) {
			pc_jw_lit(&w, "{\"col\":\"*\"}");
		} else {
			P_OPEN();
			if (argc >= 3)
				jw_kv_str(&w, "match", argv[2]);
			if (argc >= 4)
				jw_kv_ll(&w, "limit", atoll(argv[3]));
			pc_jw_lit(&w, "}");
		}
	} else if (!strcmp(m, "scan")) {
		NEED(1);
		P_OPEN();
		if (argc >= 3)
			jw_kv_ll(&w, "cursor", atoll(argv[2]));
		if (argc >= 4)
			jw_kv_str(&w, "match", argv[3]);
		if (argc >= 5)
			jw_kv_ll(&w, "count", atoll(argv[4]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "mget")) {
		NEED(2);
		P_OPEN();
		pc_jw_lit(&w, ",\"keys\":[");
		for (i = 2; i < argc; i++) {
			if (i > 2)
				pc_jw_lit(&w, ",");
			pc_jw_str(&w, argv[i], strlen(argv[i]));
		}
		pc_jw_lit(&w, "]}");
	} else if (!strcmp(m, "mset")) {
		NEED(3);
		if ((argc - 2) % 2)
			goto usage;
		P_OPEN();
		pc_jw_lit(&w, ",\"items\":[");
		for (i = 2; i + 1 < argc; i += 2) {
			if (i > 2)
				pc_jw_lit(&w, ",");
			pc_jw_lit(&w, "{\"key\":");
			pc_jw_str(&w, argv[i], strlen(argv[i]));
			pc_jw_lit(&w, ",\"value\":");
			pc_jw_str(&w, argv[i + 1], strlen(argv[i + 1]));
			pc_jw_lit(&w, "}");
		}
		pc_jw_lit(&w, "]}");
	} else if (!strcmp(m, "jget") || !strcmp(m, "jdel")) {
		NEED(2);
		P_OPEN(); P_KEY();
		if (argc >= 4)
			jw_kv_str(&w, "path", argv[3]);
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "jset")) {
		NEED(4);
		P_OPEN(); P_KEY();
		jw_kv_str(&w, "path", argv[3]);
		pc_jw_lit(&w, ",\"val\":");
		pc_jw_raw(&w, argv[4], strlen(argv[4]));   /* RAW JSON */
		if (argc >= 6)
			jw_kv_ll(&w, "ttl", atoll(argv[5]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "jarrappend")) {
		NEED(4);
		P_OPEN(); P_KEY();
		jw_kv_str(&w, "path", argv[3]);
		pc_jw_lit(&w, ",\"val\":");
		pc_jw_raw(&w, argv[4], strlen(argv[4]));   /* RAW JSON */
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "jincr")) {
		NEED(3);
		P_OPEN(); P_KEY();
		jw_kv_str(&w, "path", argv[3]);
		if (argc >= 5)
			jw_kv_ll(&w, "by", atoll(argv[4]));
		pc_jw_lit(&w, "}");
	} else if (!strcmp(m, "help")) {
		printf("commands:\n"
		  "  ping | stats [col] | save | sync | load | reset-stats\n"
		  "  name <name> - this connection's name on the node (clients)\n"
		  "  create <col> [buckets_log2] | drop <col> [force]\n"
		  "  resize <col> <buckets_log2> | rename <col> <to>\n"
	  "  map <index> <col> | unmap <index> | maps - the RESP door's DB\n"
	  "      index map, changed on the running fleet: every connection\n"
	  "      sees it on its next command, every member takes it, it\n"
	  "      survives a restart and outranks resp_collections; unmap\n"
	  "      returns the index to what resp_collections says; maps lists\n"
	  "      each index and where it came from; map/unmap need -E\n"
	  "  autoscale <col> <auto|floor|warn|off> - how the table is sized:\n"
	  "      auto grows and shrinks it, floor grows it but never shrinks\n"
	  "      it below the size it was configured, created or resized at,\n"
	  "      warn only says what auto would do, off leaves it alone;\n"
	  "      needs -E\n"
	  "  updrain <col> [stop] - read every key the collection's\n"
	  "      read-through upstream holds under its allow-list and take\n"
	  "      over what the fleet does not have yet (progress: stats,\n"
	  "      upstream.drain); run it on ONE node; needs -E\n"
	  "  upfill <col> [stop|check] - on a write-through collection, write\n"
	  "      the keys the fleet owned before the switch to the upstream's\n"
	  "      master, unless Redis has them - then Redis's value replaces\n"
	  "      the fleet's on every member; check compares each with Redis\n"
	  "      and changes nothing (progress: stats,\n"
	  "      upstream.write_through.upfill); run it on ONE node; needs -E\n"
	  "  upgrade-stage <tarball> <version> - send a build and its .sig\n"
	  "      to this node's upgrade/staged/; the node verifies it whole\n"
	  "      (sha256, signature) before it keeps it; needs -E\n"
	  "  upgrade_status - the builds staged here, an upload in progress\n"
	  "  upgrade plan <tarball> version=<v> [switch_cmd=<prog>] [journal=<f>]\n"
	  "      [ready_s=<n>] [gate_s=<n>]; then upgrade start|resume|status|\n"
	  "      abort|rollback [journal=<f>] - the roll driver, one-shot only:\n"
	  "      stages the build on every member, then one node at a time,\n"
	  "      members first and the master last, with the gates between;\n"
	  "      stops at the first failure and reverts that node;\n"
	  "      start/resume/rollback need -E\n"
	  "  upgrade commit epoch=<n> - the fleet's wire epoch, raised on the\n"
	  "      master once every member speaks <n>; needs -E\n"
	  "  upgrade-revert [force] - the helper switches this node back to the\n"
	  "      prefix it kept before its last switch (upgrade_status.previous);\n"
	  "      needs -E, one-shot only; force skips the running-build and\n"
	  "      helper checks\n"
	  "  fleet-status - this node and every member, each dialled: version,\n"
	  "      state, install mode (package/prefix/local), the builds staged\n"
	  "  mailtest - send one test mail through this node's [mail] and\n"
	  "      print the SMTP server's answer; needs -E\n"
	  "  reload - read this node's config file again and apply what a\n"
	  "      running daemon can take (allow-lists, secrets, log_level,\n"
	  "      slowlog_usec); refused whole, naming the keys, when anything\n"
	  "      else changed; needs -E.  `systemctl reload` does the same\n"
	  "  fleetstop [timeout_ms] - stop EVERY node cleanly (each\n"
	  "      snapshots on the way down); needs -E.  This node goes\n"
	  "      last, after waiting up to timeout_ms (30000) for its\n"
	  "      peers: stopped (said goodbye), still_up, or lost (left\n"
	  "      without a goodbye)\n"
	  "  forget <node> - a member removed for good: its reserved id\n"
	  "      released on this node (run it on each); needs -E\n"
	  "  master_prefer <node> - on the master: hand mastership to that\n"
	  "      member once it is the synced standby; needs -E.\n"
	  "      master_auto cancels one not yet taken\n"
	  "  probe [secs] - re-measure WAL storage iops/throughput\n"
	  "      (blocks for the probe; perturbs live fsync latency)\n"
		  "  get|del|exists|ttl <col> <key>\n"
		  "  set <col> <key> <value> [ttl]\n"
		  "  expire <col> <key> <ttl>\n"
		  "  add|sub <col> <key> [by] [ttl]\n"
		  "  keys [col-glob] [pattern] [limit]   (limit is PER COLLECTION, default 100)\n"
		  "  collections                         (name, mode, entries, buckets)\n"
		  "  exit | quit\n"
		  "  scan <col> [cursor] [match] [count]\n"
		  "  mget <col> <key>...\n"
		  "  mset <col> <key> <value>...\n"
		  "  jget|jdel <col> <key> [path]\n"
		  "  jset <col> <key> <path> <raw-json> [ttl]\n"
		  "  jarrappend <col> <key> <path> <raw-json>\n"
		  "  jincr <col> <key> <path> [by]\n"
		  "values with spaces: \"double quotes\"; jset value is raw "
		  "JSON\n"
		  "  pretty [on|off] - toggle pretty-printed results\n"
		  "editing: arrows + history, Ctrl-R search, "
		  "Ctrl-A/E/B/F/W/U/K/L\n");
		return -2;
	} else {
		fprintf(stderr, "perfcli: unknown command '%s' (try help)\n",
			m);
		return -1;
	}
	if (w.overflow || w.len >= cap) {
		fprintf(stderr, "perfcli: request too large\n");
		return -1;
	}
	out[w.len] = 0;                    /* perfd_command takes a C string */
	return (int)w.len;

usage:
	fprintf(stderr, "perfcli: bad arguments for '%s' (try help)\n", m);
	return -1;
}

/* ---- secret input ------------------------------------------------------ */

static char *prompt_secret(const char *what, char *buf, size_t cap)
{
	struct termios t, t0;
	char *nl;

	fprintf(stderr, "%s secret: ", what);
	tcgetattr(0, &t0);
	t = t0;
	t.c_lflag &= ~(tcflag_t)ECHO;
	tcsetattr(0, TCSANOW, &t);
	if (!fgets(buf, (int)cap, stdin))
		buf[0] = 0;
	tcsetattr(0, TCSANOW, &t0);
	fprintf(stderr, "\n");
	nl = strchr(buf, '\n');
	if (nl)
		*nl = 0;
	return buf[0] ? buf : NULL;
}

/* ---- usage, and a default target that is not a guess ------------------ */

static void usage(FILE *f)
{
	fprintf(f,
		"usage: perfcli [-h host] [-p port] [-u /unix.sock] [-a secret | -A] [-N name]\n"
		"               [-E secret | -e] [--encrypt] [-q] [-P]\n"
		"               [-j '{\"method\":..,\"params\":..}' | <command words...>]\n"
		"  -h host    daemon address (default: the local daemon's config, else 127.0.0.1)\n"
		"  -p port    daemon port (default 6479)\n"
		"  -u path    unix socket instead of tcp\n"
		"  -a secret  client secret ([secrets] client in the daemon's config)\n"
		"  -N name    this connection's name on the node (perfcli@<host>) - shown\n"
		"             by clients, carried by the enable/sweep/reset log lines\n"
		"  -E secret  raise privilege on this connection ([secrets] enable) -\n"
		"             create/drop/resize/rename, map/unmap and restore need it\n"
		"  -e         prompt for the enable secret\n"
		"  --encrypt  Noise-encrypt the connection (-E and -e imply it; the\n"
		"             default is authenticated plaintext where the node offers it)\n"
		"  -A         prompt for the secret\n"
		"  -q         quiet: result to stdout only, no prompt or banner\n"
		"  -P         pretty-print results ('pretty on|off' inside the session)\n"
		"  -j json    send one raw {\"method\":..,\"params\":..} request\n"
		"  -V         version\n"
		"options go BEFORE the command; after it they are the command's arguments.\n"
		"environment: PERFCLI_AUTH (secret), PERFCLI_ENABLE (-E), PERFCLI_ENCRYPT\n"
		"(--encrypt), PERFCACHED_CONF (config to read the target and secret from;\n"
		"also tried: /etc/perfcached/perfcached.conf,\n"
		"/opt/perfcached/etc/perfcached.conf, /etc/perfcached.conf)\n");
}

/* with no target, the daemon here usually binds the host's own address, not loopback: the
 * config it runs from says where it listens and what the client secret is */
static char cf_host[256], cf_port[16], cf_secret[256], cf_path[512];

static int local_defaults(const char **host, const char **port,
		const char **secret)
{
	/* /etc/perfcached.conf too - where a release tarball's install keeps it */
	static const char *cands[] = { NULL, "/etc/perfcached/perfcached.conf",
		"/opt/perfcached/etc/perfcached.conf", "/etc/perfcached.conf" };
	char line[1024], sect[64] = "";
	FILE *f = NULL;
	size_t i;
	int got = 0;

	cands[0] = getenv("PERFCACHED_CONF");
	for (i = 0; i < sizeof cands / sizeof cands[0] && !f; i++) {
		if (!cands[i] || !cands[i][0])
			continue;
		f = fopen(cands[i], "r");
		if (f)
			snprintf(cf_path, sizeof cf_path, "%s", cands[i]);
	}
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		char *s = line, *e, *v;

		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '[') {
			snprintf(sect, sizeof sect, "%.*s",
				(int)strcspn(s + 1, "]\n"), s + 1);
			continue;
		}
		if (*s == '#' || *s == ';')
			continue;
		e = strchr(s, '=');
		if (!e)
			continue;
		v = e + 1;
		while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
			e--;
		*e = 0;
		while (*v == ' ' || *v == '\t')
			v++;
		v[strcspn(v, "\r\n")] = 0;
		if (*v == '"' && (e = strrchr(v + 1, '"')) != NULL) {
			*e = 0;                /* as the daemon reads it: quotes keep a '#' */
			v++;
		} else {
			v[strcspn(v, "#")] = 0;
			for (e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t'); )
				*--e = 0;
		}
		if (!strcmp(sect, "listen") && !strcmp(s, "tcp") && !cf_host[0]) {
			char h[256];
			int pn;

			if (pc_hostport_split(v, h, sizeof h, &pn) != 0)
				continue;
			/* a wildcard bind is reachable on loopback ([::] on ::1) */
			snprintf(cf_host, sizeof cf_host, "%s",
				!strcmp(h, "0.0.0.0") ? "127.0.0.1" :
				!strcmp(h, "::") ? "::1" : h);
			snprintf(cf_port, sizeof cf_port, "%d", pn);
			*host = cf_host; *port = cf_port;
			got |= 1;
		} else if (!strcmp(sect, "secrets") && !strcmp(s, "client") &&
		        !*secret && !cf_secret[0]) {
			snprintf(cf_secret, sizeof cf_secret, "%s", v);
			*secret = cf_secret;
			got |= 2;
		}
	}
	fclose(f);
	return got;
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *host = "127.0.0.1", *port = "6479", *unixp = NULL;
	const char *secret = getenv("PERFCLI_AUTH"), *rawjson = NULL;
	const char *conn_name = NULL;          /* -N */
	const char *enable_secret = NULL;
	static char auth_buf[512], enable_buf[512];   /* typed at -A, -e */
	int prompt_enable = 0, encrypt = getenv("PERFCLI_ENCRYPT") != NULL;
	char req[REQ_CAP];
	int i, rc = 0, prompt_auth = 0, explicit_target = 0;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		int last = i + 1 >= argc;

		/* -h is the HOST flag and also the first thing anyone types for help: -h with
		 * nothing after it, or with another option after it, IS help */
		if (!strcmp(a, "--help") || !strcmp(a, "-?") ||
		    (!strcmp(a, "-h") && (last || argv[i + 1][0] == '-'))) {
			usage(stdout);
			return 0;
		} else if (!strcmp(a, "-h")) {
			host = argv[++i]; explicit_target = 1;
		} else if ((!strcmp(a, "-p") || !strcmp(a, "-u") ||
		        !strcmp(a, "-a") || !strcmp(a, "-j") || !strcmp(a, "-N")) && last) {
			fprintf(stderr, "perfcli: %s needs a value (try --help)\n", a);
			return 2;
		} else if (!strcmp(a, "-p")) {
			port = argv[++i]; explicit_target = 1;
		} else if (!strcmp(a, "-u")) {
			unixp = argv[++i]; explicit_target = 1;
		} else if (!strcmp(a, "-a"))
			secret = argv[++i];
		else if (!strcmp(a, "-N"))
			conn_name = argv[++i];
		else if (!strcmp(a, "-E")) {
			/* raise privilege on THIS connection after the handshake: the bit dies
			 * with it, so a separate `perfcli enable` cannot help a one-shot */
			if (last) {
				fprintf(stderr, "perfcli: -E needs a value "
					"(try --help)\n");
				return 2;
			}
			enable_secret = argv[++i];
		} else if (!strcmp(a, "-e"))
			prompt_enable = 1;
		else if (!strcmp(a, "--encrypt"))
			encrypt = 1;
		else if (!strcmp(a, "-A"))
			prompt_auth = 1;
		else if (!strcmp(a, "-j"))
			rawjson = argv[++i];
		else if (!strcmp(a, "-q"))
			g_quiet = 1;
		else if (!strcmp(a, "-P"))
			g_pretty = 1;
		else if (!strcmp(a, "-V")) {
			printf("perfcli %s (libperfd %s)\n", PC_VERSION,
				perfd_version());
			return 0;
		} else if (a[0] == '-' && a[1]) {
			fprintf(stderr, "perfcli: unknown option '%s' (try --help)\n", a);
			return 2;
		} else
			break;
	}
	/* options after the command are the command's ARGUMENTS (`stats -a secret` sets
	 * no secret): say what happened to the word */
	{
		int k;

		for (k = i; k < argc; k++)
			if (argv[k][0] == '-' && argv[k][1] && !argv[k][2] &&
			    strchr("hpuaAjqPV", argv[k][1]))
				fprintf(stderr, "perfcli: '%s' after the command '%s' "
					"is an argument to it, not an option - options "
					"go before the command\n", argv[k], argv[i]);
	}
	if (!explicit_target && !prompt_auth) {
		int got = local_defaults(&host, &port, &secret);

		if (got && !g_quiet)
			fprintf(stderr, "perfcli: no target given - using %s: "
				"%s:%s%s\n", cf_path, host, port,
				(got & 2) ? " and its client secret" : "");
	}

	if (prompt_auth)
		secret = prompt_secret("client", auth_buf, sizeof auth_buf);
	if (prompt_enable)
		enable_secret = prompt_secret("enable", enable_buf, sizeof enable_buf);
	/* the privilege secret is NOT the app password: PERFCLI_ENABLE, distinct from
	 * PERFCLI_AUTH, so a shell exporting the client secret holds no DDL privilege */
	if (!enable_secret)
		enable_secret = getenv("PERFCLI_ENABLE");
	{
		/* statics: the options hold a POINTER to the secrets, not a
		 * copy, and redial() reuses both long after this block */
		static const char *secrets[2];
		static perfd_opts o;

		secrets[0] = secret; secrets[1] = NULL;
		o.io_timeout_ms = 30000;
		/* a fleet stop answers once its peers have gone, up to its timeout_ms (120 s at
		 * most); one-shot only - unsafe_twice() keeps a session from resending it */
		if (i < argc && !strcmp(argv[i], "fleetstop"))
			o.io_timeout_ms = 125000;
		/* a mail test answers once the SMTP server has, up to
		 * [mail] timeout_s (300 s at most) plus 5 */
		if (i < argc && !strcmp(argv[i], "mailtest"))
			o.io_timeout_ms = 310000;
		/* a reload of client secrets runs an Argon2id derivation each */
		if (i < argc && !strcmp(argv[i], "reload"))
			o.io_timeout_ms = 60000;
		if (secret)
			o.secrets = secrets;
		/* the enable secret never rides a plaintext connection */
		o.encrypt = encrypt || enable_secret;
		/* the connection's name in the node's enable, sweep and reset log lines */
		if (!conn_name) {
			static char nm[128];
			char hn[96] = "host";

			if (gethostname(hn, sizeof hn - 1) != 0)
				snprintf(hn, sizeof hn, "host");
			hn[strcspn(hn, ".")] = 0;
			snprintf(nm, sizeof nm, "perfcli@%s", hn);
			conn_name = nm;
		}
		o.name = conn_name;
		/* -h/-p or -u name a node - a call goes there, and its answer is the answer */
		o.pin = 1;
		g_dial_host = host; g_dial_port = port; g_dial_unix = unixp;
		g_dial_opts = &o;
		/* an abort is a file beside the journal - no connection, so
		 * it still works while the seed is the node being switched */
		if (i + 1 < argc && !strcmp(argv[i], "upgrade") &&
		        !strcmp(argv[i + 1], "abort"))
			return upgrade_cmd(argc - i - 1, argv + i + 1);
		g_p = unixp ? perfd_connect_unix(unixp, &o)
		            : perfd_connect(host, atoi(port), &o);
		if (!g_p) {
			/* name what was tried: "Connection refused" alone
			 * does not say where */
			if (unixp)
				fprintf(stderr, "perfcli: %s (%s)\n", perfd_error(NULL),
					unixp);
			else
				fprintf(stderr, "perfcli: %s (%s:%s)\n", perfd_error(NULL),
					host, port);
			if (!explicit_target && !cf_path[0])
				fprintf(stderr, "  no target was given and no daemon config "
					"was readable: a deployed daemon binds its own "
					"address - pass -h <address> (or -u <socket>)\n");
			/* a secret was given and the failure is not the socket's:
			 * the handshake is what fails on a wrong secret */
			else if (secret && !strstr(perfd_error(NULL), "refused") &&
			         !strstr(perfd_error(NULL), "timed out") &&
			         !strstr(perfd_error(NULL), "route"))
				fprintf(stderr, "  the connection was made and the secure "
					"session was not - wrong secret? or this listener "
					"speaks plaintext to your address (plaintext = "
					"loopback): drop -a\n");
			return 2;
		}
		g_encrypted = !secret ? 0 : o.encrypt ? 1 : 2;
	}
	/* prove the session before announcing it: with no secret on an encrypted door the
	 * connect succeeds and the daemon drops the connection at the first request */
	if (perfd_ping(g_p) != 0) {
		if (!secret)
			fprintf(stderr, "perfcli: the daemon closed the connection on "
				"the first request - no client secret was given: pass "
				"-a <secret> or -A to be prompted ([secrets] client in "
				"the daemon's config)\n");
		else
			fprintf(stderr, "perfcli: the first request failed: %s "
				"(wrong secret?)\n", perfd_error(g_p));
		return 2;
	}

	if (enable_secret) {
		g_enable_secret = enable_secret;
		if (raise_privilege() != 0) {
			fprintf(stderr, "perfcli: enable failed: %s\n",
				perfd_error(g_p));
			fprintf(stderr, "  the PRIVILEGE secret is [secrets] enable "
				"on the daemon - not the client secret -a takes\n");
			return 2;
		}
		if (!g_quiet)
			fprintf(stderr, "privileged: create/drop/resize/rename, "
				"map/unmap and restore are available on this "
				"connection\n");
	}
	if (rawjson)
		return raw_command(rawjson) ? 1 : 0;

	if (i < argc && !strcmp(argv[i], "upgrade-stage"))
		return upgrade_stage_cmd(argc - i, argv + i);
	if (i < argc && !strcmp(argv[i], "fleet-status"))
		return fleet_status_cmd();
	if (i < argc && !strcmp(argv[i], "upgrade"))           /* the roll driver */
		return upgrade_cmd(argc - i - 1, argv + i + 1);
	if (i < argc && !strcmp(argv[i], "upgrade-revert")) {
		int force = i + 1 < argc && !strcmp(argv[i + 1], "force");
		char *r = perfd_upgrade_revert(g_p, force);

		if (!r) {
			fprintf(stderr, "upgrade-revert: %s\n", perfd_error(g_p));
			return 1;
		}
		printf("%s\n", r);
		perfd_free_value(r);
		return 0;
	}
	if (i < argc) {                        /* one-shot from argv words */
		int n = build(req, sizeof req, argc - i, argv + i);

		if (n == -1)
			return 2;
		if (n == -2)
			return 0;
		return do_command(canon(argv[i]), n ? req : NULL) ? 1 : 0;
	}

	/* REPL / pipe: one command per line; a tty gets the line editor and history,
	 * a pipe reads plain lines and never touches the terminal */
	{
		static char pline[REQ_CAP];
		char histpath[512];
		int tty = isatty(0) && isatty(2);

		if (tty) {
			const char *home = getenv("HOME");

			if (home) {
				snprintf(histpath, sizeof histpath,
					"%s/.perfcli_history", home);
				le_history_load(histpath);
			}
			if (!g_quiet)
				fprintf(stderr, "perfcached %s%s%s%s - 'help' "
					"for commands, ^D quits\n",
					unixp ? unixp : host,
					unixp ? "" : ":", unixp ? "" : port,
					g_encrypted == 1 ? " (encrypted)" :
					g_encrypted ? " (authenticated)" : "");
		}
		for (;;) {
			char *wv[512], *line, *s, *e;
			int wc, n;

			if (tty) {
				line = le_readline("perfcached> ");
				if (!line)
					break;
				le_history_add(line);
			} else {
				if (!fgets(pline, sizeof pline, stdin))
					break;
				line = pline;
				e = line + strlen(line);
				while (e > line && (e[-1] == '\n' ||
				        e[-1] == '\r'))
					*--e = 0;
			}
			s = line;
			while (*s == ' ' || *s == '\t')
				s++;
			if (!strcmp(s, "exit") || !strcmp(s, "quit"))
				break;                 /* the way out everyone types */
			if (*s == '{') {           /* raw JSON passthrough */
				rc |= raw_command(s);
			} else if (*s && *s != '#') {
				wc = split_words(s, wv, 512);
				if (wc < 0) {
					fprintf(stderr, "perfcli: more than 512 words on "
						"one line - nothing sent\n");
					rc |= tty ? 0 : 1;
				} else if (wc > 0 && !strcmp(wv[0], "fleet-status")) {
					rc |= fleet_status_cmd();
				} else if (wc > 0 && !strcmp(wv[0], "upgrade-stage")) {
					rc |= upgrade_stage_cmd(wc, wv);
				} else if (wc > 0 && !local_cmd(wc, wv)) {
					n = build(req, sizeof req, wc, wv);
					if (n >= 0)
						rc |= do_command(canon(wv[0]),
							n ? req : NULL);
					else if (n == -1)
						rc |= tty ? 0 : 1;
				}
			}
		}
	}
	return rc ? 1 : 0;
}
