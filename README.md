# perfcached

Multithreaded cache daemon for Linux — **one node or a self-clustering
fleet**, same binary either way — plus the `libperfd` C client
library.  Built from the OpenSIPS `cachedb_perf`
module's proven core, but as its own process rather than a module, so
anything that speaks TCP can use it.  Speaks an encrypted
(Noise/libsodium) triple dialect — binary frames for libraries,
newline-delimited JSON-RPC for scripts, and RESP2 so unmodified Redis
clients (redis-cli, hiredis apps, rtpengine) connect as if it were
Redis.  Clusters by multicast discovery with a master that owns a
versioned map; placement is weighted rendezvous hashing in integers, so
every node and every client reaches the same answer bit-for-bit.

**Status.**  The last tagged release is **0.3.7** (2026-09-15); the
**0.4.0** line is at release candidate 19 (2026-09-21), which is what
the test fleet runs.  Candidates 10, 13 and 14 of that line are RED -
their pipelines failed `tailtest`, `growtest` and `pintest`, each a
suite that runs only on a tag - and 11 and 12 are superseded; the
CHANGELOG says so against each.  Start from rc19, a capability step: **Redis pub/sub on every door and
across the fleet** - `SUBSCRIBE`, `PSUBSCRIBE`, `PUBLISH` and `PUBSUB`
with Redis's rules and frames, the same five verbs on the native doors
and in libperfd 0.2.10, a publish on any node reaching the subscribers
on every node over a relay port with receive threads of its own
(optionally only to the nodes that want it), pushes over UDP for native
subscribers, and Redis keyspace notifications (`notify_events`) from
the node that applies a write.  It fixes rc1's pattern matcher, which
a hostile pattern could stall or push past its buffer, and rc3's crash
of a standalone daemon with no cluster secret; delivery is written once
per worker turn, and a publisher that outruns a worker is paused rather
than dropped - on one host one worker delivers at Redis's per-core rate
([bench/RESULTS.md](bench/RESULTS.md)).  rc5 counts each entry once, not once
per copy, in the fleet-wide collection figures on `/stats` and the page;
rc6 lists the commands clients sent that no door implements, fleet-wide,
implements `CLIENT SETINFO`, and keeps the RESP password out of the slow
log.  It carries the 0.3.8 candidates' four
fixes as well: a spread node that comes back empty is handed its share
rather than the whole keyspace (S156), a fleet whose live members fall
below `replicas` says so (S157), a standalone daemon runs a fleet's
collection stanza unchanged (S158), and `max_clients` refuses with a
reason instead of failing `accept` in silence (S161).  What the 0.3.7
line added, in order: `spread` mode (K copies of P, routed by libperfd
0.2.8); DDL behind a privileged connection (`enable`, a second secret);
per-key event logging; hit counters that tell a client from a peer, and
`reach`; the durability card's `staged` / `unsynced` split; an index
that sizes itself both ways (grows at `grow_at_pct`, shrinks at
`shrink_at_pct`, the old index handed back to the arena once its
readers have moved on) and carves what it uses; and two fixes a running
fleet found and no suite did - a returning node never re-placed to, and
an empty node's "no" deleting the fleet's only copy.  The full list is
[CHANGELOG.md](CHANGELOG.md).  `spread` has not carried production
traffic; the staging fleet runs `eager`.  The over-holding a
spread node showed after a cold start in 0.3.7's tables (S156) is fixed
in the 0.3.8 line.

The OpenSIPS driver module (`cachedb_perfd`, thin glue over libperfd)
lives in the OpenSIPS tree.

Dependencies: libc, pthreads, libsodium.  Linux only (x86_64 / arm64 /
arm32 / i386 — `tools/matrix.sh` builds and tests all four via podman +
qemu-user).

## Start here

    make                                  # daemon, perfcli, perfdump, perfload, libperfd.a
    install -d -m 750 /etc/perfcached
    cp contrib/perfcached.conf.example /etc/perfcached/perfcached.conf   # set the two secrets
    perfcached -f /etc/perfcached/perfcached.conf -C    # validate, and report what it would run
    perfcached -f /etc/perfcached/perfcached.conf -D    # run in the foreground
    perfcli -p 6479 -a '<the client secret>' ping

