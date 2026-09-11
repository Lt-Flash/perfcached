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

## 0.3.0-rc34 — 2026-09-11

rc33's daemon, byte for byte.  Two test repairs, one of which is why
rc33 certifies nothing.

### Fixed
- **`splitkeeptest` asserted a snapshot, not a property.**  rc33 went red
  on both GitHub runs and green on GitLab and the build host, on the same
  binary.  The suite checked the table's state at the instant the fill
  ended - load factor under 16, leg under half the table - but how much
  overflows during a burst depends on how fast the client can push
  relative to the maintenance tick, and that is a property of the
  machine:

  | runner | fill rate | leg | load factor | verdict |
  |---|---|---|---|---|
  | build host, plain | 140,706/s | 37% | 5.5 | pass |
  | build host, sanitizer | 112,406/s | 39% | 5.7 | pass |
  | GitHub, plain | 190,391/s | 50.4% | 7.1 | fail by 0.4% |
  | GitHub, sanitizer | - | 83% | 35.7 | fail both |

  The faster runner overflowed more, which is the mechanism working.  It
  now asserts that the table CONVERGES on its target load factor after
  the burst: seconds for the time-sliced splitter, about nineteen minutes
  for the flat budget it replaced, so the two are separated by two orders
  of magnitude instead of by four tenths of a percent.  The leg is
  reported rather than asserted.
- **`fwdtest` ran `taskset` twice a run, from inside a comment.**  Every
  `check` and `check-asan` printed `taskset: bad usage` twice and nothing
  in the tree appeared to call it.  The suite writes each node's config
  from an unquoted heredoc - it must be, so the node number expands - and
  backticks inside one are command substitution, so a backticked command
  name in a CONFIG comment ran while the comment was expanded, once per
  node, and its empty output was substituted into the file.  Single
  quotes.  No assertion moved.

### Notes
- No daemon, wire, config or metric change over rc33.  The binary is
  byte-identical; only test scripts differ.

## 0.3.0-rc33 — 2026-09-11

One constant: sixteen times the overflow chains, for no extra memory.

### Changed
- **`PCACHE_OVF_BUCKETS` 1,024 -> 16,384.**  A key lives in its bucket
  OR in the overflow leg, never both, so any operation that misses the
  bucket must walk the chain for its hash under the leg's single lock -
  and that includes every store, not only operations on records that are
  actually in the leg.  A table whose leg grew during a write burst
  therefore taxes all of its subsequent writes.

  Profiled on a node applying replicas at about 118,000 records a
  second, the leg lookup was **17.0% of the applying thread**, the
  largest cost outside the cipher and ahead of the store itself at
  12.1%.  After the change, **2.92%**.

  It costs nothing.  The head array is one allocation of 8 bytes a head,
  carved as a region: 1,024 heads is 8 KB and 16,384 is 131,072, and
  both fit inside the single 256 KB region slot the smaller one already
  occupied.  Verified - the index sizing returns byte-identical figures
  at every size from 2^4 to 2^24.  32,768 heads would need a second
  slot, which is why the number stops there.

  **Throughput is unchanged** in the test that measured it: 116,974
  applied records a second against a mean of 118,271 before, inside the
  noise, because the receiver was not CPU-bound there and had headroom
  already.  What this buys is that headroom on the thread that is the
  fleet's write ceiling.  A rig that can saturate a receiver would be
  needed to turn it into a throughput figure.

### Notes
- No wire, config or metric change.  A node running this release
  interoperates with rc32 and rc31 in every mode.
- The same change is in the shared cachedb_perf core, pushed to its pull
  request.

## 0.3.0-rc32 — 2026-09-11

A table that could not keep up with a write burst, and never caught up.

### Fixed
- **The splitter's budget was a flat 128 splits per maintenance tick**,
  once a second, whatever the deficit.  A burst therefore outran it and
  it never recovered at any useful speed: measured, 600,000 records left
  a table at load factor 102 against its target of 4, needing about
  nineteen minutes of ticking, and the splitter was watched running at
  exactly its cap - 115 to 129 buckets a second - for the whole of it.
  Everything that would not fit six to a bucket went to the overflow leg,
  which is one chain per hash bucket under a single lock, and which never
  drains back into the table.

  A count was the wrong bound: what matters is how long the maintenance
  thread spends splitting, because the expiry sweep, the arena reclaim
  and the resize tick all queue behind it.  It now splits in chunks until
  the table has caught up or a 20 ms slice is spent.  A table that is not
  behind pays one call that splits nothing, as before.

  **Measured, four paired runs of the same 600,000-record fill on a fresh
  node each arm:**

  | budget | fill | write rate | load factor | in the leg |
  |---|---|---|---|---|
  | flat 128 | 20.0-22.1 s | 27,119-30,012 set/s | 102-109 | ~567,000 |
  | time-sliced | 4.9-8.1 s | 74,509-121,530 set/s | 4.2-5.4 | 150,000-173,000 |

  So 2.5x to 4.3x the write rate on this workload, because a right-sized
  table keeps inserts out of the overflow path and its single lock.

### Added
- `make check-asan-findings LOG=<file>` scans a sanitizer log for
  findings, excluding the broken-locks negative control - which always
  produces one, by design, so a plain grep of such a log always matches
  and says nothing.  Both check targets now bracket that step so the
  exclusion is mechanical.
- `test/splitkeeptest.sh`, which drives a deficit large enough to tell
  the two budgets apart.

### Notes
- No wire, config or metric change.  A node running this release
  interoperates with rc31 in every mode.

