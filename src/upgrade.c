/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * upgrade.c - UP6n/UP5 (DESIGN 12nf): who may vouch for a build to install.
 *
 * A release artifact (the tarball) carries a detached ed25519 signature
 * over its bytes - what `openssl pkeyutl -sign -rawin` makes with the
 * release key on the build host.  It verifies against the release key
 * built in here, or one of [upgrade] trusted_keys.  Anything else is
 * refused; an unsigned artifact too, unless [upgrade] allow_unsigned.
 *
 * `perfcached -K <file>` runs the same check offline.  The root helper
 * (perfcached-upgrade) asks the INSTALLED binary, so an artifact never
 * vouches for itself.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <arpa/inet.h>
#include <sodium.h>

#include "compat/dprint.h"
#include "cluster.h"
#include "ptree.h"
#include "version.h"
#include "relkey.h"
#include "upgrade.h"

#ifndef PC_BUILD_REV
#define PC_BUILD_REV "unknown"
#endif

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

/* ---- UP5: the node file ------------------------------------------------ */

static struct {
	char dir[512];                 /* <state_dir>/upgrade, "" = off */
	int state;                     /* what the file says now */
	int failed;                    /* the last write failed (said once) */
} N = { "", -1, 0 };

static void node_write(int st)
{
	char path[600], tmp[620], body[256];
	int fd, n;

	n = snprintf(body, sizeof body, "pid=%ld\nversion=%s\nrev=%s\n"
		"state=%s\nsince=%lld\n", (long)getpid(), PC_VERSION,
		PC_BUILD_REV, pc_node_state_name(st), (long long)time(NULL));
	if (n <= 0 || (size_t)n >= sizeof body)
		return;
	snprintf(path, sizeof path, "%s/node", N.dir);
	snprintf(tmp, sizeof tmp, "%s/node.tmp", N.dir);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0 || write(fd, body, (size_t)n) != n) {
		int e = errno;

		if (fd >= 0)
			close(fd);
		errno = e;
		goto bad;
	}
	if (close(fd) != 0 || rename(tmp, path) != 0)
		goto bad;
	N.failed = 0;
	N.state = st;
	return;
bad:
	if (!N.failed)
		LM_WARN("upgrade: cannot write %s (%s) - an upgrade helper "
			"will not see this node ready\n", path, strerror(errno));
	N.failed = 1;
}

int pc_upg_node_init(const char *state_dir)
{
	if (!state_dir || !*state_dir)
		return 0;
	if ((size_t)snprintf(N.dir, sizeof N.dir, "%s/upgrade", state_dir) >=
	        sizeof N.dir) {
		N.dir[0] = 0;
		return -1;
	}
	if (mkdir(N.dir, 0750) != 0 && errno != EEXIST) {
		LM_WARN("upgrade: cannot create %s (%s) - no node file, so no "
			"upgrade helper can drive this node\n", N.dir,
			strerror(errno));
		N.dir[0] = 0;
		return -1;
	}
	node_write(pc_node_state());
	return 0;
}

static uint32_t FACT;                   /* UPS: the cached fact */
static void fact_tick(void);
static void switch_asked_tick(void);
/* clmsg.h's UPGSW_* (cluster.c pins them equal) */
#define UPGSW_ACCEPTED_   0u
#define UPGSW_NOT_STAGED_ 1u
#define UPGSW_UNVERIFIED_ 2u
#define UPGSW_NO_HELPER_  3u
#define UPGSW_BUSY_       4u
#define UPGSW_OTHER_      5u

void pc_upg_node_tick(void)
{
	static unsigned int n;
	int st;

	switch_asked_tick();                    /* UPS: a member's ask - answered
	                                         * even here with none (refused) */
	if (!N.dir[0])
		return;
	if (n++ % 5 == 0)
		fact_tick();
	st = pc_node_state();
	if (st != N.state || N.failed)
		node_write(st);
}

/* ---- UP6n: staging ----------------------------------------------------- */

static pthread_mutex_t SL = PTHREAD_MUTEX_INITIALIZER;
/* the relay notes and the switch/ask queues have their own: the cluster
 * thread takes it every beat, and must never wait behind SL - SL is held
 * while a whole build is read and verified.  Order: SL, then QL */
static pthread_mutex_t QL = PTHREAD_MUTEX_INITIALIZER;
static struct pc_upgrade_conf UC;          /* set once at start */
static struct {
	char id[33];                   /* "" = no upload */
	int fd;
	long long size, got;
	unsigned char sha[32], sig[PC_UPG_SIG_LEN];
	int has_sig;
	char ver[48];
	time_t last;
} U = { "", -1, 0, 0, { 0 }, { 0 }, 0, "", 0 };
static unsigned long long staged_n, refused_n;  /* since start */
/* the relay's last word per member */
#define RELAY_MAX 32
static struct {
	uint32_t addr;                 /* network order; 0 = free */
	unsigned int status;
	time_t at;
} RL[RELAY_MAX];

