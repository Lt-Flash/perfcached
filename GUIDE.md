# perfcached guide

The operator's reference: running a node and a fleet, the cluster modes,
durability, sizing, operating it, the tools, the clients, and the
measured numbers.  The [README](README.md) is the short tour; every
option is annotated in
[contrib/perfcached.conf.example](contrib/perfcached.conf.example).

## Running it

Every option lives, annotated, in
[contrib/perfcached.conf.example](contrib/perfcached.conf.example);
the snippets below are complete working configs, and
[contrib/CONFIGURATIONS.md](contrib/CONFIGURATIONS.md) has ten whole
files - single nodes and fleets in every mode - each validated with
`perfcached -C`.

### A single node

    # /etc/perfcached/perfcached.conf  (chmod 640)
    [daemon]
    workers = 4

**How many workers.** Set it to the core count. Measured on a 16-core
host: throughput scales close to linearly to 8 workers (6.2x a single
worker), then flattens as the daemon reaches ~14 of 16 cores.
Oversubscribing is harmless rather than helpful - 32 and 64 workers on
16 cores buy 2-6% over 16 and cost ~0.3 MB of RSS each, with no
scheduler thrash. No penalty for guessing high; a real one for low.

    [memory]
    arena_mb = 1024

    [secrets]
    client = pick-a-client-password
    cluster = pick-a-DIFFERENT-cluster-password

    [listen]
    tcp = 0.0.0.0:6479

    [collection sessions]
    buckets_log2 = 18

Validate, then run (`-D` = foreground; omit it to daemonize, or use
the systemd unit):

    perfcached -f /etc/perfcached/perfcached.conf -C   # validate + report
    perfcached -f /etc/perfcached/perfcached.conf -D
    perfcli -p 6479 -a 'pick-a-client-password' ping

For production, install the unit and start it the systemd way:

    cp contrib/perfcached.service /etc/systemd/system/
    systemctl daemon-reload && systemctl enable --now perfcached

The two secrets MUST differ - clients hold the client secret, only
daemons hold the cluster secret, and the daemon refuses to start when
they are equal.  Add a second `client = ...` line to rotate client
passwords with zero downtime (new connections try each in turn).

### A cluster

Membership is automatic: there are no node ids and no peer lists to
configure.  The nodes find each other in one of two ways:

- **Multicast** (the default), for one L2 segment: membership,
  heartbeats, master claims and the placement map go to the multicast
  group for the life of the cluster, sent with a TTL of 1, so they never
  cross a router.  `[cluster] multicast_ttl = N` (1-255) raises that TTL
  so multicast routing (PIM, or static routes) can carry the cluster
  plane across segments - verified with one router in a lab rig, not
  measured over WAN links; PRODUCTION.md lists what the routers must
  pass.
- **Unicast** (`discovery = unicast`), where there is no multicast -
  AWS, GCP, Azure, Kubernetes, or a LAN that filters it: the nodes find
  each other through a few seed names, described below.

Replication, forwards and pulls are unicast in both.  Add the same
`[cluster]` section - same group (or seeds), same cluster secret - on
every node and start them in any order; they discover each other,
elect a master, and the master assigns node ids at join time.  The
SAME config file works on every node (set `advertise` only on
multi-homed hosts, to pin which address peers should use).

**Without multicast: `discovery = unicast`.**  Where the network does
not carry multicast, a fleet finds itself through seeds instead:

    [cluster]
    discovery = unicast
    port = 6480
    seeds = cache-a.internal, cache-b.internal

The seeds are names (or addresses) of a few members, re-resolved every
few seconds and on every join attempt, so a seed may be down and a name
may move to a new address.  The membership traffic - beats, joins, the
map, goodbyes - goes by unicast to every seed and to every member this
node has heard of; a member that hears a newcomer tells its master, so a
node that knows only one member still finds the fleet.  Everything else
was unicast already.  The cost is one beat per node per peer per second,
which is the reason the mode suits a fleet of a few dozen nodes.
`/stats cluster.discovery` shows the seeds, how many resolve, and the
addresses known.

**In a cloud or Kubernetes.**  Unicast mode is what AWS, GCP, Azure
and Kubernetes networks need, and it carries more protections for
them:

- **Nothing fragments.**  A cloud path MTU is about 1,460 bytes, and a
  fragmented datagram is lost with any one of its fragments - some
  networks drop fragments outright.  Every frame over `max_datagram`
  (1,400) goes over a TCP connection to the peer instead (`lane_port`,
  the cluster port + 2).  The heartbeats stay small UDP datagrams, so a
  dead node is still noticed in seconds.
- **UDP blocked is not a dead node.**  Before a peer that went silent
  on UDP is dropped, the node tries TCP; if the peer answers, UDP is
  being filtered on that path - it says so with an ERROR and sends the
  peer everything over TCP, instead of evicting it.  Every
  `lane_retry_s` (30 s) it asks the peer, by UDP, whether UDP gets
  through again; an answer by UDP proves both directions and puts
  that peer back on UDP (a NOTICE says so).  A peer that leaves, or
  says goodbye, takes its forced lane with it - a master stopping for
  a reboot is not a blocked path.
- **One cluster from a cold start.**  Set `expect` to the fleet's size:
  a node with no cluster id then founds only when it sees that many
  blank nodes, and the highest address founds while the rest join it.
  Nodes that know only one seed learn each other through it.  Without
  `expect`, a node whose seeds are all down founds a cluster of its own
  after 5 s of silence - and once that is 30 s old it never merges.
- **A slow node does not blame the healthy ones** (both modes).  A node
  whose own loop was stalled - a paused VM, a starved container - holds
  its purges and elections for as long as it was stalled, while the
  beats it missed arrive, instead of declaring every peer dead
  (`/stats cluster.self_late`).  And a member that loses its master asks
  the other members first: if one still hears the master, only this
  node's path to it is broken, and it waits up to 30 s instead of
  starting an election that would split the fleet
  (`cluster.election_holds`).

