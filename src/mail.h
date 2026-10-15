/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * mail.h - S352: the alert mailer's transport.
 *
 * A node with a [mail] section sends its own mail over SMTP (libcurl's
 * client, PC_MAIL=libcurl at build time): smtp:// upgrades with STARTTLS
 * and refuses to go on without it, smtps:// is TLS from the first byte.
 * The password lives in [secrets] mail, never in [mail].
 *
 * Sending runs on its own thread with a bounded queue: a slow or dead mail
 * server never touches a worker or the maintenance thread, and a full queue
 * drops the newest mail and counts it.  Mail is one way out, not the only
 * record - the alerts themselves are logged and kept (S352b).
 */
#ifndef PC_MAIL_H
#define PC_MAIL_H

#include <stddef.h>

#define PC_MAIL_TO_MAX      8          /* recipients */
#define PC_MAIL_QUEUE       64         /* mails waiting for the thread */
#define PC_MAIL_SUBJ_MAX    200
#define PC_MAIL_BODY_MAX    262144     /* each part (S377: text + HTML) */
#define PC_MAIL_REPLY_MAX   256        /* the SMTP server's last answer */

/* what [mail] says (config.c fills it; mail.c keeps its own copy) */
struct pc_mail_conf {
	char *url;                         /* smtp://host:port or smtps://host:port */
	char *user;                        /* NULL: no AUTH */
	char *pass;                        /* [secrets] mail */
	char *from;
	char *to[PC_MAIL_TO_MAX];
	int nto;
	char *ca_file;                     /* NULL: the system's trust store */
	int tls_verify;                    /* 0: TLS without checking the server's
	                                    * certificate or name (tls_verify = no) */
	char *subject_prefix;              /* NULL: "perfcached <node> <host>:" */
	int timeout_s;                     /* one send, connect to QUIT */
	int min_interval_s;                /* the same alert at most once per window */
	int digest_s;                      /* alerts within it go out as one mail */
	int level;                         /* PC_MAIL_LVL_*: the lowest that mails */
};

#define PC_MAIL_LVL_INFO     0         /* S377 */
#define PC_MAIL_LVL_WARNING  1
#define PC_MAIL_LVL_CRITICAL 2

/* 1 when this binary carries an SMTP client (PC_MAIL=libcurl) */
int pc_mail_built(void);
/* the reason a [mail] section cannot work in this build, for -C */
#define PC_MAIL_NOT_BUILT "this perfcached has no SMTP client: build it with " \
	"PC_MAIL=libcurl (make menuconfig), or remove [mail]"

/* start the sender with this configuration (copied); 0 ok.  Without a
 * [mail] section nothing starts and pc_mail_ready() stays 0. */
int pc_mail_start(const struct pc_mail_conf *mc);
void pc_mail_stop(void);
int pc_mail_ready(void);                 /* configured, built and started */

/* queue one mail; 0 queued, -1 not ready or the queue is full (counted) */
int pc_mail_send(const char *subject, const char *body);

/* S377: a mail with a display name for From, its own Message-ID, the mail
 * it answers (In-Reply-To / References - a resolve threads under its
 * problem), and an HTML part beside the plain one (multipart/alternative,
 * both quoted-printable).  NULL fields are left out; a NULL msgid gets one
 * generated.  0 queued, -1 not. */
struct pc_mail_msg {
	const char *subject;
	const char *from_name;             /* the display name before <from> */
	const char *msgid;                 /* "<...>", from pc_mail_msgid */
	const char *in_reply_to;
	const char *references;            /* space-separated ids */
	const char *text;                  /* text/plain */
	const char *html;                  /* text/html, NULL = plain only */
};
int pc_mail_send_msg(const struct pc_mail_msg *m);
/* a fresh Message-ID, "<...@host>" */
void pc_mail_msgid(char *buf, size_t cap);

/* `mailtest`: send one mail now and wait (at most the send timeout + a
 * margin) for the SMTP server's answer.  1 delivered, 0 refused or failed
 * (@reply says why), -1 not ready.  Blocks the calling worker, as
 * fleetstop does - a privileged verb, run by hand. */
int pc_mail_test(char *reply, size_t cap);

/* the counters (cumulative; the windows are winctr's) */
struct pc_mail_stats {
	unsigned long long sent, failed, dropped, queued_now;
	/* the same three never reset - /metrics counters only grow (S358) */
	unsigned long long sent_total, failed_total, dropped_total;
	char last_error[PC_MAIL_REPLY_MAX];
	long long last_ok_ms, last_err_ms;   /* wall clock, 0 = never */
};
void pc_mail_get_stats(struct pc_mail_stats *st);
void pc_mail_stats_reset(void);          /* S358 */

#endif /* PC_MAIL_H */
