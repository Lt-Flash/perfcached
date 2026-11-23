/* SPDX-License-Identifier: GPL-2.0-or-later */
/* execcmp.c - perfd_exec against the RESP door of the same daemon: every command of a script runs
 * on both, each on keys of its own, and the replies must have the same shape and content (errors:
 * the same type).  Usage: execcmp <port> - the daemon's port, plaintext from loopback, collection 0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "../lib/perfd.h"

static int pass, fail;
static FILE *rf;

/* "~" first: integers may differ by a clock tick (TTLs); "{x}" is key x, in each side's namespace */
static const char *const script[] = {
	"GET|{a}", "SET|{a}|v1", "GET|{a}", "SET|{a}|v2|NX", "SET|{a}|v2|XX", "GET|{a}",
	"SET|{b}|x|NX|EX|100", "~TTL|{b}", "~PTTL|{b}", "SET|{c}|y|PX|1500", "~TTL|{c}",
	"TTL|{none}", "PTTL|{none}", "SET|{a}|v|NX|XX", "SET|{a}|v|KEEPTTL", "SET|{a}|v|EX|0",
	"SET|{a}|v|EX", "SET|{a}", "SETEX|{d}|100|dv", "~TTL|{d}", "SETEX|{d}|0|dv",
	"PSETEX|{e}|2500|ev", "GET|{e}", "SET|{a}|v3|IFEQ|v2", "SET|{a}|v4|IFEQ|nope", "GET|{a}",
	"EXISTS|{a}", "EXISTS|{a}|{b}|{none}|{a}", "DEL|{none}", "DEL|{e}|{d}|{none}",
	"EXPIRE|{a}|100", "~TTL|{a}", "PEXPIRE|{a}|200000", "~PTTL|{a}", "EXPIRE|{none}|10",
	"EXPIRE|{a}|x", "EXPIRE|{b}|0", "EXISTS|{b}",
	"INCR|{n}", "INCRBY|{n}|10", "DECR|{n}", "DECRBY|{n}|3", "INCRBY|{n}|x", "INCR|{a}", "GET|{n}",
	"MGET|{a}|{none}|{n}", "MGET|{none}", "MGET",
	"HSET|{h}|f1|a|f2|b", "HSET|{h}|f1|z", "HGET|{h}|f1", "HGET|{h}|nof", "HGET|{noh}|f",
	"HMGET|{h}|f1|nof|f2", "HSETNX|{h}|f1|q", "HSETNX|{h}|f3|q", "HEXISTS|{h}|f3", "HLEN|{h}",
	"HSTRLEN|{h}|f1", "HKEYS|{h}", "HVALS|{h}", "HGETALL|{h}", "HINCRBY|{h}|cnt|5",
	"HINCRBYFLOAT|{h}|fl|1.5", "HDEL|{h}|f3|nof", "HSCAN|{h}|0", "HGETALL|{noh}", "HGET|{h}",
	"HSET|{h}|f1", "HGET|{a}|f", "GET|{h}",
	"JSON.SET|{j}|$|{\"a\":1,\"b\":[1,2]}", "JSON.GET|{j}", "JSON.GET|{j}|$", "JSON.GET|{j}|$.a",
	"JSON.GET|{j}|.a", "JSON.GET|{j}|a", "JSON.GET|{j}|$.zz", "JSON.GET|{j}|.zz", "JSON.GET|{nj}",
	"JSON.GET|{nj}|$.a", "JSON.SET|{j}|$.c|true|NX", "JSON.SET|{j}|$.c|false|NX",
	"JSON.SET|{j}|$.zz|1|XX", "JSON.SET|{j}|$.a|7|XX", "JSON.SET|{nj2}|$|1|XX",
	"JSON.SET|{j2}|$|[1]|EX|100", "~TTL|{j2}", "JSON.GET|{j}", "JSON.DEL|{j}|$.c",
	"JSON.DEL|{j}|$.c", "JSON.DEL|{j}", "EXISTS|{j}", "JSON.GET|{a}", "JSON.SET|{j}|$|{bad",
	"JSON.SET|{j}|$", "UNLINK|{a}", "GET|{a}",
	NULL
};