A second cluster that answers on the same seeds (a staging fleet given
production's seed names, say) is reported - an ERROR, `/stats
cluster.foreign_clusters` and a `/metrics` gauge - and never merged.
For Kubernetes, seed with a headless Service's name and set
`publishNotReadyAddresses: true` on it (a pod is not ready until it
has found its peers, so it must be able to find them before that); set
`identity_source = env` with `PERFCACHED_PLATFORM_KEY` from the pod's
`metadata.uid` for a Deployment, or keep the identity file on a
StatefulSet's volume.  PRODUCTION.md has the checklist.

**A node's identity comes from the machine.**  With a state directory
and the default `identity_source = auto`, the identity is derived from
the platform - the VM's DMI uuid, else the first NIC's MAC - with a
salt the node mints once into its state directory (`node-salt`, beside
`node-identity`).  A disk cloned onto another VM therefore comes up as
a NEW member rather than a twin, two daemons on one machine are two
members, and a state directory moved to another path keeps its
identity.  Inside a container the DMI uuid is the host's, so `auto`
does not use it there.  The daemon runs as `perfcached` and
`/sys/class/dmi/id/product_uuid` is readable only by root: the shipped
unit copies it into the service's runtime directory first (an
`ExecStartPre=-+` step), and the daemon reads it from there.  A node
that already has an identity but no `node-salt` - one an earlier
release minted at random, or derived under a `site_salt` since removed
- keeps it: switching makes it a new member, which an upgrade must not
do by itself.  Its log says so, naming the source it could use: set
`identity_source = dmi` (or `mac`), or delete `node-identity` while the
node is stopped, to switch.  `site_salt`, when set, replaces the node's
salt.

**A cluster has an id, and two clusters never merge.**  The node that
founds a cluster mints a cluster id (a UUID v7), every member adopts
it and keeps it in its state directory (`cluster-id`), and every
cluster datagram carries it in its authenticated header.  A node
holding an established id ignores datagrams from any other cluster -
beats, joins, maps, data, all of it - and says so once a minute
(`/stats cluster.foreign_seen`, `cluster_id`, `cluster_id_state`).  So
two fleets that happen to share a multicast group, a secret and a
config stay two.  For the first 30 s an id is *provisional*: a cold
start can elect two founders before either hears the other, and a
provisional founder that meets another cluster yields to it (always to
an established one), so a cold start still ends as one fleet.
`[cluster] cluster_id = <name or UUID>` pins the id from config (a name
is hashed into a v8 UUID) - staging and production can then never meet
even on one group.  A second established cluster heard on the same
group or seeds is reported - an ERROR the first time, `/stats
cluster.foreign_clusters` (its id, where from, when) and the
`perfcached_cluster_foreign_clusters` gauge - because it means a
misconfiguration, and it is never merged.  To move a node to another
cluster, stop it and delete `cluster-id` from its state directory.

**The cluster owns the collection config.** Declaring `collections`
makes this description authoritative: ONE mode for the whole cluster,
over an exhaustive set of collections.  A peer whose config differs is
refused at join, loudly and by name, rather than silently exchanging
data it will misinterpret - two nodes running one collection as store
and another as shard lost every write between them, in silence, before
this existed.

    [cluster]
    multicast = 239.68.68.1:6480
    mode = eager                 # ONE mode, cluster-wide
    collections = sessions       # the exhaustive clustered set
    #advertise = 10.0.0.1        # only on multi-homed hosts

    [collection sessions]
    buckets_log2 = 18            # node-local SIZING only

**Ports between nodes.**  The cluster port (the multicast group's
port, 6480 in the examples) is needed on both UDP and TCP.  UDP carries
membership, replication and pulls, to the group and unicast.  TCP, on
the same port number, is the bulk channel: records too large for a
datagram and the bootstrap of a node that joins empty; without it a
node warns that oversized migrations will drop.  The pub/sub relay
port, by default the cluster port + 1, is optional: it carries relayed
publishes on threads of its own so a publish storm cannot starve the
heartbeat socket.  A node that cannot reach a peer's relay port sends
it relays on the cluster port instead, and after 10 s logs a warning
naming the peer; `pubsub_rx_threads = 0` gives up the relay port
altogether.  In `discovery = unicast` one more TCP port is used, the
frame lane (`lane_port`, by default the cluster port + 2): frames
larger than `max_datagram` (1,400 bytes) travel on it instead of as
IP fragments.  So a unicast fleet needs the cluster port on UDP and
TCP, the lane port on TCP, and optionally the relay port on UDP - all
three addressed to the nodes themselves, never through a load balancer
or a Kubernetes Service's virtual IP.

Nodes that die and come back rejoin by themselves; a partitioned
master steps down when it sees a bigger fleet.  **Deletes survive a
partition.**  Each node remembers the deletes it makes and receives for
`tombstone_retain_s` (`[cluster]`, default 900 s), up to
`tombstone_retain_max` (default 250,000, about 25 MB, at most
1,000,000).  This memory is outside `arena_mb`, and `/stats` and
`/metrics` report it.  While it remembers a delete, a node refuses an
older copy of that key from any peer: pushed, swept, pulled on a read,
or sent in bulk.  When a peer comes back from a silence, the deletes it
may have missed are replayed to it.  A write made AFTER a delete, on
either side, is newer and wins.  Three cases can still bring a delete
back.  Re-issue deletes that matter after a long split:
- **The partition outlasts `tombstone_retain_s`.**  The node that sees
  the peer come back logs a warning saying so.
- **The cap drops entries before their time.**  At 1,000 deletes/s,
  250,000 entries cover about 4 minutes, not 15.  The
  `perfcached_cluster_tombstones_dropped_early_total` counter, and the
  same warning, say when this happened.
- **A node restarts during the partition.**  The memory is not kept on
  disk.  The node's own data comes back through the restart reconcile,
  but it can no longer send the other side the deletes it made.

Failure detection
runs on 1 Hz heartbeats with real margin - a master is presumed dead
after 8 s of silence, a peer after 10, so jitter is not death - beat
emission is watchdog-backed, and datagram ingest is fairness-bounded
so a migration burst cannot deafen membership.  Watch it settle:

    perfcli -p 6479 -a '...' -P stats
    # "cluster": { "node": 1, "role": "master", "peers_up": 2, ... }

**One cluster is one mode.** Mixing modes inside a single cluster is
rejected by design, not deferred: one membership whose members mean
different things per collection cannot be reasoned about during an
incident - the same node loss is "replicated, fine" for store and
"re-shard" for shard at once - and the rebalancer would be mixing
placement arithmetic across modes.  A deployment that genuinely needs
two modes runs TWO clusters on distinct multicast groups (or, in
unicast mode, distinct ports and seeds); a daemon can join several.  Without `collections` the legacy per-collection `mode =`
still parses and warns, because nothing then checks that your peers
agree; new deployments should declare the cluster form.

**Applying replicas on more than one thread.** By default the thread
that receives a replicated record also applies it.  `[cluster]
apply_threads = N` (1..16, default 1) keeps the receiver on the socket
and the decrypt and hands each record to one of N apply threads chosen
by a hash of its key, over that thread's own ring: every record of a key
is applied by the same thread, in the order it arrived, and a full ring
makes the receiver wait rather than drop.  The receiver still decrypts
everything, and each thread costs CPU per record - measured on an idle
16-core receiver at ~1.3M records/s offered: 0.95 us at one thread,
1.75 at two, 2.19 at four - so raise this only when the receiving
thread is demonstrably the bottleneck.  A hot key pins one thread.  `stats.cluster` shows `apply_threads`,
`apply_dispatched`, `apply_blocks` (receiver waits on a full ring) and
`apply_backlog` (bytes queued).

**What the repair walks cost.** The eager repair sweep walks the whole
table every cycle (sending only what the watermark says a peer is
owed), and a node that replays a WAL after a restart probes every
replayed key against the fleet at 64 a second, to drop what was deleted
while it was down.  Both are O(N) in the keyspace; `stats.cluster`
shows what the last completed pass of each cost - `repl_sweep_ms` with
`repl_sweep_scanned` and `repl_sweep_sent`, and `reconcile_ms` with
`reconcile_pending` - so the point where they need narrowing is read,
not guessed.

**What a node says about itself.** `stats` carries `state` and
`state_reason`, and `members` carries both for every node as the others
hear them: `starting`, `recovering`, `ready`, `draining`, `failed` (its
WAL lost or could not store acknowledged writes: reads served, writes
refused, until restarted) and `stalled`.  **Stalled** means the node's
cluster thread - the one that applies replicated records, answers pulls
and runs the control plane - has made no progress for
`[cluster] apply_stall_ms` (default 10 s, 0 = off).  A separate thread
keeps the heartbeat going, so without this a wedged node went on looking
healthy for ever; with it that heartbeat says `stalled`, peers and
routing clients steer new work elsewhere, and the node returns to `ready`
by itself once the thread has moved steadily for the same interval.  A
stalled node stays a member, so nothing re-shards over it, and it refuses
nothing.  One apply thread of several (`apply_threads`, below) wedged
with records queued on it is reported the same way.  A stalled *master* is replaced: its claim is not repeated, the
others do not consider it in the election, and it steps down when it
returns and hears the higher term.  Only `ready` (and `draining`, which still answers for what it
holds) is selected by libperfd; a client or peer built before a state
existed reads it as "not ready".

### The modes

The five modes differ in one thing: how many nodes hold a record, and
which.  Every mode may accept a write on any node.

**`store`** pulls on a local miss and KEEPS the copy, so every node
converges on the working set.  Each node's ceiling is its own arena.

**`eager`** is store plus a push: every write goes to every live peer
as it lands - on the write path, fire-and-forget, whatever the TTL - and
a background sweep repairs what a push did not reach.  Replicas are held
off the WAL: a record is durable where it was written, and a node that
restarts holds its own writes until the sweep refills the rest.

**One copy, in every mode.**  The replica count is availability, never
durability: only the node that took a write logs it, so the fleet's
data is the UNION of the WALs and no single node's is complete.  After a
whole-fleet outage - a power cut, not a rolling restart - each node
replays its own share and the fleet re-shares the rest over the repair
sweep.  A returning node still asks the fleet whether each replayed key
survives, so that a key deleted, or expired, while it was away is not
resurrected - but only if some peer was up throughout its absence.
Until the fleet has answered for a key, the returning node neither
sweeps its recovered copy to its peers nor answers their pulls with it
(`repl_held_recovered`, `pull_held_recovered`).  The question is asked
at 64 keys a second, so a large node takes a while to finish (`reconcile_pending`).  After an outage
that took the whole fleet down nobody was, the question is skipped, and
nothing is deleted (`stats.cluster.reconcile_skipped` says so on every
node): a key that expired meanwhile can then come back from a replayed
copy that was older and longer-lived.  A snapshot is what makes a single node independently
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

Applying those copies is ONE thread per node while the sending side is
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
`bench/spreadbench.sh`; the table is under Measured:

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

**Which mode to run, and the one thing that does not vary.**

*Durability is one copy in every mode.*  Whatever the replication
factor, exactly ONE node writes a given record down: the node that took
the write, into its WAL.  Every other holder - an eager replica, a
spread holder, a pulled copy in `store` - keeps it in memory only.  So
**a replication factor is an availability feature, never a durability
one**: it decides what survives a node dying, not what survives the
fleet dying.  What survives the fleet dying is the WAL and the
snapshots, in every mode, which is why `[wal] save` matters more than
the mode does (see **Durability**, below, and the warning the daemon
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
  257,301 against 2,659,574 SETs a second, 50 clients, pipeline 64).
  Eager holds every key on every node, so whichever node the balancer
  picks answers from its own memory.  Choose `shard` behind a balancer
  only when the keyspace does not fit one node.
- **`store` is a cache, not a redundancy story.**  It converges only on
  keys read on more than one node; a key written on A and never read
  elsewhere lives in exactly one memory, so it gives eager's memory cost
  for the shared read set without eager's guarantee for anything else.
- **`proxy` for the largest keyspace per byte**, when a hop on every
  miss is acceptable.

### Durability

In-memory only by default.  Add a `[wal]` section for write-ahead
logging + RDB snapshots; recovery replays snapshot then WAL tail at
startup:

    [wal]
    dir = /var/lib/perfcached
    fsync = everysec             # always | everysec | no
    save = 900 1                 # RDB snapshot rules, Redis-style,
    save = 300 10000             # repeatable and OR-ed

**Choosing `fsync`.** The pump fsyncs once per drained *batch*, so with
`fsync = always` the per-writer ring has to absorb everything that
arrives while it sits in `fdatasync`.  On storage whose fdatasync takes
milliseconds the shipped 1 MB ring is not enough, and an overflowing
ring used to drop acknowledged writes *silently* — measured at up to
13% of a 20,000-key fill, present in the live table and absent after a
restart.  The probe now derives the depth from the measured p99 and the
daemon applies it (set `ring_kb` yourself to override; with `probe = no`
nothing derives it, so set it).  A drop that still happens is logged,
and the node **heals**: the dropped record is missing from
the log but not from memory - every write reaches the table before the
WAL - so the node goes **HEALING**, refuses writes (no client selects it,
it stays in the map so nothing re-shards), keeps answering reads, takes
an unthrottled snapshot that makes every acknowledged write durable
again, and returns to READY by itself, usually in well under a second.
A segment overrun heals the same way.  Measured before this: 6,192
acknowledged writes of a burst missing after a restart; after it, none.
It goes **FAILED** instead - out of service until restarted - only when
it cannot heal: the device refused a write (fix the storage), or the
heal's own snapshot failed.  Frequent heals mean the rings, the storage
or the offered load need resizing (`perfcached_wal_heals_total`).
If the last snapshot took longer than the whole ring takes to fill at
the current rate (`perfcached_wal_ring_fill_seconds` below
`perfcached_rdb_last_duration_seconds`), no trigger can keep a snapshot
ahead of the WAL and the node will heal over and over; it logs that as a
sizing warning with both figures.
`everysec` does not stall the ring on a sync, but it does not make the
ring bottomless either: the WAL thread still writes every record, so a
write rate faster than it - or than the storage behind it - fills the
rings and the node heals the same way.  Measured with 1 KB records: the
WAL thread alone tops out near 180k records/s, and on a disk whose write-back held ~35 MB/s it fell behind at
~35k records/s.  Size for your storage, and watch
`perfcached_wal_dropped_total`.

**What a replica does NOT give you.**  An eager replica, a spread
holder and a pulled `store` copy are all held in memory and off the
WAL - the node that took the write is the one that persists it.  That is
right while the fleet survives: a node that restarts is refilled by the
repair sweep from peers that stayed up.  It is NOT a second durable
copy, so if every node goes down together each one replays only what it
authored, and what makes the fleet whole again is the snapshot, which
does carry the copies.  `mode = eager` with `save = off` is therefore a
data-loss configuration and the daemon says so at startup.

**When the device says no.** A WAL append, a segment rotation or an
`fdatasync` that the device refuses (EIO, ENOSPC...) fails the node the
same way, at the FIRST refusal, and the sync is never retried: after a
failed `fsync` Linux drops the dirty pages and reports the error once,
so a retry that succeeds proves nothing.  The durable watermark
(`wal.synced_seq`, `perfcached_wal_synced_seq`) stays where the device
last agreed to it, `sync` answers an error instead of `synced`, the
node's `state_reason` says the device refused, and
`perfcached_wal_storage_failed` goes to 1.  This holds with and without
a `[cluster]`.  A snapshot whose file or directory entry could not be
made durable is not published: `rdb.safe_marker` stays behind
`rdb.last_marker`, the WAL keeps every segment the old marker protects,
`rdb.save_errors` (`perfcached_rdb_save_errors_total`) counts it, and
the next snapshot is tried again after a back-off.

**Snapshots keeping up with the WAL.**  A snapshot frees only the WAL
segments below the point it started at, so under sustained writes it has
to start while the free segments still outlast it.  The WAL asks for one
itself - at a quarter free, and earlier when the fill rate says the free
segments will run out in less than twice the last snapshot's duration -
**also with `save = off`**: with a `[wal] dir`, "off" turns off the
timed rules, not these requests, because without them a busy ring
overruns and the node FAILS.  Alert on the same comparison:

    perfcached_wal_full_in_seconds >= 0
      and perfcached_wal_full_in_seconds < 2 * perfcached_rdb_last_duration_seconds

(`full_in_seconds` is -1 until the ring has rotated once, hence the first
line; `last_duration_seconds` is 0 until the first snapshot.)  Firing
does not mean data is at risk yet: it is the moment the WAL asks for the
next snapshot.  A rule that HOLDS (say `for: 5m`) means snapshots are
running back to back with no margin - measured in `snaptriggertest`, it
held for more than half of a load the node still survived, and the next
slower snapshot is the one that overruns.  Give the WAL more segments,
raise `rdb_mb_s`, or offer less load.  `perfcached_rdb_last_success_age_seconds`
(-1 before the first) and `perfcached_rdb_saves_total` say when snapshots
have stopped; `perfcached_rdb_in_progress` is 1 while one is written.

`perfcached -P /var/lib/perfcached` probes the storage first (fsync
latency, sustained rate) and prints the policy it would recommend;
`-I` prints the storage identity chain (NVMe/SAS/network/LVM...),
`-W`/`-R` inspect WAL segments and snapshots offline.  The `sync` and
`load` admin verbs give you an fsync barrier and additive snapshot
import at runtime.

### Read-through: taking over from Redis

A collection can name an UPSTREAM - a Redis server, Valkey, or another
perfcached - that the node reads on a local miss and never writes.  It
is how a fleet takes a keyspace over from Redis without a big-bang copy:
the clients move to perfcached, perfcached answers from its own
collection, and a key it does not hold yet is read from the upstream the
first time anyone asks for it.

```
[secrets]
upstream = <the password perfcached sends to the upstream>

[collection 0]
upstream = redis://reader@10.0.0.12:6379/0
upstream_prefixes = tok:*, cid:*
```

`upstream_prefixes` is required: only keys matching one of its patterns
(Redis glob syntax, comma-separated, up to 32) ever fall through - say
`*` to mean all of them.  `upstream = redis://[user@]host:port[/db]`;
the password lives in `[secrets] upstream`, never in the URL.

**One way only.**  perfcached sends the upstream nothing but the
handshake (`HELLO`, `AUTH`, `SELECT`, `INFO server`, `CLIENT ID`,
`CLIENT TRACKING`)
and reads (`TYPE`, `PTTL`, `GET`, `HGETALL`, `JSON.GET`, and `SCAN` for
the drain sweep).  Nothing it
does changes the upstream's data, so anything perfcached handles
differently from Redis stays on perfcached's side.  Point it at a
replica (a Redis replica refuses writes) and give it an ACL user that
can only read:

```
ACL SETUSER reader on >password ~* +@read +hello +auth +select +ping +info +client|id +client|tracking
```

**What a command does with a key it does not hold:**

- A read (`GET`, `MGET`, `EXISTS`, `TTL`, `TYPE`, the `H*` reads,
  `JSON.GET`, and the native door's `get`, `exists`, `ttl`, `jget`,
  `mget`, `hcmd` reads) waits for one fetch and answers from the
  copy.  The copy keeps the upstream's TTL, rounded up to whole
  seconds.  Concurrent misses for one key cause one fetch.
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
- `KEYS`, `SCAN`, `DBSIZE` and `RANDOMKEY` see only what the collection
  holds.  `RL.HIT` and the lock scripts never fall through.

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
visible in `stats`; do not write to it.

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
`upstream_negative_ms` (2000) and `upstream_drain_rate` (5000 keys a
second for the drain sweep below).

**In a cluster**, every node needs the same upstream configuration.
Each node fetches its own copies; what changes a key is a real write, and
it replicates.  A read on another node in the instant before a change
reaches it may answer the upstream's older value - the same window in
which an eager fleet without an upstream would answer a miss.  `proxy`,
`shard` and `spread` collections refuse an upstream, because another node
decides their keys.

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
cap) and reports in `stats` as `upstream.drain`: its state, and the keys
read, taken over, already the fleet's, gone, of a type it cannot hold,
too big, and failed.  `updrain <collection> stop` stops it.

