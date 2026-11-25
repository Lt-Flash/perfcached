# The modes

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

The five modes differ in one thing: how many nodes hold a record, and
which.  Every mode may accept a write on any node.

## store and eager

**`store`** pulls on a local miss and KEEPS the copy, so every node
converges on the working set.  Each node's ceiling is its own arena.

**`eager`** is store plus a push: every write goes to every live peer
as it lands - on the write path, fire-and-forget, whatever the TTL - and
a background sweep repairs what a push did not reach.  Replicas are held
off the WAL: a record is durable where it was written, and a node that
restarts with no peer up throughout holds its own writes until the
sweep refills the rest; with one, it copies that peer's state (0.7.5).

**Reading your own write (0.7.5).**  An eager write is acknowledged by
the node that took it before its peers have it.  So a client reads its
own write back from that node at once, and from any other node once the
push has landed - a few milliseconds.  Across that node's death or stall
the promise ends: a client that moves to another member may read the
value before its write, and if the node died before pushing, the write
is gone - for good when the node returns while a peer was up throughout,
since it then copies that peer's state (0.7.5); only a node that returns
to no such peer brings it back from its WAL.  Nothing on the client can
close that window.  Shard mode reads a key at its one owner, so it has no
such lag, but a key is out of reach while its owner is.

**One copy, in every mode.**  The replica count is availability, never
durability: only the node that took a write logs it, so the fleet's
data is the UNION of the WALs and no single node's is complete.  After a
whole-fleet outage - a power cut, not a rolling restart - each node
replays its own share and the fleet re-shares the rest over the repair
sweep.

**Copying a survivor's state (0.7.5).**  An eager node that returns
while some peer was up throughout its absence - a survivor: live, ready,
and up at least 30 s before the node went down - does not trust what it
replayed.  It drops the replayed records and copies that survivor's
state whole, so a key deleted, expired or rewritten while it was away
reads as the fleet has it.  It is RECOVERING while it copies: a client
is refused rather than answered from a half-copied store.  (In 0.7.5 a
node rejoining while a reconcile pass is open can report ready before
its copy has finished, and for that moment answer a key deleted while it
was away, or an older value - fixed in 0.7.5.1.)  The same
happens when a floor or a split's rebuild (below) puts the records it
stored in question: it drops those and copies a survivor's state over
them, keeping what it wrote since.  A write the node took and had not
pushed when it died - a few milliseconds' worth, or a push the repair
sweep had not yet mended - goes with its files: the survivor never had
it.  The copy is taken from survivors only, the next one if a source
fails, and counts only once its source is heard again unrestarted: a
survivor that restarted since it was picked answers with what it
replayed, not the fleet's state.  No snapshot runs until the copy is
complete, and one follows it, so the files from before it go.  If every
survivor is gone before it finishes, or the one that served it had
restarted, the node stops without a snapshot (exit status 75 - the
packaged service restarts it) and comes back from its own files - with
any delete the survivors replayed to it meanwhile, which its WAL kept;
with no survivor then, it keeps them.  With no survivor at all - the whole
fleet went down, or every live peer restarted while it was away - it
keeps what it replayed and nothing is deleted
(`stats.cluster.reconcile_skipped`): a key deleted or expired meanwhile
can come back.  Boot times cannot tell a fleet that rolled from the one
survivor of a staggered failure, and keeping is the safe side.
`stats.cluster.rejoins` counts the copies, `resync_dropped` the records
they dropped, and `resyncing` is true while one runs; the log names the
survivor.

Before 0.7.5 the returning node instead asked the fleet whether each
replayed key survives, holding its recovered copies until answered
(`repl_held_recovered`, `pull_held_recovered`), as the next two
paragraphs describe.

**The fast path (0.6.4).**  A node keeps no copy and no delete it
receives from its peers, so what a restart can bring back is the deletes
and expiries made since its last snapshot BEGAN.  The returning node
first asks one witness for exactly those, from the witness's retained
deletes.  When the witness vouches for that whole window and the whole
replay arrives, the pass is over: seconds, however many keys came back
(`stats.cluster.reconcile_fast` is true, `reconcile_fast_deletes` says
how many were replayed).  The witness refuses when the window is older
than its `tombstone_retain_s`, when its `tombstone_retain_max` cap
dropped entries inside the window, or when it keeps no retained deletes;
a node that loaded no snapshot does not ask; a witness older than 0.6.4
does not answer.  Then the node asks the fleet about every replayed key,
as before, and `reconcile_fast_why` says why.  `reconcile_fast = no`
(`[cluster]`) always asks key by key.

