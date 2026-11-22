# perfcached guide

How to do the common things, with the commands, the output you will see
and what to do when it looks different.  Every example output below was
captured from perfcached 0.7.5; long outputs are cut to the lines that
matter, and a cut is marked `...`.  Log lines are shown without their
timestamp, and `redis-cli` output as it prints into a pipe (at a
terminal it adds quotes, `(integer)` and `(nil)`).  The secrets in the
examples are placeholders - pick your own.

perfcached holds small, hot state - sessions, routing, counters, call
state.  It is not a store for large objects or a web page cache: a value
is at most just under 256 KB ([README](README.md), "Known limits").

How each feature works, every setting and every measured number are in
the reference documents listed at the end ([Reference](#reference)).

- [Install and start one node](#install-and-start-one-node)
- [First SET and GET](#first-set-and-get)
- [Keep the data across restarts](#keep-the-data-across-restarts)
- [Create, resize and drop collections while it runs](#create-resize-and-drop-collections-while-it-runs)
- [Form a cluster and check it](#form-a-cluster-and-check-it)
- [Add or remove a node](#add-or-remove-a-node)
- [Read-through: taking over from Redis](#read-through-taking-over-from-redis)
- [Write-through: keep Redis up to date](#write-through-keep-redis-up-to-date)
- [Passwords and TLS](#passwords-and-tls)
- [Reloading the configuration](#reloading-the-configuration)
- [Upgrade](#upgrade)
- [Back up and restore](#back-up-and-restore)
- [Watch it](#watch-it)
- [Troubleshooting](#troubleshooting)
- [Reference](#reference)

## Install and start one node

**Install.**  Debian and Ubuntu: `apt install ./perfcached_0.7.5-1+deb13_amd64.deb`
(or the `perfcached-standalone` package for a node that will never join
a cluster).  Alpine: the signed `.apk`.  A release tarball unpacks to
`bin/`, `sbin/`, `etc/`, `lib/` and `share/` and is installed under
`/opt/perfcached-0.7.5` behind the `/opt/perfcached` link.  Check a
tarball before you install it - the `.sig` file must sit beside it:

    $ perfcached -K perfcached-0.7.5.tar.gz
    artifact: perfcached-0.7.5.tar.gz
      size 4348339 bytes
      sha256 bb7a2bfb6795091e889f0670afc1cfdb4644b9906e1d6224351a047b46f8fd81
      signature: GOOD, the release key 5646e8e4 (built in)

`signature: NONE (... .sig does not exist) - refused` means the `.sig`
is not beside the tarball; `BAD` means do not install it
([operations.md](doc/operations.md#which-builds-a-node-trusts)).

**Configure.**  `/etc/perfcached/perfcached.conf`, owned by root, group
`perfcached`, mode 640:

    [daemon]
    workers = 4                        # the number of cores

    [memory]
    arena_mb = 1024                    # memory for keys and values

    [secrets]
    client = client-secret-example     # what clients present
    cluster = cluster-secret-example   # only daemons hold it; must differ

    [listen]
    tcp = 0.0.0.0:6479                 # the native door

    [collection sessions]
    buckets_log2 = 18

**Check it** before every start:

    $ perfcached -f /etc/perfcached/perfcached.conf -C
    config OK: 1 collection(s), 1 listener(s), 4 worker(s)
    ...

**Start it**: `systemctl enable --now perfcached`, or in the foreground
with `perfcached -f /etc/perfcached/perfcached.conf -D`.  The log ends:

    NOTICE: collection 'sessions' keeps: ephemeral (no [wal]) - a restart starts it empty here; copies: one
    NOTICE: cluster: node state starting -> ready
    ...
    NOTICE: perfcached ready: 4 workers, 1 collections, 1 listeners, arena reserved
    NOTICE: alert (info): started (perfcached 0.7.5), ready

    $ perfcli -p 6479 -a client-secret-example ping
    {"pong":true}

If it does not start, the reason is the first `ERROR` line - see
[Troubleshooting](#troubleshooting).  How many workers, and every
other option: [config.md](doc/config.md#a-single-node); how much memory:
[sizing.md](doc/sizing.md).

## First SET and GET

**With perfcli** (the native door; `PERFCLI_AUTH` can hold the secret
instead of `-a`):

    $ perfcli -a client-secret-example set sessions user:17 'hello world' 300
    {"stored":true}
    $ perfcli -a client-secret-example get sessions user:17
    {"found":true,"value":"hello world","ttl":300}
    $ perfcli -a client-secret-example ttl sessions user:17
    {"ttl":299}
    $ perfcli -a client-secret-example get sessions user:99
    {"found":false}
    $ perfcli -a client-secret-example del sessions user:17
    {"deleted":true}

**With redis-cli** (or any Redis client): add a Redis door, map a Redis
database number onto the collection and give it a password:

    [secrets]
    resp = resp-password-example

    [listen]
    resp = 127.0.0.1:6380
    resp_collections = 0:sessions      # SELECT 0 reaches "sessions"

```
$ redis-cli -p 6380 PING
NOAUTH Authentication required.
$ redis-cli -p 6380 -a resp-password-example --no-auth-warning SET user:42 alice EX 300
OK
$ redis-cli -p 6380 -a resp-password-example --no-auth-warning GET user:42
alice
$ redis-cli -p 6380 -a resp-password-example --no-auth-warning INCR hits
1
$ redis-cli -p 6380 -a resp-password-example --no-auth-warning HSET cart:1 apples 3 pears 2
2
$ redis-cli -p 6380 -a resp-password-example --no-auth-warning LPUSH q x
ERR unknown command 'LPUSH'
$ perfcli -a client-secret-example get sessions user:42
{"found":true,"value":"alice","ttl":299}
```

Both doors reach the same data.  Strings, counters, hashes and JSON
documents are served; lists, sets, sorted sets and streams are not
([REDIS-COMMANDS.md](REDIS-COMMANDS.md) lists every command).  A Redis
door on a LAN address also needs `resp_allow = <cidr>` - without it the
daemon refuses to start - and should be TLS ([Passwords and
TLS](#passwords-and-tls)).  Every perfcli command:
[PERFCLI-COMMANDS.md](PERFCLI-COMMANDS.md).

## Keep the data across restarts

Without a `[wal]` section a restart starts empty.  Add a write-ahead log
and snapshot rules, and a state directory:

    [daemon]
    state_dir = /var/lib/perfcached

    [wal]
    dir = /var/lib/perfcached/wal
    fsync = everysec
    save = 900 1
    save = 300 10000

At start the daemon measures the disk and says what it would recommend:

    NOTICE: wal: probe dev 0:27/794c7630: seq 2058 MB/s (...), synced-append p50 3us p99 6us, 312695 iops (...)
    wal: policy: fsync always recommended, segments 64 MB (NVMe-class sync latency: fsync=always viable); ...
    NOTICE: wal: active in /var/lib/perfcached/wal - 8 x 64 MB segments, generation 1, fsync everysec; ...
    NOTICE: collection 'sessions' keeps: recoverable - a restart replays its WAL (fsync everysec); copies: one

A clean stop (`systemctl stop`, SIGTERM) takes a snapshot, and the next
start loads it:

    NOTICE: perfcached: stopping - taking the shutdown snapshot
    NOTICE: rdb: snapshot 3 records, 173 bytes, 2 ms, wal marker 4
    NOTICE: perfcached: clean shutdown
    ...
    NOTICE: recover: rdb loaded (3 records, 0 expired, 0 orphaned), wal replay 0 applied / 4 below marker / ...

An acknowledged write is in memory and queued for the log, not yet on
disk: a crash loses what the log had not written (about the last 200 ms
at the defaults), a power cut up to about a second more under
`fsync = everysec`.  When a client needs everything so far on disk, it
sends `sync`:

    $ perfcli -a client-secret-example sync
    {"synced":true,"seq":4,"dropped":0}

`perfcli save` starts a snapshot now (`{"started":true}`).  A collection
that is only a cache can skip the log with `durable = no`.  In a cluster
only the node that took a write logs it, so the snapshots, not the
copies, bring a whole fleet back.  Detail:
[durability.md](doc/durability.md), [concepts.md](doc/concepts.md#a-records-life).

## Create, resize and drop collections while it runs

Allow it on the node and set a privilege secret (both take a restart):

    [daemon]
    allow_create = yes

    [secrets]
    enable = enable-secret-example     # not the client secret

```
$ perfcli -a client-secret-example -E enable-secret-example create sessions_restored 18
{"created":true,"col":"sessions_restored","buckets":262144}
$ perfcli -a client-secret-example -E enable-secret-example drop sessions
{"dropped":false,"entries":10002,"why":"the collection is not empty - pass force to drop it with its records"}
$ perfcli -a client-secret-example -E enable-secret-example drop sessions force
{"dropped":true,"col":"sessions","records":10002}
$ perfcli -a client-secret-example -E enable-secret-example rename sessions_restored sessions
{"renamed":true,"col":"sessions_restored","to":"sessions"}
$ perfcli -a client-secret-example collections
{"collections":[{"name":"sessions","mode":"eager","entries":10002,"buckets":65536}]}
```

perfcli prints `privileged: create/drop/resize/rename, map/unmap and
restore are available on this connection` on stderr once `-E` is
accepted.  `resize <col> <buckets_log2>` resizes in place.  In a fleet a
change made on one node reaches every member.  Without `allow_create`:

    creating and dropping collections is not enabled here - set [daemon] allow_create = yes on this node

A table also grows and shrinks by itself (the log says
`... shrinking to 2^12`); that is normal.  Detail:
[operations.md](doc/operations.md#collections-at-runtime),
[sizing.md](doc/sizing.md).

## Form a cluster and check it

Add the same `[cluster]` section to every node and start them in any
order.  On one L2 segment the nodes find each other by multicast:

    [daemon]
    state_dir = /var/lib/perfcached

    [listen]
    tcp = 192.0.2.11:6479
    http = 192.0.2.11:8080
    http_allow = 192.0.2.0/24

    [cluster]
    multicast = 239.68.68.1:6480
    mode = eager                       # every node holds every key
    collections = sessions             # the whole set, the same on every node

`advertise = <address>` pins the address on a host with several.  Open
UDP and TCP 6480 and 6481 between the nodes (and TCP 6482 with
`discovery = unicast`), never through a load balancer
([cluster.md](doc/cluster.md#the-collection-config-and-the-ports)).
Where there is no multicast - clouds, Kubernetes - use seeds
instead ([cluster.md](doc/cluster.md#unicast-discovery-clouds-and-kubernetes)):

    [cluster]
    discovery = unicast
    port = 6480
    seeds = cache-a.internal, cache-b.internal, cache-c.internal
    expect = 3

**What you see.**  One node founds the cluster, the others are admitted
one at a time, prove every link and go ready:

    NOTICE: cluster: founded the cluster - master as node 794 (0 live peer(s))
    NOTICE: cluster: node 977 (192.0.2.13) is ONLINE - all 1 pair(s) with it held both ways, 1503 ms

and on a joiner:

    NOTICE: cluster: admitted as node 720 (master is node 794, 3 member(s), part 1 of 1) - CONNECTING: proving every link before it serves
    NOTICE: cluster: ONLINE - every link to every member answered both ways (1502 ms CONNECTING)
    NOTICE: cluster: node state recovering -> ready

**Check it** from any node:

    $ perfcli -h 192.0.2.11 -a client-secret-example fleet-status
    node  member                   version   state      install  staged (newest first)
    720   192.0.2.11:6479          0.7.5     this       local    -
    794   192.0.2.12:6479          0.7.5     standby    local    -
    977   192.0.2.13:6479          0.7.5     standby    local    -

    $ perfcli -h 192.0.2.11 -a client-secret-example -P stats |
          grep -E '^    "(node|role|master|peers_up|cluster_id|cluster_id_state)":|^  "state":'
      "state": "ready",
        "node": 720,
        "cluster_id": "01a202dd-8072-798f-8b37-0bccd745ec8e",
        "cluster_id_state": "established",
        "role": "member",
        "master": 794,
        "peers_up": 2,

Every node should say `ready`, the same `master`, and `peers_up` one
less than the fleet.  A write on one node is read on another:

    $ perfcli -h 192.0.2.11 -a client-secret-example set sessions user:1 alice 600
    {"stored":true}
    $ perfcli -h 192.0.2.13 -a client-secret-example get sessions user:1
    {"found":true,"value":"alice","ttl":600}
    $ perfcli -h 192.0.2.12 -a client-secret-example del sessions user:1
    {"deleted":true}
    $ perfcli -h 192.0.2.11 -a client-secret-example get sessions user:1
    {"found":false}

`/members` on the HTTP door lists every member with its state and
version.  A node that stays `connecting` has a port blocked between it
and a member - the master's log names the pair ([Troubleshooting](#troubleshooting)).

**Which mode.**  `eager` for three nodes that must not lose
availability; `spread` (`replicas = 2` or `3`) for four or more;
`shard` for throughput with a client that routes by key; `proxy` for
the largest keyspace per byte.  One cluster runs one mode, and a
replica is availability, never durability
([modes.md](doc/modes.md#which-mode-to-run)).  Complete fleet configs
for every mode: [contrib/CONFIGURATIONS.md](contrib/CONFIGURATIONS.md).

## Add or remove a node

**Add**: start a new node with the same config (its own addresses).  It
is admitted, pulls a copy and goes ready:

    NOTICE: cluster: admitted as node 400 (master is node 794, 4 member(s), part 1 of 1) - CONNECTING: ...
    NOTICE: cluster: joined as node 400 (master is node 794, 4 member(s)) holding nothing - pulling a bootstrap before reporting ready
    NOTICE: cluster: bootstrapped from node 720 - 10002 record(s) streamed, 10002 stored, ... in 71 ms; ready
    NOTICE: cluster: node state recovering -> ready

Check that `collections` shows the same `entries` on it as on the
others.

**Remove**: stop it (`systemctl stop perfcached`).  It snapshots and
says goodbye:

    NOTICE: cluster: node state ready -> draining - handing its data over
    NOTICE: perfcached: stopping - taking the shutdown snapshot
    NOTICE: perfcached: clean shutdown

and the master logs:

    NOTICE: cluster: node 977 (192.0.2.13) said goodbye - its id is reserved for it for 604800 s

After a minute every node raises a warning, `fleet.node977`: `member
node 977 (192.0.2.13) left - it said goodbye`.  That is expected for a
node you removed.  Before you remove one, check the others can hold the
data (in `spread` and `shard` its share moves to them) and, in `eager`,
that the fleet still has the copies you want.

**Restart one** (the same node, for maintenance): first check it is
safe - its peers can witness the restart:

    $ perfcli -h 192.0.2.13 -a client-secret-example -P stats | grep -A4 '"restart"'
        "restart": {
          "safe": true,
          "witnesses": 2,
          "safe_in_ms": 0,
          "waits_for": []

`"safe": false` with `safe_in_ms` means wait that long; `waits_for`
names peers still settling.  An eager node that comes back copies the
state of a peer that stayed up:

    NOTICE: cluster: rejoin - node 794 has been up since before this node went down (98 s before), so it holds the fleet's state: dropping the replayed records and copying that state whole (10002 record(s)); ...
    NOTICE: cluster: rejoin - node 794 is still the survivor that served the copy; ready

Restart one node at a time, the master last; `master_prefer <node>` on
the master hands mastership over first
([modes.md](doc/modes.md#rolling-restarts-stopping-mastership)).
In 0.7.5 a node rejoining while a reconcile pass is open can report
ready before its copy has finished, and for that moment answer a key
deleted while it was away, or an older value (fixed in 0.7.5.1).

## Read-through: taking over from Redis

Move the clients to perfcached and let it read, on a miss, the keys it
does not hold yet from the Redis server they used before - no big-bang
copy.  perfcached only reads Redis, never writes it.

**On Redis**, a user that can only read, on a replica (`+info` is what
tracking needs, `+role` what `upstream_role` needs):

```
ACL SETUSER reader on >password ~* +@read +hello +auth +select +ping +info +role +client|id +client|info +client|tracking
```

**On perfcached** (every node the same):

    [collection tokens]
    buckets_log2 = 16
    upstream = redis://reader@127.0.0.1:6378/0
    upstream_password = reader-password-example
    upstream_prefixes = tok:*, cid:*       # required; * for every key

```
NOTICE: collection 'tokens': read-through from 127.0.0.1:6378 db 0, 2 pattern(s), 2 connection(s), negative 2000 ms, timeout 500 ms, on_error error - this node READS the upstream and never writes it
NOTICE: collection 'tokens': tracking on - 127.0.0.1:6378's invalidations reach connection 9 on server cf347cdbac5b
NOTICE: collection 'tokens.nofall' keeps: recoverable - ...
```

**What clients see** (Redis holds `tok:a`, `tok:b`, `tok:count` = 41
and a hash `tok:h`; the `redis-cli -p 6379` lines are Redis itself):

```
$ redis-cli -p 6380 -a resp-password-example --no-auth-warning GET tok:a
alpha
$ redis-cli -p 6380 ... TTL tok:a                 # Redis's TTL comes along
3577
$ redis-cli -p 6380 ... GET other:key             # outside upstream_prefixes: never read through

$ redis-cli -p 6380 ... INCR tok:count            # a change takes the key over
42
$ redis-cli -p 6379 ... GET tok:count             # Redis keeps its own value
41
$ redis-cli -p 6380 ... DEL tok:b
1
$ redis-cli -p 6380 ... GET tok:b                 # deleted here: never read through again

$ redis-cli -p 6379 ... SET tok:a ALPHA2 EX 3600  # a change in Redis reaches the copy
OK
$ redis-cli -p 6380 ... GET tok:a
ALPHA2
```

(`...` stands for `-a resp-password-example --no-auth-warning` and
Redis's own credentials.)  `perfcli -P stats tokens` has an `upstream`
block: `fetches`, `absent`, `errors`, `promoted`, `tracking: true`, and
per upstream `state: "serving"`.

**Finish the move.**  Once the writers use perfcached, take over every
key nobody has read yet, on ONE node:

    $ perfcli -a client-secret-example -E enable-secret-example updrain tokens
    {"col":"tokens","drain":"started"}

    NOTICE: collection 'tokens': the drain sweep done - 2005 read, 2003 taken over, 2 the fleet already had, 0 gone, 0 of a type it cannot hold, 0 too big, 0 errors, 0 other nodes' to take

When `fetches` stops rising, remove `upstream` and `upstream_prefixes`
on every node and restart them.

**If Redis is unreachable**, a key already held is still served and a
miss answers an error (`upstream_on_error = miss` makes it a miss):

    $ redis-cli -p 6380 ... GET tok:never-seen
    ERR upstream: the read-through fetch failed (upstream unreachable or timed out)

Several upstreams, Sentinel, Redis Cluster, `rediss://`, read-only
collections, `proxy` (refused) and what each command does with a key it
does not hold: [upstream.md](doc/upstream.md).

## Write-through: keep Redis up to date

`upstream_writes = through` sends every write on the collection to the
Redis master first, reads the key back and keeps what Redis holds - for
when other applications still read Redis.  List the master beside the
replicas; give the writer its own user:

    [collection tokens]
    upstream = redis://reader@127.0.0.1:6378/0, redis://reader@127.0.0.1:6379/0
    upstream_role = replica
    upstream_password = reader-password-example
    upstream_prefixes = tok:*, cid:*
    upstream_writes = through
    upstream_write_user = writer
    upstream_write_password = writer-password-example

```
ACL SETUSER writer on >wpass ~tok:* ~cid:* -@all +auth +select +info +role +command|info +command|getkeys +type +pttl +get +hgetall +json.get +set +del +unlink +expire +persist +incrby +decrby +hset +hsetnx +hmset +hdel +hincrby +hincrbyfloat +mset +json.set +json.del
```

(The `json` commands need RedisJSON on Redis; without it the ACL line
must leave them out, and perfcached refuses JSON writes.)

    NOTICE: collection 'tokens': read-through from 127.0.0.1:6378, 127.0.0.1:6379 db 0, replicas only, ... - WRITE-THROUGH to its master, ...
    NOTICE: collection 'tokens': write-through to the master 127.0.0.1:6379

```
$ redis-cli -p 6380 ... SET tok:new hello EX 600
OK
$ redis-cli -p 6379 ... GET tok:new               # Redis has it
hello
$ redis-cli -p 6380 ... INCRBY tok:count 8
49
$ redis-cli -p 6379 ... GET tok:count
49
$ redis-cli -p 6380 ... SET tok:new2 y NX         # Redis's own answer: not set

$ redis-cli -p 6379 ... SET tok:new changed-in-redis
OK
$ redis-cli -p 6380 ... GET tok:new               # other writers are followed
changed-in-redis
$ redis-cli -p 6380 ... LPUSH tok:list a          # unknown here: passed to Redis
NOPERM User writer has no permissions to run the 'lpush' command
```

**Switching a collection that already took writes**: the keys the fleet
wrote before the switch are its own (`write_through.fleet_owned` in
`stats`).  Compare them with Redis, then copy them over, on one node:

    $ perfcli -a client-secret-example -E enable-secret-example upfill tokens check
    {"col":"tokens","upfill":"checking"}
    NOTICE: collection 'tokens': upfill check done - 2003 found: 0 Redis lacks (a run writes them), 2002 Redis holds the same, ...
    $ perfcli -a client-secret-example -E enable-secret-example upfill tokens
    {"col":"tokens","upfill":"started"}

Limits: one node sends a collection's writes to the master one at a
time, about 1,500-2,500 a second; two clients writing one key through
two nodes at once can leave Redis and the fleet with different
winners; pub/sub is not bridged.  **In 0.7.5 keep write-through values
under 256 KB**: a larger one reaches Redis and the client gets OK, but
the fleet cannot hold it (fixed in 0.7.5.1).  Detail:
[upstream.md](doc/upstream.md#write-through).

## Passwords and TLS

**The secrets** (`[secrets]`): `client` for clients on the native door,
`cluster` for the daemons only, `enable` for privileged commands,
`resp` for the Redis door's `AUTH`, `http` for the HTTP door's token,
`mail` for SMTP.  `client` and `cluster` must differ.  The config file
holds them: mode 640, or the daemon warns `... is world-readable and
holds secrets - chmod 640 it`.

**Rotate the client secret** without dropping clients: add the new one
as a second line and reload, move the clients, remove the old line and
reload again:

    [secrets]
    client = client-secret-example
    client = client-secret-NEW-example

    $ perfcached -f /etc/perfcached/perfcached.conf -H $(pidof perfcached)   # = systemctl reload perfcached
    reloaded: the client secrets, the TLS certificate

The cluster secret rotates with three rolling restarts through
`cluster_accept` ([config.md](doc/config.md#other-settings-worth-knowing)).

**TLS on the Redis and HTTP doors** (a build with OpenSSL: `perfcached
-V` prints `tls: OpenSSL ...`).  A private CA, if you have none:

    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 3650 \
        -subj '/CN=Example cache CA' -addext 'basicConstraints=critical,CA:TRUE' \
        -addext 'keyUsage=critical,keyCertSign,cRLSign' -keyout ca.key -out ca.pem
    openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -subj '/CN=cache1' \
        -keyout node.key -out node.csr
    printf 'subjectAltName=IP:127.0.0.1,IP:192.0.2.11,DNS:cache1\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n' > node.ext
    openssl x509 -req -in node.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 825 \
        -extfile node.ext -out node.pem

The certificate must name the address clients dial (an IP SAN for an
IP).  Then:

    [listen]
    resp = 127.0.0.1:6380              # plaintext on loopback may stay
    resp = 127.0.0.1:6381 tls          # a LAN address also needs resp_allow
    http = 127.0.0.1:8443 tls
    tls_cert = /etc/perfcached/node.pem
    tls_key = /etc/perfcached/node.key

```
NOTICE: RESP listener on 127.0.0.1:6381 (tls) - 0 allow-list entries, password SET, collections 0:sessions
NOTICE: TLS: OpenSSL 3.5.7 9 Jun 2026, certificate /etc/perfcached/node.pem, TLS 1.2 or later

$ redis-cli --tls --cacert ca.pem -h 127.0.0.1 -p 6381 -a resp-password-example --no-auth-warning GET user:17
{"name":"user17","plan":"basic"}
$ redis-cli --tls -h 127.0.0.1 -p 6381 PING             # without the CA
Could not connect to Redis at 127.0.0.1:6381: SSL_connect failed: certificate verify failed
$ redis-cli -h 127.0.0.1 -p 6381 PING                   # without --tls
Error: Server closed the connection
$ curl -s --cacert ca.pem -H 'Authorization: Bearer http-token-example' https://127.0.0.1:8443/health
ok
```

A renewal is a reload: the next connection gets the new certificate.
`stats` has a `tls` block with `days_left`; the `tls.cert` alert warns
14 days before expiry.  `auth_needs_tls = yes` refuses passwords on
the plaintext doors off loopback.

**The native door** authenticates every connection (the secret is never
sent) but by default carries requests in the clear after it; the log
says `native door 0.0.0.0:6479: authenticated, not encrypted`.  Off a
network you trust, set `[listen] encrypt = yes` (`perfcli --encrypt`
asks for it per connection).  The cluster plane is always encrypted.
Client snippets for redis-py, phpredis, predis and Prometheus over TLS:
[config.md](doc/config.md#other-settings-worth-knowing), "TLS on the RESP
and HTTP doors".

## Reloading the configuration

`systemctl reload perfcached` (or `perfcached -f <conf> -H <pid>`, or
`perfcli -E <enable> reload`) applies, without a restart: the allow-lists
(`resp_allow`, `http_allow`), the `client`, `http`, `resp` and `enable`
secrets, `log_level`, `slowlog_usec`, `auth_needs_tls`, and the TLS
certificate files.

    $ perfcached -f /etc/perfcached/perfcached.conf -H $(pidof perfcached)
    reloaded: log_level

Anything else refuses the whole reload - nothing is applied:

    reload refused: [daemon] allow_create changed and cannot be reloaded - restart to apply it; nothing changed

Restart that node instead (in a fleet, one node at a time).  Detail:
[operations.md](doc/operations.md#reloading-the-configuration-071).

## Upgrade

**Within the support window** (a patch of the same minor, or the next
minor): a rolling restart - members first, the master last, one at a
time, each only when its `restart.safe` says `true` ([Add or remove a
node](#add-or-remove-a-node)).  Packages do not restart the daemon:
install, then `systemctl restart perfcached` node by node.  A tarball
install can let the roll driver do it:

    $ perfcli -h 192.0.2.11 -E enable-secret-example upgrade plan perfcached-0.7.5.tar.gz version=0.7.5
    node 720 192.0.2.11:6479 speaks wire epochs 4..4 (fleet 4)
    ...
    the candidate 0.7.5 (89e55817) speaks wire epochs 4..4 - it joins on 4
    planned: 0.7.5 (bb7a2bfb...) -> every member of 192.0.2.11:6479; journal ~/.perfcached/upgrade-192.0.2.11_6479.journal
    next: perfcli -h 192.0.2.11 -p 6479 -E <enable> upgrade start journal=...

`upgrade status` shows every node's version, state and `safe`.
`upgrade start` stages the build on every member and rolls one node at a
time, stopping and reverting the node at the first gate that fails;
`abort`, `resume` and `rollback` do what they say.  It needs the upgrade
helper on each node ([operations.md](doc/operations.md#upgrades)).

**From 0.7.4.x to 0.7.5: stop the whole fleet.**  0.7.5 speaks cluster
wire epoch 4 alone and refuses a 0.7.4 node, so it is no roll.  Stop
every node at once, install 0.7.5 everywhere, start them all - the WAL
and snapshots are kept:

    $ perfcli -h 192.0.2.11 -a client-secret-example -E enable-secret-example fleetstop
    {"stopping":true,"asked":3,"stopped":[794,977,400],"still_up":[],"lost":[]}

`still_up` or `lost` not empty: those nodes did not say goodbye - stop
them by hand before installing.  After the start each node loads its
own snapshot; because the whole fleet came back together, nothing is
deleted:

    NOTICE: recover: rdb loaded (10002 records, 0 expired, 0 orphaned), ...
    NOTICE: cluster: reconcile SKIPPED - none of the 3 live peer(s) was up and ready through this node's absence, so the fleet came back together ... Nothing is deleted. ...

Check `collections` shows the same entries on every node.  Move clients
to libperfd 0.7.6 or later with it: an older libperfd does not route by
key against 0.7.5 (every request takes a forward hop) and is refused a
value over 58,000 bytes.

A fleet on `[cluster] tombstone_acks = no` stays on `no` until 0.7.5.1:
a node switched to `yes` among members on `no` loops.

Older releases, packages, the helper and going back:
[PRODUCTION.md](PRODUCTION.md), "Upgrading";
[operations.md](doc/operations.md#upgrades);
[compatibility.md](doc/compatibility.md).

## Back up and restore

**Back up** one node (on an eager fleet any ready node holds everything):

    $ perfdump --from 127.0.0.1:6479 -a client-secret-example --out /var/backups/pc-2026-11-22
    perfdump: 10002 records, 0.4 MB of keys and values, 4 file(s), 1 collection(s), 0.01 s, 909273 records/s
    $ perfdump --inspect /var/backups/pc-2026-11-22
    source: 127.0.0.1:6479
      sessions.0000.pcd.zst            ok: 2537 records, crc 2bf66e39
      ...
    4 file(s), 10002 records verified, 0 bad

`--rate` caps the records a second per connection so a dump does not
starve production; compressed dumps need the `zstd` binary.

**Restore** with the versions and expiries the dump held.  Check first,
then load beside the live collection and swap it in:

    $ perfload /var/backups/pc-2026-11-22 --to 127.0.0.1:6479 -a client-secret-example --dry-run
    perfload: dry run: 10002 records, 0.4 MB in 4 chunk(s) would load into 127.0.0.1:6479 as sessions; 0 member(s), policy newer
    $ perfcli -a client-secret-example -E enable-secret-example create sessions_restored 18
    $ perfload /var/backups/pc-2026-11-22 --to 127.0.0.1:6479 -a client-secret-example \
          --enable enable-secret-example --collection-map sessions=sessions_restored
    perfload: 10002 records, 0.4 MB, 4 chunk(s), 1 collection(s), 0.01 s, 833500 records/s; stored 10002, older 0, existing 0, expired 0, refused 0, bad 0, rerouted 0

then `drop sessions force` and `rename sessions_restored sessions`
([Create, resize and drop
collections](#create-resize-and-drop-collections-while-it-runs)).  On a
fleet perfload reports when the members have settled:

    perfload: peers settled after 0.2 s; entries 192.0.2.11:6479=10002 192.0.2.12:6479=10002 192.0.2.13:6479=10002

A second run skips the chunks already loaded (`perfload.done`;
`--restart` loads them again).  With the default `--policy newer` a
record no newer than the fleet's copy is counted, not stored:

    perfload: 10002 records, ... stored 0, older 10002, existing 0, expired 0, ...

`--policy overwrite` rolls the fleet back to the dump.  A dump loads
into a fleet of any size or mode.  Detail: [tools.md](doc/tools.md#perfdump),
[tools.md](doc/tools.md#perfload).

## Watch it

**stats.**  `perfcli -P stats` (or `GET /stats` on the HTTP door) is
every figure as JSON; grep it for what you need:

    $ perfcli -a client-secret-example -P stats | grep -A6 '"tombstones_retained"'
        "tombstones_retained": {
          "entries": 0,
          "bytes": 0,
          "max": 250000,
          "retain_s": 900,
          ...

`perfcli collections` gives each collection's entries; `perfcli
fleet-status` each member's version and state.

**The HTTP door** (`http = <addr:port>`, with `http_allow` and the
`[secrets] http` token) serves `/stats`, `/members`, `/clients`,
`/metrics` and `/health`:

    $ curl -s http://127.0.0.1:8080/health
    a token is required: Authorization: Bearer <token>, or the browser's login - any user, the token as the password
    $ curl -s -H 'Authorization: Bearer http-token-example' http://127.0.0.1:8080/health
    ok

**Prometheus** scrapes `/metrics` with the token.  Series worth an alert:

    perfcached_node_state{state="ready"} 1
    perfcached_alerts_active{level="critical"} 0
    perfcached_cluster_nodes 4
    perfcached_cluster_restart_safe 1
    perfcached_wal_dropped_total 0
    perfcached_wal_storage_failed 0
    perfcached_rdb_last_success_age_seconds 44
    perfcached_cluster_tombstones_dropped_early_total 0
    perfcached_arena_held_bytes 6291456
    perfcached_arena_max_bytes 268435456

A Grafana dashboard is in [contrib/grafana](contrib/grafana); the WAL
capacity rule is in [durability.md](doc/durability.md#storage-failures-and-snapshots).

**Alerts.**  Each node checks itself once a second and keeps what it
raised in `stats` `alerts`:

    "active": [
      {
        "key": "fleet.node977",
        "level": "warning",
        "since_ms": 1795344556898,
        "text": "member node 977 (192.0.2.13) left - it said goodbye"
      }
    ]

and logs it: `WARNING: alert: member node 977 (192.0.2.13) left - it
said goodbye`.  **critical** means act now, **warning** today.  With
`[mail]` on one node of a fleet, every node's alerts are mailed:

    [secrets]
    mail = smtp-password-example

    [mail]
    smtp = smtp://mail.example.net:587      # STARTTLS
    user = alerts@example.net
    from = perfcached@example.net
    to = ops@example.net

    $ perfcli -a client-secret-example -E enable-secret-example mailtest
    {"sent":false,"reply":"Failed to connect to 127.0.0.1 port 2525 after 0 ms: Could not connect to server"}

`"sent":true` with the server's `250` reply is the good answer.  Without
a mailer the log says `alerts: no mailer reachable - ... is kept in this
node's ring only`.  Every alert, when it is raised and cleared:
[alerts.md](doc/alerts.md#what-raises-an-alert).

### perfcached-dashboard

The status page is its own process, a client of the native door; one
seed is enough:

    $ perfcached-dashboard -s 192.0.2.11:6479 -a client-secret-example
    perfcached-dashboard: serving http://127.0.0.1:8090/ - 1 seed, libperfd 0.7.6, libmicrohttpd 1.0.1
    perfcached-dashboard: member node 720 at 192.0.2.11:6479 connected
    perfcached-dashboard: member node 794 at 192.0.2.12:6479 connected
    ...
    $ curl -s http://127.0.0.1:8090/health
    {"ok":true,"members":4,"connected":4}

Open `http://127.0.0.1:8090/`.  The package ships
`perfcached-dashboard.service` disabled; it binds loopback, so put a
reverse proxy with TLS in front for a browser elsewhere.  The daemon's
own HTTP door serves no page (`GET /` answers 410).  Detail:
[clients.md](doc/clients.md#perfcached-dashboard),
[operations.md](doc/operations.md#watching-it).

## Troubleshooting

**The daemon does not start.**  `perfcached -f <conf> -C` prints the
reason:

| it says | what to do |
|---|---|
| `the cluster secret EQUALS client secret #1` | give `[secrets] cluster` its own value |
| `collection 'c': buckets_log2 missing` / `'30' is not an integer in [4..24]` | fix that line of the collection |
| `a non-loopback RESP listener needs resp_allow = <cidr>` | add `resp_allow`, or bind the Redis door to loopback |
| `127.0.0.1:6479 is already in use by another process` | another perfcached (or anything) holds the port: stop it, or use another address |
| `... is world-readable and holds secrets - chmod 640 it` (a warning) | `chmod 640` the config |
| `reload refused: ... changed and cannot be reloaded - restart to apply it` | that key needs a restart ([Reloading](#reloading-the-configuration)) |

**A client cannot get in.**

| it says | what it means |
|---|---|
| `perfcli: handshake failed with every secret` | wrong `-a` secret, or the listener is `plaintext = loopback` and you are on loopback: drop `-a` |
| `perfcli: enable failed: wrong enable secret` | `-E` takes `[secrets] enable`, not the client secret; 5 wrong ones from an address in 60 s lock it out for the window |
| `NOAUTH Authentication required.` / `WRONGPASS` | the Redis door wants `[secrets] resp` (`redis-cli -a`) |
| `ERR unknown command 'LPUSH'` | not served by perfcached ([REDIS-COMMANDS.md](REDIS-COMMANDS.md)) |
| `-ERR max number of clients reached` | `max_clients` or the memory floor ([config.md](doc/config.md#other-settings-worth-knowing)) |
| `node is not READY (recovering or connecting) - retry, or use another member` | the node is still joining or copying; libperfd moves to another member by itself |
| `value too large` | a value is at most just under 256 KB; perfcached is not for large objects |
| `creating and dropping collections is not enabled here` | `[daemon] allow_create = yes` and a restart |

**Alerts and states.**

| you see | what it means | what to do |
|---|---|---|
| node state `failed`, alert critical | the WAL could not store acknowledged writes (the device refused) | fix the storage, restart the node ([durability.md](doc/durability.md#storage-failures-and-snapshots)) |
| node state `healing` | the log missed a record; reads served, writes refused until a snapshot covers it - usually under a second | if frequent: faster storage, more segments, or less load (`perfcached_wal_heals_total`) |
| `wal.capacity` | the log fills faster than snapshots free it | more WAL segments, higher `rdb_mb_s`, or less load |
| `stalled` | the node's cluster thread made no progress for 10 s; peers steer around it | it returns by itself; if not, restart it and look at the host (CPU, a paused VM) |
| node stays `connecting`, alert `cluster.admission_stuck` on the master | a link between it and a member does not answer both ways; the text names the pair and the port (`the cluster socket`, `the relay port`, `the frame lane`, `clrpt`, `the bulk listener`) | open UDP+TCP 6480 and 6481 (and TCP 6482 for unicast) between the two |
| `fleet.node<id>` `left - it said goodbye` | a member was stopped | expected if you stopped it; start it again otherwise |
| `fleet.node<id>` `went silent and was purged` | a member died or was cut off | look at that host; it rejoins by itself |
| `fleet.majority` (critical) | fewer than half the members are ready | bring members back |
| `cluster.tombstones` | the cap on remembered deletes dropped some early; a deleted key could come back from a node that was away | raise `[cluster] tombstone_retain_max` above deletes-plus-expiries a second times `tombstone_retain_s` (`stats cluster.tombstones_retained`: `entries` / `oldest_age_s` is that rate) - [cluster.md](doc/cluster.md#deletes-across-a-fleet) |
| `cluster.acks_stuck` | one member stopped confirming deletes; they are kept for it | look at that member |
| `cluster.peer_absent` | a member came back after longer than deletes are remembered | in an eager fleet it rebuilds by itself; otherwise re-issue deletes that matter |
| `config.<setting>` | a member holds another value of a setting the fleet must agree on (`/members` `config_differs`) | make the configs agree |
| `memory.arena` / writes refused, arena full | the arena is near or at `arena_mb`; it evicts nothing | raise `arena_mb` or `arena_cap_mb` ([sizing.md](doc/sizing.md)) |
| `clients.refused` | the memory floor refuses new connections | free memory on the host, or lower `memory_floor` |
| `upstream.<col>`, `ERR upstream: the read-through fetch failed` | Redis unreachable | fix Redis; held keys keep being served |
| `write.<col>` (critical) | write-through has no master: every write refused | list the master, check the writer's ACL |
| `write.<col>.queue` | write-through writes refused at a full queue or too slow | a slow master; fewer writes or a faster master |
| `tls.cert` | the TLS certificate expires within 14 days, or has | renew it and reload |
| `node.start` warning "after an unclean stop" | the node crashed or was killed | it recovered from its log; find out why it stopped |

The full table, with when each alert clears:
[alerts.md](doc/alerts.md#what-raises-an-alert).  What each node state
means: [cluster.md](doc/cluster.md#what-a-node-says-about-itself).

**Known issues in 0.7.5**, fixed in 0.7.5.1: write-through values over
256 KB (above); a rejoining node may answer for a moment before its
copy has finished ([Add or remove a node](#add-or-remove-a-node)); and
a collection change from the fleet writes the collections file on the
cluster thread, so on slow storage forwards and pulls stall for the
fsync and a routed read can answer as a miss.  [README](README.md) has
the list.

## Reference

| document | what is in it |
|---|---|
| [doc/concepts.md](doc/concepts.md) | what perfcached promises: a record's life, the modes side by side, what a collection keeps, failures |
| [doc/config.md](doc/config.md) | a complete single node; every other setting: clients, memory floor, doors, encryption, the Redis door, TLS, IPv6, memory modes, logging |
| [doc/cluster.md](doc/cluster.md) | discovery (multicast, unicast, Kubernetes), identity, the cluster id, ports, deletes and tombstones, failure detection, hot keys, node states |
| [doc/modes.md](doc/modes.md) | store, eager, spread, shard, proxy; rejoining, reconcile, wire epochs, ordered joining, rolling restarts, fleetstop, which mode to run, MGET |
| [doc/durability.md](doc/durability.md) | the WAL, fsync, `on_full`, healing, snapshots, cache collections, storage failures, going back to an older build |
| [doc/upstream.md](doc/upstream.md) | read-through and write-through in full |
| [doc/sizing.md](doc/sizing.md) | the index, records, residue, the ceiling, what the host sees |
| [doc/operations.md](doc/operations.md) | the HTTP door, runtime collections, reloads, counters and resets, signatures, upgrades and the helper |
| [doc/alerts.md](doc/alerts.md) | mail, settings the fleet must agree on, every alert |
| [doc/tools.md](doc/tools.md) | perfcli, perfdump, perfload |
| [doc/clients.md](doc/clients.md) | perfcached-dashboard, libperfd, pub/sub |
| [doc/benchmarks.md](doc/benchmarks.md) | every measured number |
| [doc/compatibility.md](doc/compatibility.md) | what may change between releases |
| [doc/building.md](doc/building.md) | building from source: editions, allocator, debug and sanitizer builds, mail, TLS, tests |
| [PRODUCTION.md](PRODUCTION.md) | the checklist before a real deployment |
| [contrib/CONFIGURATIONS.md](contrib/CONFIGURATIONS.md) | eleven complete configurations |
| [contrib/perfcached.conf.example](contrib/perfcached.conf.example) | every option, annotated |
| [REDIS-COMMANDS.md](REDIS-COMMANDS.md), [PERFCLI-COMMANDS.md](PERFCLI-COMMANDS.md) | every command of each door |
| [doc/perfdump-format.md](doc/perfdump-format.md) | the dump format |
