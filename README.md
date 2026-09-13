# perfcached

Multithreaded cache daemon for Linux — **one node or a self-clustering
fleet**, same binary either way — plus client libraries (`libperfd` C
library, pure-PHP class). Built from the OpenSIPS `cachedb_perf`
module's proven core, but as its own process rather than a module, so
anything that speaks TCP can use it. Clusters via lazy pull-on-miss
self-healing, speaks an encrypted (Noise/libsodium) triple
dialect — binary frames for libraries, newline-delimited JSON-RPC for
scripts, and RESP2 so unmodified Redis clients (redis-cli, hiredis
apps, rtpengine) connect as if it were Redis.

**Status: daemon and clients complete (0.2.0)** — storage (WAL + RDB +
recovery), automatic cluster membership, store mode (pull-on-miss,
plus eager background full replication),
proxy mode (the capacity plane: placement, forwarded writes, the
coldest-first rebalancer with a TCP bulk plane), shard mode
(deterministic CRUSH-style ownership with automatic resharding), JSON
path verbs, the binary wire dialect, the RESP compatibility dialect
(the universal Redis KV command set; `SELECT n` maps onto the
collection named "n", so a RESP-serving deployment declares
`[collection 0]`…), admin verbs, ops packaging, and
all three clients below.

The master is a **control plane**, not a label: it owns a versioned
cluster map (identity, state and weight per node) published under a
monotonic term, staged and acknowledged before it takes effect, with a
deterministic standby holding a synchronized copy so a promotion is a
handover rather than a re-election.  Placement is computed from that
map — weighted rendezvous hashing in integers, so every node and every
client reaches the same answer bit-for-bit.

Remaining: client distribution from the map (built and tested, not
yet wired) and the rtpengine wire capture that settles the RESP hash
commands.  The OpenSIPS driver module (`cachedb_perfd` - thin glue
over libperfd, with its own README and timeout/policy knobs) is built
and documented on its PR branch.
Dependencies: libc, pthreads, libsodium. Linux only
(x86_64 / arm64 / arm32 / i386 — `tools/matrix.sh` builds and tests all
four via podman + qemu-user).

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
  standard, not this project's whims.

Tagged releases (`v0.2.0` up) are the states these promises are made
from; master between tags is development.

## Build and test

    make check       # every suite + the broken-locks canary
    make check-asan  # the same suite under ASan+UBSan
    make install     # daemon + perfcli + example config + systemd unit
    tools/matrix.sh  # the four-arch matrix (build host with podman)

CI runs the same spellings on every push, lint first: a GATING
`clang-tidy` stage (baseline ZERO - the 113-finding triage fixed 39
for real, two genuine bugs among them, and retired the noise with
written receipts), then the full suite natively and again under
ASan+UBSan.  A red lint stage stops the pipeline in minutes instead
of after the hour of suites.

## Running it

Every option lives, annotated, in
[contrib/perfcached.conf.example](contrib/perfcached.conf.example);
the snippets below are complete working configs.

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
                                 # (eager is a store mode: every node
                                 #  keeps a copy of every record)
    collections = sessions       # the exhaustive clustered set
    #advertise = 10.0.0.1        # only on multi-homed hosts

    [collection sessions]
    buckets_log2 = 18            # node-local SIZING only

Store mode pulls on a local miss and KEEPS the copy, so every node
converges on the working set.  `mode = eager` additionally sends every
write to every live peer as it lands - on the write path, fire-and-
forget, whatever the TTL: a 1 s key gets its copies too.  A background
sweep repairs what a push did not reach (a peer that was down, a lost
datagram), so in the steady state a write reaches each peer twice and
the second copy is refused as not newer.  Replicas are held off the
WAL: a record is durable where it was written, and a node that restarts
holds its own writes until the sweep refills the rest.

Applying those copies is ONE thread per node, while the sending side is
every worker in the fleet, so under enough write pressure a receiver
falls behind - and it falls behind QUIETLY.  It keeps heartbeating, it
stays a member, and its own client door stays fast; the only symptom is
a read of a key that exists on that node solely as a copy it has not
applied yet.  Past the receive buffer, datagrams are dropped and the
repair sweep is what puts those records back.  So an eager fleet under
overload is eventually consistent, with the sweep as the repair, not
synchronously replicated.  The `replica intake` card on the status page
is where that is visible - applied per second, the receive queue against
the buffer, and the drops - and the same figures are in
`stats.cluster` (`rx_applied_ps`, `rx_queue`, `rx_rcvbuf`,
`rx_drops_ps`) and in the metrics.  A queue that sits near the buffer is
a node at its apply limit; a nonzero drop rate is records already lost
and waiting on the sweep.