## 0.3.0-rc31 — 2026-09-10

A memory bound one verb could walk through, and a lagging node that
looked healthy.

### Fixed
- **The arena's ceiling was enforced on one of the two carve paths.**  A
  chunk carve that leaves the reservation tests `arena_mb` and refuses,
  which is what makes `cache full` reach a client; a REGION carve - the
  index a table is built from - fell through to `shm_malloc` with no test
  at all.  So records were bounded and the tables holding them were not.
  Reachable two ways: `create {buckets_log2: 24}` on a 2 GB node left the
  arena holding 2.6 GB, and ordinary traffic reached it through a table
  split, which is what failed rc30's sanitizer run on the public runner
  at 278,688 bytes past a 64 MB ceiling.  A whole index now prices itself
  and tests the ceiling BEFORE its first carve, since regions are never
  freed and a half-built table would leave its segments behind.
- **A table larger than the segment directory is refused rather than
  written past.**  `pcache_htable_new()` allocated
  `nbuckets / PCACHE_SEG_SIZE` segments into a fixed 4,096-entry array,
  so a `size_log2` of 25 wrote past it and 32 was not a defined shift.
  Latent - every caller was gated - and now guarded, with `PCACHE_NSEGS`
  derived from the ceiling so the two cannot drift.
- **`[collection] buckets_log2` parsed 1..24 where the verbs took
  4..24**, so the same value was legal in a config file and refused on
  the wire.  4 is the floor at both.
- **The held-bytes walk mis-timed itself.**  It subtracted the two
  timespec components apart and cast the nanosecond difference to
  unsigned, so about half of all walks reported roughly 1,271 seconds.
  No symptom while nobody read the figure, but the pacing reads it - the
  walk sleeps twenty times its own cost - so the interval pinned to its
  maximum whatever the walk really took.

### Added
- **Replica-lag figures**, because a node that cannot keep up with
  inbound copies still heartbeats, stays a member and answers its own
  client door quickly: `rx_applied` and `rx_applied_ps`, `rx_older_ps`,
  `rx_drops` and `rx_drops_ps`, `rx_queue` against `rx_rcvbuf`, in
  `stats.cluster`, as seven metrics, and as a `replica intake` card.
  Drops are per SOCKET (`SO_RXQ_OVFL`), not the host-wide counter a
  second daemon would pollute, and the queue depth comes from
  `SO_MEMINFO` - `FIONREAD` on a Linux UDP socket answers the size of
  the first datagram, not the queue.  The sample is taken by the thread
  answering the operator, not on a tick: both background threads were
  measured stalling under exactly the load these figures report.
- **The overflow leg and the walk's cost per collection** -
  `overflow` and `held_walk_us` in stats,
  `perfcached_collection_overflow` and
  `perfcached_collection_held_walk_seconds` in the metrics, and an "in
  leg" column that turns warn-coloured once the leg holds more than half
  the table.  Measured: the leg holds 89% of a table under sustained
  writes, at every size from 100k to 900k entries.
- **`create` and `resize` say what an index would cost** when the arena
  cannot hold it, naming what it needs, what is held and the ceiling.

### Changed
- The status page's rolling history keeps 5 minutes, not an hour.  An
  hour on a card a few hundred pixels wide drew three samples per pixel;
  the shorter window also falls inside the branch that draws minute
  ticks, which never ran before.
- README's sizing section carries the measured index table.  The cost is
  flat at 1.5 MB from 2^4 to 2^12 - segments are fixed at 4,096 buckets -
  and about 192 bytes a bucket above it, so 2^24 is 3.0 GB before a
  single record.  The old text said "log2 12 under 1 MB", which was
  wrong.
- README says plainly that an eager fleet under overload is eventually
  consistent, with the repair sweep as the repair.

## 0.3.0-rc30 — 2026-09-10

rc28's daemon, byte for byte, with the memory suites' arena-mapping
reader finished.

### Fixed
- rc29 stopped the reader measuring a neighbouring mapping but only
  tried runs beginning at a group's first mapping, so where an anonymous
  mapping sits immediately before the arena - which is every host but the
  development one - it found nothing and every residency assertion failed
  on MISSING.  It now tries every adjacent sub-run, and its logic is
  exercised against four synthetic layouts: the arena alone between
  guards, a neighbour immediately before it, a pinned reservation split
  into three pieces, and no such mapping at all.  Verified on the build
  host, where rc29 failed, plain and under the sanitizer.

## 0.3.0-rc29 — 2026-09-10

rc28's daemon, byte for byte.

### Fixed
- tailtest and growtest identified the arena in `/proc/<pid>/smaps` by
  taking the most resident anonymous mapping within a few hundred kB of
  its size, and a sanitizer build carries others in that band - so the
  reader sometimes measured a neighbour instead.  That is why the rc27
  and rc28 tag runs failed and passed alternately on identical commits.
  Both now take the adjacent run of mappings summing to exactly
  `arena_reserved`, which S97 publishes, and say so rather than guessing
  when nothing matches.

## 0.3.0-rc28 — 2026-09-10

### Fixed
- S97's lazy reservation could hand out a huge-page arena the pool never
  backed.  The mapping is `MAP_NORESERVE`, so the pool is not charged at
  mmap and the initial commit has to be secured; the kernel was asked to
  populate it and its answer believed.  Under emulation that answer is a
  lie - `madvise` reports success without backing a page - so the first
  write took SIGBUS at startup, which is how rc27's arm64 build died.
  Every huge page of the commit is now checked with `mincore`, and
  anything unverifiable counts as unbacked: the daemon falls back to the
  next page tier and says why.  Proven on the arm64 leg that failed.

