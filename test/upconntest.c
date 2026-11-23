/* upconntest.c - the read-through's RESP client (upconn.c) over an AF_UNIX socketpair standing in
 * for the upstream: the server's replies are written before each call, what was sent read back. */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "compat/dprint.h"
#include "uprespc.h"
#include "upstream.h"
#include "upstreamint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs: what upconn.c calls back, and the log ------------------- */
int compat_log_level = L_NOTICE;
static int notices, warns;
static char last_log[256];

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	va_list ap;

	(void)tag;
	va_start(ap, fmt);
	vsnprintf(last_log, sizeof last_log, fmt, ap);
	va_end(ap);
	if (level == L_NOTICE)
		notices++;
	else if (level == L_WARN)
		warns++;
}

static int pushes, flushes;
static char push_kind[32];
static int sseq[4], sn;                 /* srv_state's answers, in turn */

void on_push(struct uconn *c, struct rv *v)
{
	(void)c;
	pushes++;
	snprintf(push_kind, sizeof push_kind, "%.*s",
		v->nel > 0 && v->el[0].s ? (int)v->el[0].len : 0,
		v->nel > 0 && v->el[0].s ? v->el[0].s : "");
}

void flush_shadow(struct up_srv *s) { (void)s; flushes++; }
int srv_state(const struct up_srv *s) { (void)s; return sseq[sn < 4 ? sn++ : 3]; }
void srv_discovered(struct up_srv *m, char hosts[][64], const int *ports, int n)
{
	(void)m; (void)hosts; (void)ports; (void)n;
}
void sen_start(struct up_col *uc) { (void)uc; }
void cl_start(struct up_col *uc) { (void)uc; }
struct up_tls_conn *uptls_connect(struct up_tls *t, int fd, const char *name, long long dl,
		char *why, size_t wl) { (void)t; (void)fd; (void)name; (void)dl; (void)why; (void)wl; return NULL; }
ssize_t uptls_read(struct up_tls_conn *c, void *p, size_t n, short *ev, char *why, size_t wl)
{ (void)c; (void)p; (void)n; (void)ev; (void)why; (void)wl; return -1; }
ssize_t uptls_write(struct up_tls_conn *c, const void *p, size_t n, short *ev)
{ (void)c; (void)p; (void)n; (void)ev; return -1; }
int uptls_pending(struct up_tls_conn *c) { (void)c; return 0; }
void uptls_close(struct up_tls_conn *c) { (void)c; }
int pc_resolve_family(void) { return 0; }

/* ---- the harness ------------------------------------------------------- */
static struct up_col uc;
static struct up_srv srv;
static struct uconn c;
static int peer = -1;

static void open_pair(void)
{
	int sv[2];

	if (c.fd >= 0)
		close(c.fd);
	if (peer >= 0)
		close(peer);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		perror("socketpair");
		exit(2);
	}
	c.fd = sv[0];
	peer = sv[1];
	c.len = c.off = 0;
}

/* the server's side: queue @s for the client to read */
static void serve(const char *s, size_t n)
{
	while (n) {
		ssize_t w = write(peer, s, n);

		if (w <= 0) {
			perror("write");
			exit(2);
		}
		s += w;
		n -= (size_t)w;
	}
}
#define SERVE(lit) serve((lit), sizeof(lit) - 1)

/* a reply larger than a socket buffer, written from a thread while the client reads */
static const char *srv_big;
static size_t srv_big_n;

static void *serve_big(void *a)
{
	(void)a;
	serve(srv_big, srv_big_n);
	return NULL;
}

/* what the client sent so far, as a string; its length in sent_len */
static size_t sent_len;

static const char *sent(void)
{
	static char b[4096];
	ssize_t r = recv(peer, b, sizeof b - 1, MSG_DONTWAIT);

	sent_len = r > 0 ? (size_t)r : 0;
	b[sent_len] = 0;
	return b;
}

static long long dl(void) { return now_ms() + 2000; }

