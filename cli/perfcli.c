/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * perfcli — the command-line client (the redis-cli analogue).
 *
 * All communication rides libperfd (lib/perfd.[ch]) - the one client
 * transport, per the 08-26 decree: connection, Noise channel, framing,
 * id-matched replies and error surfacing are the library's; the CLI
 * owns only words -> params, display, and the line editor
 * (cli/lineedit.c, history persisted 0600 in ~/.perfcli_history).
 * Zero dependencies beyond libsodium (which libperfd needs anyway).
 *
 * Modes:
 *   perfcli [opts] <command words...>      one-shot, exit code honest
 *   perfcli [opts] -j '<raw json-rpc>'     raw request passthrough
 *   perfcli [opts]                         REPL on a tty, pipe otherwise
 *
 * Options: -h host (127.0.0.1)  -p port (6479)  -u /unix.sock
 *          -a secret  -A (prompt, no echo)  [PERFCLI_AUTH env]
 *          -q quiet (result to stdout only, no prompt/banner)
 *
 * Word commands mirror the verb set 1:1; values with spaces take double
 * quotes with backslash escapes.  jset's value argument is RAW JSON.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "../src/json.h"
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

/*
 * S72: what we dialled with, so a session that outlived its server can
 * be redialled instead of prompting into a corpse.
 */
static const char *g_dial_host, *g_dial_port, *g_dial_unix;
static perfd_opts *g_dial_opts;

/* Redial once after the connection died.  libperfd deliberately does
 * not reconnect by itself - that is the caller's call - and for an
 * interactive session the caller is us: redis-cli redials on the next
 * command and says so, and anything else leaves a prompt attached to a
 * dead handle.  Returns 1 if a fresh connection is in place. */
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

/* one round trip via libperfd; prints the result (or the error).
 * Returns 0 on a result, 1 on an error / transport failure. */
static int do_command(const char *method, const char *params_json)
{
	char *res = perfd_command(g_p, method, params_json);

	if (!res && perfd_state(g_p) == PERFD_ST_FAILED && redial())
		res = perfd_command(g_p, method, params_json);

	if (!res) {
		fprintf(stderr, "%s\n", perfd_error(g_p));
		return 1;
	}
	if (g_pretty)
		pretty_print(res, strlen(res));
	else
		printf("%s\n", res);
	free(res);
	return 0;
}

/* raw JSON-RPC passthrough (-j / a '{' line): pull method + params out
 * and ride the library like every other command (its id wins) */
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
 * backslash escapes inside quotes */
static int split_words(char *s, char **argv, int max)
{
	int n = 0;

	while (*s) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s || n >= max)
			break;
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

/* build the PARAMS object ("{...}") for a word command; the method is
 * argv[0] and rides perfd_command.  Returns the params length, 0 for a
 * command with no params, -1 on usage error, -2 handled locally. */
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
	    !strcmp(m, "load") || !strcmp(m, "sync")) {
		return 0;                      /* no params */
	} else if (!strcmp(m, "probe")) {
		if (argc < 2)
			return 0;
		pc_jw_lit(&w, "{\"secs\":");
		pc_jw_i64(&w, atoll(argv[1]));
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
		NEED(1);
		P_OPEN();
		if (argc >= 3)
			jw_kv_str(&w, "match", argv[2]);
		if (argc >= 4)
			jw_kv_ll(&w, "limit", atoll(argv[3]));
		pc_jw_lit(&w, "}");
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
		  "  ping | stats [col] | save | sync | load\n"
	  "  probe [secs] - re-measure WAL storage iops/throughput\n"
	  "      (blocks for the probe; perturbs live fsync latency)\n"
		  "  get|del|exists|ttl <col> <key>\n"
		  "  set <col> <key> <value> [ttl]\n"
		  "  expire <col> <key> <ttl>\n"
		  "  add|sub <col> <key> [by] [ttl]\n"
		  "  keys <col> [pattern] [limit]\n"
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
	out[w.len] = 0;                    /* perfd_command takes a C string
	                                    * - an unterminated params once
	                                    * dragged the PREVIOUS command's
	                                    * tail into the request */
	return (int)w.len;

usage:
	fprintf(stderr, "perfcli: bad arguments for '%s' (try help)\n", m);
	return -1;
}