Nodes that die and come back rejoin by themselves; a partitioned
master steps down when it sees a bigger fleet.  Failure detection
runs on 1 Hz heartbeats with real margin - a master is presumed dead
after 8 s of silence, a peer after 10, so jitter is not death - beat
emission is watchdog-backed, and datagram ingest is fairness-bounded
so a migration burst cannot deafen membership.  Watch it settle:

    perfcli -p 6479 -a '...' -P stats
    # "cluster": { "node": 1, "role": "master", "peers_up": 2, ... }

### Proxy mode - the capacity plane

In store mode every node's ceiling is its own arena.  A proxy
collection instead keeps each key on exactly ONE node - placement by
free memory at write time, reads served through without storing,
writes forwarded to the holder, and a 10s rebalancer that levels the
fleet by live utilization (coldest records first, oversized ones over
a TCP bulk channel).  Fleet capacity ~= the SUM of the arenas:

    [cluster]
    multicast = 239.68.68.1:6480
    mode = proxy
    collections = blobs

    [collection blobs]
    buckets_log2 = 16

### Shard mode - deterministic ownership

A shard collection places each key on exactly ONE node chosen by
rendezvous hashing over the members' addresses (CRUSH-style): no
locator, no placement races, misses answered authoritatively in one
round trip, and counters serialized at the owner from any ingress.
Membership changes reshard automatically - only the moving keys
travel, and reads fall back to a broadcast during the move so nothing
misses mid-reshard:

    [cluster]
    multicast = 239.68.68.1:6480
    mode = shard
    collections = ids

    [collection ids]
    buckets_log2 = 16

**One cluster is one mode.** Mixing modes inside a single cluster is
rejected by design, not deferred: one membership whose members mean
different things per collection cannot be reasoned about during an
incident - the same node loss is "replicated, fine" for store and
"re-shard" for shard at once - and the rebalancer would be mixing
placement arithmetic across modes.  A deployment that genuinely needs
two modes runs TWO clusters on distinct multicast groups; a daemon can
join several.

Without `collections` the legacy per-collection `mode =` still parses
and warns, because nothing then checks that your peers agree.  New
deployments should declare the cluster form.

An erasure-coded mode (CEPH-pool-style k+m) was built and then removed
in 0.2.0.  Replication carries the loss tolerance
these workloads need, and it does so at a fraction of the read cost.

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
  reachable at all, which is why a node holding live data under real
  names used to report one empty database to every Redis-native
  monitor.  `stats` reports a `resp` block
  (connections, allow-list rejections, auth failures).  The door also
  serves the Redis observability surface - section-faithful `INFO`
  (commandstats included), `CLIENT LIST`/`SETNAME`, `SLOWLOG`, and the
  `CLUSTER` family (`SLOTS`/`SHARDS`/`KEYSLOT`/`NODES`) - so Grafana's
  redis-datasource and cluster-aware Redis clients work against it
  unmodified; `TIME`, `EXPIREAT`/`PEXPIREAT` and `MEMORY USAGE` round
  out the tooling set.
- `arena_cap_mb` lets the arena grow elastically under pressure - by
  2 MB group inside one address-space reservation, on the same tier as
  the initial commit; `reclaim_*` returns idle groups to the kernel.
- A reserved hugepage pool makes the arena's top tier deterministic -
  see [contrib/sysctl-perfcached.conf](contrib/sysctl-perfcached.conf),
  and read back `HugePages_Total` after applying: live hosts routinely
  under-deliver the reservation until memory is compacted.
- `perfcached -E -f <conf>` dumps the normalized effective config with
  secrets masked.

### Sizing

What a set of collections costs on a host, per node - in eager mode every
node holds everything, so the figure is per node, not per fleet.  The
page's "memory budget" card computes the same arithmetic from the
daemon's exact figures; this is how to do it before the daemon exists.

1. **Index.**  Each collection's table is carved at creation, never
   freed.  `buckets_log2` is 4..24 - the same range in a config file and
   at the `create` and `resize` verbs - but the cost is FLAT below 2^12
   and about 192 bytes a bucket above it, because segments are fixed at
   4,096 buckets and a table always carves whole ones:

   | buckets_log2 | buckets | index | per bucket |
   |---|---|---|---|
   | 4 .. 12 | 16 .. 4,096 | 1.5 MB | - |
   | 16 | 65,536 | 12.8 MB | 204 B |
   | 18 | 262,144 | 48.8 MB | 195 B |
   | 20 | 1,048,576 | 192.8 MB | 193 B |
   | 24 | 16,777,216 | 3.0 GB | 192 B |

   So anything below 12 buys nothing over 12, and the top of the range
   is measured in gigabytes - all of it carved before a single record.
   A table grows by splitting once it passes four entries per bucket,
   and growth adds index.  A create or a resize whose index would not
   fit under the ceiling in item 4 is refused, naming both figures.
