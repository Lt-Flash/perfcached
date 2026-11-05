/* upgverifytest.c - an upgrade artifact's checks (upgverify.c; the 0.7.2
 * split, wave 1): a key pair made here, an artifact signed with it, and
 * -K's report captured.
 *
 *   A  pc_upg_verify: signed by trusted_keys[1] - named, with its tag;
 *      by a key nobody trusts, or over other bytes - refused, counting
 *      the keys tried; no [upgrade] section - the built-in keys only
 *   B  pc_upg_read_sig: 64 raw bytes; 128 hex characters, with or
 *      without a line end; anything else -2; no file -1 (ENOENT)
 *   C  pc_upg_read_file: the bytes, whole; a directory refused (EFBIG,
 *      which -K says as "not a regular file of at most 1 GB")
 *   D  pc_upg_check_file: GOOD naming the key (0); BAD (1); no signature
 *      refused (1) unless allow_unsigned (0); a signature file of the
 *      wrong shape UNREADABLE (1); an artifact that is not there (2)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sodium.h>
#include "config.h"
#include "relkey.h"
#include "upgrade.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static char dir[64], art[128], sigp[160];

static void put(const char *path, const void *b, size_t n)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0)
		return;
	if (write(fd, b, n) != (ssize_t)n)
		printf("  (write failed)\n");
	close(fd);
}

/* -K's report for @path under @up, captured; its exit code */
static char said[2048];

static int check(const char *path, const struct pc_upgrade_conf *up)
{
	char out[192];
	int fd, keep, rc;
	ssize_t n;

	snprintf(out, sizeof out, "%s/said.txt", dir);
	fflush(stdout);
	keep = dup(1);
	fd = open(out, O_RDWR | O_CREAT | O_TRUNC, 0600);
	dup2(fd, 1);
	rc = pc_upg_check_file(path, up);
	fflush(stdout);
	dup2(keep, 1);
	close(keep);
	lseek(fd, 0, SEEK_SET);
	n = read(fd, said, sizeof said - 1);
	said[n > 0 ? n : 0] = 0;
	close(fd);
	unlink(out);
	return rc;
}