**Asking key by key.**  The question goes out in slices ten times a
second, up to a quarter of the free pending-request slots a second,
between 64 and `reconcile_rate_max` (`[cluster]`, default 1,024, at most
65,536); a refused slot or a timed-out pull holds it at 64 for 10 s.
`reconcile_budget` is the rate allowed now; while the pass is open,
`reconcile_open`, `reconcile_walked` of `reconcile_walk_total`,
`reconcile_rate` (keys a second over the last minute) and
`reconcile_eta_s` say how far it has got, and the status page shows them
on this node's card (`reconcile_pending` counts the answers not yet in).
Since 0.7.2 the question is a hold-check: up to 256 keys of one collection
in one datagram to every live member, each answered "held, at this
version" or "not held" - never the value, which the question never
needed.  It is asked that way only while every live member answers one
(an older member drops it, so a fleet with one keeps asking key by key
with a pull, as before), and not for a write-through collection, whose
pull can be answered by a fetch from Redis.
`reconcile_holdchecks` counts the keys asked by hold-check and
`holdchecks_served` the keys this node answered for its peers.  After an outage
that took the whole fleet down nobody was, the question is skipped, and
nothing is deleted (`stats.cluster.reconcile_skipped` says so on every
node): a key that expired meanwhile can then come back from a replayed
copy that was older and longer-lived.  The same happens when every live
peer restarted while the node was away - a rolling upgrade during its
absence: nobody saw the deletes, and keeping is the safe side, because
boot times cannot tell a fleet that rolled (complete) from the one
survivor of a staggered failure (which may lack records only this node
holds).  `resync_after_s` (below) bounds how long that can last.

**A member back after a long absence (0.7.3).**  From 0.7.5 an eager node
with a survivor copies it instead (above), unless it was away longer than
`resync_after_s`.  A node that returns -
after a restart or a split - and whose records the fleet cannot rule on
one by one resyncs, as a node joins a Galera cluster by state transfer:
it drops every record it stored before its absence and pulls the
fleet's state from a ready member that holds records, as a joining node
does.  It is RECOVERING while it resyncs: a client is refused rather
than answered from what it holds, and nothing it drops reaches a peer.
(In 0.7.5 a node rejoining while a reconcile pass is open can report
ready before its copy has finished, and for that moment answer a key
deleted while it was away, or an older value - fixed in 0.7.5.1.)
Writes it accepted after the absence began are kept.  It resyncs when
it was away longer than `[cluster] resync_after_s` (default 86400 - a
day; 0 never by time; at most a year), whatever a witness could say -
a node back after a week, the fleet rolled to a new build meanwhile -
and when it comes back from a split (no restart) that no peer was up
long enough before to witness.  The member it pulls from has been up
30 s and is not much younger than the returning node: a peer that
restarted while it ran may hold only its own records.  With no such
member it keeps its records as before.  `/stats cluster.resyncs` counts the
resyncs, `resync_dropped` the records they dropped, and `resyncing` is
true while one runs; the log says why each started.

## Builds and wire epochs