2. **Records.**  A record is its key, its value and a 28-byte header,
   rounded UP to a cell class.  The classes step by 1.5 and 1.33 from
   64 bytes to 64 KB (64, 96, 128, 192, 256, 384, 512, 768, 1 K, 1.5 K,
   2 K, 3 K, 4 K, 6 K, 8 K, 12 K, 16 K, 24 K, 32 K, 48 K, 64 K), so the
   rounding is anywhere from 0 to 50 %.  A measured mix - 100,000 records
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
   within a few 2 MB groups.  The arena's pages are counted in RSS when the tier is
   transparent huge pages (the default when no hugetlb pool exists); on a
   hugetlb pool they are in `HugetlbPages` instead and RSS shows only the
   daemon itself.

Read it back on the page: `held` and its parts (structure, class
chunks, warm free), the budget card, and `resident` from the kernel.

## Measured

**The client is `redis-benchmark`** — Redis's own tool, unmodified, not
anything of ours.  It drives both servers with the same workload:

    redis-benchmark ──native RESP──> redis-server 8.0.2
    redis-benchmark ──native RESP──> perfcached's RESP door

Nothing is translated or proxied on either path.  perfcached speaks RESP2
itself — that door exists so unmodified Redis clients work — so the
client cannot tell which server it reached, and neither server is doing
anything special to be measured.  Both run on the same host.

`build=2db7baf`, 16-vCPU Debian 13, 20k keys x 200 B, 100k x pipeline
requests per cell (capped at 2M) so every cell runs for over a second,
median of 3 runs per cell.  Full table and method:
[bench/respbench.sh](bench/respbench.sh), raw rows in
[bench/results/respbench.tsv](bench/results/respbench.tsv) - the 50-client
cells this page quotes, from the one run that produced it.  Widen the
sweep with `CLIENTS=` and `PIPES=` if you want the rest.

### One server against one server

Like for like first: **one** perfcached, no cluster, against Redis.  The
cluster modes are compared separately below.

**50 clients, no pipelining:**

| | SET/s | GET/s | vs redis | SET p99 | GET p99 | tail |
|---|---|---|---|---|---|---|
| **redis-server 8.0.2** | 65,660 | 64,103 | — | 0.89 ms | 0.94 ms | — |
| perfcached, 1 node | 64,935 | 63,492 | x0.99 / x0.99 | 0.60 ms | 0.62 ms | 1.5x |

`vs redis` is SET / GET throughput; `tail` is how many times tighter the
SET p99 is.  Parity on throughput is the expected result here: with one
request in flight per connection both servers are waiting on the round
trip rather than working.  The tail is where they differ - 0.60 ms
against 0.89 ms at p99 - because four workers have three idle ones to
answer with while a single thread is busy.  (Earlier runs of this cell
read x0.98 / x0.96 and x1.08 / x1.10: it straddles parity run to run,
which is the point.)

**50 clients, pipeline 64** — where the difference is real:

| | SET/s | GET/s | vs redis | SET p99 | GET p99 | tail |
|---|---|---|---|---|---|---|
| **redis-server 8.0.2** | 845,666 | 1,087,548 | — | 6.02 ms | 3.69 ms | — |
| perfcached, 1 node | **1,860,465** | 1,819,836 | **x2.20 / x1.67** | **2.38 ms** | 1.35 ms | **2.5x** |

**+120% on SET at a 2.5x tighter p99**, and the reason is not
subtle: Redis is single-threaded, perfcached runs four workers.  Give
one core's worth of work and the numbers converge; give enough
concurrency to fill four and they do not.

An earlier version of this table quoted 934,878 and 1,786,286 GET/s
for these two pipelined cells, and those exact figures had come out of
two runs on different builds.  That was the harness, not repeatability:
redis-benchmark times a run in whole milliseconds, and 100k requests at
1.8M/s is a 56 ms run, so every figure it could print sat on a ~2%
grid.  Cells now run for over a second.

### Across a real network, client on another host

Everything above shares one machine, which flatters both servers and
costs perfcached more than Redis: its workers compete with the client
for cores where a single-threaded Redis does not.  So the same
comparison, run properly - `redis-benchmark` on one host, both servers
on another, a real NIC between them, MTU 9000 verified end to end with
`ping -M do -s 8972` before trusting it.  Same build (`2db7baf`),
median of 3, arms alternated so drift cannot favour either, 100k x
pipeline requests per cell capped at 2M - the method as a script is
[bench/xhostbench.sh](bench/xhostbench.sh), raw rows in
[bench/results/xhostbench.tsv](bench/results/xhostbench.tsv).

