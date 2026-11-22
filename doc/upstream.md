# Read-through and write-through: a Redis upstream

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

## Read-through: taking over from Redis

A collection can name an UPSTREAM - a Redis server, Valkey, or another
perfcached - that the node reads on a local miss and never writes.  It
is how a fleet takes a keyspace over from Redis without a big-bang copy:
the clients move to perfcached, perfcached answers from its own
collection, and a key it does not hold yet is read from the upstream the
first time anyone asks for it.

```
[collection 0]
upstream = redis://reader@10.0.0.12:6379/0
upstream_password = <the password perfcached sends to that upstream>
upstream_prefixes = tok:*, cid:*
```

`upstream_prefixes` is required: only keys matching one of its patterns
(Redis glob syntax, comma-separated, up to 32) ever fall through - say
`*` to mean all of them.  `upstream = redis://[user@]host:port[/db]`
(`rediss://` over TLS, below);
the password is the collection's `upstream_password` (0.6.1; it was
`[secrets] upstream`, which is now refused with that advice), never in
the URL - so one perfcached can front several Redis servers, a
collection each, each with its own password, and its clients reach
them all through it (`SELECT` picks the collection at the RESP door).
The password appears in no log line, `/stats` field, page or `-E`
dump.

## Several upstreams, Sentinel, Redis Cluster, TLS

**Several upstreams (0.6.1).**  `upstream` takes up to 32 (eight before 0.7.5), and with
`upstream_role = replica` each must say so by `ROLE` (a master is not
read - list it anyway, so the collection follows a failover: it shows
greyed on the page and raises no alert; `upstream_master_fallback = yes`
lets it serve while no replica does, in amber on the page and with a
warning).  `upstream_mode =
balance` (the default) sends each fetch to the serving upstream with the
fewest in flight; `upstream_mode = failover` sends every fetch to ONE
active upstream - `upstream_primary = host:port`, the first listed by
default - and keeps the others connected and tracked, so a switch is at
once.  When the active one stops serving, the next in order takes over
(the primary, then the list; the master fallback last), with a warning
alert.  `upstream_preempt = yes` (the default) gives the primary back
its place once it has served again for `upstream_preempt_hold_s` (30,
15-300), so a flapping primary does not drag the fetches back and forth;
`no` keeps the active one until it fails.  Each upstream's first
connection is kept and checked every 10 s (ROLE, or PING), so a dead
one is seen without traffic.  The page shows `lb` or `f/o` beside the
collection's autoscale chip and a link per upstream: the active one
raised, a standby dashed, a broken one blinking as a failed node card
does, a listed master grey.

**No rings (0.6.1).**  An upstream that is itself a perfcached (its
`INFO server` says `perfcached_version`) is not used unless the
collection says `upstream_allow_perfcached = yes`: two nodes reading
through each other, or a fleet reading its own address, would bounce a
miss from node to node.  Allowed, the fetch connection names itself
(`CLIENT SETNAME perfcached-readthrough`) and that node answers it from
what it holds - it never reads through on its behalf - so a key crosses
one perfcached hop at most.  Both ends say so: a refused perfcached
blinks on the page as "daisy-chaining off" and raises the upstream's
warning (mailed); an allowed one is tagged "chained - one hop"; and the
node read that way warns (`chain.<collection>`, mailed) and says on its
read-through line that those reads got only what it holds.

**A master names its replicas (0.7.5).**  A listed master's `ROLE`
names its replicas, and `upstream_discover` decides what is done with
them.  `auto` (the default) adds them as upstreams - read as
`upstream_role` says, tagged "named by its master" on the page and
origin `role` in `/stats` - only
when the master answers at the address dialled (`CLIENT INFO`'s
`laddr`); behind a proxy such as HAProxy in front of Sentinel the
addresses it names are the servers behind the proxy, so they are shown
(`behind`) and not used.  Nor does `auto` add them to a write-through
collection: its reads stay on the upstreams its config names, so an
upgrade moves none of them to a replica no lag check guards.  `yes` dials
them directly whatever stands in front; `no` neither uses nor shows them.  A replica the master stops
naming is "gone from the topology": not used, not dialled, its place kept
for its return.  A node holds 32 upstreams (eight before 0.7.5).  Once all
are taken, or the arena has no room for another's copies, a new address
takes the place of the discovered one gone longest - after twice
`upstream_timeout_ms` and a second, so nothing still talks to the old
address - and the log says whose place it took and why; more than fit at
once is logged and the rest not used.

