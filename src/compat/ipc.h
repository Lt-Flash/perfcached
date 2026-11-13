/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for the vendored core's ipc.h, used only by the arena's hoard flush broadcast; runs
 * inline on the caller until the daemon registers an all-threads one (compat_set_broadcast). */
#ifndef PC_COMPAT_IPC_H
#define PC_COMPAT_IPC_H

typedef void (*ipc_rpc_f)(int sender, void *param);

int ipc_send_rpc_all(ipc_rpc_f fn, void *param);

#endif /* PC_COMPAT_IPC_H */
