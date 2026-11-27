# Measured

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

> **0.4.0's numbers**, except the `spread` farm table, which is still
> 0.3.7's (not re-taken since), the TLS table,
> which is 0.7.5's, and the Redis Cluster table, a 0.7.4 build on bare
> metal (2026-11-01).

Every other table is the tagged 0.4.0 daemon (`570067a`), measured
2026-10-01, on one server host - a 16-vCPU Debian 13 VM - with the load
generated elsewhere wherever the harness allows it: the wire table and
the keys/scan table drive the server from other hosts; the cluster
tables drive it from a container on the same host, which makes those
cells client-bound on a 16-vCPU box, and the section says so where
they stand.  The cluster tables come from three runs of one harness,
named per mode below: the full sweep, and two re-runs taken because a
network incident overlapped part of the first (eager, proxy) - each
mode's rows come from one run, and its "vs redis" is against that
run's own Redis rows.  The harness stamps the binary's revision into
every results file and refuses to measure a build that cannot name
itself.  Read a figure with what bounded it: the SET cells at capacity
pin the server's cores, and most GET cells are bounded by the client
or the wire.  The method behind each table, how to reproduce it, and
the readings that changed as the harnesses improved are in
[bench/RESULTS.md](../bench/RESULTS.md); raw rows are in
[bench/results/](../bench/results/), every earlier run's included.
Numbers rot: quote from a run, not from here.

## Against Redis Cluster, on bare metal (0.7.4)

Two fleets on the same two servers, 2026-11-01: Redis 8.0.2 Cluster with
30 masters (10 per NUMA node, no replicas), and a perfcached `shard`
fleet of three nodes with 12 workers each, one node per NUMA node - a
0.7.4 development build (`5a02babb`; it reports 0.7.4.1), so 0.7.5's
admission and cluster wire are not in it.  The servers are two
bare-metal 2x E5-2690 v2 hosts on 10G, with their root filesystems in
RAM.  The clients ran on the second host's other NUMA node and on a
third host: `redis-benchmark` 8.0.2, one process per owner per client
host at pipeline 32, each sending one hash tag so that every request
reaches its owner directly; 1,000,000 keys.  The native rows are
`bench/natbench` over libperfd, routed by key, with the native door's
default `encrypt = no` (the Noise handshake, then requests in the
clear).  The rate is what the servers answered over 15 s with every
client running; server CPU is the server processes' own over 20 s inside
the cell, so it compares the two daemons whatever bounded the cell
([bench/results/redisx-0.7.4.tsv](../bench/results/redisx-0.7.4.tsv)).

**32-byte values** - the wire has room, so CPU decides:

| | SET (ops/s) | GET (ops/s) | server CPU, SET (us/op) | server CPU, GET (us/op) |
|---|---|---|---|---|
| **Redis Cluster, 30 masters** | 15.44M | 17.51M | 1.93 | 1.67 |
| perfcached, RESP door | 17.35M | 18.36M | 1.73 (-10%) | 1.29 (-23%) |
| perfcached, native door | **21.97M** | **24.57M** | **1.34 (-31%)** | **1.02 (-39%)** |

**200-byte values** - the first server's link was full (9.9 Gbit/s) in
every cell, so read the CPU columns:

| | SET (ops/s) | GET (ops/s) | server CPU, SET (us/op) | server CPU, GET (us/op) |
|---|---|---|---|---|
| **Redis Cluster, 30 masters** | 10.98M | 12.44M | **1.96** | 1.73 |
| perfcached, RESP door | 9.87M | 12.29M | 2.07 (+6%) | 1.38 (-20%) |
| perfcached, native door | 11.37M | 14.66M | 1.84 (-6%) | **1.17 (-32%)** |

**Durable, 200-byte values** - Redis with `appendonly yes`,
`appendfsync everysec` and the default save rules; perfcached with
`[wal] fsync = everysec`, 32 x 256 MB segments a node and the default
save rules:

| | SET (ops/s) | GET (ops/s) | server CPU, SET (us/op) | server CPU, GET (us/op) |
|---|---|---|---|---|
| **Redis Cluster, AOF** | 8.13M | 12.40M | 3.72 | 1.73 |
| perfcached, RESP door, WAL | 8.69M | 12.33M | 2.77 (-26%) | 1.41 (-18%) |
| perfcached, native door, WAL | **8.91M** | **14.66M** | **2.54 (-32%)** | **1.13 (-35%)** |

Through the same RESP protocol, a perfcached SET cost 10% less server CPU
than Redis's and a GET 23% less at 32 bytes; through the native door,
31% and 39% less, and the native SET cell reached 9.2 Gbit/s on the first
server's link.  At 200 bytes the link set the rate of every cell, and
the one cell where Redis cost less was SET through the RESP door, by 6%.
With logging on, Redis's SET cell became CPU-bound - 30 cores at
3.72 us a SET - while both perfcached arms stayed at the link with a
quarter to a third less CPU a SET.  No WAL record was dropped, no
segment overran and no node healed; one node's writer paused 17,760
times on a full ring, which is the ring making writers wait rather than
drop.  The filesystems were in RAM, so the durable rows measure what
logging costs the CPU, not a disk: a real disk sets its own ceiling
("The write-ahead log has a ceiling" in the README).

Read the shapes with the figures: Redis ran as 30 single-threaded
processes and perfcached as three multithreaded ones on the same three
NUMA nodes, neither with replicas, and both CPU columns cover every
server process.  With `encrypt = yes` a native request costs about
twice the CPU (see `encrypt` under `[listen]`).  To be re-taken on
0.7.5; the rig is in [bench/RESULTS.md](../bench/RESULTS.md).

## One server against one server, over the wire

`redis-benchmark` - Redis's own tool, unmodified - on one host; one
perfcached node, no cluster, and Redis on another, both as containers
with host networking, a real NIC between them at MTU 9000 verified end
to end, arms alternated so drift cannot favour either.  0.4.0's daemon
(`570067a`), 2026-10-01, the 16-vCPU Debian 13 server host, a 16-vCPU
Ubuntu 24.04 client host (`redis-benchmark` 8.10.2); 4 workers, 20k
keys x 200 B, cells sized to run for over a second, median of 3
(`bench/xhostbench.sh`,
[bench/results/xhostbench-0.4.0.tsv](../bench/results/xhostbench-0.4.0.tsv)).

**50 clients, no pipelining:**

| | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| **redis-server 8.10.1** | 41,824 | 46,468 | - | 1.54 ms | 1.47 ms |
| perfcached, 1 node | 37,425 | 44,703 | x0.89 / x0.96 | 1.08 ms | 1.00 ms |

**50 clients, pipeline 64:**

| | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| **redis-server 8.10.1** | 760,746 | 948,317 | - | 5.39 ms | 4.98 ms |
| perfcached, 1 node | **1,643,385** | **1,314,060** | **x2.16 / x1.39** | **1.73 ms** | **1.74 ms** |

At depth 1 both servers wait on the round trip and the wire decides:
perfcached lands a little under Redis on throughput and well under it
on the tail (1.08 ms against 1.54 ms on SET).  The depth-1 reps spread
wide on both arms (perfcached 36.8-52.5k SET/s, Redis 40.0-49.1k) and
both sit below 0.3.7's run of this table, so read that row as the
network's that day.  At depth 64 the difference is the servers -
**+116% on SET and +39% on GET, at about a third of Redis's p99** -
because Redis is single-threaded and perfcached runs four workers.
Against 0.3.7's run: SET +1%, GET -5%, and the p99 tighter on both
(2.67 -> 1.73 ms SET, 2.02 -> 1.74 ms GET).  This is the number a deployment gets; the loopback
comparison that stood here until 0.3.7, and why a loopback read
overstates a 1500-byte network, are in
[bench/RESULTS.md](../bench/RESULTS.md).

## The cluster modes through a Redis client