static const char *relay_word(unsigned int st)
{
	switch (st) {
	case PC_UPG_RELAY_OK:        return "staged";
	case PC_UPG_RELAY_BEGIN:     return "refused to begin (busy, or no room)";
	case PC_UPG_RELAY_CHUNK:     return "a write failed there";
	case PC_UPG_RELAY_REFUSED:   return "refused: sha256 or signature";
	case PC_UPG_RELAY_MALFORMED: return "malformed";
	case PC_UPG_RELAY_OLDER:     return "not asked: an older build";
	case PC_UPG_RELAY_BUSY:      return "not asked: the bulk queue was full";
	case PC_UPG_RELAY_SENDING:   return "sending";
	case PC_UPG_RELAY_LOST:      return "no answer: the connection failed";
	default:                     return "unknown";
	}
}

void pc_upg_relay_note(uint32_t addr_be, unsigned int status)
{
	int i, slot = -1, old = 0;

	pthread_mutex_lock(&QL);
	for (i = 0; i < RELAY_MAX; i++) {
		if (RL[i].addr == addr_be) {
			slot = i;
			break;
		}
		if (slot < 0 && !RL[i].addr)
			slot = i;
		if (RL[i].at < RL[old].at)
			old = i;
	}
	if (slot < 0)
		slot = old;
	RL[slot].addr = addr_be;
	RL[slot].status = status;
	RL[slot].at = time(NULL);
	pthread_mutex_unlock(&QL);
	if (status != PC_UPG_RELAY_SENDING) {
		struct in_addr a;

		a.s_addr = addr_be;
		if (status == PC_UPG_RELAY_OK)
			LM_NOTICE("upgrade: member %s staged the build\n",
				inet_ntoa(a));
		else
			LM_WARN("upgrade: member %s: %s\n", inet_ntoa(a),
				relay_word(status));
	}
}

void pc_upg_set_conf(const struct pc_upgrade_conf *up)
{
	if (up)
		UC = *up;
}

/* <upgrade>/<sub>[/<name>] into @b; 0 or -1 (no state directory, or long) */
static int upath(char *b, size_t bl, const char *sub, const char *name)
{
	int n;

	if (!N.dir[0])
		return -1;
	n = name ? snprintf(b, bl, "%s/%s/%s", N.dir, sub, name)
		: snprintf(b, bl, "%s/%s", N.dir, sub);
	return n > 0 && (size_t)n < bl ? 0 : -1;
}

static void upload_drop(void)               /* under SL */
{
	char p[700], nm[64];

	if (U.fd >= 0)
		close(U.fd);
	if (U.id[0]) {
		snprintf(nm, sizeof nm, "%s.part", U.id);
		if (upath(p, sizeof p, "staging", nm) == 0)
			unlink(p);
	}
	U.fd = -1;
	U.id[0] = 0;
	U.got = U.size = 0;
}

/* the staged builds, oldest first: their sha256 (64 hex) and size */
struct sent {
	char sha[65];
	long long size;
	time_t at;
};

