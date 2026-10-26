/* sha1.h - S170a: SHA-1 (FIPS 180-4), written for perfcached - used ONLY to
 * name a script the way Redis's EVALSHA / SCRIPT LOAD do (the SHA1 of its
 * body, lowercase hex).  Not a security primitive here: a script is
 * approved by a list the daemon carries, not by the hash's strength. */
#ifndef PC_SHA1_H
#define PC_SHA1_H

#include <stddef.h>

/* the 40-character lowercase hex digest of @len bytes at @p, NUL-terminated */
void pc_sha1_hex(const void *p, size_t len, char out[41]);

#endif
