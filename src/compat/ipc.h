/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for OpenSIPS ipc.h — exactly one consumer: the arena's hoard
 * flush broadcast.  Runs inline on the caller until the daemon registers
 * a real all-threads broadcaster (compat_set_broadcast, task S6). */
#ifndef PC_COMPAT_IPC_H
#define PC_COMPAT_IPC_H

typedef void (*ipc_rpc_f)(int sender, void *param);

int ipc_send_rpc_all(ipc_rpc_f fn, void *param);

#endif /* PC_COMPAT_IPC_H */