<!-- containerbench:0.4.0 -->
Three-node fleets, one per mode, driven through node 1's RESP door by
`redis-benchmark` - the honest shape of a Redis migration, and a client
that **does not route**: it dials one node, so every key that node does
not own is a forward, and under `spread` every key it does not hold is
a pull.  Real Redis (`redis:8`, persistence off) runs on the same rig in
every run.  0.4.0's daemon (`570067a`), 2026-10-01, the 16-vCPU Debian
13 server host under podman, 20k keys x 200 B, 8 workers a node, 50
clients, median of 3, every cell sized to run for at least 10 seconds
(`bench/containerbench.sh`).  Store, shard and spread are the full
sweep ([bench/results/containerbench-0.4.0.tsv](../bench/results/containerbench-0.4.0.tsv));
eager is a re-run on the same host settings
([containerbench-0.4.0-eager.tsv](../bench/results/containerbench-0.4.0-eager.tsv));
proxy is a re-run taken after the host's `invtsc` CPU flag was removed
([containerbench-0.4.0-proxy.tsv](../bench/results/containerbench-0.4.0-proxy.tsv)) -
identical cells moved -13% to +18% across that change, so read proxy's
rows with that margin.  Each row's "vs redis" is against its own run's
Redis.  One node on its own, same rig, same client: 2,391,791 /
2,442,206 at pipeline 16, 2,597,705 / 2,597,897 at 64.

**50 clients, pipeline 16** (redis, full sweep: 552,830 SET / 590,555 GET;
eager run 541,585 / 590,946; proxy run 503,983 / 551,799):

| mode | SET/s | GET/s | vs redis | SET p99 |
|---|---|---|---|---|
| store | 2,390,200 | 2,390,395 | x4.32 / x4.05 | 0.51 ms |
| eager | 1,540,669 | 2,252,540 | x2.84 / x3.81 | 1.80 ms |
| proxy | 2,604,359 | 2,704,696 | x5.17 / x4.90 | 0.46 ms |
| **shard** | **228,376** | **202,317** | **x0.41 / x0.34** | 2.06 ms |
| **spread K=2** | **1,501,898** | **1,070,058** | **x2.72 / x1.81** | 1.73 ms |

**50 clients, pipeline 64** (redis, full sweep: 811,386 SET / 859,271 GET;
eager run 858,294 / 972,850; proxy run 794,859 / 949,683):

| mode | SET/s | GET/s | vs redis | SET p99 |
|---|---|---|---|---|
| store | 2,539,969 | 2,596,783 | x3.13 / x3.02 | 1.21 ms |
| eager | 2,050,705 | 2,540,410 | x2.39 / x2.61 | 4.58 ms |
| proxy | 2,805,447 | 2,985,274 | x3.53 / x3.14 | 1.09 ms |
| **shard** | **138,874** | **127,779** | **x0.17 / x0.15** | 2.38 ms |
| **spread K=2** | **2,227,593** | **2,697,271** | **x2.75 / x3.14** | 3.49 ms |

