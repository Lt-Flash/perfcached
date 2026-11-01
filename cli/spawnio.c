/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* spawnio.c - S344: see spawnio.h */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "spawnio.h"

extern char **environ;

/* one spawn at a time: another thread's child must not inherit this pipe */
static pthread_mutex_t spawn_mx = PTHREAD_MUTEX_INITIALIZER;

int spawn_plain_name(const char *name)
{
	return name && name[0] && !strchr(name, '/') && strcmp(name, ".") &&
		strcmp(name, "..");
}

static int wait_exit(pid_t pid)
{
	int st;

	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int spawn_rd_open(struct spawn_rd *r, const char *path, int zstd)
{
	char *argv[] = { "zstd", "-dc", "-q", "--", (char *)path, NULL };
	posix_spawn_file_actions_t fa;
	int fd[2], rc;

	r->f = NULL;
	r->pid = 0;
	if (!zstd)
		return (r->f = fopen(path, "rb")) ? 0 : -1;
	pthread_mutex_lock(&spawn_mx);
	if (pipe(fd) != 0) {
		pthread_mutex_unlock(&spawn_mx);
		return -1;
	}
	fcntl(fd[0], F_SETFD, FD_CLOEXEC);
	fcntl(fd[1], F_SETFD, FD_CLOEXEC);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fd[1], STDOUT_FILENO);
	rc = posix_spawnp(&r->pid, "zstd", &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(fd[1]);
	pthread_mutex_unlock(&spawn_mx);
	if (rc != 0 || !(r->f = fdopen(fd[0], "rb"))) {
		close(fd[0]);
		if (rc == 0)
			wait_exit(r->pid);
		r->pid = 0;
		return -1;
	}
	return 0;
}

int spawn_rd_close(struct spawn_rd *r)
{
	int rc = r->f ? fclose(r->f) : 0;

	r->f = NULL;
	if (r->pid > 0) {
		rc = wait_exit(r->pid) == 0 && rc == 0 ? 0 : -1;
		r->pid = 0;
	}
	return rc;
}

int spawn_run(char *const argv[])
{
	pid_t pid;
	int rc;

	pthread_mutex_lock(&spawn_mx);
	rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
	pthread_mutex_unlock(&spawn_mx);
	return rc != 0 ? -1 : wait_exit(pid);
}