**50 clients, no pipelining:**

| | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| **redis-server 8.0.2** | 46,795 | 48,239 | — | 1.42 ms | 1.25 ms |
| perfcached, 1 node | 52,274 | 48,544 | x1.12 / x1.01 | 0.95 ms | 0.90 ms |

**50 clients, pipeline 64:**

| | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| **redis-server 8.0.2** | 686,813 | 928,074 | — | 6.22 ms | 4.62 ms |
| perfcached, 1 node | **1,526,718** | **1,439,885** | **x2.22 / x1.55** | **3.66 ms** | **2.21 ms** |

Two things to take from this rather than from the loopback tables.

**Loopback and the wire now agree on SET** - x2.20 there, x2.22 here -
and the wire costs perfcached a little more than Redis on GET, x1.67
there against x1.55 here.  Both are honest about their rig; this is the
one a deployment gets.

**Loopback understates it at depth 1**, where sharing a host was costing
perfcached real work: x0.99 / x0.99 on one machine becomes x1.12 / x1.01
across two, with a tighter tail on both operations (0.95 ms against
1.42 ms at the SET p99).

One caveat that belonged with the earlier version of these numbers has
mostly gone: perfcached's three pipeline-64 SET reps were once
1,260,639 / 1,948,260 / 1,327,575, a 55% spread, against 0.2% for Redis.
With cells that run for seconds instead of a 100k-request burst they
are 1,526,718 / 1,468,429 / 1,609,010, a 9% spread, against 3.5% for
Redis (686,813 / 707,965 / 683,994).  Most of that spread was the
measurement, not the server.  Take the medians all the same.

Reproduce it with servers on one host and the client on another:

    # on the server host - a non-loopback RESP listener REFUSES to
    # start without resp_allow, by design
    [listen]
    resp_allow = 10.0.0.0/8
    resp = <server-ip>:17910

    # on the client host, after checking the path MTU:
    ping -M do -s 8972 -c 3 <server-ip>
    redis-benchmark -h <server-ip> -p 17910 -t set,get \
        -n 300000 -c 50 -P 64 -r 20000 -d 200 --csv

### Reproducing every table above, from a clean machine

There are two routes.  **If you just want the numbers, use the container
one** - it needs a container runtime and nothing else at all:

    bench/containerbench.sh

That builds its own image, pulls its own Redis, creates its own network,
runs both phases and tears everything down.  No compiler, no libsodium,
no redis-server, no perfcached binary on the host.  It prefers `podman`
(which builds without a daemon), then `nerdctl`, then `docker`, and it
probes each with a real build before choosing - `nerdctl` answers `info`
happily and then fails every build if `buildkitd` is not running.

Verified on both podman and docker: the cluster tables below were
produced by this harness under **podman** on a 16-vCPU host, and an
earlier run of the same tables under docker on an 8-vCPU one had the
same shape.

The rest of this section is the **host** route, which is what produced
the single-node tables above (the cluster ones come from the container
harness).  Nothing in it is pre-baked either: the harness starts
its own Redis, starts its own perfcached fleet, drives both with the
same client, and tears everything down.  On Debian 13 / Ubuntu:

    # 1. a container runtime, and nothing else
    apt install -y podman        # or docker, or nerdctl + buildkitd
    git rev-parse --short HEAD   # the image is stamped with this

    # 2. run it.  REPS=3 is what the tables above used.
    REPS=3 bench/respbench.sh

    # 3. read it
    cat /var/tmp/respbench/results.tsv

**Nothing is installed on the host.** Since 2026-09-12 the harness runs
the perfcached fleet from an image built out of this tree, Redis from
`docker.io/library/redis:8`, and both load generators (`redis-benchmark`
and `perfcli`) from those same two images, all on a private bench
network. There is no `apt install redis-server` step any more, and the
two positional arguments respbench used to take - a perfcached and a
perfcli path - are gone with it: the binaries come from the image, and
the revision stamped into the results is read back out of that image
rather than off the host.

The runtime is picked by probing each of `podman`, `nerdctl` and
`docker` with a real one-step build, so a runtime that is installed but
cannot build (no `buildkitd`, an AppArmor profile that will not load) is
reported with the fix rather than failing later and blaming something
else. Override with `RUNTIME=docker`.

**Budget an hour** with `REPS=3`.  Eleven arms now — redis, two
single-node arms, and each of the four modes measured twice, once
through the node holding the data and once through a node holding none
— at ten cells each, three runs per cell, plus a fleet start and stop
per arm and a 32-second wait per mode for the reshard grace to expire.
(The tables above were produced before the cold-entry arms existed, when
it was 25-40 minutes; the full run has not been re-timed since.)

