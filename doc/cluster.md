# A cluster

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

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

## Unicast discovery, clouds and Kubernetes

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
  (1,232 - what IPv6 guarantees on any path, 0.7.4) goes over a TCP
  connection to the peer instead (`lane_port`, the cluster port + 2).
  The heartbeats stay small UDP datagrams, so a dead node is still
  noticed in seconds.
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
  blank nodes, and the lowest identity founds while the rest join it.
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

## Identity and the cluster id

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

## The collection config and the ports

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
publishes and, since 0.7.4, eager replication's write-path pushes, on
threads of its own (by default half as many as the workers), so neither a publish storm nor a write flood can
starve the heartbeat socket - and an eager fleet's writes are applied by
several threads instead of one.  A node that cannot reach a peer's relay
port sends it relays and pushes on the cluster port instead, and after
10 s logs a warning naming the peer; a peer on an older build answers
for relays only and gets pushes on the cluster port.
`pubsub_rx_threads = 0` gives up the relay port altogether.  In `discovery = unicast` one more TCP port is used, the
frame lane (`lane_port`, by default the cluster port + 2): frames
larger than `max_datagram` (1,232 bytes) travel on it instead of as
IP fragments.  In either mode, replication groups longer than
`max_datagram` go to the peer's relay port over TCP (0.7.4; 0 sends
everything as datagrams): each sending worker holds a connection a
peer, and the relay threads read them.  So a unicast fleet needs the cluster port on UDP and TCP, the
lane port on TCP, and optionally the relay port on UDP and TCP - all
three addressed to the nodes themselves, never through a load balancer
or a Kubernetes Service's virtual IP.  Every one of these a node uses -
the relay port among them while it is open - must answer before a joiner
serves (0.7.5, [modes.md](modes.md), "Ordered joining"): a blocked one holds the joiner
`connecting` rather than falling back.

## Deletes across a fleet

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
members it waits for.  In 0.7.5 a fleet on `tombstone_acks = no` cannot
be moved to `yes` by a rolling restart: a node on `yes` among members on
`no` loops.  Until 0.7.5.1, leave a fleet that runs `no` on `no` (a
switch to `yes` needs every node changed and the fleet restarted whole).

Three cases can still bring a delete back.  Re-issue deletes that matter
after a long split:
- **The partition outlasts `tombstone_retain_s`.**  The node that sees
  the peer come back logs a warning and raises `cluster.peer_absent`
  (0.6.5) naming the member and how long it was away.  In an eager
  fleet whose members acknowledge deletes (0.7.3), a member back
  without a restart that may lack a delete nobody holds any more is
  asked to rebuild instead: it copies the state of a member that was up
  through the split over every record it stored before it went silent
  (0.7.5; before, it asked the fleet about each), keeps what it wrote
  since, and the alert says so - nothing to re-issue.  When no member was up 30 s
  before the split began - long enough to witness it - the records
  cannot be ruled on one by one, and the member resyncs instead (see
  [modes.md](modes.md), "A member back after a long absence").
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
  disk.  The node's data comes back through the restart (an eager node
  copies a peer's that was up throughout, 0.7.5), but it can no longer
  send the other side the deletes it made.
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

## Failure detection, threads and hot keys

Failure detection
runs on 1 Hz heartbeats with real margin - a master is presumed dead
after 8 s of silence, a peer is dropped after 6 s (kept on TCP when
only its UDP is blocked), so jitter is not death - beat
emission is watchdog-backed (`stats cluster.hb_sent` counts the beats
sent, `hb_watchdog` the ones the watchdog re-sent, 0.7.5), and datagram
ingest is fairness-bounded
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
replayed key against the fleet, 64 to `reconcile_rate_max` a second
([modes.md](modes.md), "Asking key by key"), to drop what was deleted
while it was down.  Both are O(N) in the keyspace; `stats.cluster`
shows what the last completed pass of each cost - `repl_sweep_ms` with
`repl_sweep_scanned` and `repl_sweep_sent`, and `reconcile_ms` with
`reconcile_pending` - so the point where they need narrowing is read,
not guessed.

## What a node says about itself

**What a node says about itself.** `stats` carries `state` and
`state_reason`, and `/members` carries them for every node as the others
hear them (`state` and `reason`): `starting`, `connecting` (admitted and still
proving its links - client data refused, 0.7.5), `recovering`, `ready`, `draining`,
`healing` (its log missed a record: reads served, writes refused until
a snapshot makes every acknowledged write durable again - [durability.md](durability.md),
"When the log cannot keep up"), `failed` (its
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
nothing.  One apply thread of several (`apply_threads`, above) wedged
with records queued on it is reported the same way.  A stalled *master* is replaced: its claim is not repeated, the
others do not consider it in the election, and it steps down when it
returns and hears the higher term.  Only `ready` (and `draining`, which still answers for what it
holds) is selected by libperfd; a client or peer built before a state
existed reads it as "not ready".
