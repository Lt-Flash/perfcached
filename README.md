<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="contrib/brand/perfcached-lockup-dark.svg">
    <img src="contrib/brand/perfcached-lockup-light.svg" width="520" alt="perfcached - found fast, kept safe">
  </picture>
</h1>

<p align="center">
  A multithreaded cache for Linux, with its own encrypted protocols and
  a Redis-compatible door.  One node to start; a cluster when you need it.
</p>

perfcached is an in-memory key-value cache.  It runs as one daemon that
serves requests on every core, keeps data in memory with optional
write-ahead logging and snapshots, and is watched through a built-in
status page and Prometheus metrics.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="contrib/brand/perfd-mark-dark.svg">
  <img src="contrib/brand/perfd-mark-light.svg" width="72" align="right" alt="libperfd - the compass">
</picture>

Clients speak perfcached's own protocol: binary frames, encrypted with
Noise.  `libperfd`, the C client library (MIT), and `perfcli`, the
command-line client, use it; for scripts, `perfcli -j` takes a request
typed as JSON and sends it as the binary request.  For applications
already written against Redis, it also answers the Redis protocol
(RESP2), so
unmodified Redis clients such as `redis-cli` and hiredis work too.

```
$ perfcli -a "$SECRET" set cache greeting hello 60
{"stored":true}
$ perfcli -a "$SECRET" get cache greeting
{"found":true,"value":"hello","ttl":60}
$ redis-cli GET greeting
"hello"
```

When one machine is no longer enough, run the same binary on several:
they find each other, elect a master and share the keyspace.

## Why perfcached

- **Its own protocol, encrypted by default.**  Binary frames over
  Noise (libsodium), with typed verbs, binary-safe values and
  pipelining in libperfd.