Drop to `REPS=1` for a smoke run, but do not compare arms with it — see
the note above about the noise floor.  A quicker subset:

    CLIENTS="50" PIPES="1 16" REPS=1 bench/respbench.sh
    MODEARMS="store shard" REPS=3 bench/respbench.sh

**What it needs from the machine.** A working container runtime, the
`10.96.0.0/24` subnet free for the bench network (override with
`SUBNET=`), and roughly 2 GB free for three 512 MB arenas. It no longer
needs host ports or the `127.0.42.1-3` loopback addresses: each node has
its own address on the bench network instead.  Run it on an otherwise idle box: this rig's run-to-run
spread on a single unchanged arm is ~11%, and a busy machine makes that
much worse.

**The output.** `results.tsv` is one row per arm/clients/pipeline cell,
with a header line naming the build, host, date and workload — the
harness refuses to run at all against a binary that cannot name itself,
so a results file always says what produced it.  Column order is
`arm, clients, pipeline, set_rps, get_rps, set_p50ms, get_p50ms,
set_p99ms, get_p99ms`.  Every number in the tables above is one of those
cells; the ratio columns are that cell divided by the `redis` row at the
same clients and pipeline depth.

**Expect different numbers.**  These are loopback figures on a 16-vCPU
VM.  What should reproduce is the *shape*: the arms converging at
pipeline 1, perfcached pulling ahead as depth grows, store and proxy
staying in one band, eager's SET falling away from them at depth because
it replicates on the write path, and shard falling well behind any
client that cannot compute an owner.  If your shape differs, that is
worth more than the absolute values.

### The four cluster modes, same fleet, same client

**The first row is real Redis, not a perfcached mode.** The harness
starts its own `redis:8` container (persistence off), measures it, stops
it, and then brings up a three-node perfcached fleet for each mode.

`build=76fc29b` (v0.3.0-rc34), taken 2026-09-12 - 16-vCPU Debian 13,
podman, `redis:8` (8.10.1), 20k keys x 200 B, 8 workers, `route=1`, 50
clients, median of 3 runs per cell, and every cell sized so that each
pass runs for at least 10 seconds.  That last part matters: with
`--threads`, redis-benchmark stops only on its 250 ms progress timer, so
a cell that finishes in 1.25-2.5 s - which is what a 4M-request cell
took here - prints a rate up to 25% low.  The tables this page carried
until 2026-09-12 were built from cells that short, and every figure in
them was understated by up to one quarter-second step.  Raw rows:
[bench/results/containerbench-76fc29b.tsv](bench/results/containerbench-76fc29b.tsv),
with each cell's request count and pass times beside them in
[bench/results/containerbench-76fc29b-cells.tsv](bench/results/containerbench-76fc29b-cells.tsv).
The 2026-09-04 sweep those older figures came from - `build=19a077b`,
the same daemon source as `2db7baf`, walking more client counts and the
JSON dialect - is still
[bench/results/containerbench.tsv](bench/results/containerbench.tsv).

**There are two client stories here and they are not interchangeable.**
The RESP client used below *does not route* — it dials one node, so
every key that does not belong to that node is a forward.  A
cluster-aware client computes the owner and talks to it directly.  That
difference dominates every number below, and for shard it is worth more
than an order of magnitude.

> **Since 2026-08-30 a RESP client CAN route.**  Ownership moved to the
> Redis slot (`crc16(key) % 16384`), and the door answers `CLUSTER
> SLOTS`, `CLUSTER SHARDS` and `CLUSTER KEYSLOT`, so any cluster-aware
> Redis client places keys itself.  **The figures below were taken
> without one** - `redis-benchmark` does not route - so they show the
> non-routing path.  Read them as the floor, not the ceiling.

#### What a Redis client gets

Driven through node 1's RESP door by `redis-benchmark`, which is the
honest shape of a Redis migration.

**50 clients, pipeline 16** (redis: 529,193 SET / 599,827 GET):

| mode | SET/s | GET/s | vs redis | SET p99 |
|---|---|---|---|---|
| store | 2,089,414 | 2,543,422 | x3.95 / x4.24 | 1.62 ms |
| eager | 1,520,596 | 2,661,907 | x2.87 / x4.44 | 1.66 ms |
| proxy | 1,887,235 | 2,490,411 | x3.57 / x4.15 | 1.62 ms |
| **shard** | **242,219** | **218,962** | **x0.46 / x0.37** | 1.95 ms |

