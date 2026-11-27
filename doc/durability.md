# Durability

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

In-memory only by default.  Add a `[wal]` section for write-ahead
logging + RDB snapshots; recovery replays snapshot then WAL tail at
startup:

    [wal]
    dir = /var/lib/perfcached
    fsync = everysec             # always | everysec | no
    save = 900 1                 # RDB snapshot rules, Redis-style,
    save = 300 10000             # repeatable and OR-ed

## The write-ahead log

**Choosing `fsync`.** The pump fsyncs once per drained *batch*, so with
`fsync = always` the per-writer ring has to absorb everything that
arrives while it sits in `fdatasync`.  On storage whose fdatasync takes
milliseconds the shipped 1 MB ring is not enough, and an overflowing
ring used to drop acknowledged writes *silently* - measured at up to
13% of a 20,000-key fill, present in the live table and absent after a
restart.  The probe now derives the depth from the measured p99 and the
daemon applies it (set `ring_kb` yourself to override; with `probe = no`
nothing derives it, so set it).  Since 0.7.3 a full ring under
`fsync = always` drops nothing: the writer waits for the pump, so a
slow `fdatasync` shows as write latency (`/stats` `wal.ring_waits`,
`ring_wait_ms`) - with the derived depth alone, a fast NVMe disk
still dropped 9% of a burst from the log.

**When the log cannot keep up (`on_full`).**  Since 0.7.4 a writer waits
under every policy (`[wal] on_full = wait`, the default).  When a
worker's ring is nearly full, a connection that writes is held - its
further requests wait, unread - while the worker goes on serving its
other connections, `/stats` and the dashboard's polls among them.  When
every segment still holds records no snapshot has covered, the log asks
for a snapshot and waits for it instead of overwriting one.  Nothing
acknowledged is lost and nothing is refused; the cost is write latency
while the log catches up (`/stats` `wal.writer_pauses`, `ring_waits`,
`full_waits`, `full_wait_ms`).  A wait for a snapshot gives up after 30 s
and overwrites, as below.  `segments_max` lets the log grow instead of
waiting: from `segments` (the floor, always kept) up to the cap, a
segment at a time, provisioned in the background when the log is half
full or its free segments would last under three snapshots, and given
back after five quiet minutes (`/stats` `wal.segments`, `segments_grown`,
`segments_retired`).  Measured with 4 x 1 MB segments and a 40,000 x 1 KB
fill: a fixed log took 2.8 s, waiting 2.7 s for snapshots; one allowed to
grow to 24 took 0.8 s and was back at 4 segments within a minute of the
load stopping.  With `on_full = heal` the node behaves as
0.7.3 did under `everysec` and `no`: a drop that still
happens is logged, and the node **heals**: the dropped record is missing from
the log but not from memory - every write reaches the table before the
WAL - so the node goes **HEALING**, refuses writes (no client selects it,
it stays in the map so nothing re-shards), keeps answering reads, takes
an unthrottled snapshot that makes every acknowledged write durable
again, and returns to READY by itself, usually in well under a second.
A segment overrun heals the same way.  Measured before this: 6,192
acknowledged writes of a burst missing after a restart; after it, none.
It goes **FAILED** instead - out of service until restarted - only when
it cannot heal: the device refused a write (fix the storage), or the
heal's own snapshot failed.  Frequent heals mean the rings, the storage
or the offered load need resizing (`perfcached_wal_heals_total`).
If the last snapshot took longer than the whole ring takes to fill at
the current rate (`perfcached_wal_ring_fill_seconds` below
`perfcached_rdb_last_duration_seconds`), no trigger can keep a snapshot
ahead of the WAL and the node will heal over and over; it logs that as a
sizing warning with both figures.
`everysec` does not stall the ring on a sync, but it does not make the
ring bottomless either: the WAL thread still writes every record, so a
write rate faster than it - or than the storage behind it - fills the
rings, and the writers wait (or, under `on_full = heal`, the node heals).  Measured with 1 KB records: the
WAL thread alone tops out near 180k records/s, and on a disk whose write-back held ~35 MB/s it fell behind at
~35k records/s.  Size for your storage, and watch
`perfcached_wal_dropped_total`.

## Replicas and cache collections

**What a replica does NOT give you.**  An eager replica, a spread
holder and a pulled `store` copy are all held in memory and off the
WAL - the node that took the write is the one that persists it.  That is
right while the fleet survives: a node that restarts is refilled by the
repair sweep from peers that stayed up.  It is NOT a second durable
copy, so if every node goes down together each one replays only what it
authored, and what makes the fleet whole again is the snapshot, which
does carry the copies.  `mode = eager` with `save = off` is therefore a
data-loss configuration and the daemon says so at startup.

**A cache collection.**  A collection that is pure cache - its
contents can always be rebuilt or fetched again - does not need the log
at all:

    [collection sessions]
    durable = no