Reads stay in one band across the placement modes; eager's SET pays for
its replication on the write path, and so does spread's - each write
lands on two of the three nodes.  Shard's row is the client, not the
mode: a client that cannot compute an owner forwards two keys in three.
Spread's GET is the pull - the node the client dialled holds two keys
in three and fetches the rest - and at pipeline 1 it costs what it did
in 0.3.7 (185,912 GET/s, the same as 0.3.7's 185,900).  Deeper
pipelines no longer wait on it: 1,070,058 GET/s at 16 and 2,697,271 at
64, against 0.3.7's 433,996 and 365,967 - a same-host A/B of the two
builds confirms it is the daemon (RESULTS.md).  The next section is the same
fleets through clients that can route.
<!-- /containerbench:0.4.0 -->

## The same fleets through clients that route

Same fleets, same daemon, same runs as the section above (each mode's
rows from the run named there).  Two clients that can route, both
driven from a container on the server host - which makes every cell
here client-bound on a 16-vCPU box, so read them as floors; the
off-box farm in the `spread` section is the ceiling for the same
daemon.

**A Redis client that routes**: `redis-benchmark` given one hash tag
per owner from `CLUSTER NODES`, three drivers, one per node - what a
cluster-aware Redis client does with `CLUSTER SLOTS`, without one.
The plain rows above are the same two fleets through the one-node
client.  Both from the full sweep.

**50 clients, pipeline 16** (redis: 552,830 SET / 590,555 GET):

| mode | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| **shard** | **2,863,996** | **3,276,091** | **x5.18 / x5.55** | 0.79 ms | 0.72 ms |
| **spread K=2** | **2,085,937** | **3,223,029** | **x3.77 / x5.46** | 1.18 ms | 0.71 ms |

**50 clients, pipeline 64** (redis: 811,386 SET / 859,271 GET):

| mode | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| **shard** | **5,425,901** | **6,480,353** | **x6.69 / x7.54** | 1.53 ms | 1.33 ms |
| **spread K=2** | **3,525,133** | **6,713,506** | **x4.34 / x7.81** | 4.58 ms | 1.27 ms |

**A cluster-aware client**: `natbench` over libperfd with
`opts.route_keys`, binary protocol, one process spreading its
connections over the fleet and sending each key to its owner or
holder.  "vs redis" against each mode's own run, as above.

**50 clients, pipeline 16:**

| mode | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| store | 2,291,292 | 2,531,371 | x4.14 / x4.29 | 0.86 ms | 0.78 ms |
| eager | 1,469,631 | 2,850,139 | x2.71 / x4.82 | 1.83 ms | 0.69 ms |
| proxy | 2,305,653 | 2,878,698 | x4.57 / x5.22 | 1.02 ms | 0.59 ms |
| shard | 2,224,943 | 2,629,862 | x4.02 / x4.45 | 0.90 ms | 0.80 ms |
| spread K=2 | 1,462,521 | 2,528,190 | x2.65 / x4.28 | 2.17 ms | 1.11 ms |

**50 clients, pipeline 64:**

| mode | SET/s | GET/s | vs redis | SET p99 | GET p99 |
|---|---|---|---|---|---|
| store | 3,192,277 | 3,877,529 | x3.93 / x4.51 | 2.12 ms | 2.06 ms |
| eager | 1,964,453 | 4,367,856 | x2.29 / x4.49 | 4.29 ms | 1.89 ms |
| proxy | 3,035,413 | 3,668,588 | x3.82 / x3.86 | 2.69 ms | 2.37 ms |
| shard | 3,043,751 | 3,877,518 | x3.75 / x4.51 | 2.54 ms | 1.94 ms |
| spread K=2 | 2,037,405 | 3,984,971 | x2.51 / x4.64 | 4.47 ms | 2.21 ms |

Routing turns shard from the slowest arm into the fastest: 5,425,901
SET/s and 6,480,353 GET/s at pipeline 64 through the Redis client,
x6.69 / x7.54 against Redis, where the plain client gets x0.17 / x0.15
- shard was never the slow mode, it was the mode whose client could
not compute an owner.  It takes spread's reads with it (6,713,506
GET/s, x7.81), while spread's SET keeps the copy cost: the holder that
takes the write still pushes the second copy.  Through libperfd all
five modes read in one band, 3,668,588 to 4,367,856 GET/s at pipeline
64, and shard's SET (3,043,751) sits within 5% of store's (3,192,277);
eager's and spread's carry their copies (1,964,453 and 2,037,405).
Against 0.3.7's run the routed Redis-client rows rose 15-20% at
pipeline 64 (shard 4.51M -> 5.43M SET, 5.59M -> 6.48M GET; spread
5.83M -> 6.71M GET).  The libperfd rows sit within -8% to +10% of
0.3.7's, this rig's run-to-run spread on an unchanged arm, except
proxy's SET at pipeline 64: 3,035,413 against 2,120,087, +43% - from
the proxy re-run, taken after the `invtsc` change, so part of that may
be the host.  What routing buys is
two things, the removed hop and connections spread over the fleet,
decomposed in [bench/RESULTS.md](../bench/RESULTS.md).

**Read through a node holding nothing** (50 clients, pipeline 64, the
RESP client by construction; node 2 is restarted empty before every
rep and read through; each mode from its run above):

| mode | GET/s | GET p99 |
|---|---|---|
| store | 2,540,410 | 1.07 ms |
| eager | 2,538,500 | 1.02 ms |
| spread K=2 | 380,409 | 1.87 ms |
| shard | 134,251 | 1.99 ms |
| proxy | 75,325 | 49.95 ms |

