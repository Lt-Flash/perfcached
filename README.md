<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="contrib/brand/perfcached-lockup-dark.svg">
    <img src="contrib/brand/perfcached-lockup-light.svg" width="520" alt="perfcached - found fast, kept safe">
  </picture>
</h1>

<p align="center">
  A multithreaded cache for Linux, with its own authenticated protocol
  and a Redis-compatible door.  One node to start; a cluster when you need it.
</p>

perfcached is an in-memory key-value cache for small, hot state -
sessions, routing, counters, call state.  One daemon serves requests on
every core, keeps data in memory with an optional write-ahead log and
snapshots, and is watched through its dashboard and Prometheus metrics.
When one machine is not enough, run the same binary on several: they
find each other, elect a master and share the keyspace.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="contrib/brand/perfd-mark-dark.svg">
  <img src="contrib/brand/perfd-mark-light.svg" width="72" align="right" alt="libperfd - the compass">
</picture>

Clients speak perfcached's own protocol - binary frames opened with a
Noise handshake that proves the client secret - through `libperfd`, the
C client library (MIT), or `perfcli`, the command-line client.  Redis
clients such as `redis-cli` and hiredis work unmodified through the
Redis door (RESP2).

```
$ perfcli -a client-secret-example set sessions user:17 'hello world' 300
{"stored":true}
$ perfcli -a client-secret-example get sessions user:17
{"found":true,"value":"hello world","ttl":300}
$ redis-cli -p 6380 -a resp-password-example --no-auth-warning GET user:42
alice
```

**Current release: 0.7.5** (2026-11-22), with libperfd 0.7.6 and
php-perfcached 0.1.1.  [GUIDE.md](GUIDE.md) shows how to install it,
build a cluster, take over from Redis, upgrade, back up and watch it,
with real output.

## What it does

- **Its own protocol, authenticated at connect.**  Typed verbs,
  binary-safe values and pipelining; requests in the clear after the
  handshake by default (half the CPU), or encrypted with `[listen]
  encrypt = yes`.
