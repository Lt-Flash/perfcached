/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for OpenSIPS timer.h — get_ticks(): whole seconds since start. */
#ifndef PC_COMPAT_TIMER_H
#define PC_COMPAT_TIMER_H

#include "compat.h"

typedef unsigned long long utime_t;

static inline unsigned int get_ticks(void)
{
	return compat_ticks();
}

#endif /* PC_COMPAT_TIMER_H */
