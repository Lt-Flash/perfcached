/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * version.h — the daemon's ONE version source.  Semver; -dev marks an
 * unreleased tree.  PC_BUILD_REV (the git revision) is injected per
 * build by the Makefile; the clients version separately on purpose
 * (PERFD_VERSION in lib/perfd.h, Perfcached::VERSION in the PHP class)
 * because they ship into other codebases and move on their own cadence.
 * Both surface at runtime: `perfcached -V`, the stats verb's version/
 * rev members, perfd_version(), `perfcli -V`.
 */
#ifndef PC_VERSION_H
#define PC_VERSION_H

#define PC_VERSION_MAJOR 0
#define PC_VERSION_MINOR 2
#define PC_VERSION_PATCH 1
#define PC_VERSION "0.2.1"

#endif /* PC_VERSION_H */
