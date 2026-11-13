/* sha1.h - SHA-1 (FIPS 180-4) to name a script as EVALSHA / SCRIPT LOAD do;
 * not a security primitive: a script is approved by the daemon's list. */
#ifndef PC_SHA1_H
#define PC_SHA1_H

#include <stddef.h>

/* the 40-character lowercase hex digest of @len bytes at @p, NUL-terminated */
void pc_sha1_hex(const void *p, size_t len, char out[41]);

#endif