static int staged_list(struct sent *e, int max)  /* under SL */
{
	char d[700], p[800];
	struct dirent *de;
	struct stat st;
	DIR *dh;
	int n = 0, i, j;

	if (upath(d, sizeof d, "staged", NULL) != 0 || !(dh = opendir(d)))
		return 0;
	while ((de = readdir(dh)) && n < max) {
		size_t l = strlen(de->d_name);

		if (l != 64 + 7 || strcmp(de->d_name + 64, ".tar.gz"))
			continue;
		snprintf(p, sizeof p, "%s/%.71s", d, de->d_name);   /* l is 71 */
		if (stat(p, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		memcpy(e[n].sha, de->d_name, 64);
		e[n].sha[64] = 0;
		e[n].size = (long long)st.st_size;
		e[n].at = st.st_mtime;
		n++;
	}
	closedir(dh);
	for (i = 1; i < n; i++)               /* oldest first */
		for (j = i; j > 0 && e[j].at < e[j - 1].at; j--) {
			struct sent t = e[j];

			e[j] = e[j - 1];
			e[j - 1] = t;
		}
	return n;
}

static void staged_remove(const char *sha)  /* under SL */
{
	static const char *const ext[] = { ".tar.gz", ".tar.gz.sig", ".meta" };
	char p[800], nm[96];
	int i;

	for (i = 0; i < 3; i++) {
		snprintf(nm, sizeof nm, "%.64s%s", sha, ext[i]);
		if (upath(p, sizeof p, "staged", nm) == 0)
			unlink(p);
	}
}

static int hexsha(const char *h, unsigned char out[32])
{
	size_t bl = 0;
	const char *end = NULL;
	size_t i;

	if (!h || strlen(h) != 64)
		return -1;
	for (i = 0; i < 64; i++)                /* lowercase: the file name */
		if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f')))
			return -1;
	return sodium_hex2bin(out, 32, h, 64, NULL, &bl, &end) == 0 &&
		bl == 32 ? 0 : -1;
}

int pc_upg_stage_begin(long long size, const char *sha_hex,
		const unsigned char *sig, const char *version, char *id,
		size_t idl, const char **err)
{
	struct sent e[16];
	unsigned char rnd[8];
	long long cap = (long long)UC.stage_mb << 20, used = 0;
	char d[700], p[800], nm[64];
	int n, i;

	if (!N.dir[0]) {
		*err = "this node has no state directory - nowhere to stage";
		return -1;
	}
	if (size <= 0 || size > PC_UPG_ARTIFACT_MAX) {
		*err = "size must be 1 byte to 1 GB";
		return -1;
	}
	if (size > cap) {
		*err = "larger than [upgrade] stage_mb";
		return -1;
	}
	if (!version || !*version || strlen(version) >= sizeof U.ver ||
	        strspn(version, "0123456789abcdefghijklmnopqrstuvwxyz"
	        "ABCDEFGHIJKLMNOPQRSTUVWXYZ.+_-") != strlen(version)) {
		*err = "version is missing or not [0-9A-Za-z.+_-], under 48";
		return -1;
	}
	if (!sig && !UC.allow_unsigned) {
		*err = "unsigned - refused ([upgrade] allow_unsigned = no)";
		return -1;
	}
	pthread_mutex_lock(&SL);
	if (U.id[0] && time(NULL) - U.last < PC_UPG_IDLE_S) {
		pthread_mutex_unlock(&SL);
		*err = "another upload is in progress";
		return -1;
	}
	upload_drop();                          /* an abandoned one */
	if (hexsha(sha_hex, U.sha) != 0) {
		pthread_mutex_unlock(&SL);
		*err = "sha256 must be 64 lowercase hex characters";
		return -1;
	}
	/* room: the oldest staged go first, so this one lands beside at
	 * most PC_UPG_KEEP - 1 others, all inside stage_mb */
	n = staged_list(e, 16);
	for (i = 0; i < n; i++)
		used += e[i].size;
	for (i = 0; i < n && (n - i >= PC_UPG_KEEP || used + size > cap); i++) {
		if (!strcmp(e[i].sha, sha_hex))
			continue;               /* the same build again: kept */
		staged_remove(e[i].sha);
		used -= e[i].size;
	}
	if (upath(d, sizeof d, "staging", NULL) != 0 ||
	        (mkdir(d, 0750) != 0 && errno != EEXIST) ||
	        upath(d, sizeof d, "staged", NULL) != 0 ||
	        (mkdir(d, 0750) != 0 && errno != EEXIST)) {
		pthread_mutex_unlock(&SL);
		*err = "cannot create upgrade/staging or upgrade/staged";
		return -1;
	}
	randombytes_buf(rnd, sizeof rnd);
	sodium_bin2hex(U.id, sizeof U.id, rnd, sizeof rnd);
	snprintf(nm, sizeof nm, "%s.part", U.id);
	if (upath(p, sizeof p, "staging", nm) != 0 ||
	        (U.fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640)) < 0) {
		U.id[0] = 0;
		pthread_mutex_unlock(&SL);
		*err = "cannot create the upload's file";
		return -1;
	}
	U.size = size;
	U.got = 0;
	U.has_sig = sig != NULL;
	if (sig)
		memcpy(U.sig, sig, PC_UPG_SIG_LEN);
	snprintf(U.ver, sizeof U.ver, "%s", version);
	U.last = time(NULL);
	snprintf(id, idl, "%s", U.id);
	pthread_mutex_unlock(&SL);
	LM_NOTICE("upgrade: staging %s (%lld bytes, %s) as upload %s\n",
		version, size, sig ? "signed" : "UNSIGNED", id);
	return 0;
}

int pc_upg_stage_chunk(const char *id, long long off, const void *data,
		size_t n, long long *got, const char **err)
{
	const unsigned char *b = data;
	size_t w = 0;

	pthread_mutex_lock(&SL);
	if (!U.id[0] || !id || strcmp(id, U.id)) {
		pthread_mutex_unlock(&SL);
		*err = "no such upload (begin first; an idle one is abandoned)";
		return -1;
	}
	if (off != U.got) {
		*got = U.got;
		pthread_mutex_unlock(&SL);
		*err = "out of order: send the chunk at offset got";
		return -1;
	}
	if (n == 0 || n > PC_UPG_CHUNK_MAX || U.got + (long long)n > U.size) {
		pthread_mutex_unlock(&SL);
		*err = "a chunk is 1 byte to 256 KB, and never past the size";
		return -1;
	}
	while (w < n) {
		ssize_t r = write(U.fd, b + w, n - w);

		if (r <= 0) {
			upload_drop();
			pthread_mutex_unlock(&SL);
			*err = "the write failed - the upload is dropped";
			return -1;
		}
		w += (size_t)r;
	}
	U.got += (long long)n;
	U.last = time(NULL);
	*got = U.got;
	pthread_mutex_unlock(&SL);
	return 0;
}

int pc_upg_stage_end(const char *id, char *sha_hex, size_t shl, char *who,
		size_t wl, const char **err)
{
	unsigned char h[32];
	char part[700], dst[800], nm[96], hx[65], ver[48];
	size_t len = 0;
	void *data;
	int fd, ok;
	FILE *f;

	pthread_mutex_lock(&SL);
	if (!U.id[0] || !id || strcmp(id, U.id)) {
		pthread_mutex_unlock(&SL);
		*err = "no such upload";
		return -1;
	}
	if (U.got != U.size) {
		pthread_mutex_unlock(&SL);
		*err = "not all of it has arrived";
		return -1;
	}
	if (fsync(U.fd) != 0 || close(U.fd) != 0) {
		U.fd = -1;
		upload_drop();
		pthread_mutex_unlock(&SL);
		*err = "the file could not be made durable - dropped";
		return -1;
	}
	U.fd = -1;
	snprintf(nm, sizeof nm, "%s.part", U.id);
	upath(part, sizeof part, "staging", nm);
	/* the whole file, read back: what is verified is what is published */
	data = pc_upg_read_file(part, &len);
	if (!data || (long long)len != U.size) {
		free(data);
		upload_drop();
		refused_n++;
		pthread_mutex_unlock(&SL);
		*err = "the uploaded file cannot be read back - dropped";
		return -1;
	}
	crypto_hash_sha256(h, data, len);
	sodium_bin2hex(hx, sizeof hx, U.sha, 32);
	if (sodium_memcmp(h, U.sha, 32) != 0) {
		free(data);
		upload_drop();
		refused_n++;
		pthread_mutex_unlock(&SL);
		*err = "sha256 mismatch - refused";
		LM_WARN("upgrade: an upload of %s did not match its sha256 - "
			"refused\n", hx);
		return -1;
	}
	if (U.has_sig)
		ok = pc_upg_verify(data, len, U.sig, &UC, who, wl) == 0;
	else {
		snprintf(who, wl, "nobody - unsigned, [upgrade] allow_unsigned = yes");
		ok = UC.allow_unsigned;
	}
	free(data);
	if (!ok) {
		upload_drop();
		refused_n++;
		pthread_mutex_unlock(&SL);
		*err = "the signature does not verify - refused";
		LM_WARN("upgrade: an upload of %s is signed by %s - refused\n",
			hx, who);
		return -1;
	}
	/* published: the signature and the meta first, the tarball last - a
	 * build is staged when its .tar.gz exists */
	snprintf(nm, sizeof nm, "%s.tar.gz.sig", hx);
	if (U.has_sig && upath(dst, sizeof dst, "staged", nm) == 0) {
		fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
		if (fd < 0 || write(fd, U.sig, PC_UPG_SIG_LEN) != PC_UPG_SIG_LEN) {
			if (fd >= 0)
				close(fd);
			upload_drop();
			pthread_mutex_unlock(&SL);
			*err = "cannot write the signature beside it";
			return -1;
		}
		close(fd);
	}
	snprintf(nm, sizeof nm, "%s.meta", hx);
	if (upath(dst, sizeof dst, "staged", nm) == 0 && (f = fopen(dst, "w"))) {
		fprintf(f, "version=%s\nsize=%lld\nsigner=%s\nstaged=%lld\n",
			U.ver, U.size, who, (long long)time(NULL));
		fclose(f);
	}
	snprintf(nm, sizeof nm, "%s.tar.gz", hx);
	if (upath(dst, sizeof dst, "staged", nm) != 0 || rename(part, dst) != 0) {
		upload_drop();
		pthread_mutex_unlock(&SL);
		*err = "cannot publish it in upgrade/staged";
		return -1;
	}
	snprintf(ver, sizeof ver, "%s", U.ver);
	U.id[0] = 0;                          /* the part is now the build */
	U.got = U.size = 0;
	staged_n++;
	snprintf(sha_hex, shl, "%s", hx);
	pthread_mutex_unlock(&SL);
	LM_NOTICE("upgrade: staged %s as %s, signed by %s\n", ver, hx, who);
	return 0;
}

int pc_upg_stage_abort(const char *id, const char **err)
{
	pthread_mutex_lock(&SL);
	if (!U.id[0] || !id || strcmp(id, U.id)) {
		pthread_mutex_unlock(&SL);
		*err = "no such upload";
		return -1;
	}
	upload_drop();
	pthread_mutex_unlock(&SL);
	return 0;
}

static void meta_get(const char *sha, const char *key, char *out, size_t ol)
{
	char p[800], nm[96], line[256];
	size_t kl = strlen(key);
	FILE *f;

	out[0] = 0;
	snprintf(nm, sizeof nm, "%.64s.meta", sha);
	if (upath(p, sizeof p, "staged", nm) != 0 || !(f = fopen(p, "r")))
		return;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, key, kl) && line[kl] == '=') {
			line[strcspn(line, "\r\n")] = 0;
			snprintf(out, ol, "%s", line + kl + 1);
			break;
		}
	fclose(f);
}

