# Example configurations

Ten complete configurations - four single-node, five fleets, and a
second daemon joining a second fleet on the same host.  Each is a
whole file, not a fragment: copy it to `/etc/perfcached/perfcached.conf`
(chmod 640), replace every `CHANGE-ME` secret, put your own addresses in
place of the documentation ranges (`192.0.2.0/24`, `198.51.100.0/24`,
`203.0.113.0/24`), and validate before starting:

    perfcached -f /etc/perfcached/perfcached.conf -C   # parse, check, report
    perfcached -f /etc/perfcached/perfcached.conf -E   # the effective config, secrets masked

Every file here passed `perfcached -C` on the build it ships with, and
the checker refuses the things these files are careful about: a cluster
secret equal to a client secret, a RESP listener off loopback without
`resp_allow`, `spread` without `replicas`.  Every option, annotated,
is in [perfcached.conf.example](perfcached.conf.example); what the
sections mean is in the README under *Running it*.

Two rules that every fleet example follows.  **The same file goes on
every node** - membership is automatic, node ids are assigned by the
elected master, and the only per-node line is `advertise`, needed only
on a multi-homed host.  **The cluster owns the collection config**:
`mode` and `collections` under `[cluster]` are authoritative for the
whole fleet, and a node whose file differs is refused at join, by name.

