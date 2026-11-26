/* SPDX-License-Identifier: GPL-2.0-or-later */
/* perfdlinktest.c - libperfd's per-connection state, from inside (perfd.c compiled in): what a
 * replaced link leaves behind, what an async handle keeps, what one frame may carry, what a
 * refused async handshake says, a node's refusal before the handshake. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
/* the seam for an allocation that fails: perfd.c's reallocs only, while the flag is set */
static int realloc_fails;
static void *test_realloc(void *q, size_t n)
{
	return realloc_fails ? NULL : realloc(q, n);
}
#define realloc(q, n) test_realloc((q), (n))
#include "perfd.c"
#undef realloc

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static int holds(const char *b, size_t n, const char *s)
{
	size_t l = strlen(s), i;

	for (i = 0; i + l <= n; i++)
		if (!memcmp(b + i, s, l))
			return 1;
	return 0;
}

static void cb_count(char *result, size_t len, const char *errmsg, void *ctx)
{
	(void)len;
	(void)errmsg;
	free(result);
	(*(int *)ctx)++;
}

/* one wire frame into @b: header, then @n bytes of body */
static size_t frame(unsigned char *b, int type, unsigned long long id, const void *body, size_t n)
{
	b[0] = BIN_MAGIC;
	b[1] = BIN_VER;
	b[2] = (unsigned char)type;
	b[3] = 0;
	wle32(b + 4, (unsigned int)n);
	wle64(b + 8, id);
	memcpy(b + BIN_HDR, body, n);
	return BIN_HDR + n;
}

struct hk {
	perfd_t *p;
	int calls, depth, rc, kind;
};

/* a pub/sub handler that uses the handle it was called from */
static void hook_calls_back(const char *json, size_t len, void *ctx)
{
	struct hk *h = ctx;

	(void)json;
	(void)len;
	h->calls++;
	if (h->depth)
		return;                        /* the test's own stop to an unbounded recursion */
	h->depth++;
	h->rc = perfd_ping(h->p);
	h->kind = perfd_error_kind(h->p);
	h->depth--;
}

struct hg {
	perfd_t *p;
	int n, rc;
	char f2[8];
};

/* a field callback that looks something up on the handle it was called from */
static int hg_calls_back(void *ctx, const char *f, size_t fl, const void *v, size_t vl)
{
	struct hg *h = ctx;

	(void)v;
	(void)vl;
	if (++h->n == 1)
		h->rc = perfd_get(h->p, "c", "other", NULL, NULL, NULL);
	else if (fl < sizeof h->f2) {
		memcpy(h->f2, f, fl);
		h->f2[fl] = 0;
	}
	return 0;
}

static void on_alarm(int sig)
{
	(void)sig;
}

struct hget {
	perfd_t *p;
	int calls, rc, kind;
};

/* a pub/sub handler that reads a key on the handle it was called from */
static void hook_gets(const char *json, size_t len, void *ctx)
{
	struct hget *h = ctx;
	void *v = NULL;
	size_t vl = 0;

	(void)json;
	(void)len;
	if (h->calls++)
		return;
	h->rc = perfd_get(h->p, "c", "nested", &v, &vl, NULL);
	h->kind = perfd_error_kind(h->p);
	free(v);
}

static perfd_t *mk(void)
{
	perfd_opts o = PERFD_OPTS_INIT;

	o.spares = PERFD_SPARES_NONE;
	return pd_alloc(&o);
}