static void switch_tree(struct pc_tw *out);
static void asks_tree(struct pc_tw *out);

void pc_upg_status_tree(struct pc_tw *out)
{
	struct sent e[16];
	char v[200];
	int n, i;

	pthread_mutex_lock(&SL);
	n = staged_list(e, 16);
	pc_tw_map(out);
	pc_tw_key(out, "staged");
	pc_tw_arr(out);
	for (i = n - 1; i >= 0; i--) {          /* newest first */
		pc_tw_map(out);
		pc_tw_key(out, "sha256");
		pc_tw_str(out, e[i].sha);
		meta_get(e[i].sha, "version", v, sizeof v);
		pc_tw_key(out, "version");
		pc_tw_str(out, v);
		pc_tw_key(out, "size");
		pc_tw_i64(out, e[i].size);
		meta_get(e[i].sha, "signer", v, sizeof v);
		pc_tw_key(out, "signer");
		pc_tw_str(out, v);
		pc_tw_key(out, "staged");
		pc_tw_i64(out, (long long)e[i].at);
		pc_tw_end(out);
	}
	pc_tw_end(out);
	pc_tw_key(out, "upload");
	if (U.id[0]) {
		pc_tw_map(out);
		pc_tw_key(out, "id");
		pc_tw_str(out, U.id);
		pc_tw_key(out, "version");
		pc_tw_str(out, U.ver);
		pc_tw_key(out, "got");
		pc_tw_i64(out, U.got);
		pc_tw_key(out, "size");
		pc_tw_i64(out, U.size);
		pc_tw_key(out, "idle_s");
		pc_tw_i64(out, (long long)(time(NULL) - U.last));
		pc_tw_end(out);
	} else {
		pc_tw_nil(out);
	}
	pc_tw_key(out, "staged_total");
	pc_tw_i64(out, (long long)staged_n);
	pc_tw_key(out, "refused_total");
	pc_tw_i64(out, (long long)refused_n);
	pc_tw_key(out, "relay");
	pc_tw_arr(out);
	pthread_mutex_lock(&QL);
	{
		int i;

		for (i = 0; i < RELAY_MAX; i++) {
			struct in_addr a;

			if (!RL[i].addr)
				continue;
			a.s_addr = RL[i].addr;
			pc_tw_map(out);
			pc_tw_key(out, "member");
			pc_tw_str(out, inet_ntoa(a));
			pc_tw_key(out, "status");
			pc_tw_str(out, relay_word(RL[i].status));
			pc_tw_key(out, "at");
			pc_tw_i64(out, (long long)RL[i].at);
			pc_tw_end(out);
		}
	}
	pthread_mutex_unlock(&QL);
	pc_tw_end(out);
	switch_tree(out);                       /* UPS */
	pthread_mutex_lock(&QL);
	asks_tree(out);
	pthread_mutex_unlock(&QL);
	pc_tw_key(out, "stage_mb");
	pc_tw_i64(out, UC.stage_mb);
	pc_tw_key(out, "allow_unsigned");
	pc_tw_bool(out, UC.allow_unsigned);
	pc_tw_end(out);
	pthread_mutex_unlock(&SL);
}