/* one script line as argv, its keys in namespace @ns */
static int split(const char *line, const char *ns, char **argv, size_t *len, char *buf)
{
	int n = 0;
	char *o = buf;
	const char *s = line;

	while (*s) {
		const char *e = strchr(s, '|');
		size_t l = e ? (size_t)(e - s) : strlen(s);

		argv[n] = o;
		if (l > 2 && s[0] == '{' && s[l - 1] == '}') {
			o += sprintf(o, "%s:%.*s", ns, (int)(l - 2), s + 1);
		} else {
			memcpy(o, s, l);
			o += l;
		}
		len[n] = (size_t)(o - argv[n]);
		*o++ = 0;
		n++;
		s += l + (e ? 1 : 0);
		if (!e)
			break;
	}
	return n;
}

static perfd_reply *mk(int type)
{
	perfd_reply *r = calloc(1, sizeof *r);

	r->type = type;
	return r;
}

static void rfree(perfd_reply *r)
{
	size_t i;

	if (!r)
		return;
	for (i = 0; i < r->elements; i++)
		rfree(r->element[i]);
	free(r->element);
	free(r->str);
	free(r);
}

/* one RESP2 reply from the door */
static perfd_reply *rd(void)
{
	char line[65536];
	perfd_reply *r;
	long long n;
	size_t i;

	if (!fgets(line, sizeof line, rf))
		return NULL;
	line[strcspn(line, "\r\n")] = 0;
	n = atoll(line + 1);
	switch (line[0]) {
	case '+':
	case '-':
		r = mk(line[0] == '+' ? PERFD_REPLY_STATUS : PERFD_REPLY_ERROR);
		r->str = strdup(line + 1);
		r->len = strlen(line + 1);
		return r;
	case ':':
		r = mk(PERFD_REPLY_INTEGER);
		r->integer = n;
		return r;
	case '$':
		if (n < 0)
			return mk(PERFD_REPLY_NIL);
		r = mk(PERFD_REPLY_STRING);
		r->str = malloc((size_t)n + 2);
		if (fread(r->str, 1, (size_t)n + 2, rf) != (size_t)n + 2)
			return r;
		r->str[n] = 0;
		r->len = (size_t)n;
		return r;
	case '*':
		if (n < 0)
			return mk(PERFD_REPLY_NIL);
		r = mk(PERFD_REPLY_ARRAY);
		r->elements = (size_t)n;
		r->element = calloc((size_t)n + 1, sizeof *r->element);
		for (i = 0; i < (size_t)n; i++)
			r->element[i] = rd();
		return r;
	}
	return NULL;
}

static void show(const perfd_reply *r, char *o, size_t cap)
{
	size_t i, u;

	if (!r) {
		snprintf(o, cap, "(none)");
		return;
	}
	switch (r->type) {
	case PERFD_REPLY_NIL: snprintf(o, cap, "nil"); return;
	case PERFD_REPLY_INTEGER: snprintf(o, cap, "int:%lld", r->integer); return;
	case PERFD_REPLY_STRING: snprintf(o, cap, "bulk:%s", r->str); return;
	case PERFD_REPLY_STATUS: snprintf(o, cap, "+%s", r->str); return;
	case PERFD_REPLY_ERROR: snprintf(o, cap, "-%s", r->str); return;
	}
	u = (size_t)snprintf(o, cap, "[");
	for (i = 0; i < r->elements && u < cap; i++) {
		show(r->element[i], o + u, cap - u);
		u += strlen(o + u);
		if (u + 2 < cap)
			o[u++] = '|', o[u] = 0;
	}
	if (u + 2 < cap)
		strcpy(o + u, "]");
}

