/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * upgverify.c - an upgrade artifact's checks: read it whole (a regular
 * file, bounded), read its signature (64 bytes, or 128 hex characters),
 * verify it against the built-in release keys and [upgrade]
 * trusted_keys, and -K's report.  Moved from upgrade.c unchanged (the
 * 0.7.2 split, wave 1 - doc/MODULARITY.md).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sodium.h>

#include "relkey.h"
#include "upgrade.h"

static void key_tag(const unsigned char *k, char out[9])
{
	sodium_bin2hex(out, 9, k, 4);
}

int pc_upg_verify(const void *data, size_t len, const unsigned char *sig,
		const struct pc_upgrade_conf *up, char *who, size_t wl)
{
	char tag[9];
	int i;

	for (i = 0; i < pc_upg_n_release_keys; i++)
		if (crypto_sign_verify_detached(sig, data, len,
		        pc_upg_release_keys[i]) == 0) {
			key_tag(pc_upg_release_keys[i], tag);
			snprintf(who, wl, "the release key %s (built in)", tag);
			return 0;
		}
	for (i = 0; up && i < up->n_keys; i++)
		if (crypto_sign_verify_detached(sig, data, len,
		        up->keys[i]) == 0) {
			key_tag(up->keys[i], tag);
			snprintf(who, wl, "trusted_keys[%d] %s", i, tag);
			return 0;
		}
	snprintf(who, wl, "none of %d key(s) (%d built in, %d in trusted_keys)",
		pc_upg_n_release_keys + (up ? up->n_keys : 0),
		pc_upg_n_release_keys, up ? up->n_keys : 0);
	return -1;
}

int pc_upg_read_sig(const char *path, unsigned char *sig)
{
	unsigned char b[2 * PC_UPG_SIG_LEN + 3];
	size_t bl = 0;
	ssize_t n;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	n = read(fd, b, sizeof b);
	close(fd);
	if (n < 0)
		return -1;
	if (n == PC_UPG_SIG_LEN) {
		memcpy(sig, b, PC_UPG_SIG_LEN);
		return 0;
	}
	while (n > 0 && (b[n - 1] == '\n' || b[n - 1] == '\r'))
		n--;
	if (n == 2 * PC_UPG_SIG_LEN && sodium_hex2bin(sig, PC_UPG_SIG_LEN,
	        (const char *)b, (size_t)n, NULL, &bl, NULL) == 0 &&
	        bl == PC_UPG_SIG_LEN)
		return 0;
	return -2;
}

void *pc_upg_read_file(const char *path, size_t *len)
{
	struct stat st;
	unsigned char *buf;
	size_t got = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) != 0) {
		int e = errno;

		close(fd);
		errno = e;
		return NULL;
	}
	if (!S_ISREG(st.st_mode) || st.st_size > PC_UPG_ARTIFACT_MAX) {
		close(fd);
		errno = EFBIG;             /* said as "not a regular file of..." */
		return NULL;
	}
	buf = malloc(st.st_size ? (size_t)st.st_size : 1);
	if (!buf) {
		close(fd);
		return NULL;
	}
	while (got < (size_t)st.st_size) {
		ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);

		if (n <= 0) {
			int e = n < 0 ? errno : EIO;

			free(buf);
			close(fd);
			errno = e;
			return NULL;
		}
		got += (size_t)n;
	}
	close(fd);
	*len = got;
	return buf;
}

int pc_upg_check_file(const char *path, const struct pc_upgrade_conf *up)
{
	unsigned char sig[PC_UPG_SIG_LEN], h[crypto_hash_sha256_BYTES];
	char hx[2 * crypto_hash_sha256_BYTES + 1], sp[600], who[96];
	size_t len = 0;
	void *data;
	int r;

	if (sodium_init() < 0) {
		printf("artifact: libsodium did not initialise\n");
		return 2;
	}
	data = pc_upg_read_file(path, &len);
	if (!data) {
		printf("artifact: %s: %s\n", path, errno == EFBIG ?
			"not a regular file of at most 1 GB" : strerror(errno));
		return 2;
	}
	crypto_hash_sha256(h, data, len);
	sodium_bin2hex(hx, sizeof hx, h, sizeof h);
	printf("artifact: %s\n  size %zu bytes\n  sha256 %s\n", path, len, hx);
	snprintf(sp, sizeof sp, "%s.sig", path);
	r = pc_upg_read_sig(sp, sig);
	if (r == -1 && errno == ENOENT) {
		free(data);
		if (up && up->allow_unsigned) {
			printf("  signature: none - accepted, [upgrade] "
				"allow_unsigned = yes\n");
			return 0;
		}
		printf("  signature: NONE (%s does not exist) - refused\n", sp);
		return 1;
	}
	if (r != 0) {
		free(data);
		printf("  signature: UNREADABLE (%s: %s) - refused\n", sp,
			r == -2 ? "not 64 bytes, nor 128 hex characters" :
			strerror(errno));
		return 1;
	}
	r = pc_upg_verify(data, len, sig, up, who, sizeof who);
	free(data);
	if (r != 0) {
		printf("  signature: BAD - %s verifies it - refused\n", who);
		return 1;
	}
	printf("  signature: GOOD, %s\n", who);
	return 0;
}
