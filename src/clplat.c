/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clplat.c - platform-derived node identity.  See clplat.h.
 */
#include <dirent.h>
#include <net/if.h>
#include <fcntl.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "clplat.h"

#define DMI_UUID  "/sys/class/dmi/id/product_uuid"
#define NET_DIR   "/sys/class/net"

static const char *PREFIX[] = { "", "pve-uuid:", "pve-lxc-mac:", "env-key:" };

const char *clplat_src_name(int src)
{
	switch (src) {
	case CLPLAT_DMI: return "dmi-product-uuid";
	case CLPLAT_MAC: return "nic-mac";
	case CLPLAT_ENV: return "env " CLPLAT_ENV_VAR;
	default:         return "none";
	}
}

size_t clplat_key_build(int src, const char *raw, char *out, size_t cap)
{
	size_t n, p;

	if (src != CLPLAT_DMI && src != CLPLAT_MAC && src != CLPLAT_ENV)
		return 0;
	if (!raw || !out)
		return 0;
	n = strlen(raw);
	while (n && (raw[n - 1] == '\n' || raw[n - 1] == '\r' ||
	        raw[n - 1] == ' ' || raw[n - 1] == '\t'))
		n--;                       /* /sys hands back a newline */
	if (!n)
		return 0;
	p = strlen(PREFIX[src]);
	if (p + n + 1 > cap)
		return 0;
	memcpy(out, PREFIX[src], p);
	memcpy(out + p, raw, n);
	out[p + n] = 0;
	return p + n;
}

/* RFC 9562: version in the high nibble of byte 6, variant 0b10 in the
 * top bits of byte 8. */
static void stamp(unsigned char id[CLPLAT_ID_LEN], int ver)
{
	id[6] = (unsigned char)((id[6] & 0x0f) | ((ver & 0x0f) << 4));
	id[8] = (unsigned char)((id[8] & 0x3f) | 0x80);
}

void clplat_derive(const unsigned char *salt, size_t saltlen,
		const char *key, unsigned char out[CLPLAT_ID_LEN])
{
	/* keyed BLAKE2b with the site salt as the key: same platform on two
	 * fleets derives two identities, which is the point of the salt. */
	crypto_generichash(out, CLPLAT_ID_LEN,
		(const unsigned char *)key, strlen(key), salt, saltlen);
	stamp(out, 8);                     /* v8: vendor layout = a KDF out */
}

void clplat_random_v7(unsigned char out[CLPLAT_ID_LEN], uint64_t unix_ms)
{
	randombytes_buf(out, CLPLAT_ID_LEN);
	out[0] = (unsigned char)(unix_ms >> 40);
	out[1] = (unsigned char)(unix_ms >> 32);
	out[2] = (unsigned char)(unix_ms >> 24);
	out[3] = (unsigned char)(unix_ms >> 16);
	out[4] = (unsigned char)(unix_ms >> 8);
	out[5] = (unsigned char)unix_ms;
	stamp(out, 7);
}

int clplat_uuid_version(const unsigned char id[CLPLAT_ID_LEN])
{
	return (id[6] >> 4) & 0x0f;
}

uint64_t clplat_uuid_v7_ms(const unsigned char id[CLPLAT_ID_LEN])
{
	uint64_t v = 0;
	int i;

	if (clplat_uuid_version(id) != 7)
		return 0;
	for (i = 0; i < 6; i++)
		v = (v << 8) | id[i];
	return v;
}

void clplat_uuid_str(const unsigned char id[CLPLAT_ID_LEN], char *out)
{
	static const char H[] = "0123456789abcdef";
	static const int DASH[] = { 4, 6, 8, 10 };
	int i, j = 0, d = 0;

	for (i = 0; i < CLPLAT_ID_LEN; i++) {
		if (d < 4 && i == DASH[d]) { out[j++] = '-'; d++; }
		out[j++] = H[id[i] >> 4];
		out[j++] = H[id[i] & 0x0f];
	}
	out[j] = 0;
}

/* ---- the /sys reads ------------------------------------------------ */

static int slurp(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, cap - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	return 0;
}

/* S326: the DMI uuid, from /sys - or, where this daemon is not root and
 * the file is 0400 root, from the copy a root ExecStartPre step of the
 * shipped unit leaves in the service's RuntimeDirectory=
 * ($RUNTIME_DIRECTORY/dmi-product-uuid).  0, or -1 with *why set. */
static int dmi_read(char *raw, size_t cap, const char **why)
{
	const char *rd = getenv("RUNTIME_DIRECTORY");
	char path[512];
	size_t n;

	if (slurp(DMI_UUID, raw, cap) == 0)
		return 0;
	if (rd && *rd) {
		/* systemd lists several directories with ':' - ours is first */
		n = strcspn(rd, ":");
		if (n < sizeof path - sizeof "/" CLPLAT_DMI_COPY) {
			memcpy(path, rd, n);
			snprintf(path + n, sizeof path - n, "/%s", CLPLAT_DMI_COPY);
			if (slurp(path, raw, cap) == 0 && raw[0] && raw[0] != '\n')
				return 0;
		}
	}
	*why = access(DMI_UUID, F_OK) == 0 ?
		"product_uuid present but unreadable (0400 root), and no copy "
		"in $RUNTIME_DIRECTORY - the shipped unit's ExecStartPre makes one" :
		"no DMI product_uuid on this machine";
	return -1;
}