When it is done and `fetches` has stopped rising, nothing more comes from
the upstream.  Remove `upstream` and `upstream_prefixes` on every node and
restart them.  The companion collection is not created again, and its
records are dropped at recovery as orphans.

### Other settings worth knowing

- `max_clients` under `[daemon]` caps client connections across the
  data doors.  Unset, it is derived from the descriptor limit minus
  what the process needs open; at the limit a connection is accepted,
  told why (`-ERR max number of clients reached` on the RESP door, an
  error frame on the native door) and closed, and
  `/stats` still answers.  The refusals are counted.
- A fleet stanza runs unchanged on a single node: without a
  `[cluster]` section, a collection's `mode` and `pull` are accepted,
  ignored and named once at warning level, and the cluster secret is
  optional.  With a `[cluster]` section, a `replicas` the live members
  cannot honour is logged once and shown as `replicas_short` on
  `/stats`, `/metrics` and the status page.
- `plaintext = loopback` under `[listen]` allows plaintext binary
  frames and RESP on 127.0.0.1/unix only - handy for debugging (netcat
  or redis-cli can drive the RESP side; the native protocol needs
  perfcli or libperfd); the LAN stays on the Noise channel.  The
  default (`never`) encrypts everything.
- **`resp = <addr:port>`** adds a dedicated listener for Redis clients
  that must reach the cluster over a network.  It is RESP2 ONLY - the
  native protocol (and with it the admin verbs) is refused on it -
  and because a Redis client cannot speak the Noise channel it is
  plaintext, so it is guarded instead: `resp_allow = <cidr>[,...]` is
  REQUIRED off-box (the daemon refuses to start without it),
  `[secrets] resp` adds a Redis `AUTH` password, and
  `resp_collections` bounds which collections it can see and maps the
  Redis database index onto them: an entry `0:sessions` makes `SELECT 0`
  reach the collection `sessions`, and a bare name is reachable by that
  name.  Without a mapping a collection has to be NAMED `0` to be
  reachable at all.  `stats` reports a `resp` block (connections,
  allow-list rejections, auth failures).  The door also serves the
  Redis observability surface - section-faithful `INFO` (commandstats
  and latencystats included), `CLIENT LIST`/`SETNAME`, `SLOWLOG`, and
  the `CLUSTER` family (`SLOTS`/`SHARDS`/`KEYSLOT`/`NODES`) - so
  Grafana's redis-datasource and cluster-aware Redis clients work
  against it unmodified (a key on the wrong node is forwarded by
  default; `resp_redirect = moved` answers it with a genuine `-MOVED`
  instead, which is the only thing that makes a cluster-aware client
  refresh its slot table); `TIME`, `EXPIREAT`/`PEXPIREAT` and `MEMORY
  USAGE` round out the tooling set.  The command rows count every
  door: RESP commands under their Redis names, the native door as
  `cmd:<method>` (a method) and `bin:<verb>` (a fixed-layout data
  verb), each row with a log2 latency
  histogram behind it - and the same rows are `commands` and `slowlog`
  on `/stats`, `perfcached_command_calls_total{cmd}` /
  `perfcached_command_usec_total{cmd}` and the
  `perfcached_command_latency_seconds{cmd}` histogram on `/metrics`.