int pc_upg_relay_staged(const char *sha_hex, int *older)
{
	unsigned char sha[32], sig[PC_UPG_SIG_LEN];
	char p[800], nm[96], ver[48];
	size_t len = 0;
	void *data;
	int has_sig, n;

	*older = 0;
	if (hexsha(sha_hex, sha) != 0)
		return 0;
	meta_get(sha_hex, "version", ver, sizeof ver);
	snprintf(nm, sizeof nm, "%.64s.tar.gz.sig", sha_hex);
	has_sig = upath(p, sizeof p, "staged", nm) == 0 &&
		pc_upg_read_sig(p, sig) == 0;
	snprintf(nm, sizeof nm, "%.64s.tar.gz", sha_hex);
	if (!ver[0] || upath(p, sizeof p, "staged", nm) != 0 ||
	        !(data = pc_upg_read_file(p, &len)))
		return 0;
	n = pc_cluster_stage_relay(data, len, sha, has_sig ? sig : NULL, ver,
		older);
	free(data);
	return n;
}

/* ---- UPS: the switch ------------------------------------------------------ */

const char *pc_upg_helper_state(void)
{
	static const char *const units[] = {
		"/etc/systemd/system/perfcached-upgrade.path",
		"/usr/lib/systemd/system/perfcached-upgrade.path",
		"/lib/systemd/system/perfcached-upgrade.path",
	};
	static const char *const wants[] = {
		"/etc/systemd/system/multi-user.target.wants/perfcached-upgrade.path",
		"/etc/systemd/system/paths.target.wants/perfcached-upgrade.path",
	};
	struct stat st;
	size_t i;

	for (i = 0; i < sizeof wants / sizeof wants[0]; i++)
		if (stat(wants[i], &st) == 0)
			return "enabled";
	for (i = 0; i < sizeof units / sizeof units[0]; i++)
		if (stat(units[i], &st) == 0)
			return "installed";
	return "absent";
}

