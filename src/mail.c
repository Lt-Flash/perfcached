/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * mail.c - S352: the alert mailer's transport (mail.h).
 *
 * One sender thread, one bounded queue.  Producers (the alert engine, the
 * mailtest verb) only take the mutex long enough to link a mail in; the
 * thread does every slow thing - DNS, connect, TLS, AUTH, the transfer -
 * with nothing else held.  A mail that cannot be sent is counted and its
 * error kept (the server's own words when it answered); it is not retried:
 * the alert it carried stays in the alert ring and is raised again if the
 * condition is still true after min_interval.
 *
 * The password is handed to libcurl and nowhere else: no message built
 * here, logged or returned ever contains it.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#ifdef PC_MAIL_CURL
#include <curl/curl.h>
#endif

#include "compat/dprint.h"
#include "mail.h"

struct mitem {
	struct mitem *next;
	char *subject;
	char *body;                        /* text/plain */
	char *html;                        /* S377: text/html, NULL = plain only */
	char *from_name, *msgid, *in_reply_to, *references;    /* S377 */
	unsigned int test_gen;             /* nonzero: the mailtest waiting on it */
};

static struct {
	pthread_mutex_t mx;
	pthread_cond_t cv;                 /* the thread: work arrived, or stop */
	pthread_cond_t test_cv;            /* mailtest: its answer arrived */
	struct pc_mail_conf c;
	int started, stopping;
	pthread_t tid;
	struct mitem *head, *tail;
	int nq;
	/* the one mailtest at a time */
	unsigned int test_gen, test_done_gen;
	int test_rc;
	char test_reply[PC_MAIL_REPLY_MAX];
	struct pc_mail_stats st;
	unsigned long long base_sent, base_failed, base_dropped;   /* S358 reset */
	char host[128];
} M = {
	.mx = PTHREAD_MUTEX_INITIALIZER,
	.cv = PTHREAD_COND_INITIALIZER,
	.test_cv = PTHREAD_COND_INITIALIZER,
};

static long long wall_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int pc_mail_built(void)
{
#ifdef PC_MAIL_CURL
	return 1;
#else
	return 0;
#endif
}

static char *dupn(const char *s)
{
	return s ? strdup(s) : NULL;
}

#ifdef PC_MAIL_CURL
/* ---- one send -------------------------------------------------------- */
struct src {
	const char *p;
	size_t left;
};

static size_t read_cb(char *buf, size_t sz, size_t n, void *u)
{
	struct src *s = u;
	size_t k = sz * n < s->left ? sz * n : s->left;

	memcpy(buf, s->p, k);
	s->p += k;
	s->left -= k;
	return k;
}

/* the server's own words: the last reply line it sent (curl's protocol
 * trace, server side only - never what we sent, so never the password) */
static int trace_cb(CURL *h, curl_infotype t, char *d, size_t n, void *u)
{
	char *last = u;

	(void)h;
	if (t != CURLINFO_HEADER_IN || n < 4)
		return 0;
	while (n && (d[n - 1] == '\r' || d[n - 1] == '\n'))
		n--;
	if (n >= PC_MAIL_REPLY_MAX)
		n = PC_MAIL_REPLY_MAX - 1;
	memcpy(last, d, n);
	last[n] = 0;
	return 0;
}

/* S377: quoted-printable (RFC 2045): every line of the result is at most
 * 76 characters, so no SMTP line limit is ever near, and anything but
 * printable ASCII travels as =XX.  \n in @s is a line break (CRLF). */
static void qp_put(FILE *f, const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	int col = 0;

	for (; *p; p++) {
		unsigned c = *p;
		char enc[4];
		int len, eol;

		if (c == '\r')
			continue;
		if (c == '\n') {
			fputs("\r\n", f);
			col = 0;
			continue;
		}
		eol = p[1] == '\n' || p[1] == '\r' || !p[1];
		if ((c >= 33 && c <= 126 && c != '=') ||
		    ((c == ' ' || c == '\t') && !eol)) {
			enc[0] = (char)c;
			len = 1;
		} else {
			snprintf(enc, sizeof enc, "=%02X", c);
			len = 3;
		}
		if (col + len > 75) {
			fputs("=\r\n", f);           /* a soft break */
			col = 0;
		}
		fwrite(enc, 1, (size_t)len, f);
		col += len;
	}
	if (col)
		fputs("\r\n", f);
}

/* S377: a display name as an RFC 5322 quoted string - unquoted, a
 * parenthesis is a comment Outlook hides; non-printing bytes become '?' */
static void name_put(FILE *f, const char *n)
{
	fputc('"', f);
	for (; *n; n++) {
		unsigned char c = (unsigned char)*n;

		if (c == '"' || c == '\\')
			fputc('\\', f);
		fputc(c >= 32 && c < 127 ? (int)c : '?', f);
	}
	fputc('"', f);
}

/* the length name_put() writes */
static size_t name_len(const char *n)
{
	size_t l = 2;

	for (; *n; n++)
		l += (*n == '"' || *n == '\\') ? 2 : 1;
	return l;
}

static void hdr_date(char *b, size_t cap)
{
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(b, cap, "%a, %d %b %Y %H:%M:%S %z", &tm);
}

/* 1 delivered (@reply the server's 250), 0 not (@reply why) */
static int send_one(const struct mitem *it, char *reply, size_t cap)
{
	const struct pc_mail_conf *c = &M.c;
	struct curl_slist *rc = NULL;
	char err[CURL_ERROR_SIZE] = "", last[PC_MAIL_REPLY_MAX] = "", date[64];
	char *msg = NULL, frm[300];
	size_t ml = 0;
	FILE *f;
	CURL *h;
	CURLcode r;
	struct src s;
	long code = 0;
	int i, ok = 0;

	f = open_memstream(&msg, &ml);
	if (!f) {
		snprintf(reply, cap, "out of memory");
		return 0;
	}
	hdr_date(date, sizeof date);
	fprintf(f, "Date: %s\r\nFrom: ", date);
	if (it->from_name && it->from_name[0]) {        /* S377 */
		name_put(f, it->from_name);
		/* a header line stays within 78 (RFC 5322 2.1.1): a long
		 * host name folds before the address */
		fprintf(f, "%s<%s>", 6 + name_len(it->from_name) + 3 +
			strlen(c->from) > 78 ? "\r\n " : " ", c->from);
	} else {
		fputs(c->from, f);
	}
	fputs("\r\nTo: ", f);
	{
		size_t ll = 4;

		for (i = 0; i < c->nto; i++) {
			size_t al = strlen(c->to[i]);

			if (i && ll + 2 + al > 78) {       /* folded, as From */
				fputs(",\r\n ", f);
				ll = 1;
			} else if (i) {
				fputs(", ", f);
				ll += 2;
			}
			fputs(c->to[i], f);
			ll += al;
		}
	}
	fprintf(f, "\r\nSubject: %s\r\n", it->subject);
	if (it->msgid)
		fprintf(f, "Message-ID: %s\r\n", it->msgid);
	else
		fprintf(f, "Message-ID: <%lld.%d@%s>\r\n", wall_ms(), (int)getpid(),
			M.host);
	if (it->in_reply_to)
		fprintf(f, "In-Reply-To: %s\r\n", it->in_reply_to);
	if (it->references)
		fprintf(f, "References: %s\r\n", it->references);
	fputs("MIME-Version: 1.0\r\nX-Mailer: perfcached\r\n", f);
	/* S377: quoted-printable parts (every line <= 76), and with HTML the
	 * two as multipart/alternative - a client shows the richest it can */
	if (it->html) {
		char b[64];

		snprintf(b, sizeof b, "pc-%lld-%d", wall_ms(), (int)getpid());
		fprintf(f, "Content-Type: multipart/alternative; boundary=\"%s\"\r\n"
			"\r\nThis is a multi-part message in MIME format.\r\n"
			"\r\n--%s\r\nContent-Type: text/plain; charset=utf-8\r\n"
			"Content-Transfer-Encoding: quoted-printable\r\n\r\n", b, b);
		qp_put(f, it->body);
		fprintf(f, "\r\n--%s\r\nContent-Type: text/html; charset=utf-8\r\n"
			"Content-Transfer-Encoding: quoted-printable\r\n\r\n", b);
		qp_put(f, it->html);
		fprintf(f, "\r\n--%s--\r\n", b);
	} else {
		fputs("Content-Type: text/plain; charset=utf-8\r\n"
			"Content-Transfer-Encoding: quoted-printable\r\n\r\n", f);
		qp_put(f, it->body);
	}
	fclose(f);

	h = curl_easy_init();
	if (!h) {
		free(msg);
		snprintf(reply, cap, "cannot start a mail transfer");
		return 0;
	}
	snprintf(frm, sizeof frm, "<%s>", c->from);
	for (i = 0; i < c->nto; i++) {
		char t[300];

		snprintf(t, sizeof t, "<%s>", c->to[i]);
		rc = curl_slist_append(rc, t);
	}
	s.p = msg;
	s.left = ml;
	curl_easy_setopt(h, CURLOPT_URL, c->url);
	curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, (long)(c->timeout_s < 10 ? c->timeout_s : 10));
	curl_easy_setopt(h, CURLOPT_TIMEOUT, (long)c->timeout_s);
	if (!strncasecmp(c->url, "smtp://", 7))
		curl_easy_setopt(h, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
	if (c->ca_file)
		curl_easy_setopt(h, CURLOPT_CAINFO, c->ca_file);
	if (!c->tls_verify) {                  /* tls_verify = no: encrypted, unchecked */
		curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	if (c->user) {
		curl_easy_setopt(h, CURLOPT_USERNAME, c->user);
		curl_easy_setopt(h, CURLOPT_PASSWORD, c->pass ? c->pass : "");
	}
	curl_easy_setopt(h, CURLOPT_MAIL_FROM, frm);
	curl_easy_setopt(h, CURLOPT_MAIL_RCPT, rc);
	curl_easy_setopt(h, CURLOPT_READFUNCTION, read_cb);
	curl_easy_setopt(h, CURLOPT_READDATA, &s);
	curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
	curl_easy_setopt(h, CURLOPT_ERRORBUFFER, err);
	curl_easy_setopt(h, CURLOPT_DEBUGFUNCTION, trace_cb);
	curl_easy_setopt(h, CURLOPT_DEBUGDATA, (void *)last);
	curl_easy_setopt(h, CURLOPT_VERBOSE, 1L);
	r = curl_easy_perform(h);
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
	if (r == CURLE_OK) {
		ok = 1;
		snprintf(reply, cap, "%s", last[0] ? last : "250 accepted");
	} else if (last[0] && code >= 400) {
		snprintf(reply, cap, "%s", last);    /* the server said no */
	} else {
		snprintf(reply, cap, "%s", err[0] ? err : curl_easy_strerror(r));
	}
	curl_easy_cleanup(h);
	curl_slist_free_all(rc);
	free(msg);
	return ok;
}
#endif /* PC_MAIL_CURL */

static void item_free(struct mitem *it)
{
	free(it->subject);
	free(it->body);
	free(it->html);
	free(it->from_name);
	free(it->msgid);
	free(it->in_reply_to);
	free(it->references);
	free(it);
}

static void *mail_main(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&M.mx);
	for (;;) {
		struct mitem *it;
		char reply[PC_MAIL_REPLY_MAX];
		int ok;

		while (!M.head && !M.stopping)
			pthread_cond_wait(&M.cv, &M.mx);
		if (M.stopping)
			break;
		it = M.head;
		M.head = it->next;
		if (!M.head)
			M.tail = NULL;
		M.nq--;
		pthread_mutex_unlock(&M.mx);

#ifdef PC_MAIL_CURL
		ok = send_one(it, reply, sizeof reply);
#else
		ok = 0;
		snprintf(reply, sizeof reply, "%s", PC_MAIL_NOT_BUILT);
#endif
		if (!ok)
			LM_WARN("mail: '%s' not sent - %s\n", it->subject, reply);

		pthread_mutex_lock(&M.mx);
		if (ok) {
			M.st.sent++;
			M.st.last_ok_ms = wall_ms();
		} else {
			M.st.failed++;
			M.st.last_err_ms = wall_ms();
			snprintf(M.st.last_error, sizeof M.st.last_error, "%s", reply);
		}
		if (it->test_gen && it->test_gen == M.test_gen) {
			M.test_rc = ok;
			snprintf(M.test_reply, sizeof M.test_reply, "%s", reply);
			M.test_done_gen = it->test_gen;
			pthread_cond_broadcast(&M.test_cv);
		}
		item_free(it);
	}
	pthread_mutex_unlock(&M.mx);
	return NULL;
}

int pc_mail_start(const struct pc_mail_conf *mc)
{
	pthread_attr_t at;
	int i;

	if (!mc || !mc->url)
		return 0;                      /* no [mail]: nothing to start */
	if (!pc_mail_built()) {
		LM_ERR("mail: %s\n", PC_MAIL_NOT_BUILT);
		return -1;
	}
	if (gethostname(M.host, sizeof M.host - 1) != 0)
		snprintf(M.host, sizeof M.host, "perfcached");
	M.c = *mc;
	M.c.url = dupn(mc->url);
	M.c.user = dupn(mc->user);
	M.c.pass = dupn(mc->pass);
	M.c.from = dupn(mc->from);
	M.c.ca_file = dupn(mc->ca_file);
	/* S377: no default prefix - the From name and the subject say what a
	 * mail is; only one set in [mail] is put in front */
	M.c.subject_prefix = dupn(mc->subject_prefix);
	for (i = 0; i < mc->nto; i++)
		M.c.to[i] = dupn(mc->to[i]);
	if (M.c.timeout_s <= 0)
		M.c.timeout_s = 30;
#ifdef PC_MAIL_CURL
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		LM_ERR("mail: libcurl would not initialise\n");
		return -1;
	}
#endif
	pthread_attr_init(&at);
	if (pthread_create(&M.tid, &at, mail_main, NULL) != 0) {
		pthread_attr_destroy(&at);
		LM_ERR("mail: cannot start the sender thread\n");
		return -1;
	}
	pthread_attr_destroy(&at);
	M.started = 1;
	LM_NOTICE("mail: alerts go to %d recipient(s) through %s%s%s\n", M.c.nto,
		M.c.url, M.c.user ? " (authenticated)" : "",
		M.c.tls_verify ? "" : " - the server's certificate is NOT verified");
	return 0;
}

