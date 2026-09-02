# Changelog

perfcached follows [semantic versioning](https://semver.org): the patch
digit is a fix inside a line, the minor digit is a real capability step.
Releases are tags, and **promises are made from tags** — the wire
dialect, the metric names and the compatibility rules described in the
README hold for a tagged release, not for whatever `master` says today.

Measured numbers in the README move with a release. A reader holding an
older page can therefore tell which build produced a claim, which was
not true while results were rewritten in place.

## Unreleased

## 0.3.0-rc1 — 2026-09-02

A release candidate, cut to put the whole tag ladder — including the
four-architecture matrix that S60 added after v0.2.1 — over a real tag
for the first time. Not 0.3.0: S31 (the rtpengine connector) and S36
(discovery without multicast) are deferred, and S52/S54 are blocked.

### Added
- **The S42 fault ladder**: partition, split brain, membership churn, a
  hand-over whose receiver never receives, the tombstone boundary, and a
  seed-replayable mastership simulator (`make check-fault`, plus
  `clustersim` in the ordinary gate).
- **`stats` reports each collection's `mode`** (store / proxy / shard).

### Fixed
- **The mastership step-down (clterm.h rule 3) is wired.** A master that
  observes a higher term now steps down; the term rides `MASTER_ALIVE`
  (35 → 39 bytes, length-gated) and outranks the member count, and a
  lower term never outranks us. Mixed-version fleets fall back to the
  address ranking until upgraded.
- `failovertest`'s round-robin spread leg had never run under ASan — it
  linked with a plain `cc` against a sanitized archive and the failure
  was swallowed as a skip.
- Size constants in `proto.c` are `size_t`, not `int` widened after the
  multiplication.

### Changed
- **The container fleet runs glibc.** The musl Containerfile is deleted:
  a build target kept beside a runtime is one somebody reaches for, and
  `containers-up.sh` had been reaching for it since the day it was
  written.

### Added
- **Metrics endpoint** (`GET /metrics`, OpenMetrics text) and a
  liveness probe (`GET /health`) on an optional `http = addr:port`
  listener. The metric names are a stable contract, pinned by
  `test/httptest.sh`. Guarded by `http_allow` (an off-box listener
  without one is refused at startup) and `http_timeout` (default 5s)
  so a half-finished request cannot hold a slot.
- **Memory pressure is visible before it bites**: `stats.memory` gained
  `tier`, `headroom_pct`, `nomem` and a `reclaim` block, and
  `reclaim_floor_mb` holds a floor under give-back.
- **`perfd_fetch()`** in the OpenSIPS driver: an asynchronous cache
  fetch, so a SIP worker is not blocked for the ~0.7–1 ms a cross-node
  pull costs.
- **MGET through `cache_raw_query()`** in the OpenSIPS driver: N keys
  in one round trip.
- **Status page** (`GET /`) and `GET /members`, served by **every**
  node rather than the master: a page the master serves is missing
  exactly when the master is in trouble, and two nodes disagreeing
  about the membership is how a partition becomes visible. Optional
  `[secrets] http` bearer token guards every route on the door.
- `sysusers.d` declaration, so the account `perfcached.service` runs as
  is actually created.
- `PRODUCTION.md`: the pre-deployment checklist.
- `contrib/haproxy-perfcached.cfg`: a reference front-end for clients
  that cannot learn a topology, replacing a redis-sentinel tier
  outright.

### Changed
- The `sync` barrier's reply now carries `dropped`. A ring-full drop
  discards a record that already holds a sequence number, so the
  barrier could truthfully answer `{"synced":true,"seq":N}` for an `N`
  that included records the WAL never carried (measured: 20 000
  writes, 1 223 dropped, 18 777 replayed after a crash). Callers can
  now tell a full barrier from one covering survivors only.
- Raw secrets are wiped from memory once the PSKs are derived, rather
  than living for the process lifetime.

### Fixed
- **An oversized write through a non-holder is refused, never forked.**
  A `SET` above the peer-forward ceiling (58 000 bytes) arriving at a
  node that does not hold the key could be neither forwarded nor
  refused, so the receiving node stored a divergent local copy: two
  nodes then answered the same `GET` differently. Released as 0.2.1.
- Give-back never actually returned memory on an mlock-pinned node: one
  refused `MADV_DONTNEED` latched the whole reclaim tick off, including
  the page phase that does not use it.

## [0.2.1] — 2026-09-01

Patch release over 0.2.0: the oversized-write fork window above, plus
test-suite hardening. Wire dialect and config digest unchanged, so
0.2.0 and 0.2.1 nodes interoperate and a rolling upgrade does not
split a fleet.

## [0.2.0] — 2026-09-01

The compatibility-promise baseline: daemon plus three clients (libperfd
C, pure-PHP, and any Redis client through the RESP2 door), four cluster
modes (store / eager / proxy / shard), WAL + RDB + recovery,
Noise-encrypted transport, and the Redis observability surface.

From this tag: the binary dialect's v1 is served indefinitely, peer
frames evolve additive-tail only, and incompatible fleets are refused
at join rather than joined wrongly.
