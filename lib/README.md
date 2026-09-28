# libperfd

The client library for perfcached, as a self-contained export: a C API
over the native binary and JSON-RPC dialects, with the Noise handshake,
the slot placement and the key mixing the daemon uses, so a client can
place keys itself.

## Licence

**MIT** - see `LICENSE` here.  The export is exactly the set named in
`tools/sync-libperfd.sh` (`FILES`), and that script is the authority:
every exported file carries an `SPDX-License-Identifier: MIT` line, and
the script follows every quoted include and refuses the whole export if
any resolves outside the set.  A red sync blocks any tag that ships
libperfd.

`src/compat/dprint.h` is a GPL file the script resolves but never
copies: the consumer supplies its own `LM_*` logging shim.  Its tag is
not a crossing and must not be "fixed" by relicensing it.

The one third-party dependency is libsodium (ISC) - see `NOTICE`, which
the sync copies beside `LICENSE`.  Effective licence of a redistributed
libperfd: MIT plus that notice.

## Where the rest lives

The daemon, the cluster plane and everything else under `src/` are
GPL-2.0-or-later (`COPYING` at the repository root).  A header that
both sides need goes on the MIT side from day one, or stays daemon-only
with the minimal piece duplicated on the MIT side - never a GPL include
from an MIT header, not even temporarily.