- **Uses every core.**  A pool of worker threads, not one event loop:
  on one 16-vCPU host over a real network, four workers answer 2.2x
  Redis 8's SET rate and 1.4x its GET rate at pipeline depth 64
  ([Performance](#performance)).
- **Durable when you want it.**  A write-ahead log plus snapshots, with
  a storage probe that recommends an fsync policy.  A node that loses
  log records heals itself from memory instead of going down.
- **Redis clients work too.**  Strings with TTLs, counters, `SET ... NX`
  locks, RedisJSON documents, hashes, pub/sub, keyspace notifications,
  `SCAN`, `INFO`, `SLOWLOG`, behind an address allow-list, `AUTH` and
  optional TLS ([REDIS-COMMANDS.md](REDIS-COMMANDS.md)).
- **Takes over from Redis** without a big-bang copy: a collection reads
  a missing key from Redis on first use, or writes through to it
  ([GUIDE.md](GUIDE.md#read-through-taking-over-from-redis)).
- **Easy to watch.**  A dashboard (`perfcached-dashboard`, its own
  process), `/metrics` for Prometheus (a Grafana dashboard is in
  [contrib/grafana](contrib/grafana)) and `/stats` as JSON.  A node
  raises its own alerts and, in a build with mail, sends them over SMTP;
  in a fleet `[mail]` on one node mails for all
  ([doc/alerts.md](doc/alerts.md#mail)).
- **Changes while it runs.**  Collections are created, resized, renamed
  and dropped live; memory grows under load and is given back when idle.
- **Clusters without a coordinator.**  Nodes find each other by
  multicast on one segment, or through seed names where there is none
  (clouds, Kubernetes - [doc/cluster.md](doc/cluster.md#unicast-discovery-clouds-and-kubernetes)),
  elect a master and rejoin by themselves.  No node ids or peer lists to
  configure.  Five ways to place data ([Cluster modes](#cluster-modes)).
- **Clients that follow the fleet.**  libperfd keeps standby
  connections, fails over in one `send()` and can send each key to its
  owner; cluster-aware Redis clients read `CLUSTER SLOTS`.
- **A standalone edition** leaves every line of cluster code out, for a
  cache that will only ever be one node.

## Known limits

- **WARNING - size limits.**  A record (key, value and a 28-byte header)
  is at most 262,080 bytes, so **a value is at most just under 256 KB**;
  larger is refused (`value too large`).  A JSON document is at most
  64 KB, a hash (one value) under 58,000 bytes, a key 4,096 bytes.  In a
  cluster a write this node forwards to the key's owner carries at most
  58,000 bytes of value.  perfcached holds small, hot state - it is not
  a store for large objects or a web page cache.
- No lists, sets, sorted sets, streams, bitmaps, HyperLogLog or geo.
  Keys hold strings, counters, JSON documents or hashes.
- No transactions (`MULTI`/`EXEC`/`WATCH`) and no general scripting:
  `EVAL`/`EVALSHA` run only the approved Symfony Lock scripts.
- Linux only (x86_64, arm64, arm32, i386), on glibc or musl.
- The write-ahead log's ceiling is the storage's: about 1,000,000 small
  SETs a second under `fsync = everysec` on a fast disk, but with 1 KB
  records the log's writer tops out near 180,000 a second, and a disk
  that absorbs about 35 MB/s falls behind at about 35,000.  Past that,
  writers wait for the log ([doc/durability.md](doc/durability.md)).
- A cluster is one L2 segment by default; across routed links only with
  `multicast_ttl` and multicast routing, not measured over WAN links.
  `discovery = unicast` is tested in network namespaces that drop IP
  fragments and block UDP, not yet in a real cloud deployment, and suits
  a fleet of a few dozen nodes.  `spread` mode has not carried
  production traffic.
- Read-through and write-through carry strings, hashes and RedisJSON
  documents (a list, set, sorted set or stream answers `WRONGTYPE`);
  `KEYS` and `SCAN` see only what perfcached holds; `proxy` collections
  refuse an upstream; tracking Redis's changes needs every connection on
  one Redis server and an upstream user allowed `INFO`.  Write-through
  does not bridge pub/sub, costs each write a round trip to the master,
  and two clients writing one key through two nodes at the same moment
  can leave Redis and the fleet with different winners.
- The RESP and HTTP doors are plaintext unless their listener says
  `tls` (a build with OpenSSL).  Under `upstream_writes = through` the
  RESP door can write to Redis: keep it on a trusted segment, behind
  `resp_allow` and `[secrets] resp`, or on TLS.
- **Upgrading to 0.7.5 from 0.7.4.x is a whole-fleet stop**: 0.7.5
  speaks cluster wire epoch 4 alone and refuses an older node.  Stop
  every node (`perfcli -E <enable> fleetstop`), install 0.7.5, start
  them all; the state is kept ([GUIDE.md](GUIDE.md#upgrade)).  Use
  libperfd 0.7.6 or later with it: an older libperfd does not route by
  key against 0.7.5 (every request takes a forward hop) and is refused a
  value over 58,000 bytes.
- **`[cluster] tombstone_acks`**: a fleet running `no` cannot be
  switched to `yes` by a rolling restart - a node on `yes` among members
  on `no` loops.  Until 0.7.5.1, leave a fleet that runs `no` on `no`
  (a switch to `yes` needs every node changed and the fleet restarted
  whole).

**Known issues in 0.7.5**, fixed in **0.7.5.1**, the fix release
coming next:

- Write-through (`upstream_writes = through`): a value too large for
  one record (about 256 KB with its key) is still sent to Redis, which
  takes it, and the client gets OK, but the fleet cannot hold it.  An
  older copy of that key may keep answering until tracking invalidates
  it (for good with `upstream_tracking = off`); after that, reads fail
  as too large.  Keep write-through values under 256 KB until 0.7.5.1.
- A node rejoining while a reconcile pass is open can report ready
  before its copy of the fleet's state has finished, and for that
  moment answer a read with a key deleted while it was away, or an
  older value.
- A collection change that comes from the fleet (a resize, an autoscale
  or mode change) writes the collections file on the cluster thread.
  On slow storage that stalls the forwards and pulls the thread
  carries for as long as the fsync takes, and a read routed through
  such a node can answer as a miss.

0.7.5.1 also carries the fixes from a full code review of 0.7.5:
memory-safety fixes, refusals for malformed input, and corrections in
the cluster, the upstream bridge, the client libraries and the tools.

Report anything odd as an
[issue](https://github.com/Lt-Flash/perfcached/issues), with `perfcached
-V`, the config (secrets removed) and the log around the event.  A
security problem goes privately, as [SECURITY.md](SECURITY.md) says.

## Install

**Debian and Ubuntu:** `.deb` packages for Debian 13 and 12 and Ubuntu
24.04 and 20.04 (amd64) are on the
[releases page](https://github.com/Lt-Flash/perfcached/releases):
`perfcached` (the clustered daemon) or `perfcached-standalone` (one
node only) - they install the same files and replace each other - and
`perfcached-dashboard` (Debian 13 and 12, Ubuntu 24.04).  On Alpine all
three come as signed `.apk` files; a release tarball installs under
`/opt/perfcached-0.7.5`.

```
sudo apt install ./perfcached_0.7.5-1+deb13_amd64.deb
```

**libperfd** 0.7.6: `libperfd0` (the shared library) and `libperfd-dev`
(header, static archive, pkg-config file) as `.deb`, `libperfd` /
`libperfd-dev` as `.apk`.  A program built against `libperfd-dev`
links the shared library, so a libperfd update with the same SONAME is
an install and a restart, no rebuild ([lib/README.md](lib/README.md)).

**From source:** a C compiler, make and libsodium (OpenSSL for TLS):
`make && sudo make install && sudo systemd-sysusers`.  The editions, the
allocator, debug and sanitizer builds, mail and TLS:
[doc/building.md](doc/building.md).

## Quick start

`/etc/perfcached/perfcached.conf`, owned by root, group `perfcached`,
mode 640:

```ini
[daemon]
workers = 4                      # the number of cores

[memory]
arena_mb = 512                   # memory for keys and values

[secrets]
client = client-secret-example   # what applications present
cluster = cluster-secret-example # only the daemons hold it
enable = enable-secret-example   # privileged commands (perfcli -E)
resp = resp-password-example     # the Redis door's AUTH

[listen]
tcp = 0.0.0.0:6479               # native protocol
resp = 127.0.0.1:6380            # Redis protocol (optional)
resp_collections = 0:sessions    # Redis database 0 is the collection "sessions"

[collection sessions]
buckets_log2 = 16
```

```
perfcached -f /etc/perfcached/perfcached.conf -C    # check it
sudo systemctl enable --now perfcached
perfcli -a client-secret-example ping
```

All secrets differ.  To serve Redis clients across a network, give the
door a LAN address, `resp_allow` and TLS (`resp = <addr:port> tls`).
For a cluster, add the same `[cluster]` section to every node and start
them in any order:

```ini
[daemon]
state_dir = /var/lib/perfcached

[cluster]
multicast = 239.68.68.1:6480     # same group on every node
mode = eager                     # every node holds every key
collections = sessions
```

Open UDP and TCP 6480 and 6481 between the nodes (and TCP 6482 with
`discovery = unicast`).  `perfcli fleet-status` lists the members.
Step by step, with the output to expect: [GUIDE.md](GUIDE.md).
Complete configurations for single nodes and every mode:
[contrib/CONFIGURATIONS.md](contrib/CONFIGURATIONS.md); every option:
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

Use `eager` for three nodes that must not lose anything, `spread` for
four or more where you want a fixed redundancy, and `shard` for
throughput with a client that routes by key.  Durability is separate
from copies: only the node that took a write logs it, so turn on the
WAL and snapshots if the whole fleet may stop at once.
[doc/modes.md](doc/modes.md) has the trade-offs in full.

## Clients

- **libperfd**, the C client (MIT): authenticates, pipelines and
  follows the fleet ([lib/README.md](lib/README.md)).
- **perfcli**, the `redis-cli` of the native protocol
  ([PERFCLI-COMMANDS.md](PERFCLI-COMMANDS.md)); **perfdump** and
  **perfload** for parallel backup and restore between fleets of any
  size or mode; **perfcached-dashboard**, the status page.
- **PHP**: an extension over libperfd for PHP 8.1 or later
  ([lang/php/README.md](lang/php/README.md)), with a Symfony cache adapter.
- **Any Redis client** through the Redis door.
- **Asterisk**: dialplan functions in [contrib/asterisk](contrib/asterisk/README.md).

## Performance

**Turn NIC multiqueue on - one queue per CPU, on the nodes and the
clients - and keep the default io_uring backend
([PRODUCTION.md](PRODUCTION.md), section 7).**  With one receive queue,
one CPU handles everything a host receives and caps it.

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

With no pipelining both servers wait on the network round trip; with
pipelining perfcached's workers pull ahead, at a third of Redis's tail.
The lead comes from the workers, not from a cheaper command: per core, a
SET and its write-ahead log record cost perfcached more than a SET costs
a Redis that persists nothing, so a node with two workers can trail one
Redis thread on SET.  Three-node fleets through routing clients reach
3.7 to 6.7 million GET/s.  A `KEYS` that walks 74,000 keys 50 times a
second costs Redis's other clients a quarter of their GET rate;
perfcached's keep theirs - the `KEYS` caller waits longer instead.
Every table, its method and how to reproduce it:
[doc/benchmarks.md](doc/benchmarks.md) and
[bench/RESULTS.md](bench/RESULTS.md).

## Upgrading

- **0.7.4.x to 0.7.5**: a whole-fleet stop (above).
- **Within the support window** - from 0.7.5 to a later 0.7.5.x patch or
  to the next minor's releases - a rolling restart: members first, the master last,
  one node at a time, each only once its `stats.cluster.restart.safe`
  says so and the fleet reads healthy (every node `ready`, peers up,
  record counts not fallen).  From an older minor, step through the last
  patch of each minor in between.  Mixed versions are for the roll, not
  for running; a release note names any exception.
- Packages do not restart the daemon - install, then restart node by
  node ([PRODUCTION.md](PRODUCTION.md), section 8).  A tarball install
  upgrades through the staged build and the helper
  ([doc/operations.md](doc/operations.md#upgrades), "The upgrade helper").
- The client library of the same or the previous minor talks to both
  sides of a roll; against 0.7.5 use libperfd 0.7.6 or later.

What may change between releases: [doc/compatibility.md](doc/compatibility.md).

## Documentation

- [GUIDE.md](GUIDE.md): how to do the common things - install, a
  cluster, Redis takeover, TLS, upgrades, backups, watching it,
  troubleshooting - with real output; it lists the reference documents
  in [doc/](doc/) (modes, durability, upstream, config, alerts, tools,
  clients, measurements).
- [PRODUCTION.md](PRODUCTION.md): the pre-deployment checklist.
- [REDIS-COMMANDS.md](REDIS-COMMANDS.md) and
  [PERFCLI-COMMANDS.md](PERFCLI-COMMANDS.md): every command of each door.
- [contrib/CONFIGURATIONS.md](contrib/CONFIGURATIONS.md): eleven
  complete configurations.
- [lib/README.md](lib/README.md): libperfd.
- [doc/building.md](doc/building.md): building and testing.
- [ChangeLog](ChangeLog): what each release changed.

## License

- The daemon and its tools: **GPL-2.0-or-later** ([COPYING](COPYING)).
- libperfd, the client library: **MIT** ([lib/LICENSE](lib/LICENSE)),
  so applications of any license can link it.
- The PHP extension and its Symfony adapter: **MIT**
  ([lang/php/LICENSE](lang/php/LICENSE)).

[LICENSING.md](LICENSING.md) says which files are on which side.  Every
file libperfd is built from carries an MIT SPDX line, a file with no
SPDX line is GPL-2.0-or-later, and the test suite checks that the MIT side
never includes GPL code.  libsodium is ISC (see [lib/NOTICE](lib/NOTICE)).
Contributions need a DCO sign-off; see [CONTRIBUTING.md](CONTRIBUTING.md).
