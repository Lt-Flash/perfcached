# perfcached vs Redis in containers — the reproducible matrix

> **The METHOD here is current; the NUMBERS are from build `5112dfa`
> (2026-08-26).** This is a reproduction manual rather than a results
> page, which is why it is kept as-is - the procedure for standing the
> comparison up in containers has not changed. Anything you intend to
> quote should come from a fresh run, or from `bench/results/*.tsv`,
> which are stamped with the build that produced them.

A step-by-step manual for standing up the whole comparison on ONE
machine in four containers — a three-node perfcached cluster plus
Redis 8 — building perfcached as an Alpine (musl) image and running it
under **containerd (nerdctl)** or **docker**.  Everything a third
party needs is in this directory: the Containerfile, the bring-up
script, the measurement battery, and the client tools baked into the
image.

Being single-machine, absolute latencies flatter everybody (loopback
and veth, no real NIC); the point is the *relative* behavior — and the
effects the matrix exists to show (a 65 ms KEYS stall, a 3.5x
durability gap) are orders of magnitude above the wire noise.

## Topology

    ┌─────────────────────── bridge `pcnet` 10.99.0.0/24 ───────────────────────┐
    │  ┌─ perfcached cluster · multicast 239.255.99.9:7191 ─┐                   │
    │  │  pcnode1       pcnode2       pcnode3               │   pcredis         │
    │  │  10.99.0.11    10.99.0.12    10.99.0.13            │   10.99.0.20      │
    │  │  automatic membership - no ids, no peer lists      │   redis:8-alpine  │
    │  └────────────────────────────────────────────────────┘                   │
    │  (durability arm adds pcredis-aof 10.99.0.21, --appendonly yes -          │
    │   also outside the cluster)                                               │
    └───────────────────────────────────────────────────────────────────────────┘

The three node configs differ **only by the advertise IP** — there are
no node ids and no peer lists.  The user-defined bridge is a plain
Linux bridge, so it carries the multicast membership traffic under
both runtimes.  The Redis containers share the bridge but are **not
cluster members** — they never see the membership plane (and could not
join it: joining takes the cluster secret); they exist purely as the
comparison target the bench client talks RESP to.  Every perfcached
node serves two collections:

- **`b` — store mode with pull**: a local miss fetches the value from
  a peer and KEEPS the copy (replicate-on-demand; every node converges
  on the working set; warm reads never leave the node).
- **`px` — proxy mode**: every key lives on exactly ONE node
  (placement by free memory at write time), reads are served through
  without storing, writes forward to the holder, and the ~10 s
  rebalancer levels the fleet by live utilization — fleet capacity is
  the sum of the arenas.

## 1. Prerequisites

- Linux host, x86_64, a few GB free RAM (3 × 128 MB arenas + slack).
- Either **docker**, or **containerd + nerdctl**.  For containerd
  without buildkitd, build with **podman** and load the archive into
  containerd (shown below).
- `git`, `python3` on the host (the convergence checks parse JSON).

## 2. Build the image

From the repo root ([bench/Containerfile.debian](Containerfile.debian)
is a two-stage build: `debian:13-slim` + build-essential +
libsodium-dev compiles the daemon, perfcli, natbench and the `pcbench`
dual-protocol client; the runtime stage is debian-slim + libsodium23 +
python3-minimal and carries all the bench tools, so every measurement
runs INSIDE the container):

    docker build --platform linux/amd64 -f bench/Containerfile.debian \
        --build-arg REV=$(git rev-parse --short HEAD) \
        -t perfcached:bench .

**Why Debian and not Alpine**, at +48.5 MB (53.5 -> 102 MB): musl's
allocator serialises libperfd's per-reply malloc on the JSON path, so a
musl-built client peaks at TWO threads and then goes backwards. Same
host, same network, same daemon, json GET at depth 16 over 64
connections:

| threads | alpine | debian |
|---|---|---|
| 1 | 217,391 | 339,555 |
| 2 | 326,728 | 665,145 |
| 4 | 111,112 | 934,374 |
| 8 | 42,104 | 905,550 |

21.5x at 8 threads, and nothing to do with the dialect, the wire or the
daemon - but it silently turned every containerised JSON figure into a
measurement of the allocator. The binary dialect is far less exposed
(1.4x -> 2.9x) because it reads a length header instead of allocating
per reply.

[bench/Containerfile.alpine](Containerfile.alpine) is still there and
still builds: musl is a supported target and the portability check
wants it. It is simply not what we benchmark on. To use it:

    CONTAINERFILE=bench/Containerfile.alpine bench/containerbench.sh

containerd hosts without buildkitd build with podman and hand the
image over:

    podman build --platform linux/amd64 -f bench/Containerfile.debian \
        --build-arg REV=$(git rev-parse --short HEAD) \
        -t perfcached:bench .
    podman save --format oci-archive -o /var/tmp/perfcached-bench.oci \
        localhost/perfcached:bench
    nerdctl load -i /var/tmp/perfcached-bench.oci
    nerdctl tag localhost/perfcached:bench perfcached:bench