int pc_upg_switch(const char *sha_hex, int force, char *id, size_t idl,
		char *ver, size_t vl, const char **err)
{
	unsigned char sha[32], h[32], sig[PC_UPG_SIG_LEN], rnd[8];
	char p[800], tmp[820], nm[96], who[96], rid[17];
	struct stat st;
	size_t len = 0;
	void *data;
	int has_sig, ok, fd, n;
	char body[400];

	if (!N.dir[0]) {
		*err = "this node has no state directory";
		return -1;
	}
	if (hexsha(sha_hex, sha) != 0) {
		*err = "sha256 must be 64 lowercase hex characters";
		return -1;
	}
	pthread_mutex_lock(&SL);
	if (upath(p, sizeof p, "request", NULL) == 0 && lstat(p, &st) == 0) {
		pthread_mutex_unlock(&SL);
		*err = "a request is already waiting for the helper";
		return PC_UPG_SW_WAITING;
	}
	/* the cheap answers first - not staged, no helper - and only then
	 * the read and the verification */
	meta_get(sha_hex, "version", ver, vl);
	snprintf(nm, sizeof nm, "%.64s.tar.gz", sha_hex);
	if (!ver[0] || upath(p, sizeof p, "staged", nm) != 0 || stat(p, &st) != 0) {
		pthread_mutex_unlock(&SL);
		*err = "no such build staged here";
		return PC_UPG_SW_NOT_STAGED;
	}
	if (!force && strcmp(pc_upg_helper_state(), "enabled") != 0) {
		pthread_mutex_unlock(&SL);
		*err = "the upgrade helper is not enabled on this host "
			"(systemctl enable --now perfcached-upgrade.path) - "
			"force to write the request all the same";
		return PC_UPG_SW_NO_HELPER;
	}
	if (!(data = pc_upg_read_file(p, &len))) {
		pthread_mutex_unlock(&SL);
		*err = "no such build staged here";
		return PC_UPG_SW_NOT_STAGED;
	}
	/* verified again: the file as it is now, before root is asked */
	crypto_hash_sha256(h, data, len);
	snprintf(nm, sizeof nm, "%.64s.tar.gz.sig", sha_hex);
	has_sig = upath(tmp, sizeof tmp, "staged", nm) == 0 &&
		pc_upg_read_sig(tmp, sig) == 0;
	ok = sodium_memcmp(h, sha, 32) == 0 &&
		(has_sig ? pc_upg_verify(data, len, sig, &UC, who, sizeof who) == 0
		: UC.allow_unsigned);
	free(data);
	if (!ok) {
		pthread_mutex_unlock(&SL);
		*err = "the staged build no longer verifies - not requested";
		LM_WARN("upgrade: staged %s no longer verifies - not requested\n",
			sha_hex);
		return PC_UPG_SW_UNVERIFIED;
	}
	randombytes_buf(rnd, sizeof rnd);
	sodium_bin2hex(rid, sizeof rid, rnd, sizeof rnd);
	n = snprintf(body, sizeof body, "id=%s\nartifact=staged/%.64s.tar.gz\n"
		"sha256=%.64s\nversion=%s\n", rid, sha_hex, sha_hex, ver);
	/* the id first, so the process the helper starts knows what to ask */
	if (upath(p, sizeof p, "requested", NULL) == 0 &&
	        upath(tmp, sizeof tmp, "requested.tmp", NULL) == 0) {
		FILE *f = fopen(tmp, "w");

		if (!f || fprintf(f, "%s\n%.64s\n%lld\n", rid, sha_hex,
		        (long long)time(NULL)) < 0 || fclose(f) != 0 ||
		        rename(tmp, p) != 0) {
			pthread_mutex_unlock(&SL);
			*err = "cannot write upgrade/requested";
			return -1;
		}
	}
	if (upath(p, sizeof p, "request", NULL) != 0 ||
	        upath(tmp, sizeof tmp, "request.tmp", NULL) != 0 ||
	        (fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640)) < 0) {
		pthread_mutex_unlock(&SL);
		*err = "cannot write upgrade/request";
		return -1;
	}
	{
		int bad = write(fd, body, (size_t)n) != n || fsync(fd) != 0;

		bad |= close(fd) != 0;                  /* closed on every path */
		if (bad || rename(tmp, p) != 0) {
			unlink(tmp);
			pthread_mutex_unlock(&SL);
			*err = "cannot write upgrade/request";
			return -1;
		}
	}
	pthread_mutex_unlock(&SL);
	snprintf(id, idl, "%s", rid);
	LM_NOTICE("upgrade: asked the helper to install %s (%s), request %s%s\n",
		ver, sha_hex, rid, force ? " - FORCED, no helper seen" : "");
	return 0;
}

/* the request this node (or the process before it) made, and the helper's
 * word on it: upgrade/requested, then <results>/result-<id> */
static void switch_tree(struct pc_tw *out)
{
	char p[800], line[200], rid[40] = "", rsha[80] = "", rat[40] = "";
	char rp[200], k[40], v[300];
	struct stat st;
	FILE *f;

	pc_tw_key(out, "helper");
	pc_tw_str(out, pc_upg_helper_state());
	pc_tw_key(out, "request_waiting");
	pc_tw_bool(out, upath(p, sizeof p, "request", NULL) == 0 &&
		lstat(p, &st) == 0);
	pc_tw_key(out, "requested");
	if (upath(p, sizeof p, "requested", NULL) != 0 || !(f = fopen(p, "r"))) {
		pc_tw_nil(out);
		return;
	}
	if (fgets(rid, sizeof rid, f))
		rid[strcspn(rid, "\r\n")] = 0;
	if (fgets(rsha, sizeof rsha, f))
		rsha[strcspn(rsha, "\r\n")] = 0;
	if (fgets(rat, sizeof rat, f))
		rat[strcspn(rat, "\r\n")] = 0;
	fclose(f);
	pc_tw_map(out);
	pc_tw_key(out, "id");
	pc_tw_str(out, rid);
	pc_tw_key(out, "sha256");
	pc_tw_str(out, rsha);
	pc_tw_key(out, "at");
	pc_tw_i64(out, atoll(rat));
	pc_tw_key(out, "result");
	snprintf(rp, sizeof rp, "%.150s/result-%.32s", UC.results_dir, rid);
	if (strspn(rid, "0123456789abcdef") != strlen(rid) || !rid[0] ||
	        !(f = fopen(rp, "r"))) {
		pc_tw_nil(out);
	} else {
		pc_tw_map(out);
		while (fgets(line, sizeof line, f)) {
			char *eq = strchr(line, '=');

			line[strcspn(line, "\r\n")] = 0;
			if (!eq || eq == line)
				continue;
			*eq = 0;
			snprintf(k, sizeof k, "%.39s", line);
			snprintf(v, sizeof v, "%s", eq + 1);
			if (!strcmp(k, "status") || !strcmp(k, "reason") ||
			        !strcmp(k, "from") || !strcmp(k, "to")) {
				pc_tw_key(out, k);
				pc_tw_str(out, v);
			} else if (!strcmp(k, "at")) {
				pc_tw_key(out, "at");
				pc_tw_i64(out, atoll(v));
			}
		}
		fclose(f);
		pc_tw_end(out);
	}
	pc_tw_end(out);
}