Redis clients reach it through a `resp =` listener (configuration 02 in
[contrib/CONFIGURATIONS.md](contrib/CONFIGURATIONS.md)); what they get
is [REDIS-COMMANDS.md](REDIS-COMMANDS.md).  A fleet is the same file on
every node plus a `[cluster]` section - [A cluster](#a-cluster) below,
and the configurations page has one per mode.

## Contents

This page: [Compatibility promise](#compatibility-promise) ·
[Build and test](#build-and-test) ·
[Running it](#running-it) (a node, a cluster, the modes, durability,
sizing, example configurations) ·
[Operating it](#operating-it) ·
[Tools](#tools) (perfcli, perfdump, perfload) ·
[Clients](#clients) (libperfd, cachedb_perfd) ·
[Measured](#measured) · [Layout](#layout) · [Licensing](#licensing).

Other pages:

- [REDIS-COMMANDS.md](REDIS-COMMANDS.md) — every Redis command the
  door answers, its forms, replies and differences; what is not
  supported and what a client gets.
- [PERFCLI-COMMANDS.md](PERFCLI-COMMANDS.md) — every perfcli command,
  its arguments, its reply shape, and where the word form is thinner
  than the JSON-RPC method under it.
- [contrib/CONFIGURATIONS.md](contrib/CONFIGURATIONS.md) — ten complete
  configurations, single nodes and fleets in every mode, each validated
  with `perfcached -C`; [contrib/perfcached.conf.example](contrib/perfcached.conf.example)
  is every option, annotated.
- [bench/RESULTS.md](bench/RESULTS.md) — how every measured table was
  produced, how to reproduce it, and the readings that changed.
- [PRODUCTION.md](PRODUCTION.md) — running it in production.
- [lib/README.md](lib/README.md) — libperfd, the C client, on its own.
- [doc/perfdump-format.md](doc/perfdump-format.md) — the dump format.
- [CHANGELOG.md](CHANGELOG.md) — what each release changed, and the
  measured numbers that moved with it.
- [CONTRIBUTING.md](CONTRIBUTING.md).

## Compatibility promise

Anything that vendors a client (the OpenSIPS `cachedb_perfd` module
does) or packages this daemon can rely on:

- **The binary dialect is versioned and v1 is served indefinitely.**
  Every frame carries the version byte it has carried since day one; a
  future dialect is an addition, never a replacement.
- **Peer-plane frames evolve additive-tail only.**  A peer built
  before a field reads the prefix it knows and ignores the rest - the
  ALIVE frame has already grown five fields this way.  Changes that
  cannot be additive bump the route algorithm version, and a
  mismatched peer is REFUSED at join rather than joined wrongly.
- **Mixed-build fleets are refused, not corrupted.**  The cluster
  config digest and route-algorithm version make an incompatible
  upgrade an explicit, loud event - a rolling upgrade that would split
  placement is refused by the joining side.
- **RedisJSON on the door.**  `JSON.SET key path value [NX|XX] [EX
  seconds]`, `JSON.GET key [path]`, `JSON.DEL key [path]`,
  `JSON.NUMINCRBY`, `JSON.ARRAPPEND` and the `JSON.DEBUG HELP` probe
  OpenSIPS's `cachedb_redis` sends at connect, over the native JSON path
  operations.  Paths are `$`, `.name` and `[index]`; a `$` path answers
  RedisJSON v2 style (an array of matches), a `.path` the bare value.
  `EX` is an extension - the store sets a TTL in the same call - and
  without it a field update preserves the key's expiry.
- **RESP2 is RESP2.**  The Redis-client door tracks the de-facto
  standard, not this project's whims.  The command set, form by form,
  is [REDIS-COMMANDS.md](REDIS-COMMANDS.md).

Tagged releases (`v0.2.0` up) are the states these promises are made
from; master between tags is development.

## Build and test

    make check       # every suite + the broken-locks canary
    make check-asan  # the same suite under ASan+UBSan
    make install     # daemon + perfcli + example config + systemd unit
    tools/matrix.sh  # the four-arch matrix (build host with podman)

CI runs the same spellings on every push, lint first: a gating
`clang-tidy` stage at a zero baseline, then the full suite natively and
again under ASan+UBSan.  A red lint stage stops the pipeline in minutes
instead of after the hour of suites.

## Running it

Every option lives, annotated, in
[contrib/perfcached.conf.example](contrib/perfcached.conf.example);
the snippets below are complete working configs, and
[contrib/CONFIGURATIONS.md](contrib/CONFIGURATIONS.md) has ten whole
files - single nodes and fleets in every mode - each validated with
`perfcached -C`.

### A single node

    # /etc/perfcached/perfcached.conf  (chmod 640)
    [daemon]
    workers = 4

**How many workers.** Set it to the core count. Measured on a 16-core
host: throughput scales close to linearly to 8 workers (6.2x a single
worker), then flattens as the daemon reaches ~14 of 16 cores.
Oversubscribing is harmless rather than helpful - 32 and 64 workers on
16 cores buy 2-6% over 16 and cost ~0.3 MB of RSS each, with no
scheduler thrash. No penalty for guessing high; a real one for low.

    [memory]
    arena_mb = 1024

    [secrets]
    client = pick-a-client-password
    cluster = pick-a-DIFFERENT-cluster-password

    [listen]
    tcp = 0.0.0.0:6479

    [collection sessions]
    buckets_log2 = 18

Validate, then run (`-D` = foreground; omit it to daemonize, or use
the systemd unit):

    perfcached -f /etc/perfcached/perfcached.conf -C   # validate + report
    perfcached -f /etc/perfcached/perfcached.conf -D
    perfcli -p 6479 -a 'pick-a-client-password' ping

For production, install the unit and start it the systemd way:

    cp contrib/perfcached.service /etc/systemd/system/
    systemctl daemon-reload && systemctl enable --now perfcached

The two secrets MUST differ - clients hold the client secret, only
daemons hold the cluster secret, and the daemon refuses to start when
they are equal.  Add a second `client = ...` line to rotate client
passwords with zero downtime (new connections try each in turn).

### A cluster

Membership is automatic, in the clusterer_controller style: there are
no node ids and no peer lists to configure.  Add the same `[cluster]`
section - same multicast group, same cluster secret - on every node
and start them in any order; they discover each other over multicast,
elect a master, and the master assigns node ids at join time.  The
SAME config file works on every node (set `advertise` only on
multi-homed hosts, to pin which address peers should use).

**The cluster owns the collection config.** Declaring `collections`
makes this description authoritative: ONE mode for the whole cluster,
over an exhaustive set of collections.  A peer whose config differs is
refused at join, loudly and by name, rather than silently exchanging
data it will misinterpret - two nodes running one collection as store
and another as shard lost every write between them, in silence, before
this existed.

    [cluster]
    multicast = 239.68.68.1:6480
    mode = eager                 # ONE mode, cluster-wide
    collections = sessions       # the exhaustive clustered set
    #advertise = 10.0.0.1        # only on multi-homed hosts

    [collection sessions]
    buckets_log2 = 18            # node-local SIZING only

**Two UDP ports between nodes.**  The cluster port (the multicast
group's port, used for unicast too) carries membership, replication and
pulls; the pub/sub relay port, by default the cluster port + 1, carries
relayed publishes on threads of its own so a publish storm cannot starve
the heartbeat socket.  Open both in a firewall between nodes.  A node
that cannot reach a peer's relay port sends it relays on the cluster
port instead and says so in its log; `pubsub_rx_threads = 0` gives up
the relay port altogether.

Nodes that die and come back rejoin by themselves; a partitioned
master steps down when it sees a bigger fleet.  Failure detection
runs on 1 Hz heartbeats with real margin - a master is presumed dead
after 8 s of silence, a peer after 10, so jitter is not death - beat
emission is watchdog-backed, and datagram ingest is fairness-bounded
so a migration burst cannot deafen membership.  Watch it settle:

    perfcli -p 6479 -a '...' -P stats
    # "cluster": { "node": 1, "role": "master", "peers_up": 2, ... }

**One cluster is one mode.** Mixing modes inside a single cluster is
rejected by design, not deferred: one membership whose members mean
different things per collection cannot be reasoned about during an
incident - the same node loss is "replicated, fine" for store and
"re-shard" for shard at once - and the rebalancer would be mixing
placement arithmetic across modes.  A deployment that genuinely needs
two modes runs TWO clusters on distinct multicast groups; a daemon can
join several.  Without `collections` the legacy per-collection `mode =`
still parses and warns, because nothing then checks that your peers
agree; new deployments should declare the cluster form.

### The modes

The five modes differ in one thing: how many nodes hold a record, and
which.  Every mode may accept a write on any node.

**`store`** pulls on a local miss and KEEPS the copy, so every node
converges on the working set.  Each node's ceiling is its own arena.

**`eager`** is store plus a push: every write goes to every live peer
as it lands - on the write path, fire-and-forget, whatever the TTL - and
a background sweep repairs what a push did not reach.  Replicas are held
off the WAL: a record is durable where it was written, and a node that
restarts holds its own writes until the sweep refills the rest.

Applying those copies is ONE thread per node while the sending side is
every worker in the fleet, so under enough write pressure a receiver
falls behind, and it falls behind QUIETLY: it keeps heartbeating, stays
a member, and its client door stays fast; the only symptom is a read of
a key that exists on that node solely as a copy it has not applied yet.
Past the receive buffer, datagrams are dropped and the repair sweep puts
those records back.  So an eager fleet under overload is eventually
consistent, with the sweep as the repair.  The `replica intake` card on
the status page shows it - applied per second, the receive queue
against the buffer, the drops - as do `stats.cluster` (`rx_applied_ps`,
`rx_queue`, `rx_rcvbuf`, `rx_drops_ps`) and `/metrics`.  A queue that
sits near the buffer is a node at its apply limit; a nonzero drop rate
is records already lost and waiting on the sweep.  Eager's other cost
is that every node applies every write, so the fleet's write rate is
one node's apply path and falls as nodes are added - the measured case
for `spread`, below.

**`proxy`** - the capacity plane - keeps each key on exactly ONE node:
placement by free memory at write time, reads served through without
storing, writes forwarded to the holder, and a 10 s rebalancer that
levels the fleet by live utilization (coldest records first, oversized
ones over a TCP bulk channel).  Fleet capacity ~= the SUM of the arenas.

    [cluster]
    multicast = 239.68.68.1:6480
    mode = proxy
    collections = blobs

**`shard`** - deterministic ownership - places each key on exactly ONE
node chosen by rendezvous hashing over the members' addresses
(CRUSH-style): no locator, no placement races, misses answered
authoritatively in one round trip, and counters serialized at the
owner from any ingress.  Membership changes reshard automatically -
only the moving keys travel, and reads fall back to a broadcast during
the move so nothing misses mid-reshard.

    [cluster]
    multicast = 239.68.68.1:6480
    mode = shard
    collections = ids

**`spread`** - K copies, not P.  Each record is held by **K nodes chosen
by placement**, where eager is this with K = P and shard is this with
K = 1:

    [cluster]
    multicast = 239.68.68.1:6480
    mode = spread
    replicas = 3
    collections = sessions

The point is apply load.  Under `eager` every node applies every write
in the fleet; under a copy factor the passive work is (K-1)/P per node
and **falls as the fleet grows**.  `replicas = 1` is refused rather than
aliased to shard.  Any node may accept a write, but only holders keep
it: a node outside the set forwards the record to the K holders and
does not retain a copy - otherwise every writing node becomes a soft
K+1 whose extra copies are orphans, never repaired or reclaimed - and a
pull served through a non-holder is served, not cached.  It is
replication, not erasure coding: the unit of storage stays the whole
record (an erasure-coded mode was built and removed in 0.2.0).

*Status, 2026-09-15.*  Complete since 0.3.7-rc2: placement, the write
path, the read path with libperfd 0.2.8, membership repair on a set
change, the K-scoped counters (`replicas` in `stats`, the `spread_*`
counters) and the fleet measurements below.  A node that bounced inside
another node's slot grace was never re-placed to until rc13 (S147).
What it has not had is production-shaped traffic: the staging fleet
runs `eager`.  Treat it as a candidate mode - run it on a fleet you can
watch (`spread_*`, `replicas`, `perfd_route_missed()`) before one you
cannot.

*Where it pays, and where it does not* - measured on rc16 with
`bench/spreadbench.sh`; the table is under Measured:

- **Use it where eager's write ceiling binds.**  Every eager node
  applies every write, so the fleet's write rate is one node's apply
  path and FALLS as nodes are added: 1.84M SET/s at three nodes, 1.11M/s
  at six.  `spread` K=2 held 1.76M/s at six nodes on the same cores,
  +59% at a lower p99, and the gain grows with P.  Each node also holds
  K/P of the keyspace instead of all of it - the memory case is the
  same arithmetic.
- **Use it only with a routing client.**  libperfd 0.2.8 sends each key
  to a holder, and through it reads are eager's, within 5%, at the
  fleet's worker ceiling: 4.25M GET/s at K=2, P=3 with zero pulls.  A
  client that picks a node without the map - a plain load balancer, a
  connection pool - reads at ~0.3M/s with a p99 over 30 ms, nine times
  slower than eager, because a node that is not a holder pulls the
  record over the peer plane, one thread, and at K=2 of P=6 that is two
  reads in three.  Adding nodes makes that worse, not better.  Whether
  a cluster-aware Redis client on the RESP door lands on a holder has
  not been measured.
- **Choose K against the failures you intend to survive**, not against
  eager's habit: losing K specific nodes loses the keys they held,
  where eager loses nothing until the last node.  K=3 of P=3 is eager
  with more bookkeeping - measured identical on reads.
- **Not this mode**: fleets where P <= K (that is eager), read-heavy
  fleets behind a balancer you cannot make cluster-aware, keyspaces
  that need rack or zone anti-affinity (placement is not
  topology-aware), and anything that wants cross-key atomicity (no mode
  has it).

### Durability

In-memory only by default.  Add a `[wal]` section for write-ahead
logging + RDB snapshots; recovery replays snapshot then WAL tail at
startup:

    [wal]
    dir = /var/lib/perfcached
    fsync = everysec             # always | everysec | no
    save = 900 1                 # RDB snapshot rules, Redis-style,
    save = 300 10000             # repeatable and OR-ed

**Choosing `fsync`.** The pump fsyncs once per drained *batch*, so with
`fsync = always` the per-writer ring has to absorb everything that
arrives while it sits in `fdatasync`.  On storage whose fdatasync takes
milliseconds the shipped 1 MB ring is not enough, and an overflowing
ring used to drop acknowledged writes *silently* — measured at up to
13% of a 20,000-key fill, present in the live table and absent after a
restart.  The probe now derives the depth from the measured p99 and the
daemon applies it (set `ring_kb` yourself to override; with `probe = no`
nothing derives it, so set it).  A drop that still happens is logged,
and because it is data loss the client was told succeeded, the node
marks itself **FAILED**: it stays a member and keeps answering reads,
but refuses writes and no client selects it for new work until it is
restarted with a deeper ring, `everysec`, or less offered load.
`everysec` does not have this problem: it fsyncs on a timer, so the
ring drains freely between them.

`perfcached -P /var/lib/perfcached` probes the storage first (fsync
latency, sustained rate) and prints the policy it would recommend;
`-I` prints the storage identity chain (NVMe/SAS/network/LVM...),
`-W`/`-R` inspect WAL segments and snapshots offline.  The `sync` and
`load` admin verbs give you an fsync barrier and additive snapshot
import at runtime.

### Other settings worth knowing

- `max_clients` under `[daemon]` caps client connections across the
  data doors.  Unset, it is derived from the descriptor limit minus
  what the process needs open; at the limit a connection is accepted,
  told why (`-ERR max number of clients reached` on the RESP door, an
  error reply in its own dialect on the native door) and closed, and
  `/stats` still answers.  The refusals are counted.
- A fleet stanza runs unchanged on a single node: without a
  `[cluster]` section, a collection's `mode` and `pull` are accepted,
  ignored and named once at warning level, and the cluster secret is
  optional.  With a `[cluster]` section, a `replicas` the live members
  cannot honour is logged once and shown as `replicas_short` on
  `/stats`, `/metrics` and the status page.
- `plaintext = loopback` under `[listen]` allows unencrypted dialects
  on 127.0.0.1/unix only - handy for netcat debugging; the LAN stays
  on the Noise channel.  The default (`never`) encrypts everything.
- **`resp = <addr:port>`** adds a dedicated listener for Redis clients
  that must reach the cluster over a network.  It is RESP2 ONLY - the
  native dialects (and with them the admin verbs) are refused on it -
  and because a Redis client cannot speak the Noise channel it is
  plaintext, so it is guarded instead: `resp_allow = <cidr>[,...]` is
  REQUIRED off-box (the daemon refuses to start without it),
  `[secrets] resp` adds a Redis `AUTH` password, and
  `resp_collections` bounds which collections it can see and maps the
  Redis database index onto them: an entry `0:sbcha` makes `SELECT 0`
  reach the collection `sbcha`, and a bare name is reachable by that
  name.  Without a mapping a collection has to be NAMED `0` to be
  reachable at all.  `stats` reports a `resp` block (connections,
  allow-list rejections, auth failures).  The door also serves the
  Redis observability surface - section-faithful `INFO` (commandstats
  and latencystats included), `CLIENT LIST`/`SETNAME`, `SLOWLOG`, and
  the `CLUSTER` family (`SLOTS`/`SHARDS`/`KEYSLOT`/`NODES`) - so
  Grafana's redis-datasource and cluster-aware Redis clients work
  against it unmodified; `TIME`, `EXPIREAT`/`PEXPIREAT` and `MEMORY
  USAGE` round out the tooling set.  The command rows count every
  door: RESP commands under their Redis names, the native doors as
  `json.<verb>` and `bin.<verb>`, each row with a log2 latency
  histogram behind it - and the same rows are `commands` and `slowlog`
  on `/stats`, `perfcached_command_calls_total{cmd}` /
  `perfcached_command_usec_total{cmd}` and the
  `perfcached_command_latency_seconds{cmd}` histogram on `/metrics`.
- `arena_cap_mb` lets the arena grow elastically under pressure - by
  2 MB group inside one address-space reservation, on the same tier as
  the initial commit; `reclaim_*` returns idle groups to the kernel.
- A reserved hugepage pool makes the arena's top tier deterministic -
  see [contrib/sysctl-perfcached.conf](contrib/sysctl-perfcached.conf),
  and read back `HugePages_Total` after applying: live hosts routinely
  under-deliver the reservation until memory is compacted.
- `log_events = miss, expired, store, remove` (per collection, any
  subset) logs one line at NOTICE per event with the key, the door and
  the peer, and for a miss whether the key was absent or expired.  It
  is a per-collection mask, not a level: `log_level` is a ceiling,
  `[daemon] log_events` is the default, `log_events_rate` (100/s)
  bounds it and the daemon reports what it suppressed,
  `log_events_hash = yes` logs a hash instead of the key.
- `perfcached -E -f <conf>` dumps the normalized effective config with
  secrets masked.

### Sizing

What a set of collections costs on a host, per node - in eager mode every
node holds everything, so the figure is per node, not per fleet.  The
page's "memory budget" card computes the same arithmetic from the
daemon's exact figures; this is how to do it before the daemon exists.

1. **Index.**  Each collection's table is carved at creation.
   `buckets_log2` is 4..24 - the same range in a config file and at the
   `create` and `resize` verbs - but the cost is FLAT below 2^12 and 68
   bytes a bucket above it, because segments are fixed at 4,096 buckets
   and a table always carves whole ones:

   | buckets_log2 | buckets | index | per bucket |
   |---|---|---|---|
   | 4 .. 12 | 16 .. 4,096 | 1.25 MB | - |
   | 16 | 65,536 | 5.0 MB | 80 B |
   | 18 | 262,144 | 17.8 MB | 71 B |
   | 20 | 1,048,576 | 68.8 MB | 69 B |
   | 24 | 16,777,216 | 1.1 GB | 68 B |

   So anything below 12 buys nothing over 12, and the top of the range
   is measured in gigabytes - all of it carved before a single record.
   A table grows by splitting once it passes `grow_at_pct` of its slot
   capacity (`[daemon]`, default 75% - six slots a bucket, so 4.5
   entries), one bucket at a time, and growth adds index.  It shrinks
   too: once it has sat below `shrink_at_pct` (default a quarter of
   `grow_at_pct`) for `shrink_cooloff_s` (default 60 s) since it last
   split, was resized or started, it is resized down to the power of
   two midway between the two thresholds - one copy, never below one
   segment - and the old index goes back to the arena once every reader
   has moved on.  A table may also be widened PAST `grow_at_pct` by its
   own overflow leg: a key whose bucket is full lives in a shared leg
   and is found by walking a chain under one lock, the maintenance tick
   puts back every leg record whose bucket has since made room, and what
   it cannot put back is counted (`leg_stuck`).  Once that exceeds
   `leg_stuck_pct` of the entries (default 1%) the splitter aims at
   `grow_floor_pct` (default 50%) instead, which is the only thing that
   can help - those buckets are full.  Set `grow_floor_pct = 0` to turn
   it off; a table whose leg is quiet never notices it.  A create or a resize whose index would not fit under
   the ceiling in item 4 is refused, naming both figures.
2. **Records.**  A record is its key, its value and a 28-byte header,
   rounded UP to a cell class.  The classes step by 1.5 and 1.33 from
   64 bytes to 64 KB (64, 96, 128, 192, 256, 384, 512, 768, 1 K, 1.5 K,
   2 K, 3 K, 4 K, 6 K, 8 K, 12 K, 16 K, 24 K, 32 K, 48 K, 64 K) and then
   to 96 K and 255.9 K, so the rounding is anywhere from 0 to 50 % - and
   up to a whole 256 KB chunk for the last two, which are cut one and
   two cells to a chunk.  **The largest record is 262,080 bytes**, key
   and header included: one cell per chunk, which is the most a 256 KB
   chunk can hold.  A larger value is refused, not truncated.  A measured mix - 100,000 records
   of 96 B, 768 B, 2 KB and 4 KB values, 62.9 MB of values - occupies
   94 MB.  Budget 1.5 x the key+value bytes for a mixed set, or bin by
   bin from your own size histogram.
3. **Residue after a burst.**  About 10 MB of warm free slots kept
   resident for the next growth, and up to 10.5 MB of chunks the size
   classes own (two 256 KB chunks per class).
4. **The ceiling.**  1 + 2 + 3 at the PEAK must fit under `arena_mb` (or
   `arena_cap_mb`): a full arena refuses writes and evicts nothing.
5. **What the host sees.**  The arena reserves `arena_cap_mb` (or
   `arena_mb` when no cap is set) of address space, charged for
   nothing, and commits `arena_mb` of it at start - populated, and
   pinned when the daemon may pin - so resident memory is `arena_mb`
   plus about 15 MB for the daemon from the first second.  Growth
   commits 2 MB groups inside the reservation as records arrive, all on
   the same tier; once idle the give-back returns the never-carved part
   of the commit, and after a burst it returns the drained groups, so
   resident follows `arena_committed` (in `stats` and on `/metrics`) to
   within a few 2 MB groups.  The arena's pages are counted in RSS when
   the tier is transparent huge pages (the default when no hugetlb pool
   exists); on a hugetlb pool they are in `HugetlbPages` instead and RSS
   shows only the daemon itself.

Read it back on the page: `held` and its parts (structure, class
chunks, warm free), the budget card, and `resident` from the kernel.

### Example configurations

[contrib/CONFIGURATIONS.md](contrib/CONFIGURATIONS.md) - ten complete
files, each validated with `perfcached -C`:

- **single node**: minimal; for Redis clients (the RESP door with its
  guards, `SELECT n` onto named collections, the status page, a local
  socket); durable (identity, WAL, snapshots, runtime DDL behind the
  `enable` secret, a sampled query log); elastic memory (a capped arena
  that gives back, an index that grows and shrinks).
- **fleets**: eager; store (pull-on-miss); proxy (the capacity plane);
  shard behind cluster-aware Redis clients; spread with `replicas`.
- **two fleets on one host**: a second daemon in a second cluster.

## Operating it

### Watching it

`http = <addr:port>` under `[listen]` serves the status page - one page
per node, the fleet's view - and beside it `/stats` (what the `stats`
verb answers, as JSON), `/metrics` for Prometheus, `/members`, and
`/clients` (this node's connections: the table behind the page's clients
strip).  The page's headline strip reads `clients 123 / 1,000
fleet-wide` - this node over the fleet, the denominator summed from the
map every member gossips its count into, absent on a standalone daemon
- and clicking it opens this node's connections (door, dialect, wire,
address, name, age, idle, commands, last command, pending output bytes,
subscriptions) with a filter on address and name; the cluster plane
carries the commands cards: by calls, slowest per call, and the slow
log's tail.  The page stays read-only: closing a client is `CLIENT
KILL` on the RESP door.  The collection rows answer for the fleet: every
figure shows the fleet's total first and this node's share beside it
("63 · 39 here"), summed from the per-collection block each member
gossips (its own client hits and misses, so a pull served for a peer
counts once), joined by the name's hash; entries summed that way count
copies, and the cell says so.
`http_allow` bounds who may reach it and `[secrets] http` adds a token.
`perfcli stats` gives the same figures on the JSON door.

### Collections at runtime

A collection can be created and dropped while the daemon serves, so a
new keyspace does not need a config edit on every node and a fleet
restart:

    perfcli create sessions            # 2^12 buckets by default
    perfcli create sessions 16         # or say the size
    perfcli drop sessions              # refused while it holds records
    perfcli drop sessions force        # dropped with them

A collection can also be resized in either direction and renamed, both
while it serves:

    perfcli resize sessions 16         # either direction
    perfcli rename sessions_restored sessions

A resize fills a second table at the new size from the live one, swaps
the two in one step, and collects what the swap window left behind; the
call returns as soon as the migration has started, and `stats` carries
`resizing_to` and `resize_moved` while it runs.  A key deleted during the
migration stays deleted, a key written during it keeps its new value, and
a target the splitter would immediately grow back is refused.  The old
table's index goes back to the arena once every reader has moved on
(`stats.retire` shows it pending, then cleared).  A rename is one
pointer swap, which makes it the atomic cutover a restore wants: load a
dump into a second collection with `perfload --collection-map`, verify
it, rename it into place, drop the old one.

This is off by default.  Set `[daemon] allow_create = yes` on the nodes
where a client may do it: a driver that creates on a miss turns a typo
into a second, empty collection and an operator into someone whose cache
"lost everything".  The setting gates who may ORIGINATE a create, not
what a node accepts from the fleet, so a create made anywhere reaches
every member whatever their own setting, and turning it on does not need
a fleet restart.

**`allow_create` is not enough on its own, and deliberately so.** It
answers "may this NODE originate DDL". It cannot answer "may THIS
CONNECTION", because applications, ops tooling and `perfcli` all present
the same `[secrets] client` value - so gating on that alone would hand
`create`, `drop`, `resize`, `rename` and `restore` to every application
link. A second secret settles the second question:

    [secrets]
    enable = <a value that is NOT the client secret>

A connection raises privilege with `enable` and drops it with `disable`,
both on the **JSON door only** - never RESP, which may run plaintext
off-box and would put the highest-value secret in the fleet on the wire
in the clear. The privilege lives on that connection, dies with it, is
never replicated, and is never consulted when a peer applies DDL the
fleet already agreed. Failed attempts are counted as
`stats.door.enable_fails`.

**Fail-closed:** with no `enable` secret configured, `enable` always
fails and those verbs are unreachable from the client door **however
`allow_create` is set**. A node with `allow_create = yes` and no enable
secret therefore refuses all client DDL, and says so at startup. That is
the safe direction, but it will surprise you on an upgrade if the config
is not updated with it.

For the tools: `perfcli -E <secret>` (or `-e` to be prompted, or
`PERFCLI_ENABLE` - deliberately not `PERFCLI_AUTH`, so exporting the
application's secret does not confer privilege), and `perfload
--enable <secret>`, which `restore` needs.

A created collection takes the cluster's mode: a fleet is one mode over
one collection set.  It is remembered in `[daemon] state_dir` and comes
back after a restart, it rides the WAL as a record of its own so a
replay lands records in the collections that existed when they were
written, and it is announced to every live peer, with the whole set
re-announced every few seconds so a node that was down through the
change picks it up when it returns.  Without a `state_dir` a created
collection is ephemeral, and the daemon says so.

### Open connections, running totals, and resetting them

`stats` reports, per door and dialect, what is open right now beside
what has happened since the daemon started: `stats.resp.open` and
`stats.native.{json,binary,resp}.open` are gauges; `conns` and
`requests` beside them are running totals.  `stats.since` says what
the totals count from: `{"reset_at": <unix seconds, 0 = never>, "s":
<seconds since the reset, else since start>}`.  The page's door cards
lead with "open" and label the totals "since start" or "since reset";
`/metrics` exposes the gauges as `perfcached_connections_open{door,
dialect}`.

The running totals can be started again, on one node at a time:

    perfcli reset-stats                          # the JSON verb reset_stats
    redis-cli -p 6380 CONFIG RESETSTAT            # the RESP door
    curl -X POST http://node:8080/reset-stats     # the page's button does this

A reset covers the doors and dialects, each collection's table counters
(hits, misses, stores, removes, expired - the core's own re-baseline),
the store's size tallies, and the cluster, proxy and WAL counters.  It
never touches a gauge: open connections, entries, buckets, memory,
sequence numbers, roles.  `/metrics` keeps counting from the start -
Prometheus counters are meant to be monotonic, and its `rate()` would
read a reset as a wrap.  `POST /reset-stats` is the one mutating HTTP
route: body-less by contract (a body is a 400), guarded by the
`[secrets] http` token like every other route, `POST` anywhere else a
404.

## Tools

### perfcli

The redis-cli analogue.  Word commands mirror the verb set; `-a` runs
the Noise handshake (client principal) for encrypted listeners:

    perfcli -p 6479 set sessions user:17 "some value" 300
    perfcli -a 's3cret' get sessions user:17
    printf 'ping\nstats\n' | perfcli -q          # pipe mode
    perfcli -j '{"method":"keys","params":{"col":"sessions","match":"user:*"}}'

`help` inside the REPL lists everything; jset takes raw JSON.  Every
command, its arguments and its reply shape:
[PERFCLI-COMMANDS.md](PERFCLI-COMMANDS.md).  The
REPL has its own line editor - arrows + history (persisted 0600 in
`~/.perfcli_history`, duplicates collapsed), emacs keys
(Ctrl-A/E/B/F/W/U/K/L), Ctrl-R reverse search, Ctrl-C discards the
line, Ctrl-D quits.

`pretty on` (or `-P`) re-indents every JSON result - together with the
JSON path verbs:

    $ perfcli -p 6479 jset sessions user:17 '$' \
        '{"name":"ann","roles":["admin","ops"],"quota":{"used":3,"max":10}}'
    {"set":true}
    $ perfcli -p 6479 -P jget sessions user:17 '$.quota'
    {
      "found": true,
      "value": {
        "used": 3,
        "max": 10
      }
    }

### perfdump

A parallel dumper over the client door, shaped like mydumper: one node's
collections walked by N connections, each owning a range of the table's
buckets, into a directory of chunk files with a manifest.

    perfdump --from 192.0.2.10:6479 -a <client secret> --out /var/backups/pc-$(date +%F)
    perfdump --inspect /var/backups/pc-2026-09-09

`--threads` (default 4) connections per collection, `--count` buckets per
call (default 1024, halved on the daemon's refusal), `--chunk-mb` (64)
before compression, `--zstd` level (3; 0 = raw), `--rate` records per
second per connection so a dump never starves production,
`--collections a,b` to pick.  Every chunk carries a CRC-32 and a trailer,
the manifest is rewritten atomically as each chunk completes, and
`--inspect` reads every file back and checks both.  The format is
[doc/perfdump-format.md](doc/perfdump-format.md); `perfload` loads it
back.  On an eager fleet any READY node holds everything, so dump from
one; a node still pulling its bootstrap is refused as a source.  A
table that grows under the dump can repeat a record (the cursor is
at-least-once across a split, as Redis SCAN is); the loader upserts by
version, so that costs bytes, never correctness.  Needs the `zstd`
binary for compressed dumps.

### perfload

The loader, the reverse of perfdump: every record goes back with the
value, the VERSION and the absolute expiry the dump held, through the
daemon's write path (the WAL, the eager push), so a fleet of any size
or mode takes it as if a client had written it - only with the dump's
history.  Load a three-node eager fleet's dump into a two-node shard
fleet, or the other way round.

    perfload /var/backups/pc-2026-09-09 --to 192.0.2.10:6479 -a <client secret>
    perfload /var/backups/pc-2026-09-09 --to 192.0.2.10:6479 -a <client secret> --dry-run
    perfload /var/backups/pc-2026-09-09 --to 192.0.2.10:6479 -a <client secret> --collection-map sessions=sessions_restored

Every chunk's count and checksum is verified before anything is sent
(`--no-verify` skips it).  `--threads` (default 4) connections stream
`restore` batches of `--batch` records (256) at `--depth` calls in
flight (8); `--rate` caps records per second over all threads.  On a
shard fleet each record goes straight to its owner (the library
routes it); on an eager or plain fleet the chunks are spread across
the members and the push carries the rest.  A record the daemon does
not own comes back named and is re-sent to the node it names.

`--policy newer` (default) is the store's own rule: a record older
than, or as old as, the copy the fleet holds loses and is counted.
`--policy skip` leaves every existing key alone.  `--policy overwrite`
installs the loaded value under a fresh version, so every replica
takes it - a fleet rolled back to a dump.  A record whose expiry has
passed is skipped and counted, never re-based.  `--collections a,b`
picks, `--collection-map a=b` loads a's chunks into b; the target must
already have the collection.

A chunk is marked done in `DIR/perfload.done` once its last batch is
acknowledged, and the next run skips it (`--restart` reloads
everything); with the default policy a re-run over a finished load
stores nothing and counts everything older, so a loader killed halfway
is simply run again.  The final line has records, bytes, elapsed,
records per second and the counts (stored, older, existing, expired,
refused, bad, rerouted); on a fleet a second line says how long the
members took to settle after the load.  `--via-set` loads through
plain pipelined `set` instead - the comparator: it re-bases TTLs and
assigns fresh versions, the two things a loader must not do.
Measured on the build host: a million 224-byte records in 0.8 s on
eight threads, 1.4x faster than plain SET on the same connections.

## Clients

### libperfd

The hiredis-analogue C client: typed verbs, binary-safe values, the
Noise channel with a secret LIST (rotation = add-new/drain-old), and a
pipeline that delivers replies in request order.  `opts.binary = 1`
switches the data verbs to raw binary frames (no JSON/b64 leg) behind
the identical API:

    #include <perfd.h>
    const char *secrets[] = { "new-secret", "old-secret", NULL };
    perfd_opts o = { .secrets = secrets };   /* defaults for the rest */
    perfd_t *p = perfd_connect("10.0.0.1", 6479, &o);
    perfd_set(p, "sessions", "user:17", blob, blob_len, 300);
    if (perfd_get(p, "sessions", "user:17", &val, &vlen, &ttl) == 1) { ... }
    perfd_free(p);
    // link: cc app.c libperfd.a -lsodium -lpthread

**Cluster-aware.** Set `opts.spares` and the library learns the
fleet on connect, keeps standby connections open to the other nodes,
and swaps onto one when the node it is using dies - a failover costs a
`send()` on an established socket, not a TCP+Noise handshake.
`opts.policy` picks where a client works: `FAILOVER` (default),
`ROUND_ROBIN` (independent random start per client - a thousand
clients spread with no coordination), `LEAST_CONN` or `WEIGHTED` (by
the free arena each node reports).  Idempotent verbs are replayed
across a failover; `add`/`sub` are NOT - the caller is told, because a
double increment is worse than a visible error.  `perfd_member_count`,
`perfd_active_node`, `perfd_spare_count` and `perfd_failovers` let a
caller see what it is doing.

**Per-key routing.**  Add `opts.route_keys = 1` and each request goes
to the node that should hold its key, so the daemon's forward hop
disappears.  It applies to `shard` (the owner is computable), `spread`
(the holders are the top K of the same ranking; from 0.2.8) and `store`
(hashing a key to one node makes the client a de-facto single writer
for it, which is what stops concurrent writers forking a key); `proxy`
is not routed.  It is never load-bearing: the daemon re-checks
ownership and forwards a wrong guess, so a stale view costs a hop, not
correctness - `perfd_route_missed()` counts those.  Off by default,
like the spreading policy.  On the async API the application routes:
one handle per node, picked with `perfd_owner_of()`, because the
library will not open connections an async caller would never poll.

### Redis pub/sub

Every door publishes and subscribes on one global channel space:
`SUBSCRIBE`, `PSUBSCRIBE`, `PUBLISH` and `PUBSUB` on the RESP door work
as on Redis, with Redis's subscribed-mode rules and message frames, and
a subscriber that cannot keep up is closed at the output cap the way
Redis closes it.  The
native doors have the same five verbs, JSON methods or binary verbs 10
to 14, with messages delivered as id-less notifications so a native
connection stays multiplexed, and libperfd wraps them as
`perfd_publish`, `perfd_subscribe` and friends with delivery on its
notify hook.  On a fleet a publish on any node reaches the subscribers
on every node, over the same sealed unicast plane the replication
rides, at-most-once with gaps counted; `PUBLISH` answers the local
receiver count, as Redis Cluster does.  Behind the HAProxy
VIP that means a subscriber pinned to one node hears a publisher that
landed on another.  A node set to `pubsub_relay = interested` is
relayed only the publishes that may match a subscription it holds - its
channels travel to its peers as a Bloom filter, its patterns as
strings, a new subscription at once - so a large fleet stops sending
every publish to every node; the default, `all`, relays everything.
Messages waiting for a worker to write them are capped per worker
(`pubsub_queue_mb`, 16 MB by default).  A publishing connection that
fills a worker's queue to half the cap is paused - the rest of its
input waits, and TCP holds the client - until the queue is below a
quarter, so a publish the node answered is a publish it delivers, and
`publish_paused` on `/stats` counts the pauses.  Messages from peers and
keyspace events have no connection to pause: past the cap they are
dropped and counted (`queue_dropped`), never allowed to pin memory or
delay a shutdown.  Raising the cap lets publishers run longer before a
pause and costs memory and a slower exit under load.
A native connection may take its deliveries over UDP instead
(`pubsub_udp {"port":N}`): they go only to the connection's own
address, from the door's address and port, sealed with a key the
connection was given, and only after the client echoed a probe's cookie
over the connection; the client acknowledges every 256 messages or 5 s,
and one that stops - or whose acknowledgements stop moving - loses its
subscriptions and is told.  Deliveries over 1,400 bytes stay on TCP;
the protocol is in `lib/perfd_push.h`.  In libperfd (0.2.10) it is
`perfd_pubsub_udp(handle, port, timeout_ms)`, then
`perfd_pubsub_udp_ready()` on the socket's readability and at least
once a second: the messages reach the same notify hook with the same
JSON, on blocking and async handles alike.  A collection with `notify_events` publishes Redis
keyspace notifications for what this node applies, never relayed, so
`PSUBSCRIBE __keyevent@0__:*` works as it does against Redis.  The commands and the frames are in
[REDIS-COMMANDS.md](REDIS-COMMANDS.md); the figures are the `pubsub`
block on `/stats` and `/metrics`.

### cachedb_perfd

The OpenSIPS driver module, thin glue over libperfd with its own README
and timeout/policy knobs, lives in the OpenSIPS tree.

## Measured

Every table below is one day's run of the tagged 0.3.7 daemon
(`e58ee8e`), 2026-09-16, on one server host - a 16-vCPU Debian 13 VM -
with the load generated elsewhere wherever the harness allows it: the
wire table and the `spread` table drive the server from other hosts;
the cluster tables drive it from a container on the same host, which
makes those cells client-bound on a 16-vCPU box, and the section says
so where they stand.  The harness stamps the binary's revision into
every results file and refuses to measure a build that cannot name
itself.  Read a figure with what bounded it: the SET cells at capacity
pin the server's cores, and most GET cells are bounded by the client
or the wire.  The method behind each table, how to reproduce it, and
the readings that changed as the harnesses improved are in
[bench/RESULTS.md](bench/RESULTS.md); raw rows are in
[bench/results/](bench/results/), every earlier run's included.
Numbers rot: quote from a run, not from here.

### One server against one server, over the wire

`redis-benchmark` - Redis's own tool, unmodified - on one host; one
perfcached node, no cluster, and Redis on another, both as containers
with host networking, a real NIC between them at MTU 9000 verified end
to end, arms alternated so drift cannot favour either.  0.3.7's daemon
(`e58ee8e`), 2026-09-16, the 16-vCPU Debian 13 server host, a 16-vCPU
Ubuntu 24.04 client host; 4 workers, 20k keys x 200 B, cells sized to
run for over a second, median of 3 (`bench/xhostbench.sh`,
[bench/results/xhostbench-0.3.7.tsv](bench/results/xhostbench-0.3.7.tsv)).

**50 clients, no pipelining:**

| | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| **redis-server 8.10.1** | 53,879 | 50,710 | - | 1.29 ms | 1.12 ms |
| perfcached, 1 node | 47,103 | 46,361 | x0.87 / x0.91 | 0.97 ms | 0.90 ms |

**50 clients, pipeline 64:**

| | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| **redis-server 8.10.1** | 736,106 | 948,317 | - | 5.33 ms | 4.39 ms |
| perfcached, 1 node | **1,622,060** | **1,381,216** | **x2.20 / x1.46** | **2.67 ms** | **2.02 ms** |

At depth 1 both servers wait on the round trip and the wire decides:
perfcached lands a little under Redis on throughput and well under it
on the tail (0.97 ms against 1.29 ms on SET).  At depth 64 the
difference is the servers - **+120% on SET at a 2x tighter p99, +46%
on GET** - because Redis is single-threaded and perfcached runs four
workers.  This is the number a deployment gets; the loopback
comparison that stood here until 0.3.7, and why a loopback read
overstates a 1500-byte network, are in
[bench/RESULTS.md](bench/RESULTS.md).

### The cluster modes through a Redis client

<!-- containerbench:0.3.7 -->
Three-node fleets, one per mode, driven through node 1's RESP door by
`redis-benchmark` - the honest shape of a Redis migration, and a client
that **does not route**: it dials one node, so every key that node does
not own is a forward, and under `spread` every key it does not hold is
a pull.  The first row is real Redis (`redis:8`, persistence off) on
the same rig.  0.3.7's daemon (`e58ee8e`, the tagged tree), 2026-09-16,
the 16-vCPU Debian 13 server host under podman, 20k keys x 200 B, 8
workers a node, 50 clients, median of 3, every cell sized to run for at
least 10 seconds (`bench/containerbench.sh`,
[bench/results/containerbench-0.3.7.tsv](bench/results/containerbench-0.3.7.tsv)).
One node on its own, same rig, same client: 2,019,182 / 2,439,768 at pipeline 16,
2,460,398 / 2,860,570 at 64.

**50 clients, pipeline 16** (redis: 584,759 SET / 637,948 GET):

| mode | SET/s | GET/s | vs redis | SET p99 |
|---|---|---|---|---|
| store | 1,951,522 | 2,251,502 | x3.34 / x3.53 | 1.68 ms |
| eager | 1,522,083 | 2,391,401 | x2.60 / x3.75 | 1.53 ms |
| proxy | 1,791,693 | 2,229,510 | x3.06 / x3.49 | 1.67 ms |
| **shard** | **249,719** | **215,994** | **x0.43 / x0.34** | 1.88 ms |
| **spread K=2** | **1,562,500** | **433,996** | **x2.67 / x0.68** | 1.30 ms |

**50 clients, pipeline 64** (redis: 849,397 SET / 993,728 GET):

| mode | SET/s | GET/s | vs redis | SET p99 |
|---|---|---|---|---|
| store | 2,375,931 | 2,803,655 | x2.80 / x2.82 | 3.94 ms |
| eager | 1,872,439 | 2,987,149 | x2.20 / x3.01 | 4.38 ms |
| proxy | 2,188,910 | 2,692,763 | x2.58 / x2.71 | 4.12 ms |
| **shard** | **153,592** | **142,597** | **x0.18 / x0.14** | 3.23 ms |
| **spread K=2** | **1,928,828** | **365,967** | **x2.27 / x0.37** | 4.02 ms |

Reads stay in one band across the placement modes; eager's SET pays for
its replication on the write path, and so does spread's - each write
lands on two of the three nodes.  Two rows are the client, not the
mode.  Shard's is the forwarding hop: a client that cannot compute an
owner forwards two keys in three.  Spread's GET is the pull: the node
the client dialled holds two keys in three and fetches the rest on
every read - a pulled copy is not kept - and that fetch is what a
Redis client pays for the mode.  Both are the floor; the next section
is the same fleets through clients that can route.
<!-- /containerbench:0.3.7 -->

### The same fleets through clients that route

Same fleets, same daemon, same day.  Two clients that can route, both
driven from a container on the server host - which makes every cell
here client-bound on a 16-vCPU box, so read them as floors; the
off-box farm in the `spread` section is the ceiling for the same
daemon.

**A Redis client that routes**: `redis-benchmark` given one hash tag
per owner from `CLUSTER NODES`, three drivers, one per node - what a
cluster-aware Redis client does with `CLUSTER SLOTS`, without one.
The plain rows above are the same two fleets through the one-node
client.

**50 clients, pipeline 16** (redis: 584,759 SET / 637,948 GET):

| mode | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| **shard** | **2,799,758** | **3,128,998** | **x4.79 / x4.90** | 0.91 ms | 0.69 ms |
| **spread K=2** | **1,998,812** | **3,143,135** | **x3.42 / x4.93** | 1.24 ms | 0.73 ms |

**50 clients, pipeline 64** (redis: 849,397 SET / 993,728 GET):

| mode | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| **shard** | **4,506,397** | **5,590,501** | **x5.31 / x5.63** | 2.10 ms | 1.46 ms |
| **spread K=2** | **3,112,913** | **5,831,098** | **x3.66 / x5.87** | 5.20 ms | 1.42 ms |

**A cluster-aware client**: `natbench` over libperfd with
`opts.route_keys`, binary dialect, one process spreading its
connections over the fleet and sending each key to its owner or
holder.

**50 clients, pipeline 16** (redis: 584,759 SET / 637,948 GET):

| mode | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| store | 2,277,099 | 2,716,575 | x3.89 / x4.26 | 0.83 ms | 0.82 ms |
| eager | 1,364,667 | 2,733,418 | x2.33 / x4.28 | 1.97 ms | 0.72 ms |
| proxy | 1,895,392 | 2,999,965 | x3.24 / x4.70 | 1.05 ms | 0.66 ms |
| shard | 2,416,601 | 2,838,655 | x4.13 / x4.45 | 0.73 ms | 0.62 ms |
| spread K=2 | 1,526,034 | 2,697,405 | x2.61 / x4.23 | 1.98 ms | 0.81 ms |

**50 clients, pipeline 64** (redis: 849,397 SET / 993,728 GET):

| mode | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| store | 3,289,013 | 4,100,332 | x3.87 / x4.13 | 2.14 ms | 1.72 ms |
| eager | 1,788,391 | 4,099,295 | x2.11 / x4.13 | 4.84 ms | 2.01 ms |
| proxy | 2,120,087 | 3,895,724 | x2.50 / x3.92 | 3.83 ms | 2.11 ms |
| shard | 3,319,680 | 4,193,129 | x3.91 / x4.22 | 2.17 ms | 1.77 ms |
| spread K=2 | 2,001,896 | 4,205,866 | x2.36 / x4.23 | 4.77 ms | 1.74 ms |

Routing turns shard from the slowest arm into the fastest: 4,506,397 SET/s
and 5,590,501 GET/s at pipeline 64 through the Redis client, x5.31 / x5.63 against
Redis, where the plain client gets x0.18 / x0.14 - shard was never the
slow mode, it was the mode whose client could not compute an owner.
It takes spread's reads with it (5,831,098 GET/s, x5.87, where the plain
client's pull path gets x0.37), while spread's SET keeps the copy cost:
the holder that takes the write still pushes the second copy.  Through
libperfd all five modes read in one band, 3,895,724 to 4,205,866 GET/s at pipeline
64, and shard's SET (3,319,680) sits within 1% of store's (3,289,013); eager's and
spread's carry their copies (1,788,391 and 2,001,896).  What routing buys is two
things, the removed hop and connections spread over the fleet,
decomposed in [bench/RESULTS.md](bench/RESULTS.md).

**Read through a node holding nothing** (50 clients, pipeline 64, the
RESP client by construction; same run - node 2 is restarted empty
before every rep and read through):

| mode | GET/s | GET p99 |
|---|---|---|
| store | 3,078,317 | 1.25 ms |
| eager | 2,730,017 | 1.16 ms |
| spread K=2 | 2,657,723 | 1.10 ms |
| shard | 147,880 | 1.86 ms |
| proxy | 77,513 | 47.36 ms |

Store pulls the key and keeps it, eager already replicated it, and a
spread node that comes back empty is handed the whole keyspace by the
boot pull before the reclaim pass trims it away, so inside the bench
window it reads everything locally at eager's rate - that is S156,
fixed on master after this tag (the boot pull now hands a spread
joiner its share), and it is what 0.3.7 as tagged does.  The two modes
that have to go and get the key differ in how: shard unicasts to the
one owner, proxy consults a locator and broadcasts on a miss.

### `spread` at capacity, from an off-box farm

`bench/spreadbench.sh`, 0.3.7's daemon (`e58ee8e`), 2026-09-16.  P bare
daemons on the 16-vCPU server host; natbench (this tree's, over
libperfd, binary dialect, 64 connections, 8 threads, depth 32, 200,000
keys x 200 B, 30 s cells) on a 24-core farm of two other hosts (16 +
8).  *Un-routed* is one natbench per node per farm host with the
connections split - a dumb load balancer; *routed* is one natbench per
farm host letting libperfd 0.2.8 send each key to a holder.  Server
cores are the fleet's CPU-seconds over the cell: the three-node fleets
run 5 workers a node (15 threads), the six-node fleets 2 (12), so a
cell at the thread count is a server ceiling.  Eager is the yardstick
on the same fleet
([bench/results/spreadbench-0.3.7.tsv](bench/results/spreadbench-0.3.7.tsv)).

| fleet | SET, un-routed | SET, routed | GET, un-routed | GET, routed / eager GET |
|---|---|---|---|---|
| eager, 3 nodes | 2.39M (16.2, p99 7.6 ms) | - | - | **4.49M (13.3, p99 5.2 ms)** |
| spread K=2, 3 nodes | 2.35M (15.9, p99 7.7 ms) | **2.59M (16.2, p99 6.9 ms)** | **0.42M (11.8, p99 54.4 ms)** | **4.86M (13.7, p99 3.1 ms)** |
| spread K=3, 3 nodes | 2.17M (16.0, p99 7.1 ms) | 2.19M (16.2, p99 8.1 ms) | 4.61M (13.6, p99 5.9 ms) | **4.72M (13.8, p99 3.1 ms)** |
| eager, 6 nodes | **1.39M (16.0, p99 10.5 ms)** | - | - | **3.89M (11.6, p99 4.0 ms)** |
| spread K=2, 6 nodes | 1.80M (15.0, p99 8.8 ms) | **2.03M (14.1, p99 6.8 ms)** | **0.41M (13.5, p99 47.9 ms)** | **3.71M (10.7, p99 2.9 ms)** |
| spread K=3, 6 nodes | 1.54M (15.5, p99 9.5 ms) | 1.68M (15.3, p99 9.7 ms) | 0.52M (13.5, p99 44.9 ms) | **4.23M (11.5, p99 2.9 ms)** |

Writes are a server ceiling in every cell, and eager's falls with the
fleet: 2.39M on three nodes, 1.39M on six, because every node applies
every write.  Spread's holds up - 2.03M routed at K=2 on six nodes,
+46% over eager on the same six - because each node applies K of P.
Un-routed spread reads at K=2 are bound by the pull path, 0.41-0.42M
at a p99 around 50 ms, and at K=3 on three nodes there is nothing to
pull (every node holds everything) so they read at eager's rate.
Routed spread reads are eager's or better, at the fleet's worker
ceiling on three nodes - 4.86M at 13.7 of 15 threads, against eager's
4.49M - with a p99 near 3 ms against eager's 5 ms from the even split
of connections, and zero pulls.  Rows with p50, pulls and forward
counts per cell: the results file; `clients` says how many natbench
instances drove the row.

## Layout

    src/        daemon
    cli/        perfcli, perfdump, perfload
    lib/        libperfd (perfd.h + perfd.c -> libperfd.a)
    src/core/   vendored htable/arena core (see tools/sync-core.sh)
    bench/      the harnesses, RESULTS.md and results/
    test/       selftests, rigs
    tools/      sync-core.sh, matrix.sh, build tooling
    contrib/    systemd unit, annotated config, sysctl example

## Licensing

Two licenses, one boundary, machine-checkable via SPDX headers:

- **The daemon** (everything linking `src/core/`) is
  **GPL-2.0-or-later** - see `COPYING`.  The engine is shared with the
  OpenSIPS `cachedb_perf` module, and this keeps code flowing both
  ways without ceremony.
- **libperfd, the client library, is MIT** - see `lib/LICENSE` - so it
  can be embedded anywhere, proprietary software included.  The MIT
  set is exactly `lib/perfd.[ch]`, `src/json.[ch]`,
  `src/pc_noise.[ch]`, `src/pc_slot.h`, `src/pc_mix.h`, and
  `tools/sync-libperfd.sh` exports precisely that set to consumers.

The boundary is enforced, not remembered: `test/synctest.sh` runs the
sync's SPDX and include-closure check in `make check-fast`, on every
push, and a red sync blocks any tag that ships libperfd.  A header both
sides need goes on the MIT side from day one, or stays daemon-only with
the minimal piece duplicated under MIT - never a GPL include from an MIT
header.  `src/compat/dprint.h` is GPL and is resolved but never copied
(the consumer supplies its own logging shim).  The six vendored
`src/core/` files keep their full OpenSIPS GPL headers in place of an
SPDX line - a recorded exception.  See `CONTRIBUTING.md`.

Third-party: libsodium (ISC) - named with its notice in `lib/NOTICE`,
which the sync copies beside `lib/LICENSE`.  A redistributed libperfd
is MIT plus that notice.

Contributions: the project relies on being single-copyright-holder to
keep licensing decisions simple; outside contributions need a DCO
sign-off.