- `arena_cap_mb` lets the arena grow elastically under pressure - by
  2 MB group inside one address-space reservation, on the same tier as
  the initial commit; `reclaim_*` returns idle groups to the kernel.
- A reserved hugepage pool makes the arena's top tier deterministic -
  see [contrib/sysctl-perfcached.conf](contrib/sysctl-perfcached.conf),
  and read back `HugePages_Total` after applying: live hosts routinely
  under-deliver the reservation until memory is compacted.
- `log_events = miss, expired, store, remove` (per collection, any
  subset) logs one line at NOTICE per event with the key, the door and
  the peer, and for a miss whether the key was absent or expired.  It
  is a per-collection mask, not a level: `log_level` is a ceiling,
  `[daemon] log_events` is the default, `log_events_rate` (100/s)
  bounds it and the daemon reports what it suppressed,
  `log_events_hash = yes` logs a hash instead of the key.
- `perfcached -E -f <conf>` dumps the normalized effective config with
  secrets masked.

### Sizing

What a set of collections costs on a host, per node - in eager mode every
node holds everything, so the figure is per node, not per fleet.  The
page's "memory budget" card computes the same arithmetic from the
daemon's exact figures; this is how to do it before the daemon exists.

1. **Index.**  Each collection's table is carved at creation.
   `buckets_log2` is 4..24 - the same range in a config file and at the
   `create` and `resize` verbs - but the cost is FLAT below 2^12 and 68
   bytes a bucket above it, because segments are fixed at 4,096 buckets
   and a table always carves whole ones:

   | buckets_log2 | buckets | index | per bucket |
   |---|---|---|---|
   | 4 .. 12 | 16 .. 4,096 | 1.25 MB | - |
   | 16 | 65,536 | 5.0 MB | 80 B |
   | 18 | 262,144 | 17.8 MB | 71 B |
   | 20 | 1,048,576 | 68.8 MB | 69 B |
   | 24 | 16,777,216 | 1.1 GB | 68 B |

   So anything below 12 buys nothing over 12, and the top of the range
   is measured in gigabytes - all of it carved before a single record.
   A table grows by splitting once it passes `grow_at_pct` of its slot
   capacity (`[daemon]`, default 75% - six slots a bucket, so 4.5
   entries), one bucket at a time, and growth adds index.  It shrinks
   too: once it has sat below `shrink_at_pct` (default a quarter of
   `grow_at_pct`) for `shrink_cooloff_s` (default 60 s) since it last
   split, was resized or started, it is resized down to the power of
   two midway between the two thresholds - one copy, never below one
   segment - and the old index goes back to the arena once every reader
   has moved on.  A table may also be widened PAST `grow_at_pct` by its
   own overflow leg: a key whose bucket is full lives in a shared leg
   and is found by walking a chain under one lock, the maintenance tick
   puts back every leg record whose bucket has since made room, and what
   it cannot put back is counted (`leg_stuck`).  Once that exceeds
   `leg_stuck_pct` of the entries (default 1%) the splitter aims at
   `grow_floor_pct` (default 50%) instead, which is the only thing that
   can help - those buckets are full.  Set `grow_floor_pct = 0` to turn
   it off; a table whose leg is quiet never notices it.  All of that
   is the `auto` **autoscale** mode, the default.  `autoscale = floor`
   is `auto` with a floor: it grows the same way, but never shrinks the
   table below the size the operator gave it - `buckets_log2` in its
   `[collection]` section, the size of a `create`, or an explicit
   `resize`, which becomes the new floor (an automatic resize never
   moves it).  A collection sized for the keys it expects keeps that
   size while it is still empty, and after a burst comes back down to
   it, not below; `/stats` carries the floor as `autoscale_floor_log2`.
   `autoscale = warn`
   (`[collection x]`, or `[daemon]` for the default) changes nothing and
   says what auto would do, in the log - at once, then at most every ten
   minutes - on the status page (an amber `warn -> 2^N` beside the
   collection) and in `/metrics`
   (`perfcached_collection_autoscale_target_buckets`); `autoscale = off`
   changes nothing and says nothing.  Under both, growth is blocked too:
   keys past the slots go to the overflow leg, and the page shows that
   collection red, because every lookup that reaches the leg takes one
   lock for the whole table.  `perfcli -E ... autoscale <col>
   <auto|floor|warn|off>` sets it at runtime; it is persisted and outranks the
   config from then on.  Per node, like the sizing itself.  A create or a resize whose index would not fit under
   the ceiling in item 4 is refused, naming both figures.