Store pulls the key and keeps it, and eager already replicated it.  A
spread node that comes back empty is now handed only its share (two
keys in three) by the boot pull, so it pulls the rest like any
non-holder: 380,409 GET/s.  0.3.7's run of this table read 2,657,723
here because its boot pull handed the returning node the WHOLE keyspace
and the bench window fell before the reclaim trimmed it - that was the
defect (fixed in 0.4.0), not a speed.  The two modes that have to
go and get the key differ in how: shard unicasts to the one owner,
proxy consults a locator and broadcasts on a miss.

## `spread` at capacity, from an off-box farm

> **Still 0.3.7's numbers** - not re-taken since.  Read it as an older
> release's.

`bench/spreadbench.sh`, 0.3.7's daemon (`e58ee8e`), 2026-09-16.  P bare
daemons on the 16-vCPU server host; natbench (this tree's, over
libperfd, binary protocol, 64 connections, 8 threads, depth 32, 200,000
keys x 200 B, 30 s cells) on a 24-core farm of two other hosts (16 +
8).  *Un-routed* is one natbench per node per farm host with the
connections split - a dumb load balancer; *routed* is one natbench per
farm host letting libperfd 0.2.8 send each key to a holder.  Server
cores are the fleet's CPU-seconds over the cell: the three-node fleets
run 5 workers a node (15 threads), the six-node fleets 2 (12), so a
cell at the thread count is a server ceiling.  Eager is the yardstick
on the same fleet
([bench/results/spreadbench-0.3.7.tsv](../bench/results/spreadbench-0.3.7.tsv)).

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

## `KEYS` and `SCAN` against Redis, on production-shaped keyspaces

New in 0.4.0's measurements.  Two keyspaces built to the SHAPES of two
production Redis instances - their classes of key, proportions, name
lengths and TTL share, under neutral names; no key or value of theirs
is in the repository: `mixed`, 73,733 keys, half with a TTL (51% 40-
character session tokens, 30% PHP-style sessions, the rest counters
and small classes), and `tokens`, 38,232 keys (96% tokens).  The
`KEYS` pattern on `mixed` is the one the source instance really sends
- a lookup that matches about one key and walks all 73,733.  perfcached
0.4.0 (`570067a`, 4 workers) and `redis-server` 8.10.2, both on the
same 4 cores of one host; the client on another host; arms alternated,
median of 3, servers allowed 75 s to settle after the load
(`bench/keyscanbench.sh`,
[bench/results/keyscanbench-0.4.0.tsv](../bench/results/keyscanbench-0.4.0.tsv)).

**`mixed`, 73,733 keys:**

| | redis-server 8.10.2 | perfcached 0.4.0 |
|---|---|---|
| `KEYS`, 1 match, alone: p50 | 4.6 ms | 4.8 ms |
| `KEYS`, 4,423 matches, alone: p50 | 12.7 ms | 10.8 ms |
| `SCAN MATCH` walk, COUNT 1000 | 42 ms, 74 calls | **18 ms, 33 calls** |
| `SCAN MATCH` walk, COUNT 100 | 191 ms, 736 calls | **82 ms, 328 calls** |
| 50 connections of GETs, no `KEYS`: ops/s, p50, p99 | 71,558, 0.68 ms, 1.16 ms | **142,016, 0.27 ms**, 2.16 ms |
| the same GETs, `KEYS` at 50/s: ops/s | 52,320 (-27%) | **139,318 (-2%)** |
| the same GETs, `KEYS` at 50/s: p99 / p99.9 | 7.64 / 9.35 ms | **1.57 / 3.72 ms** |
| that `KEYS` under the GET load: p99 | **10.0 ms** | 63.9 ms (112 ms at 10/s) |

`tokens` has the same shape: under `KEYS` at 50/s Redis's GETs lose
15% and their p99 goes from 1.05 to 4.17 ms, while perfcached's hold at
136,925 GET/s and 1.48 ms; perfcached's `KEYS` p99 under load is 14-39
ms against Redis's 5.

