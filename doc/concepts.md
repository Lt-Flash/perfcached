# What perfcached promises

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

The contract in one place: what perfcached is and is not, a record's life,
the modes side by side, what a collection keeps, and what happens when
things fail.  The reference documents named have the detail.

## What it is, and what it is not

perfcached is an in-memory key-value cache: one multithreaded daemon a
host, a write-ahead log and snapshots when you ask for them, and, when
one machine is not enough, a fleet that shares the keyspace in one of
five modes.  It is not a database, and does not try to be one:

- **No cross-key transactions.**  perfcached runs no `MULTI`/`EXEC`
  or `WATCH` of its own, and no mode changes two keys as one.
- **No lists, sets, sorted sets or streams**
  ([REDIS-COMMANDS.md](../REDIS-COMMANDS.md) lists what is refused).  A
  new data type comes only with a named user of it.
- **No sync between data centres.**  A fleet is one network, and two
  fleets never merge ([cluster.md](cluster.md), "A cluster has an id").
- **No redirect a client must handle.**  Any node serves any key and
  forwards what it does not hold; RESP answers `-MOVED` only under
  `resp_redirect = moved`.
- **No acknowledgement that waits for the disk** (below).

## A record's life

**A write** goes into the table of the node that took it, then into
that worker's ring for the write-ahead log, and is acknowledged there.
The WAL thread writes the ring out and syncs it as `[wal] fsync` says;
a snapshot later covers it and its log segment is reused.  In `eager`
and `spread` the write also goes to the other holders as it lands,
without waiting for them; in `shard` and `proxy` a node that does not
hold the key forwards the write and answers once the holder has taken
it.  One node logs a write - the one that took it, or the holder it was
forwarded to.  The other holders keep their copies in memory, and a
snapshot carries them ([durability.md](durability.md), "What a replica does NOT give
you").

So an acknowledgement means "in memory, and in the log's queue".  A
`kill -9` loses what the ring still held - at the defaults up to the
last 200 ms or so of that node's writes, more while the log is
behind - and a power cut also loses what was written but not yet
synced, up to about a second under `fsync = everysec`.  `fsync =
always` syncs sooner; it does not make the acknowledgement wait.  A
client that needs a write on disk sends the `sync` verb after it
(`perfcli sync`), a barrier for everything acknowledged before.  A ring
that fills makes the writer wait (`on_full = wait`, the default since
0.7.4) rather than drop.  A collection that writes through to Redis
acknowledges a write once Redis has applied it ([upstream.md](upstream.md), "Write-through").

**A delete** removes the key, is numbered and logged by the node that
made it, and goes to every peer.  Each node remembers it, and refuses
an older copy of the key while it does ([cluster.md](cluster.md), "Deletes survive a
partition").  With acknowledgements (0.7.3, the default) a node forgets
a delete once every member holds it; without them, after
`tombstone_retain_s`.  Either way `tombstone_retain_max` caps the
memory, and three cases can still bring a delete back ([cluster.md](cluster.md),
"Three cases can still bring a delete back").

**Two writes of one key** order by version: a write made after another
was seen is newer and wins on every holder, and so does a write after a
delete.  Two writes of one key made through two nodes at the same
moment race, and which one survives is arbitrary.  Before 0.7.5 a tie -
two writes at one version - could leave eager holders disagreeing; now
every holder settles it the same way, and the repair sweep mends a
holder left with the losing copy ([modes.md](modes.md), "0.7.5 speaks wire epoch 4 alone").
Writing a key through one node - libperfd's `route_keys` does it -
avoids the race.

## The modes, side by side

| mode | holds a record | a miss | a write is acknowledged | holders agree |
|---|---|---|---|---|
| `store` | the node that took it, and each node that pulled it | asks every peer, and keeps what comes | by the node that took it | never: a pulled copy keeps the version it came with until it expires; a later write elsewhere does not reach it, a delete does |
| `eager` | every node | answered "absent" once the node has settled | by the node that took it, before its peers have it | milliseconds after the push lands; the repair sweep mends a lost one |
| `spread` | K nodes chosen by placement | asks the best-ranked holder | by the node that took it, before the holders have it | as eager, among the K |
| `shard` | one node, chosen by placement | the owner answers for it | by the owner | one copy: nothing to agree on |
| `proxy` | one node, chosen by free memory | asks the node the locator remembers, else every peer | by the holder | one copy: nothing to agree on |

A collection with an upstream answers its misses from Redis
([upstream.md](upstream.md)).  The one thing that does not vary: a replication
factor is availability, never durability.  Whatever the mode, one node
logs a record.  The choice between the modes is in [modes.md](modes.md), "Which mode to
run".

## What a collection keeps

Three combinations, named here, in a start line for each collection,
and in `/stats` (0.7.5): `collections[].keeps` says which holds now -
fleet-delete-safe only while every live member acknowledges deletes -
and `copies` how many copies the mode keeps: `1`, the copy factor K,
`all` (eager) or `pulled` (store).

- **ephemeral**: no `[wal]`, or `durable = no` on the collection.  A
  restart starts it empty on that node.  In a fleet the copies its
  peers hold come back the ordinary ways (the sweep, a pull on a
  miss), so in `eager` only a restart of every node at once empties it
  everywhere.
- **recoverable**: a `[wal]`.  A node comes back from its last snapshot
  and its log, losing only what a crash takes from the ring (above).
  After the whole fleet goes down together, each node replays its own
  share and the snapshots carry the copies - which is why the daemon
  warns at start about `mode = eager` with `save = off`.
- **fleet-delete-safe**: recoverable, in a fleet whose every member
  acknowledges deletes (0.7.3, `tombstone_acks = yes`, the default).
  A delete is kept until every member holds it, so a member that was
  restarting, frozen or cut off gets it on return - within
  `tombstone_retain_s` and `tombstone_retain_max`.  Past them, an eager
  member back without a restart is asked to rebuild instead.

## When things fail

| mode | a node crashes | a peer is slow |
|---|---|---|
| `store` | the keys only it held are out of reach until it returns | a read only it could answer waits for it, up to `pull_timeout_ms` |
| `eager` | nothing is lost but what it had not pushed; it returns and copies the state of a peer that was up throughout (0.7.5), or, with none, returns from its log | it applies late, quietly (the `replica intake` card), and the sweep mends what it drops |
| `spread` | its keys survive while one of their K holders is up | as eager |
| `shard` | its slice is out of reach until it returns | the keys it owns wait for it |
| `proxy` | its keys are out of reach until it returns | the keys it holds wait for it |

**A split.**  Both sides keep serving and taking writes: the fleet
favours availability, and no side waits for a majority.  On the heal
one master remains, the newer version of each key wins, and the deletes
made meanwhile are replayed, within `tombstone_retain_s` ([cluster.md](cluster.md), "Deletes
survive a partition").  **An upgrade** is a rolling restart, one node at
a time, each once `stats.cluster.restart.safe` says so ([modes.md](modes.md), "Rolling
restarts") - except one to a release that changes the cluster wire, as
0.7.5 does: then every node stops and they all start on the new build
([modes.md](modes.md), "0.7.5 speaks wire epoch 4 alone").

In every mode a node that stalls says `stalled` and is steered around,
and a node whose log failed says `failed` and refuses writes ([cluster.md](cluster.md), "What a
node says about itself").  An eager node back while a peer was up
throughout its absence copies that peer's state whole ([modes.md](modes.md), "Copying a
survivor's state", 0.7.5).  A member back after longer than
`resync_after_s`, or from a split nobody witnessed, drops what it held
and pulls the fleet's state again ([modes.md](modes.md), "A member back after a long
absence").  The whole fleet down at once is the WAL's and the
snapshots' to bring back ([durability.md](durability.md)).  A release whose cluster
header changes cannot talk to the one before it: its ChangeLog entry
says so, and the fleet moves together - `fleetstop`, install, start
(0.7.4.3 was one).  [PRODUCTION.md](../PRODUCTION.md), "Upgrading", has
the order of a rolling restart.