/* ---- UPS: the fact ---------------------------------------------------------- */

uint32_t pc_upg_fact(void)
{
	return __atomic_load_n(&FACT, __ATOMIC_RELAXED);
}

const char *pc_upg_fact_helper(uint32_t f)
{
	static const char *const w[] = { "absent", "installed", "enabled", "?" };

	return w[f & 3];
}

const char *pc_upg_fact_state(uint32_t f)
{
	static const char *const w[] = { "idle", "staged", "requested", "done",
		"failed" };
	unsigned int s = (f >> 4) & 15;

	return s < 5 ? w[s] : "?";
}

static void fact_tick(void)
{
	struct sent e[16];
	char p[800], rid[40] = "", rsha[80] = "", rp[200], line[120];
	const char *h = pc_upg_helper_state();
	uint32_t f, st = PC_UPG_ST_IDLE, pre = 0;
	struct stat sb;
	int n;
	FILE *fp;

	f = !strcmp(h, "enabled") ? 2u : !strcmp(h, "installed") ? 1u : 0u;
	pthread_mutex_lock(&SL);
	n = staged_list(e, 16);
	pthread_mutex_unlock(&SL);
	if (n > 0) {
		unsigned char b[32];

		if (hexsha(e[n - 1].sha, b) == 0)
			pre = (uint32_t)b[0] << 8 | b[1];
		st = PC_UPG_ST_STAGED;
		f |= PC_UPG_FACT_HAS_STAGED;    /* a prefix of 0000 is a prefix */
	}
	if (upath(p, sizeof p, "requested", NULL) == 0 && (fp = fopen(p, "r"))) {
		if (fgets(rid, sizeof rid, fp))
			rid[strcspn(rid, "\r\n")] = 0;
		if (fgets(rsha, sizeof rsha, fp))
			rsha[strcspn(rsha, "\r\n")] = 0;
		fclose(fp);
	}
	if (upath(p, sizeof p, "request", NULL) == 0 && lstat(p, &sb) == 0) {
		st = PC_UPG_ST_REQUESTED;
	} else if (rid[0] && (n == 0 || !strcmp(rsha, e[n - 1].sha)) &&
	        strspn(rid, "0123456789abcdef") == strlen(rid)) {
		/* the newest staged build is the one last requested: the
		 * helper's word on it, if there is one */
		snprintf(rp, sizeof rp, "%.150s/result-%.32s", UC.results_dir, rid);
		if ((fp = fopen(rp, "r"))) {
			while (fgets(line, sizeof line, fp))
				if (!strncmp(line, "status=", 7)) {
					st = !strncmp(line + 7, "ok", 2) ?
						PC_UPG_ST_DONE : PC_UPG_ST_FAILED;
					break;
				}
			fclose(fp);
		}
	}
	__atomic_store_n(&FACT, f | st << 4 | pre << 16, __ATOMIC_RELAXED);
}

/* ---- UPS: M_UPG_SWITCH / M_UPG_ACK - the queues ------------------------------ */

static struct {
	int state;                     /* 0 free, 1 asked, 2 answered */
	uint32_t addr, req;
	uint16_t port;
	char sha[65];
	unsigned int status;
} SW;

void pc_upg_switch_ask(uint32_t addr_be, uint16_t port_be, uint32_t req,
		const unsigned char *sha)
{
	pthread_mutex_lock(&QL);
	if (SW.state == 0) {
		SW.state = 1;
		SW.addr = addr_be;
		SW.port = port_be;
		SW.req = req;
		sodium_bin2hex(SW.sha, sizeof SW.sha, sha, 32);
	}
	/* else one is being looked at: the asker hears nothing and asks
	 * again - an asker that waits forever is not one this answers */
	pthread_mutex_unlock(&QL);
}

