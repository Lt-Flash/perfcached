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
either side, is newer and wins.  In an eager collection a key that
EXPIRES is remembered the same way, at the version that expired (0.6.4):
an expiry sends nothing to the peers, so without it an older,
longer-lived copy could come back from a restarted node.  Size the cap
for both: deletes plus expiries a second, times `tombstone_retain_s`
(a collection expiring 420 keys a second and deleting 70 needs about
440,000 entries for 900 s).

**Deletes are acknowledged (0.7.3).**  The node that makes a delete
numbers it, and every member says in its keepalive how far it holds
each node's deletes - a delete counts as held once the member's WAL has
synced it (on apply with `fsync = no`).  A node forgets a delete as soon
as every member holds it, counting the members gone for less than
`tombstone_retain_s` at what they last said, so a member that is
restarting, frozen or cut off is waited for.  A member that missed a
delete - a lost datagram - asks for it again.  An expiry goes 10 s after
it is reaped (every member reaps its own copy), unless a write had
shortened the key's TTL: that one keeps `tombstone_retain_s`.  So while
the fleet is whole the table holds a beat or two of deletes, and
`tombstone_retain_s` and `tombstone_retain_max` become how long and how
much a MISSING member is waited for.  On a multicast fleet a delete goes
to the group once instead of to each peer, when it fits `max_datagram`
(`[cluster] data_multicast = auto`, the default; `off` keeps one copy a
peer).  This needs every member on 0.7.3 or later with `[cluster]
tombstone_acks = yes` (the default): while one is not, the whole fleet
keeps the time rule above, and `/stats cluster.acks.fleet` names the
members it waits for.

Three cases can still bring a delete back.  Re-issue deletes that matter
after a long split:
- **The partition outlasts `tombstone_retain_s`.**  The node that sees
  the peer come back logs a warning and raises `cluster.peer_absent`
  (0.6.5) naming the member and how long it was away.  In an eager
  fleet whose members acknowledge deletes (0.7.3), a member back
  without a restart that may lack a delete nobody holds any more is
  asked to rebuild instead: it reconciles every record it stored before
  it went silent with the fleet, keeps what it wrote since, and the
  alert says so - nothing to re-issue.  When no member was up 30 s
  before the split began - long enough to witness it - the records
  cannot be ruled on one by one, and the member resyncs instead (see
  "A member back after a long absence" below).
- **The cap drops entries before their time.**  At 1,000 deletes/s,
  250,000 entries cover about 4 minutes, not 15.  The
  `perfcached_cluster_tombstones_dropped_early_total` counter, the
  same warning and the alert `cluster.tombstones` (0.6.5) say when
  this happened.  From 0.7.2 they say how many of those entries were
  expiries (an eager collection's reaped keys) rather than deletes:
  the alert's text splits the two, `/stats` has `expiries` and
  `dropped_early_expiries` (with a 5-minute `win_` figure beside each
  drop count), and `/metrics` has
  `perfcached_cluster_tombstones_retained_expiries` and
  `perfcached_cluster_tombstones_dropped_early_expiries_total`.  A
  dropped expiry matters less than a dropped delete: the copy that
  comes back expires again, unless it was written with a longer TTL.
- **A node restarts during the partition.**  The memory is not kept on
  disk.  The node's own data comes back through the restart reconcile,
  but it can no longer send the other side the deletes it made.
  Restart one node at a time, and never the whole fleet during a split.

**Sizing the memory.**  While the fleet acknowledges deletes (0.7.3,
above) the table holds what a missing or slow member has not confirmed:
a beat or two of deletes and 10 s of expiries while all are up, and
the deletes made since a member went missing while one is.  Size the
cap for the longest absence you want covered - the rate below times that
absence - rather than for the whole window; `/stats
cluster.acks.held_by` says which member each kept delete waits for,
`/members` gives each member's mark of each node's deletes, how far
behind it is and how long its mark has stood still, and the alert
`cluster.acks_stuck` names a member whose mark stands still while the
others confirm.  Without acknowledgements (an older member,
`tombstone_acks = no`): every node remembers every delete the fleet
carries out and, in an eager collection, every key that expires (0.6.4),
so the rate that matters is the fleet's deletes plus expiries.  `/stats`
has it while the cap is not biting: `cluster.tombstones_retained.entries`
divided by its `oldest_age_s` is that rate a second.  Set
`tombstone_retain_s` above the longest split you want to survive without
re-issuing deletes, and `tombstone_retain_max` above that rate times
`tombstone_retain_s`, with room for the peak - at 300 deletes a second,
900 s needs 270,000, so 500,000 (about 50 MB, outside `arena_mb`).  The
page's negative-cache card shows the deletes kept against the cap, the
window, and any the cap dropped early.  Where deletes are the bulk of the
traffic, a pattern that does not need them survives any split: a TTL
instead of a delete, versioned keys (a new key per version, the old one
left to expire), or one writer per key - though in an eager collection
the expiries count against the cap too.  When the cap is reached, the
expiries go first: an expiry never pushes out a delete (0.7.2), so a
short-TTL collection's expiries fill the room the deletes leave, and
size the cap for the deletes alone if losing expiries early is
acceptable - a dropped expiry matters less, the copy that comes back
expires again unless it was written with a longer TTL.

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
unicast mode, distinct ports and seeds) - one daemon per cluster, since a daemon
runs exactly one `[cluster]`, so two modes on one host are two daemons.  Without `collections` the legacy per-collection `mode =`
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
thread is demonstrably the bottleneck.  A hot key pins one thread, and
0.7.1 names it: every collection in `stats` carries `hot_keys` (below).  `stats.cluster` shows `apply_threads`,
`apply_dispatched`, `apply_blocks` (receiver waits on a full ring) and
`apply_backlog` (bytes queued).


**Hot keys (0.7.1).**  Each node samples one request in 64 - per thread,
at random, so a client cycling through 64 keys is not aliased onto one -
into a space-saving sketch of the 32 heaviest keys per collection.  A
request counts where it enters the node (every door: native, the
fixed-layout verbs, RESP, JSON and hash commands, each key of an MGET)
and a replicated write counts where it is applied, the thread a hot key
pins on a replica.  `stats.collections[].hot_keys` gives `rate` (64),
`sampled` (the samples behind the table), `sampled_total` (every sample
since the last reset), `busy` (samples dropped because another thread
held the sketch - it never waits) and `top`: `key` (its first 64 bytes;
`cut` says when the key is longer, `key_len` how long), `est` and `err`
in samples (the true count lies between `est - err` and `est`), and
`share_pm`, per mille of `sampled`.  A key not surely seen twice (`est -
err` under 2) is noise and is not listed.  The counts halve every minute, so the table is the last few
minutes, as the page's other tables are; reset-stats empties it.  The
status page lists the keys of every collection, the hottest share first.
Nothing is exported to `/metrics`: a key is not a label.
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
(`repl_held_recovered`, `pull_held_recovered`).

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