| # | configuration | shows |
|---|---|---|
| 01 | [One node, minimal](#one-node-minimal) | In-memory only, one encrypted door, one collection.  The smallest thing that serves. |
| 02 | [One node for Redis clients](#one-node-for-redis-clients) | The RESP door on the LAN with its three guards, the status page, a local socket for tools, and `SELECT n` mapped onto named collections. |
| 03 | [One node that survives a restart](#one-node-that-survives-a-restart) | Identity and runtime-created collections in `state_dir`, a WAL with snapshots, runtime DDL behind the `enable` secret, a sampled query log. |
| 04 | [One node whose memory follows the load](#one-node-whose-memory-follows-the-load) | An elastic arena that grows under pressure and gives idle memory back, and an index that grows and shrinks by itself. |
| 05 | [An eager fleet](#an-eager-fleet) | Every node holds every record.  The same file on every node; membership is automatic; the RESP door and the status page on each. |
| 06 | [A store fleet](#a-store-fleet) | Pull-on-miss: a node fetches what it misses from a peer and keeps the copy, so each node converges on its own working set. |
| 07 | [A proxy fleet](#a-proxy-fleet) | The capacity plane: each key on exactly one node placed by free memory, forwarded writes, served-through reads, a rebalancer.  Fleet capacity is the sum of the arenas. |
| 08 | [A shard fleet behind Redis clients](#a-shard-fleet-behind-redis-clients) | Deterministic ownership, and the slot map published on the RESP door so a cluster-aware Redis client sends every key to its owner. |
| 09 | [A spread fleet](#a-spread-fleet) | K copies of each record on nodes chosen by placement.  Per-node apply load falls as the fleet grows; reads want a routing client. |
| 10 | [Two fleets on one host](#two-fleets-on-one-host) | One cluster is one mode; a second daemon joins a second fleet on its own group, ports, state and identity salt. |

A collection stanza written for a fleet runs unchanged on a single
node: without a `[cluster]` section the daemon accepts a collection's
`mode` and `pull`, runs it as store, and names what it ignored once at
warning level, so the first node of a future fleet can carry the
fleet's file from day one.  The cluster secret is optional there, and
required, and distinct from the client secret, the moment a `[cluster]`
section appears.

## One node, minimal

In-memory only, one encrypted door, one collection.  The smallest thing that serves.

```ini
# One node, in-memory only: the smallest configuration that serves.
[daemon]
workers = 4                          # set it to the core count
log_level = notice

[memory]
arena_mb = 256                       # the hard ceiling; a full arena refuses writes

[secrets]
client = CHANGE-ME-client            # what clients present (repeat the line to rotate)
cluster = CHANGE-ME-cluster          # optional standalone; if set, must differ from the client secret

[listen]
tcp = 0.0.0.0:6479                   # the encrypted native door: binary, JSON-RPC and RESP

[collection sessions]
buckets_log2 = 16                    # index sizing only; it grows and shrinks by itself
```

## One node for Redis clients

The RESP door on the LAN with its three guards, the status page, a local socket for tools, and `SELECT n` mapped onto named collections.

```ini
# One node for Redis clients on the LAN, with the status page and a
# local socket for tools.
[daemon]
workers = 8
log_level = notice
slowlog_usec = 10000                 # SLOWLOG threshold, Redis's own default

[memory]
arena_mb = 2048

[secrets]
client = CHANGE-ME-client
cluster = CHANGE-ME-cluster
resp = CHANGE-ME-redis-AUTH          # the Redis AUTH password; differs from both above
http = CHANGE-ME-http-token          # the status page and /metrics, /stats, /members

[listen]
tcp = 0.0.0.0:6479                   # native, encrypted
unix = /run/perfcached/perfcached.sock
plaintext = loopback                 # plain dialects on 127.0.0.1 and the socket only
resp = 192.0.2.10:6379               # RESP2 only, plaintext: the guards below are required
resp_allow = 192.0.2.0/24, 198.51.100.0/24
resp_collections = 0:sessions, 1:counters, 2:presence   # SELECT 0/1/2 -> these
http = 192.0.2.10:8080
http_allow = 192.0.2.0/24
http_timeout = 5

[collection sessions]
buckets_log2 = 18
[collection counters]
buckets_log2 = 14
[collection presence]
buckets_log2 = 14
log_events = expired, remove         # one NOTICE line per expiry and delete, this collection only
```

## One node that survives a restart

Identity and runtime-created collections in `state_dir`, a WAL with snapshots, runtime DDL behind the `enable` secret, a sampled query log.

```ini
# One node that survives a restart: identity, WAL and snapshots.
[daemon]
workers = 8
log_level = notice
state_dir = /var/lib/perfcached      # node identity, term, runtime-created collections
allow_create = yes                   # clients may create/drop/resize - with the enable secret
query_log = sampled:100              # one request in a hundred, to the log
query_log_keys = hashed              # never the key itself

[memory]
arena_mb = 4096

[secrets]
client = CHANGE-ME-client
cluster = CHANGE-ME-cluster
enable = CHANGE-ME-enable            # the privilege secret: DDL and restore need it

[listen]
tcp = 0.0.0.0:6479

[wal]
dir = /var/lib/perfcached            # absolute; the segments and snapshots live here
probe = auto                         # measure the device at startup, recommend a policy
fsync = everysec                     # at most one second of acknowledged writes at risk
segment_mb = 64
segments = 8
save = 900 1                         # snapshot after 15 min if anything changed
save = 300 10000                     # or after 5 min and 10,000 changes
rdb_mb_s = 64                        # the snapshot walker's write rate

[collection sessions]
buckets_log2 = 18
[collection dialogs]
buckets_log2 = 16
[collection tokens]
buckets_log2 = 14
```

## One node whose memory follows the load

An elastic arena that grows under pressure and gives idle memory back, and an index that grows and shrinks by itself.

```ini
# One node whose memory follows the load: an elastic arena and a
# self-sizing index.
[daemon]
workers = 8
grow_at_pct = 75                     # split once a table passes 75% of its slots (default)
shrink_at_pct = 18                   # resize down once it sits below this (default: a quarter of grow)
shrink_cooloff_s = 60                # ...for this long since it last split, was resized or started

[memory]
arena_mb = 512                       # committed and pinned at start
arena_cap_mb = 4096                  # may grow to this, 2 MB at a time, under pressure
reclaim_keep = 4                     # drained chunks kept per size class for the next burst
reclaim_quiet_s = 300                # idle this long before a chunk is given back
reclaim_cooloff_s = 60               # no give-back this soon after a carve
reclaim_giveback = 1
reclaim_floor_mb = 256               # never give back below this
shrink_step_mb = 64                  # the most give-back one tick may do
pin = auto                           # require = refuse to start unless the arena is locked
huge_pages = auto                    # off = plain 4K pages, THP disabled for the process

[secrets]
client = CHANGE-ME-client
cluster = CHANGE-ME-cluster

[listen]
tcp = 0.0.0.0:6479

[collection cache]
buckets_log2 = 12                    # start at the floor; it grows as the keyspace does
```

## An eager fleet

Every node holds every record.  The same file on every node; membership is automatic; the RESP door and the status page on each.

```ini
# Three (or more) nodes, every node holding every record: the same file
# on every node.  Membership is automatic.
[daemon]
workers = 8
log_level = notice
state_dir = /var/lib/perfcached      # so a restart comes back as the SAME member
allow_create = yes

[memory]
arena_mb = 2048

[secrets]
client = CHANGE-ME-client
cluster = CHANGE-ME-cluster          # seals the peer plane; only daemons hold it
enable = CHANGE-ME-enable
resp = CHANGE-ME-redis-AUTH
http = CHANGE-ME-http-token

[listen]
tcp = 0.0.0.0:6479
resp = 0.0.0.0:6379
resp_allow = 192.0.2.0/24
resp_collections = 0:sessions
http = 0.0.0.0:8080
http_allow = 192.0.2.0/24, 203.0.113.0/24

[cluster]
multicast = 239.68.68.1:6480         # same group and cluster secret = same fleet
#advertise = 192.0.2.11              # only on a multi-homed host: the address peers use
mode = eager                         # ONE mode for the whole cluster
collections = sessions, dialogs      # the exhaustive clustered set; a differing peer is refused
pull_timeout_ms = 200                # how long a miss waits for a peer's answer

[wal]
dir = /var/lib/perfcached
fsync = everysec
save = 900 1

[collection sessions]
buckets_log2 = 18
[collection dialogs]
buckets_log2 = 16
```

## A store fleet

Pull-on-miss: a node fetches what it misses from a peer and keeps the copy, so each node converges on its own working set.

```ini
# Store mode: a node pulls a key it misses from a peer and keeps the
# copy, so each node converges on ITS working set - for read-heavy
# shared data where no node needs everything.
[daemon]
workers = 8
state_dir = /var/lib/perfcached

[memory]
arena_mb = 1024

[secrets]
client = CHANGE-ME-client
cluster = CHANGE-ME-cluster

[listen]
tcp = 0.0.0.0:6479

[cluster]
multicast = 239.68.68.2:6480
mode = store
collections = profiles, routes
pull_timeout_ms = 300
negative_ms = 500                    # a confirmed fleet-wide miss is remembered this long
tombstone_ms = 2000                  # a delete shields against racing pulls this long

[collection profiles]
buckets_log2 = 16
[collection routes]
buckets_log2 = 14
```

## A proxy fleet

The capacity plane: each key on exactly one node placed by free memory, forwarded writes, served-through reads, a rebalancer.  Fleet capacity is the sum of the arenas.

```ini
# Proxy mode, the capacity plane: each key on exactly one node, placed
# by free memory, forwarded on write, served through on read, and a
# rebalancer that levels the fleet.  Fleet capacity ~ the sum of arenas.
[daemon]
workers = 8
state_dir = /var/lib/perfcached

[memory]
arena_mb = 8192

[secrets]
client = CHANGE-ME-client
cluster = CHANGE-ME-cluster

[listen]
tcp = 0.0.0.0:6479

[cluster]
multicast = 239.68.68.3:6480
mode = proxy
collections = blobs
pull_timeout_ms = 400
max_pending = 8192                   # parked forwards; size it from stats.cluster.pend_peak

[collection blobs]
buckets_log2 = 18
```

## A shard fleet behind Redis clients

Deterministic ownership, and the slot map published on the RESP door so a cluster-aware Redis client sends every key to its owner.

```ini
# Shard mode behind Redis clients: each key on exactly one node chosen by
# rendezvous hashing, and the RESP door publishes the slot map (CLUSTER
# SLOTS / SHARDS / KEYSLOT / NODES) so a cluster-aware Redis client
# sends every key to its owner.  A plain client is forwarded, correctly
# and slowly - see the README's measured tables.
[daemon]
workers = 8
state_dir = /var/lib/perfcached

[memory]
arena_mb = 4096

[secrets]
client = CHANGE-ME-client
cluster = CHANGE-ME-cluster
resp = CHANGE-ME-redis-AUTH
http = CHANGE-ME-http-token

[listen]
tcp = 0.0.0.0:6479
resp = 0.0.0.0:6379
resp_allow = 192.0.2.0/24
resp_collections = 0:ids             # SELECT 0 -> ids; the map is per collection
http = 0.0.0.0:8080
http_allow = 192.0.2.0/24

[cluster]
multicast = 239.68.68.4:6480
mode = shard
collections = ids
max_pending = 8192

[collection ids]
buckets_log2 = 16
```

## A spread fleet

K copies of each record on nodes chosen by placement.  Per-node apply load falls as the fleet grows; reads want a routing client.

```ini
# Spread mode: K copies of each record on nodes chosen by placement,
# where eager is K = P and shard is K = 1.  Per-node apply load falls as
# the fleet grows.  Reads need a routing client - libperfd 0.2.8 or
# newer - or every read that lands off a holder is a pull.
[daemon]
workers = 8
state_dir = /var/lib/perfcached

[memory]
arena_mb = 4096

[secrets]
client = CHANGE-ME-client
cluster = CHANGE-ME-cluster
http = CHANGE-ME-http-token

[listen]
tcp = 0.0.0.0:6479
http = 0.0.0.0:8080
http_allow = 192.0.2.0/24

[cluster]
multicast = 239.68.68.5:6480
mode = spread
replicas = 3                         # K; refused at 1 (that is shard) and above the fleet size
collections = sessions, presence
pull_timeout_ms = 200

[wal]
dir = /var/lib/perfcached
fsync = everysec

[collection sessions]
buckets_log2 = 18
[collection presence]
buckets_log2 = 14
```

## Two fleets on one host

One cluster is one mode; a second daemon joins a second fleet on its own group, ports, state and identity salt.

```ini
# A second daemon on the same host, in a different fleet - one cluster
# is one mode, so a deployment that needs two modes runs two fleets on
# distinct multicast groups.  Distinct ports, state, WAL and identity
# salt; the client secret may be shared, the cluster secrets should not.
# A cluster port takes the one above it too, for pub/sub relays, so the
# two fleets' cluster ports are not adjacent.
[daemon]
workers = 4
state_dir = /var/lib/perfcached-shard

[memory]
arena_mb = 1024

[secrets]
client = CHANGE-ME-client
cluster = CHANGE-ME-cluster-SECOND-FLEET

[listen]
tcp = 0.0.0.0:6489

[cluster]
multicast = 239.68.68.6:6490
mode = shard
collections = ids
site_salt = fleet-b                  # the same machine in two fleets is two members

[wal]
dir = /var/lib/perfcached-shard

[collection ids]
buckets_log2 = 16
```

## Reading a running node back

`perfcli -P stats` answers with the collections, the cluster (node,
role, peers, term), the memory budget and the doors; the status page on
the `http` listener shows the same as the fleet's view; `/metrics` is
the Prometheus form.  `perfcli collections` lists what exists at
runtime; `GET /members` on the `http` listener (or
`perfcli -j '{"method":"members"}'`) says who is in the fleet.  A fleet that did not
form is loud about it: `peers_up` stays 0 and the log names the reason
(a group the network does not carry, a cluster secret that differs, a
config digest that does not match).
