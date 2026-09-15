# Measured results — the method, the discussion, the history

The README carries the tables and one reading each.  This file carries
the rest: how every table was produced, how to reproduce it, what
bounds each figure, and the readings that changed as the harnesses
improved.  Every number here names the build it came from; quote from
a run, not from a page.

Raw rows live beside this file in `results/`: one row per arm / clients
/ pipeline cell, with a header naming the build, host, date and
workload.  The harnesses refuse to measure a binary that cannot name
itself, so a results file always says what produced it.

## The client, and what it measures

**The client is `redis-benchmark`** for every Redis comparison — Redis's
own tool, unmodified.  It drives both servers with the same workload:

    redis-benchmark ──native RESP──> redis-server 8.0.2
    redis-benchmark ──native RESP──> perfcached's RESP door

Nothing is translated or proxied on either path.  perfcached speaks
RESP2 itself — that door exists so unmodified Redis clients work — so
the client cannot tell which server it reached, and neither server is
doing anything special to be measured.

**The cluster-aware figures use `natbench` over libperfd** (this tree's
own client), because `redis-benchmark` cannot route: it dials one node.
The two clients are different programs, and a resp figure and a native
figure are not interchangeable — redis-benchmark out-pipelines natbench,
which is why a resp arm can appear to beat the binary arm.  Valid
comparisons: redis vs perfcached-resp (same client), json vs bin (same
client).

## Reproducing every table, from a clean machine

There are two routes.  **If you just want the numbers, use the container
one** - it needs a container runtime and nothing else at all:

    bench/containerbench.sh

That builds its own image, pulls its own Redis, creates its own network,
runs both phases and tears everything down.  No compiler, no libsodium,
no redis-server, no perfcached binary on the host.  It prefers `podman`
(which builds without a daemon), then `nerdctl`, then `docker`, and it
probes each with a real build before choosing - `nerdctl` answers `info`
happily and then fails every build if `buildkitd` is not running.

Verified on both podman and docker: the cluster tables were produced by
this harness under **podman** on a 16-vCPU host, and an earlier run of
the same tables under docker on an 8-vCPU one had the same shape.

The other route is the **host** one, which is what produced the
single-node tables (`bench/respbench.sh`).  Nothing in it is pre-baked
either: the harness starts its own Redis, starts its own perfcached
fleet, drives both with the same client, and tears everything down.  On
Debian 13 / Ubuntu:

    # 1. a container runtime, and nothing else
    apt install -y podman        # or docker, or nerdctl + buildkitd
    git rev-parse --short HEAD   # the image is stamped with this

    # 2. run it.  REPS=3 is what the tables used.
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

**Budget an hour** with `REPS=3`.  Thirteen arms — redis, two
single-node arms, and each of the five modes measured twice, once
through the node holding the data and once through a node holding none
— at ten cells each, three runs per cell, plus a fleet start and stop
per arm and a 32-second wait per mode for the reshard grace to expire.

Drop to `REPS=1` for a smoke run, but do not compare arms with it — see
the note on the noise floor below.  A quicker subset:

    CLIENTS="50" PIPES="1 16" REPS=1 bench/respbench.sh
    MODEARMS="store shard" REPS=3 bench/respbench.sh

**What it needs from the machine.** A working container runtime, the
`10.96.0.0/24` subnet free for the bench network (override with
`SUBNET=`), and roughly 2 GB free for three 512 MB arenas. It no longer
needs host ports or the `127.0.42.1-3` loopback addresses: each node has
its own address on the bench network instead.  Run it on an otherwise
idle box: this rig's run-to-run spread on a single unchanged arm is
~11%, and a busy machine makes that much worse.

**The output.** `results.tsv` is one row per arm/clients/pipeline cell,
with a header line naming the build, host, date and workload.  Column
order is `arm, clients, pipeline, set_rps, get_rps, set_p50ms,
get_p50ms, set_p99ms, get_p99ms`.  Every number in the README's tables
is one of those cells; the ratio columns are that cell divided by the
`redis` row at the same clients and pipeline depth.