int main(void)
{
	unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
	unsigned char pk2[crypto_sign_PUBLICKEYBYTES], sk2[crypto_sign_SECRETKEYBYTES];
	unsigned char sig[PC_UPG_SIG_LEN], other[PC_UPG_SIG_LEN], got[PC_UPG_SIG_LEN];
	static const char body[] = "a build, as bytes";
	char hex[2 * PC_UPG_SIG_LEN + 3], who[96], tag[9], want[160];
	struct pc_upgrade_conf up;
	size_t len = 0;
	void *data;

	if (sodium_init() < 0)
		return 2;
	snprintf(dir, sizeof dir, "/tmp/upgverifytest.XXXXXX");
	if (!mkdtemp(dir))
		return 2;
	snprintf(art, sizeof art, "%s/build.tar.gz", dir);
	snprintf(sigp, sizeof sigp, "%s.sig", art);
	crypto_sign_keypair(pk, sk);
	crypto_sign_keypair(pk2, sk2);
	crypto_sign_detached(sig, NULL, (const unsigned char *)body, sizeof body - 1, sk);
	crypto_sign_detached(other, NULL, (const unsigned char *)body, sizeof body - 1, sk2);
	memset(&up, 0, sizeof up);
	memcpy(up.keys[0], pk2, 32);             /* another trusted key first */
	memcpy(up.keys[1], pk, 32);
	up.n_keys = 2;
	sodium_bin2hex(tag, sizeof tag, pk, 4);

	/* A */
	snprintf(want, sizeof want, "trusted_keys[1] %s", tag);
	CHK(pc_upg_verify(body, sizeof body - 1, sig, &up, who, sizeof who) == 0 && !strcmp(who, want),
		"A: signed by trusted_keys[1] - verified, named (%s)", who);
	up.n_keys = 1;
	snprintf(want, sizeof want, "none of %d key(s) (%d built in, 1 in trusted_keys)",
		pc_upg_n_release_keys + 1, pc_upg_n_release_keys);
	CHK(pc_upg_verify(body, sizeof body - 1, sig, &up, who, sizeof who) == -1 && !strcmp(who, want),
		"A: its key not trusted - refused (%s)", who);
	up.n_keys = 2;
	CHK(pc_upg_verify("a build, as bytez", sizeof body - 1, sig, &up, who, sizeof who) == -1,
		"A: one byte of the artifact changed - refused");
	CHK(pc_upg_verify(body, sizeof body - 1, sig, NULL, who, sizeof who) == -1 && strstr(who, "0 in trusted_keys"),
		"A: no [upgrade] section - only the built-in keys tried");

	/* B */
	put(sigp, sig, sizeof sig);
	CHK(pc_upg_read_sig(sigp, got) == 0 && !memcmp(got, sig, sizeof sig), "B: 64 raw bytes - read");
	sodium_bin2hex(hex, sizeof hex, sig, sizeof sig);
	put(sigp, hex, 2 * PC_UPG_SIG_LEN);
	memset(got, 0, sizeof got);
	CHK(pc_upg_read_sig(sigp, got) == 0 && !memcmp(got, sig, sizeof sig), "B: 128 hex characters - read");
	strcat(hex, "\r\n");
	put(sigp, hex, strlen(hex));
	memset(got, 0, sizeof got);
	CHK(pc_upg_read_sig(sigp, got) == 0 && !memcmp(got, sig, sizeof sig), "B: and with a CRLF after them");
	put(sigp, "too short", 9);
	CHK(pc_upg_read_sig(sigp, got) == -2, "B: 9 bytes - the wrong shape, -2");
	unlink(sigp);
	errno = 0;
	CHK(pc_upg_read_sig(sigp, got) == -1 && errno == ENOENT, "B: no signature file - -1, ENOENT");

	/* C */
	put(art, body, sizeof body - 1);
	data = pc_upg_read_file(art, &len);
	CHK(data && len == sizeof body - 1 && !memcmp(data, body, len), "C: the artifact - read whole (%zu bytes)", len);
	free(data);
	errno = 0;
	CHK(!pc_upg_read_file(dir, &len) && errno == EFBIG, "C: a directory - refused, EFBIG");

	/* D */
	put(sigp, sig, sizeof sig);
	CHK(check(art, &up) == 0 && strstr(said, "size 17 bytes") && strstr(said, "signature: GOOD, trusted_keys[1]"),
		"D: -K - GOOD, naming the key");
	put(sigp, other, sizeof other);
	up.n_keys = 1;
	memcpy(up.keys[0], pk, 32);              /* trusts only the first key */
	CHK(check(art, &up) == 1 && strstr(said, "signature: BAD - none of"),
		"D: signed by a key it does not trust - BAD, 1");
	unlink(sigp);
	CHK(check(art, &up) == 1 && strstr(said, "signature: NONE (") && strstr(said, "does not exist) - refused"),
		"D: no signature - NONE, refused, 1");
	up.allow_unsigned = 1;
	CHK(check(art, &up) == 0 && strstr(said, "signature: none - accepted, [upgrade] allow_unsigned = yes"),
		"D: no signature under allow_unsigned - accepted, 0");
	up.allow_unsigned = 0;
	put(sigp, "too short", 9);
	CHK(check(art, &up) == 1 && strstr(said, "signature: UNREADABLE (") &&
		strstr(said, "not 64 bytes, nor 128 hex characters) - refused"),
		"D: a signature of the wrong shape - UNREADABLE, 1");
	unlink(sigp);
	unlink(art);
	CHK(check(art, &up) == 2 && strstr(said, "artifact: ") && strstr(said, "No such file or directory"),
		"D: no artifact - 2, the error said");
	rmdir(dir);

	printf("upgverifytest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