## 0.3.0-rc27 — 2026-09-10

rc25's daemon, byte for byte.  Four test suites made honest on a slow,
unprivileged runner and two made runnable on a host that lacks an
optional tool - which is why the full sanitizer gate could not reach its
end here, and why the public runners were finding these instead of this
machine.  Library unchanged (0.2.7).

### Fixed
- probetest counted the overflow leg by subtracting an estimate of the
  last bucket's slots, and over-subtracted into a negative count when the
  table grew between two reads; S121 gave the leg its own cursor space, so
  it is now counted exactly, on a settled table.
- tailtest's bounded waits for the give-back go to 45 and 60 seconds, and
  its failures carry the released bytes and the give-back latch - a
  one-hertz maintenance duty under a sanitizer on two processors gets a
  fraction of the ticks it gets here.
- dumptest skips its compressed case where the `zstd` binary is absent,
  and noiseinterop skips where the host's python `cryptography` cannot
  drive the handshake, rather than reporting either as a failure.

## 0.3.0-rc26 — 2026-09-10

rc25's daemon, byte for byte, with two test suites made honest on a
slow, unprivileged runner.  Library unchanged (0.2.7).

### Fixed
- legwalktest asserted every record once while the maintenance thread was
  still splitting buckets, which is the walk's at-least-once contract
  behaving as documented; it now settles the table first, as dumptest
  does.  growtest asserted the arena mapping's resident bytes under a
  sanitizer that intercepts `mlock()`, reports success and populates
  nothing; the two residency checks now skip loudly when the kernel's
  `VmLck` says no pin happened while the daemon logged one.

## 0.3.0-rc25 — 2026-09-10

Collections that no longer need a config edit and a fleet restart, an
arena whose whole cap is one tier, and a cooperative walk that no longer
hands out its overflow leg in one piece.  Library unchanged (0.2.7).
Gate: the touched suites plain on the build host, then the tag pipeline.

### Added
- S69: collections created and dropped while the daemon serves.  `create
  {col, buckets_log2?}` and `drop {col, force?}` on the JSON door and in
  perfcli, gated by `[daemon] allow_create` (off by default, and it gates
  origination only, so a create still reaches every member and turning
  the gate on needs no fleet restart).  A created collection takes the
  cluster's mode.  The created set is persisted under `state_dir` and
  loaded before the replay and the snapshot import; the WAL carries the
  create and the drop as records of their own; a create or drop is
  announced to every live peer and the whole set is re-announced every
  few seconds, so a node that missed one converges.  A Lamport
  generation per name orders a create against a drop across the fleet.
- S69: a collection can be resized in either direction and renamed while
  it serves.  `resize {col, buckets_log2}` fills a second table at the new
  size from the live one on the maintenance thread, swaps the two in one
  step and collects the swap window's stragglers; deletes reach both
  tables while it runs, so a key deleted after the copier passed it is not
  resurrected.  `rename {col, to}` is one pointer swap, and the WAL
  carries it so a replay meets the old name before the rename and the new
  one after.  Both are announced to the fleet.  `perfcli resize`/`rename`.
- S69: the RESP door maps a database index to a collection name.
  `resp_collections = 0:sbcha, 1:th` makes `SELECT 0` reach `sbcha`; a
  bare name keeps its old meaning.  Until now a collection had to be
  NAMED `0` to be reachable, so a node holding live data reported one
  empty database to every Redis-native monitor - `DBSIZE` zero, `KEYS`
  nothing, `INFO keyspace` a flat line through an outage and a normal
  day alike.  `INFO keyspace` now reports every collection the door can
  reach with its real key count.  coltest.
- S97: growth by 2 MB group under an address-space reservation.  The
  arena reserves the cap (`arena_cap_mb`, else `arena_mb`) as address
  space charged for nothing, commits `arena_mb` at start, and commits
  the rest group by group as a carve reaches it - on the same tier,
  never as 4K pages - populated with the kernel's answer so an empty
  hugetlb pool is a refusal, not a SIGBUS; a punched group drops its
  commit, and a pinned arena gives back too (a run is unpinned before
  its punch).  `stats.memory.arena_committed` / `arena_reserved`,
  metrics `perfcached_arena_committed_bytes` /
  `perfcached_arena_reserved_bytes`.  growtest.

### Fixed
- S121: the cooperative walk budgets the overflow leg instead of draining
  it whole in its final call.  A table at load factor four keeps some
  percent of its records in the overflow chains (nine percent of a
  million), and `scan`, `keys`, the replication sweep's walk and the held
  walk all paid the whole leg in one step; the cursor space now has a
  position inside the leg and each call emits about `count` buckets'
  worth of records from it, whole chains at a time.  Fixed upstream
  first (cachedb_perf) and ported.  `dump`'s tail phase and its
  `tail_limit` are gone: the ordinary cursor carries the leg.
  legwalktest.

## 0.3.0-rc24 — 2026-09-09

rc23 plus the open-connection gauges and the stats reset, and two suites
made independent of the runner's privileges.  Library unchanged
(0.2.7).  Gate: the touched suites plain and under the sanitizer on the
build host, clang-tidy clean on the changed files, the tag pipeline.