**Expect different numbers.**  The loopback figures are from a 16-vCPU
VM.  What should reproduce is the *shape*: the arms converging at
pipeline 1, perfcached pulling ahead as depth grows, store and proxy
staying in one band, eager's SET falling away from them at depth because
it replicates on the write path, and shard falling well behind any
client that cannot compute an owner.  If your shape differs, that is
worth more than the absolute values.

**Cells must run for seconds, not milliseconds.**  An earlier version of
the one-server table quoted 934,878 and 1,786,286 GET/s for the two
pipelined cells, and those exact figures had come out of two runs on
different builds.  That was the harness, not repeatability:
redis-benchmark times a run in whole milliseconds, and 100k requests at
1.8M/s is a 56 ms run, so every figure it could print sat on a ~2%
grid.  Cells now run for over a second (`n_for` sizes them).  The same
lesson bit the container harness a second time: with `--threads`,
redis-benchmark stops only on its 250 ms progress timer, so a cell that
finishes in 1.25-2.5 s prints a rate up to 25% low.  Every cell there is
now sized to run for at least 10 seconds, and the cluster tables the
README carried until 2026-09-12 were understated by up to one
quarter-second step.

## Across a real network — the caveats behind the table

Loopback shares one machine, which flatters both servers and costs
perfcached more than Redis: its workers compete with the client for
cores where a single-threaded Redis does not.  The cross-host table
(`bench/xhostbench.sh`, build `2db7baf`, median of 3, arms alternated
so drift cannot favour either, MTU 9000 verified end to end with
`ping -M do -s 8972` before anything is trusted) is the one a deployment
gets.  Two things to take from it rather than from the loopback tables:

**Loopback and the wire agree on SET** - x2.20 there, x2.22 here - and
the wire costs perfcached a little more than Redis on GET, x1.67 there
against x1.55 here.  Both are honest about their rig.

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

## The 0.3.7 tables, one host — the readings (S162, 2026-09-16)

**Why every table moved at once.**  Until 0.3.7 the README's Measured
section mixed hosts: the one-server and network tables from a Debian
13 host on 2026-09-03, the cluster-aware table from the same host on
rc34, and the 0.3.7 cluster tables from an Ubuntu 24.04 host on
2026-09-15.  The tables were each honest and the section was not
comparable with itself.  So every table was re-taken on the night the
release was cut: one server host (the Debian 13 VM, 16 vCPU, 15 GB),
one daemon (`e58ee8e`, the tagged tree, built and stamped on that
host), one day, with the load off-box wherever a harness allows it -
the wire table from an Ubuntu 24.04 host, the `spread` table from a
farm of two - and from a driver container on the server host where it
does not (the cluster tables).  The earlier runs stay in
`results/` under their own names, and their readings below stay
dated.

**One server over the wire** (`results/xhostbench-0.3.7.tsv`).  At
depth 1 perfcached sits a little UNDER Redis on this pair of hosts -
47,103 against 53,879 SET/s, 46,361 against 50,710 GET/s - where the
2026-09-03 pair had it a little over (52,274 against 46,795).  Both
readings are the wire: with one request in flight per connection both
servers wait on the round trip, and a 13% gap at that depth is the
kind of number two runs on two host pairs disagree about.  What does
not move between the pairs is the tail (0.97 ms against Redis's 1.29
on SET, 0.90 against 1.12 on GET) and depth 64: x2.20 / x1.46 here,
x2.22 / x1.55 there.  The loopback comparison that stood beside this
table until 0.3.7 (`results/respbench.tsv`, `2db7baf`, 2026-09-03) is
retired from the README because a loopback read is a best case a
1500-byte network does not reproduce - see "What bounds a loopback
read" below - and a table that a deployment cannot get is not a table
to size from.