/* S326: inside a container /sys/class/dmi, where present, is the HOST's:
 * every container on one host would derive one identity from it.  The
 * usual markers; *what names the one found. */
int clplat_in_container(const char **what)
{
	static char sd[64];
	const char *e = getenv("container");

	if (e && *e) {
		*what = e;
		return 1;
	}
	if (access("/.dockerenv", F_OK) == 0) {
		*what = "docker";
		return 1;
	}
	if (access("/run/.containerenv", F_OK) == 0) {
		*what = "podman";
		return 1;
	}
	if (slurp("/run/systemd/container", sd, sizeof sd) == 0 && sd[0]) {
		sd[strcspn(sd, "\n")] = 0;
		*what = sd;
		return 1;
	}
	*what = "";
	return 0;
}

/* the first non-loopback interface with a non-zero MAC, by name, so the
 * choice is stable across boots rather than whatever enumerated first */
static int first_mac(char *buf, size_t cap)
{
	char names[16][IFNAMSIZ], path[128];
	DIR *d = opendir(NET_DIR);
	struct dirent *e;
	int n = 0, i, j;

	if (!d)
		return -1;
	while ((e = readdir(d)) && n < 16) {
		size_t l = strlen(e->d_name);

		if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo"))
			continue;
		if (l >= sizeof names[0])
			continue;              /* not an interface name */
		memcpy(names[n], e->d_name, l + 1);
		n++;
	}
	closedir(d);
	/* by NAME, so the pick is stable across boots rather than whatever
	 * readdir happened to enumerate first */
	for (i = 1; i < n; i++) {
		char t[IFNAMSIZ];

		memcpy(t, names[i], sizeof t);
		for (j = i; j > 0 && strcmp(names[j - 1], t) > 0; j--)
			memcpy(names[j], names[j - 1], sizeof t);
		memcpy(names[j], t, sizeof t);
	}
	for (i = 0; i < n; i++) {
		if (snprintf(path, sizeof path, "%s/%s/address", NET_DIR,
		        names[i]) >= (int)sizeof path)
			continue;
		if (slurp(path, buf, cap) == 0 &&
		    strncmp(buf, "00:00:00:00:00:00", 17) != 0)
			return 0;
	}
	return -1;
}

int clplat_probe(char *key, size_t cap, const char **why)
{
	char raw[CLPLAT_KEY_MAX];

	const char *ctr, *dwhy = "";

	*why = "";
	if (clplat_in_container(&ctr)) {
		/* S326: the host's uuid, shared by every container on it */
		*why = "inside a container - its DMI uuid is the host's";
	} else if (dmi_read(raw, sizeof raw, &dwhy) == 0) {
		if (clplat_key_build(CLPLAT_DMI, raw, key, cap))
			return CLPLAT_DMI;
		*why = "product_uuid unusable";
	} else if (access(DMI_UUID, F_OK) == 0) {
		/* The path is there but the read failed.  Two causes seen:
		 * it is 0400 root and this daemon is not root (and no copy
		 * was left in $RUNTIME_DIRECTORY), or an LXC exposes
		 * /sys/class/dmi and masks the contents - MEASURED on
		 * 245-247, where it reads empty even as root.  Report the fact
		 * and not a guess at which, but say it: falling silently
		 * through to a random mint is how a clone keeps its identity. */
		*why = dwhy;
	}
	if (first_mac(raw, sizeof raw) == 0 &&
	    clplat_key_build(CLPLAT_MAC, raw, key, cap))
		return CLPLAT_MAC;
	if (!**why)
		*why = "no DMI uuid and no usable NIC MAC";
	return CLPLAT_NONE;
}

int clplat_probe_one(int src, char *key, size_t cap, const char **why)
{
	char raw[CLPLAT_KEY_MAX];
	const char *v;

	*why = "";
	switch (src) {
	case CLPLAT_DMI:
		if (dmi_read(raw, sizeof raw, why) == 0 &&
		        clplat_key_build(CLPLAT_DMI, raw, key, cap))
			return CLPLAT_DMI;
		if (!**why)
			*why = "product_uuid unusable";
		return CLPLAT_NONE;
	case CLPLAT_MAC:
		if (first_mac(raw, sizeof raw) == 0 &&
		        clplat_key_build(CLPLAT_MAC, raw, key, cap))
			return CLPLAT_MAC;
		*why = "no usable NIC MAC";
		return CLPLAT_NONE;
	case CLPLAT_ENV:
		v = getenv(CLPLAT_ENV_VAR);
		if (v && *v && clplat_key_build(CLPLAT_ENV, v, key, cap))
			return CLPLAT_ENV;
		*why = v && *v ? CLPLAT_ENV_VAR " is too long" :
			CLPLAT_ENV_VAR " is not set";
		return CLPLAT_NONE;
	}
	*why = "not a platform source";
	return CLPLAT_NONE;
}
