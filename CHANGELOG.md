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

## 0.3.0-rc6 — 2026-09-05

The release rc5 should have been.  rc5's tag pipeline found a real
defect under load - one late acknowledgement made a backfill re-push the
whole keyspace - and this fixes it, together with the batch that
followed rc5: perfcli that names the mistake, a daemon that says who
connected, a heartbeat that carries the HTTP door and uptime, a RESP door
that tells cluster-aware clients the truth and serves the slot map from
a cache, a fleet that is uniform about the WAL, and the licence
boundary checked on every push with the export carrying its notices.

### Fixed
- A late acknowledgement during a backfill or sweep dirtied the whole
  cycle, and the next cycle re-walked the entire keyspace with the
  passive copies included - the echo the passive rule exists to prevent,
  arriving by another door.  A batch whose ack is 1.5 s overdue is now
  re-sent, up to twice, from the window's own bookkeeping; a loss is a
  peer that did not answer three times in five seconds.  `migrate_retx`
  counts re-sends beside `migrate_lost`.
- `INFO server` said `redis_mode:standalone` and `INFO cluster` did not
  exist while `CLUSTER INFO` said the cluster was enabled, so no
  cluster-aware client ever fetched the slot map.  Both now derive from
  the one function `CLUSTER INFO` uses; a clusterless node still says
  standalone.

### Added
- `stats.native` and a card per dialect were rc5; this adds the
  connection log: one NOTICE when a client's dialect settles (binary,
  JSON, RESP, or the RESP door; encrypted or plaintext) and one when it
  closes, with the reason - peer closed, quit, read or write error,
  protocol error, or a handshake that never happened for want of a
  secret.  Twenty lines per ten seconds, then a count.  HTTP polls are
  never logged.
- The heartbeat carries the node's HTTP door and uptime (an additive
  tail, 64 -> 70 bytes); `members` reports `http` and `uptime_s` per
  member and the fleet grid links each node through its own door.
- `CLUSTER SLOTS` and `CLUSTER SHARDS` are served from a cache keyed by
  the membership snapshot and rebuilt only when it changes;
  `stats.resp` gains `slots_hits` and `slots_builds`.
- `lib/NOTICE` names libsodium under the ISC License with its text
  reproduced; the libperfd export now carries `LICENSE` and `NOTICE`
  beside the code, and `synctest` runs on every push, not only on tags.
  `CONTRIBUTING.md` and `lib/README.md` state the boundary rule.

### Changed
- A fleet is uniform about the WAL: a node that logs cannot join a fleet
  that does not, and the reverse, refused at the join with the posture
  named.  Folded into the config digest only when a node logs, so a fleet
  without a WAL keeps the digest it has and a rolling upgrade across this
  change does not split it.  `fsync` policy stays per node.
- perfcli: `-h` with nothing after it is help; an unknown option is
  refused by name; the connect error names the target; with no target
  given the local daemon's config supplies the address and client
  secret; the banner waits for a successful first request, and a drop
  there is diagnosed (no secret, wrong secret, or a plaintext listener);
  an option after the command is called out as the command's argument.

## 0.3.0-rc5 — 2026-09-05

A delivery-and-visibility release inside the 0.3.0 line, and the fix
for what turned the rc4 tag red.  Eager mode now delivers on the write
instead of on a timer; the decision to refill a restarted node moved
from a guess to a fact the node states itself; an operator can finally
watch requests and see what the native door is carrying; and the public
library's export tree no longer crosses a licence boundary.

### Added
- `query_log = off | all | sampled:N` (`[daemon]`): one line per request
  through `LM_INFO` for every dialect on the native door - verb,
  collection, key, hit or miss, latency, client.  Off by default and one
  predictable branch when off; `sampled:N` stamps every line `sample=1/N`
  so a sample cannot be read as the whole; `query_log_keys = no | hashed
  | full` keeps keys out of the journal or prints eight hex digits of a
  hash.  Turning it on below `log_level = info` warns at startup.
- `stats.native`: per-dialect connection and request counters for the
  native door (`json`, `binary`, `resp`), and a card per dialect on the
  built-in page beside the RESP door's own.

### Changed
- Eager mode replicates on the write: a write pushes one group to every
  live peer from the write path, fire-and-forget, with no TTL threshold -
  a 5 s key reaches its replicas as surely as a 5-day one.  The sweep
  every 10 beats is repair behind it, not delivery, and its "dying soon"
  skip is derived from its own cadence.  Counters `repl_pushed`,
  `repl_skipped_dying`; a replication card on the page.
