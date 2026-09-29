# Contributing

## Licences and the boundary

Two licences, one boundary, checked by a tool on every push:

- Files in the libperfd export set (`FILES` in `tools/sync-libperfd.sh`)
  are **MIT** and carry `SPDX-License-Identifier: MIT`.
- Every other file under `src/`, `cli/`, `tools/` and `test/` is
  **GPL-2.0-or-later** and carries that SPDX line.  The six vendored
  `src/core/` files keep their full OpenSIPS GPL headers instead of an
  SPDX line - a recorded exception, not an omission.
- A header both sides need goes on the MIT side from day one with no
  GPL-only dependency, or stays daemon-only with the minimal piece
  duplicated under MIT (as `PC_MUST_CHECK` is).  Never include a GPL
  header from an MIT one, not even temporarily.
- `src/compat/dprint.h` is GPL and is resolved but never copied by the
  sync; the consumer supplies its own shim.  Leave its tag alone.
- `test/synctest.sh` runs in `make check-fast`, so every push proves
  the boundary.  A red sync blocks any tag that ships libperfd.
- Third-party code stays third-party: libsodium is ISC and is named in
  `lib/NOTICE`.  Adding a dependency to the export means adding its
  notice there.

Expanding the MIT set is a licensing decision, not a refactor - say so
in the commit and the ledger.

## Copyright

The project relies on being single-copyright-holder to keep licensing
decisions simple.  Outside contributions need a DCO sign-off
(`Signed-off-by:` in the commit).