void pc_mail_stop(void)
{
	struct mitem *it;

	if (!M.started)
		return;
	pthread_mutex_lock(&M.mx);
	M.stopping = 1;
	pthread_cond_broadcast(&M.cv);
	pthread_mutex_unlock(&M.mx);
	pthread_join(M.tid, NULL);
	while ((it = M.head)) {               /* what was never sent */
		M.head = it->next;
		item_free(it);
	}
	M.tail = NULL;
	M.nq = 0;
	M.started = 0;
}

int pc_mail_ready(void)
{
	return M.started && !M.stopping;
}

/* link one mail in; @gen nonzero for mailtest.  0 queued, -1 not. */
static int enqueue(const char *subject, const char *body, unsigned int gen,
		const struct pc_mail_msg *m)
{
	struct mitem *it;

	if (!pc_mail_ready())
		return -1;
	it = calloc(1, sizeof *it);
	if (!it)
		return -1;
	if (m) {                                         /* S377 */
		it->html = m->html ? strndup(m->html, PC_MAIL_BODY_MAX) : NULL;
		it->from_name = m->from_name ? strndup(m->from_name, 128) : NULL;
		it->msgid = m->msgid ? strndup(m->msgid, 200) : NULL;
		it->in_reply_to = m->in_reply_to ? strndup(m->in_reply_to, 200) : NULL;
		it->references = m->references ? strndup(m->references, 900) : NULL;
		if ((m->html && !it->html) || (m->from_name && !it->from_name) ||
		    (m->msgid && !it->msgid) || (m->in_reply_to && !it->in_reply_to) ||
		    (m->references && !it->references)) {
			item_free(it);
			return -1;
		}
	}
	if (M.c.subject_prefix && M.c.subject_prefix[0]) {
		size_t n = strlen(M.c.subject_prefix) + 2 + PC_MAIL_SUBJ_MAX;

		it->subject = malloc(n);
		if (it->subject)
			snprintf(it->subject, n, "%s %.*s", M.c.subject_prefix,
				PC_MAIL_SUBJ_MAX, subject);
	} else {
		it->subject = strndup(subject, PC_MAIL_SUBJ_MAX);
	}
	it->body = strndup(body, PC_MAIL_BODY_MAX);
	it->test_gen = gen;
	if (!it->subject || !it->body) {
		item_free(it);
		return -1;
	}
	pthread_mutex_lock(&M.mx);
	if (M.nq >= PC_MAIL_QUEUE) {
		M.st.dropped++;
		pthread_mutex_unlock(&M.mx);
		item_free(it);
		return -1;
	}
	if (M.tail)
		M.tail->next = it;
	else
		M.head = it;
	M.tail = it;
	M.nq++;
	pthread_cond_signal(&M.cv);
	pthread_mutex_unlock(&M.mx);
	return 0;
}