**50 clients, pipeline 64** (redis: 858,531 SET / 981,260 GET):

| mode | SET/s | GET/s | vs redis | SET p99 |
|---|---|---|---|---|
| store | 2,504,048 | 3,187,417 | x2.92 / x3.25 | 3.46 ms |
| eager | 1,866,618 | 2,829,394 | x2.17 / x2.88 | 4.81 ms |
| proxy | 2,261,537 | 2,596,016 | x2.63 / x2.65 | 3.60 ms |
| **shard** | **161,323** | **147,869** | **x0.19 / x0.15** | 2.97 ms |

**Reads stay in one band; eager's writes no longer do.**  On GET the
three placement modes span x4.15-x4.44 at pipeline 16 and x2.65-x3.25 at
64 - close enough, on a rig whose run-to-run spread is several percent,
to read as one band rather than a ranking.  On SET eager now trails
store by about a quarter (1.52M against 2.09M at depth 16, 1.87M against
2.50M at 64), and that is a change made after the 2026-09-04 figures
this page used to show: eager pushes every record to its peers **on the
write path** instead of leaving it to the sweep, so a write pays for its
own replication as it happens.  What it buys is on the read side - every
node stays hot, and eager's GET is the fastest of the three at pipeline
16 - which is the trade the mode is for.

**shard answers now, and on 2026-09-04 it did not.**  Then, past a few
hundred requests in flight, every cell came back `ERR holder timed out`:
the forward was routed, sent and parked, and the answer missed its
deadline.  Today the same cells answer - x0.46 / x0.37 at pipeline 16,
x0.19 / x0.15 at 64, at a p99 of 2-3 ms - so what was a wall is now a
slope.  It is still much the worst way to drive this mode, for the
unchanged reason: a client that cannot compute an owner forwards nearly
every key, and the fleet has to answer all of them.  Both refusals still
exist for when it goes further: a forward that cannot be parked because
the table is full (`[cluster] max_pending`, 8192 by default) is refused
with the retryable `TRYAGAIN cluster busy, retry`, and one that is
parked but not answered in time is `ERR holder timed out`.

#### What a cluster-aware client gets

Same fleet, same daemon, same moment — `natbench` over `libperfd` with
`opts.route_keys`, binary dialect.

**50 clients, pipeline 64:**

| mode | SET/s | GET/s | vs redis | SET p99 |
|---|---|---|---|---|
| store | 3,356,422 | 4,202,499 | x3.91 / x4.28 | 2.11 ms |
| eager | 1,846,908 | 4,178,683 | x2.15 / x4.26 | 4.66 ms |
| proxy | 2,106,369 | 3,788,786 | x2.45 / x3.86 | 4.36 ms |
| **shard** | **3,296,256** | **4,188,213** | **x3.84 / x4.27** | 2.43 ms |

**The mode a Redis client drives at x0.19 comes within 2% of store on
both operations once the client can route.**  That is the whole result:
shard was never the slow mode, it was the mode whose client could not
compute an owner.  Proxy trails on SET, and since 2026-09-05 so does
eager - it writes to every peer on the write path, which is why its SET
is about half of store's here while its GET is level with it.

Two things to keep straight about that table.  The `vs redis` column
compares a fleet-aware client against a single-node one, so it is a
deployment comparison rather than a wire comparison.  And routing buys
two separate things: it removes the forward hop, and it spreads
connections across the fleet.  Measured apart on a 3-node fleet, 50
connections at depth 32 (`bench/routepair.sh`, build `2db7baf`, median
of 3, raw rows in `bench/results/routepair.tsv`) — store fetches
nothing either way after its first pass and still gains **1.4x** purely
from spreading (1,750,568 -> 2,447,670 GET/s), while shard gains
**9.4x** (275,312 -> 2,579,164 GET/s), with the daemons' own pull
counter going from ~940,000 pulls per 5-second run un-routed to zero
routed.  So shard's figure is that 1.4x times roughly 7x from the hop.

**Per-key routing was unreachable from libperfd's async API until
`e0a0a83`** — an async handle never learned the fleet, so
`opts.route_keys` had nothing to route with.  Every shard number this
project published before that date is the un-routed path.

#### Reading through a node that holds nothing

This is a **RESP-client scenario by construction.**  A routed client
reads from each key's owner and never lands on a node that lacks the
key, so the case does not arise for it — and emptying a node to
manufacture it merely deletes part of the keyspace, which a routed arm
then reports as a fast, wrong number.

**50 clients, pipeline 64, read through a node holding nothing:**

| mode | GET/s | GET p99 |
|---|---|---|
| store | 2,750,727 | 1.02 ms |
| eager | 2,921,250 | 1.06 ms |
| shard | 58,086 | 33.60 ms |
| proxy | 79,850 | 47.46 ms |