**The cluster modes through a plain Redis client**
(`results/containerbench-0.3.7.tsv`).  The shape of the 2026-09-15
run on the other host, with every ratio a little lower because this
Redis is faster (584,759 SET/s at pipeline 16 against 491,205 there)
and this perfcached a little slower (store 1,951,522 against
2,254,387): the placement modes' reads in one band (x3.49-x3.75 at
16, x2.71-x3.01 at 64), eager's and spread's SET at their copy cost
(x2.60 and x2.67 against store's x3.34), shard through a client that
cannot route at the forwarding floor (x0.43 / x0.34, x0.18 / x0.14),
spread's GET the pull path (x0.68 at 16, x0.37 at 64) and never
warming, since a spread node does not keep what it pulls.  The host
difference is the one to remember when reading cross-host numbers:
this VM shows its guest two NUMA sockets of eight cores, the other
one socket of sixteen; both are the same Xeon at the same clock.

**Through clients that route.**  The Redis client with one
hash-tagged driver per owner takes shard to 4,506,397 SET/s and
5,590,501 GET/s at pipeline 64 (x5.31 / x5.63) and spread's reads to
5,831,098 (x5.87) - the same two results the 2026-09-15 run gave on
the other host, within 3%.  Through libperfd, binary dialect,
`route_keys`, all five modes read in one band at pipeline 64
(3,895,724 to 4,205,866 GET/s) and shard's SET (3,319,680) is within
1% of store's (3,289,013): the rc34 reading, "shard was never the
slow mode", reproduced on 0.3.7 on the release's own host.  Eager and
spread write at their copy cost through every client.  All of these
cells are client-bound: three eight-thread drivers, or one
eight-thread natbench, beside three eight-worker nodes on sixteen
vCPUs.  The off-box `spread` table is the ceiling for the same daemon.

**Read through a node holding nothing.**  Store 3,078,317 GET/s at a
1.25 ms p99, eager 2,730,017 at 1.16, shard 147,880 at 1.86 ms, proxy
77,513 at 47.36 ms - the two that go and get the key in the order
every run has put them, and proxy's tail where 2026-09-12 left it.
Spread's restarted node reads at 2,657,723: as tagged, 0.3.7 hands a
spread joiner the boot sender's whole store (S156), so the node holds
everything for the bench window.  The fix - the boot pull scoped to
the joiner's share, `spreadcoldtest` guarding it - is on master after
the tag and will move this cell to the pull path's figure in the next
release's run.

**`spread` at capacity** (`results/spreadbench-0.3.7.tsv`, the farm
being the Ubuntu 24.04 host and an 8-core host, 24 cores).  The
2026-09-15 shape on a faster server: eager's write ceiling 2.39M on
three nodes falling to 1.39M on six (1.84M -> 1.11M there); routed
spread K=2 at 2.59M and 2.03M SET on the same fleets, +46% over eager
on six; routed spread reads 4.86M on three nodes at 13.7 of 15 threads
against eager's 4.49M (4.25M against 4.10M there), and on six nodes
3.71M at K=2 against eager's 3.89M - the six-node routed read cells sit
at 10.7-11.5 server cores of 12, so they are the farm's ceiling as much
as the server's.  Un-routed K=2 reads are the pull path in every
fleet, 0.41-0.42M at a 48-54 ms p99; K=3 on three nodes reads at
eager's rate un-routed because every node holds everything, which is
the one un-routed cell a deployment could lean on and only because K
equals P.

## The cluster modes through a Redis client — the readings

**The first row of that table is real Redis, not a perfcached mode.** The
harness starts its own `redis:8` container (persistence off), measures
it, stops it, and then brings up a three-node perfcached fleet for each
mode.  8 workers a node, 20k keys x 200 B, 50 clients, median of 3 runs
per cell, every cell sized to run for at least 10 seconds.

**There are two client stories and they are not interchangeable.**  The
RESP client *does not route* — it dials one node, so every key that does
not belong to that node is a forward, and under `spread` every key that
node does not hold is a pull.  A cluster-aware client computes the owner
and talks to it directly.  That difference dominates every number in
that table, and for shard it is worth more than an order of magnitude.

Since 2026-08-30 a RESP client CAN route: ownership moved to the Redis
slot (`crc16(key) % 16384`), and the door answers `CLUSTER SLOTS`,
`CLUSTER SHARDS` and `CLUSTER KEYSLOT`, so any cluster-aware Redis
client places keys itself.  The table's figures were taken without one -
`redis-benchmark` does not route - so they show the non-routing path.
Read them as the floor, not the ceiling.

**Reads stay in one band; eager's writes no longer do** (build
`76fc29b`, v0.3.0-rc34, 2026-09-12).  On GET the three placement modes
span x4.15-x4.44 at pipeline 16 and x2.65-x3.25 at 64 - close enough, on
a rig whose run-to-run spread is several percent, to read as one band
rather than a ranking.  On SET eager trails store by about a quarter
(1.52M against 2.09M at depth 16, 1.87M against 2.50M at 64), a change
made after the 2026-09-04 figures: eager pushes every record to its
peers **on the write path** instead of leaving it to the sweep, so a
write pays for its own replication as it happens.  What it buys is on
the read side - every node stays hot, and eager's GET is the fastest of
the three at pipeline 16 - which is the trade the mode is for.

