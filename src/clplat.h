/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clplat.h - deriving a node's identity from the PLATFORM, so a cloned
 * disk does not carry a cluster member's identity with it.
 *
 * An identity stored only on disk is copied by any full clone.  The
 * existing defence - refusing a duplicate LIVE identity - only fires
 * while the donor is running, so a clone of a STOPPED machine, or one
 * started after the donor aged out, presents a perfectly valid identity
 * with nothing to conflict with.  Templates are sealed from powered-off
 * machines, which is exactly that case.
 *
 * So the identity is derived from something the hypervisor regenerates:
 *
 *     1  cloud instance id (IMDS)     - not implemented here yet
 *     2  DMI product_uuid             - QEMU/KVM guests, incl. Proxmox
 *     3  primary NIC MAC              - LXC, which has NO DMI at all
 *     4  random                       - the caller's fallback
 *
 * MEASURED on this fleet 2026-09-13: the three VMs have distinct
 * product_uuid; the three LXC containers have NONE - /sys/class/dmi is
 * absent in a container, not merely shared.  Without step 3 the whole
 * cascade is a no-op on the machines perfcached actually runs on.
 * Proxmox regenerates a container's NIC MAC on clone, which is what
 * gives step 3 the clone-distinct property step 2 gets from the UUID.
 *
 * Both share a hole worth stating: a RESTORED BACKUP keeps its UUID and
 * its MAC, so it derives the donor's identity.  That is the live-duplicate
 * check's job, and it is why that check must stay.
 *
 * PRIVILEGE: /sys/class/dmi/id/product_uuid is 0400 root.  The daemon
 * runs as User=perfcached, so it CANNOT read it and step 2 will fail on
 * a VM unless an operator supplies the value another way.  The MAC is
 * 0444 and needs nothing.  A failed read is logged and falls through -
 * never treated as "no platform".
 *
 * THE UUID VERSION CARRIES THE PROVENANCE.  RFC 9562 leaves v8 free for
 * vendor layouts, which is exactly a KDF output, and v7 is time-ordered:
 *
 *     v8  derived from a platform id
 *     v7  randomly minted, and the embedded timestamp says WHEN
 *
 * so no extra field is needed to tell "I derived this" from "I gave up
 * and rolled dice" - the identity says which. 0.3.6 already forced every
 * identity file to be deleted, so there are no older identities to
 * misread: an unrecognised version is a refusal, not a guess.
 */
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
};

/* ---- pure: everything below is testable without touching /sys ---- */

/* Compose the platform key.  Returns its length, 0 if @raw is empty or
 * will not fit.  Trailing whitespace in @raw is dropped - /sys files
 * come with a newline. */
size_t clplat_key_build(int src, const char *raw, char *out, size_t cap);

/* identity = keyed BLAKE2b(salt, key) truncated to 16, stamped v8. */
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

/* ---- the /sys reads ---- */

/* Probe for a platform key.  Returns the source, CLPLAT_NONE if none is
 * available; *why is a short reason when it is NONE or a read failed. */
int clplat_probe(char *key, size_t cap, const char **why);

const char *clplat_src_name(int src);

#endif /* PC_CLPLAT_H */