- A client outside `http_allow` gets `HTTP/1.0 403 Forbidden` with a body
  that names the setting, instead of a silent close that curl reports as
  a protocol error and an operator reads as a dead listener.
- The ALIVE heartbeat grows one additive tail byte saying how the node
  started - cold, recovered from a WAL, or established.  A build before
  the byte reads its prefix and is judged as before.
- libperfd 0.2.2: `pc_noise.h` in the exported tree no longer includes a
  GPL header; `tools/sync-libperfd.sh` resolves every include against
  the export set and refuses a licence crossing.

### Fixed
- A restarted node could be left without the fleet's records: the
  backfill was armed on "holds zero records" in the first heartbeat a
  peer read, and a peer's steady sweep could land a couple of records
  before that heartbeat, making an empty node look like a recovered one
  (`test/eagertest.sh`: a 480-record refill stalled at 2).  The node now
  says how it started, and a peer arms the backfill from that - for a
  restart it witnessed, or once it is itself past its own start window.
- The rc4 tag's matrix run failed on i386 and arm32: `statfs.f_type` is
  a plain `int` there and the tmpfs/ramfs compare was signed against
  unsigned under `-Werror`.  The compare is now unsigned on both sides.

### Closed without a change
- "The dashboard starts silently": it does not - config.c logs every HTTP
  listener at NOTICE with its allow-list size and token state.  What was
  silent was the wire (the 403 above).

## 0.3.0-rc4 — 2026-09-05

A fix-and-observability release inside the 0.3.0 line.  Three defects
in how a node comes back into an eager cluster, all found on a real
fleet during rolling restarts, each with a test that fails against the
previous build; the eager mode named as the mode it is; and a stats
document and built-in page that answer the questions an operator
actually has.

### Fixed
- A node that restarted empty could be left without the fleet's records
  for good.  The backfill's designated sender could change hands after
  an ordinary sweep had advanced its mark, and the backfill then
  discarded every passive copy on `wtick <= since`, walked "clean" and
  logged itself complete.  A backfill now walks from zero and only a
  whole cycle as the sender may clear it (`test/backfilltest.sh`,
  phase 1).
- A node that arrived empty could be elected backfill sender for the
  next one and hand it nothing, its own fill landing a moment later as
  copies that are never re-sent.  A peer that arrived empty within
  three sweeps is not a sender candidate (`backfilltest.sh`, phase 2).
- A rejoining node did not keep its id, because identity and the
  mastership term persisted only in the WAL directory and a pure cache
  has none.  `[daemon] state_dir` names where they live; the daemon
  creates it if the parent exists, refuses an unusable one, and accepts
  tmpfs with a warning.  A WAL-only configuration is unchanged;
  introducing `state_dir` beside a WAL carries the existing identity
  and term across once (`test/statedirtest.sh`).
- libperfd (0.2.1): a failed round trip left the pipeline counters set,
  so every later call on the handle failed with `pipeline desync` until
  the process restarted.  The counters are dropped on every failure
  path (`test/wedgetest.c`).  `perfcli` redials once when its handle has
  failed.
- `stats` published `arena_total`/`arena_used`/`arena_free` as zeros on
  a node whose huge-page arena was not the backing in use, which read
  as an arena with nothing left.  They are `null` there, with
  `arena_capacity_valid` beside them.
- The eager-mode bench cells were quantised by `redis-benchmark`'s
  millisecond clock; requests now scale with pipeline depth.

### Changed
- `mode = store | eager | proxy | shard`.  The `eager` key is retired:
  eager was only ever legal beside store, so it is the mode, not a flag
  on one.  The old key is a hard error naming the replacement.  Internal
  state and the wire are unchanged, so a rolling upgrade interoperates.
- `stats.cluster.mode` and each collection's `mode` report `eager` when
  eager is on; `/members` gains `routing.eager` and `routing.mode_name`
  beside the unchanged `routing.mode`.

### Added
- `stats.process`: uptime, pid, threads, cumulative CPU and RSS.  CPU is
  cumulative, never a rate - only the caller knows its polling window.
- `stats.cluster.state_dir` and `state_on_tmpfs`.
- `/stats` on the HTTP door serves the stats document; the `stats` verb
  takes an optional `col`.
- The built-in page is a fleet view: members as a grid legible to 64
  nodes, tinted and labelled by state, a selector to any member,
  collections, cluster-plane cards filed by what they measure, memory
  with live/held/free/ceiling/RSS, and rates differenced from the
  previous sample rather than lifetime ratios.
- Release tarballs per distribution and a Debian package (`debian/`).