**shard answers now, and on 2026-09-04 it did not.**  Then, past a few
hundred requests in flight, every cell came back `ERR holder timed out`:
the forward was routed, sent and parked, and the answer missed its
deadline.  On 2026-09-12 the same cells answered - x0.46 / x0.37 at
pipeline 16, x0.19 / x0.15 at 64, at a p99 of 2-3 ms - so what was a
wall is now a slope.  It is still much the worst way to drive this
mode, for the unchanged reason: a client that cannot compute an owner
forwards nearly every key, and the fleet has to answer all of them.
Both refusals still exist for when it goes further: a forward that
cannot be parked because the table is full (`[cluster] max_pending`,
8192 by default) is refused with the retryable `TRYAGAIN cluster busy,
retry`, and one that is parked but not answered in time is `ERR holder
timed out`.

**The same table on 0.3.7's daemon, with `spread`** (`4d5b412`,
2026-09-15, 16-vCPU Ubuntu 24.04 host, podman, five modes,
`results/containerbench-rc16.tsv`).  The shape is rc34's, one host and
three days later: the placement modes' reads in one band (x4.48-x4.58
at pipeline 16, x3.03-x3.10 at 64), eager's SET at x2.87 against
store's x4.59, shard through a plain client the forwarding hop (x0.48
/ x0.39 at 16, x0.19 / x0.16 at 64).  The new row is spread at K=2:
SET x2.92 at 16 and x2.11 at 64, alongside eager's, since each write
lands on two of the three nodes; GET x0.79 at 16 and x0.41 at 64, the
pull path - the node the client dialled holds two keys in three and
fetches the third.  The results file for this run was rebuilt from the
run's log after a second run's setup overwrote it; the p50 columns were
not logged.

## The cluster-aware client — the readings

**The mode a Redis client drives at x0.19 comes within 2% of store on
both operations once the client can route.**  That is the whole result:
shard was never the slow mode, it was the mode whose client could not
compute an owner.  Proxy trails on SET, and since 2026-09-05 so does
eager - it writes to every peer on the write path, which is why its SET
is about half of store's while its GET is level with it.

Two things to keep straight about that table.  The `vs redis` column
compares a fleet-aware client against a single-node one, so it is a
deployment comparison rather than a wire comparison.  And routing buys
two separate things: it removes the forward hop, and it spreads
connections across the fleet.  Measured apart on a 3-node fleet, 50
connections at depth 32 (`bench/routepair.sh`, build `2db7baf`, median
of 3, raw rows in `results/routepair.tsv`) — store fetches nothing
either way after its first pass and still gains **1.4x** purely from
spreading (1,750,568 -> 2,447,670 GET/s), while shard gains **9.4x**
(275,312 -> 2,579,164 GET/s), with the daemons' own pull counter going
from ~940,000 pulls per 5-second run un-routed to zero routed.  So
shard's figure is that 1.4x times roughly 7x from the hop.

**A Redis client that routes gets the same result, on 0.3.7's daemon**
(run 2, 2026-09-15, 16-vCPU Ubuntu 24.04 host, podman,
`results/containerbench-rc16-routed.tsv`).  `redis-benchmark` cannot
route, so the harness gives it one hash tag per owner from `CLUSTER
NODES` and runs three drivers, one per node - the shape a
cluster-aware Redis client produces by itself.  Shard: 4,365,587 SET/s and
5,425,932 GET/s at pipeline 64 (x5.61 / x6.69 against Redis) where the plain client
gets x0.20 / x0.18 - the forwarding hop was the whole story, again.
Spread K=2: the plain client reads at 350,820 GET/s at pipeline 64 (the
pull path, one read in three fetched from a holder every time); routed,
5,756,621, x3.77 / x7.10 - every read lands on a holder.  Its SET stays at 2,938,075 (x3.77):
the holder that takes the write pushes the second copy, and that is
the mode's price, not the client's.  These cells are client-bound -
three eight-thread drivers beside three eight-worker nodes on sixteen
vCPUs - so read them as the floor; the two-host farm in the spread
section is the ceiling for the same daemon.