int main(void)
{
	struct rv v;
	int rc;

	memcpy(uc.name, "c", 2);
	uc.nlen = 1;
	uc.max_lag_ms = 1000;
	uc.discover = PC_UP_DISC_NO;       /* the scripted handshake has no CLIENT INFO */
	srv.uc = &uc;
	memcpy(srv.addr, "10.0.0.1:6379", 14);
	pthread_mutex_init(&srv.run_mx, NULL);
	c.uc = &uc;
	c.s = &srv;
	c.fd = -1;
	open_pair();

	/* A: send_cmd - a RESP array of bulks, byte for byte; binary arguments carried by length */
	{
		static const char want[] = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\na\r\n\0b\r\n";
		const char *av[3] = { "SET", "k", "a\r\n\0b" };
		size_t al[3] = { 3, 1, 5 };
		const char *got;

		rc = send_cmd(&c, dl(), 3, av, al);
		got = sent();
		CHK(rc == 0 && sent_len == sizeof want - 1 && !memcmp(got, want, sizeof want - 1),
			"A: SET k <5 binary bytes> - a RESP array of bulks, byte for byte (%zu bytes)",
			sent_len);
	}

	/* B: the reply reader - pushes, spans, big replies; -3 past rv_max, -2 late, -1 gone */
	SERVE("+OK\r\n");
	CHK(read_reply(&c, &v, dl()) == 0 && is_ok(&v), "B: +OK read");
	rv_free(&v);
	SERVE(">2\r\n$10\r\ninvalidate\r\n*1\r\n$1\r\nk\r\n$1\r\nv\r\n");
	{
		size_t sp = 0, ep = 0;

		rc = read_reply_span(&c, &v, dl(), &sp, &ep);
		CHK(rc == 0 && v.t == '$' && v.len == 1 && v.s[0] == 'v' && pushes == 1 &&
			!strcmp(push_kind, "invalidate"),
			"B: a push before the reply goes to on_push, the reply after it is returned");
		CHK(ep - sp == 7 && !memcmp(c.buf + sp, "$1\r\nv\r\n", 7), "B: and its span in the buffer");
		rv_free(&v);
	}
	{
		/* 70,000: past the first 64 KB buffer, and inside any AF_UNIX
		 * send buffer - this side writes it all before the client reads */
		static char big[70100];
		size_t i;

		memcpy(big, "$70000\r\n", 8);
		for (i = 0; i < 70000; i++)
			big[8 + i] = (char)('a' + i % 26);
		memcpy(big + 70008, "\r\n", 2);
		serve(big, 70010);
		rc = read_reply(&c, &v, dl());
		CHK(rc == 0 && v.len == 70000 && v.s[0] == 'a' &&
			v.s[69999] == (char)('a' + 69999 % 26) && c.cap > 65536,
			"B: a 70,000-byte bulk, past the first 64 KB buffer, read whole");
		rv_free(&v);
	}
	rv_max = 4;
	SERVE("$10\r\n0123456789\r\n");
	CHK(read_reply(&c, &v, dl()) == -3, "B: a bulk over this thread's rv_max (4) is -3");
	rv_max = UP_RBUF_MAX;
	open_pair();
	{
		long long t0 = now_ms();

		rc = read_reply(&c, &v, now_ms() + 50);
		CHK(rc == -2 && now_ms() - t0 >= 50, "B: nothing by the deadline is -2");
	}
	close(peer);
	peer = -1;
	CHK(read_reply(&c, &v, dl()) == -1, "B: the server gone is -1");
	open_pair();

	/* C: role_check - ROLE master, a replica's link, lag and master, "other"; changes logged */
	sn = 0;
	sseq[0] = PC_UPS_SERVE; sseq[1] = PC_UPS_SERVE;
	SERVE("*3\r\n$6\r\nmaster\r\n:0\r\n*0\r\n");
	rc = role_check(&c, dl());
	CHK(rc == 0 && srv.role == UPR_MASTER && srv.lag_ms == -1 && srv.checked_ms > 0 &&
		!strcmp(sent(), "*1\r\n$4\r\nROLE\r\n"), "C: ROLE master - only ROLE asked");
	sn = 0;
	SERVE("*5\r\n$5\r\nslave\r\n$8\r\n10.1.2.3\r\n:6380\r\n$9\r\nconnected\r\n:100\r\n");
	SERVE("$120\r\n# Replication\r\nrole:slave\r\nmaster_host:10.1.2.3\r\nmaster_port:6380\r\n"
		"master_link_status:up\r\nmaster_last_io_seconds_ago:2\r\n\r\n");
	rc = role_check(&c, dl());
	CHK(rc == 0 && srv.role == UPR_REPLICA && !srv.link_down && srv.lag_ms == 2000 &&
		!strcmp(srv.mhost, "10.1.2.3") && srv.mport == 6380,
		"C: a replica - link up, lag 2 s as 2000 ms, its master 10.1.2.3:6380");
	CHK(!strcmp(sent(), "*1\r\n$4\r\nROLE\r\n*2\r\n$4\r\nINFO\r\n$11\r\nreplication\r\n"),
		"C: ROLE, then INFO replication");
	sn = 0;
	SERVE("*5\r\n$5\r\nslave\r\n$8\r\n10.1.2.3\r\n:6380\r\n$9\r\nconnected\r\n:100\r\n");
	SERVE("$56\r\nmaster_link_status:down\r\nmaster_last_io_seconds_ago:-1\r\n\r\n");
	rc = role_check(&c, dl());
	CHK(rc == 0 && srv.role == UPR_REPLICA && srv.link_down == 1, "C: link down");
	(void)sent();
	sn = 0;
	SERVE("*1\r\n$8\r\nsentinel\r\n");
	rc = role_check(&c, dl());
	CHK(rc == 0 && srv.role == UPR_OTHER, "C: neither master nor replica - other");
	(void)sent();
	sn = 0;
	sseq[0] = PC_UPS_SERVE; sseq[1] = PC_UPS_OFF;
	notices = warns = 0;
	SERVE("*3\r\n$6\r\nmaster\r\n:0\r\n*0\r\n");
	rc = role_check(&c, dl());
	CHK(rc == 0 && notices == 1 && srv.refused_said == 1 &&
		strstr(last_log, "is a master - not used") != NULL,
		"C: a master leaving the usable set - said once: %s", last_log);
	(void)sent();
	sn = 0;
	sseq[0] = PC_UPS_OFF; sseq[1] = PC_UPS_FALLBACK;
	SERVE("*3\r\n$6\r\nmaster\r\n:0\r\n*0\r\n");
	rc = role_check(&c, dl());
	CHK(rc == 0 && notices == 2 && srv.refused_said == 0 &&
		strstr(last_log, "taken back") != NULL, "C: and its return - said once");
	(void)sent();

	/* D: uc_close - counted down; a tracked fetch link's copies flushed; twice is harmless */
	srv.conns_up = 2;
	c.tracked = 1;
	c.inv = 0;
	flushes = 0;
	uc_close(&c);
	CHK(c.fd == -1 && srv.conns_up == 1 && flushes == 1 && !c.tracked && !c.len,
		"D: a tracked fetch connection - closed, counted down, its copies flushed");
	uc_close(&c);
	CHK(srv.conns_up == 1 && flushes == 1, "D: closing it again changes nothing");
	open_pair();
	c.tracked = 1;
	c.inv = 1;
	uc_close(&c);
	CHK(srv.conns_up == 0 && flushes == 1, "D: the invalidation connection - no flush");

	/* E: nap_ms sleeps at least its time by now_ms; now_us agrees with now_ms */
	{
		long long t0 = now_ms(), u0 = now_us();

		nap_ms(20);
		CHK(now_ms() - t0 >= 20 && now_us() - u0 >= 20000, "E: nap_ms(20) - at least 20 ms by both clocks");
	}

	/* F: a discovery connection (a sentinel's, a cluster seed's) has no server: a push on it is
	 * dropped, never handed to on_push, and the reply after it still read */
	open_pair();
	c.s = NULL;
	pushes = 0;
	SERVE(">2\r\n$10\r\ninvalidate\r\n*1\r\n$1\r\nk\r\n+OK\r\n");
	rc = read_reply(&c, &v, dl());
	CHK(rc == 0 && is_ok(&v) && pushes == 0,
		"F: a push on a connection with no server - dropped (on_push %d times), +OK read", pushes);
	if (rc == 0)
		rv_free(&v);
	close(c.fd);
	c.fd = -1;
	c.s = &srv;

	/* G: rv_max 150,000 - the buffer grows to rv_max + 64 KB, not only to the powers of two
	 * under it: a 140,000-byte bulk (over 131,072, under rv_max) is read whole */
	open_pair();
	free(c.buf);
	c.buf = NULL;
	c.cap = 0;
	rv_max = 150000;
	{
		static char big[140100];
		pthread_t th;
		size_t i;

		memcpy(big, "$140000\r\n", 9);
		for (i = 0; i < 140000; i++)
			big[9 + i] = (char)('a' + i % 26);
		memcpy(big + 140009, "\r\n", 2);
		srv_big = big;
		srv_big_n = 140011;
		if (pthread_create(&th, NULL, serve_big, NULL) != 0) {
			perror("pthread_create");
			exit(2);
		}
		rc = read_reply(&c, &v, dl());
		pthread_join(th, NULL);
		CHK(rc == 0 && v.len == 140000 && v.s[139999] == (char)('a' + 139999 % 26),
			"G: rv_max 150,000 - a 140,000-byte bulk read whole (rc %d, buffer %zu)", rc, c.cap);
		if (rc == 0)
			rv_free(&v);
	}
	rv_max = UP_RBUF_MAX;
	close(c.fd);
	c.fd = -1;

	if (peer >= 0)
		close(peer);
	free(c.buf);
	printf("upconntest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
