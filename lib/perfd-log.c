/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov - see lib/LICENSE */
/*
 * perfd-log.c - S370: where libperfd's few log lines go (perfd.h,
 * perfd_set_log).  A consumer with a logger of its own hands it here; with
 * none, errors and worse go to stderr, as before the hook existed.
 */
#include <stdarg.h>
#include <stdio.h>

#include "perfd.h"
#include <compat/dprint.h>        /* lib/shim's (-Ilib/shim): pdl_log */

static perfd_log_cb log_cb;
static void *log_ctx;

void perfd_set_log(perfd_log_cb cb, void *ctx)
{
	log_ctx = ctx;
	__atomic_store_n(&log_cb, cb, __ATOMIC_RELEASE);
}

void pdl_log(int level, const char *fmt, ...)
{
	perfd_log_cb cb = __atomic_load_n(&log_cb, __ATOMIC_ACQUIRE);
	char msg[512];
	va_list ap;

	if (!cb && level > L_ERR)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	if (cb)
		cb(level, msg, log_ctx);
	else
		fprintf(stderr, "libperfd: %s", msg);
}