**Per-key routing was unreachable from libperfd's async API until
`e0a0a83`** — an async handle never learned the fleet, so
`opts.route_keys` had nothing to route with.  Every shard number this
project published before that date is the un-routed path.

## Reading through a node that holds nothing — the readings

This is a **RESP-client scenario by construction.**  A routed client
reads from each key's owner and never lands on a node that lacks the
key, so the case does not arise for it — and emptying a node to
manufacture it merely deletes part of the keyspace, which a routed arm
then reports as a fast, wrong number.

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
which was called unexplained: it does not reproduce, and two runs on
2026-09-11/12 put it at 33 and 34 ms.  Proxy's tail moved the other way
over the same period, 8.7 ms to 47 ms, and that is not explained either
- it is the one number here that got worse.

One caveat if you reproduce this: for 30 seconds after a membership
change (`SHARD_GRACE_S`) a shard miss does not answer authoritatively,
it retries once as a broadcast, because the data may still sit on the
old owner.  Measured inside that window shard looks *slower*.  The
harness waits it out; a hand-rolled test that starts a fleet and
measures immediately will get the wrong answer, as two of ours did.

**On 0.3.7's daemon** (`4d5b412`, 2026-09-15, same run as the five-mode
table): store 3,047,663 GET/s at a 0.96 ms p99 and eager 2,862,855 at
1.02 ms, as before; shard 149,237 at 2.09 ms and proxy 74,278 at
50.98 ms, so the two that go and get the key kept their order, and
proxy's tail is where the 2026-09-12 run left it.  Spread's restarted
node reads at 2,922,175 with a 1.09 ms p99 - eager's rate - while the
node that took the writes reads at 352,709 in the five-mode table.
Measured on a three-node K=2 fleet on 2026-09-15: a node that starts
cold is backfilled with the whole keyspace - its share is two keys in
three, and it held all of them within 18 s - and is trimmed back by the
reclaim pass at 64 keys a tick, so during the bench window it holds
everything and pulls nothing.  A node in the steady state never keeps
what it pulls: reading 3,000 keys through it twice pulled 993 and then
993 again.  So the five-mode table's figure is the one to size an
un-routed spread client from, and the over-holding after a cold start
is S156 in the ledger.

## If you are putting a Redis client in front of shard mode

Shard is a mode this project relies on — it is the one with computed,
deterministic ownership — so its number is a headline result and not a
footnote.  Through a RESP client it is ahead of Redis at low
concurrency (x1.96 SET / x1.79 GET at 50 clients, pipeline 1; the
2026-09-04 sweep, which walked more client counts, read x1.33 at 16
clients), and falls away as requests in flight grow: x0.46 at pipeline
16, x0.19 at 64.  On 2026-09-04 it stopped answering entirely at those
depths; now it answers, slowly.

The cause is not shard mode.  The client used there has no cluster map,
so it cannot compute the owner and forwards most keys.  Every other mode
places or replicates rather than computing an owner, so a wrong guess
costs less.  (A map is available to RESP clients — see above — but
those numbers were taken without one.)

**Two distinct failures live behind that, and they are told apart.**
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
  landing 5% above routed store.  Part of that gain is fleet
  utilisation rather than the removed hop — decomposed above.  And
  **on the async API this only works from `e0a0a83`**: before it, an
  async handle never learned the fleet.  Async callers open one handle
  per node and pick with `perfd_owner_of()`; the library will not open
  connections an async caller would never poll, because that caller
  drives one fd per handle.