- **Uses every core.**  Requests are served by a pool of worker threads,
  not one event loop.  On the same 16-vCPU host, over a real network,
  four workers answer 2.2x Redis 8's SET rate and 1.4x its GET rate at
  pipeline depth 64 ([measured](#performance)).
- **Durable when you want it.**  A write-ahead log plus snapshots, with
  a storage probe that recommends an fsync policy for your disks.  A
  node that loses log records heals itself from memory instead of going
  down.
- **Redis clients work too.**  The Redis door is guarded by an address
  allow-list and `AUTH`, and answers strings with TTLs, counters,
  `SET ... NX` locks, RedisJSON documents, pub/sub, keyspace
  notifications, `SCAN`, `INFO` and `SLOWLOG`.  The full list with every
  difference is in [REDIS-COMMANDS.md](REDIS-COMMANDS.md).
- **Easy to watch.**  A status page, `/metrics` for Prometheus (a
  Grafana dashboard is in [contrib/grafana](contrib/grafana)), and
  `/stats` as JSON.
- **Changes while it runs.**  Collections are created, resized,
  renamed and dropped live.  Memory grows under load and is given back
  when idle.
- **A standalone edition** leaves every line of cluster code out of
  the build, for a cache that will only ever be one node
  ([choosing a build](#choosing-a-build)).

## When one node is not enough

- **Clusters without a coordinator.**  Nodes find each other - over
  multicast on one network segment, or through a few seed names where
  there is no multicast (clouds, Kubernetes) - elect a master that owns
  a versioned placement map, and rejoin on their own after a crash.
  There are no node ids or peer lists to configure.
- **Five ways to place data**, from every node holding everything to
  exactly one owner per key.  Choose for availability, memory or write
  throughput ([the modes](#cluster-modes)).
- **Clients that follow the fleet.**  libperfd keeps spare connections,
  fails over in one `send()` and can send each key straight to its
  owner.  Redis clients that understand Redis Cluster read `CLUSTER
  SLOTS` and route themselves.
- **Pub/sub across the fleet**: a publish on any node reaches the
  subscribers on every node.
- **On a LAN or in a cloud.**  By default membership, heartbeats and
  the placement map travel by multicast on one L2 segment (TTL 1;
  `[cluster] multicast_ttl` lets multicast routing carry them across
  segments, verified with one router in a lab rig).  Where there is no
  multicast - AWS, GCP, Azure, Kubernetes - `discovery = unicast`
  finds the fleet through seed names, sends anything larger than the
  path MTU over TCP so nothing fragments, and tells a peer whose UDP is
  blocked from a dead one (GUIDE.md, "In a cloud or Kubernetes").

## Status

The last tagged release is **0.5.6.1** (2026-10-07).  It is young
software and we want to hear how it behaves on your workload.  Please
[open an issue](https://github.com/Lt-Flash/perfcached/issues) for
anything odd, and include the output of `perfcached -V`, your config
(secrets removed) and the log around the event.

Known limits in 0.5.6.1:

- From 0.4.4 or earlier, the upgrade replaces a fleet whole: since
  0.4.5 the native door speaks one binary protocol (the JSON-RPC
  dialect is gone) and the cluster wire epoch changed, so stop every
  node, start them all on the new version, and move every client to
  libperfd 0.3.0 or later (or the RESP door) at the same time.  State
  from 0.4.1 or later is kept.  No upgrade path from 0.3.x.  From
  0.4.5, 0.5.6.1 is a rolling restart; use SET IFEQ/IFNE and DELEX once
  every node runs 0.5.0 or later.  A node keeps the identity an
  earlier release minted at random, and its log says how to switch to
  one derived from the machine.
- No sorted sets, lists or streams yet.  Keys hold strings, counters,
  JSON documents or hashes (a hash is one value, under 58,000 bytes).
- Linux only (x86_64, arm64, arm32, i386), on glibc or musl.
- The write-ahead log has a ceiling: on a test host it keeps up with
  about 1,000,000 small SETs a second under `fsync = everysec`, but a
  write rate faster than the disk takes it, or a log that fills before
  a snapshot completes, drops acknowledged writes from the log.  They
  are still in memory; the node logs an ERROR, refuses writes, takes a
  snapshot that makes them durable again and returns to service by
  itself.  Size the disk and the log for the write rate, or run without
  a log where a cache may lose writes on a crash.
- In a cluster: one L2 segment by default; across routed links only with
  `multicast_ttl` and multicast routing, not measured over WAN links;
  `discovery = unicast` (clouds, Kubernetes) is tested in lab network
  namespaces that drop IP fragments and block UDP, not yet in a real
  cloud deployment, and suits a fleet of a few dozen nodes; `spread`
  mode has not yet carried production traffic; and in `eager` mode a
  delete made on one side of a network partition can be undone when
  the partition heals.
- Read-through from a Redis upstream carries strings, hashes and
  RedisJSON documents (a list, set, sorted set or stream answers
  `WRONGTYPE`); `KEYS` and `SCAN` see only what perfcached holds;
  `proxy`, `shard` and `spread` collections refuse an upstream; and
  tracking the upstream's changes needs every connection on one Redis
  server and, from 0.5.6, an upstream user allowed `INFO`.

## Install

**Debian and Ubuntu:** `.deb` packages for Debian 13 and 12 and
Ubuntu 24.04 and 20.04 (amd64) are on the
[releases page](https://github.com/Lt-Flash/perfcached/releases), in
both editions (see [Choosing a build](#choosing-a-build)):
`perfcached` is the clustered daemon and `perfcached-standalone` the
single-node one.  They install the same files and replace each other.

```
sudo apt install ./perfcached_0.5.6.1-1+deb13_amd64.deb
sudo apt install ./perfcached-standalone_0.5.6.1-1+deb13_amd64.deb   # or this one
```

**From source:** needs a C compiler, make and libsodium.

```
make                 # perfcached, perfcli, perfdump, perfload, libperfd.a
sudo make install    # binaries, example config, systemd unit
```

### Choosing a build

`make menuconfig` chooses which daemon `make` builds and saves the
choice in `config.mk`:

| edition | build it for | what you get |
|---|---|---|
| `clustered` (the default) | anything that may grow beyond one node | every cluster mode: eager, store, shard, proxy, spread |
| `standalone` | a cache that will only ever be one node | no cluster code at all: the binary is a third smaller, its static memory 7 MB less, and a `[cluster]` section in the config is refused rather than ignored |

Without a `config.mk` the build is `clustered`.  The `.deb` packages
come in both editions: `perfcached` and `perfcached-standalone`.  To
choose without the menu, for scripts and CI:

```
make menuconfig                      # interactive (whiptail, dialog or plain prompts)
sh tools/menuconfig.sh standalone    # the same, non-interactive
make PC_EDITION=standalone           # one build, without writing config.mk
```

A standalone daemon reports `standalone` in `perfcached -V` and
`"edition"` in `/stats`.  Its tests are `make check-standalone`; the
clustered suites need the cluster code and `make check` says so.

**The allocator** is the second thing `make menuconfig` asks (or `make
PC_ALLOC=libc|mimalloc|jemalloc`): the C library's own malloc by
default, or mimalloc 2.x or jemalloc linked ahead of it.  The C library
itself is the toolchain's - glibc on Debian, musl on Alpine.
`perfcached -V` names a non-default allocator, and `/stats` carries
`process.allocator`.  After changing it on the command line rather than
through `make menuconfig`, `make clean` first.

## Quick start

### A single node

The native door on the network, and the Redis door on
localhost.  Save this as
`/etc/perfcached/perfcached.conf` (mode 640):

```ini
[daemon]
workers = 4                      # the number of cores

[memory]
arena_mb = 512                   # memory for keys and values

[secrets]
client = pick-a-client-secret    # for the native, encrypted protocol
enable = pick-an-enable-secret   # privileged commands only (perfcli -E)

[listen]
tcp = 0.0.0.0:6479               # native protocol (encrypted)
resp = 127.0.0.1:6379            # Redis protocol (optional)
resp_collections = 0:cache       # Redis database 0 is the collection "cache"

[collection cache]
buckets_log2 = 16
```

Three secrets, all different, none shared with anyone who does not
need it: `client` is what applications present; `enable` raises one
connection's privilege (`perfcli -E`) for the commands that change the
node or the fleet - creating, resizing and dropping collections (with
`[daemon] allow_create = yes`) and `fleetstop`; the `cluster` secret,
below, only the daemons hold.  Without an `enable` secret those
commands cannot be run at all.

Check it and start it:

```
perfcached -f /etc/perfcached/perfcached.conf -C    # validate, show what it would run
sudo systemctl enable --now perfcached
perfcli -a pick-a-client-secret set cache greeting hello 60
perfcli -a pick-a-client-secret get cache greeting
redis-cli -p 6379 GET greeting
```

To serve Redis clients across a network, give the door a LAN address
plus `resp_allow` and a `[secrets] resp` password; the daemon refuses
to open it off-box without an allow-list.

### Growing into a cluster

When one node is no longer enough, add machines.  On one L2 segment
the nodes find each other by multicast: it is sent with a TTL of 1 and
does not cross a router unless `[cluster] multicast_ttl` raises it for
multicast routing (PRODUCTION.md has the checklist).  Add the same
`[cluster]` section to the config on every machine and start them in
any order:

```ini
[cluster]
multicast = 239.68.68.1:6480     # same group on every node
mode = eager                     # every node holds every key
collections = cache

[secrets]
client = pick-a-client-secret
enable = pick-an-enable-secret
cluster = a-different-secret     # only the daemons hold this one
```

Between the nodes, open port 6480 for both UDP (membership,
replication, reads from peers) and TCP (large records and a new node's
initial copy).  UDP 6481 is optional: it keeps pub/sub relays off the
membership socket, and when it is closed they travel on 6480 instead.

**No multicast** (AWS, GCP, Azure, Kubernetes, or a LAN that filters
it): name a few members as seeds instead of a group.  Every node
re-resolves the names, so a seed may be down or move:

```ini
[cluster]
discovery = unicast
port = 6480
seeds = cache-0.cache, cache-1.cache, cache-2.cache
expect = 3                       # a cold start waits for all three
mode = eager
collections = cache
```

The ports are the same, plus TCP 6482: frames larger than the network's
MTU go over a TCP connection between the two nodes instead of as IP
fragments.  [GUIDE.md](GUIDE.md) ("In a cloud or Kubernetes") covers
Kubernetes and node identity.
Within seconds `perfcli -a
<client secret> stats` shows the members and which node is master.
Stop a node and the others carry on; start it again and it rejoins.

Complete, validated configurations for single nodes and every cluster
mode are in [contrib/CONFIGURATIONS.md](contrib/CONFIGURATIONS.md).
Every option is annotated in
[contrib/perfcached.conf.example](contrib/perfcached.conf.example).

## Cluster modes

One cluster runs one mode.  The modes differ in how many nodes hold a
key, and which.

| mode | copies of a key | memory per node | when a node dies |
|---|---|---|---|
| `eager` | every node | the whole keyspace | nothing is lost |
| `spread` | K nodes (you pick K) | K/P of the keyspace | survives K-1 failures |
| `shard` | one owner, by hash | 1/P | its share is unavailable |
| `proxy` | one holder, by free memory | 1/P | its share is unavailable |
| `store` | nodes that read it | what each node reads | keys read only there are lost |

In short: use `eager` for three nodes that must not lose anything,
`spread` for four or more where you want a fixed redundancy, and
`shard` for throughput with a client that routes by key.  Durability is
separate from copies.  Only the node that took a write logs it, so
turn on the WAL and snapshots if the whole fleet may stop at once.
[GUIDE.md](GUIDE.md#the-modes) has the trade-offs in full.

## Clients

- **libperfd**, the C client (MIT, so it can go in proprietary code).
  It is encrypted and pipelined, and it follows the fleet: it keeps
  spare connections, fails over in one `send()`, and can route each key
  straight to its owner.  See [lib/README.md](lib/README.md).
- **perfcli**, the `redis-cli` of the native protocol
  ([PERFCLI-COMMANDS.md](PERFCLI-COMMANDS.md)); for scripts, `-j` takes
  a request typed as JSON and sends it as the binary request (plain
  text on localhost with `plaintext = loopback`, encrypted otherwise).
  Plus **perfdump** and **perfload** for parallel backup and restore
  between fleets of any size or mode.
- **Any Redis client** through the Redis door (RESP2).  Cluster-aware
  clients read `CLUSTER SLOTS` and send each key to its owner.
- **Asterisk**: dialplan functions in [contrib/asterisk](contrib/asterisk).

## Performance

`redis-benchmark`, unmodified, against one perfcached 0.4.0 node (4
workers) and against `redis-server` 8.10.1, each on the same 16-vCPU
host, with the client on a second host across a real network.  20k
keys of 200 bytes, 50 clients, median of 3:

| | SET/s | GET/s | SET p99 | GET p99 |
|---|---|---|---|---|
| redis-server, no pipelining | 41,824 | 46,468 | 1.54 ms | 1.47 ms |
| perfcached, no pipelining | 37,425 | 44,703 | 1.08 ms | 1.00 ms |
| redis-server, pipeline 64 | 760,746 | 948,317 | 5.39 ms | 4.98 ms |
| perfcached, pipeline 64 | **1,643,385** | **1,314,060** | **1.73 ms** | **1.74 ms** |

With no pipelining, both servers wait on the network round trip.  With
pipelining the servers themselves are the limit, and perfcached's
workers pull ahead, at a third of Redis's tail.  The lead comes from the
workers, not from a cheaper command: per core, a SET and its write-ahead
log record cost perfcached more than a SET costs a Redis that persists
nothing, so a node with two workers can trail one Redis thread on SET.
Three-node fleets through routing clients reach 3.7 to 6.7 million
GET/s.  A `KEYS` that walks 74,000 keys 50 times a second costs Redis's
other clients a quarter of their GET rate; perfcached's keep theirs -
the `KEYS` caller waits longer instead.  Every table, its method and how
to reproduce it: [GUIDE.md](GUIDE.md#measured) and
[bench/RESULTS.md](bench/RESULTS.md).

## Documentation

- [GUIDE.md](GUIDE.md): running and operating it.  Nodes and fleets, the
  modes, durability, sizing, the status page, runtime collections, the
  tools, the clients, and all measurements.
- [REDIS-COMMANDS.md](REDIS-COMMANDS.md): every Redis command it
  answers, and what an unsupported one gets.
- [contrib/CONFIGURATIONS.md](contrib/CONFIGURATIONS.md): ten complete
  configurations.
- [PRODUCTION.md](PRODUCTION.md): the pre-deployment checklist.
- [PERFCLI-COMMANDS.md](PERFCLI-COMMANDS.md): the perfcli reference.
- [lib/README.md](lib/README.md): libperfd.
- [doc/perfdump-format.md](doc/perfdump-format.md): the dump format.
- [ChangeLog](ChangeLog): what each release changed.

## Building and testing

```
make check             # every test suite (clustered edition)
make check-asan        # the same under AddressSanitizer + UBSan
make check-standalone  # the suite for the standalone edition
tools/matrix.sh        # build and test all four architectures (podman + qemu)
tools/alpine-check.sh  # Alpine (musl, gcc 15): both editions + check-standalone
```

CI runs lint and the fast suite on every push, and the full suite,
the sanitizer suite, the four-architecture matrix and the Alpine check
on every release tag.  Debian and Alpine - glibc and musl - are both
built and tested; `bench/Containerfile.alpine` is the small image.

## Compatibility

perfcached is pre-1.0 and makes no compatibility promise between
releases.  The native protocols, the cluster wire, the write-ahead log
and the snapshot format can change in any release.  An upgrade across
such a change means stopping every node, clearing its state and
starting the new version empty, as from 0.3.x to 0.4.0.  Run one
version across a fleet, and read a release's notes before upgrading.

## License

- The daemon and its tools: **GPL-2.0-or-later** ([COPYING](COPYING)).
- libperfd, the client library: **MIT** ([lib/LICENSE](lib/LICENSE)),
  so applications of any license can link it.

Each source file carries an SPDX line, and CI checks that the MIT side
never includes GPL code.  libsodium is ISC (see
[lib/NOTICE](lib/NOTICE)).  Contributions need a DCO sign-off; see
[CONTRIBUTING.md](CONTRIBUTING.md).