Store and eager are not really cold: store pulls the key and keeps it,
eager already replicated it, so both stop being cold almost at once.
Between the two modes that genuinely have to go and get it, **proxy
carries the worse tail at depth 64** - 80k GET/s at a 47 ms p99 against
shard's 58k at 34 ms - while at pipeline 16 proxy has the throughput
(106k against 95k) and shard has the tighter tail again (5.4 ms against
10.2 ms).  Shard computes the owner and unicasts to exactly one node;
proxy consults a locator and broadcasts when that misses.

Two earlier readings of this cell are worth naming, because neither
survived.  On 2026-09-04 shard read 33k GET/s at a **2,034 ms** p99,
which this page called unexplained: it does not reproduce, and two runs
on 2026-09-11/12 put it at 33 and 34 ms.  Proxy's tail moved the other
way over the same period, 8.7 ms to 47 ms, and that is not explained
either - it is the one number here that got worse.

One caveat if you reproduce this: for 30 seconds after a membership
change (`SHARD_GRACE_S`) a shard miss does not answer authoritatively,
it retries once as a broadcast, because the data may still sit on the
old owner.  Measured inside that window shard looks *slower*.  The
harness waits it out; a hand-rolled test that starts a fleet and
measures immediately will get the wrong answer, as two of ours did.

### If you are putting a Redis client in front of shard mode, read this

Shard is a mode this project relies on — it is the one with computed,
deterministic ownership — so its number above is a headline result and
not a footnote.  Through a RESP client it is ahead of Redis at low
concurrency (x1.96 SET / x1.79 GET at 50 clients, pipeline 1; the
2026-09-04 sweep, which walked more client counts, read x1.33 at 16
clients), and falls away as requests in flight grow: x0.46 at pipeline
16, x0.19 at 64.  On 2026-09-04 it stopped answering entirely at those
depths; now it answers, slowly.

The cause is not shard mode.  The client used here has no cluster map,
so it cannot compute the owner and forwards most keys.  Every other mode
places or replicates rather than computing an owner, so a wrong guess
costs less.  (A map is now available to RESP clients — see the note
above — but these numbers were taken without one.)

**Two distinct failures live behind that, and they are now told apart.**
A forward that cannot park is refused with `TRYAGAIN cluster busy,
retry` — a retryable backpressure signal, and the parked-request table
is `[cluster] max_pending`, 8192 by default rather than the fixed 1024
it once was.  A forward that was sent and parked but whose answer never
came back inside its deadline is `ERR holder timed out`, which is what
deep pipelining produced on 2026-09-04 once the table was big enough;
on the 2026-09-12 run those same cells answered.  That one is
deliberately **not** retryable: the holder may have stored it, so
retrying is safe for SET and not for INCR.

**What to do about it**, in order:

- Use `libperfd` with `opts.route_keys`, which learns the fleet and
  computes the same owner hash the daemon does.  It parks no slot, so
  the ceiling does not exist for it.  Measured on a 3-node fleet, 50
  connections at depth 32: **275,312 -> 2,579,164 GET/s, a 9.4x
  gain**, with the daemons' pull counter falling to zero and shard
  landing 5% above routed store.

  Part of that gain is fleet utilisation rather than the removed hop —
  decomposed in the routed table above.  And **on the async API this
  only works from `e0a0a83`**: before it, an async handle never learned
  the fleet, so `route_keys` had nothing to route with.  Async callers
  open one handle per node and pick with `perfd_owner_of()`; the
  library will not open connections an async caller would never poll,
  because that caller drives one fd per handle.
- If the client must be a Redis one, prefer `store` - or `eager` when
  every node must stay hot - and NOT proxy.  None of the three has an
  owner to guess wrong, and all sit in one band read through a node
  that holds the data; but a non-routing client lands on arbitrary
  nodes, and read through a node holding nothing, store and eager stay
  in the millions while proxy drops to 79,850 GET/s at a 47 ms p99
  (the table above) - a locator miss there is a broadcast.  Choose
  proxy for what it is for - fleet capacity ~= the sum of the arenas -
  and accept the cold-read cost knowingly.
- If it must be shard AND a Redis client, keep pipeline depth modest.
  The wall is in-flight requests, not request rate.

### The rest of what this does not say

**Most of these tables are one host, loopback — and for reads that is
not a small caveat.**  The cross-host section above measures the same
comparison over a real NIC and is the number to quote; this explains
why the two differ.  Loopback has a 64 KB MTU.  A pipelined batch of GET responses is one
segment there and nine on an ordinary 1500-byte network, and read
throughput tracks that directly.  Same host, same binary, same
container, only the MTU changed:

| MTU | SET/s | GET/s | GET/SET |
|---|---|---|---|
| 1500 (bridge) | 1,351,784 | 730,161 | 0.54 |
| 9000 (bridge) | 1,370,301 | 793,905 | 0.58 |
| 65536 (loopback) | 1,852,444 | 1,786,286 | 0.96 |

**Writes barely move; reads nearly triple.**  So the GET figures above
are a best case that a real 1500-byte network does not reproduce, and a
deployment that can raise its MTU should.  Found by running the
container harness on someone else's machine; nothing on loopback would
ever have shown it.

Part of what used to sit under this heading as "not yet understood" was
not the wire at all.  Every GET allocated and freed a buffer it did not
need - the record was already copied out into a per-thread scratch, and
the verb layer then malloc'd a second one, copied again, wrote it to the
socket and freed it.  Removing that (`78b122b`) more than doubled reads
where the *server* is the bottleneck: on an 8-vCPU host across a bridge,
same harness and configuration either side of the commit, GET went
483,246 -> 1,099,253 (+127%) with its p99 5.4x tighter, while Redis
moved 2.0%.  It changed nothing on loopback, where the client is the
limit and freed server CPU has nowhere to go - which is why it hid for
so long.

A read/write gap remains at lower MTU (0.54 and 0.58 against 0.96 on
loopback) and that part still is not fully explained.  Segmentation is
the obvious candidate and is clearly not all of it.

**Numbers rot.** Every harness stamps the binary's revision into its
results and refuses to measure a build that cannot name itself; quote
from a run, not from here.

## perfcli

The redis-cli analogue.  Word commands mirror the verb set; `-a` runs
the Noise handshake (client principal) for encrypted listeners:

    perfcli -p 6479 set sessions user:17 "some value" 300
    perfcli -a 's3cret' get sessions user:17
    printf 'ping\nstats\n' | perfcli -q          # pipe mode
    perfcli -j '{"method":"keys","params":{"col":"sessions","match":"user:*"}}'

`help` inside the REPL lists everything; jset takes raw JSON.  The
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

## perfdump

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

## Collections at runtime

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
a target the splitter would immediately grow back is refused.  A rename
is one pointer swap, which makes it the atomic cutover a restore wants:
load a dump into a second collection with `perfload --collection-map`,
verify it, rename it into place, drop the old one.

This is off by default.  Set `[daemon] allow_create = yes` on the nodes
where a client may do it: a driver that creates on a miss turns a typo
into a second, empty collection and an operator into someone whose cache
"lost everything".  The setting gates who may ORIGINATE a create, not
what a node accepts from the fleet, so a create made anywhere reaches
every member whatever their own setting, and turning it on does not need
a fleet restart.

A created collection takes the cluster's mode: a fleet is one mode over
one collection set.  It is remembered in `[daemon] state_dir` and comes
back after a restart, it rides the WAL as a record of its own so a
replay lands records in the collections that existed when they were
written, and it is announced to every live peer, with the whole set
re-announced every few seconds so a node that was down through the
change picks it up when it returns.  Without a `state_dir` a created
collection is ephemeral, and the daemon says so.

## perfload

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

## Open connections, running totals, and resetting them

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

## libperfd

**Cluster-aware (S34).** Set `opts.spares` and the library learns the
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

**Per-key routing (S35).**  Add `opts.route_keys = 1` and each request
goes to the node that should hold its key, so the daemon's forward hop
disappears - measured at 0 forwards for a load that made an unrouted
client cause 133.  It applies to `shard` (the owner is computable) and
`store` (hashing a key to one node makes the client a de-facto single
writer for it, which is what stops concurrent writers forking a key);
`proxy` is not routed.  It is never load-bearing: the daemon
re-checks ownership and forwards a wrong guess, so a stale view costs a
hop, not correctness.  Off by default, like the spreading policy.

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

## Perfcached.php

The single-file pure-PHP client (ext-sodium + ext-json, both bundled
since PHP 7.2) - same contract, same Noise handshake, same secret-list
rotation:

    require 'Perfcached.php';
    $pc = new Perfcached('10.0.0.1', 6479,
        ['secrets' => ['new-secret', 'old-secret']]);
    $pc->set('sessions', 'user:17', $blob, 300);
    $v = $pc->get('sessions', 'user:17');        // null on miss

## Layout

    src/        daemon
    cli/        perfcli
    lib/        libperfd (perfd.h + perfd.c -> libperfd.a)
    src/core/   vendored htable/arena core (see tools/sync-core.sh)
    lang/php/   Perfcached.php single-file pure-PHP client
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