/* 1 when @a and @b agree; @tick: integers may differ by up to 1000 (a TTL second, in ms) */
static int same(const perfd_reply *a, const perfd_reply *b, int tick)
{
	size_t i;

	if (!a || !b || a->type != b->type)
		return 0;
	switch (a->type) {
	case PERFD_REPLY_INTEGER:
		return tick ? llabs(a->integer - b->integer) <= 1000 : a->integer == b->integer;
	case PERFD_REPLY_STRING:
	case PERFD_REPLY_STATUS:
		return a->len == b->len && !memcmp(a->str, b->str, a->len);
	case PERFD_REPLY_ERROR:
		return 1;
	case PERFD_REPLY_ARRAY:
		if (a->elements != b->elements)
			return 0;
		for (i = 0; i < a->elements; i++)
			if (!same(a->element[i], b->element[i], tick))
				return 0;
	}
	return 1;
}

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 0, i, j, fd, exact = 0, errs = 0;
	perfd_opts o = PERFD_OPTS_INIT;
	struct sockaddr_in sa;
	perfd_t *p;

	o.spares = PERFD_SPARES_NONE;
	p = perfd_connect("127.0.0.1", port, &o);
	fd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (!p || fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
	    !(rf = fdopen(fd, "r+"))) {
		printf("execcmp: cannot reach the daemon on %d (%s)\n", port, p ? "resp" : perfd_error(NULL));
		return 1;
	}
	for (i = 0; script[i]; i++) {
		char *av[16], buf[2048], sr[4096], se[4096];
		size_t al[16];
		const char *line = script[i] + (script[i][0] == '~');
		int n = split(line, "r", av, al, buf);
		perfd_reply *rr, *er;

		fprintf(rf, "*%d\r\n", n);
		for (j = 0; j < n; j++) {
			fprintf(rf, "$%zu\r\n", al[j]);
			fwrite(av[j], 1, al[j], rf);
			fputs("\r\n", rf);
		}
		fflush(rf);
		rr = rd();
		n = split(line, "n", av, al, buf);
		er = perfd_exec(p, "0", n, (const char *const *)av, al);
		show(rr, sr, sizeof sr);
		show(er, se, sizeof se);
		if (same(rr, er, script[i][0] == '~')) {
			pass++;
			if (rr->type == PERFD_REPLY_ERROR) {
				errs++;
				exact += !strcmp(rr->str, er->str);
				if (strcmp(rr->str, er->str))
					printf("  note %-36s resp %s / exec %s\n", line, sr, se);
			}
		} else {
			fail++;
			printf("FAIL %-36s resp %s / exec %s%s%s\n", line, sr, se,
				er ? "" : " - ", er ? "" : perfd_error(p));
		}
		rfree(rr);
		perfd_reply_free(er);
	}
	printf("  errors: %d of %d texts identical\n", exact, errs);
	{
		/* MSET (the door has none): OK, and the values as GET reads them */
		const char *ms[] = { "MSET", "n:m1", "x1", "n:m2", "x2" };
		const char *mg[] = { "MGET", "n:m1", "n:m2" };
		const char *un[] = { "FLUSHALL" };
		perfd_reply *r = perfd_exec(p, "0", 5, ms, NULL);

		if (r && r->type == PERFD_REPLY_STATUS && !strcmp(r->str, "OK"))
			pass++;
		else
			fail++, printf("FAIL MSET: not +OK\n");
		perfd_reply_free(r);
		r = perfd_exec(p, "0", 3, mg, NULL);
		if (r && r->type == PERFD_REPLY_ARRAY && r->elements == 2 &&
		    !strcmp(r->element[0]->str, "x1") && !strcmp(r->element[1]->str, "x2"))
			pass++;
		else
			fail++, printf("FAIL MSET then MGET\n");
		perfd_reply_free(r);
		r = perfd_exec(p, "0", 1, un, NULL);
		if (r && r->type == PERFD_REPLY_ERROR && strstr(r->str, "unsupported command"))
			pass++;
		else
			fail++, printf("FAIL FLUSHALL: not refused as unsupported\n");
		perfd_reply_free(r);
	}
	perfd_free(p);
	fclose(rf);
	printf("execcmp: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