/* the maintenance thread: look at an asked switch */
static void switch_asked_tick(void)
{
	char sha[65], id[33], ver[48];
	const char *e = "";
	int r;

	pthread_mutex_lock(&QL);
	if (SW.state != 1) {
		pthread_mutex_unlock(&QL);
		return;
	}
	memcpy(sha, SW.sha, sizeof sha);
	pthread_mutex_unlock(&QL);
	r = pc_upg_switch(sha, 0, id, sizeof id, ver, sizeof ver, &e);
	if (r != 0) {
		struct in_addr a;

		a.s_addr = SW.addr;
		LM_WARN("upgrade: member %s asked this node to install %s - "
			"refused: %s\n", inet_ntoa(a), sha, e);
	}
	pthread_mutex_lock(&QL);
	SW.status = r == 0 ? UPGSW_ACCEPTED_ : r == PC_UPG_SW_NO_HELPER ?
		UPGSW_NO_HELPER_ : r == PC_UPG_SW_WAITING ? UPGSW_BUSY_ :
		r == PC_UPG_SW_NOT_STAGED ? UPGSW_NOT_STAGED_ :
		r == PC_UPG_SW_UNVERIFIED ? UPGSW_UNVERIFIED_ : UPGSW_OTHER_;
	SW.state = 2;
	pthread_mutex_unlock(&QL);
}

int pc_upg_switch_answer(uint32_t *addr_be, uint16_t *port_be, uint32_t *req,
		unsigned int *status)
{
	int r = 0;

	pthread_mutex_lock(&QL);
	if (SW.state == 2) {
		*addr_be = SW.addr;
		*port_be = SW.port;
		*req = SW.req;
		*status = SW.status;
		SW.state = 0;
		r = 1;
	}
	pthread_mutex_unlock(&QL);
	return r;
}

/* asking: one at a time, and what each member answered */
#define ASK_MAX 32
static struct {
	int queued;
	uint32_t addr, req;
	unsigned char sha[32];
} AQ;
static struct {
	uint32_t addr, req;
	unsigned int status;           /* UPGSW_*, or ASK_SENT */
	time_t at;
	char sha[9];
} AK[ASK_MAX];
#define ASK_SENT 100u

int pc_upg_ask_member(uint32_t addr_be, const char *sha_hex, const char **err)
{
	unsigned char sha[32];

	if (hexsha(sha_hex, sha) != 0) {
		*err = "sha256 must be 64 lowercase hex characters";
		return -1;
	}
	pthread_mutex_lock(&QL);
	if (AQ.queued) {
		pthread_mutex_unlock(&QL);
		*err = "another ask is still waiting to be sent";
		return -1;
	}
	AQ.queued = 1;
	AQ.addr = addr_be;
	randombytes_buf(&AQ.req, sizeof AQ.req);
	memcpy(AQ.sha, sha, 32);
	pthread_mutex_unlock(&QL);
	return 0;
}

static void ask_note(uint32_t addr, uint32_t req, unsigned int st,
		const unsigned char *sha)    /* under QL */
{
	int i, slot = -1, old = 0;

	for (i = 0; i < ASK_MAX; i++) {
		if (AK[i].addr == addr) {
			slot = i;
			break;
		}
		if (slot < 0 && !AK[i].addr)
			slot = i;
		if (AK[i].at < AK[old].at)
			old = i;
	}
	if (slot < 0)
		slot = old;
	if (st != ASK_SENT && (AK[slot].addr != addr || AK[slot].req != req))
		return;                        /* an answer to an ask not ours */
	AK[slot].addr = addr;
	AK[slot].req = req;
	AK[slot].status = st;
	AK[slot].at = time(NULL);
	if (sha)
		sodium_bin2hex(AK[slot].sha, sizeof AK[slot].sha, sha, 4);
}

int pc_upg_ask_take(uint32_t *addr_be, uint32_t *req, unsigned char *sha)
{
	int r = 0;

	pthread_mutex_lock(&QL);
	if (AQ.queued) {
		*addr_be = AQ.addr;
		*req = AQ.req;
		memcpy(sha, AQ.sha, 32);
		ask_note(AQ.addr, AQ.req, ASK_SENT, AQ.sha);
		AQ.queued = 0;
		r = 1;
	}
	pthread_mutex_unlock(&QL);
	return r;
}

void pc_upg_ask_acked(uint32_t addr_be, uint32_t req, unsigned int status)
{
	pthread_mutex_lock(&QL);
	ask_note(addr_be, req, status, NULL);
	pthread_mutex_unlock(&QL);
}

static const char *ask_word(unsigned int st)
{
	switch (st) {
	case UPGSW_ACCEPTED_:   return "accepted: the request is written there";
	case UPGSW_NOT_STAGED_: return "refused: not staged there";
	case UPGSW_UNVERIFIED_: return "refused: it no longer verifies there";
	case UPGSW_NO_HELPER_:  return "refused: no helper enabled there";
	case UPGSW_BUSY_:       return "refused: a request already waits there";
	case ASK_SENT:          return "sent, no answer yet";
	default:                return "refused";
	}
}

static void asks_tree(struct pc_tw *out)    /* under QL */
{
	int i;

	pc_tw_key(out, "asked");
	pc_tw_arr(out);
	for (i = 0; i < ASK_MAX; i++) {
		struct in_addr a;

		if (!AK[i].addr)
			continue;
		a.s_addr = AK[i].addr;
		pc_tw_map(out);
		pc_tw_key(out, "member");
		pc_tw_str(out, inet_ntoa(a));
		pc_tw_key(out, "staged");
		pc_tw_str(out, AK[i].sha);
		pc_tw_key(out, "status");
		pc_tw_str(out, ask_word(AK[i].status));
		pc_tw_key(out, "at");
		pc_tw_i64(out, (long long)AK[i].at);
		pc_tw_end(out);
	}
	pc_tw_end(out);
}
