/* SPDX-License-Identifier: GPL-2.0-or-later */
/* perfdexectest.c - perfd_exec's table from inside (perfd.c compiled in): what it refuses before any
 * I/O and in which words, how a daemon's error and a reply tree become replies, and that a lost
 * connection is NULL, never a reply. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>
#include "perfd.c"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static perfd_t *mk(void)
{
	perfd_opts o = PERFD_OPTS_INIT;

	o.spares = PERFD_SPARES_NONE;
	return pd_alloc(&o);
}

/* the command (args split by |) is answered with an ERROR reply holding @want, no I/O */
static void refuses(perfd_t *p, const char *cmd, const char *want)
{
	char buf[512], *av[16], *s;
	int n = 0;
	perfd_reply *r;

	snprintf(buf, sizeof buf, "%s", cmd);
	for (s = strtok(buf, "|"); s && n < 16; s = strtok(NULL, "|"))
		av[n++] = s;
	r = perfd_exec(p, "c", n, (const char *const *)av, NULL);
	CHK(r && r->type == PERFD_REPLY_ERROR && strstr(r->str, want) && r->len == strlen(r->str),
		"%-34s -> %s", cmd, r ? r->str : perfd_error(p));
	perfd_reply_free(r);
}

int main(void)
{
	perfd_t *p;
	perfd_reply *r;
	size_t used;

	signal(SIGPIPE, SIG_IGN);
	p = mk();

	printf("--- A. refused before any I/O, in the door's words\n");
	refuses(p, "FLUSHALL", "ERR unsupported command 'FLUSHALL'");
	refuses(p, "TYPE|k", "ERR unsupported command 'TYPE'");
	refuses(p, "GET", "ERR wrong number of arguments for 'get' command");
	refuses(p, "get|a|b", "ERR wrong number of arguments for 'get' command");
	refuses(p, "SET|k", "ERR wrong number of arguments for 'set' command");
	refuses(p, "SET|k|v|EX|0", "ERR invalid expire time in 'set' command");
	refuses(p, "SET|k|v|EX", "ERR invalid expire time in 'set' command");
	refuses(p, "SET|k|v|PX|x", "ERR invalid expire time in 'set' command");
	refuses(p, "SET|k|v|EX|2147483648", "ERR invalid expire time in 'set' command");
	refuses(p, "SET|k|v|NX|XX", "ERR syntax error");
	refuses(p, "SET|k|v|NX|IFEQ|x", "ERR syntax error");
	refuses(p, "SET|k|v|KEEPTTL", "ERR unsupported SET option");
	refuses(p, "SET|k|v|IFDEQ|x", "IFDEQ/IFDNE are not supported");
	refuses(p, "SETEX|k|0|v", "ERR invalid expire time");
	refuses(p, "PSETEX|k|v", "ERR wrong number of arguments");
	refuses(p, "DEL", "ERR wrong number of arguments for 'del' command");
	refuses(p, "EXISTS", "ERR wrong number of arguments for 'exists' command");
	refuses(p, "EXPIRE|k|x", "ERR wrong number of arguments for 'expire' command");
	refuses(p, "EXPIRE|k|2147483648", "ERR invalid expire time in 'expire' command");
	refuses(p, "TTL|a|b", "ERR wrong number of arguments for 'ttl' command");
	refuses(p, "INCR|a|b", "ERR wrong number of arguments");
	refuses(p, "INCRBY|k|x", "ERR value is not an integer or out of range");
	refuses(p, "DECRBY|k|-9223372036854775808", "ERR value is not an integer or out of range");
	refuses(p, "MGET", "ERR wrong number of arguments for 'mget' command");
	refuses(p, "MSET|k", "ERR wrong number of arguments for 'mset' command");
	refuses(p, "MSET|k|v|k2", "ERR wrong number of arguments for 'mset' command");
	refuses(p, "HGET|k", "ERR wrong number of arguments for 'hget' command");
	refuses(p, "HSET|k|f", "ERR wrong number of arguments for 'hset' command");
	refuses(p, "HSET|k|f|v|f2", "ERR wrong number of arguments for 'hset' command");
	refuses(p, "HLEN|k|x", "ERR wrong number of arguments for 'hlen' command");
	refuses(p, "JSON.SET|k|$", "ERR wrong number of arguments for this JSON command");
	refuses(p, "JSON.SET|k|$|1|NX|XX", "ERR syntax error");
	refuses(p, "JSON.SET|k|$|1|EX|0", "ERR invalid expire time in 'json.set' command");
	refuses(p, "JSON.SET|k|$|1|FOO", "ERR unsupported JSON.SET option");
	refuses(p, "JSON.GET", "ERR wrong number of arguments for this JSON command");
	{
		const char *av[] = { "GET", "a\0b", "JSON.GET", "k", "" };
		size_t al[] = { 3, 3, 8, 1, 0 };

		r = perfd_exec(p, "c", 2, av, al);
		CHK(r && r->type == PERFD_REPLY_ERROR && strstr(r->str, "NUL byte"),
			"A1 a key holding a NUL is refused (%s)", r ? r->str : "-");
		perfd_reply_free(r);
		al[1] = 0;
		r = perfd_exec(p, "c", 2, av, al);
		CHK(r && r->type == PERFD_REPLY_ERROR && !strcmp(r->str, "ERR key too long"),
			"A2 an empty key: ERR key too long");
		perfd_reply_free(r);
		r = perfd_exec(p, "c", 3, av + 2, al + 2);
		CHK(r && r->type == PERFD_REPLY_ERROR && !strcmp(r->str, "ERR bad path"),
			"A3 an empty JSON path: ERR bad path");
		perfd_reply_free(r);
		CHK(perfd_exec(p, NULL, 1, av, NULL) == NULL && perfd_error_kind(p) == PERFD_E_ARG,
			"A4 no collection: NULL, PERFD_E_ARG");
		CHK(perfd_exec(p, "c", 0, av, NULL) == NULL && perfd_error_kind(p) == PERFD_E_ARG,
			"A5 no command: NULL, PERFD_E_ARG");
		CHK(perfd_exec(NULL, "c", 1, av, NULL) == NULL, "A6 no handle: NULL");
		perfd_reply_free(NULL);
	}

	printf("--- B. a daemon's error becomes the door's error line; a transport failure is NULL\n");
	{
		static const struct { const char *err; int kind; const char *want; } t[] = {
			{ "WRONGTYPE Operation against a key holding the wrong kind of value", PERFD_E_SERVER,
			  "WRONGTYPE Operation against a key holding the wrong kind of value" },
			{ "value is not an integer or out of range", PERFD_E_SERVER,
			  "ERR value is not an integer or out of range" },
			{ "Existing key has wrong Redis type", PERFD_E_SERVER, "Existing key has wrong Redis type" },
			{ "stored value is not JSON", PERFD_E_SERVER,
			  "WRONGTYPE the value at this key is not a JSON document" },
			{ "node is not READY (recovering)", PERFD_E_REFUSED, "ERR node is not READY (recovering)" },
			{ "ERR hash too large - this is a cache", PERFD_E_SERVER, "ERR hash too large - this is a cache" },
		};
		size_t i;

		for (i = 0; i < sizeof t / sizeof t[0]; i++) {
			snprintf(p->err, sizeof p->err, "%s", t[i].err);
			p->err_kind = t[i].kind;
			r = exec_fail(p);
			CHK(r && r->type == PERFD_REPLY_ERROR && !strcmp(r->str, t[i].want),
				"B%zu %s", i + 1, r ? r->str : "(NULL)");
			perfd_reply_free(r);
		}
		snprintf(p->err, sizeof p->err, "connection closed");
		p->err_kind = PERFD_E_TRANSPORT;
		CHK(exec_fail(p) == NULL, "B7 a transport error: NULL");
		p->err_kind = PERFD_E_PROTOCOL;
		CHK(exec_fail(p) == NULL, "B8 a protocol error: NULL");
	}
	{
		perfd_t *q = mk();
		int sv[2] = { -1, -1 };
		const char *av[] = { "GET", "k" };

		CHK(q && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "B9 a link");
		q->fd = sv[0];
		close(sv[1]);
		r = perfd_exec(q, "c", 2, av, NULL);
		CHK(r == NULL && perfd_error_kind(q) == PERFD_E_TRANSPORT,
			"B10 a GET on a closed link is NULL, PERFD_E_TRANSPORT (%s)", perfd_error(q));
		perfd_reply_free(r);
		perfd_free(q);
	}

	printf("--- C. a reply tree becomes the reply the door renders from it\n");
	{
		struct pc_tw w;

		pc_tw_init(&w, NULL, 0);
		pc_tw_arr(&w);
		pc_tw_i64(&w, -5);
		pc_tw_nil(&w);
		pc_tw_bulk(&w, "a\0b", 3);
		pc_tw_ok(&w);
		pc_tw_bool(&w, 1);
		pc_tw_bool(&w, 0);
		pc_tw_err(&w, "WRONGTYPE x");
		pc_tw_map(&w);
		pc_tw_key(&w, "f");
		pc_tw_str(&w, "v");
		pc_tw_end(&w);
		pc_tw_end(&w);
		r = exec_tree(p, w.b, w.n, &used, 0);
		CHK(r && used == w.n && r->type == PERFD_REPLY_ARRAY && r->elements == 8,
			"C1 an array of eight, the whole tree read");
		if (r && r->elements == 8) {
			CHK(r->element[0]->type == PERFD_REPLY_INTEGER && r->element[0]->integer == -5,
				"C2 an integer");
			CHK(r->element[1]->type == PERFD_REPLY_NIL, "C3 nil");
			CHK(r->element[2]->type == PERFD_REPLY_STRING && r->element[2]->len == 3 &&
				!memcmp(r->element[2]->str, "a\0b", 3), "C4 a bulk, binary-safe");
			CHK(r->element[3]->type == PERFD_REPLY_STATUS && !strcmp(r->element[3]->str, "OK"),
				"C5 OK is a status");
			CHK(r->element[4]->integer == 1 && r->element[5]->integer == 0 &&
				r->element[5]->type == PERFD_REPLY_INTEGER, "C6 true and false are :1 and :0");
			CHK(r->element[6]->type == PERFD_REPLY_ERROR && !strcmp(r->element[6]->str, "WRONGTYPE x"),
				"C7 an error item");
			CHK(r->element[7]->type == PERFD_REPLY_ARRAY && r->element[7]->elements == 2 &&
				!strcmp(r->element[7]->element[0]->str, "f"), "C8 a map is its pairs, flat");
		}
		perfd_reply_free(r);
		r = exec_tree(p, w.b, w.n - 1, &used, 0);
		CHK(r == NULL && used == 0, "C9 a cut tree is refused");
		pc_tw_free(&w);
	}

	printf("--- D. JSON paths as the door reads them\n");
	{
		static const struct { const char *in; const char *jp; int v2, root; } t[] = {
			{ "$.a", "$.a", 1, 0 }, { "$", "$", 1, 1 }, { ".a", "$.a", 0, 0 },
			{ ".", "$", 0, 1 }, { "a", "$.a", 0, 0 }, { "[0]", "$[0]", 0, 0 },
		};
		char jp[600];
		size_t i;
		int v2, root;

		for (i = 0; i < sizeof t / sizeof t[0]; i++) {
			const char *av[3] = { "JSON.GET", "k", t[i].in };
			size_t al[3] = { 8, 1, strlen(t[i].in) };
			struct exec_args a = { 3, av, al };

			r = exec_jpath(&a, 1, jp, &v2, &root);
			CHK(!r && !strcmp(jp, t[i].jp) && v2 == t[i].v2 && root == t[i].root,
				"D%zu %-4s -> %s v2 %d root %d", i + 1, t[i].in, jp, v2, root);
			perfd_reply_free(r);
		}
		{
			const char *av[2] = { "JSON.GET", "k" };
			size_t al[2] = { 8, 1 };
			struct exec_args a = { 2, av, al };

			r = exec_jpath(&a, 0, jp, &v2, &root);
			CHK(!r && !strcmp(jp, "$") && !v2 && root, "D7 no path: the root, a bare reply");
		}
	}
	perfd_free(p);
	printf("perfdexectest: %s\n", fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