int pc_mail_send(const char *subject, const char *body)
{
	return enqueue(subject, body, 0, NULL);
}

int pc_mail_send_msg(const struct pc_mail_msg *m)            /* S377 */
{
	return enqueue(m->subject, m->text, 0, m);
}

void pc_mail_msgid(char *buf, size_t cap)                  /* S377 */
{
	static unsigned int seq;

	snprintf(buf, cap, "<%lld.%d.%u@%s>", wall_ms(), (int)getpid(),
		__atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED), M.host[0] ? M.host
		: "perfcached");
}

int pc_mail_test(char *reply, size_t cap)
{
	char subj[200], body[512];
	struct timespec until;
	unsigned int gen;
	int rc;

	if (!pc_mail_ready()) {
		snprintf(reply, cap, "%s", pc_mail_built() ? "no [mail] section "
			"in this node's configuration" : PC_MAIL_NOT_BUILT);
		return -1;
	}
	pthread_mutex_lock(&M.mx);
	gen = ++M.test_gen ? M.test_gen : ++M.test_gen;
	pthread_mutex_unlock(&M.mx);
	snprintf(subj, sizeof subj, "perfcached test mail from %s", M.host);
	snprintf(body, sizeof body, "This is a test of perfcached's alert mail "
		"on %s, sent by the mailtest command.\n\nIf it arrived, alerts from "
		"this node reach you.\n", M.host);
	if (enqueue(subj, body, gen, NULL) != 0) {
		snprintf(reply, cap, "the mail queue is full (%d waiting)",
			PC_MAIL_QUEUE);
		return 0;
	}
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += M.c.timeout_s + 5;
	pthread_mutex_lock(&M.mx);
	while (M.test_done_gen != gen &&
	       pthread_cond_timedwait(&M.test_cv, &M.mx, &until) != ETIMEDOUT)
		;
	if (M.test_done_gen == gen) {
		rc = M.test_rc;
		snprintf(reply, cap, "%s", M.test_reply);
	} else {
		rc = 0;
		snprintf(reply, cap, "no answer within %d s - the mail is still "
			"queued behind %d other(s)", M.c.timeout_s + 5, M.nq);
	}
	pthread_mutex_unlock(&M.mx);
	return rc;
}

void pc_mail_get_stats(struct pc_mail_stats *st)
{
	pthread_mutex_lock(&M.mx);
	*st = M.st;
	st->sent_total = M.st.sent;
	st->failed_total = M.st.failed;
	st->dropped_total = M.st.dropped;
	st->sent -= M.base_sent;
	st->failed -= M.base_failed;
	st->dropped -= M.base_dropped;
	st->queued_now = (unsigned long long)M.nq;
	pthread_mutex_unlock(&M.mx);
}

/* S358: a stats reset starts the counters again; the last error and the
 * times stay - they are facts, not counts */
void pc_mail_stats_reset(void)
{
	pthread_mutex_lock(&M.mx);
	M.base_sent = M.st.sent;
	M.base_failed = M.st.failed;
	M.base_dropped = M.st.dropped;
	pthread_mutex_unlock(&M.mx);
}