**A Sentinel as the upstream (0.7.5).**  An upstream whose `INFO server`
says `redis_mode:sentinel` is never read from.  The collection asks it,
every 10 s and at once on its events (`+switch-master`, `+sdown`,
`+odown`, `+slave`, `+sentinel` and their pairs: a failover, a server
held down or back, a replica or a sentinel added), which master it
watches, where that master is, its replicas and the other sentinels, and makes the
master and the replicas its upstreams - "named by a sentinel" on the
page, `origin` `sentinel` in `/stats` - read as `upstream_role` says.  A
replica the sentinel holds down is not read, and a server it stops
naming is gone from the topology, as above; but one sentinel saying
every replica is down retires none until another sentinel agrees.  A
sentinel watching several masters needs `upstream_sentinel_master =
<name>`; until it is set the collection's `sentinel` field in `/stats`
says which masters it saw.  `upstream_sentinel_password` is the
sentinels' own `AUTH` password, where they have one.  When the sentinel
stops answering, the others it named are asked; they and the servers
are kept in `state_dir` and tried at a start with the seed down.  Under
`upstream_writes = through` the writer takes the master the sentinels
name.  With `upstream_discover = no` what they say is shown and none of
it used.

    [collection 0]
    upstream = redis://reader@10.0.0.20:26379
    upstream_sentinel_master = mymaster

**TLS to the upstreams (0.7.5).**  `rediss://` in place of `redis://`
reaches the upstreams over TLS - all of a collection's or none, since
what is found from one (a master's replicas, a sentinel's servers and
the other sentinels) is dialled the same way.  The certificate is
checked against `upstream_tls_ca` (a PEM file; the system's store when
not given) and against the URL's host - an IP address against the
certificate's IP addresses - or against `upstream_tls_name`, which is
also sent as the server name; that is the one to set when the
certificate names a host and the URL an address.  A Redis with
`tls-auth-clients yes` (its default once TLS is on) wants a client
certificate: `upstream_tls_cert` and `upstream_tls_key`.  A self-signed
certificate can be named in `upstream_tls_ca`; `upstream_tls_verify =
no` skips the check altogether and is warned about at start, since
anyone on the path can then pose as the upstream.  The files are read at
start (`-C` checks them).  A reload (`systemctl reload perfcached`)
reads them again: a renewed CA, client certificate or key is used by the
next connection, and a file that does not load fails the reload and
changes nothing; naming other files takes a restart.  `/stats` shows a
`tls` block for the collection: what is checked, the handshakes, the
failures and the last reason, which the log says too.

    [collection 0]
    upstream = rediss://reader@10.0.0.12:6380/0
    upstream_tls_ca = /etc/perfcached/redis-ca.pem
    upstream_tls_cert = /etc/perfcached/redis-client.pem
    upstream_tls_key = /etc/perfcached/redis-client.key

