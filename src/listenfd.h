/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * listenfd.h - the listener sockets (listenfd.c), for daemon.c.
 */
#ifndef PC_LISTENFD_H
#define PC_LISTENFD_H

#include "config.h"

/* 1 when something already listens on @l's address:port (a bind without
 * SO_REUSEPORT failed with EADDRINUSE); 0 otherwise, a failure included */
int tcp_port_taken(const struct pc_listener *l);
/* a non-blocking listening socket on @l, SO_REUSEPORT, dual-stack where
 * the address is IPv6; -1, logged */
int tcp_listener_fd(const struct pc_listener *l);
/* a non-blocking listening unix socket at @path; a stale file replaced,
 * a live one (another instance accepting there) refused; -1, logged */
int unix_listener_fd(const char *path);

#endif
