/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clplat.h - a node identity derived from the PLATFORM, so a cloned disk does not carry it; a
 * restored backup keeps its UUID and MAC, which is why the live-duplicate check must stay */
#ifndef PC_CLPLAT_H
#define PC_CLPLAT_H

#include <stddef.h>
#include <stdint.h>

#define CLPLAT_KEY_MAX   96
#define CLPLAT_ID_LEN    16

enum {
	CLPLAT_NONE = 0,
	CLPLAT_DMI,                        /* "pve-uuid:<product_uuid>" */
	CLPLAT_MAC,                        /* "pve-lxc-mac:<mac>" */
	CLPLAT_ENV,                        /* "env-key:<$PERFCACHED_PLATFORM_KEY>" */
};

/* ---- pure: everything below is testable without touching /sys ---- */

/* compose the platform key: its length, 0 if @raw is empty or will not fit; trailing whitespace
 * (a /sys file's newline) is dropped */
size_t clplat_key_build(int src, const char *raw, char *out, size_t cap);

/* identity = keyed BLAKE2b(salt, key) truncated to 16, stamped v8 (RFC 9562 vendor layout): the
 * version tells derived (v8) from rolled (v7); any other is refused, not guessed */
void clplat_derive(const unsigned char *salt, size_t saltlen,
		const char *key, unsigned char out[CLPLAT_ID_LEN]);

/* A random identity, stamped v7: 48-bit big-endian ms timestamp then
 * randomness, so two identities can be ordered by when they were made. */
void clplat_random_v7(unsigned char out[CLPLAT_ID_LEN], uint64_t unix_ms);

/* 4 or 7 or 8 ... whatever the version nibble says. */
int clplat_uuid_version(const unsigned char id[CLPLAT_ID_LEN]);
/* The 48-bit timestamp of a v7, or 0 for anything else. */
uint64_t clplat_uuid_v7_ms(const unsigned char id[CLPLAT_ID_LEN]);
/* canonical 8-4-4-4-12 plus NUL; @out needs 37 bytes */
void clplat_uuid_str(const unsigned char id[CLPLAT_ID_LEN], char *out);

#define CLPLAT_IFNAME    16            /* IFNAMSIZ */
/* @name into @names (@n held, sorted), keeping the @max smallest whatever the order read: the
 * new count; a name of CLPLAT_IFNAME or more is skipped */
int clplat_keep_smallest(char (*names)[CLPLAT_IFNAME], int n, int max, const char *name);

/* ---- the /sys reads ---- */

/* DMI product_uuid, else the primary NIC MAC (LXC has no DMI): the source, or CLPLAT_NONE; *why on
 * NONE or a failed read (product_uuid is 0400 root) - never taken as "no platform" */
int clplat_probe(char *key, size_t cap, const char **why);

const char *clplat_src_name(int src);

/* the key from ONE named source - no cascade.  @src is CLPLAT_DMI,
 * CLPLAT_MAC or CLPLAT_ENV; returns @src, or CLPLAT_NONE with @why. */
#define CLPLAT_ENV_VAR "PERFCACHED_PLATFORM_KEY"
/* the name of the DMI uuid's copy in $RUNTIME_DIRECTORY */
#define CLPLAT_DMI_COPY "dmi-product-uuid"
/* 1 when this process runs inside a container (*what says how it
 * knows): its /sys/class/dmi, if any, is the host's */
int clplat_in_container(const char **what);
int clplat_probe_one(int src, char *key, size_t cap, const char **why);

#endif /* PC_CLPLAT_H */