**A member back after a long absence (0.7.3).**  A node that returns -
after a restart or a split - and whose records the fleet cannot rule on
one by one resyncs, as a node joins a Galera cluster by state transfer:
it drops every record it stored before its absence and pulls the
fleet's state from a ready member that holds records, as a joining node
does.  It is RECOVERING while it resyncs: a client is refused rather
than answered from what it holds, and nothing it drops reaches a peer.
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

**Which build each node runs (0.6.5).**  Every member's keepalive carries
its build - the version and the wire epochs it speaks - so `/members` on
any node lists `version` and `wire_epochs` for every member (null for a
member older than 0.6.5), each card on the status page shows it, and the
header turns amber with `mixed versions: 0.6.4 x1, 0.6.5 x2` while the
live members run more than one build: an upgrade in progress, or one that
stopped half way.  **The wire epoch (0.7.1).**  A build speaks a range of
wire epochs (`cluster.epoch.min..max` in `stats`; both 2 today) and the
fleet speaks one committed epoch (`cluster.epoch.committed`, carried in the
master's keepalive and persisted on every node).  A roll never changes it:
when every member runs a build that speaks a newer epoch, `perfcli -E ...
upgrade commit epoch=<n>` asks the master to raise it, and the master
refuses by name while any live member cannot speak it.  A member that
cannot speak the committed epoch is refused by name.  `upgrade plan`
finds that before the roll: every release tarball carries
`share/perfcached/RELEASE` (version, revision, edition, the wire epochs it
speaks), and the plan refuses a candidate whose range leaves out the
fleet's committed epoch.  A rollback to a
build that cannot speak the committed epoch is refused by the driver until
the epoch is committed back down, which the master allows while every
member still speaks the old one.  A datagram from a build whose cluster header version
differs is counted as `cluster.wire_ver_refused` and logged once a minute
by address - "builds this far apart cannot talk" - rather than as a
failed authentication.

**Rolling restarts: one node at a time, and only when it is safe.**  The
peers that were up throughout a node's absence are its WITNESSES; restart
a witness while that node is still asking, and its question can lose the
only node able to answer it.  Every node says, in `stats.cluster.restart`,
whether restarting IT now is safe: `safe` is false until a restart of it
would be witnessed (`safe_in_ms` says how long), and while any peer's
reconcile is still open - `waits_for` names those peers.  Restart a node
only once it says `safe`, then wait for the next one to say so too.  A
pass left with no witness at all - every peer restarted since the node
went down - waits rather than guess: the node holds its replayed copies,
`stats.cluster.reconcile_no_witness_s` counts the time, and the log says
once, after 30 seconds, to restart that node itself (its peers have been
up long enough by then to witness it).  A snapshot is what makes a single node independently
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

**A cache collection.**  A collection that is pure cache - its
contents can always be rebuilt or fetched again - does not need the log
at all:

    [collection sessions]
    durable = no

Its writes, deletes and TTL changes are never appended to the WAL, it is
never written to a snapshot, and a restart starts it empty on that node:
records logged or snapshotted before it was made a cache are skipped,
and the start says how many.  In a fleet the peers' copies still reach a
restarted node the ordinary ways - the repair sweep, a pull on a miss -
so only a restart of every node at once leaves it empty everywhere.
`/stats` reports `"durable": false` for it, the members compare which
collections are caches (`config.caches`), and a reload refuses the
change - it takes a restart.

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

**Watching the WAL's room (0.7.2).**  The node raises that comparison
itself as the `wal.capacity` alert - a warning once it has held five
minutes - and after a minute when the whole log fills faster than one
snapshot takes.  The
status page's WAL card shows the log's shape and how fast it fills: the
free segments of all of them and their size ("8 free of 48 · 48 × 64 MB
= 3.00 GB"), the share no snapshot covers yet, the fill rate and how long
the free space lasts at it (amber under twice a snapshot), and how long
the whole log takes to fill beside how long a snapshot takes (red when
the log is faster).  `stats.wal` carries `segments`, `segment_mb`,
`size_mb`, `used_segments` and `used_pct`; `/metrics` adds
`perfcached_wal_segments`, `perfcached_wal_size_bytes`,
`perfcached_wal_free_segments` and `perfcached_wal_used_ratio`.

**Going back to an older build.**  A build that finds state a NEWER
build wrote refuses to start and says so, rather than reading the newer
format as damage:

- a snapshot of a format past the one it reads (`-R` answers
  `INVALID (newer-format)`) - starting from the WAL alone would lose
  every replica the snapshot holds;
- a WAL record of a type it does not know, when no snapshot it can read
  already covers it - starting without it would drop a write;
- a `cluster-id` file whose header version is newer - starting as if it
  had none would join or found ANOTHER cluster.

The refusal leaves the file it refused as it was: run the newer build
again, or decide.  For the WAL that decision is `[wal] unknown_records = skip`,
which starts without those records and says how many it skipped; keep it
set for as long as the older build runs, since the records stay in their
segment until it is reused.  A newer record that lies under the loaded
snapshot's marker never refuses: the snapshot already holds what it did.
For the snapshot, move it aside; `-W` shows unknown records by type.

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
[collection 0]
upstream = redis://reader@10.0.0.12:6379/0
upstream_password = <the password perfcached sends to that upstream>
upstream_prefixes = tok:*, cid:*
```

`upstream_prefixes` is required: only keys matching one of its patterns
(Redis glob syntax, comma-separated, up to 32) ever fall through - say
`*` to mean all of them.  `upstream = redis://[user@]host:port[/db]`;
the password is the collection's `upstream_password` (0.6.1; it was
`[secrets] upstream`, which is now refused with that advice), never in
the URL - so one perfcached can front several Redis servers, a
collection each, each with its own password, and its clients reach
them all through it (`SELECT` picks the collection at the RESP door).
The password appears in no log line, `/stats` field, page or `-E`
dump.

**Several upstreams (0.6.1).**  `upstream` takes up to eight, and with
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
visible in `stats`; do not write to it.  The status page shows it on its
collection's read-through line ("N keys written here, never read from the
upstream again"), not as a row of its own, and counts the copies a
read-through collection fetched in its entries.

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
`read_lost` in the read-through stats counts the reads answered again.

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

**Where read-through and write-through are proven.**  Their suites run
on single nodes and eager fleets, and 245-247 run them in eager.  In
`shard` (0.7.1, DESIGN 12nt) a read-through collection fetches at the
key's OWNER: a miss on any node pulls from the owner, which fetches from
the upstream and keeps the one copy, so the fleet holds each key once
and no node fetches a key it does not own; the daemon refused the
combination before 0.7.1.  Write-through in `shard` (0.7.1) runs at the
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
upstream and keeps the copy (a second holder fills on its own demand -
at most K copies, never one per node); a write-through runs on the node
the client wrote on and its read-back is pushed to the K holders.  The
drain sweep (`updrain`) on every node takes over its own share only -
the owner's in `shard`, a holder's in `spread` (`drain.elsewhere` in
`/stats` counts the keys it left to the other nodes).  In `proxy`
read-through is still refused at start.

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
the key it had just set (about 4% of writes on a staging fleet whose
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
deleted (once in about 29,000 deletes on a staging fleet whose replicas lag
a few milliseconds).  A delete now opens the same window as a write, so
that read goes to the master.  `upstream_echo_ms = 0` turns it off with the
rest.

**After a tracking gap: warm again (0.7.1).**  A restart, or a lost
tracking connection, is a gap: Redis sent invalidations to nobody, so the
records written through that the node still holds (the WAL replays them)
cannot be trusted and are dropped.  Before 0.7.1 the node came back cold,
every held key one fetch on its next read (a roll on the test fleet took
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
stay unsupported.  The RESP door is plaintext and no TLS is planned for it
for now: under `through` it can write to Redis, so keep it on a trusted
segment, behind `resp_allow` and `[secrets] resp`.

### Other settings worth knowing

- `max_clients` under `[daemon]` caps client connections across the
  data doors.  Unset, it is the smaller of what the descriptor limit
  allows (minus what the process needs open) and what the memory holds:
  the memory limit, less the floor below, the arena's ceiling and the
  process's own base, at about 6 KB an idle connection.  `/stats` says
  which (`clients.max_basis`).  At the limit a connection is accepted,
  told why and closed, and `/stats` still answers: the RESP door and an
  encrypted native door answer `-ERR max number of clients reached` at
  once - the native client before any handshake, so a storm of refused
  clients costs no cryptography (a library that does not read the line
  sees its handshake fail); a plaintext native door (loopback, unix)
  answers an error frame to the first request.  The refusals are
  counted.
- `memory_floor` under `[daemon]` (default `15%`) is what keeps the
  out-of-memory killer away from the daemon, whatever `max_clients`
  says.  Once a second the daemon reads the memory it may still use -
  `MemAvailable`, or the headroom of a memory-limited cgroup it runs in,
  whichever is tighter - and takes off the arena's not-yet-committed
  ceiling, which the store may still grow into.  While less than the
  floor remains, new connections are refused exactly as at
  `max_clients`; connections already open are untouched, and refusing
  stops once a further 5% of the limit is free again.  `N%` is a share
  of the memory limit (1-50, kept between 128 MB and 4 GB), `N MB` /
  `N GB` an absolute floor, `off` disables the guard.  The state, the
  figures and the refusals are under `clients.memory` on `/stats`, on
  `/metrics` and as a red "memory floor" chip on the page.
- An idle connection holds no buffers: after two quiet seconds its
  input and output buffers are released (they grow back on its next
  request), and once a burst of released or closed connections has
  settled the daemon hands the freed heap back to the system
  (`malloc_trim`).  `clients.buffers` on `/stats` counts both.
- A client that pipelines faster than it reads is throttled, not
  closed.  Once 4 MB of replies wait for it, its further requests wait
  too and its socket is not read - TCP holds the client back - until
  the replies drain below 1 MB; then it is served from where it stopped.
  The first time a connection is throttled the log says
  `throttling client <ip:port>: N MB of replies unread`, and its
  disconnect line ends `, throttled N times`.  A client that reads
  nothing for `output_stall_s` under `[daemon]` (default 30) meanwhile
  is closed as a slow consumer.  `clients.output` on `/stats` and
  `/metrics` count the throttled connections, the pauses and the
  stall closes.  The 8 MB output cap still closes a pub/sub subscriber
  that does not read its messages - those are not requests it paced.
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
  reachable at all.  The map can be changed on a running fleet (0.7.1):
  `perfcli map <index> <collection>`, `unmap <index>` and `maps` (see
  PERFCLI-COMMANDS.md) - every connection sees the change on its next
  command, every member takes it, it is kept in `<state_dir>/respmap`
  across restarts and outranks `resp_collections` for its index until it
  is unmapped; the page shows each collection's DB index and, under the
  dashboard tool, map and unmap.  `stats` reports a `resp` block (connections,
  allow-list rejections, auth failures, the map as it holds: `map`,
  `map_overrides`).  The door also serves the
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
- **IPv6 on the client doors (0.7.2).**  `tcp`, `resp` and `http` take
  an IPv6 address: `[::1]:6480` listens on IPv6 loopback, `[::]:6480` on
  every address of both families - an IPv4 client of it arrives as
  `::ffff:a.b.c.d` (the door turns `IPV6_V6ONLY` off itself, whatever
  the host's `bindv6only` says).  `resp_allow` and `http_allow` take IPv6
  prefixes beside the IPv4 ones (`fd00::/8`, `[::1]/128`; a bare address
  is `/128`), and an IPv4 client of a `[::]` door is checked against the
  IPv4 entries.  `plaintext = loopback` counts `::1` as loopback.  An
  upstream may be written `redis://[::1]:6379`, and perfcli, perfdump,
  perfload and libperfd take `[v6]:port` and IPv6 literals.  Before any
  of this the node checks that the host has IPv6 at all - an IPv6 socket
  and a bind to `[::1]`, never a kernel version or a sysctl alone.
  Where that fails (`ipv6.disable=1` at boot, `disable_ipv6 = 1`, a
  container without IPv6) an explicit IPv6 listener is refused at `-C`
  and at start with the reason, and every name the node resolves - the
  listeners, the upstreams, the library's connects - asks for IPv4 only,
  so no AAAA query goes out.  `-C` prints `net: IPv6 is usable on this
  host` or `... is not usable ... - <why>`; `/stats` has it as
  `io.ipv6`.  The cluster stays IPv4 in 0.7.2: members advertise and
  reach each other over IPv4, and clients reach them over either.
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
   has moved on.  An index given back (a shrink, a resize, a `drop`) is
   kept for the next index; records can use its room again only once it
   borders the free space, which it does when nothing was carved after
   it (0.7.2).  `/stats` says which: `memory.arena_free` is the room
   records can still have, `memory.arena_free_tables` the given-back
   index space kept for tables (`perfcached_arena_regions_free_bytes`),
   and neither counts in `arena_used`.  A restart puts everything back.
   A table may also be widened PAST `grow_at_pct` by its
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
   config from then on.  In a fleet (0.7.2) the mode set this way is the
   fleet's: it reaches every member, a member that was away takes it when
   it is back, and the verb is refused while a member runs an older build
   (it names that member).  `/stats` shows each collection's
   `autoscale_set` and `autoscale_gen`, and `/members` each member's
   `autoscale_differs`; the alert `cluster.autoscale` says when a member
   holds other modes.  The sizing itself stays each member's: under `auto`
   a member sizes the table for the share of the keys it holds.  A create or a resize whose index would not fit under
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
subscriptions) with a filter on address and name.  Under the collections,
the commands table (sorted by its slowest call, or by calls on a click of
that column), the slow log and the alerts each fold to their first rows
until clicked; a read-through collection's own rows - its upstreams and
its writes - hang under it as one group, named.  The page stays read-only, and no door closes another client's
connection.  The collection rows answer for the fleet: every
figure shows the fleet's total first and this node's share beside it
("63 · 39 here"), summed from the per-collection block each member
gossips (its own client hits and misses, so a pull served for a peer
counts once), joined by the name's hash; entries summed that way count
copies, and the cell says so.
`http_allow` bounds who may reach it and `[secrets] http` adds a token -
sent as `Authorization: Bearer <token>`, or by a browser as its own login
(any user name, the token as the password: the 401 asks for it).
`http_rate` (20 requests a second per address, loopback exempt) answers
an address that asks faster with 429, and `http_cache_ms` (1000) builds
`/stats` and `/metrics` at most once in that long for any number of
readers - a page polls every 3 s, and loopback always reads them fresh.
`perfcli stats` gives the same figures over the native door.

### Mail

A node can send mail itself.  It needs a build with an SMTP client - `make
menuconfig`, mail = `libcurl` (libcurl's development files: Debian/Ubuntu
`libcurl4-openssl-dev`, Alpine `curl-dev`); a build without one refuses a
`[mail]` section at `-C` and says so, rather than ignoring it.

```
[secrets]
mail = <the SMTP password>

[mail]
smtp = smtp://mail.example.net:587      # STARTTLS, required
user = alerts@example.net
from = perfcached@example.net
to = ops@example.net, oncall@example.net
```

`smtp://` upgrades with STARTTLS and will not send without it;
`smtps://host:465` is TLS from the first byte.  The server's certificate
is checked against the system's CAs, or `ca_file`.  For an internal relay
whose certificate cannot be verified - Postfix's self-signed default, an
expired one, one that names another host - `tls_verify = no` keeps the
encryption and skips the check; `-C` and the log say so, since anyone on
the path could then pose as the server.  The password lives in
`[secrets] mail` - a URL with credentials in it is refused - and appears
in no log line, `/stats` field or `-E` dump.  `to` takes up to 8
addresses.

Sending runs on its own thread with a bounded queue, so a slow or dead
mail server never holds a worker; `timeout_s` (30) bounds one send.
`perfcli -E <enable secret> mailtest` sends one mail now and prints the
server's answer - `{"sent": true, "reply": "250 ..."}`, or `false` with
what the server or the connection said (a wrong password's `535`, a
server offering no STARTTLS, a timeout).  `/stats` `mail` carries
`built`, `ready`, `sent`, `failed`, `dropped` (the queue was full),
`queued`, and `last_error` with its time.

**Settings the fleet must agree on (0.6.5).**  Each member carries, in
its heartbeat, a hash of each setting whose difference changes what the
fleet does: `[wal] fsync`, whether the HTTP token is set, the `[alerts]`
thresholds, `tombstone_retain_s` and `tombstone_retain_max`, and for each
read-through collection `upstream_writes`, `upstream_prefixes`,
`upstream_role` and `upstream_unknown` - a member on `allow` beside
members on `through` takes writes that never reach Redis.  Per setting,
the value most live members hold is the fleet's (with two members, each
holds its own); every node raises `config.<setting>` naming the member
that holds another, its fleet card says "settings differ from this node",
and `/members` lists them as `config_differs`.  Per-node settings -
addresses, `io`, `workers`, `[mail]` - are not compared, and a member
before 0.6.5 reports none.

**What raises an alert.**  Every node checks, once a second, the figures
it already keeps:

| alert | level | raised | cleared |
|---|---|---|---|
| the node FAILED / HEALING | critical / warning (critical after 10 minutes HEALING) | at once | when it is not |
| the node's reconcile has no witness - every live peer restarted after it went down: restart this node (`node.reconcile`, 0.6.4) | warning | after 30 s | 30 s after it resumes or ends |
| the WAL dropped or overran records | warning (critical for the third loss within an hour) | the second it happens | after 5 quiet minutes |
| WAL fsync's mean over the last minute above 4x the start-up probe | warning | after 5 minutes of it | after a minute below |
| the WAL filling faster than snapshots free it - the whole log fills faster than one snapshot takes, or its free segments last under twice the last snapshot's duration (`wal.capacity`, 0.7.2) | warning | after a minute of the first, 5 minutes of the second | after 5 minutes without |
| writes refused, the arena full | critical | the second it happens | after 5 quiet minutes |
| the arena under 10% free (under 1%: at its ceiling) | warning (critical) | after 30 s | after a minute above |
| a read-through upstream unreachable, or its tracking off | warning | after a minute | after a minute back |
| a write-through collection without a master: every write refused (`write.<col>`) | critical | after 10 s | after 10 s back |
| no RedisJSON on the write-through master (`write.<col>.json`) | warning | after 10 s | after a minute back |
| writes refused at a full write-through queue, or not applied after waiting too long in it (`write.<col>.queue`) | warning | after 10 s | after 5 minutes without |
| a write-through collection not following other clients' changes in Redis - broadcast tracking off or refused (`write.<col>.tracking`) | warning | after a minute | after a minute back |
| commands perfcached does not know over a tenth of the writes in 5 min, of 20 or more (`write.<col>.passed`) | warning | after a minute | after 5 minutes below |
| on epoll, the busiest worker over `[alerts] epoll_busy_pct` (80) of its time outside its wait, over a minute - with the change to make | warning | after 5 minutes of it | after a minute below |
| deletes or expiries the tombstone cap dropped before `tombstone_retain_s` in the last 5 minutes, the text saying how many of each (`cluster.tombstones`) | warning | after 10 s | 5 minutes after the 5-minute count is back to 0 |
| a member back after a silence longer than the retained deletes cover (`cluster.peer_absent`) - or, where members acknowledge deletes (0.7.3), one asked to rebuild for it | warning | once | - |
| a member whose mark of a node's deletes has stood still for 30 s while the others confirm them - every delete since is kept for it (`cluster.acks_stuck`, 0.7.3) | warning | after 10 s | after a minute moving |
| a setting the fleet must agree on held differently by a member (`config.<setting>`, 0.6.5) | warning | after a minute | after a minute agreeing |
| a member holding other autoscale modes than this node's, or one too old to take them while modes are set (`cluster.autoscale`, 0.7.2) | warning | after 30 s | after a minute agreeing |
| the node started (ready to serve) | info; warning after an unclean stop | once | - |

A level says how soon a person must act, not how bad the event was:
**critical** - now, the node cannot recover by itself; **warning** -
today, the node is handling it (a lost WAL record heals by a snapshot, a
node back after a crash is serving again); **info** - routine.  A
self-healing alert turns critical when the healing stalls.  An unclean
stop is told by the WAL: a clean stop snapshots after the WAL
is flushed, so a start that replays records above the last snapshot
follows a crash, a `kill -9` or a power loss.  Each alert is logged (at
NOTICE, WARNING or CRITICAL by its level, and its clear at NOTICE) and kept in `/stats`
`alerts` - `active`, and `recent`, the last 100 raised and cleared,
newest first, each saying what became of it by mail.  With `[mail]`,
alerts at `level` or above (`warning` by default; `info` mails a clean
start too, `critical` only what needs someone now) are mailed: the same alert
at most once per `min_interval_s` (900) - a repeat inside it is kept as
`suppressed` - unless it got worse; everything raised or cleared within
`digest_s` (60) goes out as one mail; and a clear is mailed when its raise
was.  Without `[mail]` the ring says `no mailer`.

A mail has a plain-text part and an HTML part (inline styles, no
images or links to fetch) and reads like this:

```
From: "au-dev-perfcached1" <perfcached@example.net>
Subject: WARNING: WAL fsync slow on au-dev-perfcached1
```

The subject is `<STATE>: <title> on <host>`, at most 78 characters -
CRITICAL, WARNING, INFO, or RESOLVED - with `(+N)` when the mail carries
N more; From's name is the host the alerts are about.  The body says,
per alert, what happened, its status (a new problem, still open,
resolved, or an event), when it started, what the node does by itself,
what to do, and the alert's ID (`wal.fsync`, `upstream.0`, ...).  A
RESOLVED mail says what was wrong at its worst, the value now and how
long it lasted, and answers its problem's mail (`In-Reply-To`), so a mail
client shows the two together.

**In a fleet, `[mail]` on one node is enough.**  A node that can mail
says so in its heartbeat, and every node agrees on one MAILER: the
lowest node id among the ready members that can.  A node without
`[mail]` sends the alerts it would mail to that node over the cluster
plane, and the mailer sends them as mail about that host - From
its name, its node id beside it - and one mail per host; the SMTP
password stays on the nodes whose config holds it.  What happens to the
fleet itself - a member silent, failed or stalled, gone or missing
against the fleet's size, or fewer ready than a majority (critical) -
is seen by every member and mailed by the mailer alone, once.  Put
`[mail]` on two or more nodes and any of them can carry the fleet.  When
no member can mail, a node says "no mailer reachable" in its log (at most
every ten minutes) and keeps its alerts in the ring.  A network split
elects a mailer on each side, so a split fleet may send a mail from each
- two rather than none.  The status page shows where mail goes - "mail
this node / via node N / none" in the header - and an alerts card with
what is raised now and the recent ring.  `/stats` carries
`alerts.mailer`, `alerts.fleet_mailer` and, in `mail`, `relayed_in`,
`relayed_out` and `relay_dropped`; `/metrics` has
`perfcached_alerts_active{level}`, `perfcached_mail_ready`,
`perfcached_mail_{sent,failed,dropped}_total` and
`perfcached_alert_relays_total{direction}`.

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

A read-through collection is the exception: its upstream is bound to the
name in the config, and so is its `.nofall` companion's, so `rename`
refuses both (and refuses a new name that a read-through collection has
in the config).  Rename one in the config instead, and restart every
node at once: the members compare their collection lists before they
admit each other, so a node restarted alone with the new name refuses
the others until they have it too.

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
`stats.door.enable_fails`, and after `enable_lockout_after` of them from
one address within `enable_lockout_s` seconds (5 in 60 by default) that
address's `enable` is refused for the window - "too many wrong enable
secrets from this address" - whatever sends them; `door.enable_lockouts`
and `door.enable_refused` count the lockouts and the refusals.

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

### Reloading the configuration (0.7.1)

`systemctl reload perfcached` reads `/etc/perfcached/perfcached.conf`
again and applies what a running daemon can take, without a restart:

- `[listen] resp_allow` and `http_allow` - the next connection is checked
  against the new list;
- `[secrets] http`, `resp` and `enable` - the next request or AUTH;
- `[secrets] client` - every handshake after the reload needs one of the
  new secrets; a connection already open keeps its session;
- `[daemon] log_level` and `slowlog_usec`.

Anything else that changed - the cluster mode, a listener's address, a
collection, the memory, `[secrets] cluster`, ... - refuses the reload
WHOLE: nothing is applied, not even the keys that could have been, the
daemon logs an ERROR naming every such key, and `systemctl reload` fails
with the same words:

    reload refused: [cluster] mode changed and cannot be reloaded - restart
    to apply it; nothing changed

A file that does not load is refused before the daemon is asked, with
the line that is wrong.  Under the hood the unit's `ExecReload` runs
`perfcached -f <conf> -H $MAINPID`: it checks the file as `-C` does,
sends the daemon SIGHUP and waits for its answer, so the exit status is
the daemon's verdict.  A plain `kill -HUP` does the same, its outcome only
in the log (before 0.7.1 SIGHUP stopped the daemon).  `perfcli -E <enable
secret> reload` asks over the native door and answers when it is done;
`stats.reload` counts the reloads applied and refused, with the last
outcome.

The comparison is by key, with the file the daemon started with (or last
reloaded): a key added, removed or given another value is a change, so
deleting a line to fall back to its default counts too.  The daemon keeps
no secret in clear for this - each value is held as a digest under a key
of its own.  Rotating the client secret without dropping clients: add the
new one as a second `client =` line and reload, move the clients over,
then delete the old line and reload again.

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

    perfcli -E <enable> reset-stats              # this node only (the native method reset_stats)
    redis-cli -p 6380 CONFIG RESETSTAT            # this node only (the RESP door)

The dashboard's button sends the native `reset_stats` with `fleet: true`
(what `POST /reset-stats` on the daemon's HTTP door did until 0.6.6; that
door is read-only since 0.7.0): it resets its node and asks every live peer to reset as of
the same moment, so the fleet figures the page shows all count from one
time; each peer logs whose reset it took.  A reset covers the doors, each
collection's table counters (hits, misses, stores, removes, expired - the
core's own re-baseline), the read-through figures (fetches, absent,
errors, ... - not a drain run's), the store's size tallies, and the
cluster, proxy and WAL counters.  It never touches a gauge: open
connections, entries, buckets, memory, sequence numbers, roles.
`/metrics` is never reset, by the schedule or by hand: it counts from the
daemon's start - Prometheus counters are meant to be monotonic, and a
drop would read as a counter reset.  The HTTP door has no mutating route
since 0.7.0: any `POST` is a 405.

### Which builds a node trusts

A release tarball comes with a detached ed25519 signature beside it
(`perfcached-<version>.tar.gz.sig`).  The release key's public half is
built into every binary; `perfcached -E` prints it on a comment line
under `[upgrade]`.  Check an artifact before you install it, with the
binary you already run:

    perfcached -K perfcached-0.6.5.tar.gz
    perfcached -f /etc/perfcached/perfcached.conf -K perfcached-0.6.5.tar.gz

It prints the size, the sha256 and the verdict: `GOOD` with the signer
named (exit 0), or `BAD`, `NONE` or `UNREADABLE` and refused (exit 1).
The signature file holds the 64 raw bytes `openssl pkeyutl -sign -rawin`
writes, or the same as 128 hex characters.  With `-f`, the keys the
configuration adds are tried too:

    [upgrade]
    trusted_keys = <64 hex characters>[, ...]   # up to 4, beside the release key
    allow_unsigned = no                         # yes: an artifact with no .sig passes

`trusted_keys` is for builds you sign yourself.  Make a key with
`openssl genpkey -algorithm ed25519`.  Its public half for this line is
the last 32 bytes of `openssl pkey -pubout -outform DER`, in hex.

**The support window.**  A fleet on any patch of a minor upgrades by a
rolling restart to any later patch of that minor and to the next minor's
releases; from an older minor, step through the last patch of each minor
in between, each step a rolling restart.  Mixed versions are for the roll,
not for running: the daemons report their build on the wire, the status
page says "mixed versions" and blinks the cards still on the older build,
and a release note names any exception.  The order is members first, the
master last, one node at a time, reading the fleet between nodes (every
node `ready`, its peers up, the record counts not fallen, no reconcile
waiting on the node you are about to restart - the page's header says
"do not restart" while one is).  The client library of the same or the
previous minor talks to both sides of a roll (the test and staging
fleets run libperfd 0.6.0 against 0.6.x daemons).

**Staging a build on a node.**  `upgrade_stage` (native door, a
privileged connection) receives a tarball in order: `begin` with its
size, sha256, the signature as 128 hex characters and the version, then
`chunk`s of at most 256 KB (base64, each at the offset the node holds),
then `end`.  Only at the end does the node read the whole file back,
check the sha256 and the signature (the built-in key, `trusted_keys`;
unsigned only under `allow_unsigned`), and publish it as
`<state_dir>/upgrade/staged/<sha256>.tar.gz` with its `.sig` and
`.meta` beside it.  A refused upload leaves nothing behind.  One upload
runs at a time (an idle one is abandoned after five minutes), at most
two builds stay staged, and `[upgrade] stage_mb` (256) bounds them.
`upgrade_status`, unprivileged, lists the staged builds - version,
size, signer - and the upload in progress.  From the command line:
`perfcli -E <enable> upgrade-stage perfcached-0.6.5.tar.gz 0.6.5` sends
the tarball and the `.sig` beside it.

In a fleet, the node you upload to passes the build on to every member
over the cluster's bulk lane once it has staged it.  Each member stages
it the same way and verifies it with its OWN keys, so a member whose
`trusted_keys` do not include the signer refuses it.  A member never
passes on what it received.  `upgrade_status` on the node you uploaded
to says what each member answered: `relay`, one entry a member -
staged, refused, no answer, or "not asked: an older build" for a
member that cannot take one.

**The upgrade helper.**  An install from the release tarballs lives in
`/opt/perfcached-<version>` behind a link, `/opt/perfcached`, which the
unit's `ExecStart` runs through.  For that layout `make install` adds a
root helper, `sbin/perfcached-upgrade`, and two units that are NOT
enabled until you opt in:

    systemctl enable --now perfcached-upgrade.path

The path unit starts the helper when the daemon writes
`<state_dir>/upgrade/request`.  The helper copies the request and the
artifact into a directory only root writes,
`/var/lib/perfcached-upgrade`, reading them AS the state directory's
owner and following no symlink, so a request can only lead where the
daemon could read already, and nothing the daemon's user can change is
read twice.  It then installs the build only if the binary already
installed verifies the copy (`-K`, with the configuration's
`trusted_keys`).  It unpacks the tarball beside the live prefix and
checks the live configuration with the new binary (`-C`).  It stops the
unit, carries the state across when `state_dir` lives in the prefix,
switches the link and starts the unit.  It waits up to `ready_s` for the
NEW process to report ready; if that never comes, it switches back.  The
outcome goes to `/var/lib/perfcached-upgrade/result-<request id>`, and
no prefix is ever deleted.

To install a staged build, ask the node: `upgrade_switch` with the
build's sha256 (a privileged connection).  It verifies the file again,
writes `<state_dir>/upgrade/request`, and the helper takes it from there.
It refuses while a request is already waiting, and when the helper's
path unit is not enabled on the host (`force` writes the request all the
same).  `upgrade_status` then says `requested`, with the helper's
`result` once there is one - read from `[upgrade] results_dir`, which
must be where the helper writes (`results` in its settings; both
default to `/var/lib/perfcached-upgrade`).  The helper stops the node
cleanly, so a master says goodbye and the fleet elects another, as in
any rolling restart: switch the master last.  `upgrade_switch` with
`member` set to a member's address asks THAT node instead, over the
cluster; it answers whether it wrote the request (or why not: not staged
there, no helper there, a request already waiting), and
`upgrade_status.asked` on the asking node shows the answer.  A member's
ask is never forced.

**Rolling a fleet (0.7.0, DESIGN 12ns).**  `perfcli upgrade` is the roll
driver: a program with a journal and no state of its own, reading the
fleet before every step.

    perfcli -h <seed> -E <enable> upgrade plan perfcached-0.7.0.tar.gz version=0.7.0
    perfcli -h <seed> -E <enable> upgrade start
    perfcli -h <seed> -E <enable> upgrade status | abort | resume | rollback

`plan` records the artifact, its sha256, the version and the seed in
`~/.perfcached/upgrade-<seed>.journal` (`journal=` names another).
`start` stages the build on the seed, waits until every member holds it
(the daemons relay and verify it), then rolls one node at a time:
members by id, the backup, the master last - the order recomputed before
each node from the live member list.  Before a node: every node
`restart.safe`.  After its switch: a new pid on the version asked within
`ready_s` (600), `ready` with its peers up within `gate_s` (90), the
peers' record counts not fallen, every node restart-safe again; then it
is `gated` and the next begins.  The first gate that fails stops the
roll: the driver reverts THAT node (`upgrade_revert`), leaves the nodes
already rolled on the new build, and prints the three commands - revert
this node, `rollback` the roll, `resume` it.  `abort` stops a running
roll between nodes; `resume` re-reads the fleet and the journal and rolls
only what is left; `rollback` reverts the rolled nodes in reverse order,
the master first; `status` shows the plan, the journal and every node's
live version, state and roll state.  A node that is unreachable is never
skipped: the roll stops until it is back.  `switch_cmd=<program>` replaces
the daemon's switch and revert verbs with a program of yours (`<program>
switch|revert <addr> <port> <node> <artifact> <sha256>`) - what the CI
roll uses with a loopback fleet, and what a package host could use with
its package manager.

**Going back (0.7.0, UP7b).**  The helper keeps the prefix it switched
away from, and `upgrade_status` says so: `previous` is its last switch
(`from`, `to`, `at`, read from its results), `live_prefix` the prefix this
process runs from, and `switching` a request the helper has taken and not
answered yet (`id`, `since`).  `upgrade_revert` (privileged; `perfcli -E
<enable> upgrade-revert [force]`) asks the helper to switch back to
`previous.from`: refused on a package or local install, with no switch on
record, while a request waits, or when this node does not run the build
the helper switched to last (`force` overrides that and the helper check).
The helper believes only its own records - the prefix must be the one its
newest ok result switched from - and then takes the same path as a
switch: stop, carry the state, switch the link, start, wait for ready,
and switch forward again if the previous build does not come up.  A
revert's result becomes the new `previous`, so a revert of a revert goes
forward again.  This is what the roll driver (12ns) uses when a gate
fails on a node.

Every node writes `<state_dir>/upgrade/node`: its pid, version,
revision, node state and since when.  That is how the helper knows the
new process is ready without reading a secret (`/health` is liveness
only, and behind the token).  The helper's settings live in
`/etc/perfcached/upgrade.conf` (example installed beside it): `unit`,
`link`, `conf`, `state_dir`, `ready_s`, `results`.  Upgrades need
`[daemon] state_dir`: the node file, the staged builds and the request
live there, and a node without one stages nothing (the helper reads the
directory from the configuration; its own `state_dir` only overrides
that).  If `state_dir` is not the unit's StateDirectory, point the path
unit at it with a drop-in.  The Debian packages do not carry the helper: a package
upgrades through the package manager.

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

### perfcached-dashboard

The status page, served by its own process (0.6.6).  The daemon's HTTP
door serves the same page today, but every action a dashboard offers
would have widened the daemon's largest attack surface, and a dashboard
under attack would have stalled the daemon serving it.  So the page
moved: `perfcached-dashboard` is a client of the fleet's native door
through libperfd, like perfcli - encrypted end to end, one connection
per member - and nothing in a daemon knows it exists.

    perfcached-dashboard -s 10.0.0.1:6479 -a 's3cret'       # serve on 127.0.0.1:8090
    perfcached-dashboard -f /etc/perfcached/perfcached-dashboard.conf

One seed is enough: the member list it answers names the rest, and the
node switch at the top of the page moves between them.  A node asked for
is that node or an error naming it (0.7.1): while a member is away - left,
restarting - its figures are not shown and an action addressed to it is
refused, never served by or run on another member.  Privileged
actions - the write-through line's **check**, **upfill** and **stop**,
the read-through line's **drain** and **stop drain** (0.7.0; the line
shows the sweep's figures while it runs), and **reset stats** - ask for the fleet's enable secret once per browser
page; the tool keeps it in memory for its session and never writes it
anywhere.  Wrong secrets are counted per address and lock it out.
The collections table carries the live DDL too (0.7.0): **+ new
collection**, and **rename**, **resize** (a background migration the
row shows) and **drop** (the confirmation names the entry count) on
each row - the daemon's own verbs on the selected node, announced to
the fleet.  A collection's *settings* (its read-through or
write-through, prefixes, timeouts) are configuration, not DDL: they
change in `perfcached.conf` and apply with a rolling restart.

**Where it runs.**  The package ships `perfcached-dashboard.service`
disabled, with its own user and a memory and CPU cap; `systemctl enable
--now perfcached-dashboard` on one node shows the whole fleet.  It binds
loopback unless its config says otherwise.  TLS, HTTP/2 and a port that
faces a network are a reverse proxy's job (nginx, HAProxy, Caddy)
terminating in front and speaking HTTP/1.1 to the tool - the shape
Grafana and Prometheus take.  A dashboard under attack costs the host the
unit's slice and the daemon nothing.

**Several dashboards.**  A dashboard holds no state worth replicating -
the fleet's state lives in the daemons - so redundancy is running more
than one, anywhere: two behind a keepalived VIP, or N behind HAProxy or
nginx with `GET /health` as the check (503 when the dashboard sees no
member).  Give the instances the same `session_key` in their configs and
a privileged session opened on one is honoured by the others: the token
the browser holds carries the session, encrypted under that key, so the
load balancer needs no stickiness.  Ten dashboards on one fleet cost one
native connection per member each; actions never conflict, the daemon
is the arbiter (a second `upfill` on a sweeping collection is refused).
`contrib/haproxy-perfcached-dashboard.cfg` and
`contrib/nginx-perfcached-dashboard.conf` are complete front ends for
N dashboards (TLS and HTTP/2 there, `/health` as the check); name the
proxies in `trusted_proxy =` so the lockout counts the client the proxy
saw (its last `X-Forwarded-For` address), not the proxy.

**The fleet in one table.**  Under the tool the page adds a table
below the member tiles that describes the NODES: each member's version
and revision, install mode, the builds staged there, the records it
holds over all its collections and the read-through copies of Redis
values beside them (a dash where it has no read-through collection) -
the tool's `/fleet`, and the view an upgrade is planned from.  The last
row is the fleet, counted the way the collections table counts it,
collection by collection by its mode's rule: `shard` adds the members
up, `spread` divides by K, `eager` takes the fullest member and `store`
takes the distinct-key estimate below (`≈`), or the fullest member as a
lower bound (`≥`) until there is one.  A collection's figures member by
member are under the collection: its **members** chip in the
collections table opens a line per member (records, Redis copies) and
it stays open across the redraws.  In the collections table a
collection's entries cell shows its records - the fleet figure, then
this node's - and this node's Redis copies on a line of their own
(0.7.1; before, the fleet table had a column per collection, which
does not fit 64 of them).

**Distinct keys in store mode** (0.7.1).  In `store` a node keeps what
it pulled, so a key lives on every member that asked for it: adding the
members up counts it once per member, and the fullest member is only a
lower bound.  Each member's held walk (every 5 to 60 s) builds a
HyperLogLog sketch of the keys it holds - 4,096 one-byte registers
under one fixed hash on every node - and hands it out on the
unprivileged `sketch` verb.  The tool merges the members' sketches (the
register-wise maximum is the sketch of the union), keeps each for 10 s,
and shows the estimate as `≈ N`; its standard error is 1.6%, so 3,000
keys read between about 2,950 and 3,050 two times in three.  `/fleet`
reports it per collection under `distinct`:
`{"estimate":N,"members":k,"of":n,"oldest_walk_s":s}`.  The page uses
it only when every reporting member sent a sketch (`members` equals
`of`): during a roll from a daemon without the verb it shows the lower
bound instead of a union that can count low.  A sketch is as old as the
walk that made it; a key written since is in the next one.

Below it, **stage a build**: choose the release tarball, its version
(filled from the file name) and, when you have it, the signature as
128 hex characters; the tool sends it to the selected member over the
native door and the daemon verifies and keeps it as `upgrade_stage`
does (`[upgrade] allow_unsigned`, `trusted_keys` apply).  The tool
accepts a build up to `stage_max_mb` (64).

**TLS without a proxy.**  A bare install can serve TLS itself (0.7.0):
`tls_cert` and `tls_key` in its config (PEM), libmicrohttpd's own TLS;
`perfcached-dashboard -V` says whether the library has it.  HTTP/2 and
HTTP/3 still come from a proxy.

**The admin image.**  `contrib/docker/admin/` builds an image with the
dashboard, perfcli and nginx in front (`tools/image-admin.sh`, podman or
docker; CI publishes it as a tar on each tag): `podman run -p 8080:8080
-e PCD_SEEDS=10.0.0.1:6479 -e PCD_SECRET_FILE=/run/secrets/pcd -v
./pcd:/run/secrets/pcd:ro perfcached-admin`; mount
`/etc/perfcached/tls/{cert,key}.pem` and it serves 8443 with TLS and
HTTP/2 instead.  The dashboard inside listens on the container's loopback
only; `perfcli` is there for the shell.  `contrib/docker/admin/compose.yml`
runs two replicas behind one nginx (TLS and HTTP/2 there; open-source
nginx checks passively, retrying a failed or 503 answer on the other
replica; the session key shared through `PCD_SESSION_KEY`); with
`PCD_FRONT_PROXY=yes` the image's nginx passes the browser's address
through, so the lockout counts browsers, not the front.  Set that only
where your own proxy is the sole thing reaching the published port.

**The daemon's own page is gone** (0.7.0): `GET /` on the daemon's HTTP
door answers 410 and names the dashboard, and the door is read-only:
`POST /reset-stats` went with the page (405; the reset is the native
`reset_stats` verb - `perfcli -E <enable> reset-stats`, or the
dashboard's button).  `/stats`, `/members`, `/clients`, `/metrics` and
`/health` stay for Prometheus and scripts.  The page's bytes live in one object the
dashboard links; `./pagedump` prints them for the tests.

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

**The key, derived once (0.5.6.5).**  The Noise key is derived from
the secret with Argon2id - about 150 ms of CPU and a 64 MiB mapping, on
purpose: an attacker guessing a stolen handshake's secret pays the same.
libperfd derives it once per secret per process, and every connect,
standby and reconnect reuses it; threads that connect at the same moment
with nothing derived yet wait for the first one's key instead of each
deriving their own.  A server that forks its workers can derive before
the fork - `perfd_psk_prepare(secrets)` in the parent - and no worker
derives at all.

**Cluster-aware.** Set `opts.spares` and the library learns the
fleet on connect, keeps standby connections open to the other nodes,
and swaps onto one when the node it is using dies - a failover costs a
`send()` on an established socket, not a TCP+Noise handshake.
`opts.policy` picks where a client works: `FAILOVER` (default),
`ROUND_ROBIN` (independent random start per client - a thousand
clients spread with no coordination), `LEAST_CONN` or `WEIGHTED` (by
the free arena each node reports).  Idempotent verbs are replayed
across a failover; `add`/`sub` are NOT - the caller is told, because a
double increment is worse than a visible error.  A member that is back
up but not READY yet (restarted, replaying and pulling its bootstrap)
refuses every request with "node is not READY (recovering) - retry, or
use another member"; nothing was applied, so the library (0.7.0) takes
the request to another member - any verb, a counter update too - and
marks the refusing one down until the next members refresh sees it
ready.  `perfd_member_count`,
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

### nginx in front: caching responses (0.7.1, no module of our own)

nginx can keep whole HTTP responses in perfcached today, through modules
OpenResty already bundles - `srcache` decides, `redis` (ngx_http_redis)
fetches, `redis2` stores, `set-misc` and `echo` build the key and the
body - against the RESP door.  ngx_http_redis cannot send `AUTH` and
neither module speaks TLS, so the door they use stays on loopback: the
node on the nginx host listens on `127.0.0.1` with
`resp_allow = 127.0.0.1/32` and no `[secrets] resp`.

```ini
[listen]
resp = 127.0.0.1:6380
resp_allow = 127.0.0.1/32
```

```nginx
http {
    upstream perfcached {
        server 127.0.0.1:6380;
        keepalive 32;
    }
    upstream app {
        server 127.0.0.1:8080;      # the application
    }
    server {
        listen 80;

        # fetch: GET <key>; ngx_http_redis answers with the value as the body
        location = /pc-get {
            internal;
            set_md5 $redis_key $args;
            redis_pass perfcached;
        }
        # store: SET <key> <response>, then EXPIRE <key> <seconds>
        location = /pc-set {
            internal;
            set_unescape_uri $exptime $arg_exptime;
            set_unescape_uri $key $arg_key;
            set_md5 $key;
            redis2_query set $key $echo_request_body;
            redis2_query expire $key $exptime;
            redis2_pass perfcached;
        }
        location / {
            set $key $uri$is_args$args;
            set_escape_uri $escaped_key $key;
            srcache_fetch GET /pc-get $key;
            srcache_store PUT /pc-set key=$escaped_key&exptime=$srcache_expire;
            srcache_default_expire 120s;
            srcache_store_max_size 255k;    # a record is at most 262,080 bytes, key included
            add_header X-Cache $srcache_fetch_status;
            proxy_pass http://app;
        }
    }
}
```

A response is stored whole - status line, headers and body - under the
MD5 of its URI and arguments.  srcache takes its lifetime from the
response's `Cache-Control` or `Expires`, or from `srcache_default_expire`
when it sends neither, and `EXPIRE` sets it on the key.  A fetch that
finds the key answers without touching the application, and `X-Cache`
says `HIT` or `MISS`.  The modules use database 0, so the responses land
in the collection `0` unless `resp_collections` maps 0 elsewhere
(`0:http` keeps them apart from the rest).  A record is at most 262,080
bytes, key and header included (see Sizing); `srcache_store_max_size
255k` keeps nginx from offering a larger one, which is served and not
stored.  Checked with OpenResty 1.25.3.2 on this configuration: the
second and third requests for a URL were hits and the application saw
one, each key held a 120-second TTL, and a 300 KB page was served twice
and stored never.  A module that speaks the native door - encrypted to
remote nodes, routed to a key's owner - is G7b in DESIGN 12nh, not built
yet.

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
