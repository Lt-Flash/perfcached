/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* statefile.c - the state directory's small files (term, cluster id, identity, salt): one write
 * path, so none of them reports a store the directory never made durable */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "statefile.h"

int pc_state_sync_dir(const char *dir)
{
	int fd = open(dir, O_RDONLY | O_DIRECTORY), e;

	if (fd < 0)
		return -1;
	if (fsync(fd) != 0) {
		e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	close(fd);
	return 0;
}

int pc_state_write(const char *dir, const char *name, const void *buf, size_t n)
{
	char fin[640], tmp[660];
	const char *p = buf;
	int fd, e;

	if (snprintf(fin, sizeof fin, "%s/%s", dir, name) >= (int)sizeof fin) {
		errno = ENAMETOOLONG;
		return -1;
	}
	snprintf(tmp, sizeof tmp, "%s.tmp.%d", fin, (int)getpid());
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0) {
			if (w == 0)
				errno = EIO;
			goto fail;
		}
		p += w;
		n -= (size_t)w;
	}
	if (fsync(fd) != 0)
		goto fail;
	e = close(fd);
	fd = -1;
	if (e != 0 || rename(tmp, fin) != 0)
		goto fail;
	return pc_state_sync_dir(dir);
fail:
	e = errno;
	if (fd >= 0)
		close(fd);
	unlink(tmp);
	errno = e;
	return -1;
}
