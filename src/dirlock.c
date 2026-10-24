/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * dirlock.c - S290: one daemon per state / WAL directory.  See dirlock.h.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "compat/dprint.h"
#include "dirlock.h"

#define DIRLOCK_NAME    "perfcached.lock"
#define DIRLOCK_WAIT_MS 5000           /* a dying predecessor's grace */

int pc_dir_lock(const char *dir, const char *what, int create)
{
	char path[4096], pid[32];
	struct flock fl;
	struct stat st;
	int fd, n;

	if (!dir || !*dir)
		return 0;
	if (stat(dir, &st) != 0) {
		if (!create || errno != ENOENT)
			return 0;              /* the caller's checks say why */
		if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
			LM_CRIT("%s %s cannot be created: %s - refusing to "
				"start\n", what, dir, strerror(errno));
			return -1;
		}
	} else if (!S_ISDIR(st.st_mode)) {
		return 0;                      /* the caller says "not a dir" */
	}
	if ((size_t)snprintf(path, sizeof path, "%s/" DIRLOCK_NAME, dir)
	        >= sizeof path) {
		LM_CRIT("%s %s: path too long for its lock file\n", what, dir);
		return -1;
	}
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0) {
		LM_CRIT("%s %s: cannot open its lock file %s: %s - refusing "
			"to start\n", what, dir, path, strerror(errno));
		return -1;
	}
	/* A predecessor that is still DYING holds the lock for a moment: a
	 * kill -9 is delivered asynchronously, a clean stop takes as long
	 * as its last snapshot, and a restart that starts the next process
	 * at once would otherwise be refused by its own past.  So wait up to
	 * DIRLOCK_WAIT_MS for the holder to let go, saying so once; a
	 * holder that is alive and staying is refused after the wait. */
	{
		int waited = 0, said = 0;

		for (;;) {
			memset(&fl, 0, sizeof fl);
			fl.l_type = F_WRLCK;
			fl.l_whence = SEEK_SET;    /* the whole file */
			if (fcntl(fd, F_SETLK, &fl) == 0)
				break;
			if ((errno != EAGAIN && errno != EACCES) ||
			        waited >= DIRLOCK_WAIT_MS)
				break;
			if (!said) {
				struct flock q;

				memset(&q, 0, sizeof q);
				q.l_type = F_WRLCK;
				q.l_whence = SEEK_SET;
				if (fcntl(fd, F_GETLK, &q) == 0 &&
				        q.l_type != F_UNLCK)
					LM_NOTICE("%s %s is held by pid %d - "
						"waiting up to %d s for it to let "
						"go\n", what, dir, (int)q.l_pid,
						DIRLOCK_WAIT_MS / 1000);
				said = 1;
			}
			usleep(100 * 1000);
			waited += 100;
		}
		if (said && fl.l_type == F_WRLCK && waited < DIRLOCK_WAIT_MS)
			LM_NOTICE("%s %s: the previous holder let go after %d ms\n",
				what, dir, waited);
	}
	if (fcntl(fd, F_SETLK, &fl) != 0) {
		int err = errno;

		memset(&fl, 0, sizeof fl);
		fl.l_type = F_WRLCK;
		fl.l_whence = SEEK_SET;
		if (fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type != F_UNLCK)
			LM_CRIT("%s %s is in use by another perfcached (pid %d) "
				"- two daemons on one directory would write the "
				"same identity, log and snapshot.  Refusing to "
				"start: stop the other one, or give this one its "
				"own %s\n", what, dir, (int)fl.l_pid, what);
		else
			LM_CRIT("%s %s: cannot lock %s: %s - refusing to "
				"start\n", what, dir, path, strerror(err));
		close(fd);
		return -1;
	}
	/* the pid in the file is for a human reading the directory; the
	 * lock is what decides */
	n = snprintf(pid, sizeof pid, "%d\n", (int)getpid());
	if (ftruncate(fd, 0) == 0 && write(fd, pid, (size_t)n) != n)
		LM_WARN("%s %s: could not write the pid into %s\n", what, dir,
			path);
	/* held for the life of the process: the fd is deliberately never
	 * closed - closing ANY descriptor of the file would drop an fcntl
	 * lock, which is why nothing else in the daemon opens this file */
	return 0;
}