### Added
- S123: the doors report what is OPEN now beside the running totals, and
  the totals can be reset.  `stats.native.<dialect>.open` and
  `stats.resp.open` (gauges, metrics `perfcached_connections_open` by
  door and dialect); `stats.since` says what the totals count from.  A
  reset - the JSON verb `reset_stats`, RESP `CONFIG RESETSTAT`, the
  page's "reset stats" button (`POST /reset-stats`, the one mutating
  HTTP route, body-less, token-guarded like the rest) or `perfcli
  reset-stats` - starts every running total again: doors and dialects,
  the collections' table counters (the core's own re-baseline), the
  store's size tallies, the cluster, proxy and WAL counters.  Gauges
  stand: open connections, entries, buckets, memory, sequence numbers,
  roles.  `/metrics` keeps the raw totals (Prometheus counters stay
  monotonic).  The page's door cards lead with "open" and label the
  totals "since start" or "since reset".  openstatstest.

### Fixed
- tailtest runs the daemon plainly when not root (an unprivileged runner
  cannot apply a bounding set, and cannot pin past RLIMIT_MEMLOCK
  either); stalebackfilltest waits for the restarted node's join line
  instead of reading the log once.  Both caught by the public runner
  after rc23.

## 0.3.0-rc23 — 2026-09-09

