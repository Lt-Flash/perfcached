perfcached is licensed in two parts, on purpose.

THE DAEMON AND ITS TOOLS - everything that links src/core/ (the server,
its cluster plane, the tests and benches) and the tools beside it
(perfcli, perfdump, perfload, perfcached-dashboard) - are licensed
under the GNU General Public License, version 2 or (at your option) any
later version.  The full text is in COPYING.  The files under src/core/
keep their original GPL-2.0-or-later headers; the same terms cover the
rest of the daemon.

THE CLIENT LIBRARY - libperfd - is licensed under the MIT License (see
lib/LICENSE; LICENSE-MIT at the top is the same text), so it can be
embedded in software under any license, proprietary included.  The
MIT-licensed set is what the library is compiled from - PERFD_SRCS in
the Makefile and every file the compiler reads for them (make
print-libperfd); today:

    lib/perfd.c  lib/perfd.h  lib/perfd-*.inc  lib/perfd_push.h
    lib/perfd-log.c  lib/shim/compat/dprint.h
    src/json.c   src/json.h   src/ptree.c  src/ptree.h
    src/pc_noise.c  src/pc_noise.h  src/pc_slot.h  src/pc_mix.h
    src/pcaddr.c  src/pcaddr.h

None of them reads anything from the daemon's side (verified on every
push: test/mitboundtest.sh asks the compiler for the set and fails on any
file in it without an MIT SPDX line).

THE PHP EXTENSION - lang/php, with its Symfony adapter - is MIT as well
(lang/php/LICENSE); it includes no daemon header.

A file's SPDX-License-Identifier states which side it is on; files
without one are daemon-side, GPL-2.0-or-later.

Copyright (c) 2026 Yury Kirsanov
