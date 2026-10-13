<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="../contrib/brand/perfd-mark-dark.svg">
    <img src="../contrib/brand/perfd-mark-light.svg" width="96" alt="libperfd - the compass">
  </picture>
  <br>libperfd
</h1>

The client library for perfcached, as a self-contained export: a C API
over the native binary protocol (the fixed-layout data verbs and
`PC_VERB_CMD`, whose parameters and reply are trees - `src/ptree.[ch]`,
part of the export), with the Noise handshake, the slot placement and
the key mixing the daemon uses, so a client can place keys itself.
JSON appears only at the API edge (`perfd_command` and friends take and
return JSON text; the library converts), never on the wire.

## Building and linking (libperfd 0.6.0, perfcached 0.6.3)

`make libperfd.a libperfd.so` builds both from the same MIT sources;
`make install` puts `libperfd.so.<version>` (SONAME `libperfd.so.0`),
`libperfd.a`, `perfd.h` and `lib/pkgconfig/libperfd.pc` under the
prefix.  Link with pkg-config, shared by default:

```
cc -o app app.c $(pkg-config --cflags --libs libperfd)                # shared
cc -o app app.c $(pkg-config --cflags libperfd) /usr/lib/libperfd.a -lsodium -lpthread   # static
```

The SONAME changes only when the ABI breaks (a symbol removed, a struct
reshaped); a new library version with the same SONAME is a drop-in
replacement - install it and restart the program.  That holds because:

- `perfd_opts` carries its size: initialise it with `PERFD_OPTS_INIT`
  (or `perfd_opts_init()` from C++ or FFI) and the library reads only
  the fields your build knows;
- every buffer the library hands out is released with
  `perfd_free_value()`, never libc `free()`;
- errors carry a kind, `perfd_error_kind()` - branch on that, not on the
  text;
- the library logs through `perfd_set_log()` if you give it a callback
  (errors to stderr otherwise);
- it exports `perfd_*` and nothing else.

`test/abitest.sh` holds the shared library to that: a caller built
against the frozen header of its SONAME (`test/abi/`) must still run
against what the tree builds, and no exported symbol may disappear.

### Reads that allocate nothing (libperfd 0.6.0)

`perfd_get`, `perfd_jget` and `perfd_hget` hand back a value the
library allocated.  Their `_buf` twins - `perfd_get_buf`,
`perfd_jget_buf`, `perfd_hget_buf` (`PERFD_HAS_GET_BUF`) - copy it into a
buffer you own instead, so a read on a warm handle allocates nothing:
1 found, 0 absent, -1 error, and -2 when the buffer is too small, with
`*vlen` set to the size needed and the buffer left as it was.  The copy is
binary-safe and not NUL-terminated.

## Licence

**MIT** - see `LICENSE` here.  The MIT set is what the library is built
from: `PERFD_SRCS` in the Makefile and every file the compiler reads for
them (`make print-libperfd` gives the sources and flags).
`test/mitboundtest.sh`, in `make check-fast`, asks the compiler for that
list on every push and fails if any file in it lacks an
`SPDX-License-Identifier: MIT` line, or if the archive holds an object not
built from `PERFD_SRCS`.

The sources the library shares with the daemon log through
`#include <compat/dprint.h>`; the include path picks the logger -
`lib/shim/compat/dprint.h` (MIT, through `lib/perfd-log.c`) for the
library, the daemon's GPL `src/compat/dprint.h` for the daemon.  The
library's build never opens the daemon's file.  Before 0.5.0 the archive
carried the daemon's GPL `compat.o`: a crossing in the shipped `.a`, found
while building the shared library.

Consumers link the installed library (perfcached S371); none carries a
copy of its source.  The one third-party dependency is libsodium (ISC) -
see `NOTICE`.  Effective licence of a redistributed libperfd: MIT plus
that notice.

## Where the rest lives

The daemon, the cluster plane and everything else under `src/` are
GPL-2.0-or-later (`COPYING` at the repository root).  A header that
both sides need goes on the MIT side from day one, or stays daemon-only
with the minimal piece duplicated on the MIT side - never a GPL include
from an MIT header, not even temporarily.
