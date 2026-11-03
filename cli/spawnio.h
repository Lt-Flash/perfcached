/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* spawnio.h - S344: run zstd from an argv, never through a shell, and open
 * the files a dump's manifest names only as plain names in its directory */
#ifndef PC_SPAWNIO_H
#define PC_SPAWNIO_H

#include <stdio.h>
#include <sys/types.h>

struct spawn_rd {
	FILE *f;
	pid_t pid;                         /* 0: a plain file, not a process */
};

/* 1 when @name is a file name alone: no '/', not "." or "..", not empty */
int spawn_plain_name(const char *name);
/* @path's bytes: decompressed by `zstd -dc -q` when @zstd, else the file */
int spawn_rd_open(struct spawn_rd *r, const char *path, int zstd);
/* 0 when the file closed, or the process exited 0 */
int spawn_rd_close(struct spawn_rd *r);
/* run @argv (argv[0] looked up on PATH) and wait: its exit status, -1 on error */
int spawn_run(char *const argv[]);

#endif