rc22 plus the loader (the reverse of rc22's dumper), and a fix in the
core's existence probe.  Library unchanged (0.2.7).  Gate: the whole
suite plain and under the sanitizer on the build host, the two tool
suites with leak detection on, clang-tidy clean on the new tool.

### Fixed
- S122: the existence probe reported every record in the overflow leg
  as absent - RESP `EXISTS` and `TYPE` answered nothing for a third of a
  2,000-record table in 256 buckets, the binary `exists` likewise, and
  the proxy-mode holder test on `set`/`del` took a key held in the leg
  for one to forward.  Fixed upstream first (cachedb_perf) and ported
  to the vendored core; probetest.

### Added
- S113: perfload, the loader, and the `restore` verb behind it.  A
  batch of records installed with their own version and absolute expiry
  through the write path (WAL, push), so a fleet of any size or mode
  takes a dump as if a client had written it; policies newer, skip,
  overwrite; expired-at-load skipped and counted; a key another node
  owns refused and named, never forwarded, and the loader re-sends it.
  The tool verifies every chunk first, streams N routed connections,
  resumes by a done file, maps collections, paces, dry-runs, and says
  when the members have caught up.  1M records in 0.8 s on 8 threads
  (1.4x plain pipelined SET); loadtest.
- S112: perfdump, a parallel dumper over the client door, and the `dump`
  verb behind it.  The verb yields a chunk of whole records (key, value,
  ttl, version) from a bucket-bounded key scan and one atomic fetch per
  key, so a chunk never repeats a record; `count` is buckets, `end`
  bounds a bucket range, a slot range splits a keyspace across shard
  nodes, the overflow leg comes out over bounded chunks, and the reply
  grows on the heap.  The tool walks N bucket ranges per collection into
  PCD1 chunk files with checksums and an atomically rewritten manifest,
  compresses through the zstd binary, paces with `--rate`, and
  `--inspect` verifies a dump; doc/perfdump-format.md.  Measured on the
  build host: 704,000 records a second on eight threads.

### Fixed
- walobstest asserted that the WAL fsync detector stays quiet on
  "unthrottled storage" - an assertion about the host, which a
  concurrent CI job made false; it now asserts the flag agrees with the
  figures it is computed from, in one snapshot.

## 0.3.0-rc22 — 2026-09-09

rc21 plus the six page and memory-accounting tasks filed from the
operator's evening on the fleet, and a test-harness sweep.  Library
unchanged (0.2.7).  Gate: the whole suite plain and under the sanitizer
on the build host (67 suites clean on each, the new tail suite
included), every task's suite watched fail first against the rc21
daemon before it passed on the fix.  On the fleet this brings each
node's resident memory down by about 70 MB: the never-carved part of
the reservation goes back to the host.

### Changed
- S117: the headroom a member advertises in its beat - what the fleet
  cards divide and what placement reads - is the ceiling's: `free_mb`
  and `total_mb` carry ceiling minus held, and the ceiling.  They used
  to be the slot pool's, which rose when records were freed and fell
  when the give-back returned their pages to the host, so a node that
  had just handed 84 MB back read as nearly full beside an arena card
  saying 60 %.  Same fields, same units; heldtest asserts each node is
  seen at the headroom it reports for itself.

### Added
- S120: a memory budget the operator can size from.  `stats` reports each
  collection's index regions (exact, noted at creation and growth) and
  its records as the cells they occupy (the walk rounds every record to
  its class), plus a `budget` block against the ceiling; `/metrics` the
  matching gauges; the page a budget card beside memory.  The README's
  new "Sizing" section is the same arithmetic done before the daemon
  exists: index per bucket, the cell ladder, the residue after a burst,
  the refuse-not-evict rule, and what the host sees.
- S115: the trend cards carry axes.  Y labels beside each plot, one per
  gridline, with percentages and counts anchored at zero; time ticks
  under the plot and a mark per minute on short windows; a peak dot
  with its time; a pointer readout with the value and time of the
  sample under the cursor.  A spike's height and duration read off the
  card.
- S116: the sizes column shows what is HELD.  The maintenance thread's
  walk bins every record's value length into the eight classes the
  column already draws and publishes them as `held_hist` beside
  `held_bytes`, so a member whose records all arrived by replication
  shows the same bars as the node that took the writes; the column used
  to count this node's own client writes and stayed blank on every other
  member.  `size_hist` keeps that meaning in `stats`.
- S118: `held` is exactly its parts.  `stats` gains `arena_class_chunks`
  (the chunks the size classes own - live records inside, free cells
  that belong to the class) and `arena_page_slack` (the alignment slot
  of every shm page); `/metrics` the matching gauges; the memory card
  the rows, so structure + class chunks + warm free (+ page slack)
  equals held on every refresh.  ceilingtest asserts the identity
  byte-exact at start, at the ceiling and after the give-back.

### Fixed
- S119: the reservation's never-carved tail is given back.  The arena is
  populated whole at start and the give-back walked only the groups
  below the chunk frontier, so a node that never needed its whole
  reservation kept the rest resident for the life of the process (the
  fleet: 70 MB beside 102 MB held).  The give-back tick now punches the
  groups between the two frontiers once, under its usual latch and
  cooloff; `reclaim.tail_released` says how much.  A new suite starts
  the daemon unpinned and asserts the mapping's resident size from the
  kernel, not a counter.
- Every suite killed its daemons by a pattern bound to the binary's
  name; a run against a renamed daemon outlived its suite and held the
  ports against the next runs.  Every suite now records the pids it
  starts and kills those, each checked against its command line first,
  with the pattern kept as a fallback.

## 0.3.0-rc21 — 2026-09-09

rc20 plus the remedy for the eager write ceiling rc20 measured, and a
test-harness repair.  Library unchanged (0.2.7).  Gate: the whole suite
plain and under the sanitizer on the build host (66 suites clean on
each), the failover suite twice more on its own, and the eager suite's
coalescing assertion watched fail first against the mutex it replaces.

### Changed
- S110: no shared lock on the eager write path.  The push group is per
  worker thread per peer, appended without a lock; the maintenance
  thread asks the workers to flush through a rate-limited broadcast
  instead of taking their groups from them.  On the build host at fifty
  clients the ceiling moved from 349,000 to 1,044,059 SET/s (3.0x; 2.0x
  the same run's Redis at 515,000), p50 2.5 ms, p99 8.8 ms; the futex
  calls per worker per drive fell from 150,000 to 4; one peer alone
  reaches 1,287,001.  The next ceiling is the receiver's single cluster
  thread at full load, filed and not built.

### Fixed
- failovertest's exit trap killed only its first fleet; an early exit
  left the routing fleet's daemons running and blocked the next job's
  busy guard for a quarter of an hour.  The trap covers both fleets and
  the member client.

### Measured, not changed
- Give-back inside a fixed reservation (no cap): a burst of 100,000
  records of four sizes (62.9 MB of values, 94 MB live once cell-rounded)
  took each node from 90 to 186 MB held; after the TTL the sweep cleared
  it within 15 s and the reclaim handed 88-90 MB per node back to the
  kernel, RSS 261 to 173 MB.  Thirteen such bursts in a row: the peak
  identical every time, the settled floor bounded at 101-103 MB, no
  write refused.

### Filed
- From the operator's evening on the fleet: the trend cards carry no
  axes (S115); `sizes` counts client writes on this node only, so a
  push-fed member shows nothing for 100k records it holds (S116); the
  member cards' headroom is the pool's free share and drops when the
  give-back returns memory (S117); the memory card's parts do not add up
  to `held` - the class-owned chunks have no row (S118); the never-carved
  tail of the reservation stays resident, so RSS does not follow `held`
  (S119); a memory budget the operator can size from (S120).

## 0.3.0-rc20 — 2026-09-08

rc19 plus the operator's three reports from the fleet and the first
measurement of the eager write ceiling.  Library unchanged (0.2.7).
Gate: the whole suite plain and under the sanitizer on the build host
(66 suites clean on each), the two new suites failing first against
rc19's daemon.

### Added
- S109: the page's collection size is a figure, not an estimate.  A walk
  in the maintenance thread sums every record's key and value bytes per
  collection, publishes the sum with its age, and paces itself at twenty
  times its own cost between 5 s and 60 s; `held_bytes` and `held_age_s`
  in `stats`, a `perfcached_collection_held_bytes` gauge, the column
  "held" on the page - real on a node whose records all arrived by
  replication, which used to show a dash.  The vendored table core is
  untouched (a first cut that counted inside it was reverted).
- S114: `held` says what it is made of - `arena_regions` (the index
  regions, carved at creation and on growth, never freed) and
  `arena_warm_free` (free slots kept resident) in `stats`, `/metrics`
  and the page's memory card.
- The daemon's threads are named (pc-w<n>, pc-cluster, pc-bulk, pc-beat,
  pc-maint, pc-wal, pc-rdb) so top, perf and a core say which is busy.

### Fixed
- S114: `at_ceiling` stayed true until a chunk carve succeeded, which a
  drained table never needs; the fleet showed "at ceiling" for ten hours
  beside half an arena of headroom.  The spell now ends at the give-back
  tick as soon as a carve would succeed.
- S111: the connection close line and the connections page counted RESP
  commands only; JSON and binary links closed with "0 requests" whatever
  they carried.  Every dialect counts now.

### Measured, not changed
- S110: the eager write ceiling at the README's load shape is 330-381k
  SET/s, latency-bound.  The receiver's cluster thread is at half; the
  sender spends 5.3 us of CPU per record against 2.55 standalone and
  meets a seal held under the per-peer group lock some 3,300 times a
  second.  The remedy - a per-peer push thread fed by per-worker rings -
  is filed, not built.

## 0.3.0-rc19 — 2026-09-07

rc18's batch with the two fixes its tag pipeline demanded.  No library
change (0.2.7); one two-byte fix in the RESP door and one test-harness
repair.  Gate: the touched suites plain and the whole suite under the
sanitizer on the build host (64 suites clean) before the tag - rc18 had
been tagged on a plain run alone, and the sanitizer job is exactly what
caught it.

### Fixed
- `JSON.DEBUG HELP` sent 55 bytes of a 53-byte literal - two bytes of
  whatever followed it in read-only data, silent in a plain build and
  an abort under the sanitizer, which took every later RESP reply with
  it.  Literal replies now take their length from the compiler.
- stalebackfilltest asserted that the sender had dropped its stale
  backfill flag at the instant the other survivor's drop appeared; each
  node drops on its own tick, and the runner's concurrent sanitizer job
  widened that gap.  The test polls for each survivor.

### Filed
- S112 perfdump and S113 perfload (a parallel dumper and loader over the
  client door, mydumper/myloader shape, with `dump` and `restore` verbs);
  S114 (`at_ceiling` stays set until a chunk carve, and the structural
  part of `held` is invisible).

## 0.3.0-rc18 — 2026-09-07 (red on its tag pipeline: a test race and a sanitizer over-read; superseded by rc19)

The batch since rc17: an empty joiner pulls its own bootstrap, the
write-path push coalesces, a client recovers members in the background
and is told when membership changes, and the RESP door speaks
RedisJSON.  libperfd 0.2.4 to 0.2.7.  Gate: the full suite on the build
host (64 suites, plain) and failovertest 51/51 twice; the OpenSIPS
module built from libperfd 0.2.7 runs on six test nodes.

### Added
- S83: a node that joins an eager fleet empty PULLS its bootstrap over
  the bulk plane from a ready peer (records first, lowest id) and is
  ready only when the stream ends; writes that land during the pull
  ride the write-path push; a founder with live peers pulls too.
  Counters `boot_out`, `boot_in`, `boot_failed`; boottest.
- S107: libperfd re-dials a member marked down in the background (1 s
  doubling to 30 s) while every request keeps flowing; every node tells
  its clients `joined` / `expelled` over the push channel, and the
  library dials the newcomer or drops the expelled node completely.
  `perfd_maintain()`, `perfd_recovered()`.
- S105: the write-path push coalesces into one datagram per peer (48 KB
  or 3 ms, whichever first) carrying the message the sweep has always
  sent, so an rc17 receiver reads it; eager SET at depth 64 went from
  289k/s to 349k/s.  Counter `repl_groups`.
- S86: `JSON.SET`, `JSON.GET`, `JSON.DEL`, `JSON.NUMINCRBY`,
  `JSON.ARRAPPEND` and `JSON.DEBUG` on the RESP door, with RedisJSON v2
  reply shapes, `NX`/`XX`, `WRONGTYPE` on a non-JSON value, and `EX` as
  an extension.
- S104: a failover probes a standby before adopting it and re-dials any
  learned member; SO_KEEPALIVE on every link; idle standbys are pinged
  and a dead one retired (libperfd 0.2.4, 0.2.5).
- S106: the identity and mastership-term files are text, versioned and
  checksummed; a damaged file refuses the start; legacy files migrate.
- S108: a joiner the master holds hears `JOIN_WAIT` instead of silence
  and does not found a fleet of its own.
- `make check` refuses to start if a suite in `test/` is not wired in.

### Fixed
- S108: the master read a joiner's identity two bytes off, so a copied
  identity was always admitted; and two claimants of a lapsed identity
  are told apart by the identity's last home.
- Found by S107's verification: a membership push reached only the
  clients of one worker (the RPC indexed the thread table by a worker's
  public idx, which is its slot plus one); a node restarted at the same
  address under a new identity was never announced; a call routed to a
  standby ran none of the handle's maintenance.
- The JSON value validator accepted any bare word as a primitive, so
  `JSON.SET` could store non-JSON; a primitive is `true`, `false`,
  `null` or a number.
- Test harnesses: waluniformtest polls instead of sleeping (red on
  GitHub's slower runner); eagertest's empty-restart leg and
  failovertest's sampling assertions assert properties, not moments.

### Changed
- The `members` reply carries the bootstrap and push-group counters.

### Open, filed
- S109: the page shows no size for a collection whose records arrived
  by replication.  S110: eager writes at depth 64 still run at a fifth
  of the store path.  S111: the connection close line counts RESP
  commands only.

## 0.3.0-rc17 — 2026-09-07

rc16's batch with one more test-harness repair.  No daemon or library
change; rc16's plain test job passed every suite, and the repaired
suite ran on the build host plain and under the sanitizer before the
tag.

### Fixed
- rc16's sanitized test job failed eagertest's empty-restart leg, which
  asserted a zero entry count on the restarted node immediately after
  start.  Nothing persists in that fleet, so the count is non-zero only
  once a peer's refill lands, and every pipeline run has that refill
  landing inside the first sample; the slower sanitized daemon lost the
  race.  The leg now asserts the node's own start kind, "cold", from
  its members entry, the property rather than the outcome.

## 0.3.0-rc16 — 2026-09-07 (red on that harness leg under the sanitizer; superseded by rc17)

rc15's batch with its test harness repaired.  No daemon or library
change; the first candidate since rc13 whose changed suite ran on the
build host before the tag.

### Fixed
- rc15 failed both test jobs on one assertion of the harness's own
  making: the failover test's spread leg expected the main fleet to
  keep two live nodes, and the standby-kill cue added in rc14 leaves it
  one, so round-robin put all twelve clients on the survivor.  The
  spread leg now runs against the fresh three-node fleet the routing
  leg starts, which starts before it.

## 0.3.0-rc15 — 2026-09-07 (red on that harness leg; superseded by rc16)

rc14's batch, compiled.

### Fixed
- rc14 failed its pipeline seven seconds in: the two writes of an error
  string into the 64-byte member reason field lacked the explicit bound
  that tells the compiler the truncation is meant, and the pipeline's
  `-Werror` rejected them.  The local pre-push check had been
  `-fsyntax-only`, which skips the optimiser and with it that whole
  warning class; it is a real compile now.

## 0.3.0-rc14 — 2026-09-07 (never compiled; superseded by rc15)

The client library learns to say what it found, and to notice a link that died while idle.

### Changed
- libperfd 0.2.4: TCP keepalive on every link (`keepalive_s`, 30 s idle,
  10 s interval, three probes) and an idle ping on standbys
  (`idle_ping_ms`, 30 s) that retires a dead one and marks its member
  down with the reason, so a failover never adopts a link that died
  while idle; every link carries the time of its last accepted reply.
  The first slice of S104.
- libperfd 0.2.3: `perfd_server_version()` and `perfd_member_state()`,
  so a client can say what it found without `ss` on the cache hosts;
  the `members` reply carries the daemon's `version` and `rev`.  S70.

## 0.3.0-rc13 — 2026-09-07

rc12's red, root-caused: the last "two senders" defect.

### Fixed
- A peer's record count and start kind are written only by its own
  heartbeat.  The master's keepalive publishes the master itself, and a
  JOIN or an ASSIGN list publishes the members they name, through the
  same path with no identity, zero entries and an unknown start kind,
  and the peer table stored the zeros.  So every non-master's view of
  the master flapped once a second between what its heartbeat said and
  nothing.  The backfill sender election reads that view; at the wrong
  phase it saw the lowest id as empty, skipped it, and the reader
  walked the backfill as well - two senders for one backfill in every
  CI run of eagertest and stalebackfilltest, never on the build host,
  where the two datagrams happened to land the other way round.  What
  a caller does not know it no longer writes.  This is also every
  "two senders" observation in the ledger since S82.

### Changed
- `members` reports, per member, this node's view of its record count
  and start kind, the values the sender election decides from, so a
  flap is visible to an operator rather than inferred from a second
  walker.
- stalebackfilltest probes that view twenty times and requires it
  steady.  A view lags the peer's own count by up to one heartbeat, so
  the probe waits a beat after the fill; the first cut did not and
  read the lag as a flap.

## 0.3.0-rc12 — 2026-09-06

Test-only over rc11; the daemon is byte-for-byte rc11.

### Fixed
- eagertest sampled its quiet window early.  The wait for "the sender's
  completion" grepped node 2's whole log for any `backfilled node …
  after its restart`, and node 2 already carried that line from node
  3's rejoin minutes earlier, so the wait was satisfied instantly and
  had been inert since rc9.  The window then opened when node 1's
  record count was reached, which a second sender's copies can satisfy
  while the first sender's cycle still has a slice to collect: 618 →
  766, one slice, on rc10 and rc11.  The daemon sent nothing after its
  real completion.  The window now opens on the restarted node's own
  completion, from a line written after its restart, allows one sweep
  period for a second sender, and requires every node's counter flat.
  Proven on the build host with the node logs kept.
- stalebackfilltest restarted its target while every node was still
  inside its 30 s cold window, which makes two senders legitimate; it
  now waits the window out, truncates the log on a restart, and awaits
  the sender's completion rather than asserting it.

### Filed
- S103: a second sender walked a backfill in rc11's CI run.  Tolerated
  by design during the holdoff; twice the traffic, not a fault; cause
  unknown without node logs.

## 0.3.0-rc11 — 2026-09-06

rc10's red, root-caused; and the arena's give-back made honest.

### Fixed
- A stale backfill flag fired when the ids reshuffled.  Every live node
  arms a backfill for a peer whose heartbeat says it started cold, but
  only the lowest live id walks it and only the walker clears the flag;
  on every other node it stayed armed.  When the lowest live id later
  left, the stale holder became the designated sender, pushed the whole
  keyspace at a node that had been full for minutes, and logged
  "backfilled node N after its restart" for a restart long past.  That
  was eagertest's 148, the same first 4 MB slice from bucket 0 on
  alternate tags, and the false completion line seen on the fleet.  A
  walk now starts only while its target still reports itself cold; one
  in flight runs to completion; a flag whose target is established is
  dropped with a notice; a reused peer slot no longer inherits its last
  occupant's flag.  Not covered, filed against S83: a sender dying after
  the target's cold window leaves it partial with nobody armed.
- A punched-out hugetlb group was re-committed by faulting.  On tier 1
  the punch had returned that page to the kernel pool, and with the
  pool empty the re-fault was a SIGBUS.  The group is now prefaulted
  with `MADV_POPULATE_WRITE` first (a kernel too old for it consults the
  pool's free count), and a refused re-commit is counted as
  `pool_empty` while the carve carries on to the 4K overflow.  A cold
  group also comes back whole, all eight slots.
- The give-back re-faulted what it had just released.  After a
  successful punch the code rebuilt the eight slot headers "cold",
  eight writes into the memory just given back: on hugetlb that took
  the whole 2 MB page straight back, in a `MADV_HUGEPAGE` region it
  asked for a THP.  `cold_bytes` said cold while RSS said resident.  A
  cold slot now has no header; the cold map is the only truth for it,
  and the header is rebuilt when the group is secured and re-committed.

### Changed
- Give-back is three phases: decide and mark under the arena lock,
  issue the syscalls with it dropped, book what happened under it
  again.  A group in flight sits on neither the warm nor the cold map,
  so a carve cannot take a slot the punch is about to zero.  Runs of
  consecutive groups go out in one `madvise`; `punch_calls` and
  `punch_groups` show the ratio.
- Give-back is bounded per tick by `[memory] shrink_step_mb`, groups
  and pages together; unset, an eighth of the ceiling and never below
  8 MB.  Reported as `reclaim.shrink_step_bytes`.
- The ceiling is a latched state: `stats.memory.at_ceiling` and
  `at_ceiling_since`, set on the first refusal and cleared by the next
  successful carve; the arena card turns red and says for how long.
- The overview card reads "members up 3/3", the fleet's view, instead
  of "peers up 2", this node's.
- `arena_profile` was never an accepted key here; configtest now pins
  its refusal so it cannot become accepted-and-ignored later.

## 0.3.0-rc10 — 2026-09-06

The fleet showed a defect no suite could: every test starts its nodes
into a cluster that already has a master.

### Fixed
- A founding node discarded the id its own identity proposes.
  `ident_proposed_id()` hashes the persisted identity into 1..1023, and
  a master honours that proposal for any joiner whose id is free - that
  is what carries an id across a restart, and what the identity file
  exists for.  `become_master()` never called the allocator: a node that
  found no cluster to join took id 1 and handed out ids from there.  The
  proposal was therefore honoured in exactly the case where a master
  already existed, and discarded in the case where the node was deciding
  for itself.  A whole-fleet restart renumbered the fleet around
  whichever node happened to boot first, and two partitions that each
  found would both claim 1 for two different identities, to be told
  apart on the merge.  **1 remains a legal id**: it is what an identity
  hashing to 1 is granted, and what a node with no durable identity
  still falls out of the allocator's scan as.  It was 1 being the
  default, not the value, that was wrong.  `test/statedirtest.sh` case 6
  covers the founding path, which the suite had never exercised.

### Changed
- The status page reports what durability is DOING rather than that it
  is configured.  `wal on` and `rdb on` named a feature and said nothing
  about whether it was keeping up: a WAL dropping records, overrunning
  its fsync budget or running out of free segments looked exactly like a
  healthy one.  Two cards now carry the counters - records appended and
  dropped, late syncs, overruns, free segments, unsynced records, the
  storage class, fsync now and the probe's percentiles for the WAL;
  snapshots taken, whether one is running, and the last one's age, size
  and duration for the RDB.
- The history graphs say what window they cover.  A series was plotted
  with no time axis at all, so a reader could see a trend without
  knowing whether it spanned five minutes or three hours.  One constant
  now drives the sample timer, each graph's footer and the heading hint,
  so the page cannot describe a cadence it is not keeping.
- The graphs are twice as tall and ruled at the quarters.  At 40px
  successive samples sat within a few pixels of one another: the trend
  was visible and the shape was not.

## 0.3.0-rc9 — 2026-09-06

rc8's gate found a real defect this time, in the rule S82 added.

### Fixed
- A backfill sender had to have restarted a while ago; it did not have
  to hold anything.  The S82 holdoff skips a candidate whose backfill is
  armed and whose fresh stamp is inside 30 s, but emptiness is a state
  and that is a timer: a node whose own fill outlasted the holdoff
  became eligible while still empty, took the role as the lowest id,
  handed over the little it had and cleared its flag - the failure the
  holdoff exists to prevent, one timeout later.  The election now skips
  a candidate reporting zero records, or reporting itself cold, at any
  age; both facts already ride the heartbeat, so there is no new state
  and no wire change.  A candidate holding part of the keyspace while
  still filling remains uncovered and is S83.


## 0.3.0-rc8 — 2026-09-05

rc7's own gate found a false alarm in the test suite rather than a
defect in the daemon; this is that fix, and it is the only change.

### Fixed
- `test/eagertest.sh` started its post-backfill quiet window when the
  restarted node's record COUNT reached the target, but the count is
  reached when the last record lands while the backfill flag clears only
  after a whole cycle has walked cleanly as the sender.  The remainder of
  the in-flight cycle - one 4 MB budget slice - fell inside the window
  and was read as an echo, failing rc5 and rc7 with an identical 342 ->
  490.  The window now opens when the sender reports the backfill
  complete, and the test fails if that report never comes.


## 0.3.0-rc7 — 2026-09-05

Browsing, visibility and identity: the things an operator reaches for
after connecting.  None of it changes the data path.


### Added
- `collections` verb and perfcli command; `keys` with no collection or a
  glob lists every collection the glob names, grouped, bounded across the
  whole result; a single-collection listing names its collection and says
  when it is one node's share of a placement-spread collection; `exit`
  and `quit` end a perfcli session.
- `stats.listeners` and a listeners card on the page: every configured
  door with its kind, address, port, plaintext or not, allow-list size and
  whether a secret is set.  A RESP client on the native door is named in
  the log, with the RESP door's address.
- The query log line says hit, miss, ok or err.
- `stats.cluster.reserved_ids`: a member that leaves keeps its id reserved
  for an hour; a returning identity, or a replacement at the same address,
  gets it back, and a newcomer is not handed it meanwhile.
- Per collection, what is being stored: `stored_bytes`, `stored_n` and a
  log2 `size_hist`, kept at store time from the write stream rather than by
  walking the table.  The collections table gains an estimate of memory
  held (entries x mean stored size, labelled as the estimate it is) and a
  size histogram.


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