**A Redis Cluster (0.7.5).**  One node of it is enough: an upstream
whose `INFO server` says `redis_mode:cluster` puts the collection in
cluster mode.  A thread asks a node for `CLUSTER SHARDS` (`CLUSTER
SLOTS` before Redis 7) every 10 s, adds every master and replica it
names as an upstream (origin `cluster`), drops the ones it stops naming,
and keeps the slot map: each key's fetch goes to its slot's shard, and
`upstream_role` and `upstream_master_fallback` choose within the shard.
A replica connection sends `READONLY` first.  A `MOVED` is followed to
the node it names - read there once, the map asked again at once - and
an `ASK` there after `ASKING`, three hops at most; `TRYAGAIN` and
`CLUSTERDOWN` are fetch errors.  `/stats` gives the collection a
`redis_cluster` block (what the cluster said, the slots no node serves,
`moved`, `asked`) and each upstream its `shard` and `slots`.  The
nodes are kept in `state_dir` and tried at a start with the seed down.
A cluster has database 0 only: a URL ending `/1` is refused by its
`SELECT`, and the log says so.  A cluster has at most 32 nodes here
(`upstream` takes 32 now); a larger one is logged and its nodes past
32 are not used.  Under `upstream_writes = through` the writer holds a
connection to each master and sends each write to the master of its
key's slot, a run of writes to one master pipelined; a write whose keys
are in different slots is refused as Redis refuses it (`CROSSSLOT`, use
a hash tag), a `MOVED` (not applied) is sent once more to the node it
names, and an `ASK` - a slot migrating - is refused, not applied.  A
command perfcached does not know is not passed to a cluster
(`upstream_unknown = pass` does not reach one).  The drain sweep SCANs
each shard on a server that serves it, a replica first; the upfill
writes each key through the writer, so to its slot's master; `upfill
check` reads one master and is refused for a cluster.  A
perfcached saying `redis_mode:cluster` (its RESP door's cluster mode)
is a perfcached, under the rule above.

## What perfcached sends the upstream

**One way only.**  perfcached sends the upstream nothing but the
handshake (`HELLO`, `AUTH`, `SELECT`, `INFO server`, `CLIENT INFO`,
`CLIENT ID`, `CLIENT TRACKING`; a cluster's `READONLY`, `ASKING` and
`CLUSTER SHARDS` or `SLOTS`), the checks (`ROLE` and `INFO replication`,
`PING`; a sentinel's `SENTINEL` queries and `SUBSCRIBE`)
and reads (`TYPE`, `PTTL`, `GET`, `HGETALL`, `JSON.GET`, and `SCAN` for
the drain sweep).  Nothing it
does changes the upstream's data, so anything perfcached handles
differently from Redis stays on perfcached's side.  Point it at a
replica (a Redis replica refuses writes) and give it an ACL user that
can only read:

```
ACL SETUSER reader on >password ~* +@read +hello +auth +select +ping +info +role +client|id +client|info +client|tracking
```

`+role` is what `upstream_role = replica` and a master's list of its
replicas are read by, and `+client|info` the proxy check of
`upstream_discover = auto`; a Redis Cluster also needs
`+readonly +asking +cluster|shards +cluster|slots`.

## A key it does not hold

**What a command does with a key it does not hold:**

- A read (`GET`, `MGET`, the `H*` reads, `JSON.GET`, and the native
  door's `get`, `jget`, `mget`, `hcmd` reads) waits for one fetch and
  answers from the copy.  The copy keeps the upstream's TTL, rounded up
  to whole seconds.  Concurrent misses for one key cause one fetch.
- A probe (`EXISTS`, `TTL`, `PTTL`, `TYPE`, and the native door's
  `exists` and `ttl`) needs no value: one round trip of `TYPE` and `PTTL`
  answers it, and a hash or a JSON document is remembered as PRESENT -
  its type and the upstream's TTL, no value moved or stored.  The next
  probes answer from that; a read of the key then fetches the value.  A
  string's value comes in the same round trip and is kept as a copy.
  Client tracking covers the key as it covers a copy: the upstream
  changing, deleting or expiring it drops the PRESENT mark.
- A change (`INCR`/`DECR` and their `BY` forms, `HSET`, `HINCRBY`,
  `HDEL`, `JSON.*` path writes, `SET` with `NX`/`XX`/`GET`/`KEEPTTL`/
  `IFEQ`/`IFNE`, `DELEX`, `DEL`, `UNLINK`, the `EXPIRE` family, and
  their native counterparts) waits for the fetch too, then PROMOTES
  the copy - writes it into the collection as a client write would,
  logged and replicated - and only then runs.  `INCR` of an upstream
  `41` answers `42`; `DEL` of a key only the upstream had answers 1.
- A plain overwrite (`SET`, `SETEX`, `PSETEX`, `JSON.SET` at the root,
  native `set` and `mset`) does not wait.  An ownership check reads the
  key from the upstream in the background.
- `KEYS`, `SCAN` and `DBSIZE` see only what the collection holds
  (`RANDOMKEY` is not served).  `RL.HIT` and the lock scripts never
  fall through.

A RESP pipeline whose first command waits on a fetch is answered in
order: the connection holds until that command has run.  The native
door's frames carry ids, so one waiting frame does not hold the others.

**A key the fleet has written or deleted never falls through again.**
perfcached cannot delete the upstream's copy, so it remembers instead:
each read-through collection has a companion collection, `<name>.nofall`,
created and replicated with it and persisted like any collection.  It
holds a key for as long as the upstream's copy may live - the copy's TTL,
or for good when it has none.  Without it, a key deleted here, or one
that expired here, would read through to the old value.  The companion is
visible in `stats`; do not write to it.  The status page shows it on its
collection's read-through line ("N keys written here, never read from the
upstream again"), not as a row of its own, and counts the copies a
read-through collection fetched in its entries.

## Tracking the upstream

**The upstream's own changes** reach perfcached through Redis client
tracking (`upstream_tracking = on`, the default; it needs RESP3, so Redis
6 or later).  The fetching connections hand their invalidations to one
dedicated connection, and a key the upstream changes, deletes or expires
is dropped from the fetched copies at once - the next read fetches it
again.  If a tracking connection is lost, every fetched copy is dropped,
because an invalidation may have been missed.  A copy fetched while
tracking is not running lives at most `upstream_negative_ms`.  With
`upstream_tracking = off`, copies keep the upstream's TTL and see none of
its later changes.

An invalidation can overtake a fetch: the fetch reads the value, the key
changes, and the push for that change arrives on the dedicated connection
before the fetch's reply arrives on its own.  Before 0.7.1 the reply was
then kept - the value from before the change, with nothing left to
invalidate it.  Now an invalidation marks a fetch of its key that is in
flight, and when that fetch completes its copy is dropped and the key
fetched again, its waiters still waiting (three rounds at most; after
them the copy is kept for the waiters, but held no longer than
`upstream_negative_ms`, as a copy nothing tracks is: the change that
overtook it has spent its invalidation, so none would reach it).
`inflight_invalidated`, `inflight_refetches` and `inflight_kept` in the
read-through stats count them.

A read can lose its key the same way, after the fetch.  The node decides
that a key is held (a record, a copy) and then reads it, and a change can
land in between: an invalidation from any tracked server takes the key's
copies from every server, and the echo of a write drops the record.
Before 0.7.2 such a read answered nil for a key the upstream holds -
rarely, and each time just after a replica's echo of a write.  Now a pure
read (GET, MGET, EXISTS, TYPE, TTL, PTTL, the hash reads, JSON.GET) whose
key went while it ran is answered again from a fetch, twice a command at
most.  When nothing was removed meanwhile this costs two atomic reads;
`read_lost` in the read-through stats counts the reads answered again, and
`read_lost_spent` (0.7.5) the ones still lost after both, answered as read.

Tracking needs every connection on ONE server.  A fetching connection
names the dedicated one by its client id, and a client id means something
only on the server that issued it: behind a round-robin proxy in front of
several replicas, the id may be nobody's on another server - or somebody
else's, who would then receive the invalidations while the copies here
never change again.  So each connection reads its server's `run_id`
(`INFO server`, hence `+info` in the ACL line above), and one that reached
another server than the dedicated connection is not tracked: its copies
live at most `upstream_negative_ms`, `stats` counts it as
`tracking_other_server`, and the log says so once.  Point `upstream` at
one server, or at a proxy backend that keeps a client on one server
(HAProxy `balance source`, and `on-marked-down shutdown-sessions` so a
failover drops the connections - and with them every fetched copy).  A
user that may not run `INFO` gets no tracking at all.  `stats` shows the
dedicated connection's server as `upstream.server`.

## Failures, read-only collections, knobs

**When the upstream fails**, a read answers
`-ERR upstream: the read-through fetch failed ...`
(`upstream_on_error = miss` makes a read answer as a miss instead).  A
change always answers the error: it never runs on a guess.  A value the
collection cannot hold - a list, set, sorted set or stream - answers
`WRONGTYPE`, and one larger than a record answers an error.  Neither is
stored.  A key the upstream does not have is remembered as absent for
`upstream_negative_ms`.

**A collection clients can only read**: `upstream_writes = refuse`
(default `allow`).  Every write to the collection - SET, DEL, INCR,
EXPIRE, HSET, JSON.SET and the rest, on every door - is refused with
`collection 'X' is a read-through of <host>:<port> and refuses writes
(upstream_writes = refuse)`, and so are a drain sweep and a restore into
it.  With writes allowed, the first write to a key makes it the fleet's
own and the upstream's later changes to it no longer reach the readers;
for a collection that only mirrors its upstream, refusing the write is
the safer answer.  No `<name>.nofall` companion is created, `stats`
counts the refusals as `upstream.writes_refused`, and every member must
agree on the setting.

The other knobs: `upstream_timeout_ms` (500 - one fetch, connecting
included), `upstream_conns` (2 connections per node),
`upstream_negative_ms` (2000), and `upstream_drain_rate` (5000 keys a
second) with `upstream_drain_mb_rate` (16 MB a second, 0.6.5) for the
drain and upfill sweeps below.

**How long to believe "absent"**: with tracking running, Redis tracks the
name of every key a fetch asked about, present or not - a key created
after a miss invalidates its absent mark as it would a copy, and the next
read fetches it at once.  `upstream_negative_ms` then only bounds how
stale an answer can be while tracking is NOT running (a lost tracking
connection, a fetch that reached another server, `upstream_tracking =
off`).  A collection that is asked mostly about keys the upstream does not
have - deny lists, push tokens of devices that never registered - can
raise it to minutes and send the upstream a small fraction of the
lookups; keep the default when tracking is off.

## In a cluster, and finishing a takeover

**In a cluster**, every node needs the same upstream configuration.
Each node fetches its own copies; what changes a key is a real write, and
it replicates.  A read on another node in the instant before a change
reaches it may answer the upstream's older value - the same window in
which an eager fleet without an upstream would answer a miss.  A `proxy`
collection refuses an upstream at start, because another node decides
its keys; `shard` and `spread` fetch at the key's owner or holders
("Where read-through and write-through are proven", below).

**Watching it, and finishing.**  `stats` has an `upstream` block per
collection: connections and tracking, fetches and fills by type, absent
and wrong-type answers, promotions, no-fall entries, ownership checks,
invalidations, flushes, errors, timeouts and the average fetch time.

A key nobody reads stays on the upstream, so finishing usually starts
with the DRAIN SWEEP, `perfcli -E <enable> updrain <collection>`, once
the writers have moved.  It scans the upstream (`SCAN ... MATCH` for each
pattern, still reads only) and takes over every key that the fleet does
not already hold, has not written and has not deleted.  Each one is
written here with the upstream's TTL and replicated; the fleet's own
version always wins.  Run it on one node - the writes reach the others.
It reads at most `upstream_drain_rate` keys a second (5000; 0 removes the
cap) and `upstream_drain_mb_rate` MB of values a second (16; 0 removes
it), whichever is reached first and reports in `stats` as `upstream.drain`: its state, and the keys
read, taken over, already the fleet's, gone, of a type it cannot hold,
too big, and failed.  `updrain <collection> stop` stops it.  Under the
dashboard tool the same sweep is the read-through line's **drain**
button, and the line carries its figures.

When it is done and `fetches` has stopped rising, nothing more comes from
the upstream.  Remove `upstream` and `upstream_prefixes` on every node and
restart them.  The companion collection is not created again, and its
records are dropped at recovery as orphans.

## Read-through and write-through in shard and spread

**Where read-through and write-through are proven.**  Their suites run
on single nodes and on eager fleets.  In `shard` (0.7.1) a read-through
collection fetches at the key's OWNER: a miss on any node pulls from the owner, which fetches from
the upstream and keeps the one copy, so the fleet holds each key once
and no node fetches a key it does not own; the daemon refused the
combination before 0.7.1.  That holds with or without
`upstream_writes = through` since 0.7.5 - before it, an owner fetched
for a pull only when the collection wrote through, and a read-only one
answered absent every key the node asked on did not own.  A hash read
(HGET, HGETALL and the rest) and a JSON GET on a node that does not own
the key pull the record from the owner the same way and are answered
here (0.7.5); before, they asked the owner's table only, and a key it
held no copy of read as empty.  In `spread` a node that holds no copy of
a key pulls a hash or a document from a holder as it pulls a string
(0.7.5; before, it read its own table, which holds K of the keys).  On
RESP a JSON command on a key another node holds is still refused, except
that JSON.GET pulls as above.  Write-through in `shard` (0.7.1) runs at the
key's OWNER too: a write on any other node is forwarded to the owner as
every shard write is, the owner sends it to the master, reads the key
back and keeps the one copy, and the client gets the master's answer
(`stored: false` for a `SET NX` the master declined; "holder rejected"
for an error there, whose text is in the owner's `last_errors`).  This
covers the single-key writes the forward plane carries: set, setnx,
setxx, del, add, sub, expire and their RESP forms.  A hash write, a JSON
write and mset still run through on the node the client wrote on.  In
`spread` (0.7.1) a through collection lives at the key's K HOLDERS: a
miss on any other node pulls from a holder, which fetches from the
upstream (read-only or written through, since 0.7.5) and keeps the copy (a second holder fills on its own demand -
at most K copies, never one per node); a write-through runs on the node
the client wrote on and its read-back is pushed to the K holders.  The
drain sweep (`updrain`) on every node takes over its own share only -
the owner's in `shard`, a holder's in `spread` (`drain.elsewhere` in
`/stats` counts the keys it left to the other nodes).  In `proxy`
read-through is still refused at start.

## Write-through

**Write-through (0.6.2).**  `upstream_writes = through` sends every client
write on the collection to the upstream's master first.  The master is the
listed upstream whose `ROLE` says master - list it beside the replicas;
reads still go only where `upstream_role` says.  Two that say master are
decided by the master the serving replicas follow; with none, or a tie,
every write is refused (`-ERR upstream: no master reachable ...`) and
reads go on.  One writer thread per collection holds one connection to the
master and sends the writes in the order they arrived.  Each write is
followed in the same round trip by a read-back of its keys (`TYPE`, `PTTL`
and the value), and the fleet then holds what Redis holds: a value as the
collection's record, with Redis's expiry; a key gone as gone (reads do not
fall through to the replicas for the next `upstream_max_lag_ms`, at least
2 s, while they catch up); a list, set, sorted set or stream left to
Redis.  The client gets Redis's own reply.  A write the master refuses
(`WRONGTYPE` ...) changes nothing here, and neither does a conditional one
it did not apply (`SET NX` on a key that exists).  If the connection to
the master fails during a write, the client is told the write may or may
not have been applied.

What is forwarded: at the RESP door, every write command as the client sent
it - perfcached's `EX` on `JSON.SET` becomes an `EXPIRE` after it, as
RedisJSON has none; `EVAL`, `EVALSHA` and `RL.HIT` write this node only
and are refused.  At the native door: `set`, `setnx`, `setxx`, `del`,
`expire` (a TTL of 0 is `PERSIST`), `jset` (not `mkpath`), `jdel`, `add`
and `sub` (`INCRBY`/`DECRBY`; the verb's TTL as it means it - below 0 keeps
the key's, 0 is a `PERSIST` after it, above 0 an `EXPIRE`; the reply is
Redis's new value), `hcmd` writes (`HSET`, `HSETNX`, `HMSET`, `HDEL`,
`HINCRBY`, `HINCRBYFLOAT` as sent, read back whole; Redis's reply, an error
too, in the shape the hash commands answer here) and `mset` (one `MSET`, an
`EXPIRE` after it per item with a TTL, every key read back; an `mset`
whose keys are all outside `upstream_prefixes` stays on this node, and one
mixing keys inside and outside is refused whole).  The conditional sets
and deletes (`setifeq`, `setifne`, `delifeq`, `delifne`), `jincr`,
`jarrappend` and `rlhit` are refused, naming what is forwarded.  JSON writes need
RedisJSON on the master: the writer asks `COMMAND INFO JSON.SET JSON.DEL`
when it connects, and without them JSON writes are refused here, with no
round trip.

**Switching to `through`: the keys the fleet already owns (0.6.4).**
Keys written here while the collection was on `allow` are the fleet's
own: Redis never had them, and under `through` they stay on the fleet.
Reads are answered here, but a change Redis must apply first fails there
- a field update inside a document (`JSON.SET k $.x v`) has no document
to update.  `upstream.write_through.fleet_owned` in `stats` counts them on
each node, a minute apart.  After the switch, run `perfcli -E <enable>
upfill <collection>` on one node: it writes each of them to the master
through the writer, in order with the clients' writes - `SET ... NX` with
its remaining TTL for a string or a counter (its value on that node),
`JSON.SET $ ... NX` and, only where that applied, a `PEXPIRE` for a
document.  A key Redis has meanwhile keeps Redis's value: the node reads
it back as it does after any write, and it replaces the fleet's record
on every member.  The sweep compares the two first (0.6.5) and counts
Redis's as the same, the same with another expiry, or different; each
key that differed is named in the log with both lengths and expiries,
never its value.  The comparison runs on the sweep's own thread, not
the writer's, so the clients' writes never wait behind it;
`compared_text`, `compared_walked` and `uncompared` say how each verdict
was reached, and a document nested too deep to walk, or one the
hand-off had no room for, counts as uncompared - never as different.
Run `perfcli -E <enable> upfill <collection> check`
before the sweep: it reads each key from the master and makes the same
comparison, writing nothing, so you know what the sweep would replace.
Hashes, rate-limit and lock
records are skipped: Redis has no conditional write for a whole hash, and
the other two are perfcached's own.  The sweep writes at most
`upstream_drain_rate` keys and `upstream_drain_mb_rate` MB of values a
second and never fills more than half of `upstream_write_queue`, so the
clients' writes keep room - at the defaults it takes at most about a
quarter of the writer thread whatever the values' size
(`writer_busy_pct` shows it); `stats` shows it
as `upstream.write_through.upfill` (found, written, Redis had and how
it compared, missing for a check, skipped, gone, errors), and
`upfill <collection> stop` stops it.

**Commands perfcached does not know** (`upstream_unknown = pass`, the
default under `through`; `refuse` keeps "unknown command"), at the RESP
door only.  Redis classifies them - `COMMAND INFO`, `COMMAND GETKEYS` - and
a write goes to the master with its keys read back as above, a read to a
serving replica on a thread of its own.  The reply is relayed byte for byte
(these connections speak RESP2, as the RESP door does), up to
`upstream_pass_max_kb` (1024, from 64 to 4096); a larger one is refused,
never cut.  Refused whatever Redis says: admin, dangerous, blocking,
pub/sub, transaction and script commands, commands naming another
database, writes that name no key, and keys outside `upstream_prefixes`.

**The checks never follow the traffic.**  The writer looks for the master
when the node starts and after a failure - at once, then backing off to
every 10 s - and asks it `ROLE` every 10 s while connected; RedisJSON is
asked again every 60 s while it is missing.  `stats` has the writer under
`upstream.write_through`: its target, its state, RedisJSON, what it
forwarded by outcome and by command (each with its five-minute window),
the slowest write in five minutes, and the last eight errors with
Redis's own words.  The page shows them under the read-through line.
`writer_busy_pct` (0.6.5) is the writer thread's share of the CPU over
the last five minutes: one thread carries every write-through write of
the collection, and an upfill sweep's beside them, so this is the gauge
for how much room the writes have; the page shows it on the writes
line, amber from 75%.

## Other writers, echoes, reading your own write

**Other writers.**  Every node also follows every change Redis makes under
the collection's prefixes, whoever makes it: its tracking connection to
each upstream asks for broadcast tracking (`CLIENT TRACKING ON BCAST`,
with a `PREFIX` per `upstream_prefixes` pattern that is a prefix, or every
key when one is not - `*`, or a wildcard in the middle).  When Redis says a
key changed, the node drops the record written through, and the next read
fetches Redis's value from a serving replica.  So a write another Redis
client makes directly is seen, within the replicas' lag.  The key's
no-fall entry goes with the record - unless the record is not there
(0.6.4): one replicated from the node that made the write may land after
the invalidation, and it must land known as written through, not as the
fleet's own, so an entry found alone is kept.  A read that finds an entry
without its record drops it and fetches Redis's value; `bare_marks` in
the write-through stats counts those, and they are rare.  A key outside `upstream_prefixes` is the
fleet's own and is never dropped.  If a tracking connection is lost, every
key written through is dropped on that node (it may have missed a change)
and fetched again when read; keys the fleet owned before the switch to
`through` are kept.  The cost is one invalidation message per change under
the prefixes, to every node - with `upstream_prefixes = *`, per change in
the database.

**The echo of a node's own write (0.7.1).**  Redis sends that invalidation
for the node's own write too - the writer's connection is not the tracked
one, so NOLOOP cannot stop it - and before 0.7.1 the echo dropped the
record the node had just stored: a key written through was never cached,
and every read after a write fetched.  Now an invalidation that arrives
within `upstream_echo_ms` of this node's own write (2000 ms, raised to
twice `upstream_max_lag_ms` when that is larger) refetches the key instead
of leaving the slot empty.  The copy that comes back is Redis's current
state, so a change by another client racing the write is honoured, not
papered over; each tracked upstream echoes once, so a key costs at most
one fetch per tracked upstream inside the window.  `echo_refetches` in the
write-through stats counts the refetches asked (a second echo arriving while
the first fetch is under way joins it).  `upstream_echo_ms = 0` restores the
drop.

**Reading your own write (0.7.1).**  The master echoes the write at once,
before the replicas have it, and a fetch from a replica that has not caught
up answers the value before the write, or none: a client could read nil for
the key it had just set (about 4% of writes where the
replicas lag a few milliseconds).  So inside the same window a fetch of that
key - the echo's refetch, or a read that misses - goes to the master listed
among the upstreams, which has the write by definition; outside it reads go
to the replicas as before.  The master must be listed (its `ROLE` says so)
and reachable, else the fetch goes where it always did.
`echo_master_fetches` in the write-through stats counts them: about one per
write per tracked upstream, a read on the master that the window costs.
`upstream_echo_ms = 0` turns this off with the refetch.

**Reading your own delete (0.7.3).**  A delete written through leaves a
mark that answers "gone" here while the replicas catch up - but Redis
echoes a delete too, and the echo took the mark: a read after it could go
to a replica that had not applied the delete yet and answer the value just
deleted (once in about 29,000 deletes where the replicas lag
a few milliseconds).  A delete now opens the same window as a write, so
that read goes to the master.  `upstream_echo_ms = 0` turns it off with the
rest.

**After a tracking gap: warm again (0.7.1).**  A restart, or a lost
tracking connection, is a gap: Redis sent invalidations to nobody, so the
records written through that the node still holds (the WAL replays them)
cannot be trusted and are dropped.  Before 0.7.1 the node came back cold,
every held key one fetch on its next read (measured: a restart took
a node from 1,036 held records to 12).  Now their names go on a re-warm
list and are fetched again in the background at `upstream_drain_rate`
(the drain sweep's throttle), newest written first, at most
`upstream_rewarm_max` names (100,000; 0 turns it off); a key a client
reads before its turn is skipped, a key the upstream no longer has comes
back as an absent mark.  A clean stop also saves the names of every copy
the node holds (the fetched ones are not persisted) to the state
directory, so a restart re-warms them too; the file is read once and
removed, and `upstream_rewarm_max = 0` discards it.  A lost tracking
connection drops the copies fetched from that server too (an
invalidation may have been missed); their names go on the same list
(0.7.1).  The run fetches only from a server whose tracking is up - a copy
fetched before would be untracked, and dropped again when the tracking
comes up - so at a start it waits for the first such server.
`rewarm` in the collection's upstream stats shows the run (state, queued,
started, skipped, dropped, runs, and requeued: names queued again because
their copy was dropped at a tracking change or landed untracked), and the
read-through line on the page says how many are still to come.  A server that refuses broadcast tracking (before Redis 6, or
an ACL without `CLIENT`) raises `write.<col>.tracking`: other clients'
changes are then not seen until the key expires.

## Queue, credentials, limits

**A slow master.**  Writes wait for the master in one queue per collection,
at most `upstream_write_queue` of them (10,000; the ones the writer is
sending count): past it a write is refused at once - `the write-through
queue is full ... not applied`.  A write's `upstream_timeout_ms` runs from
the moment it is queued, and one with less than a quarter of it left is
answered "not applied" without being sent.  So a client hears "may or may
not have been applied" only about a write that was sent and not answered;
every other refusal is certain.  The warning `write.<col>.queue` says
either happened in the last five minutes.

**The writer's own credential (0.6.4).**  The writer logs in with the
collection's `upstream_password` (and the user in the `upstream` URL)
unless the collection gives it `upstream_write_user` and
`upstream_write_password`: then the writer alone uses those, and every
other connection - fetches, tracking, passed reads - keeps the read
credential, so the readers' Redis user needs no write rights at all.
Both keys need `upstream_writes = through`, and a user without its
password is refused.  Give each user the collection's key patterns and
the commands it sends, never a superuser - for `upstream_prefixes = tok:*,
cid:*`:

```
ACL SETUSER writer on >wpass ~tok:* ~cid:* -@all +auth +select +info +role +command|info +command|getkeys +type +pttl +get +hgetall +json.get +set +del +unlink +expire +persist +incrby +decrby +hset +hsetnx +hmset +hdel +hincrby +hincrbyfloat +mset +json.set +json.del
```

`+info`, `+role` and `+command|info` let the writer find the master and
RedisJSON on it, `+command|getkeys` classes the commands perfcached does
not know, `TYPE`, `PTTL` and the value reads are the read-backs, and the
rest are what the native door forwards; add the write commands your RESP
clients send (they are forwarded as sent).

**Limits.**  Pub/sub is not bridged: Redis's channels and this node's are
separate, so move a channel's publishers and subscribers together.  Two
clients writing one key through two nodes at the same moment can leave
Redis and the fleet with different winners; one writer per key is exact.
Each write costs a round trip to the master, and one node sends a
collection's writes to it one at a time: at a 0.4-0.7 ms round trip that
is about 1,500-2,500 writes a second per node and collection, whatever
Redis can take, and a slow write holds the ones behind it.  `MULTI`/`EXEC`
stay unsupported.  In 0.7.5 a value too large for one record (about
256 KB with its key) is still sent to Redis, which takes it, and the
client gets OK, but the fleet cannot hold it: an older copy may answer
until tracking invalidates it, then reads fail as too large.  Keep
write-through values under 256 KB until 0.7.5.1.  Under `through` the
RESP door can write to Redis, so keep a plaintext one on a trusted
segment, behind `resp_allow` and `[secrets] resp`, or serve it over TLS (0.7.5, "TLS on the RESP and
HTTP doors" in [config.md](config.md)).