## 0.3.0-rc3 — 2026-09-03

rc2 with one test defect fixed. **No product change**: the daemon in
rc3 is byte-for-byte the daemon in rc2 apart from its version string.

rc2's tag run was red, and a red tag certifies nothing — which was the
whole reason rc2 was cut over rc1. `walobstest` asserted that at least
five fsyncs had happened after seventy round-trip writes, and that
number is a property of the *device*, not of the daemon: the pump
group-commits, so on millisecond-class storage each round trip becomes
its own batch while on a microsecond-class CI runner seventy writes
drain as two. The same snapshot passed on one GitHub runner and failed
on another nine seconds apart.

### Fixed
- `walobstest` counts fsyncs it forces with `sync` barriers rather than
  fsyncs that ordinary writes happen to produce, so the count no longer
  depends on how fast the storage is. `PCOBS_TMP` points the fixture at
  a chosen filesystem — with `/dev/shm` the microsecond-class runner
  reproduces locally, which is how the fix was verified.

## 0.3.0-rc2 — 2026-09-03

Cut because rc1's tag had gone 51 commits stale: its CI is red, it
predates the durability fixes below, and every binary since has been
stamped `0.3.0-rc1` while being something materially different. A tag
that certifies nothing is worse than no tag.

Durability is the theme. Two defects here destroyed acknowledged writes
without saying so, and a third would take the host's filesystem down
with a refused start.

### Added
- **A node leaves service when it destroys an acknowledged write.** The
  first dropped ring record marks the node `FAILED`: it stays a member
  and keeps answering reads, but refuses writes and is not selected for
  new work, and in shard mode its keys move to the survivors. A segment
  overrun now takes the same path — the two are one event in different
  costumes. `FAILED` is terminal until an operator restarts the node,
  because recovery would re-shard the fleet a second time.
- **`wal/CONTROL`**, a durable record of the WAL's sequence and each
  segment's span — the missing half of the recycling rule.
- **The daemon measures its own fsyncs** and says once, with both
  numbers, when they cost 4x what the startup probe predicted. `stats`
  gains `wal.observed`.
- **A pre-flight space check**: a WAL that will not fit is refused with
  both sizes and the knobs to change, instead of discovering it by
  filling the filesystem.
- Nightly valgrind over the unit binaries (`tools/nightly-valgrind.sh`,
  schedule-only in the GitLab pipeline). First run 12/12 clean.

### Fixed
- **The WAL sequence survives a restart.** It restarted from zero, so
  the post-recovery checkpoint stamped `wal marker 0` — measured in 11
  of 12 snapshots — and every restart replayed the entire WAL because
  nothing is ever below a marker of zero.
- **The recycling rule is enforced after a restart.** Its per-segment
  spans lived only in RAM, so `seg_hot()` called every segment cold and
  the rule documented in DESIGN section 7 went unenforced: `overruns`
  read 0 while records were overwritten. The restart path also claimed
  its next segment without asking.
- **A failed WAL provision leaves nothing behind.** The partial segment
  used to survive, so a refused start left the volume full after the
  daemon exited and a supervisor's retry re-filled whatever had been
  freed.
- The mastership step-down's equal-term "collision" was a simulator
  artefact, not a defect; the retraction is recorded rather than the
  claim.

### Changed
- **The storage probe reports a burst ceiling, not a sustained rate.**
  It writes at most 16 MB in ~1.2s, so on any host with a write-back
  disk cache — every VM running `cache=writeback` — it measures the
  cache. Measured on Ceph: the probe said SSD-class, 139us p50 and
  ~3467 sustainable synced writes/s where `fio` in the WAL's own shape
  saw 3.7ms and 236/s, and it ranked btrfs above ext4 when ext4 is 1.6x
  faster under durable load. The number now states the volume behind it
  and that it is an upper bound.
- **The per-device probe cache is gone.** It was keyed by a device id
  that a hypervisor-side storage migration does not change, so it
  served numbers for storage that no longer existed. `auto` and
  `always` both measure; `probe = no` still costs nothing.
- `PRODUCTION.md` gains two measured items: verify with fio (or set
  `cache = none`) before enabling `fsync = always`, and put the WAL on
  ext4 rather than btrfs.

### Known
- `check` is intermittent on GitHub-hosted runners (S62), unresolved
  and now parked. Five storages were eliminated before the runner
  measured its own WAL directory at **p50 2us** — memory-backed, and
  roughly 1800x faster than our Ceph — which means every hypothesis
  tested had assumed the wrong premise. The same commits pass on the
  GitLab runner and on the build host.

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
