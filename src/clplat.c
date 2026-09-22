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

static const char *PREFIX[] = { "", "pve-uuid:", "pve-lxc-mac:" };

const char *clplat_src_name(int src)
{
	switch (src) {
	case CLPLAT_DMI: return "dmi-product-uuid";
	case CLPLAT_MAC: return "nic-mac";
	default:         return "none";
	}
}

size_t clplat_key_build(int src, const char *raw, char *out, size_t cap)
{
	size_t n, p;

	if (src != CLPLAT_DMI && src != CLPLAT_MAC)
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

	*why = "";
	if (slurp(DMI_UUID, raw, sizeof raw) == 0) {
		if (clplat_key_build(CLPLAT_DMI, raw, key, cap))
			return CLPLAT_DMI;
		*why = "product_uuid unusable";
	} else if (access(DMI_UUID, F_OK) == 0) {
		/* The path is there but the read failed.  Two causes seen:
		 * it is 0400 root and this daemon is not root, or an LXC
		 * exposes /sys/class/dmi and masks the contents - MEASURED on
		 * 245-247, where it reads empty even as root.  Report the fact
		 * and not a guess at which, but say it: falling silently
		 * through to a random mint is how a clone keeps its identity. */
		*why = "product_uuid present but unreadable";
	}
	if (first_mac(raw, sizeof raw) == 0 &&
	    clplat_key_build(CLPLAT_MAC, raw, key, cap))
		return CLPLAT_MAC;
	if (!**why)
		*why = "no DMI uuid and no usable NIC MAC";
	return CLPLAT_NONE;
}