/* ---- secret input ------------------------------------------------------ */

static char *prompt_secret(void)
{
	static char buf[512];
	struct termios t, t0;
	char *nl;

	fprintf(stderr, "secret: ");
	tcgetattr(0, &t0);
	t = t0;
	t.c_lflag &= ~(tcflag_t)ECHO;
	tcsetattr(0, TCSANOW, &t);
	if (!fgets(buf, sizeof buf, stdin))
		buf[0] = 0;
	tcsetattr(0, TCSANOW, &t0);
	fprintf(stderr, "\n");
	nl = strchr(buf, '\n');
	if (nl)
		*nl = 0;
	return buf[0] ? buf : NULL;
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *host = "127.0.0.1", *port = "6479", *unixp = NULL;
	const char *secret = getenv("PERFCLI_AUTH"), *rawjson = NULL;
	char req[REQ_CAP];
	int i, rc = 0, prompt_auth = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") && i + 1 < argc)
			host = argv[++i];
		else if (!strcmp(argv[i], "-p") && i + 1 < argc)
			port = argv[++i];
		else if (!strcmp(argv[i], "-u") && i + 1 < argc)
			unixp = argv[++i];
		else if (!strcmp(argv[i], "-a") && i + 1 < argc)
			secret = argv[++i];
		else if (!strcmp(argv[i], "-A"))
			prompt_auth = 1;
		else if (!strcmp(argv[i], "-j") && i + 1 < argc)
			rawjson = argv[++i];
		else if (!strcmp(argv[i], "-q"))
			g_quiet = 1;
		else if (!strcmp(argv[i], "-P"))
			g_pretty = 1;
		else if (!strcmp(argv[i], "-V")) {
			printf("perfcli %s (libperfd %s)\n", PC_VERSION,
				perfd_version());
			return 0;
		}
		else if (!strcmp(argv[i], "--help") ||
		         !strcmp(argv[i], "-?")) {
			printf("usage: perfcli [-h host] [-p port] "
				"[-u /unix.sock] [-a secret | -A] [-q] [-P] "
				"[-V]\n"
				"               [-j '<raw json-rpc>' | "
				"<command words...>]\n"
				"  -P pretty-prints results ('pretty on|off' "
				"inside the session)\n");
			return 0;
		} else
			break;
	}

	if (prompt_auth)
		secret = prompt_secret();
	{
		/* statics: the options hold a POINTER to the secrets, not a
		 * copy, and redial() reuses both long after this block */
		static const char *secrets[2];
		static perfd_opts o;

		secrets[0] = secret; secrets[1] = NULL;
		o.io_timeout_ms = 30000;
		if (secret)
			o.secrets = secrets;
		g_dial_host = host; g_dial_port = port; g_dial_unix = unixp;
		g_dial_opts = &o;
		g_p = unixp ? perfd_connect_unix(unixp, &o)
		            : perfd_connect(host, atoi(port), &o);
		if (!g_p) {
			fprintf(stderr, "perfcli: %s\n", perfd_error(NULL));
			return 2;
		}
		g_encrypted = secret != NULL;
	}

	if (rawjson)
		return raw_command(rawjson) ? 1 : 0;

	if (i < argc) {                        /* one-shot from argv words */
		int n = build(req, sizeof req, argc - i, argv + i);

		if (n == -1)
			return 2;
		if (n == -2)
			return 0;
		return do_command(argv[i], n ? req : NULL) ? 1 : 0;
	}

	/* REPL / pipe: one command per line.  On a tty the in-house line
	 * editor supplies arrows + persisted history; pipes read plain
	 * lines and never touch the terminal. */
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
					g_encrypted ? " (encrypted)" : "");
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
			if (*s == '{') {           /* raw JSON passthrough */
				rc |= raw_command(s);
			} else if (*s && *s != '#') {
				wc = split_words(s, wv, 512);
				if (wc > 0 && !local_cmd(wc, wv)) {
					n = build(req, sizeof req, wc, wv);
					if (n >= 0)
						rc |= do_command(wv[0],
							n ? req : NULL);
					else if (n == -1)
						rc |= tty ? 0 : 1;
				}
			}
		}
	}
	return rc ? 1 : 0;
}