**Which build each node runs (0.6.5).**  Every member's keepalive carries
its build - the version and the wire epochs it speaks - so `/members` on
any node lists `version` and `wire_epochs` for every member (null for a
member older than 0.6.5), each card on the status page shows it, and the
header turns amber with `mixed versions: 0.6.4 x1, 0.6.5 x2` while the
live members run more than one build: an upgrade in progress, or one that
stopped half way.  **The wire epoch (0.7.1).**  A build speaks a range of
wire epochs (`cluster.epoch.min..max` in `stats`) and the fleet speaks one
committed epoch (`cluster.epoch.committed`, carried in the master's
keepalive and kept in every node's `state_dir` as `node-epoch`).  A member
that cannot speak the committed epoch is refused by name.  When a release
speaks a newer epoch beside the old one, a roll leaves the fleet where it
was and `perfcli -E ... upgrade commit epoch=<n>` asks the master to raise
it, refused by name while any live member cannot speak it.  `upgrade plan`
finds that before the roll: every release tarball carries
`share/perfcached/RELEASE` (version, revision, edition, the wire epochs it
speaks), and the plan refuses a candidate whose range leaves out the
fleet's committed epoch.  A rollback to a build that cannot speak the
committed epoch is refused by the driver until the epoch is committed
back down, which the master allows while every member still speaks the
old one.  A datagram from a build whose cluster header version
differs is counted as `cluster.wire_ver_refused` and logged once a minute
by address - "builds this far apart cannot talk" - rather than as a
failed authentication.

**0.7.5 speaks wire epoch 4 alone.**  It keeps no codec of an earlier
release's: a 0.7.4 node (epoch 2) and a 0.7.5 node refuse each other by
name, so moving a fleet to 0.7.5 stops every node and starts them all on
it - the state is kept, the WAL and the snapshot did not change.  A node
whose `node-epoch` still names 2 logs, on its first start, that the whole
fleet moves.  Epoch 4 brings two changes, on from a fleet's first start:
the eager repair sweep compares each record's value checksum as well as
its version, so a holder left with the losing copy of a tie ([concepts.md](concepts.md), "A record's
life" - two nodes writing one key at one version) is sent the winner; and
joining is ordered (below).  A libperfd older than 0.7.6 does not route
by key against epoch 4: every request then takes the forward.

## Ordered joining

**Ordered joining (0.7.5).**  Joining is ordered:

- **Who founds.**  A node that hears no master - a new fleet, or one after
  a whole-fleet stop - founds only at the end of a settle window -
  3 s, stretched while new joiners keep appearing (until 1 s with none), at
  most 10 s - and each live joiner with a lower identity puts it off 3 s
  more, so the lowest identity founds and the rest join it.  A member
  whose master has missed 3 beats tells a joiner the fleet is electing, and
  the joiner waits for the new master instead of founding beside it.
- **One at a time.**  The master admits joiners from a queue in order of
  arrival - one in flight up to 16 members, four above 16 - and tells each
  queued joiner its place.  A newly elected master admits nobody until it
  has heard every live member since its election.  The member list a
  joiner is sent goes in numbered parts once it passes one datagram
  (`max_datagram`), not as one frame that fragments.
- **Every link before it serves.**  An admitted node is `connecting`: live,
  but refusing client data, holding no share of the keyspace and standing
  for no election.  Every pair of it and a member must answer both ways on
  the cluster port, on the pub/sub relay port where one is open, and - by
  a TCP connect, both ways - on each TCP path the configuration uses: the
  frame lane (`discovery = unicast` with `max_datagram` above 0), the
  relay port's TCP where replication groups ride it (`max_datagram` above
  0 and a relay port), and, from a joiner that will pull a bootstrap, each
  member's cluster port on TCP (the bulk channel).  Then the master moves
  it ONLINE and it goes `ready` (`recovering` while it pulls a
  bootstrap).  If a pair has not answered after 10 s the master logs
  which pairs and the first path missing on each (`the cluster socket`,
  `the relay port`, `the frame lane`, `clrpt` - the relay port's TCP - or
  `the bulk listener` `did not answer`), keeps it `connecting` and
  probing, raises `cluster.admission_stuck` and admits the next.
- **A member that misses a peer.**  A member whose list of members differs
  from the master's for 3 beats asks the master for it (`stats
  cluster.members_asked`, and the master's `members_sent`): a member it did
  not know is added, one it dropped for silence in the last 30 s is not.

Every sealed cluster frame names its sender, the sending thread's stream
and its place in that stream, inside the authenticated header, and each
is taken once: a copy the network doubled or someone sent again, one
older than the last 131,072 of its stream, or one from a sender's previous
process while the new one is heard is dropped and counted in
`cluster.replay` (`dup`, `old`, `incarn`; `unchecked` counts frames let
through past the receiver's tables).  A steady `old` count is a path
reordering more than that - a lane or relay backlog - and worth a look.
Since 0.7.5 a frame is admitted only after the sender checks, and only
from the host its header names (`host` counts the others): a holder of
the cluster secret elsewhere cannot speak for a member.  The windows have
a budget (4,096 streams in all, 64 MiB); past it frames go unchecked, as
past the tables.  What the rule does not cover: a frame from a sender's
previous process is taken again once the new process has been silent
60 s - the allowance for a clock stepped back at a restart - so traffic
recorded from a node's last run replays while that node stays down.

## Rolling restarts, stopping, mastership

**Rolling restarts: one node at a time, and only when it is safe.**  The
peers that were up throughout a node's absence are its WITNESSES - from
0.7.5 its survivors, whose state it copies.  Every node says, in `stats.cluster.restart`,
whether restarting IT now is safe: `safe` is false until a restart of it
would be witnessed (`safe_in_ms` says how long), and while any peer's
reconcile is still open - `waits_for` names those peers.  Restart a node
only once it says `safe`, then wait for the next one to be ready and say
so too.  A node restarted with no witness keeps what it replayed; one
whose survivors all restart while it copies goes back to its own files
and keeps them (above) - it never waits for a witness.  Before 0.7.5 its
pass waited instead, holding its replayed copies, and said after 30
seconds to restart that node (`reconcile_no_witness_s`).  A snapshot is what makes a single node independently
recoverable, because the snapshot walk stores the copies too - so `save`
rules are the difference between recovering from any one node and
recovering only from the fleet.

**Stopping on purpose.**  A clean stop (SIGTERM - what `systemctl stop`
sends) takes a snapshot on the way down, whatever the `save` rules say,
so a node stopped on purpose comes back complete on its own.  To stop
the whole fleet, run `perfcli -E <enable secret> fleetstop` on any node:
every node stops that way, and the one you asked reports on its peers
before it stops too.  `stopped` are the peers that said goodbye,
`still_up` the ones still serving when the wait ran out, and `lost` the
ones that left without a goodbye, so nothing says they stopped.  The
wait is 30 s; `fleetstop 60000` waits longer.  It needs the `[secrets]
enable` value, because it takes the fleet down.

**Moving mastership before a restart.**  Restarting the master moves
mastership through an election.  To move it first, run `perfcli -E
<enable secret> master_prefer <node>` on the master: that member becomes
the standby at the next map, and once it has acknowledged the synced
state the master tells it to take over - it promotes at the next term and
the old master steps down on its first beacon, with no silent interval.
A handover not taken within 20 s is given up (the log says so), as is one
whose member leaves; `master_auto` cancels one still pending.

**Removing a member for good.**  A member that leaves keeps its id
reserved for a week, so it gets the same id back if it returns; until
then it stays on the fleet view and raises its `fleet.node<id>` warning.
When it will not come back, run `perfcli -E <enable secret> forget <id>`
on each remaining node: the id is released there, the card and the
warning go, and the fleet counts one member fewer.  Each node holds its
own reservations, so each needs the command.  A member still listed as
live is refused; stop it first.  If it does come back later, it joins
as a new member.

## An eager fleet under write pressure

Applying eager's copies takes a few threads per node - the relay
threads since 0.7.4, or `apply_threads` - while the sending side is
every worker in the fleet, so under enough write pressure a receiver
falls behind, and it falls behind QUIETLY: it keeps heartbeating, stays
a member, and its client door stays fast; the only symptom is a read of
a key that exists on that node solely as a copy it has not applied yet.
Past the receive buffer, datagrams are dropped and the repair sweep puts
those records back.  So an eager fleet under overload is eventually
consistent, with the sweep as the repair.  The `replica intake` card on
the status page shows it - applied per second, the receive queue
against the buffer, the drops - as do `stats.cluster` (`rx_applied_ps`,
`rx_queue`, `rx_rcvbuf`, `rx_drops_ps`) and `/metrics`.  A queue that
sits near the buffer is a node at its apply limit; a nonzero drop rate
is records already lost and waiting on the sweep.  Eager's other cost
is that every node applies every write, so the fleet's write rate is
one node's apply path and falls as nodes are added - the measured case
for `spread`, below.

## proxy, shard and spread

**`proxy`** - the capacity plane - keeps each key on exactly ONE node:
placement by free memory at write time, reads served through without
storing, writes forwarded to the holder, and a 10 s rebalancer that
levels the fleet by live utilization (coldest records first, oversized
ones over a TCP bulk channel).  Fleet capacity ~= the SUM of the arenas.
Whether the rebalancer has anything to do is on every node's `/stats`
(`cluster.rebalance`: `settled` or `leveling`, the reason in words, how
long it has been settled, records moved), the status page's replication
card and `/metrics` (`perfcached_rebalance_settled`) - under steady load
a fleet sits settled; leveling follows a node joining, leaving or
taking most of the writes.

    [cluster]
    multicast = 239.68.68.1:6480
    mode = proxy
    collections = blobs

**`shard`** - deterministic ownership - places each key on exactly ONE
node chosen by rendezvous hashing over the members' addresses
(CRUSH-style): no locator, no placement races, misses answered
authoritatively in one round trip, and counters serialized at the
owner from any ingress.  Membership changes reshard automatically -
only the moving keys travel, and reads fall back to a broadcast during
the move so nothing misses mid-reshard.

    [cluster]
    multicast = 239.68.68.1:6480
    mode = shard
    collections = ids

**`spread`** - K copies, not P.  Each record is held by **K nodes chosen
by placement**, where eager is this with K = P and shard is this with
K = 1:

    [cluster]
    multicast = 239.68.68.1:6480
    mode = spread
    replicas = 3
    collections = sessions

The point is apply load.  Under `eager` every node applies every write
in the fleet; under a copy factor the passive work is (K-1)/P per node
and **falls as the fleet grows**.  `replicas = 1` is refused rather than
aliased to shard.  Any node may accept a write, but only holders keep
it: a node outside the set forwards the record to the K holders and
does not retain a copy - otherwise every writing node becomes a soft
K+1 whose extra copies are orphans, never repaired or reclaimed - and a
pull served through a non-holder is served, not cached.  It is
replication, not erasure coding: the unit of storage stays the whole
record (an erasure-coded mode was built and removed in 0.2.0).

*Status.*  Complete since 0.3.7: placement, the write path, the read
path with libperfd 0.2.8, membership repair on a set change, the
K-scoped counters (`replicas` in `stats`, the `spread_*` counters) and
the fleet measurements below.  What it has not had is production
traffic.  Treat it as a candidate mode - run it on a fleet you can
watch (`spread_*`, `replicas`, `perfd_route_missed()`) before one you
cannot.

*Where it pays, and where it does not* - measured on 0.3.7 with
`bench/spreadbench.sh`; the table is in [benchmarks.md](benchmarks.md):

- **Use it where eager's write ceiling binds.**  Every eager node
  applies every write, so the fleet's write rate is one node's apply
  path and FALLS as nodes are added: 1.84M SET/s at three nodes, 1.11M/s
  at six.  `spread` K=2 held 1.76M/s at six nodes on the same cores,
  +59% at a lower p99, and the gain grows with P.  Each node also holds
  K/P of the keyspace instead of all of it - the memory case is the
  same arithmetic.
- **Use it only with a routing client.**  libperfd 0.2.8 sends each key
  to a holder, and through it reads are eager's, within 5%, at the
  fleet's worker ceiling: 4.25M GET/s at K=2, P=3 with zero pulls.  A
  client that picks a node without the map - a plain load balancer, a
  connection pool - reads at ~0.3M/s with a p99 over 30 ms, nine times
  slower than eager, because a node that is not a holder pulls the
  record over the peer plane, one thread, and at K=2 of P=6 that is two
  reads in three.  Adding nodes makes that worse, not better.  Whether
  a cluster-aware Redis client on the RESP door lands on a holder has
  not been measured.
- **Choose K against the failures you intend to survive**, not against
  eager's habit: losing K specific nodes loses the keys they held,
  where eager loses nothing until the last node.  K=3 of P=3 is eager
  with more bookkeeping - measured identical on reads.
- **Not this mode**: fleets where P <= K (that is eager), read-heavy
  fleets behind a balancer you cannot make cluster-aware, keyspaces
  that need rack or zone anti-affinity (placement is not
  topology-aware), and anything that wants cross-key atomicity (no mode
  has it).

## Which mode to run

**Which mode to run, and the one thing that does not vary.**

*Durability is one copy in every mode.*  Whatever the replication
factor, exactly ONE node writes a given record down: the node that took
the write, into its WAL.  Every other holder - an eager replica, a
spread holder, a pulled copy in `store` - keeps it in memory only.  So
**a replication factor is an availability feature, never a durability
one**: it decides what survives a node dying, not what survives the
fleet dying.  What survives the fleet dying is the WAL and the
snapshots, in every mode, which is why `[wal] save` matters more than
the mode does (see [durability.md](durability.md), and the warning the daemon
prints for `mode = eager` with `save = off`).

With that settled, the choice is about memory, spread and throughput:

| | copies | keys per node | write cost | a node dies |
|---|---|---|---|---|
| `proxy` | 1 | 1/P, by free memory | 1 hop | its keys unavailable |
| `shard` | 1 | 1/P, deterministic | 1 hop, 0 with a routing client | its slice unavailable |
| `spread` | K | K/P | K-way push | survives K-1 |
| `store` | grows with the shared read set | converges on what is read there | 0 | keys read only there are unavailable |
| `eager` | P | all of them | P-way push | nothing lost |

- **Four nodes or more, general purpose: `spread` with K=2 or 3.**  The
  only mode whose redundancy is a number you choose rather than a
  consequence of fleet size - bounded memory, real node-loss survival,
  no fan-out that grows as you add nodes.  It needs more members than
  K: at P=3, K=3 is eager with extra bookkeeping, so spread starts
  being worth it at four.
- **Three nodes that must not lose availability: `eager`.**  At P=3 it
  is the only mode where a node dying costs nothing at all, and that is
  usually what a three-node fleet was built for.  Snapshots are not
  optional here.
- **Throughput first, with a cluster-aware client: `shard`.**  One copy,
  even spread, and a routing client reaches the owner directly.
- **Behind a load balancer, with clients that do not route: `eager`
  (or `store`), not `shard`.**  Through a round-robin VIP a plain client
  reaches a key's owner one time in P; every other request is forwarded,
  and a forwarded request costs about ten times a routed one (measured:
  257,301 against 2,659,574 SETs a second, 50 clients, pipeline 64 -
  before 0.7.2, which keeps RESP replies in order by having a pipeline
  wait behind each forwarded command, so a pipelined plain client now
  forwards one command at a time per connection).
  Eager holds every key on every node, so whichever node the balancer
  picks answers from its own memory.  Choose `shard` behind a balancer
  only when the keyspace does not fit one node.
- **`store` is a cache, not a redundancy story.**  It converges only on
  keys read on more than one node; a key written on A and never read
  elsewhere lives in exactly one memory, so it gives eager's memory cost
  for the shared read set without eager's guarantee for anything else.
- **`proxy` for the largest keyspace per byte**, when a hop on every
  miss is acceptable.

## Reading many keys at once

**Reading many keys at once (0.7.2).**  An `MGET` (or the native `mget`)
answers every key, wherever it lives.  The node asked reads the keys it
holds and looks up the rest exactly as a `GET` would - the owner in
`shard`, the holder the locator remembers (else everyone) in `proxy`,
and, where the collection says `pull = 1`, the best holder in `spread`,
everyone in `store`, and in `eager` only while the node has not settled.
All of them at once, so the read waits about one `pull_timeout_ms`,
not one per key.  Before 0.7.2 a key on another node read as a miss.
- A key whose node does not answer in time is not reported as a miss.
  The native `mget` gives it `"found": false, "unanswered": true`;
  RESP has no way to say that per key, so an `MGET` with any such key
  answers `-TRYAGAIN mget: N key(s) not answered` instead of values,
  and the client asks again.  In libperfd, `perfd_mget` fails
  (`PERFD_E_REFUSED`) and `perfd_mget_ex` gives each key its own
  status: found, absent or unanswered.
- An mget that would wait on more than 1,000 keys from other nodes is
  refused (`mget needs more than 1000 remote lookups - split it`), and
  so is one that would leave less than a quarter of `max_pending` free
  for everything else (`-TRYAGAIN`, retry or split it).
- On RESP, a command that waits on another node - an `MGET`, or a
  `GET`, `SET`, `DEL` and the like of a key this node does not hold -
  holds what the client pipelined behind it until its own reply is out,
  so replies stay in order.  Before 0.7.2 a later reply could overtake
  one waiting on a pull or a forward.  A pipeline of such commands on
  one connection therefore goes one at a time; a client that routes,
  or uses several connections, keeps them concurrent.
- `stats.cluster` counts `mget_fanout` (mgets that asked other nodes),
  `mget_unanswered` (of those, the ones with a key not answered) and
  `mget_refused`; `/metrics` has the same three as
  `perfcached_cluster_mget_*_total`.