int main(void)
{
	printf("--- A. a replaced connection takes the replies parked on it along\n");
	{
		perfd_t *p = mk(), *s = mk();
		char *out = NULL;
		size_t n = 0;
		int rc;

		/* a pipeline on the old link: 5002 and 5003 overtook 5001 and were parked, then it died */
		p->next_id = 5100;
		push_id(p, 5002);
		push_id(p, 5003);
		park_msg(p, 5002, "stale-5002", 10, "");
		park_msg(p, 5003, "stale-5003", 10, "");
		s->next_id = 2;                /* the standby's counter: a ping and a conninfo */
		adopt_conn(p, s);
		CHK(p->early == NULL, "A1 no reply parked on the dead link survives adopt_conn");
		CHK(p->next_id >= 5100, "A2 the id counter does not go back below the dead link's (%llu)",
			p->next_id);
		/* the new link's request that gets the number 5002 is not answered from the dead link */
		push_id(p, 5002);
		rc = collect_head(p, &out, &n);
		CHK(rc != 0 || n != 10 || memcmp(out, "stale-5002", 10),
			"A3 a request numbered 5002 on the new link is not handed the old link's reply");
		perfd_free(s);
		perfd_free(p);
	}

	printf("--- B. an async handle keeps no blocking FIFO\n");
	{
		perfd_t *p = mk();
		int i, ok = 0;

		p->async = 1;                  /* as perfd_connect_async makes it */
		for (i = 0; i < 1000; i++) {
			if (enqueue_bin(p, BV_GET, "c", "k", NULL, 0, 0, 0) == 0)
				ok++;
			p->qlen = 0;           /* as submit_framed hands the frame on */
		}
		CHK(ok == 1000, "B1 1000 binary requests framed on an async handle");
		CHK(p->nids == 0, "B2 none of them is on the blocking collector's id FIFO (%d are)",
			p->nids);
		for (i = 0; i < 10; i++) {
			enqueue_cmd(p, "ping", NULL, 0);
			p->qlen = 0;
		}
		CHK(p->nids == 0, "B3 nor a command request (%d)", p->nids);
		perfd_free(p);
	}

	printf("--- C. a ping on a route_hints handle\n");
	{
		perfd_opts o = PERFD_OPTS_INIT;
		perfd_t *p;
		pid_t pid;
		int st = -1;

		o.route_hints = 1;
		p = pd_alloc(&o);
		CHK(p && p->route_hints, "C1 the handle keeps a holder table");
		/* what bin_roundtrip does after the pong: PING carries neither collection nor key */
		fflush(stdout);
		pid = fork();
		if (pid == 0) {
			hint_after(p, p, BV_PING, NULL, NULL, 0, "\x01", 1);
			_exit(0);
		}
		if (pid > 0)
			waitpid(pid, &st, 0);
		CHK(pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
			"C2 the reply to a ping is not hashed as a key (signal %d)",
			pid > 0 && WIFSIGNALED(st) ? WTERMSIG(st) : 0);
		perfd_free(p);
	}

	printf("--- D. one frame is at most the daemon's 1 MiB (PC_MAX_REQ)\n");
	{
		perfd_t *p = mk();
		size_t fixed = 1 + 3 + 8 + 1 + 1;      /* verb, cn/kn, ttl, "c", "k" */
		size_t at = (1u << 20) - fixed;
		char *v = calloc(1, at + 2);
		unsigned long long id0;

		CHK(v && enqueue_bin(p, BV_SET, "c", "k", v, at, 0, 0) == 0,
			"D1 a SET whose frame is exactly 1 MiB is framed");
		p->qlen = 0;
		p->nids = p->idhead = 0;
		id0 = p->next_id;
		CHK(v && enqueue_bin(p, BV_SET, "c", "k", v, at + 1, 0, 0) != 0 &&
			perfd_error_kind(p) == PERFD_E_ARG,
			"D2 one byte more is refused as an argument error, not sent to cost the link (%s)",
			perfd_error(p));
		CHK(p->qlen == 0 && p->nids == 0 && !p->poisoned && p->next_id == id0,
			"D3 and nothing of it was queued, numbered or poisoned");
		free(v);
		perfd_free(p);
	}

	printf("--- E. the enable secret never crosses a plaintext link\n");
	{
		static const char *const secs[] = { "client-secret", NULL };
		perfd_opts o = PERFD_OPTS_INIT;
		perfd_t *p;
		struct pc_tw w;
		char *out = NULL, seen[4096];
		size_t n = 0;
		ssize_t got;
		int sv[2] = { -1, -1 }, rc;

		o.spares = PERFD_SPARES_NONE;
		o.secrets = secs;
		p = pd_alloc(&o);
		/* a link authenticated by the handshake and then granted plaintext (encrypted 0): what a
		 * failover can adopt, or what perfd_command("enable") is called on */
		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "E1 a plaintext link");
		p->fd = sv[0];
		shutdown(sv[1], SHUT_WR);              /* the peer answers nothing */
		CHK(json_to_tree(p, "{\"secret\":\"the-enable-secret\"}", &w) == 0, "E2 the params");
		rc = roundtrip_once(p, "enable", w.b, w.n, &out, &n);
		fcntl(sv[1], F_SETFL, O_NONBLOCK);
		got = read(sv[1], seen, sizeof seen);
		CHK(rc != 0 && perfd_error_kind(p) == PERFD_E_STATE,
			"E3 enable on it is refused as a state error (%s)", perfd_error(p));
		CHK(got <= 0 || !holds(seen, (size_t)got, "the-enable-secret"),
			"E4 and the secret was not written to the link (%zd bytes were)", got);
		pc_tw_free(&w);
		close(sv[1]);
		perfd_free(p);
	}

	printf("--- F. an async handshake the daemon drops is an auth failure, not a transport one\n");
	{
		static const char *const secs[] = { "old-secret", "new-secret", NULL };
		perfd_opts o = PERFD_OPTS_INIT;
		perfd_t *p, *q;
		int sv[2] = { -1, -1 }, rc;

		o.spares = PERFD_SPARES_NONE;
		o.secrets = secs;
		p = pd_alloc(&o);
		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "F1 a link");
		/* what perfd_connect_async leaves once msg1 is out: waiting for msg2 */
		p->async = 1;
		p->st = PERFD_ST_CONNECTING;
		p->hs_stage = PD_HS_WAIT;
		p->fd = sv[0];
		fcntl(sv[0], F_SETFL, O_NONBLOCK);
		close(sv[1]);                  /* the daemon closes on a msg1 its secrets cannot open */
		rc = perfd_read_ready(p);
		CHK(rc == -1 && perfd_state(p) == PERFD_ST_FAILED && perfd_error_kind(p) == PERFD_E_AUTH,
			"F2 the handle fails as PERFD_E_AUTH (kind %d: %s)", perfd_error_kind(p), perfd_error(p));
		CHK(p->hs_sec == 0, "F3 having tried secret 0 (%d)", p->hs_sec);
		CHK(perfd_secret_index(p) == 0, "F3b perfd_secret_index says so (%d)", perfd_secret_index(p));
		/* the caller's next dial: the list from the secret after the one tried */
		o.secrets = secs + perfd_secret_index(p) + 1;
		q = pd_alloc(&o);
		CHK(q && q->nsec == 1 && !strcmp(q->sec[0], "new-secret"), "F4 the next dial tries new-secret");
		perfd_free(q);
		perfd_free(p);

		o.secrets = secs;
		p = pd_alloc(&o);
		sv[0] = sv[1] = -1;
		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "F5 a link");
		p->async = 1;
		p->st = PERFD_ST_READY;        /* past the handshake a close is the link's */
		p->hs_stage = PD_HS_DONE;
		p->fd = sv[0];
		fcntl(sv[0], F_SETFL, O_NONBLOCK);
		close(sv[1]);
		rc = perfd_read_ready(p);
		CHK(rc == -1 && perfd_error_kind(p) == PERFD_E_TRANSPORT,
			"F6 a ready link the daemon closes stays PERFD_E_TRANSPORT (kind %d)", perfd_error_kind(p));
		perfd_free(p);
	}

	printf("--- G. an async write to a peer that closed fails the handle, it does not raise SIGPIPE\n");
	{
		perfd_t *p = mk();
		int sv[2] = { -1, -1 }, st = -1;
		pid_t pid;

		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "G1 a link");
		p->async = 1;
		p->st = PERFD_ST_READY;
		p->hs_stage = PD_HS_DONE;
		p->fd = sv[0];
		close(sv[1]);
		fflush(stdout);
		pid = fork();
		if (pid == 0) {
			signal(SIGPIPE, SIG_DFL);      /* a host program that never ignored it */
			_exit(wbuf_put(p, "x", 1) == 0 && wbuf_push(p) < 0 ? 0 : 1);
		}
		if (pid > 0)
			waitpid(pid, &st, 0);
		CHK(pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
			"G2 wbuf_push returns the error and the process lives (signal %d)",
			pid > 0 && WIFSIGNALED(st) ? WTERMSIG(st) : 0);
		perfd_free(p);
	}

	printf("--- H. a submit that fails reports its request once: by -1, or by its callback\n");
	{
		perfd_t *p = mk();
		int sv[2] = { -1, -1 }, first = 0, second = 0, third = 0, rc;

		signal(SIGPIPE, SIG_IGN);
		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "H1 a link");
		p->async = 1;
		p->st = PERFD_ST_READY;
		p->hs_stage = PD_HS_DONE;
		p->fd = sv[0];
		rc = perfd_submit_kv(p, BV_GET, "c", "k1", NULL, 0, 0, 0, cb_count, &first);
		CHK(rc == 0 && first == 0, "H2 a request batched, its callback pending");
		close(sv[1]);
		p->eager_push = 1;
		rc = perfd_submit_kv(p, BV_GET, "c", "k2", NULL, 0, 0, 0, cb_count, &second);
		CHK(rc == -1, "H3 an eager push to the closed peer fails (%s)", perfd_error(p));
		CHK(second == 0, "H4 and that request's callback was not called too (%d calls)", second);
		CHK(first == 1, "H5 the request already in flight is failed through its callback (%d)", first);
		perfd_free(p);

		p = mk();
		p->async = 1;
		p->st = PERFD_ST_READY;
		p->hs_stage = PD_HS_DONE;
		call_add(p, 7, cb_count, &third, 0);
		p->poisoned = 1;               /* as a failed allocation leaves it */
		rc = perfd_read_ready(p);
		CHK(rc == -1 && third == 1,
			"H6 an entry point that finds the handle poisoned fails the calls in flight (%d)", third);
		perfd_free(p);
	}

	printf("--- I. a named handle that only uses typed verbs sends its name\n");
	{
		perfd_opts o = PERFD_OPTS_INIT;
		perfd_t *p;
		char seen[4096];
		ssize_t got;
		int sv[2] = { -1, -1 }, rc;

		o.spares = PERFD_SPARES_NONE;
		o.name = "named-by-opts";
		p = pd_alloc(&o);
		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "I1 a link");
		p->fd = sv[0];
		shutdown(sv[1], SHUT_WR);              /* the peer answers nothing */
		rc = perfd_set(p, "c", "k", "v", 1, 0);
		(void)rc;                              /* the peer answers nothing */
		fcntl(sv[1], F_SETFL, O_NONBLOCK);
		got = read(sv[1], seen, sizeof seen);
		CHK(got > 0 && holds(seen, (size_t)got, "named-by-opts"),
			"I2 opts.name went out before the set (%zd bytes, %s)", got,
			got > 0 && holds(seen, (size_t)got, "named-by-opts") ? "named" : "no name");
		close(sv[1]);
		perfd_free(p);
	}

	printf("--- J. a notify hook that calls its own handle\n");
	{
		perfd_t *p = mk();
		unsigned char tb[128], wire[512];
		struct pc_tw w;
		struct hk h = { NULL, 0, 0, -1, 0 };
		size_t n;
		int sv[2] = { -1, -1 };

		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "J1 a link");
		p->fd = sv[0];
		h.p = p;
		perfd_set_notify(p, hook_calls_back, &h);
		pc_tw_init(&w, tb, sizeof tb);
		pc_tw_map(&w);
		pc_tw_key(&w, "notify");
		pc_tw_str(&w, "message");
		pc_tw_key(&w, "channel");
		pc_tw_str(&w, "ch");
		pc_tw_end(&w);
		p->next_id = 40;
		/* a delivery read between calls, then the pong to the hook's ping (id 41) */
		n = frame(wire, BIN_NOTIFY, 0, tb, w.n);
		n += frame(wire + n, BIN_RSP, 41, "perfd", 5);
		CHK(write(sv[1], wire, n) == (ssize_t)n, "J2 the node sent a message and a pong");
		drain_link(p);
		CHK(h.calls == 1, "J3 the hook ran once for the one message (%d)", h.calls);
		CHK(h.rc == 0, "J4 its ping on the handle was answered (%s)", perfd_error(p));
		CHK(p->roff <= p->rlen && !p->poisoned,
			"J5 the stream is intact after it (roff %zu, rlen %zu, poisoned %d)",
			p->roff, p->rlen, p->poisoned);
		close(sv[1]);
		perfd_free(p);
	}

	printf("--- K. a failover is bounded in total, not per standby\n");
	{
		perfd_t *p = mk();
		int sv[PERFD_MAX_SPARES][2], i, rc;
		long long t0, took;

		p->io_ms = 300;
		p->active_node = 1;
		for (i = 0; i < PERFD_MAX_SPARES; i++) {
			perfd_t *c = mk();

			sv[i][0] = sv[i][1] = -1;
			if (!c || socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) != 0)
				break;
			c->fd = sv[i][0];          /* a standby whose node went silent */
			c->is_child = 1;
			c->io_ms = 300;
			c->active_node = i + 2;
			p->spare[p->nspare++] = c;
		}
		CHK(p->nspare == PERFD_MAX_SPARES, "K1 %d silent standbys", p->nspare);
		t0 = now_ms_lib();
		rc = failover(p);
		took = now_ms_lib() - t0;
		CHK(rc != 0 && took < 1000,
			"K2 the failover gives up within about the call's io timeout of 300 ms (%lld ms)", took);
		for (i = 0; i < PERFD_MAX_SPARES; i++)
			if (sv[i][1] >= 0)
				close(sv[i][1]);
		perfd_free(p);
	}

	printf("--- L. a blocking write to a peer that closed fails the handle, it does not raise SIGPIPE\n");
	{
		perfd_t *p = mk();
		int sv[2] = { -1, -1 }, st = -1;
		pid_t pid;

		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "L1 a link");
		p->fd = sv[0];
		close(sv[1]);
		fflush(stdout);
		pid = fork();
		if (pid == 0) {
			signal(SIGPIPE, SIG_DFL);
			_exit(io_all(p, 1, "x", 1) != 0 && p->poisoned ? 0 : 1);
		}
		if (pid > 0)
			waitpid(pid, &st, 0);
		CHK(pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
			"L2 io_all returns the error and the process lives (signal %d)",
			pid > 0 && WIFSIGNALED(st) ? WTERMSIG(st) : 0);
		perfd_free(p);
	}

	printf("--- M. only a reply some request waits for is parked\n");
	{
		perfd_t *p = mk();
		unsigned char wire[512];
		struct early *e;
		char *out = NULL;
		size_t n = 0, w = 0;
		int sv[2] = { -1, -1 }, rc, parked = 0;

		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "M1 a link");
		p->fd = sv[0];
		push_id(p, 10);
		push_id(p, 11);
		push_id(p, 12);
		w = frame(wire, BIN_RSP, 99, "stray", 5);      /* never issued */
		w += frame(wire + w, BIN_RSP, 12, "twelve", 6);
		w += frame(wire + w, BIN_RSP, 12, "again!", 6); /* a duplicate */
		w += frame(wire + w, BIN_RSP, 10, "ten", 3);
		CHK(write(sv[1], wire, w) == (ssize_t)w, "M2 the node sent four replies");
		rc = collect_head(p, &out, &n);
		CHK(rc == 0 && n == 3 && !memcmp(out, "ten", 3), "M3 the head's reply is collected");
		for (e = p->early; e; e = e->next)
			parked++;
		CHK(parked == 1 && p->early && p->early->id == 12,
			"M4 only the one reply to 12 is parked, not 99's nor the duplicate (%d parked)", parked);
		close(sv[1]);
		perfd_free(p);
	}

	printf("--- N. an hgetall callback that calls its handle\n");
	{
		static const unsigned char hash[] = { 'a', 4, 0, 0, 0,
			'b', 2, 0, 0, 0, 'f', '1', 'b', 2, 0, 0, 0, 'v', '1',
			'b', 2, 0, 0, 0, 'f', '2', 'b', 2, 0, 0, 0, 'v', '2' };
		unsigned char miss[sizeof hash], wire[512];
		perfd_t *p = mk();
		struct hg h = { NULL, 0, -1, "" };
		size_t w;
		int sv[2] = { -1, -1 }, rc;

		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "N1 a link");
		p->fd = sv[0];
		h.p = p;
		p->next_id = 60;
		memset(miss, 'X', sizeof miss);        /* a GET reply as long as the hash's */
		miss[0] = 0;
		w = frame(wire, BIN_RSP, 61, hash, sizeof hash);
		w += frame(wire + w, BIN_RSP, 62, miss, sizeof miss);
		CHK(write(sv[1], wire, w) == (ssize_t)w, "N2 the node sent the hash and the get's miss");
		rc = perfd_hgetall(p, "c", "h", hg_calls_back, &h);
		CHK(h.rc == 0, "N3 the callback's get was answered (%s)", perfd_error(p));
		CHK(rc == 2 && h.n == 2 && !strcmp(h.f2, "f2"),
			"N4 hgetall still hands both fields over (rc %d, %d calls, second field '%s': %s)",
			rc, h.n, h.f2, rc < 0 ? perfd_error(p) : "");
		close(sv[1]);
		perfd_free(p);
	}

	printf("--- O. a signal while a plaintext call waits for its reply\n");
	{
		struct sigaction sa, old;
		struct itimerval it;
		struct timeval tv = { 0, 100000 };
		unsigned char wire[64];
		perfd_t *p = mk();
		char *out = NULL;
		size_t n = 0, w;
		int sv[2] = { -1, -1 }, rc, st;
		pid_t pid;

		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "O1 a link");
		p->fd = sv[0];
		push_id(p, 5);
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = on_alarm;              /* no SA_RESTART: the read returns EINTR */
		sigaction(SIGALRM, &sa, &old);
		w = frame(wire, BIN_RSP, 5, "late", 4);
		fflush(stdout);
		pid = fork();
		if (pid == 0) {
			usleep(300000);
			_exit(write(sv[1], wire, w) == (ssize_t)w ? 0 : 1);
		}
		memset(&it, 0, sizeof it);
		it.it_value.tv_usec = 100000;
		setitimer(ITIMER_REAL, &it, NULL);
		rc = collect_head(p, &out, &n);
		if (pid > 0)
			waitpid(pid, &st, 0);
		sigaction(SIGALRM, &old, NULL);
		CHK(rc == 0 && n == 4 && !memcmp(out, "late", 4) && !p->poisoned,
			"O2 the signal does not poison the link: the reply is read (%s)", rc ? perfd_error(p) : "ok");
		close(sv[1]);
		perfd_free(p);

		p = mk();
		sv[0] = sv[1] = -1;
		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "O3 a link");
		p->fd = sv[0];
		setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		push_id(p, 6);
		rc = collect_head(p, &out, &n);
		CHK(rc != 0 && p->io_silent, "O4 a reply that never comes is a silent link, as io_all says (io_silent %d)",
			p->io_silent);
		close(sv[1]);
		perfd_free(p);
	}

	printf("--- P. a failed dial frees what the handle had made\n");
	{
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
		static const char *const secs[] = { "s1", "s2", NULL };
		static const char *const nowhere = "/nonexistent/perfdlinktest.sock";
		perfd_opts o = PERFD_OPTS_INIT;
		struct mallinfo2 a, b;
		perfd_t *q;
		int i, got = 0;

		o.route_hints = 1;                     /* a 98 KB holder table a handle */
		o.name = "leaky";
		o.secrets = secs;
		q = dial_one(NULL, 0, nowhere, &o);    /* warm the allocator */
		a = mallinfo2();
		for (i = 0; i < 40; i++)
			if ((q = dial_one(NULL, 0, nowhere, &o)) != NULL)
				got++;
		b = mallinfo2();
		CHK(!got && b.uordblks < a.uordblks + 100000,
			"P1 forty failed dials leave nothing allocated (%zu bytes more in use)",
			b.uordblks > a.uordblks ? b.uordblks - a.uordblks : 0);
		(void)q;
#else
		printf("  skip P: no mallinfo2 here\n");
#endif
	}

	printf("--- Q. a failed async connect leaves its reason where perfd_error(NULL) reads\n");
	{
		pid_t pid;
		int st = -1;

		fflush(stdout);
		pid = fork();
		if (pid == 0) {
			struct rlimit rl;
			perfd_opts o = PERFD_OPTS_INIT;
			perfd_t *q;
			int fd;

			errk(NULL, PERFD_E_NONE, "an older failure");
			for (fd = 3; fd < 1024; fd++)
				close(fd);
			getrlimit(RLIMIT_NOFILE, &rl);
			rl.rlim_cur = 3;               /* no socket can be made */
			setrlimit(RLIMIT_NOFILE, &rl);
			q = perfd_connect_async("127.0.0.1", 1, &o);
			_exit(!q && strstr(perfd_error(NULL), "cannot") &&
				perfd_error_kind(NULL) == PERFD_E_TRANSPORT ? 0 : 1);
		}
		if (pid > 0)
			waitpid(pid, &st, 0);
		CHK(pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
			"Q1 perfd_connect_async returns NULL with its own reason, not an older one (exit %d)",
			pid > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	}

	printf("--- R. the UDP window drops what has left it\n");
	{
		struct pd_udp u;
		uint64_t seq;

		memset(&u, 0, sizeof u);
		for (seq = 1; seq <= 100; seq++)
			udp_window(&u, seq);
		CHK(udp_window(&u, 100) == 0, "R1 a repeat inside the window is a duplicate");
		CHK(udp_window(&u, 37) == 0, "R2 a datagram 63 behind the highest, seen, is a duplicate");
		CHK(udp_window(&u, 10) == 0, "R3 one 90 behind the highest is not delivered again");
		CHK(udp_window(&u, 101) == 1, "R4 the next one is new");
	}

	printf("--- S. perfd_ttl's no expiry after maintenance inside the call failed\n");
	{
		unsigned char wire[64], v[8];
		perfd_t *p = mk();
		long long t;
		size_t w;
		int sv[2] = { -1, -1 };

		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "S1 a link");
		p->fd = sv[0];
		p->next_id = 70;
		wle64(v, (unsigned long long)-1LL);    /* the key has no expiry */
		w = frame(wire, BIN_RSP, 71, v, sizeof v);
		CHK(write(sv[1], wire, w) == (ssize_t)w, "S2 the node answered -1");
		shutdown(sv[1], SHUT_WR);              /* and closed: the drain after the reply fails */
		t = perfd_ttl(p, "c", "k");
		CHK(t == -1 && perfd_error_kind(p) == PERFD_E_NONE,
			"S3 -1 with the kind NONE: the answer, not an error (%lld, kind %d: %s)",
			t, perfd_error_kind(p), perfd_error(p));
		close(sv[1]);
		perfd_free(p);
	}

	printf("--- T. perfd_jset_ex refuses flags it does not know before any I/O\n");
	{
		perfd_t *p = mk();

		CHK(p && perfd_jset_ex(p, "c", "k", "$", "1", 0, PERFD_JSET_NX | PERFD_JSET_XX) == -1 &&
			perfd_error_kind(p) == PERFD_E_ARG, "T1 NX and XX together: PERFD_E_ARG");
		CHK(p && perfd_jset_ex(p, "c", "k", "$", "1", 0, 4) == -1 &&
			perfd_error_kind(p) == PERFD_E_ARG, "T2 an unknown flag: PERFD_E_ARG");
		perfd_free(p);
	}

	printf("--- U. perfd_fleet_changes moves with the member list and a member's state; perfd_describe\n");
	{
		perfd_t *p = mk();
		struct pc_tw w;
		char line[160], small[12];
		unsigned long long c0;
		int i, n;

		pc_tw_init(&w, NULL, 0);
		pc_tw_map(&w);
		pc_tw_key(&w, "version");
		pc_tw_str(&w, "0.7.5");
		pc_tw_key(&w, "members");
		pc_tw_arr(&w);
		for (i = 1; i <= 2; i++) {
			pc_tw_map(&w);
			pc_tw_key(&w, "addr");
			pc_tw_str(&w, i == 1 ? "10.9.0.1" : "10.9.0.2");
			pc_tw_key(&w, "port");
			pc_tw_i64(&w, 7000 + i);
			pc_tw_key(&w, "node");
			pc_tw_i64(&w, i);
			pc_tw_key(&w, "state");
			pc_tw_str(&w, "ready");
			pc_tw_key(&w, "self");
			pc_tw_bool(&w, i == 1);
			pc_tw_end(&w);
		}
		pc_tw_end(&w);
		pc_tw_end(&w);
		c0 = perfd_fleet_changes(p);
		CHK(c0 == 0 && parse_members(p, w.b, w.n) == 2 && perfd_fleet_changes(p) == 1,
			"U1 the first member list learned is a change (%llu)", perfd_fleet_changes(p));
		CHK(parse_members(p, w.b, w.n) == 2 && perfd_fleet_changes(p) == 1,
			"U2 the same list again is not (%llu)", perfd_fleet_changes(p));
		pc_tw_free(&w);

		/* membership pushes, as the daemon sends them */
#define PUSH(ev, node, st) do { pc_tw_init(&w, NULL, 0); pc_tw_map(&w); \
		pc_tw_key(&w, "notify"); pc_tw_str(&w, "membership"); \
		pc_tw_key(&w, "event"); pc_tw_str(&w, ev); \
		pc_tw_key(&w, "addr"); pc_tw_str(&w, "10.9.0.3"); \
		pc_tw_key(&w, "port"); pc_tw_i64(&w, 7003); \
		pc_tw_key(&w, "node"); pc_tw_i64(&w, node); \
		pc_tw_key(&w, "state"); pc_tw_str(&w, st); pc_tw_end(&w); \
		notify_internal(p, w.b, w.n); pc_tw_free(&w); } while (0)
		PUSH("state", 2, "recovering");
		CHK(perfd_fleet_changes(p) == 2, "U3 a member's state pushed (%llu)", perfd_fleet_changes(p));
		PUSH("state", 2, "recovering");
		CHK(perfd_fleet_changes(p) == 2, "U4 the same state again counts nothing");
		PUSH("joined", 3, "starting");
		CHK(perfd_fleet_changes(p) == 3 && p->nmem == 3, "U5 a member joined (%llu, %d)",
			perfd_fleet_changes(p), p->nmem);
		PUSH("expelled", 3, "ready");
		CHK(perfd_fleet_changes(p) == 4 && p->nmem == 2, "U6 a member expelled (%llu)",
			perfd_fleet_changes(p));
