/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* version.h - the daemon's ONE version source (semver; -dev marks an unreleased tree).
 * The clients version separately (lib/perfd.h): they ship into other codebases. */
#ifndef PC_VERSION_H
#define PC_VERSION_H

#define PC_VERSION_MAJOR 0
#define PC_VERSION_MINOR 7
#define PC_VERSION_PATCH 5
#define PC_VERSION_TWEAK 0          /* the fourth number, the daemon's alone; 0: none */
#define PC_VERSION "0.7.5"

#endif /* PC_VERSION_H */