- If the client must be a Redis one, prefer `store` - or `eager` when
  every node must stay hot - and NOT proxy.  None of the three has an
  owner to guess wrong, and all sit in one band read through a node
  that holds the data; but a non-routing client lands on arbitrary
  nodes, and read through a node holding nothing, store and eager stay
  in the millions while proxy drops to 79,850 GET/s at a 47 ms p99 - a
  locator miss there is a broadcast.  Choose proxy for what it is for -
  fleet capacity ~= the sum of the arenas - and accept the cold-read
  cost knowingly.  The same applies to `spread` through a plain client:
  a node that is not a holder pulls, and at K=2 of 6 that is two reads
  in three.
- If it must be shard AND a Redis client, keep pipeline depth modest.
  The wall is in-flight requests, not request rate.

## `spread` at capacity — the rig

`bench/spreadbench.sh`, rc16, 2026-09-15.  Neither earlier measurement
of the mode was a speed: the six-node run of 2026-09-13 measured apply
*ratios* at a fixed rate (at fixed K, doubling the fleet halves each
node's apply load), and the 2026-09-14 RESP run drove one node at
pipeline 1 from one host, bounded by the round trip.

P daemons on one 16-core host, each on its own private address on the
NIC - the advertise address is what the `members` reply hands a routed
client, so loopback aliases on the server would refuse a client on
another host - with natbench on separate hosts: one 12-core host for the
SET and un-routed cells, a 20-core farm (12 + 8) for the routed and eager
GET cells, which one host could not saturate.  64 connections, 8 threads,
depth 32, 200,000 keys x 200 B, 30 s cells, three-node fleets at 5
workers a node and six-node fleets at 2.  *Un-routed* is one natbench
per node with the connections split - a dumb load balancer; *routed* is
one natbench letting libperfd 0.2.8 send each key to a holder.  Every
cell records the fleet's CPU-seconds, so a figure reads as a server
ceiling or as client-bound: the SET cells sit at 15-16 of 16 cores, the
routed GET cells at the worker thread count (14.4 of 15, 12.1 of 12),
and eager's GET on the same farm within 5% of them.  The harness pins
`shrink_cooloff_s` for the run, because under K=2 of 6 a node's share of
the keyspace sits below the shrink threshold and rc16 would resize it
mid-cell - correct on a fleet, wrong inside a measurement.  Rows, with
p50, pulls and forward counts per cell and a `clients` column saying
which farm drove each: `results/spreadbench.tsv`.

## What bounds a loopback read

**Most of the older tables are one host, loopback — and for reads that
is not a small caveat.**  Loopback has a 64 KB MTU.  A pipelined batch
of GET responses is one segment there and nine on an ordinary 1500-byte
network, and read throughput tracks that directly.  Same host, same
binary, same container, only the MTU changed:

| MTU | SET/s | GET/s | GET/SET |
|---|---|---|---|
| 1500 (bridge) | 1,351,784 | 730,161 | 0.54 |
| 9000 (bridge) | 1,370,301 | 793,905 | 0.58 |
| 65536 (loopback) | 1,852,444 | 1,786,286 | 0.96 |

**Writes barely move; reads nearly triple.**  So loopback GET figures
are a best case that a real 1500-byte network does not reproduce, and a
deployment that can raise its MTU should.  Found by running the
container harness on someone else's machine; nothing on loopback would
ever have shown it.

Part of what used to sit under "not yet understood" was not the wire at
all.  Every GET allocated and freed a buffer it did not need - the
record was already copied out into a per-thread scratch, and the verb
layer then malloc'd a second one, copied again, wrote it to the socket
and freed it.  Removing that (`78b122b`) more than doubled reads where
the *server* is the bottleneck: on an 8-vCPU host across a bridge, same
harness and configuration either side of the commit, GET went 483,246
-> 1,099,253 (+127%) with its p99 5.4x tighter, while Redis moved 2.0%.
It changed nothing on loopback, where the client is the limit and freed
server CPU has nowhere to go - which is why it hid for so long.

A read/write gap remains at lower MTU (0.54 and 0.58 against 0.96 on
loopback) and that part still is not fully explained.  Segmentation is
the obvious candidate and is clearly not all of it.

## The older result page

`CONTAINERS.md` is an earlier page whose method is current and whose
numbers are not; it says so at the top.