#undef PUSH
		member_down(p, 2, "", "no answer");
		CHK(perfd_fleet_changes(p) == 5, "U7 a member this handle found down (%llu)",
			perfd_fleet_changes(p));
		member_down(p, 2, "", "no answer again");
		CHK(perfd_fleet_changes(p) == 5, "U8 found down again: no change");

		n = perfd_describe(p, line, sizeof line);
		CHK(!strcmp(line, "perfcached 0.7.5, 2 members, routing off, 0 standbys") &&
			n == (int)strlen(line), "U9 described: %s", line);
		CHK(perfd_describe(p, small, sizeof small) == n && strlen(small) == sizeof small - 1,
			"U10 a short buffer: cut and terminated, the length it needed returned");
		p->srv_version[0] = 0;
		p->nmem = 1;
		p->route_ok = 1;
		perfd_describe(p, line, sizeof line);
		CHK(!strcmp(line, "perfcached (version unknown), 1 member, routing on, 0 standbys"),
			"U11 %s", line);
		CHK(perfd_describe(NULL, line, sizeof line) == -1 && perfd_fleet_changes(NULL) == 0,
			"U12 a NULL handle");
		perfd_free(p);
	}

	printf("--- V. a submit whose frame cannot be buffered takes its request back out\n");
	{
		perfd_t *p = mk();
		int a = 0, b = 0, rc;

		p->async = 1;
		p->st = PERFD_ST_READY;
		p->hs_stage = PD_HS_DONE;
		rc = perfd_submit_kv(p, BV_GET, "c", "k1", NULL, 0, 0, 0, cb_count, &a);
		CHK(rc == 0 && p->ncalls == 1, "V1 one request buffered");
		/* a full write buffer whose growth fails */
		{
			size_t had = p->wlen;

			p->wlen = p->wcap;
			realloc_fails = 1;
			rc = perfd_submit_kv(p, BV_GET, "c", "k2", NULL, 0, 0, 0, cb_count, &b);
			realloc_fails = 0;
			p->wlen = had;
		}
		CHK(rc == -1 && perfd_error_kind(p) == PERFD_E_NOMEM, "V2 the second submit fails (%s)",
			perfd_error(p));
		CHK(p->ncalls == 1 && p->calls[0].ctx == &a, "V3 and its entry is gone, the first's kept");
		rc = perfd_read_ready(p);
		CHK(rc == -1 && a == 1 && b == 0,
			"V4 the poisoned handle fails the first through its callback, never the second (%d, %d)",
			a, b);
		perfd_free(p);
	}

	printf("--- W. a notify hook that calls its own handle inside a call is refused\n");
	{
		perfd_t *p = mk();
		unsigned char tb[128], wire[512];
		struct pc_tw w;
		struct hk h = { NULL, 0, 0, -1, 0 };
		size_t n;
		int sv[2] = { -1, -1 }, rc;

		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "W1 a link");
		p->fd = sv[0];
		h.p = p;
		perfd_set_notify(p, hook_calls_back, &h);
		pc_tw_init(&w, tb, sizeof tb);
		pc_tw_map(&w);
		pc_tw_key(&w, "notify");
		pc_tw_str(&w, "message");
		pc_tw_key(&w, "channel");
		pc_tw_str(&w, "ch");
		pc_tw_end(&w);
		p->next_id = 80;
		/* a delivery that overtakes the pong to the caller's own ping (id 81) */
		n = frame(wire, BIN_NOTIFY, 0, tb, w.n);
		n += frame(wire + n, BIN_RSP, 81, "perfd", 5);
		CHK(write(sv[1], wire, n) == (ssize_t)n, "W2 the node sent a message and the pong");
		shutdown(sv[1], SHUT_WR);              /* nothing more comes */
		rc = perfd_ping(p);
		CHK(h.calls == 1, "W3 the hook ran once (%d)", h.calls);
		CHK(h.rc != 0 && h.kind == PERFD_E_STATE,
			"W4 its ping on the handle, inside the caller's ping, is refused as a state error (rc %d, kind %d)",
			h.rc, h.kind);
		CHK(rc == 0, "W5 the caller's ping still gets its own pong (%s)", rc ? perfd_error(p) : "ok");
		close(sv[1]);
		perfd_free(p);
	}

	printf("--- X. a notify hook run in the drain after a call's reply may not call its handle\n");
	{
		static const unsigned char outer[] = { 1, 0, 0, 0, 0, 'o', 'u', 't', 'e', 'r' };
		static const unsigned char nested[] = { 1, 0, 0, 0, 0, 'n', 'e', 's', 't', 'd' };
		perfd_t *p = mk();
		unsigned char tb[128], wire[256];
		struct pc_tw w;
		struct hget h = { NULL, 0, -1, 0 };
		void *v = NULL;
		size_t n, vl = 0;
		int sv[2] = { -1, -1 }, rc, ok = 1;

		/* SEQPACKET: one read takes one frame, so the delivery is left for the drain to read */
		CHK(p && socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "X1 a link");
		p->fd = sv[0];
		h.p = p;
		perfd_set_notify(p, hook_gets, &h);
		pc_tw_init(&w, tb, sizeof tb);
		pc_tw_map(&w);
		pc_tw_key(&w, "notify");
		pc_tw_str(&w, "message");
		pc_tw_key(&w, "channel");
		pc_tw_str(&w, "ch");
		pc_tw_end(&w);
		p->next_id = 90;
		n = frame(wire, BIN_RSP, 91, outer, sizeof outer);
		ok &= write(sv[1], wire, n) == (ssize_t)n;
		n = frame(wire, BIN_NOTIFY, 0, tb, w.n);
		ok &= write(sv[1], wire, n) == (ssize_t)n;
		n = frame(wire, BIN_RSP, 92, nested, sizeof nested);    /* for a nested get, if sent */
		ok &= write(sv[1], wire, n) == (ssize_t)n;
		CHK(ok, "X2 the node sent the reply, then a message, then a second reply");
		rc = perfd_get(p, "c", "k", &v, &vl, NULL);
		CHK(h.calls == 1, "X3 the hook ran in the drain after the reply (%d)", h.calls);
		CHK(h.rc != 0 && h.kind == PERFD_E_STATE,
			"X4 its get on the handle is refused as a state error (rc %d, kind %d)", h.rc, h.kind);
		CHK(rc == 1 && vl == 5 && !memcmp(v, "outer", 5),
			"X5 the caller's get returns its own value, not the nested call's ('%.*s')",
			rc == 1 ? (int)vl : 0, rc == 1 ? (const char *)v : "");
		free(v);
		close(sv[1]);
		perfd_free(p);
	}

	printf("--- Y. a node that refuses the connection before the handshake is a refusal, not auth\n");
	{
		static const char *const secs[] = { "a-secret", NULL };
		static const char full[] = "-ERR max number of clients reached\r\n";
		perfd_opts o = PERFD_OPTS_INIT;
		perfd_t *p;
		char why[128] = "";
		int sv[2] = { -1, -1 }, rc, k;

		o.spares = PERFD_SPARES_NONE;
		o.secrets = secs;
		for (k = 0; k < 2; k++) {      /* the line then the close; the line alone */
			p = pd_alloc(&o);
			sv[0] = sv[1] = -1;
			CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "Y%d a link", 1 + 3 * k);
			p->async = 1;
			p->st = PERFD_ST_CONNECTING;
			p->hs_stage = PD_HS_WAIT;
			p->fd = sv[0];
			fcntl(sv[0], F_SETFL, O_NONBLOCK);
			CHK(write(sv[1], full, sizeof full - 1) == (ssize_t)(sizeof full - 1),
				"Y%d the node writes its refusal%s", 2 + 3 * k, k ? ", the link left open" : " and closes");
			if (!k)
				close(sv[1]);
			rc = perfd_read_ready(p);
			CHK(rc == -1 && perfd_state(p) == PERFD_ST_FAILED &&
				perfd_error_kind(p) == PERFD_E_REFUSED && strstr(perfd_error(p), "max number of clients"),
				"Y%d async: PERFD_E_REFUSED with the node's words (kind %d: %s)", 3 + 3 * k,
				perfd_error_kind(p), perfd_error(p));
			if (k)
				close(sv[1]);
			perfd_free(p);
		}
		p = pd_alloc(&o);
		sv[0] = sv[1] = -1;
		CHK(p && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "Y7 a link");
		if (p && sv[0] >= 0) {
			p->fd = sv[0];
			if (write(sv[1], full, sizeof full - 1) < 0)
				fails++;
			shutdown(sv[1], SHUT_WR);      /* the close a reader sees; msg1 still lands */
			rc = noise_handshake(p, secs[0], why, sizeof why);
			close(sv[1]);
			CHK(rc == -3 && !strcmp(why, "max number of clients reached"),
				"Y8 blocking: the handshake says refused, with the node's words (%d, '%s')", rc, why);
		}
		perfd_free(p);
	}

	printf("perfdlinktest: %s\n", fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