Its writes, deletes and TTL changes are never appended to the WAL, it is
never written to a snapshot, and a restart starts it empty on that node:
records logged or snapshotted before it was made a cache are skipped,
and the start says how many.  In a fleet the peers' copies still reach a
restarted node the ordinary ways - the repair sweep, a pull on a miss -
so only a restart of every node at once leaves it empty everywhere.
`/stats` reports `"durable": false` for it, the members compare which
collections are caches (a member that differs is named in `/members`
`config_differs` as `[collection] durable = no`), and a reload refuses
the change - it takes a restart.

## Storage failures and snapshots

**When the device says no.** A WAL append, a segment rotation or an
`fdatasync` that the device refuses (EIO, ENOSPC...) fails the node the
same way, at the FIRST refusal, and the sync is never retried: after a
failed `fsync` Linux drops the dirty pages and reports the error once,
so a retry that succeeds proves nothing.  The durable watermark
(`wal.synced_seq`, `perfcached_wal_synced_seq`) stays where the device
last agreed to it, `sync` answers an error instead of `synced`, the
node's `state_reason` says the device refused, and
`perfcached_wal_storage_failed` goes to 1.  This holds with and without
a `[cluster]`.  A snapshot whose file or directory entry could not be
made durable is not published: `rdb.safe_marker` stays behind
`rdb.last_marker`, the WAL keeps every segment the old marker protects,
`rdb.save_errors` (`perfcached_rdb_save_errors_total`) counts it, and
the next snapshot is tried again after a back-off.

**Snapshots keeping up with the WAL.**  A snapshot frees only the WAL
segments below the point it started at, so under sustained writes it has
to start while the free segments still outlast it.  The WAL asks for one
itself - at a quarter free, and earlier when the fill rate says the free
segments will run out in less than twice the last snapshot's duration -
**also with `save = off`**: with a `[wal] dir`, "off" turns off the
timed rules, not these requests, because without them a busy ring
overruns and the node FAILS.  Alert on the same comparison:

    perfcached_wal_full_in_seconds >= 0
      and perfcached_wal_full_in_seconds < 2 * perfcached_rdb_last_duration_seconds

(`full_in_seconds` is -1 until the ring has rotated once, hence the first
line; `last_duration_seconds` is 0 until the first snapshot.)  Firing
does not mean data is at risk yet: it is the moment the WAL asks for the
next snapshot.  A rule that HOLDS (say `for: 5m`) means snapshots are
running back to back with no margin - measured in `snaptriggertest`, it
held for more than half of a load the node still survived, and the next
slower snapshot is the one that overruns.  Give the WAL more segments,
raise `rdb_mb_s`, or offer less load.  `perfcached_rdb_last_success_age_seconds`
(-1 before the first) and `perfcached_rdb_saves_total` say when snapshots
have stopped; `perfcached_rdb_in_progress` is 1 while one is written.

**Watching the WAL's room (0.7.2).**  The node raises that comparison
itself as the `wal.capacity` alert - a warning once it has held five
minutes - and after a minute when the whole log fills faster than one
snapshot takes.  The
status page's WAL card shows the log's shape and how fast it fills: the
free segments of all of them and their size ("8 free of 48 · 48 × 64 MB
= 3.00 GB"), the share no snapshot covers yet, the fill rate and how long
the free space lasts at it (amber under twice a snapshot), and how long
the whole log takes to fill beside how long a snapshot takes (red when
the log is faster).  `stats.wal` carries `segments`, `segment_mb`,
`size_mb`, `used_segments` and `used_pct`; `/metrics` adds
`perfcached_wal_segments`, `perfcached_wal_size_bytes`,
`perfcached_wal_free_segments` and `perfcached_wal_used_ratio`.

## Going back to an older build

**Going back to an older build.**  A build that finds state a NEWER
build wrote refuses to start and says so, rather than reading the newer
format as damage:

- a snapshot of a format past the one it reads (`-R` answers
  `INVALID (newer-format)`) - starting from the WAL alone would lose
  every replica the snapshot holds;
- a WAL record of a type it does not know, when no snapshot it can read
  already covers it - starting without it would drop a write;
- a `cluster-id` file whose header version is newer - starting as if it
  had none would join or found ANOTHER cluster.

The refusal leaves the file it refused as it was: run the newer build
again, or decide.  For the WAL that decision is `[wal] unknown_records = skip`,
which starts without those records and says how many it skipped; keep it
set for as long as the older build runs, since the records stay in their
segment until it is reused.  A newer record that lies under the loaded
snapshot's marker never refuses: the snapshot already holds what it did.
For the snapshot, move it aside; `-W` shows unknown records by type.

## Offline tools

`perfcached -P /var/lib/perfcached` probes the storage first (fsync
latency, sustained rate) and prints the policy it would recommend;
`-I` prints the storage identity chain (NVMe/SAS/network/LVM...),
`-W`/`-R` inspect WAL segments and snapshots offline.  The `sync` and
`load` admin verbs give you an fsync barrier and additive snapshot
import at runtime.