2. **Records.**  A record is its key, its value and a 28-byte header,
   rounded UP to a cell class.  The classes step by 1.5 and 1.33 from
   64 bytes to 64 KB (64, 96, 128, 192, 256, 384, 512, 768, 1 K, 1.5 K,
   2 K, 3 K, 4 K, 6 K, 8 K, 12 K, 16 K, 24 K, 32 K, 48 K, 64 K) and then
   to 96 K and 255.9 K, so the rounding is anywhere from 0 to 50 % - and
   up to a whole 256 KB chunk for the last two, which are cut one and
   two cells to a chunk.  **The largest record is 262,080 bytes**, key
   and header included: one cell per chunk, which is the most a 256 KB
   chunk can hold.  A larger value is refused, not truncated.  A measured mix - 100,000 records
   of 96 B, 768 B, 2 KB and 4 KB values, 62.9 MB of values - occupies
   94 MB.  Budget 1.5 x the key+value bytes for a mixed set, or bin by
   bin from your own size histogram.
3. **Residue after a burst.**  About 10 MB of warm free slots kept
   resident for the next growth, and up to 10.5 MB of chunks the size
   classes own (two 256 KB chunks per class).
4. **The ceiling.**  1 + 2 + 3 at the PEAK must fit under `arena_mb` (or
   `arena_cap_mb`): a full arena refuses writes and evicts nothing.  An
   overwrite whose value fits the cell the key already holds still
   lands (`nomem_inplace`, inside `nomem`).  In a fleet, a node that
   cannot store a peer's newer copy of a key drops the older copy it
   holds instead of serving it (`nomem_stale_dropped`), and a read of
   that key pulls from a peer.  A full node answers correctly, but it is
   not a whole replica: size every eager node for the whole data set.
5. **What the host sees.**  The arena reserves `arena_cap_mb` (or
   `arena_mb` when no cap is set) of address space, charged for
   nothing, and commits `arena_mb` of it at start - populated, and
   pinned when the daemon may pin - so resident memory is `arena_mb`
   plus about 15 MB for the daemon from the first second.  Growth
   commits 2 MB groups inside the reservation as records arrive, all on
   the same tier; once idle the give-back returns the never-carved part
   of the commit, and after a burst it returns the drained groups, so
   resident follows `arena_committed` (in `stats` and on `/metrics`) to
   within a few 2 MB groups.  The arena's pages are counted in RSS when
   the tier is transparent huge pages (the default when no hugetlb pool
   exists); on a hugetlb pool they are in `HugetlbPages` instead and RSS
   shows only the daemon itself.

Read it back on the page: `held` and its parts (structure, class
chunks, warm free), the budget card, and `resident` from the kernel.

### Example configurations

[contrib/CONFIGURATIONS.md](contrib/CONFIGURATIONS.md) - ten complete
files, each validated with `perfcached -C`:

- **single node**: minimal; for Redis clients (the RESP door with its
  guards, `SELECT n` onto named collections, the status page, a local
  socket); durable (identity, WAL, snapshots, runtime DDL behind the
  `enable` secret, a sampled query log); elastic memory (a capped arena
  that gives back, an index that grows and shrinks).
- **fleets**: eager; store (pull-on-miss); proxy (the capacity plane);
  shard behind cluster-aware Redis clients; spread with `replicas`.
- **two fleets on one host**: a second daemon in a second cluster.

## Operating it

### Watching it

`http = <addr:port>` under `[listen]` serves the status page - one page
per node, the fleet's view - and beside it `/stats` (what the `stats`
verb answers, as JSON), `/metrics` for Prometheus, `/members`, and
`/clients` (this node's connections: the table behind the page's clients
strip).  The page's headline strip reads `clients 123 / 1,000
fleet-wide` - this node over the fleet, the denominator summed from the
map every member gossips its count into, absent on a standalone daemon
- and clicking it opens this node's connections (door, dialect, wire,
address, name, age, idle, commands, last command, pending output bytes,
subscriptions) with a filter on address and name; the cluster plane
carries the commands cards: by calls, slowest per call, and the slow
log's tail.  The page stays read-only, and no door closes another client's
connection.  The collection rows answer for the fleet: every
figure shows the fleet's total first and this node's share beside it
("63 · 39 here"), summed from the per-collection block each member
gossips (its own client hits and misses, so a pull served for a peer
counts once), joined by the name's hash; entries summed that way count
copies, and the cell says so.
`http_allow` bounds who may reach it and `[secrets] http` adds a token.
`perfcli stats` gives the same figures over the native door.

### Collections at runtime

A collection can be created and dropped while the daemon serves, so a
new keyspace does not need a config edit on every node and a fleet
restart:

    perfcli create sessions            # 2^12 buckets by default
    perfcli create sessions 16         # or say the size
    perfcli drop sessions              # refused while it holds records
    perfcli drop sessions force        # dropped with them

A collection can also be resized in either direction and renamed, both
while it serves:

    perfcli resize sessions 16         # either direction
    perfcli rename sessions_restored sessions

A resize fills a second table at the new size from the live one, swaps
the two in one step, and collects what the swap window left behind; the
call returns as soon as the migration has started, and `stats` carries
`resizing_to` and `resize_moved` while it runs.  A key deleted during the
migration stays deleted, a key written during it keeps its new value, and
a target the splitter would immediately grow back is refused.  The old
table's index goes back to the arena once every reader has moved on
(`stats.retire` shows it pending, then cleared).  A rename is one
pointer swap, which makes it the atomic cutover a restore wants: load a
dump into a second collection with `perfload --collection-map`, verify
it, rename it into place, drop the old one.

This is off by default.  Set `[daemon] allow_create = yes` on the nodes
where a client may do it: a driver that creates on a miss turns a typo
into a second, empty collection and an operator into someone whose cache
"lost everything".  The setting gates who may ORIGINATE a create, not
what a node accepts from the fleet, so a create made anywhere reaches
every member whatever their own setting, and turning it on does not need
a fleet restart.

**`allow_create` is not enough on its own, and deliberately so.** It
answers "may this NODE originate DDL". It cannot answer "may THIS
CONNECTION", because applications, ops tooling and `perfcli` all present
the same `[secrets] client` value - so gating on that alone would hand
`create`, `drop`, `resize`, `rename` and `restore` to every application
link. A second secret settles the second question:

    [secrets]
    enable = <a value that is NOT the client secret>

A connection raises privilege with `enable` and drops it with `disable`,
both on the **native door only** - never RESP, which may run plaintext
off-box and would put the highest-value secret in the fleet on the wire
in the clear. The privilege lives on that connection, dies with it, is
never replicated, and is never consulted when a peer applies DDL the
fleet already agreed. Failed attempts are counted as
`stats.door.enable_fails`.

**Fail-closed:** with no `enable` secret configured, `enable` always
fails and those verbs are unreachable from the client door **however
`allow_create` is set**. A node with `allow_create = yes` and no enable
secret therefore refuses all client DDL, and says so at startup. That is
the safe direction, but it will surprise you on an upgrade if the config
is not updated with it.

For the tools: `perfcli -E <secret>` (or `-e` to be prompted, or
`PERFCLI_ENABLE` - deliberately not `PERFCLI_AUTH`, so exporting the
application's secret does not confer privilege), and `perfload
--enable <secret>`, which `restore` needs.

A created collection takes the cluster's mode: a fleet is one mode over
one collection set.  It is remembered in `[daemon] state_dir` and comes
back after a restart, it rides the WAL as a record of its own so a
replay lands records in the collections that existed when they were
written, and it is announced to every live peer, with the whole set
re-announced every few seconds so a node that was down through the
change picks it up when it returns.  Without a `state_dir` a created
collection is ephemeral, and the daemon says so.

### Open connections, running totals, and resetting them

`stats` reports, per door and dialect, what is open right now beside
what has happened since the daemon started: `stats.resp.open` and
`stats.native.{binary,resp}.open` are gauges; `conns` and
`requests` beside them are running totals.  `stats.since` says what
the totals count from: `{"reset_at": <unix seconds, 0 = never>,
"next_reset": <unix seconds, 0 = no schedule>, "s": <seconds since the
reset, else since start>}`.  The page's door cards
lead with "open" and label the totals "since start" or "since reset";
`/metrics` exposes the gauges as `perfcached_connections_open{door,
dialect}`.

The running totals start again on a schedule - daily at local midnight
unless `[daemon]` says otherwise - so no count `stats` or the page shows
grows without end:

    [daemon]
    stats_reset = 1d          # off, or 1d .. 7d (7d: every Monday)
    stats_reset_at = 00:00    # local time; every node of a fleet in one
                              # timezone resets at the same moment

`PERFCACHED_STATS_RESET` in the daemon's environment (`off`, `1d` ..
`7d`) overrides `stats_reset`; `make check` sets it to `off`, so no suite
sees its counters start again under it.  They can also be started again
by hand:

    curl -X POST http://node:8080/reset-stats     # the page's button: the whole FLEET
    perfcli reset-stats                          # this node only (the native method reset_stats)
    redis-cli -p 6380 CONFIG RESETSTAT            # this node only (the RESP door)

The page's reset resets its node and asks every live peer to reset as of
the same moment, so the fleet figures the page shows all count from one
time; each peer logs whose reset it took.  A reset covers the doors, each
collection's table counters (hits, misses, stores, removes, expired - the
core's own re-baseline), the read-through figures (fetches, absent,
errors, ... - not a drain run's), the store's size tallies, and the
cluster, proxy and WAL counters.  It never touches a gauge: open
connections, entries, buckets, memory, sequence numbers, roles.
`/metrics` is never reset, by the schedule or by hand: it counts from the
daemon's start - Prometheus counters are meant to be monotonic, and a
drop would read as a counter reset.  `POST /reset-stats` is the one mutating HTTP
route: body-less by contract (a body is a 400), guarded by the
`[secrets] http` token like every other route, `POST` anywhere else a
404.

## Tools

### perfcli

The redis-cli analogue.  Word commands mirror the verb set; `-a` runs
the Noise handshake (client principal) for encrypted listeners:

    perfcli -p 6479 set sessions user:17 "some value" 300
    perfcli -a 's3cret' get sessions user:17
    printf 'ping\nstats\n' | perfcli -q          # pipe mode
    perfcli -j '{"method":"keys","params":{"col":"sessions","match":"user:*"}}'

`help` inside the REPL lists everything; jset takes raw JSON.  Every
command, its arguments and its reply shape:
[PERFCLI-COMMANDS.md](PERFCLI-COMMANDS.md).  The
REPL has its own line editor - arrows + history (persisted 0600 in
`~/.perfcli_history`, duplicates collapsed), emacs keys
(Ctrl-A/E/B/F/W/U/K/L), Ctrl-R reverse search, Ctrl-C discards the
line, Ctrl-D quits.

`pretty on` (or `-P`) re-indents every JSON result - together with the
JSON path verbs:

    $ perfcli -p 6479 jset sessions user:17 '$' \
        '{"name":"ann","roles":["admin","ops"],"quota":{"used":3,"max":10}}'
    {"set":true}
    $ perfcli -p 6479 -P jget sessions user:17 '$.quota'
    {
      "found": true,
      "value": {
        "used": 3,
        "max": 10
      }
    }

### perfdump

A parallel dumper over the client door, shaped like mydumper: one node's
collections walked by N connections, each owning a range of the table's
buckets, into a directory of chunk files with a manifest.

    perfdump --from 192.0.2.10:6479 -a <client secret> --out /var/backups/pc-$(date +%F)
    perfdump --inspect /var/backups/pc-2026-09-09

`--threads` (default 4) connections per collection, `--count` buckets per
call (default 1024, halved on the daemon's refusal), `--chunk-mb` (64)
before compression, `--zstd` level (3; 0 = raw), `--rate` records per
second per connection so a dump never starves production,
`--collections a,b` to pick.  Every chunk carries a CRC-32 and a trailer,
the manifest is rewritten atomically as each chunk completes, and
`--inspect` reads every file back and checks both.  The format is
[doc/perfdump-format.md](doc/perfdump-format.md); `perfload` loads it
back.  On an eager fleet any READY node holds everything, so dump from
one; a node still pulling its bootstrap is refused as a source.  A
table that grows under the dump can repeat a record: growth splits
buckets in place, and a split can move records ahead of the cursor
(at-least-once, as Redis SCAN is); the loader upserts by version, so a
repeat costs bytes.  A RESIZE is different - an operator's `resize`, or
the automatic shrink after mass deletes, swaps in a table of another
shape - and a walk carried across one used to repeat records or
silently LOSE them (measured: a shrink between two chunks lost 2,200 of
3,000).  Since 0.4.0 every chunk names the table it walked, the
daemon refuses a chunk once the collection has been swapped, and
perfdump dumps that collection again from scratch (it says so, and gives
up after five swaps in a row).  Needs the `zstd` binary for compressed
dumps.

### perfload

The loader, the reverse of perfdump: every record goes back with the
value, the VERSION and the absolute expiry the dump held, through the
daemon's write path (the WAL, the eager push), so a fleet of any size
or mode takes it as if a client had written it - only with the dump's
history.  Load a three-node eager fleet's dump into a two-node shard
fleet, or the other way round.

    perfload /var/backups/pc-2026-09-09 --to 192.0.2.10:6479 -a <client secret>
    perfload /var/backups/pc-2026-09-09 --to 192.0.2.10:6479 -a <client secret> --dry-run
    perfload /var/backups/pc-2026-09-09 --to 192.0.2.10:6479 -a <client secret> --collection-map sessions=sessions_restored

Every chunk's count and checksum is verified before anything is sent
(`--no-verify` skips it).  `--threads` (default 4) connections stream
`restore` batches of `--batch` records (256) at `--depth` calls in
flight (8); `--rate` caps records per second over all threads.  On a
shard fleet each record goes straight to its owner (the library
routes it); on an eager or plain fleet the chunks are spread across
the members and the push carries the rest.  A record the daemon does
not own comes back named and is re-sent to the node it names.

`--policy newer` (default) is the store's own rule: a record older
than, or as old as, the copy the fleet holds loses and is counted.
`--policy skip` leaves every existing key alone.  `--policy overwrite`
installs the loaded value under a fresh version, so every replica
takes it - a fleet rolled back to a dump.  A record whose expiry has
passed is skipped and counted, never re-based.  `--collections a,b`
picks, `--collection-map a=b` loads a's chunks into b; the target must
already have the collection.

A chunk is marked done in `DIR/perfload.done` once its last batch is
acknowledged, and the next run skips it (`--restart` reloads
everything); with the default policy a re-run over a finished load
stores nothing and counts everything older, so a loader killed halfway
is simply run again.  The final line has records, bytes, elapsed,
records per second and the counts (stored, older, existing, expired,
refused, bad, rerouted); on a fleet a second line says how long the
members took to settle after the load.  `--via-set` loads through
plain pipelined `set` instead - the comparator: it re-bases TTLs and
assigns fresh versions, the two things a loader must not do.
Measured on the build host: a million 224-byte records in 0.8 s on
eight threads, 1.4x faster than plain SET on the same connections.

## Clients

### libperfd

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="contrib/brand/perfd-mark-dark.svg">
  <img src="contrib/brand/perfd-mark-light.svg" width="72" align="right" alt="libperfd - the compass">
</picture>

The hiredis-analogue C client: typed verbs, binary-safe values, the
Noise channel with a secret LIST (rotation = add-new/drain-old), and a
pipeline that delivers replies in request order - all of it over the
native binary protocol (the data verbs as fixed-layout frames, every
other method with its parameters as a tree), behind one API:

    #include <perfd.h>
    const char *secrets[] = { "new-secret", "old-secret", NULL };
    perfd_opts o = { .secrets = secrets };   /* defaults for the rest */
    perfd_t *p = perfd_connect("10.0.0.1", 6479, &o);
    perfd_set(p, "sessions", "user:17", blob, blob_len, 300);
    if (perfd_get(p, "sessions", "user:17", &val, &vlen, &ttl) == 1) { ... }
    perfd_free(p);
    // link: cc app.c libperfd.a -lsodium -lpthread

**Cluster-aware.** Set `opts.spares` and the library learns the
fleet on connect, keeps standby connections open to the other nodes,
and swaps onto one when the node it is using dies - a failover costs a
`send()` on an established socket, not a TCP+Noise handshake.
`opts.policy` picks where a client works: `FAILOVER` (default),
`ROUND_ROBIN` (independent random start per client - a thousand
clients spread with no coordination), `LEAST_CONN` or `WEIGHTED` (by
the free arena each node reports).  Idempotent verbs are replayed
across a failover; `add`/`sub` are NOT - the caller is told, because a
double increment is worse than a visible error.  `perfd_member_count`,
`perfd_active_node`, `perfd_spare_count` and `perfd_failovers` let a
caller see what it is doing.

**Per-key routing.**  Add `opts.route_keys = 1` and each request goes
to the node that should hold its key, so the daemon's forward hop
disappears.  It applies to `shard` (the owner is computable), `spread`
(the holders are the top K of the same ranking; from 0.2.8) and `store`
(hashing a key to one node makes the client a de-facto single writer
for it, which is what stops concurrent writers forking a key); `proxy`
is not routed by it - a proxy key's holder is not computable.

**Holder hints for `proxy` (0.5.0).**  Add `opts.route_hints = 1` and
every data call asks the node that answers it to say which node held
the key ("currently at node X", with the record's remaining TTL).  The
handle remembers that per key, in a fixed 4,096-entry table that never
allocates, and sends the next call for the key straight to X over the
standby it already holds - so a proxy fleet read through any node pays
the extra hop once per key, not on every read.  A hint is never
load-bearing: if the rebalancer moved the key, X misses, asks the fleet
and answers with the value and the new holder - one extra hop, counted
by `perfd_hint_stale()`; `perfd_hint_hits()` counts the calls that
landed where the hint said.  An entry dies with its record's TTL, and
one naming a member the handle has no link to is forgotten.  Off by
default; a daemon older than 0.5.0 sends no hints and the handle routes
as it would without the option.  The daemon counts `holder_hints_sent`
and `holder_hint_local` (hint-asking requests answered from its own
table) in `/stats`.  It is never load-bearing: the daemon re-checks
ownership and forwards a wrong guess, so a stale view costs a hop, not
correctness - `perfd_route_missed()` counts those.  Every keyed verb
routes the same way, and that uniformity - not the routing - is what
orders two requests on one key from one client: `jdel` and `jincr` once did
not, and a `jdel` right behind a routed `jset` could reach a
peer before the owner's push and delete nothing.  A reply the daemon
had to forward says so (`"moved"`, with the stamp of the map it routed
by), and a routing handle that holds another stamp re-learns its
members behind that call - one request, where it used to wait for the
next push or `refresh_ms`; `perfd_moved_hints()` counts them (libperfd
0.2.12).  Off by default,
like the spreading policy.  On the async API the application routes:
one handle per node, picked with `perfd_owner_of()`, because the
library will not open connections an async caller would never poll.

### Redis pub/sub

Every door publishes and subscribes on one global channel space:
`SUBSCRIBE`, `PSUBSCRIBE`, `PUBLISH` and `PUBSUB` on the RESP door work
as on Redis, with Redis's subscribed-mode rules and message frames, and
a subscriber that cannot keep up is closed at the output cap the way
Redis closes it.  The
native door has the same five methods, with messages delivered as
NOTIFY frames so a native connection stays multiplexed, and libperfd
wraps them as
`perfd_publish`, `perfd_subscribe` and friends with delivery on its
notify hook.  On a fleet a publish on any node reaches the subscribers
on every node, over the same sealed unicast plane the replication
rides, at-most-once with gaps counted; `PUBLISH` answers the local
receiver count, as Redis Cluster does.  Behind the HAProxy
VIP that means a subscriber pinned to one node hears a publisher that
landed on another.  A node set to `pubsub_relay = interested` is
relayed only the publishes that may match a subscription it holds - its
channels travel to its peers as a Bloom filter, its patterns as
strings, a new subscription at once - so a large fleet stops sending
every publish to every node; the default, `all`, relays everything.
Messages waiting for a worker to write them are capped per worker
(`pubsub_queue_mb`, 16 MB by default).  A publishing connection that
fills a worker's queue to half the cap is paused - the rest of its
input waits, and TCP holds the client - until the queue is below a
quarter, so a publish the node answered is a publish it delivers, and
`publish_paused` on `/stats` counts the pauses.  Messages from peers and
keyspace events have no connection to pause: past the cap they are
dropped and counted (`queue_dropped`), never allowed to pin memory or
delay a shutdown.  Raising the cap lets publishers run longer before a
pause and costs memory and a slower exit under load.
A native connection may take its deliveries over UDP instead
(`pubsub_udp {"port":N}`): they go only to the connection's own
address, from the door's address and port, sealed with a key the
connection was given, and only after the client echoed a probe's cookie
over the connection; the client acknowledges every 256 messages or 5 s,
and one that stops - or whose acknowledgements stop moving - loses its
subscriptions and is told.  Deliveries over 1,400 bytes stay on TCP;
the protocol is in `lib/perfd_push.h`.  In libperfd (0.2.10) it is
`perfd_pubsub_udp(handle, port, timeout_ms)`, then
`perfd_pubsub_udp_ready()` on the socket's readability and at least
once a second: the messages reach the same notify hook with the same
JSON, on blocking and async handles alike.  A collection with `notify_events` publishes Redis
keyspace notifications for what this node applies, never relayed, so
`PSUBSCRIBE __keyevent@0__:*` works as it does against Redis.  The commands and the frames are in
[REDIS-COMMANDS.md](REDIS-COMMANDS.md); the figures are the `pubsub`
block on `/stats` and `/metrics`.

## Measured

> **0.4.0's numbers**, except the `spread` farm table, which is still
> 0.3.7's (re-taking it means a high packet rate across the lab's
> shared network; it waits for a quiet window).

Every table below is the tagged 0.4.0 daemon (`570067a`), measured
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
[bench/RESULTS.md](bench/RESULTS.md); raw rows are in
[bench/results/](bench/results/), every earlier run's included.
Numbers rot: quote from a run, not from here.

### One server against one server, over the wire

`redis-benchmark` - Redis's own tool, unmodified - on one host; one
perfcached node, no cluster, and Redis on another, both as containers
with host networking, a real NIC between them at MTU 9000 verified end
to end, arms alternated so drift cannot favour either.  0.4.0's daemon
(`570067a`), 2026-10-01, the 16-vCPU Debian 13 server host, a 16-vCPU
Ubuntu 24.04 client host (`redis-benchmark` 8.10.2); 4 workers, 20k
keys x 200 B, cells sized to run for over a second, median of 3
(`bench/xhostbench.sh`,
[bench/results/xhostbench-0.4.0.tsv](bench/results/xhostbench-0.4.0.tsv)).

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
[bench/RESULTS.md](bench/RESULTS.md).

### The cluster modes through a Redis client

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
sweep ([bench/results/containerbench-0.4.0.tsv](bench/results/containerbench-0.4.0.tsv));
eager is a re-run on the same host settings
([containerbench-0.4.0-eager.tsv](bench/results/containerbench-0.4.0-eager.tsv));
proxy is a re-run taken after the host's `invtsc` CPU flag was removed
([containerbench-0.4.0-proxy.tsv](bench/results/containerbench-0.4.0-proxy.tsv)) -
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

### The same fleets through clients that route

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
decomposed in [bench/RESULTS.md](bench/RESULTS.md).

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
defect (S156, fixed in 0.4.0), not a speed.  The two modes that have to
go and get the key differ in how: shard unicasts to the one owner,
proxy consults a locator and broadcasts on a miss.

### `spread` at capacity, from an off-box farm

> **Still 0.3.7's numbers** - not re-taken for 0.4.0: the farm drives
> the server with a high packet rate from two other hosts across the
> lab's shared network, which waits for a quiet window.  Read it as the
> previous release's.

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
([bench/results/spreadbench-0.3.7.tsv](bench/results/spreadbench-0.3.7.tsv)).

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

### `KEYS` and `SCAN` against Redis, on production-shaped keyspaces

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
[bench/results/keyscanbench-0.4.0.tsv](bench/results/keyscanbench-0.4.0.tsv)).

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

## Compatibility

perfcached is pre-1.0 and makes no compatibility promise between
releases.  The native protocols, the cluster wire, the write-ahead log
and the snapshot format can change in any release.  An upgrade across
such a change means stopping every node, clearing its state and
starting the new version empty, as from 0.3.x to 0.4.0.  Run one
version across a fleet, and read a release's notes before upgrading.