Redis runs one command at a time, so every `KEYS` holds every other
client for its whole walk.  perfcached's `KEYS` is cooperative - it
walks a chunk per event-loop turn and lets the worker's other
connections run between chunks - so the other clients barely notice,
and the `KEYS` caller pays instead: under load its walk waits behind
those turns.  From 0.4.1 a turn walks by time rather than by a fixed
chunk of 1,024 hash buckets: it walks that floor, then goes on until
the turn has spent `[daemon] keys_turn_us` (default 250 us), so a
sparse table no longer costs one turn per 1,024 empty buckets (1,000
keys in 131,072 buckets: 128 turns before, about 6 now).  The setting
is the trade itself - larger makes `KEYS` faster for its caller and
holds everyone else's turn longer.  On one host (servers on 4 cores,
clients on 8 others, loopback; `mixed`, 50 GET connections, `KEYS` at
50/s, median of 3): 0 (0.4.0's walk) gave `KEYS` p99 13.1 ms and GET
p99 531 us; 1000 gave `KEYS` p99 8.2 ms (Redis 10.7) and GET p99 1,244
us; 250 moved neither beyond the noise.  The farm figures above are
0.4.0's and were not re-run for this.  Two caveats: with no `KEYS` at all,
perfcached's GET p99 here is above Redis's (2.16 against 1.16 ms) while
its median is less than half Redis's; and three single cells (one rep
each) came in far below their siblings - the client host was shared -
which the medians do not use; the results file keeps them.

## TLS on the RESP door (0.7.5)

What TLS costs a RESP door: `redis-benchmark` 8.0.2 against one node's
plaintext door and its TLS door, the same process, arms alternated.
2026-11-06, the 0.7.5 development daemon (`c611430e`, OpenSSL 3.5.7)
on a bare-metal 2x E5-2690 v2 (one NUMA node, 4 workers, no WAL), the
client on another of the same model over 10G; 64 connections, 32-byte
values, a million keys, median of 3
([bench/results/tlsbench-0.7.5.tsv](../bench/results/tlsbench-0.7.5.tsv)).
TLS 1.3 with AES-256-GCM and the X25519MLKEM768 key exchange, which
OpenSSL 3.5 picks by default at both ends.  Server CPU is the process's
own, over each cell.

**No pipelining:**

| | SET (requests/s) | GET (requests/s) | server CPU, SET (us/request) | server CPU, GET (us/request) | SET p50 / p99 (ms) | GET p50 / p99 (ms) |
|---|---|---|---|---|---|---|
| plaintext | 315,590 | 324,044 | 11.58 | 11.23 | 0.143 / 0.327 | 0.143 / 0.311 |
| TLS | 196,194 (-38%) | 199,402 (-38%) | 20.38 (+76%) | 19.68 (+75%) | 0.255 / 0.519 | 0.239 / 0.527 |

**Pipeline 16:**

| | SET (requests/s) | GET (requests/s) | server CPU, SET (us/request) | server CPU, GET (us/request) | SET p50 / p99 (ms) | GET p50 / p99 (ms) |
|---|---|---|---|---|---|---|
| plaintext | 2,959,894 | 3,074,086 | 1.33 | 1.29 | 0.287 / 0.559 | 0.247 / 0.535 |
| TLS | 1,997,004 (-33%) | 2,214,349 (-28%) | 1.98 (+49%) | 1.79 (+39%) | 0.447 / 0.687 | 0.407 / 0.671 |

TLS costs the server 8.5-8.8 us of CPU a request without pipelining and
0.5-0.65 us at depth 16: a record each way per round trip, so pipelining
spreads it.  The four workers were the limit in the TLS cells and the
pipelined plaintext ones (3.8-4.0 cores busy); the plaintext depth-1
cells ran at 3.5-3.75, so there the throughput drop overstates the
server's share a little - size by the CPU column.  A profile of the TLS
depth-1 cell puts 31% of the server's CPU in OpenSSL (libcrypto 19%,
libssl 12%), with no single hot spot.

**Full handshakes** (8 `openssl s_time -new` clients for 10 s, twice):

| certificate | handshakes (/s) | server CPU (us/handshake) |
|---|---|---|
| RSA 2048 | 3,277 | 936 |
| ECDSA P-256 | 6,931 | 405 |

The server was 2.8-3.1 cores busy, so these rates are bounded by the
clients as much as by the server; a client that keeps its connections
pays the handshake once.