The `tag` line matters: podman names the loaded image
`localhost/perfcached:bench`, and a bare `perfcached:bench` would
otherwise send nerdctl to Docker Hub instead of the local store.

Pin `--platform` always: a poisoned multi-arch cache once produced an
arm/v7 image with x86 binaries inside.  Smoke-test the result:

    nerdctl run --rm perfcached:bench -V      # or docker run

Container note: the runtime's default RLIMIT_MEMLOCK (64 KB) makes the
arena's mlock fail into a "continuing unpinned" warning.  The bring-up
script passes `--ulimit memlock=-1:-1` so the arenas stay pinned, the
way the systemd unit's LimitMEMLOCK does on a host install.

## 3. Pull Redis 8

    nerdctl pull redis:8-alpine               # or docker pull

## 4. Start the topology

    bench/containers-up.sh            # 128 MB arenas, in-memory only
    bench/containers-up.sh 128 wal    # + WAL/RDB per node + pcredis-aof

The script autodetects the runtime (`RUNTIME=docker` overrides),
creates `pcnet`, writes the three configs to `/var/tmp/pccluster/`,
starts the containers, then measures and prints **membership
convergence**: the elapsed time until every node reports
`peers_up=2` with an assigned id and an elected master.  Nothing was
configured to make that happen — the nodes discover each other over
multicast, the highest address founds as master, ids are assigned at
join.  Nodes can start in any order; a killed node rejoins by itself.

## 5. Run the matrix

    bench/run-matrix.sh               # results land in bench/results-*.txt
    RUN_RECOVERY=1 bench/run-matrix.sh   # + the kill -9 recovery proof

What each leg measures, in order:

| leg | what it shows |
|---|---|
| **store latency/throughput** | GET/SET RTT at depth 1 (p50/p99/p999 histograms) and pipelined ops/s (8 conns × depth 32, plus 90/10 mixed) on collection `b`, then the identical shapes against Redis through the same client binary (`pcbench` speaks both wires — no client-side bias). perfcached legs run on pcnode1's loopback; Redis legs cross the bridge veth (~+10–20 µs on a baseline, noted so nobody has to guess). |
| **CPU under load** | `docker/nerdctl stats` sampled five times during a 30 s 90/10 run: per-container CPU% and RSS — what a fixed workload actually costs each server. |
| **KEYS/SCAN stall probes** | The production outage mechanism in miniature: one connection samples GET/SET RTTs at ~1 ms cadence while a second fires a full KEYS or SCAN storm over the 200k keyspace. A server that executes the storm on its only thread (Redis) stalls *every* sampled op for the storm's duration; perfcached's seqlock walk on one worker must stay flat. |
| **store-mode pull convergence** | pcnode2 — cold, zero entries — reads the whole keyspace: the first pass pays miss→pull→keep, the second is all-local. The entry census before/after shows the working set replicating on demand. |
| **proxy spread + rebalancer convergence** | 200k × 256 B written into `px` through ONE ingress node, then the per-node entry counts sampled every 10 s until stable: how placement spreads at write time and how fast the utilization-leveling rebalancer (coldest-first victims, gathered datagrams, TCP bulk channel for oversized records) finishes the job. The sum must stay exactly 200,000 — stub-first migration loses nothing. Then GET through the ingress and through a non-holder, pricing the locator-unicast pull hop. |
| **memory census** | Arena total/used/live plus per-collection entries from `stats`, Redis `INFO memory`, and container RSS from the runtime — data efficiency vs footprint policy. |
| **durability legs** (wal topology) | SET at depth 1 and pipelined, fsync=everysec WAL vs Redis `--appendonly yes` (appendfsync everysec) — equal guarantees, measured cost. With `RUN_RECOVERY=1`: kill -9 a node holding the dataset, restart, count entries and time the recovery (RDB load + WAL tail replay). |

## 6. Both modes, explicitly

The matrix exercises both cluster modes side by side on the same
daemons: `b` answers the read-scaling question (how a cold replica
warms, what warm local reads cost), `px` answers the capacity question
(how 200k keys written through one node end up level across three, and
what a read through a non-holder costs).  There is nothing to switch —
mode is a per-collection setting, and both collections live in every
node simultaneously.

## 7. Results

Measured **2026-08-26** on a 16-vCPU build host: the Alpine
(musl) image of this repo, nerdctl/containerd on a CNI bridge, against
**redis:8-alpine (8.10.1, jemalloc)** — matrix passes across the
day's revisions as fixes landed (final daemon rev measured:
`5112dfa`; the plain and `wal` topologies both ran).  Each table's
numbers come from a single pass.  (An earlier debian-image / redis:7
generation is retired: its Redis storm magnitudes were inflated by a
probe flaw fixed since — storm and sampler shared one Python GIL — so
the numbers BELOW are the honest ones.)

**Membership**: three cold containers self-assembled in **2.3–2.8 s**
— ids assigned, master elected, `peers_up=2` everywhere, from nothing
but a shared multicast group and secret.

**Proxy placement** (200k × 256 B through ONE ingress, clean fleet):
spread **66,029 / 63,633 / 70,338 — sum exactly 200,000, stable from
t=0**.  Write-time placement alone landed near-perfect thirds inside
the rebalancer's dead-band: nothing needed moving.  After the 220k
re-write legs the spread is **byte-identical and the sum still
exactly 200,000** — a re-write whose locator entry was lost probes
the fleet before placement, so it forwards to the true holder instead
of minting a duplicate (one extra RTT on precisely those ops; the
get-miss→set create pattern skips the probe via the negative cache).
Later, when the store-mode legs loaded two nodes with 73 MB of
replica data, the utilization leveler visibly shifted proxy records
off the loaded nodes — leveling reacts to EXTERNAL pressure, not just
its own collection.

**Proxy mode — the full leg battery** (through one ingress, locator
warmed by the read legs; all columns of this table from one run):

| leg | proxy (px) | store (b) warm | proxy tax |
|---|---|---|---|
| GET rtt d1, p50/p99 | 248–261 / ~500 µs (either node — reads stay remote forever) | 59–61 / ~120 µs | ×4.3 |
| SET rtt d1 (forwarded to the holder) | **248 / 494 µs** | 61 / 137 µs | ×4.1 |
| GET piped 8c×32 | 36k ops/s | 494k | ×14 |
| SET piped 8c×32 | 40k ops/s | 1.02M | ×26 |
| MIX 90/10 8c×32 | 32k ops/s | 544k | ×17 |

The fair cold-start comparison (a store column that is only ever warm
would flatter it): a COLD store-mode node reads the same keyspace at
**60 µs p50 while pulling** — pull-and-KEEP converges it to fully
local within a single 5 s pass (200,000 copies kept), after which
reads are 59 µs and never leave the node.  Proxy reads never converge
by design — ~250 µs is their steady state, the price of fleet-sized
capacity.  (Pulls continue until full convergence; a keyspace whose
TTLs expire faster than it is read may never fully converge — the
planned eager-store mode closes that gap by pushing copies in the
background.)

The proxy tax is the datagram round-trip to the holder and, pipelined,
the peer-plane serialization — proxy is the CAPACITY plane; hot data
belongs in a store collection.

**Store-mode pull convergence**: a cold node read the whole keyspace
through itself at **60 µs p50** while replicating — **200,000 copies
kept after a 5 s read pass** — and the warm pass that follows runs
entirely local at 59 µs.  Miss→pull→keep is the read-repair working
at full speed.

**Memory** (200k × 256 B live in `b`): live data ~73 MB vs Redis
used_memory 71.5 MB (~366 B/entry both — a wash).  Container RSS
149–250 MB per node (128 MB arena prefaulted + pinned by policy, plus
process) vs 77–93 MB for Redis, which grows as it fills.

**Durability, everysec vs everysec** (`wal` topology, redis
`--appendonly yes`):

| leg | perfcached WAL | redis AOF |
|---|---|---|
| SET rtt d1 p50 | **52 µs** (WAL tax ≈ 0–6 µs) | 97 µs |
| SET piped 8c×32 | **1.19M ops/s (498 MB/s through the WAL)** | 266k ops/s |

**4.5× apart with equal guarantees.**  Crash proof: kill -9 on a node
→ restart → **its full WAL-backed shard recovered exactly (17,136 of
17,136 records) in 3.8 s**, and the node rejoined the cluster by
itself.  The prior
generation proved the same at 200k records in 4.9 s.  (Store-mode
PULLED replicas are passive and deliberately not persisted — they
re-pull from peers on demand.)

### What this matrix run itself found

Running the daemon on musl for the first time surfaced two real
portability bugs and two performance cliffs — all fixed in this repo,
all invisible on glibc: 128 KB default thread stacks vs ~130 KB of
on-stack datagram buffers (segfault; threads now spawn with 1 MB
stacks), per-request 1 MB scratch mallocs turning into mmap/munmap
per op under musl's allocator (~50× throughput loss; scratch is now
per-thread), a per-message input-buffer memmove quadratic in pipeline
depth (13% of a piped run; drains now consume by offset), and a WAL
autosize that happily provisioned 2 GB/node until the filesystem
filled (the bench pins 8 × 64 MB; a free-space guard in autosize is
an open item).  If you rerun this matrix on an older revision, expect
to rediscover them.

## 8. Teardown

    for c in pcnode1 pcnode2 pcnode3 pcredis pcredis-aof; do
        nerdctl rm -f $c 2>/dev/null; done     # or docker rm -f
    nerdctl network rm pcnet
    rm -rf /var/tmp/pccluster

## Caveats, so the numbers travel honestly

- One machine, shared kernel: perfcached legs are loopback, Redis legs
  cross one veth pair.  Both are far below the effects being measured,
  and both are stated next to every number.
- 2 s warm-up discarded on every timed leg — cold caches are not data.
- `pcbench` fails a leg loudly on ANY error reply; a bench that cannot
  fail is not a bench.
- Redis runs its stock container entrypoint (default RDB snapshotting)
  except where the AOF arm says otherwise; perfcached runs without WAL
  except in the `wal` topology.  Neither side pays persistence inside
  a timed window unless the leg is about persistence.
