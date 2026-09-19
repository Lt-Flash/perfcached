# Changelog

perfcached follows [semantic versioning](https://semver.org): the patch
digit is a fix inside a line, the minor digit is a real capability step.
Releases are tags, and **promises are made from tags** — the wire
dialect, the metric names and the compatibility rules described in the
README hold for a tagged release, not for whatever `master` says today.

Measured numbers in the README move with a release. A reader holding an
older page can therefore tell which build produced a claim, which was
not true while results were rewritten in place.

## Unreleased

## 0.4.0-rc7 — 2026-09-19

rc6 with the status page's numbers made unambiguous: numbers of five
digits or more are grouped with a narrow space ("10 134") instead of the
browser's locale separator, which read as a decimal point, and
microseconds are written "μs".  Only the page changed; the daemon's
behaviour, wire, config and metrics are rc6's.  libperfd is unchanged
(0.2.10).  Everything under 0.4.0-rc6 below is in rc7.

### The status page groups numbers with a space, and writes μs

The page grouped large numbers with the browser's locale, so 10134 read
"10,134" in an English browser, easy to take for ten point something,
and "10.134" in a German one.  Numbers of five digits or more are now
grouped with a narrow space ("10 134", "1 234 567"), so a point or comma
in a number is only ever a decimal point.  Four-digit numbers are left
whole.  Microseconds are written "μs" rather than "us", and the last
RDB save's duration switches to ms or s like the other durations.

## 0.4.0-rc6 — 2026-09-19

rc5 with four operator-facing changes and a security fix:
- the commands clients sent that perfcached does not implement are
  listed fleet-wide, on `/stats` and the page;
- `CLIENT SETINFO` is implemented;
- a node restarted in an `interested` pub/sub fleet no longer filters
  out a subscription made while it rejoined;
- the dashboard's clients panel, cluster-plane cards and slow-call cards
  are fixed.

**Security:** the slow log recorded `AUTH`'s arguments, so a slow `AUTH`
stored the RESP password in `SLOWLOG GET`, `/stats` and the page.  It
now stores `(redacted)`; clear any slow log that may hold an `AUTH`, and
consider rotating the password (below).

Five more parts of the cluster plane moved into modules with their own
tests, with no wire, config, metric or log-message change.

**Upgrading from rc5:** a member still on rc5 ignores the unknown-command
tables the others send, so the fleet-wide card counts it as not
reporting until it runs rc6.  libperfd is unchanged (0.2.10).
Everything under 0.4.0-rc5 below is in rc6.

### SECURITY: the slow log no longer records the RESP password

The slow log keeps the first arguments of every command slower than
`slowlog_usec`.  It kept them for `AUTH` too, so an `AUTH` that ran
slow stored the RESP password, or a username and password, in three
places:
- `SLOWLOG GET`;
- `/stats`;
- the dashboard's slow log card.

A mistyped password was stored the same way.  `HELLO ... AUTH <user>
<password>` stored the username, and the password too if the version
was left out.  Those arguments are now stored as `(redacted)`, with the
argument count kept, before the entry is written.  The native and JSON
doors never recorded arguments beyond the collection and key.

If a node's slow log may have caught an `AUTH` - `slowlog_usec = 0`
logs every command, and the default 10 ms threshold catches a slow
one - clear it with `SLOWLOG RESET` after upgrading, and consider
rotating the `[secrets] resp` password.  `test/slowredacttest.sh`
failed 9 of its 18 checks before the fix and passes all 18 with it.

### The dashboard's slow log and slowest-calls cards fit, and show the slow calls

A slow log row is its command line, and a long key has no space to wrap
at, so it pushed the call's time off the card.  Those rows now end in an
ellipsis, with the whole command, the client and the time in the hover,
and a duration reads as "10.1 ms" rather than "10,103 us".  The
slowest-calls card showed each command's mean and p99, and neither can
reveal a single slow call.  On a node that logged a 10.1 ms `SET`, the
mean over 64,000 SETs read 7.6 us.  The card now shows each command's
p99 and its slowest call, taken from the latency histogram the daemon
already kept, and sorts by the slowest call.  The command with a
millisecond outlier is listed first, with "max <= 16.4 ms" (the
histogram's 8.2-16.4 ms bucket).  The mean stays in the by-calls card.

### Five more parts of the cluster plane are modules with their own tests

`cluster.c` is 7,071 lines, 1,450 fewer.  Five more of its parts moved
out behind an API, each with a unit test that runs in milliseconds
with no socket and no second daemon:
- the send path (`clsend`);
- the fleet's per-collection figures (`clfleet`);
- the workers' pull, forward and write-push entry points (`clwork`);
- the pub/sub relay plane (`clps`);
- spread's decisions (`clspread`): when a surplus copy may be
  reclaimed, when the holder set has changed, and when the fleet is
  short of `replicas`.

The rule that a surplus copy is reclaimed only on the fleet's shared
map, never on one node's own view of the fleet, is now one of those
assertions.  No wire, config, metric or log-message change.

### A node restarted in an `interested` fleet no longer filters out a new subscription

With `pubsub_relay = interested`, a node that restarted could ask a peer
for its subscription state before the fleet had assigned it an id, and
the peer answered - matched by the empty slot the node's own departure
had left at its address - although it did not yet count the node as a
member and so sent it none of its later subscription updates.  The
restarted node then filtered relays against that state: a channel the
peer subscribed to in the second or so before it heard the restarted
node's next heartbeat was not relayed to it.  A request from a node with
no id is now ignored; the node asks again once it has one, which is when
the peer also starts sending it updates.  `relayinteresttest` failed this
step intermittently (1 of 3 CI runs; 6 of 9 on two pinned CPUs), and
passes it 60 of 60 with the fix.

### The dashboard's clients panel shows its clients

Since the panel arrived in 0.4.0-rc1 (S160), opening it left the table
empty on every node: its two functions were declared inside the stats
redraw, where neither the click that opens the panel nor the 3-second
poll could reach them, so each call failed with a reference error and
the table was never drawn.  They are top level now.  `clientstest`
checks their placement; it fails against the page as it was.

A connection that has sent nothing since it connected - most often one
of the standby connections libperfd keeps to every member for failover -
now reads "idle" in the dialect column (the dialect is learned from the
first request) with a dash for its name and last command, and the count
of such rows is in the panel's summary line.

### `CLIENT SETINFO` is implemented

The first command the new unknown-command card caught on the staging
fleet: 66 times in five minutes, from a client library announcing itself
on connect through HAProxy.  `CLIENT SETINFO LIB-NAME <name>` and
`CLIENT SETINFO LIB-VER <version>` now answer `OK` (the attribute is
case-insensitive; a value with a space, newline or other unprintable byte
is refused, as are other attributes and the wrong argument count), and
the values are kept on the connection: `CLIENT LIST` carries
`lib-name=` and `lib-ver=`, `/clients` carries `lib_name` and `lib_ver`,
and the dashboard's clients panel shows the library beside the name and
filters on it - behind a proxy that hides the client's address, that is
what says which client a connection is.  `clientstest` covers it.

### The commands clients sent that perfcached does not implement are listed, fleet-wide

Every door answered an unknown command and kept nothing, so a client
library starting to use a command or subcommand perfcached lacks was
found only in that client's error log.  They are now counted by name:
RESP commands and unsupported subcommands (`CLIENT TRACKING`, `CONFIG
SET`, ...), native JSON methods and binary verbs, each with its count,
first and last sighting and the last client's address and name.  Only
the name is kept, never an argument; the table holds 64 names and counts
the rest in `other`; names are cut to 32 bytes and made printable; an
unknown command before AUTH is counted but not named.  Every member
sends its table to the others, so `/stats` `unknown_commands` - and the
new "unknown commands" card beside the slow log - is the fleet's list
on every node, with this node's share beside each count.  The first
sighting of each name logs one NOTICE on the node that saw it.
`/metrics` gains `perfcached_commands_unknown_total{dialect}` and
`perfcached_commands_unknown_preauth_total`.  A member still on 0.4.0-rc5
does not send its table, and the card says how many members report.
`unknowntest` and `unknownfleettest` (in `check-fast`) and `clunktest`
cover it; the two suites fail against a build without it (0.4.0-rc5 has no
`unknown_commands`; run red on an rc4 build).

### The page's cluster-plane cards are laid out by size

The cards under "cluster plane" were written in a fixed order, so one
long card (a filled slow log, the durability cards) left the shorter
cards in its row stretched around empty space.  After every redraw the
page now measures each card and orders them tallest first, so a row
holds cards of one height; cards of equal height keep their usual order,
and a card moves only when its own height changes.  `httptest` asserts
it; the check fails against the 0.4.0-rc5 page.

## 0.4.0-rc5 — 2026-09-18

rc4 with the fleet's collection figures corrected.  The status page
added every member's collection figures together, so under eager -
where every member holds every entry - 2,043 entries on each of three
nodes read 6,129 fleet-wide, and stores and expired were multiplied the
same way.  The daemon now computes them: `/stats` collection rows on a
clustered node carry `fleet`, with entries and expired following the
mode's copy rule and client hits, misses, stores and removes counted
once; `perfcli stats` shows it and the page renders it.  **Upgrading
from rc4:** each member's `stores` and `removes` in `/members` now count
client writes only; during a rolling upgrade older members still send
every store, so the fleet's stores figure is mixed until every member
runs rc5.  The page's fleet cells no longer run their two figures
together, and its scrollbars follow the dark theme.  `colsfleettest` no
longer takes a membership notification for its reply, which failed
GitHub's master runs of rc3 and rc4.  libperfd is unchanged (0.2.10).
Everything under 0.4.0-rc4 below is in rc5.

### The page's collection table no longer runs the fleet total into this node's share

Since 0.4.0-rc2 (S163) the entries, hits, misses, stores and expired
cells show the fleet's total with this node's share under it, but the
rule that stacks the two figures only reached the hit-rate cell.  In
every other cell they rendered inline as one number: 6,129 entries
fleet-wide with 2,043 here read "6,1292,043 here".  The stacking rule
now applies to any table cell.  `httptest` (in `check-fast`) asserts it; the
check fails against the 0.4.0-rc4 page.

### Fleet collection figures come from the daemon and count entries, not copies

The status page's collection table added every member's figures
together (S163), so under eager - where every member holds every entry -
2,043 entries on each of three nodes read 6,129 fleet-wide.  Stores and
expired were inflated the same way: a node counts a replicated apply as a
store, and each member's sweep counts its own copy's expiry.

`/stats` collection rows on a clustered node now carry `fleet`: `basis`,
`members`, `reporting`, `entries`, `copies`, `expired`, `hits`, `misses`,
`stores`, `removes`.  Entries and expired follow the mode: eager takes
the fullest member, spread the copies over K, store the fullest member
as a lower bound (a pulled entry is kept where it was read), shard and
proxy the sum.  Hits, misses, stores and removes are client operations,
each counted once on the node the client reached, and summed.  Rows also
gain `stores_client` and `removes_client`, and the gossiped per-member
`stores` and `removes` (in `/members`) are now client-only; during a
rolling upgrade, older members still send every store.  The page shows
the daemon's figures with the rule in the entries hover, and `perfcli
stats` shows the same block.  `fleetcolstest` (in `check-fast`) runs a
three-node eager fleet against it; it fails against 0.4.0-rc4.

In store mode a pulled entry is kept by a client worker, so there the
client store count includes kept pulls.

### The page's scrollbars follow its dark theme

The page switches to its dark palette with the system theme but never
told the browser, so the browser's own parts - the horizontal scrollbar
under a wide table, the connection filter - stayed light on the dark
page.  It now declares `color-scheme: light dark`; `httptest` asserts
it.

## 0.4.0-rc4 — 2026-09-17

rc3 with a crash fixed and pub/sub delivery reworked.  **A standalone
daemon with no `[secrets] cluster` crashed** (0.3.8-rc1 through
0.4.0-rc3) at startup whenever a native door was encryption-required,
on `-C` with a RESP password, and on `-E` - fixed, and such a door now
refuses the cluster principal instead of accepting an all-zero key.
Pub/sub frames are written once per worker turn instead of once per
message, a notification is built once per message instead of once per
subscriber, and deliveries are counted per worker: on one host with the
same client, one worker delivers at Redis 8's per-core rate and four
deliver 3.6x its fan-out (`bench/RESULTS.md`).  **Upgrading from rc3:**
a publishing connection that fills a worker's queue to half its cap is
now paused until the queue drains, where rc3 dropped the messages after
PUBLISH had counted them; `publish_paused` on `/stats` counts the pauses.
Relayed messages stay at-most-once and are counted in `relay_lost`.
libperfd is unchanged (0.2.10).  Everything under 0.4.0-rc3 below is in
rc4.

### Pub/sub across two nodes against Redis Cluster, and through Noise

`bench/rpsrelay.sh` (with `bench/rpsrelay-node.sh` on each host) runs
publishers on one node and subscribers on another from a third host -
perfcached relaying, or a two-master Redis Cluster - and `rpsbench -B/-b`
sends subscribers to a different address than publishers.
`bench/psnoise.sh` compares the native door plaintext and under Noise.
Hosts, runtimes and interfaces are all parameters.  Readings in
`bench/RESULTS.md`, rows in `bench/results/`; in short, on 7253754:

- at 20k publishes a second both are lossless with one subscriber, and
  perfcached's p50 is lower (0.48-0.50 ms against 0.64 ms); with 64
  subscribers its p99 is higher (15-20 ms against 2.4-2.8 ms) and its
  relay lost 0.09-1.6% in some runs - intermittent, and as often on
  889e0f6;
- flat out, Redis never drops on the bus and falls seconds behind
  (p50 2.7 s with one subscriber, 7.6-8.4 s with 64) until subscribers
  hit their output limit; perfcached delivers at milliseconds (6.2M
  deliveries a second to 64) and loses 0.3-5% at its relay, counted in
  `relay_lost` - at-most-once, as designed;
- Noise costs ~20% of throughput with one subscriber, 17-20% more CPU
  at a fixed rate with 64, and half the throughput with 1 KB payloads.

A fan-out latency is only as good as the driver's receive path: with one
receive queue and no RPS, four workers' packet rate saturated it (p50 9.7
ms against 1.06 ms with RPS).  The script warns, and `DRIVER_RPS` spreads
it for the run and restores it.

### Two costs of delivering to many subscribers, measured and removed

A profile of 64 subscribers fed flat out (223, four workers) named two
costs that grow with every delivery.  On the RESP door a quarter of the
server's CPU (24%) was one shared counter: every worker added 1 to the
same `delivered` total for every message it wrote, and that line of
memory bounced between the workers.  On the native doors 44% went to
building the JSON notification - once for every subscriber, although it
is the same bytes for all of them.

Deliveries are now counted per worker, on a cache line of its own, one
addition per message, and `/stats` sums them.  The notification is built
by the first native subscriber a delivery meets and reused by the rest,
separately for the message and for each pattern it matched.  After: the
counter no longer appears in the RESP profile, and the JSON builders
fall from 44% to 17% of the native door's.  The engine in isolation
(`psengine`, four workers) delivers 2.37-3.16M messages a second, from
1.95-2.16M.  On the rig, flat out, the gain stays inside run-to-run
spread, and at paced rates the per-turn cost of waking and flushing
small batches dominates either way.  `psnotifytest` (in `check-fast`)
pins what sharing must not change - text and binary subscribers on a
channel and on two patterns, each getting its own body, for plain,
escaped and binary payloads; a body shared across patterns, or between
the channel and a pattern, fails it.

### A publisher that outruns a worker is paused, not dropped

A publish for subscribers on another worker waits in that worker's
queue, and past the queue's cap (`pubsub_queue_mb`) it was dropped and
counted - after PUBLISH had already answered every receiver.  With push
batching in, one worker's delivery is fast, but enough publishers on the
others still outran it: on the 223 rig, at the default 16 MB, 32
closed-loop publishers of 1 KB into one subscriber lost 66% of what they
were told was delivered, and every shape tried lost something, with no
subscriber closed and no sign to either side but the counter.

Now a door's publish that leaves a worker's queue at half its cap pauses
the publishing connection: the rest of its input waits, nothing more is
read, and TCP holds the client, until the queue is below a quarter - the
draining worker wakes the waiting one when it passes that mark, and a
50 ms timer asks again in case a wake is missed.  Other connections on
the worker are served meanwhile.  `publish_paused` on `/stats` and
`perfcached_pubsub_publish_paused_total` count the pauses.  Messages
from peers and keyspace events have no connection to pause and keep the
counted drop at the cap.

`pubsubpressuretest` (in `check-fast`, driving `rpsbench`): four
workers, a 1 MB cap, 32 publishers of 1 KB into one subscriber and 16
into four, flat out for 3 s.  Before: 1,648,310 and 2,141,532 messages
lost at the queue, every one counted by PUBLISH as delivered.  Now: none
lost, no gaps, every counted receiver served, 36,461 and 29,363 pauses,
other connections answering PING within 14 ms - and more delivered, not
less: 313k and 906k deliveries a second against 249k and 741k.  With the
reads left running while paused, 12,157 were still dropped; with the
drain's wake removed nothing is lost, but throughput falls 13-22% to
the timer.

### Pub/sub against Redis through one client (rpsbench)

`bench/rpsbench.c` (`make rpsbench`) is a RESP pub/sub driver:
publishers paced or closed-loop, subscribers that measure latency and
count gaps from a send time and sequence in every payload, and the
server's CPU and the driver's own beside every figure, since fan-out
saturates the client first.  `bench/rpscmp.sh` runs Redis and perfcached
in containers pinned to the same cores against it.  On one 16-vCPU host,
e9b7fdb: with 64 subscribers four workers deliver 12.9M messages a
second where Redis 8 delivers 3.5-3.7M on its thread, one worker 4.8M;
one subscriber flat out, one worker 533-554k publishes a second against
Redis's 572-574k.  Nothing lost in any cell.  The readings and what they
do not cover (loopback, one node) are in `bench/RESULTS.md`.

### Pub/sub pushes are written once a turn, not once a message

Measured against Redis 8 on one host (223, both pinned to the same eight
cores, the same RESP client): a worker wrote every delivered message the
moment it was staged, so 350,064 deliveries took 467,871 write(2) calls
and the worker that owned a subscriber topped out near 120k deliveries
a second - Redis delivered 580k on one core, and 3.7M a second to 64
subscribers.  With several workers the owner was the ceiling, and what
the others accepted beyond it overflowed its 16 MB queue.

A push now appends its frame and marks the connection; the worker
writes each marked connection once when its turn is over, or at once
past 64 KB of staged output, so a burst meets the slow-consumer cap
only on output the socket refused.  A worker's wakes to other workers
wait for the end of its turn the same way, so the target drains the
turn's messages as one batch instead of waking for one or two - except
that a turn which has queued a drain batch or 256 KB for a worker wakes
it at once: waiting for the end of one long pipelined turn let that
turn fill the queue's cap before the target started (9,950 of 12,000
4 KB publishes dropped, to a subscriber that would have taken them).
Pushes from threads that are not workers still write at once.

`pushbatchtest` (in `check-fast`) counts the daemon's own write syscalls
across a pipelined burst of 2,000 PUBLISH: one worker and one subscriber
3 writes (was 2,001), two workers and four subscribers - proven by the
per-thread counters to span both - 11-14 writes for 8,000 deliveries
(was 8,021; 1,164-1,795 with the wakes not deferred), every message in
order; and a 15 MB burst to subscribers that keep up on the other
worker arrives whole (1,981 of 4,000 dropped without the early wake).

### A standalone daemon without a cluster secret starts

0.3.8-rc1 made `[secrets] cluster` optional on a daemon with no
`[cluster]` section (S158), but three readers still took it as set, and
through 0.4.0-rc3 such a daemon crashed (SIGSEGV): at startup whenever a
native door is encryption-required - any `tcp` door off loopback, or
`plaintext = never`; on `-C` with a `[secrets] resp` password; and on
`-E`.  0.3.7 and earlier refused the configuration instead.  Found
starting a container for the Redis pub/sub comparison.

All three now cope with its absence, and `-E` prints `cluster = (not
set)`.  Deriving nothing would leave the cluster key all zeros, and a
native door also accepts the cluster principal - so without a cluster
secret the door now refuses that principal outright (a WARNING per
attempt) rather than hand-shake with a key anyone can present.  With a
cluster secret nothing changes.  `standalonetest` (in `check-fast`):
`-E`, a resp password, and a live encryption-required door with no
cluster secret - the client key hand-shakes, a wrong client key and the
cluster principal (all-zero key, or a derived one) are refused; with a
cluster secret the cluster principal hand-shakes and the all-zero key is
still refused.  It failed 3 checks on the unfixed build, and 1 (the
all-zero handshake) with the refusal removed.

## 0.4.0-rc3 — 2026-09-17

0.4.0-rc2 went red on its tag pipeline (107935), in `check` and
`check-asan`: `loadtest` started a second fleet on the cluster port + 1,
which the first fleet's nodes now hold for pub/sub relays (PS11), so the
second fleet could not bind its cluster socket; `failovertest` and
`statedirtest` had the same shape.  rc3 is rc2 with those three suites'
ports moved, and the daemon and libperfd unchanged - plus the upgrade
note rc2 should have carried: **two fleets on one host need cluster
ports at least 2 apart**, because every node also binds the port above
its cluster port for relays (or give one fleet `pubsub_rx_threads = 0`).
It also carries the two export fixes that let rc2's public snapshot go
out: a real address in an example in `PERFCLI-COMMANDS.md`, and that
file admitted to the public export.  Everything under 0.4.0-rc2 below is
in rc3.

## 0.4.0-rc2 — 2026-09-17

Red on its tag pipeline (107935) - superseded by 0.4.0-rc3 above.

The second candidate of the 0.4.0 line, and the first tag with the fix
for 0.4.0-rc1's pattern matcher: a `PSUBSCRIBE` pattern could stall a
node or read past its buffer (SECURITY, below) - do not run rc1 where a
client is not trusted.  Beyond that fix: the pub/sub engine without its
global lock and with a pattern index (PS9, PS10); relays on a port and
threads of their own, and bounded worker queues (PS11); relaying only
what a node wants, opt-in (PS12); pushes over UDP for native
connections, in the daemon and in libperfd (PS5); command statistics,
the clients strip and the fleet's collection cards (S159, S160, S163);
an idle metrics connection closed on a quiet worker.  Measured on the
two-host rig and soaked for an hour through restarts and a firewall on
the relay port (below).

Upgrading from rc1: relays now arrive on the cluster port + 1 over UDP
(`[cluster] pubsub_port`, `pubsub_rx_threads`) - open it between nodes;
a node that cannot reach it falls back to the cluster port by itself.
Native doors bind their own port over UDP for pushes (`[listen]
pubsub_udp = yes`), and a worker's queue is capped at 16 MB
(`[daemon] pubsub_queue_mb`).  `[cluster] pubsub_relay` defaults to
`all`, rc1's behaviour.  The cluster wire gains datagram types 26 to 30,
which an older peer ignores, and a heartbeat field after the collection
block, which an older build reads past.  The native doors gain
`pubsub_udp`, `pubsub_udp_confirm` and `pubsub_udp_ack`; libperfd is
0.2.10.  Also in the tree and documented in their own files: the
Asterisk dialplan functions in `contrib/asterisk` (func_perfd) and
`PERFCLI-COMMANDS.md`.

### A one-hour pub/sub soak on the rig (pssoak)

`bench/pssoak.c` (`make pssoak`) drives pub/sub over libperfd async
handles for as long as asked: publishers, subscribers - exact channels,
patterns, and UDP pushes - and subscribe/unsubscribe churn, each
reconnecting and subscribing again after its node restarts, each
printing its counters every 10 s.  Payloads carry a sequence per
publisher connection and channel, so a subscriber counts real gaps
apart from reorders.

One hour on 4cc7e17: node A on 222 (`pubsub_relay = all`), node B on 223
(`interested`), every client on 190; 72 million publishes at 20,000 a
second, subscribers of every kind on both nodes, churn on both; B
restarted at 15 and 45 minutes, and a firewall dropped B's relay port for
20 s at 30 minutes.  Outside those windows (10 s before to 90 s after
each) and the first minute: no gap, reorder, UDP gap or duplicate, prune
or publish error on any client, and no relay lost or duplicated, message
dropped at a queue, slow subscriber closed or UDP send refused on either
node.  Inside them the counters did move - 24,321 gaps on B's pattern
subscribers across the restarts - so the clean figures are not an
instrument that could not see.  Node A's memory held at 9,724 kB for the
55 steady minutes; each of B's three processes grew 0.3-0.6%.  Every
SIGTERM, restarts and the final stop, ended the process in 156-208 ms.
No ERROR or CRIT line; node A's two warnings sit inside windows - the
relay fallback during the firewall, and a cluster control-state sync
warning during B's second restart.

### libperfd: an async handle keeps a UDP probe that beat the reply

The daemon sends a UDP push stream's first probe while it answers
`pubsub_udp`, so an event loop can read the probe before the reply that
names the stream.  An async handle rejected that probe as belonging to
no stream, and the stream became active only with the next probe, a
second later - seen in the rig soak as `rejected` on every UDP
subscriber's first connection and each reconnection.  The handle now
keeps such a probe and takes it once the reply arrives.  The same change
resets a handle's stream state without relying on the order of its
fields, and removes an assignment inside a condition clang-tidy flagged.

`udppushcli` reads the socket before the reply on purpose: with 0.2.10 as
committed the stream is active after 1,002 ms with one probe rejected,
with the fix at once and none rejected (`udppushtest` 51 of 51).

### An idle metrics connection is closed on a quiet worker too

A connection to the `http` door that never sends its request is closed
after `http_timeout` seconds (S46) - by a sweep the worker runs when it
wakes.  The worker chose how long to wait from the count that sweep had
taken, before the turn's accepts, so an idle HTTP connection accepted on
a worker with nothing else to do was not counted: the worker waited with
no timeout and the connection stayed open until some other event came.
The wait now reads a live per-worker count, as the UDP push streams'
does.  `httpidletest` (in `check-fast`), one worker, an idle native
connection opened first and a finished request as the control: the idle
HTTP connection is closed 2.0 s after a 2 s timeout (three runs); the
build before keeps it open past 6 s (two runs).

### Pub/sub pushes over UDP in libperfd (PS5)

libperfd 0.2.10 takes a handle's pub/sub deliveries over UDP:

    perfd_pubsub_udp(p, port, timeout_ms)   0 = any free port
    perfd_pubsub_udp_fd(p)                  the socket to watch
    perfd_pubsub_udp_ready(p)               on readability, and >= 1/s
    perfd_pubsub_udp_off(p)
    perfd_pubsub_udp_stats(p, &s)           active, pruned, received,
                                            gaps, duplicates, rejected, acks

- **The same hook, the same JSON.**  A datagram is opened, checked and
  turned into exactly the notification the connection would have carried
  - `message` or `pmessage`, the payload base64 with `"enc"` when it is
  not clean UTF-8 - and handed to `perfd_set_notify`'s hook.  Deliveries
  too large for a datagram still come over the connection, to the same
  hook.
- **Blocking and async handles.**  On a blocking handle the call waits
  for the probe (3 s by default), confirms it and returns; no probe means
  UDP does not reach the client, and deliveries stay on the connection.
  On an async handle the call submits the request and
  `perfd_pubsub_udp_ready` takes the probe and confirms from the loop.
- **Checked before it counts.**  A datagram must come from the door's
  address and port, carry this stream and open under its key before its
  sequence number is trusted; duplicates are dropped, gaps counted when
  they leave a 64-message window.  The daemon's confirmation now answers
  the sequence messages follow (`{"udp":true,"seq":N}`), so probes the
  client never saw are not counted as gaps.
- **Acknowledged, and told.**  `perfd_pubsub_udp_ready` acknowledges every
  256 messages or 5 s, on the timer when nothing arrives.  A
  `pubsub_udp_pruned` notification closes the handle's socket before the
  hook sees it; the handle does not re-subscribe.  A failover ends the
  stream as well.
- `lib/perfd_push.h` joins the MIT export (`tools/sync-libperfd.sh`).

Checks: `udppushcli` (libperfd, in `udppushtest`): 110 deliveries through
UDP to the hook in order, the hook's JSON byte-for-byte as the
connection's, a binary payload, a 2,500-byte one over the connection, a
stranger's datagram rejected, a 3,000-message burst into a 2 KB socket
counted exactly (delivered + gaps = sent), an async handle through its
own loop, 17 s idle kept alive by the timer acknowledgements on both
handle kinds while a handle that never acknowledged is pruned - hook
told, socket closed, subscriptions gone, connection still usable - and a
door with `pubsub_udp = no` refusing.  Three broken libraries each fail
it: a window that never counts gaps, acknowledgements only every 256
messages (the idle handles are pruned under it), and a pruned notification
not acted on.  Not covered: a replayed datagram - the test cannot send
from the daemon's bound port - so duplicate rejection is exercised only
through the window the gap burst drives.

### Pub/sub pushes over UDP, the daemon's side (PS5)

A native connection may now take its pub/sub deliveries as sealed UDP
datagrams instead of on the connection - the 08-29 directive, with
guaranteed detection rather than delivery.  One stream per connection,
covering all its subscriptions:

    pubsub_udp {"port":N}           -> {"stream":"<16 hex>","key":"<64 hex>",
                                        "max_datagram":1400,"ack_every":256,
                                        "ack_ms":5000,"prune_ms":15000}
    pubsub_udp {"port":0}           -> {"udp":false}    (stops the stream)
    pubsub_udp_confirm {"cookie"}   -> {"udp":true}
    pubsub_udp_ack {"seq":N}        -> {"acked":N}
    notification pubsub_udp_pruned  {"stream","reason":"no ack"|"no progress"}

- **Only to the connection's own address.**  The port is the client's,
  the address never is: it is the TCP peer's.  A request that names a
  host is sent to the peer regardless.
- **From the door's address and port.**  Each native TCP door binds the
  same address and port over UDP at start (`[listen] pubsub_udp = yes`,
  the default; a port held elsewhere turns it off for that door with a
  warning), so one firewall rule admits the datagrams.
- **Nothing before the path is proven.**  The daemon sends up to three
  probes a second apart, each carrying a cookie sealed under the stream's
  key; deliveries stay on TCP until the client echoes the cookie over the
  connection, and a stream not confirmed in 10 s is dropped.
- **Sealed.**  ChaCha20-Poly1305 under a random per-stream key sent over
  the connection, the nonce the stream's sequence number, the header
  authenticated.  No retransmit: the client counts gaps.
- **Pruned when the acknowledgements stop.**  15 s without one, or 15 s of
  sending past one that stopped moving (UDP broken while TCP still carries
  the acknowledgements): every subscription on the connection is dropped
  and the connection is told why.  It stays open and may subscribe again.
- Deliveries over 1,400 bytes stay on the connection.
- `stats.pubsub.udp` (streams, probes, confirmed, expired, pushed,
  oversized_to_tcp, send_errors, acks, pruned by reason), `/metrics`, and
  two rows on the pub/sub card.

A bug found on the way: a worker chose how long to wait from a stream
count taken before it handled the turn's events, so a stream opened in
that turn did not shorten the wait and its second and third probes
waited for some other event.  The test caught it only on a quiet daemon
and now checks the probes there.

Checks: `psudptest` (in `check-fast`) walks a stream through probes,
expiry, an idle stream kept by its timer acknowledgements, a minute of
traffic acknowledged a little behind without a prune, and a stall pruned
15 s after the first unacknowledged send; each of four broken variants
of `src/psudp.h` fails it.  `udppushtest` (in `check-fast`) opens the
daemon's datagrams with libsodium: the probe's source address and port,
its seal, the cookie gate and a wrong cookie, 60 deliveries in sequence
and none on TCP, a 2,000-byte one on TCP, an acknowledgement beyond
what was sent refused, a connection from 127.0.0.2 probed at 127.0.0.2
only, both prunes and the dropped subscriptions, expiry, probes on an
idle daemon, port 0, and a door with `pubsub_udp = no`.  The PS12 build
has no such setting; the build with the stale wait fails the idle check.

libperfd's side - its own socket, the probe, acknowledgements and gap
counting, delivered to the notify hook - is the next change.

### Relaying only what a node wants (PS12)

Every publish was relayed to every other node, whether or not anything
there subscribed.  A node set to `[cluster] pubsub_relay = interested`
is now relayed only the publishes that may match a subscription it
holds.  The default stays `all`, and the setting belongs to the
receiving node: a node on `all`, or an older build, is sent everything
as before, and a fleet can move one node at a time.

- **What travels.**  A node's exact channels go to its peers as a
  32 KiB Bloom filter (four probes; about 0.04% of other channels pass at
  10,000 subscribed names, measured 0.041%), its patterns as strings,
  matched on the sending side through the same literal-prefix index the
  engine uses.  Its first subscription to a name is sent at once - the
  filter's probes or the pattern.  Losing the last subscription sends
  nothing: the node rebuilds its filter at most every 30 s, and until then
  a peer relays a little extra, which is the safe direction.
- **Staying current.**  The heartbeat carries an interest version -
  process epoch, rebuild count, update count.  A sender filters to a peer
  only while every update up to that count has reached it; with no full
  state yet, another epoch or an update missing for a second it relays
  everything to that peer and asks for the full state, one datagram of the
  filter plus the patterns.  Patterns that do not fit beside the filter
  make the node ask for everything.  A rebuild does not stop filtering,
  since the older filter is a superset; the sender just asks for the new
  one.
- **Loss stays exact.**  A filtered peer is relayed on counters of its
  own, in a sequence space of their own, so a publish it was not sent is
  never counted as lost; that is one extra seal per such peer per relay,
  about 0.7 us on 222.  Peers on `all` keep the single shared seal.
- **The window.**  A publish on another node in the moment after a first
  subscription, before its update lands, is not relayed to that node.
- `/stats` carries `relay_mode`, `interest_version` (hex: epoch, rebuild,
  updates), `relay_skipped`, the peers filtered and the peers sent
  everything, and updates and resyncs sent and received; `/members` shows
  for each node whether relays to it are `all`, `filtered` or
  `broadcast`; `/metrics` and the relay card carry the same.

Checks: `psinteresttest` (in `check-fast`) runs the filter (no false
negatives in 10,000 names, false positives at the expected rate) and a
sender's view of a peer: updates out of order, a lost one, a full state
older than what is held refused, a new epoch, a rebuild.
`relayinteresttest` (in `check-fast`) runs three nodes, two interested
and one on `all`: an exact channel and a pattern each relayed only where
subscribed, 400 relays counted as skipped and none as lost, the node on
`all` still relayed everything, a new subscription relayed 100 ms later
by an update, a restarted sender resyncing from both interested peers, a
channel dropped after `UNSUBSCRIBE` once the node rebuilt, and a restarted
receiver's old patterns gone with its old process.  Four broken senders
each fail it: no filtering (7 checks), counters shared with the other
peers (156 relays counted lost on each filtered peer - the first version
of the test published the three channels in blocks, so the gaps fell
after a peer's last relay and went uncounted, and that sender passed),
no full-state requests (11) and updates ignored (7); the PS11 build
refuses the setting.

What it costs and saves the sender, on the two-host rig (node A on 222
publishing, node B on 223, driver on 190, 64-byte payloads, 10 s a cell,
two runs each), as node A's CPU per publish - the whole node's work,
the client's sealed request included:

| offered | B on `all` | B `interested`, subscribed | B `interested`, no match |
|---|---|---|---|
| 100k/s | 22.3, 22.0 us | 20.7, 20.2 us | 10.7, 11.8 us |
| 137-198k/s reached | 19.9, 18.1 us | 19.3, 18.4 us | 8.8, 9.5 us |

A relay to one peer is about half of the sender's work per publish, and
a filtered peer that wants nothing gives all of it back.  Sending to a
subscribed filtered peer - its own counter and seal after the filter
read - is within the noise of the shared path with one peer; the rig has
one peer, so the extra seal each further filtered peer adds (~0.7 us,
measured apart) is not measured here.  Above 100k/s node B's two receive
threads lost relays at its socket buffer in every mode (up to 27%), as
they did in the PS11 runs.
### Pub/sub relays get a port and threads of their own (PS11), and worker queues a bound

Every relayed publish arrived on the cluster socket, read by the one
thread that also reads heartbeats, pulls and replication.  The PS7 bench
found that socket's buffer overflowing somewhere between 40k and 100k
relayed messages a second per receiving node.

- **A relay port.**  `[cluster] pubsub_port`, by default the cluster port
  + 1, is read by `pubsub_rx_threads` threads (default 2, one
  `SO_REUSEPORT` socket each).  Each publishing worker sends from a socket
  of its own, so one sender's lane lands on one receive thread and stays
  in order.  `pubsub_rx_threads = 0` keeps relays on the cluster socket.
  A firewall between nodes must admit both UDP ports.
- **Used only once it answers.**  A node advertises its relay port on the
  heartbeat, in a field after the collection block, so an older build
  reads the frame as before (checked against the previous master, which
  first misread the field when it sat inside the block).  A sender relays
  to the port only after a sealed probe there has been answered,
  re-probes every second, and goes back to the cluster socket after three
  unanswered probes, about 3.5 s; each change is logged once.  A port that
  never answers gets one warning after 10 s that names a firewall as the
  likely cause, a node that comes back without a relay port is a notice
  rather than an outage, and a relay port another process holds is
  refused at start with a warning, the node then advertising none.
- **Worker queues are bounded.**  A message for a subscriber on another
  worker waits in that worker's queue.  The queue had no limit and its
  worker emptied it in one pass, so on the rig a node took 12.9 s to exit
  after a 200k/s burst.  A drain now takes at most 1,024 messages and
  wakes its worker again for the rest, and `[daemon] pubsub_queue_mb`
  (default 16, 0 = no cap) bounds the bytes waiting for one worker; past
  it a message is dropped and counted as `queue_dropped`, with
  `queue_bytes` beside it.  The same node exits in 104-258 ms, including
  in the middle of a burst.
- `/stats` and `/metrics` carry the relay port and threads, the peers
  relayed to directly, relays sent by path and datagrams the receive
  threads read; `/members` shows each node's relay port and whether
  relays to it go there; the pub/sub card shows the queue figures.

Two-host rig, one subscriber on node B (223), node A on 222, driver on
190, 64-byte payloads, 10 s per cell, relays A sent that B never received:

| offered | B on the cluster socket | B on its relay port, 2 threads |
|---|---|---|
| 100k/s, two runs | 0 and 42 | 0 and 6 |
| 165-183k/s, two runs | 27.9% and 19.2%, all at B's socket buffer | 8.3% and 0.32% |

In the 0.32% run B's socket buffer dropped nothing.  One more cell at
148k/s read every drop counter on both hosts: A's kernel accepted every
relay, B's UDP layer received 2,513 fewer (0.17%), and no counter in
either guest - socket, queueing discipline, link, softnet, IP - saw them
go, so they were lost between the two virtual machines.  Above 100k/s one
subscriber's worker could not keep up and the cap dropped 230k-580k
messages a cell; at 100k/s on the relay port it dropped 3,103 and 11,957.

With a firewall dropping B's relay port for 40 s at 2,000 publishes/s,
node A logged the fallback 4 s in and went back to the port 2 s after the
rule was removed; 6,857 relays were lost (4.9% of the run).  With the
first timings - re-probe every 10 s, fall back after 30 s - the same run
lost 59,602 (42.6%) and warned twice for one outage.

Checks: `psqueuetest` (in `check-fast`) links the real engine: under a
64 KB cap the queue stays under it and every publish is queued or
dropped, with no cap nothing drops, each drain delivers at most one batch
and re-arms the worker only while something is left, and the byte gauge
returns to zero - removing the cap, the batch limit, the re-wake or the
byte accounting each fails it, and so does waking after the last batch.
`psrelaytest` walks the path through ticks a little over a second apart
(a 3 s deadline or the old 10 s / 30 s timings fail it).  `relayrxtest`
(in `check-fast`) runs three nodes, two with the relay port and one
without: direct paths, 12,000 concurrent publishes reaching both peers by
the right path with nothing lost or duplicated, a relay port another
process holds, the config dump, the queues drained, and a node restarted
without its port logged as a notice - the build before that fix logs it
as a fallback and fails.  `clmembtest` checks the heartbeat with and
without the relay field, and a frame cut one byte short.

### The pub/sub engine without its global lock, and a pattern index (PS9, PS10)

The first engine guarded everything with one mutex: every publish
resolved its receivers under it, every worker's delivery resolved them
again under it, and every RESP command took it to ask whether the
connection was subscribed.  Every pattern was globbed on every publish.
`bench/psengine` measures the engine alone (real `src/pubsub.o`, stubbed
sockets) on 12 cores, and the numbers settled the redesign:

| | before | after |
|---|---|---|
| 1 publishing worker | 2.37M publishes/s | 2.53M |
| 8 publishing workers | 122k publishes/s, 973k deliveries/s | 547k, 4.37M |
| 8 workers, 1,000 patterns | 6,967 publishes/s | 537,073 |
| SUBSCRIBE+UNSUBSCRIBE p99 during that storm | 1.78 ms | 10.6 us |
| ... and its worst case | 6.4 ms | 485 us |

- **Each worker owns its subscribers.**  A connection lives on one worker,
  and so do its subscribe, unsubscribe, close and delivery - so each
  worker keeps its own tables, which nothing else touches and nothing
  locks.  Delivery walks them directly and builds a `pmessage` frame once
  per matching pattern instead of copying pattern names out under a lock.
  The subscribed-mode check every RESP command makes is now a lookup in
  the worker's own table, and returns at once on a worker with no
  subscribed connection.
- **Publishers read a shared index without a lock.**  What a publish needs
  is how many subscriptions match and which workers hold them: one shared
  entry per distinct name with a count and a bitmap of owning workers.  It
  changes only when a worker gains its first or loses its last
  subscription to a name, under one writer mutex; a removed entry is freed
  only after every reader thread has passed a quiescent point (the same
  `quiesce.h` protocol the index tables use), and a thread outside that
  protocol reads under the writer mutex instead.
- **Patterns are indexed by literal prefix** (`src/psindex.[ch]`).  The
  bytes before a pattern's first `*`, `?`, `[` or `\` must match the
  channel byte for byte, so a pattern is filed under its first 32 literal
  bytes and a publish globs only the patterns whose prefix the channel
  starts with.  Patterns that begin with a metacharacter cannot be pruned
  and are always tried.
- **A cross-worker wake is written only when the worker's queue was
  empty**; every message queued behind it rides the same wake.

Checks: `psindextest` (in `check-fast`) compares the prefix index with a
brute-force glob over every pattern - 40,000 channels against 3,000
patterns with literal prefixes shorter and longer than the index depth,
removals half way, in a table that grows and one that does not - with no
disagreement (dropping the deepest level from the lookup gives 2,243).
`psengine 8 10 200 2` subscribes and unsubscribes, a million times,
channels and patterns the publishers are hitting and nobody else holds;
under AddressSanitizer it is clean, and the same run with the grace
period removed reports a heap-use-after-free in a publisher.  The RESP
door itself did not move measurably: GET at eight workers read 2.39M/s on
both builds, bounded by the benchmark client on that host.

### The pub/sub fan-out bench (PS7)

`bench/psbench.c` drives pub/sub from a third host: `psbench sub` holds N
subscriber connections and counts what each receives, `psbench pub`
publishes at a paced rate with 16 requests in flight per connection, both
over libperfd's sealed native dialect.  It reports its own CPU over the
ACTIVE window (first to last event) and says when it is at its own
ceiling - averaging over an idle tail hid a pegged driver on the first
run.  Two-host rig: node A on 222 (the publisher's node), node B on 223
(the subscribers'), six workers each, the driver on 190, 64-byte payloads,
10 s of publishing, so every delivery crosses the relay and the driver
shares neither server's cores.

| Cell | Relayed | Missing | `relay_lost` | Delivered (expected) |
|---|---|---|---|---|
| 2k/s, 32 subscribers | 19,857 | 0 | 0 | 635,424 (635,424) |
| 10k/s, 32 subscribers | 96,895 | 0 | 0 | 3,100,640 (3,100,640) |
| 40k/s, 1 subscriber | 395,902 | 0 | 0 | 395,902 (395,902) |
| 100k/s, 1 subscriber | 990,732 | 56 | 56 | 955,324 of 990,676 * |
| 100k/s, 1 subscriber | 990,212 | 0 | 0 | 990,212 (990,212) |

\* the subscribers were released 6 s after the publisher while node B
was still draining; held 30 s, the same cell delivered exactly.  After
the accounting fix `relay_lost` equals the true loss in every cell.

What limits it, as measured: the relay receive side is one thread per
node.  Near 100k relayed messages/s it lost 7.9% at the receiver's kernel
socket buffer in one run and 0.006% and 0% in two later ones; at 180k/s
(one run) it lost 35%, nearly all of it counted by the kernel as receive
buffer overflow.  A sealed native delivery cost node B about 15-17 us of
CPU each; the driver, not the daemon, bounded the 32-subscriber cells.

### Pub/sub counts what happened, and a close no longer queues behind it

From a review of the engine and from the PS7 two-host bench, which found
the counters describing events that had not occurred.

- **Relay loss counts only loss.**  The relay stamped every datagram with
  one global counter, taken by whichever worker handled the publish, so
  two workers publishing at once put N+1 on the wire before N and the
  receiver counted N lost for good.  On the bench every relayed message
  arrived and every delivery was made, yet `relay_lost` read 411 at 2k
  publishes/s - and at 100k/s it read 142,710 where 77,914 were truly
  missing.  The sequence in the same 8 bytes is now epoch (random per
  process start), lane (one per sending thread, so a lane leaves in
  order) and counter; the receiver keeps a 64-message window per sender
  lane, counts a message lost only when it leaves the window unseen,
  drops a repeat (new counter `relay_duplicates`), and treats a new epoch
  as a restart rather than a gap - the old high-water mark hid real gaps
  after a restart until the counter caught up.  `psrelaytest` drives the
  accounting deterministically (14 checks; rc1's rule counts 499 losses
  on 1000 swapped arrivals with none missing); `pubsubrelaytest` has
  eight concurrent publishers relay 12,000 messages: rc1 reports 468
  lost, now 0 lost and 0 duplicates.
- **A slow subscriber is one kill.**  A subscriber past the output cap is
  closed asynchronously, and every message still handed to it before the
  close landed counted another `slow_kills`: 43,579 on the bench with 32
  subscribers attached.  It counts once per connection now; a pipelined
  test counted 9,337 against the old rule and 1 against the new.
- **Our allocation failure is not the client's fault.**  A failed
  allocation while delivering was counted as a slow kill and CLOSED the
  subscriber; a failed local queue node was counted as a relay drop; a
  message that could not be built still answered PUBLISH with the
  receivers it never reached; a receiver the delivery list could not grow
  to hold ended the walk and lost every receiver after it.  Each now
  counts in `alloc_failed` (on `/stats`, `/metrics` and the page), skips
  only the delivery it failed, never closes a connection, and an unbuilt
  message answers 0.
- **A close takes the engine's lock only if the connection subscribed.**
  Every connection close in the daemon - GET clients, `/metrics`
  scrapes - took the engine lock and walked every subscribed connection.
  A connection the engine never saw now skips it, and the subscribed
  connections are hashed by pointer instead of listed.
- **Patterns are walked as a list**, not by visiting all 256 hash buckets
  on every publish and again on every worker's delivery.
- Documented and tested, as Redis does it: a connection subscribed to a
  channel and to a pattern matching it receives both a `message` and a
  `pmessage`, and `PUBLISH` counts it twice - the count is deliveries,
  not distinct clients.

### SECURITY: a PSUBSCRIBE pattern could stall a node, or read past its buffer

Two defects in the pub/sub pattern matcher of 0.4.0-rc1, both reachable
by any client holding the ordinary client secret, both fixed by
replacing the matcher.  0.4.0-rc1 must not face untrusted clients.

- **Exponential in `*`.**  The pattern `*a*a*a*a*a*a*a*a*b` made one
  `PUBLISH` to a 40-byte channel of `a`s take 2.1 s, and twelve stars ran
  past two minutes.  The match runs under the engine lock that every
  `PUBLISH`, `SUBSCRIBE` and connection close on the node takes, so the
  node's workers stalled behind it - on rc1 the rest of `pubsubtest`
  failed behind the one stalled check.
- **An open set read past the pattern.**  A pattern ending inside a
  `[set]`, such as `[a`, matched against a channel like `ab`, wrapped its
  remaining length below zero and read beyond the pattern's allocation -
  shown under AddressSanitizer as a heap-buffer-overflow.

The new matcher is written for this engine: iterative, with one
backtrack point (every token but `*` consumes exactly one byte, so only
the most recent star ever needs to take one more), which bounds the work
by pattern length times channel length with no recursion at all.  A set
left open is closed at the pattern's end.  Its answers were checked
against the previous matcher's on 20 million generated pattern/channel
pairs with no disagreement, and the pattern vectors are unchanged.
`pubsubtest` now asserts a 16-star pattern against a 200-byte channel
answers in under a second (0.000 s) and that `[q` matches `q` and not
`qq`.

Licensing: the matcher it replaces was a port of Redis's
`stringmatchlen`, which Redis published under BSD-3-Clause (before its
2024 relicensing).  BSD-3 is compatible with this project's GPL-2.0-or-later
daemon, but it requires Redis's copyright notice to travel with the
code, and 0.4.0-rc1 did not carry it.  No Redis-derived code remains.
`KEYS` / `SCAN` globbing was checked and is unaffected: it is this
project's own iterative matcher, already linear.

### The pub/sub card, and the engine under mutation (PS7)

The cluster plane gains two cards: pub/sub (subscribers, channels and
patterns as gauges; published, delivered locally, keyspace events and
slow subscribers closed as totals) and pub/sub - fleet relay (sent,
received, gaps seen, too large to relay), the last two in the warn
colour because both mean a subscriber on another node did not hear it.
The `/metrics` gauges behind them already shipped with PS1 and PS3.

The engine was then mutated to find out what the suites actually catch.
Dropping the cross-worker wake, handing every exact-channel message to
worker 0 whoever owns the subscriber, and stopping `*` from consuming
characters were each caught (13 checks, 2 checks and 2 checks red
respectively, across pubsubtest, pubsubrelaytest and keyspacetest).  A
fourth - requiring one character after a trailing `*` - survived, and
is an EQUIVALENT mutation: the star case sits inside `while (plen &&
slen)`, so the length it tests can never be zero there.  Chasing it
found the real gap next door: the tail that swallows a trailing `*`
once the channel is exhausted, the path `news*` takes against `news`
itself, which every existing vector missed.  `pubsubtest` gained four
vectors for it (`zed*` against `zed`, `zedx` and `ze`, and a bare `*`),
and removing that tail now fails the suite - so the coverage is real
rather than assumed.

### The collection cards answer for the fleet (S163)

Every per-collection figure the page renders - entries, client hits and
misses, stores, expired - shows the FLEET's total first and this node's
share beside it ("63 · 39 here"), so the question "is `th` being read"
gets the same answer on every node's page; the node-local view stays as
the share.  The figures ride the membership plane the way S160's
connection counts do: each node's heartbeat carries a bounded block
(`CLMEMB_A_COLS`: the first 16 live collections, seven 64-bit fields
each - the name's FNV-1a hash, entries, its OWN client hits and misses,
stores, removes, expired; a fleet with more shows the first 16 and says
so), `/members` publishes it per member as `collections` (with
`collections_total`), `/stats` rows carry the `hash` to join on, and the
page sums the map.  Client-only on purpose: a pull one node serves for a
peer stays out of the fleet total, or that lookup would count twice.
Entries summed across members count COPIES (every node under eager, K
under spread, about one otherwise), and the cell's hover says so.  The
heartbeat grew from 90 to at most 988 bytes; an older peer ignores the
tail.  `colsfleettest` (in `check`): a three-node store-mode fleet, the
map's client hits equal the members' own, a pull served for a peer moves
the server's figure not at all, the hash joins, a standalone daemon has
no fleet column.

### The status page's clients strip, `/clients`, and the commands card (S160)

The page's headline strip gains `clients 123 / 1,000 fleet-wide`: this
node's open client connections over the fleet's, with chips for the
dialect split (binary, json, resp) and the `max_clients` limit when one
is set.  The denominator rides the membership plane - every node puts
its open count and the four-way split into the heartbeat it already
sends each second (`CLMEMB_A_CLIENTS`, twenty bytes an older peer
ignores), `/members` publishes them per member as `clients`, and the
page sums the map, so the figure is at most one heartbeat stale and the
page never fetches another node; on a standalone daemon the denominator
is simply absent, and a fleet mid-upgrade says how many members report.
Clicking the strip opens this node's connections: id, door, dialect,
wire, address, name, connected since, idle, commands, last command,
pending output bytes (the figure that predicts a slow-consumer close at
the output cap) and subscriptions, active first, a filter on address
and name, the newest 200 rows with "showing N of M".  Behind it is a
fifth read-only GET, `/clients`, on the HTTP listener under the same
allow-list and token as the other four, produced by the walk that makes
`CLIENT LIST`.  No kill button, by decision: `CLIENT KILL` on the RESP
door is the auditable, secret-gated way.  The cluster plane gains three
cards from S159's rows: commands by calls, commands slowest per call,
and the slow log's tail.  `clientstest` (in `check-fast`): one
connection per door lands in the table with its door, dialect and wire,
a named one carries its name, a subscriber its count, a closed one
leaves, a subscriber that stops reading shows pending bytes and is then
gone; `clientsfleettest` (in `check`): a three-node fleet's map carries
each member's count and their sum, absent on a standalone daemon;
`grafanatest`'s CLIENT LIST assertions unchanged.

### Command statistics on every door (S159)

The JSON and binary doors now count into the same command rows and the
same slow log as the RESP door, under their dialect's name - `json.get`,
`bin.get`; RESP keeps its bare Redis name - because the two native
dialects cost differently and a merged row would hide it.  A native
slow-log entry's argv is verb, collection, key.  The rows are reachable
off the RESP door: `/stats` gains `commands` (name, calls, usec, the
p50/p99/p99.9 bucket bounds and the seventeen buckets) and `slowlog`
(the newest 32 entries, SLOWLOG GET's six fields), `/metrics` gains
`perfcached_command_calls_total{cmd}`, `perfcached_command_usec_total{cmd}`
and the histogram `perfcached_command_latency_seconds{cmd}` (log2
buckets, 1 us to 32.768 ms and +Inf), and `INFO latencystats` answers
`latencystat_<cmd>:p50=,p99=,p99.9=` in Redis 7's shape, each figure the
bucket bound the percentile falls under - so `INFO all` grew a section
and bare `INFO` did not.  Same accounting as before: per-worker rows,
owner writes, merged at read.  The tax, measured on 223 (16 vCPU, W=8,
natbench 64 connections / 6 threads / depth 32, plaintext loopback,
GET-only 10 s cells, 3 alternating reps against v0.4.0-rc1): binary GET
7.43M/s to 7.21M/s (-3%), JSON GET 3.19M/s to 3.03M/s (-5%).  The first
cut cost -10% / -13%; two trims bought the difference - the row slot is
resolved once per worker and kept thread-local, so the name hash is
paid on a verb change only, and the clock is carried from one request's
end to the next's start inside a drain, so a pipelined batch reads it
once per request.  What remains is that one clock read and three
increments.  `cmdstatstest` (29 checks, in `check-fast`):
exact counts per dialect, the native names in the slow log, bucket sums
equal to calls, the /stats mirror of SLOWLOG GET, the histogram's shape,
and a ten-second threshold that counts and never logs; `grafanatest`
unchanged.

## 0.4.0-rc1 — 2026-09-16

The first candidate of the 0.4.0 line, a capability step: Redis pub/sub
on every door and across the fleet.  The tree is 0.3.8-rc2 (the four
daemon fixes below, green on both CI systems) plus PS1 to PS4 and PS8; the
wire dialects gain the pub/sub verbs and a message notification and
change nothing else; the cluster wire gains one datagram type,
`M_PUBLISH`, which an older peer ignores.  libperfd is 0.2.9.

### Pub/sub over the RESP door (PS1, PS2)

`SUBSCRIBE`, `UNSUBSCRIBE`, `PSUBSCRIBE`, `PUNSUBSCRIBE`, `PUBLISH`,
`PUBSUB CHANNELS / NUMSUB / NUMPAT` and `RESET`, with Redis's
subscribed-mode gate, its `["pong", ""]` PING, its `message` /
`pmessage` frames and its pattern matching.  Channels are one global
string space on every door.  A subscription lives on the connection
that made it; a publish is delivered to every local subscriber by the
worker that owns each connection - a publish on one worker hands the
others the message through their queues and eventfds, and each resolves
its own subscribers - so no connection pointer crosses a thread.
`PUBLISH` answers the local receiver count, as Redis Cluster does.
`PUBSUB NUMSUB` is O(1) per name.  A subscriber that does not read is
closed at the output cap and counted (`stats.pubsub.slow_kills`).  The
`__pc.` prefix is reserved.  `[listen] keepalive_s` (default 300, as
Redis's `tcp-keepalive`) puts TCP keepalive on every accepted data-door
socket, since the daemon never idle-closes one and a dead subscriber
would otherwise look live until a message went out.  `pubsubtest`:
37 checks, from confirmations and fan-out over four workers to the
matching vectors and the slow-subscriber kill.

### Pub/sub on the native doors and in libperfd (PS4)

The JSON and binary dialects publish and subscribe on the same engine:
`subscribe` / `psubscribe` / `unsubscribe` / `punsubscribe` answer
`{"subscribed": N}` and `publish` answers `{"receivers": N}`; on the
binary dialect the same five as verbs 10 to 14.  Deliveries are id-less
notifications - `{"method":"message","params":{"channel","payload"
[,"enc":"b64"]}}` and `pmessage` with a `pattern` - framed per dialect
the way the daemon's own pushes already are, so a native connection
stays multiplexed: a blocking get keeps waiting for its own id while
the messages go to the notify hook.  A RESP publish reaches a native
subscriber and the reverse.  libperfd 0.2.9 adds `perfd_publish`,
`perfd_subscribe`, `perfd_psubscribe`, `perfd_unsubscribe` and
`perfd_punsubscribe`; messages arrive on `perfd_set_notify`'s hook.
`pubsubtest` grows to 63 checks with the two doors and a libperfd
client; one test lesson worth keeping: the hook's `(json, len)` is not
NUL-terminated at `len`, since the next line may follow it in the
library's buffer.

### Pub/sub across the fleet (PS3)

A publish on any node reaches the subscribers on every node: one
sealed datagram per live peer on the unicast plane the replication
rides, sent by the publishing worker, never the multicast group, so
S36's poll mode will need nothing from it.  A relayed message is
delivered to the peer's own subscribers and never relayed again, and a
sender never receives its own.  At-most-once, with a per-sender
sequence so a receiver counts what it missed (`relay_lost`) rather
than retransmitting; a payload that does not fit a datagram is
delivered locally and counted (`relay_dropped`).  `PUBLISH` keeps
answering the local count.  `pubsubrelaytest`: 18 checks on a
two-node fleet, both directions, patterns, 30 in order, the oversized
case.

### Keyspace notifications (PS8)

`[collection X] notify_events = miss, expired, store, remove | all`
(and a `[daemon] notify_events` default), beside `log_events`: the same
event kinds published as Redis keyspace notifications on
`__keyspace@<collection>__:<key>` (payload: the event) and
`__keyevent@<collection>__:<event>` (payload: the key) - `set`, `del`,
`expired`, `keymiss` - so a Redis client's `PSUBSCRIBE __keyevent@0__:*`
works unchanged.  Emitted by the node that APPLIES the mutation: the
door for a local write, the holder for a forwarded one, every node for
an eager replica, the sweep for an expiry; never relayed, as Redis
Cluster's primaries do it, so a subscriber attached to one node hears
that node's stream once and a handle attached to every node hears each
event per copy.  Off unless a mask names it.  `keyspacetest`: 17
checks on one node and two.

## 0.3.8-rc2 — 2026-09-16

rc1's tag pipeline was red at `configtest`, which still asserted the
contract S158 replaced (a standalone `mode = proxy`, `mode = shard` or
missing cluster secret refused).  rc2 is rc1 with that test moved to the
new contract; the daemon is unchanged.

## 0.3.8-rc1 — 2026-09-16

Follows 0.3.7 (2026-09-15).  Four daemon changes, each with a test
that fails on 0.3.7, plus the one-host re-measurement of every table.
The wire dialect and the cluster wire are unchanged.

### A fleet whose live members fall below `replicas` says so (S157)

Validation cannot check it - membership is automatic - so it is a
runtime condition on the cluster thread's 1 Hz beat, once the node is
ready and outside the shard grace, held for three ticks before it is
believed: one WARNING on the way in ("replicas = 3 with 2 live
member(s): every key has at most 2 copies until the fleet grows"),
one NOTICE on the way out, `replicas_short` on `/stats`, a gauge on
`/metrics`, and "N copies short" beside members-up on the status page.
A single node founding a fleet with `replicas = 2` warns within
seconds, which is what a reader of the configurations page needs to
hear.  Not a refusal: a fleet below K is degraded, not wrong.

### A standalone daemon runs a fleet's collection stanza unchanged (S158)

Without a `[cluster]` section, a collection's `mode` (all five values)
and `pull = 1` are accepted, run as store, and named once at warning
level by `-C` and again at startup - "collection 'a': mode = shard
ignored - no [cluster] section, running as store" - so one file serves
a single node first and the fleet later.  The per-collection `spread`
refusal moved from the parser to validation, where it fires only when
a cluster exists.  The cluster secret is optional standalone, since
nothing there derives from it, and still required, and still distinct
from the client secret, with a `[cluster]` section.  `mode` under
`[daemon]` stays an unknown key.

### `max_clients` refuses with a reason (S161)

`[daemon] max_clients` caps client connections across the data doors;
unset, it is derived from the descriptor limit minus what the process
needs open, and the soft limit is raised toward the hard one when the
configured value needs it - a node that still cannot fit runs with the
limit it can afford and says so.  At the limit a RESP client gets
`-ERR max number of clients reached` at accept, a native client an
error reply in its own dialect at its first request, and the
connection is closed; HTTP stays outside the limit so `/stats` keeps
answering.  Refusals are counted (`clients` on `/stats`, three series
on `/metrics`) and logged once per transition.  Before this a node at
the descriptor ceiling failed `accept` in silence.

### Measured: every table on one host, one daemon, one day (S162)

The README's Measured section is one run of the tagged 0.3.7 daemon on
the 16-vCPU Debian 13 host, 2026-09-16, with the load off-box wherever
the harness allows it - the wire table from another host, the `spread`
table from a 24-core farm of two - and from a driver container on the
server host where it does not (the cluster tables, marked client-bound).
The loopback one-server table and the MTU section move to
`bench/RESULTS.md`; every earlier run stays in `bench/results/`.
`bench/xhostbench.sh` now asks the RESP door for readiness instead of
reading a container log that a detached container never fills - its
first end-to-end containerised run found that.

### A spread node that comes back empty is handed its share, not the store (S156)

The boot pull streamed every record of every eager collection to a
joiner - right for eager, where the joiner owes everything, and wrong
under `spread`, where a node that restarted empty ended up holding the
whole keyspace until the reclaim pass trimmed it at 64 keys a sweep.
Measured on three nodes at K=2 with 3,000 keys: the returning node
reached 3,000 of 3,000 within 18 s and the fleet 7,001 where K x N is
6,000.  `bulk_serve_boot` now scopes its walk with the write path's
own placement predicate, so the joiner receives exactly the records it
is a holder for (its pre-restart count, 1,999 of 3,000, fleet 6,000)
and every key stays readable through it; a joiner the map does not
list yet gets nothing from the boot pull and its share from set-repair,
which a cold start already arms on every holder (S147).  The log line
now says how many records were outside the joiner's share.  Eager is
unchanged - the same restart still hands the node everything - and
`spreadcoldtest` asserts both, failing on the previous daemon in the two
places it should.

## 0.3.7 — 2026-09-15

The 0.3.7 release: the tree of `v0.3.7-rc16` with the documentation and
measurements below; the daemon, the library and the tools are
source-identical to rc16, whose tag pipeline was green on GitLab and
GitHub.  What the line added over 0.3.6.1, in order of arrival (rc1 to
rc16, each with its own section further down):

- **`mode = spread`** - K copies of each record on P nodes, placed by
  HRW over the stable advertise addresses; the push targets the set,
  reads pull from a holder, the reclaim pass drops surplus only on a
  holder's yes (S127 Z1-Z6, S146); libperfd 0.2.8 learns the fleet from
  any node's `members` reply and routes to the holders.
- **DDL behind a privileged connection** (`enable`, a second secret) -
  a client secret alone can no longer create, drop or resize a
  collection (S129).
- **Per-key event logging**, selected per collection (S153); **client
  and peer told apart** in the hit counters, and `reach` - distinct
  keys served (S148, S151); the durability card's **`staged` /
  `unsynced`** split (S145); `-LOADING` as Redis spells it (S138).
- **An index that sizes itself both ways**: grows at `grow_at_pct`,
  shrinks at `shrink_at_pct` after `shrink_cooloff_s`, the retired
  table handed back to the arena once its readers have moved on (S150
  A-D), and carves what it uses - 68 bytes a bucket above the flat
  floor, was 192 (S155).
- **Two fixes a running fleet found and no suite did**: a node that
  bounced inside another node's slot grace was never re-placed to
  (S147), and an empty node's "not found" could delete the fleet's
  only copy (S152).  An online resize broadcast as a DROP (S69), the
  apply path prefetching the next bucket (S126), `keys` bounded per
  collection.

### Documentation, this release

- The README reworked around a **Start here** on-ramp and a Contents
  index; the measurement discussion moved to `bench/RESULTS.md`; ten
  validated configurations in `contrib/CONFIGURATIONS.md`; every RESP
  command with its parameters in `REDIS-COMMANDS.md`.
- `test/readmetest.sh` in `check-fast`: the README's status must name
  the version being built and the CHANGELOG must carry its section.

### Measured, this release (bench/containerbench.sh on a 16-vCPU Ubuntu 24.04 host, podman)

- **The cluster modes through a plain Redis client, five modes**, on
  0.3.7's daemon: reads in one band across store, eager, proxy (x4.5
  against Redis at pipeline 16), eager's and spread's SET paying for
  their copies (x2.9), shard through a client that cannot route the
  forwarding floor (x0.4), spread's GET the pull path (x0.8 at 16,
  x0.4 at 64) - a spread node never keeps what it pulls.
- **The same fleets through a Redis client that routes** (one
  hash-tagged driver per owner): shard at x5.6 SET / x6.7 GET at
  pipeline 64 where the plain client gets x0.2, spread at x3.8 / x7.1.
- **`spread` at capacity** from an off-box client (`bench/spreadbench.sh`):
  the routed read ceiling on a 20-core farm, 4.25M GET/s at K=2 P=3.
- **Known, found while wording the cold-node table (S156)**: a spread
  node that restarts empty is backfilled with the whole keyspace and
  trimmed back to its share by the reclaim pass at about ten keys a
  second.  Nothing is lost; the K-copies memory bound is exceeded after
  a cold start for minutes on a bench keyspace and hours on a
  production one.  Filed, not fixed in this release.

## 0.3.7-rc16 — 2026-09-15

Follows rc15 (green on GitLab; its GitHub tag run hit a test flake that
is fixed here). rc16 is S150's shrink, built in four steps on the
maintenance thread and the arena, and S155, which was found grounding
it: the index carves what it uses. The wire dialect and the cluster
behaviour are unchanged from rc15.

### Index regions carve what they use (S155)

A bucket segment is exactly one 256 KB arena slot, and the 16-byte
region header in front of it cost every segment a second slot; the
16 KB hint block cost a third. Reservation regions are now header-less
and hint blocks are packed sixteen to a slot. A 2^16 index is 5 MB (was
12.75), 2^20 is 68.75 MB (was 192.75): 68 bytes a bucket above the flat
floor, was 192. Nothing about lifetime changes — regions are still never
freed — and `held` on the memory card drops by the same amount for the
same tables. `indextest` pins the geometry on both carve paths and the
oracle the shrink work (S150) is measured against: what
`pcache_htable_index_bytes()` quotes is what `regions_bytes` moves by.

### A retired index table waits for its readers (S150, step B)

The read path is lock-free, so a table the registry no longer publishes
— the old table of a resize, a dropped collection's — may still be under
a thread's feet. Every thread that can hold a table pointer now marks
itself inside for the span of a loop turn or a walk (`src/quiesce.c`:
two stores per turn, nothing on a fetch); a retirement takes a stamp
after the unpublish and clears once every line has parked since. A
resize's old table sits on a retirement queue until then, and a dropped
collection's table is reused by the next `create` of its size only once
clear. `/stats` shows `retire.pending`, `retire.cleared`,
`retire.keys_ended_on_swap`, and the quiescence `lines` / `inside`. A
RESP `KEYS` walk, which resumes across turns holding a table pointer,
now carries the table's registry slot and publish generation and ends
on a swap — what it emitted stands — instead of reading on from a table
that is being retired. Nothing is freed yet (step C). `quiesctest`
proves the primitive; `keysretiretest` runs three walks through a
resize.

### A retired table's index goes back to the arena (S150, step C)

Once a retired table's readers have parked, its regions join the
arena's free set of index slots — one set, no keying by size, since
every index region is one slot after S155. The next table of any size
takes those slots before the frontier moves, so resize up/down cycles
and drop/create cycles keep `regions_bytes` and `held` flat instead of
stair-stepping. Past the give-back cool-off, whole quiet groups of
retired slots are punched out through the existing give-back path,
above a keep of one group; a punched group is re-committed on the next
take. The S128 ceiling credits resident retired slots to a create or
resize. `/stats` `memory` gains `arena_regions_free_warm`,
`arena_regions_free_cold`, `arena_regions_retired`, `arena_region_reuse`;
`retire.returned_bytes` counts what went back; `/metrics` gains the
matching gauges and counters. `regiontest` proves the flat cycle and
that the process RSS drops by exactly the punched group and returns on
the re-take; two arena mutants (no give-back, no punch) fail it.

### A table shrinks (S150, step D)

The other half of the splitter. A table that has sat below
`[daemon] shrink_at_pct` of its slot capacity (default a quarter of
`grow_at_pct`, so 18% against 75%) for `shrink_cooloff_s` (default
60 s) since it last split, was resized or started is resized down — by
the S69 migration, to the size midway between the two thresholds so one
copy lands it where neither fires — and never below one segment. The
old index goes back to the arena through steps B and C. Local, like the
splitter: each node sizes what it holds. A table split up from the size
it was made at may now be resized back to that size. `shrinktest`: a
2^18 table with 100,000 keys shrinks to 2^16 and an empty 2^14 to 2^12
and both stop there; 79 slots go back, 16 MB of them are punched, and a
later `create` takes 2 MB of them back with the process RSS rising by
exactly that.

### The README says what is true of the build (item E)

`spread` is labelled by its status, not by the rc that introduced it:
complete since rc2, repaired in rc13 (S147), never run on a production
fleet - the staging fleet is `eager`. The Measured section names the
build and date each table was taken on and what has moved on the read
path since (S148, S150, S151, S155), none of it re-measured yet. The
design ledger's S127 line carries its BUILT stamp - it read FILED while
the mode was complete - and MODULARITY records `cluster.c` at 7,171
lines against its ~2,700 target.

### Measured: `spread` at capacity (bench/spreadbench.sh)

The first speed measurement of the mode, on rc16 from an off-box client:
P bare daemons on one 16-core host, natbench over libperfd, routed and
un-routed, eager on the same fleet as the yardstick. Writes are a server
ceiling in every cell; eager's falls from 1.84M/s at P=3 to 1.11M/s at
P=6 while spread K=2 holds 1.76M/s. Un-routed spread reads are
pull-bound at ~0.3M/s with p99 over 30 ms; routed they are eager's or
better, 3.36M/s at K=2 P=3 with zero pulls. Rows in
`bench/results/spreadbench.tsv`; the reading in DESIGN 12cz.

### Fixed

- README: the `log_events` paragraph had landed inside the sizing
  section, between its sentence and its table.
- `recoverytest`: the storm slept a fixed interval for the WAL to drain
  before each `kill -9`; on the two-core ASAN runner it had not drained,
  and rc15's GitHub run reported 95 of 600 acked keys "lost". It now
  waits on `wal.staged`.

## 0.3.7-rc15 — 2026-09-15

**Replaces rc14, which did not link on either CI.** `wipetest` builds
`config.c` standalone, and rc14's `config.c` called the event-list parser
that lived in `events.c`. The parser now lives in `config.c`, where a
parser belongs. No daemon behaviour change from rc14.

## 0.3.7-rc14 — 2026-09-15

Follows rc13 (green on both CIs, live on the staging fleet). rc14 adds
the operator's event log, the durability split, the growth threshold,
and takes the index charge back to rc10's figure. The daemon's cluster
behaviour is unchanged from rc13.

### The growth threshold is a percentage, and configurable (S150)

A table used to split once it passed four entries per bucket — 4 of 6
slots, 67%, hardcoded. `[daemon] grow_at_pct` sets it as a percentage of
slot capacity; the default is now **75**. The increment is unchanged:
one bucket and its partner per split, budgeted per maintenance tick.

### The durability card says which kind of "not yet durable" (S145)

One "unsynced records" figure spanned two states with different
consequences, and misled the rc5/rc6 diagnosis. `/stats` now publishes
`staged` — acknowledged writes still in a producer ring, lost on
`kill -9` and dropped outright if the ring fills — and `unsynced` —
in the log file, not yet fsynced, lost only to power loss. The status
page shows both and flags only `staged`; a small non-zero `unsynced` is
what an everysec WAL looks like. `/metrics` gains the two gauges.

### The index charge is back to rc10's figure (S154)

rc11's `reach` sketch doubled the per-thread stat line, and the array
behind it was sized to a compile-time maximum of 1,024 threads — 256 KB
of index per collection, charged against the arena ceiling on every
create and resize. The daemon now registers its real thread count, so
a 2^12 collection costs 1.50 MB again instead of 1.75.

### Per-key event logging, selected per collection (S153)

```
[collection 0]
log_events = miss, expired, store, remove     # any subset; all; none
```
One line at NOTICE per event, naming the collection, the key, the door
it came in by and the peer address — and for a miss, *why*: `cause=absent`
or `cause=expired`. Selection is a per-collection mask, not the global
level (`dbg` would flood every subsystem to watch one collection), and
`log_level` still acts as a ceiling. `[daemon] log_events` sets the
fleet default; `log_events_rate` (100/s per collection per kind) bounds
it, and when the bucket runs dry the daemon says so — `suppressed=N` —
rather than dropping silently; `log_events_hash = yes` logs a short hash
instead of the key. Off costs one load on the get path. Client traffic
only: peer pulls and recovery probes never appear.

## 0.3.7-rc13 — 2026-09-15

**Replaces rc12, which was red on GitHub `check-asan`.** `coltest` asserted
the originator of a resize without waiting for its incremental copy to
finish — the same race fixed in `colresizetest` for rc12 — and ASAN's
slowdown exposed it. Test fix only; the daemon is rc12 plus S151 below.

### The hit rate now counts clients, not "anyone" (S151)

`hits`, `misses` and `reach` count every origin: a peer serving a pull
and recovery probing each replayed key land in them alongside client
lookups. They keep that meaning. New beside them: `hits_client`,
`misses_client` and `reach_client` — client workers only — and the
status page's hit rate, reach and hits/misses columns now read those.
`/metrics` gains `perfcached_collection_client_hits_total` and
`..._client_misses_total`. Additive; nothing existing changes shape.

## 0.3.7-rc12 — 2026-09-15

**Replaces rc11, which was red on both CIs.** GitLab failed `restarttest`
and GitHub failed `colresizetest`. The second was a test race. The first
was real, and it predates rc11: `restarttest` was reporting an actual
data-loss bug and being read as flaky.

### A node that came back empty could delete the fleet's only copy (S152)

After a simultaneous restart, a node replays its WAL and then asks the
fleet about each recovered key; if nobody has it, it was deleted while
the node was down, so the record is dropped **with a WAL tombstone**. The
guard meant to keep an incomplete node from answering tested a lifecycle
state — RECOVERING — and a node that replayed *nothing* never enters
it. Empty peers answered "not found" authoritatively from stores they
had never been filled into, and the one node holding the data deleted
exactly what they denied. Permanently.

Measured on the CI runner with only the writer's WAL populated — the
ordinary shape, since replicas take pushes and do not log them: 400
replayed, 64 dropped, 336 back on all three. Under I/O pressure the
window widens with the replay it races: 336, 80, 80, **16 survivors of
400**. The trigger is a fleet restarting at once; a single node
restarting is safe.

Fixed on the **asker**, not the answerer. Two answerer-side fixes were
tried first and both were wrong for one reason: the request carries no
purpose, and a probe-before-place needs the very "nobody has it" that a
reconcile must distrust — keeping cold nodes silent starved writes in a
fresh fleet (`proxytest`: 0 of 120 landed) to protect replays. The asker
knows its purpose from its slot and every peer's provenance from the
heartbeat, so `reconcile_tick()` now waits until no live peer is still
cold before issuing a probe, and each drop re-checks. The pass defers by
at most the 30 s backfill holdoff; it does not skip. Measured: the pass
fires 6 s after recovery unpatched and 37 s patched, probing the same
424 keys.

**New in `/stats`:** `cluster.reconcile_deferred` — drops refused because
a peer was still cold.

### `colresizetest` waited for the peer and not the originator

The originator resizes incrementally off the maintenance thread while the
peer adopts the announced size on receipt, so peer-first is a legitimate
ordering the test forbade. It only failed on a loaded runner.

## 0.3.7-rc11 — 2026-09-15

### The status page's two "hit rate" numbers were different numbers (S148)

The per-collection **hit rate** column divided the counters since the
node started, while the **hit rate (now)** card directly beside it
differenced consecutive samples. Two figures, one name. A cumulative
mean drifts toward the present rate and can never arrive, so the only
visible symptom was "the rate is falling slowly on every collection" —
the average being dragged by a current rate nobody could see.

The column is now the same poll-to-poll window the card uses, with the
lifetime ratio kept under it as `life N%`. On two real samples a
collection went from a green 60.7% to a red 20.0%, which is what it was
actually doing; another read a confident 100% while nothing had read it
all window. A window with no reads renders `no reads`, never 0% — 0%
would invent a fault that is not there. `reset-stats` rebases the
counters downward, so both baselines drop when `since.reset_at` moves.

The **durability · wal** card had the same defect. `fsync now (avg/max)`
showed the mean over every fsync since start beside a high-water mark
that never decays, so a single 117 ms stall sat under the word "now"
for the rest of the process lifetime and read as a live condition. What
is *now* is the EWMA — which is also the figure `probe underestimated`
tests against — and it was published in `/stats` and shown nowhere. Now
split into `fsync now (ewma)` and `fsync since start (avg/max)`.

### `reach`: distinct keys served, because a rate counts lookups (S148)

`hits/(hits+misses)` counts **lookups, not keys**. One key pulled
100,000 times a second beside 2,005 keys nobody touches reads as ~100%:
a cache that looks perfect while almost everything it holds is dead
weight. The rate cannot distinguish that from broad, healthy coverage.

`reach` is the number of **distinct keys actually served** in the last
closed window, published per collection beside `entries` and shown as
its own column. Driven into both shapes: one collection showed a
perfect 100% rate over **one key out of 300 held**, while another read a
worse 93.1% and was covering all 2,000 of its entries. The rate alone
ordered those backwards.

A 64-register HyperLogLog, ~13% error, 128 bytes per process per
collection. Approximate on purpose — the question is "dozens or
thousands", not an exact set. The window is closed on the daemon side
because a sketch cannot be differenced the way counters can, and each
worker rolls its own sketch: `pcache_pstat_t` is one cache line per
process precisely so no other thread writes it. The estimator is
integer throughout, since this build links no libm.

**New in `/stats`:** `reach` and `reach_window_s` per collection.

### Corrected: peer pulls DO move the per-collection counters (S151 filed)

`doc/DESIGN.md` asserted that a peer's pull probe reaches the table
through a function that touches neither counter, and said so as
something checked rather than assumed. It is wrong —
`pcache_ht_fetch_inner()` *calls* `pcache_ht_fetch_buf_inner()`, so
serving a peer's pull increments `hits` or `misses` like any client
get. Proven by calling the entry point directly.

Measured share on a live node: 12 pulls in 352 counted lookups over
15 s, **3.4%** — real, and nowhere near enough to explain the open
question about where a collection's misses come from, which therefore
stays open. Separating client from peer at the counter is filed as
S151; until then `hits`, `misses` and `reach` on a clustered node all
mean "anyone", not "a client".

## 0.3.7-rc10 — 2026-09-15

**Replaces rc9, which was red on `check` and `check-asan`.** The daemon
is unchanged from rc9; what was wrong was that S129's gate broke its
own test suites, in exactly the way it had already broken `perfload`.

`loadtest` drives `perfload`, whose `restore` is now privileged, and
every connection it opened was refused — 18 failures. `coltest`,
`indexceiltest` and `colresizetest` broke the same way: their helpers
dial **once per call**, and privilege is per connection and dies with
it, so a helper that dials per call must raise privilege per call.

Each suite's daemons gained an enable secret and each driver now raises
privilege on the connection it actually uses. The README documents
`[secrets] enable` beside `allow_create` — the two answer different
questions, and fail-closed is stated plainly because it will otherwise
surprise an upgrade.

## 0.3.7-rc9 — 2026-09-15

### DDL now needs a privileged connection, not just the client secret (S129)

`allow_create` answers *"may this NODE originate DDL"*. It cannot answer
*"may THIS LINK"* — applications, ops tooling and `perfcli` all present
the same `[secrets] client` value, so turning the node gate on handed
`create`/`drop`/`resize`/`rename` to every application connection.
Demonstrated against the previous build: an ordinary client connection
got `{"created":true}` and `{"dropped":true}` with no privilege raised.

A distinct **`[secrets] enable`** (rotation list, refused empty) raises a
bit on the **connection**, via `enable`/`disable` on the **JSON door
only** — not RESP, which may run plaintext off-box and would put the
highest-value secret in the fleet on the wire in the clear. The bit dies
with the connection, never crosses the wire between nodes, and is never
consulted when a peer applies replicated DDL. Comparison is constant
time across the whole list; failures are counted and published as
`door.enable_fails`.

**Fail-closed:** with no enable secret configured, `enable` always fails
and DDL is unreachable from the client door however `allow_create` is
set. The daemon warns at startup about that exact combination.

The privileged set is chosen by **blast radius**, not by the word DDL:
the four DDL verbs **and `restore`**, because `restore` with
`policy: overwrite` replaces every record it is handed at a fresh
version. `probe` is the next candidate and is deliberately out of v1.

**Tools** — `perfcli` gains `-E <secret>` / `-e` / `PERFCLI_ENABLE`
(distinct from `PERFCLI_AUTH` on purpose), and re-raises after a redial;
`perfload` gains `--enable`, raised on every connection it fans out
across, since `restore` is in the set.

**Upgrading:** a node with `allow_create = yes` and no `[secrets] enable`
will refuse all DDL from clients. Add an enable secret to those configs
before deploying, or `perfcli` DDL and `perfload` stop working there.


## 0.3.7-rc8 — 2026-09-14

**rc7's own gates split**: GitHub went fully green — `check-asan` there
had been red on rc5 and rc6, so the `restarttest` work landed — while
GitLab failed both jobs in `spreadtest`, on a daemon byte-identical to
rc6 which had passed. This tag carries the fix for that, plus the apply
prefetch and the `keys` change below.

### An online resize was broadcast to the fleet as a DROP (S69)

**A `resize` deleted the collection on every node.** `pc_cluster_col_announce()`
took an int named `drop` and folded it to
`op = drop ? CLCOL_OP_DROP : CLCOL_OP_SET`, while every caller already
passed a `CLCOL_OP_*` value. `SET`(0) and `DROP`(1) came out right by
coincidence; `RESIZE`(3), being merely truthy, went to the fleet as a
drop. The call returned `{"resizing":true}` and the daemon logged a
resize — nothing said a collection had been dropped.

Found on a live fleet against an **empty** collection. With records in
it this was fleet-wide data loss from a call that reported success.

The receiver was never at fault: `handle_col_set()` has always
understood `CLCOL_OP_RESIZE`. The announce now carries the op verbatim
and **refuses** an op it cannot express rather than degrading it to
something destructive, and the call sites pass named constants instead
of `0`/`1`/`3`.

New suite `test/colresizetest.sh` — a two-node **eager** pair with the
collection declared in both configs; 3 of its 6 assertions fail on the
unfixed binary. It needed its own suite because `coltest`'s pair runs
`mode = store`, which does not reproduce this.

### Filed, not built: S148, S149

The status page's per-collection **hit rate** column is lifetime-
cumulative under a bare `hit rate` header, beside a headline that *is*
windowed — so it reads as current when it is not (measured: 63.8%
lifetime against 6.4% over 20 s). Not a counting bug; peer probes are
correctly excluded from the counters. And separately, what those misses
actually look up is unidentified — the counters count lookups, not
names.

### A node that bounces inside another node's slot grace was never re-placed to (S147)

The failure behind rc7's red — the returning node ending short of its
share, the fleet at 222 of 240 — was not reclaim. A node that dies and
comes back at the **same address** inside a survivor's
`PEER_SLOT_GRACE_MS` (30 s) never changes that survivor's holder-set
fingerprint, so that survivor never re-places toward it; the only fill
it arms is the lowest-id backfill, which under spread holds K of P.
That is the ordinary production crash: a supervisor restart in under
30 s.

Now a witnessed cold start arms `setrepair` on every holder under
spread whether or not the set changed, and `setrepair` is cleared only
by a cycle that actually walked as a set repair.

Verified on the CI runner with two concurrent suites: 10 of 10 green,
against 5-of-16 and 5-of-10 red before it. Three of the ten reproduced
the low-surplus condition that produced every earlier failure and came
out whole.

### Reclaim asks before it drops (S146), and its two guards share one authority

Two changes to the reclaim pass, both real, neither the cause above:

- `pc_spread_in_set` consulted the master's map while
  `pc_spread_set_live` ranked HRW over locally-visible peers, so a
  delete could be authorised by the liveness of nodes that were not
  the map's holders. `set_live` now takes the map's top-K under the
  same coverage test.
- A surplus copy is dropped only after a designated holder confirms it
  has the record. A negative completes as a miss and a timeout has no
  handler: silence and "no" are NOT-KNOWN and neither may delete —
  S144's rule mirrored. No new wire: the probe is the ordinary pull
  with a different completion kind. This makes losing the last copy
  impossible whatever repair does; it did not, and could not, deliver
  a copy that was never sent.

### The apply path prefetches the next record's bucket (S126 item 3)

Profiling redirected this task: the registry scan it was filed against
measures 0.60%, which is why memoising it bought nothing. The cost is
the hash store — `store_impl` 23.5% beside `find_slot` 13.8%. A batch
applier knows the next key a record ahead, so the line has a whole
record to arrive in.

Measured as apply-thread CPU per applied record (the thread runs at
0.61 of a core, so a saving cannot appear as records/s): **1.5325 →
1.4683 µs/record, −4.19%, faster in 12 of 12 paired runs**.

### `keys` with a collection glob now bounds PER COLLECTION

**The default limit changes from 1000 shared across the result to 100
per collection**, and `keys *` now lists every matched collection.

It used to draw one budget down in registry order, so a first
collection larger than the budget spent all of it and every later
collection was dropped from the reply without being named. `truncated`
was the only signal, and it cannot distinguish "some keys missing" from
"whole collections missing". `matched` was counted inside the same
loop, so a glob matching three collections reported `matched: 1` - the
reply did not merely omit them, it said they had not matched.

Reply shape is unchanged and gains one additive field,
`truncated_collections`, naming the collections that hit their cap, so
nothing reading `collections` today breaks. The writer's overflow now
also stops the walk, which it did not before - the bound that matters
is response size.

`keys` still has no cursor; `scan` remains the cursored walk.

## 0.3.7-rc7 — 2026-09-14

**No daemon change from rc6 — `src/` differs only by `version.h`.** This
tag replaces rc6, whose `check-asan` was red on GitHub, and it also
**corrects the explanation given in the rc6 entry below.**

rc6 said the loss was a short WAL tail permitted by `fsync = everysec`,
and switched the suite to `fsync = always`. That reasoning was wrong.
`kill -9` ends a *process*; it does not touch the page cache, so
everything the WAL thread has already `write()`n survives it whether or
not an fsync has run. The flush policy cannot protect this suite, and
the recovered count did not move by a single record when it changed —
336 under `everysec`, 336 under `always`.

What `kill -9` does take is whatever is still *inside* the process:
records staged in a producer ring that the pump has not drained. A
worker is acknowledged at the ring, never blocks on storage, and a full
ring drops the record and counts it — all as documented in `wal.h`. So
a burst can be acknowledged and legitimately never reach the log.

`fsync = always` did help, but not for the reason rc6 claimed. The mode
also sets the pump's **poll cadence**: `always` re-drains immediately
while there is work, `everysec` waits 20 ms after a drain and up to
200 ms idle. It shortened ring residency, which was enough for the
GitLab runner — rc6 turned both `check` and `check-asan` green there —
and not enough for GitHub's slower one.

`test/restarttest.sh` now asserts what it is actually about:

- it **establishes its premise** instead of sleeping on it, polling
  `wal.appended` until the log holds the keyspace and reporting how long
  the drain took;
- no-loss is owed only where the log held everything, and where it did
  not, recovery must still return everything that *was* logged — so the
  shortfall is bounded and cannot excuse a real loss;
- it tracks a per-node **high-water mark**, because recovering short and
  shedding after recovery give the same final triple and are different
  faults;
- its premise check reads appended records rather than `du` on the WAL
  directory, which measured *preallocated* segments and so passed on an
  empty log;
- and the backticks in its config heredoc are gone — the heredoc is
  unquoted, so the shell was executing `everysec` three times a run.

Also filed: **S145**, the durability gauge on the status page adds two
states with different consequences (a record in a ring can be lost to
`kill -9` and dropped outright; a record appended and not yet fsynced
can be lost only to power loss), and the fsync mode silently changes
which one you are looking at.

## 0.3.7-rc6 — 2026-09-14

**Replaces 0.3.7-rc5, which was red on a test's own bad premise, not on
the product.** rc5's `check` and `check-asan` failed only in
`restarttest`, reporting 336/336/336 of 400 records with **zero** birth
races — so the fix that suite exists to guard was working, and the
assertion around it was wrong.

The premise I had not checked: **an eager replica is stored off the
WAL.** The author persists a record and a restarted replica resyncs, so
of three nodes only *one* holds durable state. A short WAL tail on that
single author therefore appears identically on all three — which by
entry count is indistinguishable from the bug the suite was written to
catch. And a short tail is exactly what the suite invited, because it
kills with `-9` while the WAL ran `fsync = everysec`, which never
promised the last second of writes. It now uses `fsync = always`, and
checks that at least one node actually carries a WAL before the restart
rather than assuming it.

No daemon change since rc5. The restart fixes are as that entry
describes, and the code has been running on a live three-node fleet
since.


## 0.3.7-rc5 — 2026-09-14

**A whole fleet restarting at once no longer deletes its own data.**
Found while deploying rc4 to a three-node fleet: every node recovered
the same ~1,990 records from its own WAL, and two of them ended holding
~425. It did not heal, because the eager sweep re-sends only records a
node authored. **Upgrade from rc4 if you ever restart more than one node
at a time** — a coordinated upgrade, a power event, a host reboot.

Two defects, both the same shape — an absence of evidence read as
evidence of absence:

**Two holders was treated as a conflict.** A second peer answering a
pull positively was declared a "birth race" and the loser's copy
demoted. But in `eager` mode two holders is the *normal* state: every
key is on every node by design. After a simultaneous restart every
reconcile probe drew two positives, so all but one node shed its copy.
The record version was already on the wire and simply never consulted.
Equal versions now mean the same record and are not a race; a genuine
disagreement resolves by **newer version wins**, which is a property of
the data rather than of which node id happens to be smaller. A version
of 0 means *unknown*, not equal, so a peer too old to report one still
resolves by id exactly as before.

**A recovering node answered pulls authoritatively.** A node still
replaying its WAL reported "not found" for keys it holds, and the
reconcile path deletes a replayed record when every peer says no. Three
half-loaded tables answered each other's probes and the fleet deleted
records all three were holding. A recovering node is now silent: the
asker times out, which is inconclusive and drops nothing. A confirmed
absence needs an answer from a node whose store is complete.

`test/restarttest.sh` covers it — 400 keys, WAL on every node, all three
killed and restarted together. Before: 336/184/336 with 152 demotions.
After: 400/400/400 with none.

Verified on a live three-node fleet: converged to 1,988 records on all
three in about five seconds, with zero birth races, where the same
operation on rc4 had left two nodes at ~425.


## 0.3.7-rc4 — 2026-09-13

**Replaces 0.3.7-rc3, which was red on a test-harness bug rather than on
the product.** rc3's `check` passed and every suite ran clean under
ASan+UBSan; only `clitest` failed, and only when two CI jobs ran it at
once. If you are already running rc3, the only daemon-visible change
below is the mode label.

**A spread fleet reported itself as `eager` on the dashboard.** `spread`
sets the per-collection eager flag, because it reuses eager's push
machinery aimed at K holders rather than at every peer, and both
per-collection mode strings derived from that flag. The dashboard labels
the fleet from those strings, so it told an operator "all in eager mode"
about a fleet holding K copies of P — a confident wrong answer in
exactly the property that line is read for. Both sites now ask the
cluster.

**`clitest` gave every concurrent run the same `HOME`.** Its pty section
used a fixed `/var/tmp`, so the perfcli history file was one shared path
for the whole host; CI runs two jobs concurrently with network — but not
filesystem — isolation, and both delete that file. Each run now uses its
own directory. Reproduced under two concurrent `netns-run.sh`
invocations before fixing, and green over four runs after.

The `spread` design record now also states the rule that took three
attempts to find: **adding a copy may act on a local opinion; removing
one may not.**


## 0.3.7-rc3 — 2026-09-13

**Replaces 0.3.7-rc2, whose `check` was red.** rc2's reclaim pass could
delete records a node genuinely holds: a returning node went from 58
records to 25 and the fleet to 207 where it owed 240. It was caught by
CI and not locally — three local runs had passed, and the CI runner is
simply slower and converges later.

The rule the fix establishes, now enforced in the code:

> **Adding a copy may act on a local opinion** — being wrong costs a
> surplus, which the reclaim pass tidies. **Removing one may not** —
> being wrong costs the record.

Placement falls back to rendezvous over what a node can see whenever the
cluster map is short or unusable. That is right for repair, where two
nodes with different views simply both send. It is wrong for reclaim, so
reclaim now requires the master's map — the one view every node adopts,
and therefore the only one that agrees by construction — and requires
that the map both covers the fleet and **lists this node**. Without
that, `pc_clplace_in_set()` answering 0 is ambiguous between "not a
holder" and "this map has never heard of you", and a node that has just
rejoined can hold an id an older published map predates. No map, no
reclaim: the surplus waits, which is the safe direction.

`pc_clplace_has()` now names that distinction in the placement module,
so a caller about to delete on a 0 has to decide rather than fall into
it.

Nothing else changed since rc2: the mode, the measurement and the
failure model are as that entry describes.


## 0.3.7-rc2 — 2026-09-13

**`spread` is complete: S127 Z1 through Z6.** Still a candidate — the
mode has never run on a production fleet — but every part of it is now
built and measured, where rc1 carried only placement and the write path.

**Upgrade from 0.3.7-rc1 if you are running `spread` at all.** rc1 could
DELETE a record it had just accepted. Placement prefers the cluster map;
a master that has published a map listing fewer nodes than are live
makes a genuine holder conclude it is not one, and exact-K retention
then drops the record on the write path. Every write returned `+OK`,
`stores` incremented, `entries` stayed at zero. For `shard` the same
short map costs a forwarded hop; here it cost the record. The map is now
used only when it lists at least as many nodes as the node can see live.

### The figure the mode exists for

Six nodes, ~11.6k writes/s, replication counters read per node:

| arm | fleet applies | per node | per **applying** node |
|---|---|---|---|
| eager, P=6 | 4.77× | 0.795 | 0.95 |
| spread K=3, P=3 | 2.00× | 0.667 | 1.000 |
| spread K=3, P=6 | 2.49× | 0.415 | 0.50 |

**At a fixed K, doubling the fleet halves each node's apply load.** Under
eager it does not fall at all whatever P is — one node's apply path
bounds the fleet however many nodes are added, which is what this mode
was built against.

### Reads

A miss unicasts the best-ranked holder instead of broadcasting: 53 pull
requests for ~53 misses where a broadcast produced ~159. A pull served
through a **non-holder is not cached** — keeping it re-created, through
the read door, the same orphan the write path avoids.

**libperfd 0.2.8** routes `spread`. Older libraries do not recognise the
mode, turn routing off and pay a forward per operation — measured at
400 of 400, with every write and read still succeeding. Correct, because
the daemon forwards and there are no MOVED redirects; a performance
floor, not a compatibility break. Nothing refuses an older client.

### Membership

A quiet node keeps its placement slots for 30s, so a blip does not
re-replicate its share. A real departure re-places its keys — which did
not happen at all before: survivors sat at 182 of the 240 owed,
indefinitely. When the node returns, the surplus is reclaimed and the
fleet settles back to exactly K×N.

Reclaim is a settled-only local pass, never a receive-time refusal: a
receiver whose view has not converged refuses copies it should hold, and
that measured 212 where 240 was owed.

### Also

`fwd_fails` — a **failure** counter for forwards that could not be sent
— has been maintained and never published for as long as it has
existed, and is now readable, along with `migrate_skipped_big` and the
three `spread_*` counters. A new `statlint` check fails the build if any
incremented counter is missing from the stats dump.


### `spread`: the read path, and libperfd 0.2.8 (S127 Z3)

A miss now unicasts the best-ranked holder instead of broadcasting: only
K of P nodes hold the record, so a broadcast asked P-1 peers a question
K of them could answer. Measured on four nodes, 120 reads with ~53
misses: 53 pull requests received fleet-wide, where a broadcast produced
~159.

A pull served through a **non-holder is no longer cached**. Keeping it
re-created, through the read door, exactly the orphan the write path was
taught to avoid: reading the whole keyspace through one non-holder took
it from its placement share of 67 back to all 120, and the fleet from
K×N to 293. The fleet now measures K×N both before and after.

**libperfd 0.2.8** routes `spread` (the holders are the top K of the
same rendezvous ranking, so shard is this with K = 1) and prefers the
best-ranked holder it already has a connection to. Older libraries do
not recognise the mode, turn routing off and pay a pull per read — they
remain correct, because the daemon forwards. Nothing refuses them.


## 0.3.7-rc1 — 2026-09-13

**A release candidate, not a release.** `spread` mode is half built: the
placement, the config and the write path are done and measured, the read
path and the client-side routing that make it usable are not. Tagged so
the full CI matrix gates the write path before the read path is built on
top of it.

### `mode = spread`: bounded replication (S127, Z1 and Z2 of six)

A record is held by **K nodes chosen by placement**, not by every node.
Eager is this with K = P and shard is this with K = 1, and the point is
apply load: under eager every node applies every write in the fleet, so
per-node apply equals the fleet's total write rate and one node's apply
path caps the fleet however many nodes are added. Under a copy factor
the passive work is (K-1)/P per node and **falls as the fleet grows**.

    [cluster]
    mode = spread
    replicas = 3

`replicas = 1` is refused rather than aliased to `shard`, and `replicas`
outside `mode = spread` is refused: two names for one behaviour is how
config drift starts.

**A non-holder does not keep what it accepts.** Any node may accept a
write — admission is not placement — but a node outside the top-K set
forwards it and drops its local copy. Retaining it would make every
writing node a soft K+1, and those extra copies are orphans: outside the
set, so the repair sweep neither maintains nor reclaims them. Measured
on four nodes with 120 keys: eager holds 480 copies, spread K=2 holds
240, which is K×N exactly.

**K is in the interchange digest**, so a member running a different K is
refused at the join rather than quietly placing records where the rest
of the fleet is not looking. It is folded only when set, so every fleet
that is not spread keeps the digest it had before and a rolling upgrade
does not split.

### Not in this candidate

The read path still serves a miss by pulling from any peer rather than
from a set member, and libperfd cannot compute the set, so a client
reading from a non-holder pays a pull. On a fleet of P nodes at K
copies that is most reads. **`spread` is not ready for production use
in this candidate** — it is tagged for CI, not for deployment.

## 0.3.6.1 — 2026-09-13

**A heap buffer overflow in the write path. Upgrade from 0.3.6.**

`clpush_append` sized a peer's gather buffer from the FIRST record it
ever held — `malloc(hdr + flush_bytes + n)` — and never grew it, while
the guard before each append tested `qlen + n` against `cap`, the wire's
gather ceiling, which has nothing to do with what was allocated. A small
first record followed by a larger one passed every check and wrote past
the end of the block. Reachable on any node forwarding writes of mixed
sizes.

The comment above that allocation asserted the invariant that made it
look safe — "the pre-append guard keeps qlen + n inside cap, so nothing
larger is reachable" — and it was wrong twice: `cap` is not the
allocation, and the guard does not run at all when the group is empty.
Writing an invariant down is not enforcing it. The allocated size is now
recorded and grown when a record needs it; a failed grow drops the
record rather than writing it.

Also in this release, 0.3.6 claimed cluster.c had no frame layouts left
as literal byte offsets. **That claim was false.** It was measured with a
pattern that matched only `pXX(buf + N)` and `memcpy(buf + N, …)`, and
missed pointer arithmetic and byte indexing entirely — so roughly twenty
sites went uncounted, including several where a parse had already
produced the value and the code went back to the wire for it anyway
(`pt + 23 + cn` for a key clfwd_op_parse had located, `pt + 19` for an
identity clmemb_joinreq_parse had located). Those are fixed, and the
bulk channel's handshake framing — `[len2][principal1]`, written out
three times — now has one definition in clbulk.

No wire change from 0.3.6.

## 0.3.6 — 2026-09-13

**A fleet on 0.3.5 must restart COORDINATED, not rolling, and each node
loses its stored identity.** Read this before upgrading.

The FNV-1a offset basis was wrong. It read `1469598103934665603`, one
digit short of `14695981039346656037`, in seven places. Nothing
misbehaved — the prime was right, so it hashed perfectly well — but it
was not FNV-1a and matched no published vector, and a hash is only ever
compared against another value from the same code, so no copy could
notice. Correcting it changes two things a fleet can see:

  - `member_digest` rides the MASTER_ALIVE keepalive. A 0.3.6 node and a
    0.3.5 node compute different digests for the same membership and
    will disagree. Restart the whole fleet together.
  - the `node-term` and `node-identity` files are checked with it.
    Existing files no longer validate, and **a node whose identity file
    fails its check refuses to start** rather than silently minting a new
    id. Delete the file to mint one deliberately; the daemon's own
    message says so.

Everything else here is internal. `cluster.c` went from 7,530 lines to
6,407, with fourteen modules carved out of it, each with unit tests that
run in milliseconds against no socket and no second daemon. Every wire
frame's build and parse now sit adjacent in one module rather than
hundreds of lines apart, and no frame layout is written down as literal
byte offsets any more.

(Corrected in 0.3.6.1: that sweep was incomplete. See below.)

No dialect, config or metric change. The wire is byte-identical to
0.3.5 apart from the digest above — which is asserted, not assumed:
each frame codec that moved is pinned by golden vectors captured from
the previous implementation, after a demonstration that a consistent
offset shift passes an entire round-trip suite untouched.

Fixes carried from the rc series: a dangling collection pointer in the
pull-response path, a doubled pend release, and a heartbeat that
re-armed the shard grace window (measured 59x on cross-node reads).

## 0.3.6-rc1 — 2026-09-12

`cluster.c` is 7,530 lines lighter by 776: seven of its planes are now
modules with their own unit tests, each running in milliseconds with no
socket and no second daemon. No wire, config or metric change. Three
behaviour changes are FIXES, below.

### Changed
- The peer table (`clpeers`), parked requests and completion queues
  (`clpend`), the locator and negative caches (`clloc`), the bootstrap
  decision (`clboot`), the write-path push groups (`clpush`), the sealed
  datagram and beat frames (`clwire`) and the bulk plane's framing and
  batch handoff (`clbulk`) each moved out behind an API, with
  `cluster.c` keeping the orchestration — anything that sends, logs or
  reaches the store. Every command's behaviour, and every wire format,
  is unchanged.

### Fixed
- The 520 completion-queue mutexes were being initialised 256 times each
  at startup. S105 added a per-peer loop here and S110 deleted its body,
  leaving the header standing over the queue loop; re-initialising a
  live mutex is undefined behaviour. It never bit because this runs
  before any worker exists.
- A bootstrap re-pick that found no candidate reported READY on the very
  next tick instead of waiting the beat its own comment promises: the
  deadline extension was overwritten two lines later. A node with peers
  worth pulling from now waits for them.
- `boot_tick` read `pending` and `round_failed` under two separate
  acquisitions of the same lock, so the two could come from different
  instants. One acquisition now.

## [0.3.5] — 2026-09-12

A shape release. The RESP door's dispatch and the cluster module's peer
scans were rewritten without changing what either does; no wire, config
or metric change is intended. One behaviour change is a FIX, below.

Cut from `v0.3.5-rc1`, whose tag pipeline was green 5/5 (clang-tidy,
check-fast, check, check-asan, matrix); the daemon differs from that rc
only in its version string.

### Changed
- The RESP door dispatches from a command table instead of a chain of 86
  `resp_is()` comparisons. `pc_verb_resp` is 129 lines, from 1,470: the
  44 commands are 29 handlers behind 44 rows. Every command's replies,
  its error strings and their codes are unchanged.
- The peer table is read through one liveness predicate rather than
  thirty open-coded loops (S139).

### Fixed
- A node that is still RECOVERING refuses `DBSIZE`, `FLUSHDB` and
  `FLUSHALL` with `-LOADING` again. Moving them behind the command table
  had put them in front of the readiness gate, so a node still pulling
  its bootstrap reported its keyspace and accepted a flush instead of
  refusing. Introduced and fixed inside this release — no tagged build
  carries it — and `readygatetest` now covers all three commands.

## [0.3.0] — 2026-09-12

A capability step over the 0.2 line, and the first tag a stock Redis
client can route against: ownership moved to the Redis slot
(`crc16(key) % 16384`) and the door answers `CLUSTER SLOTS`, `CLUSTER
SHARDS` and `CLUSTER KEYSLOT`, so a cluster-aware client places keys
itself instead of paying a forward for most of them.

What else the line brought, roughly in the order an operator meets it:

- **Collections at runtime** (S69): created, dropped, resized in either
  direction and renamed while the daemon serves, with the RESP door
  mapping a database index to a collection name.
- **A fleet that recovers itself**: a member joining an eager fleet
  empty pulls its bootstrap over the bulk plane before it reports READY
  (S83), the write-path push coalesces into one datagram per peer
  (S105), a failover probes a standby before adopting it (S104), and a
  joiner the master is holding hears `JOIN_WAIT` rather than silence
  (S108).
- **Refusals that say which one they are**: a forward that cannot be
  parked answers the retryable `TRYAGAIN cluster busy, retry`, one that
  was parked and never answered `ERR holder timed out`, and a node that
  is still recovering `-LOADING` (S138) - the code a Redis client
  dispatches on.
- **The arena is a ceiling on both carve paths** (S128): records and the
  tables holding them are bounded, `create` and `resize` price an index
  before refusing it, and growth takes 2 MB groups under an
  address-space reservation (S97).
- **Durability that reports itself**: `wal/CONTROL` is a durable record
  of the WAL's sequence and spans, and a node that destroys an
  acknowledged write leaves service instead of serving from a hole.
- **Operability**: a built-in fleet page, `/stats` on the HTTP door,
  per-dialect and per-listener counters beside the running totals
  (S123), replica-lag figures, a per-collection memory budget (S120),
  named threads, an optional query log, `perfload` (S113) and
  `perfdump` (S112), release tarballs per distribution and a Debian
  package.
- **RedisJSON on the door** (S86), which is what OpenSIPS's
  `cachedb_redis` sends at connect.

From this tag the README's compatibility promises hold: the binary
dialect's v1 is served indefinitely, peer-plane frames evolve
additive-tail only, an incompatible fleet is refused at join rather
than joined wrongly, and RESP2 tracks the de-facto standard.

The measured figures for this tag are in the README. Its container-route
tables were re-taken on 2026-09-12 with every benchmark cell sized to
run at least ten seconds, because redis-benchmark's threaded stop only
lands on its 250 ms progress timer and short cells therefore read low.

The thirty-four release candidates below carry the detail.

## 0.3.0-rc34 — 2026-09-11

rc33's daemon, byte for byte.  Two test repairs, one of which is why
rc33 certifies nothing.

### Fixed
- **`splitkeeptest` asserted a snapshot, not a property.**  rc33 went red
  on both GitHub runs and green on GitLab and the build host, on the same
  binary.  The suite checked the table's state at the instant the fill
  ended - load factor under 16, leg under half the table - but how much
  overflows during a burst depends on how fast the client can push
  relative to the maintenance tick, and that is a property of the
  machine:

  | runner | fill rate | leg | load factor | verdict |
  |---|---|---|---|---|
  | build host, plain | 140,706/s | 37% | 5.5 | pass |
  | build host, sanitizer | 112,406/s | 39% | 5.7 | pass |
  | GitHub, plain | 190,391/s | 50.4% | 7.1 | fail by 0.4% |
  | GitHub, sanitizer | - | 83% | 35.7 | fail both |

  The faster runner overflowed more, which is the mechanism working.  It
  now asserts that the table CONVERGES on its target load factor after
  the burst: seconds for the time-sliced splitter, about nineteen minutes
  for the flat budget it replaced, so the two are separated by two orders
  of magnitude instead of by four tenths of a percent.  The leg is
  reported rather than asserted.
- **`fwdtest` ran `taskset` twice a run, from inside a comment.**  Every
  `check` and `check-asan` printed `taskset: bad usage` twice and nothing
  in the tree appeared to call it.  The suite writes each node's config
  from an unquoted heredoc - it must be, so the node number expands - and
  backticks inside one are command substitution, so a backticked command
  name in a CONFIG comment ran while the comment was expanded, once per
  node, and its empty output was substituted into the file.  Single
  quotes.  No assertion moved.

### Notes
- No daemon, wire, config or metric change over rc33.  The binary is
  byte-identical; only test scripts differ.

## 0.3.0-rc33 — 2026-09-11

One constant: sixteen times the overflow chains, for no extra memory.

### Changed
- **`PCACHE_OVF_BUCKETS` 1,024 -> 16,384.**  A key lives in its bucket
  OR in the overflow leg, never both, so any operation that misses the
  bucket must walk the chain for its hash under the leg's single lock -
  and that includes every store, not only operations on records that are
  actually in the leg.  A table whose leg grew during a write burst
  therefore taxes all of its subsequent writes.

  Profiled on a node applying replicas at about 118,000 records a
  second, the leg lookup was **17.0% of the applying thread**, the
  largest cost outside the cipher and ahead of the store itself at
  12.1%.  After the change, **2.92%**.

  It costs nothing.  The head array is one allocation of 8 bytes a head,
  carved as a region: 1,024 heads is 8 KB and 16,384 is 131,072, and
  both fit inside the single 256 KB region slot the smaller one already
  occupied.  Verified - the index sizing returns byte-identical figures
  at every size from 2^4 to 2^24.  32,768 heads would need a second
  slot, which is why the number stops there.

  **Throughput is unchanged** in the test that measured it: 116,974
  applied records a second against a mean of 118,271 before, inside the
  noise, because the receiver was not CPU-bound there and had headroom
  already.  What this buys is that headroom on the thread that is the
  fleet's write ceiling.  A rig that can saturate a receiver would be
  needed to turn it into a throughput figure.

### Notes
- No wire, config or metric change.  A node running this release
  interoperates with rc32 and rc31 in every mode.
- The same change is in the shared cachedb_perf core, pushed to its pull
  request.

## 0.3.0-rc32 — 2026-09-11

A table that could not keep up with a write burst, and never caught up.

### Fixed
- **The splitter's budget was a flat 128 splits per maintenance tick**,
  once a second, whatever the deficit.  A burst therefore outran it and
  it never recovered at any useful speed: measured, 600,000 records left
  a table at load factor 102 against its target of 4, needing about
  nineteen minutes of ticking, and the splitter was watched running at
  exactly its cap - 115 to 129 buckets a second - for the whole of it.
  Everything that would not fit six to a bucket went to the overflow leg,
  which is one chain per hash bucket under a single lock, and which never
  drains back into the table.

  A count was the wrong bound: what matters is how long the maintenance
  thread spends splitting, because the expiry sweep, the arena reclaim
  and the resize tick all queue behind it.  It now splits in chunks until
  the table has caught up or a 20 ms slice is spent.  A table that is not
  behind pays one call that splits nothing, as before.

  **Measured, four paired runs of the same 600,000-record fill on a fresh
  node each arm:**

  | budget | fill | write rate | load factor | in the leg |
  |---|---|---|---|---|
  | flat 128 | 20.0-22.1 s | 27,119-30,012 set/s | 102-109 | ~567,000 |
  | time-sliced | 4.9-8.1 s | 74,509-121,530 set/s | 4.2-5.4 | 150,000-173,000 |

  So 2.5x to 4.3x the write rate on this workload, because a right-sized
  table keeps inserts out of the overflow path and its single lock.

### Added
- `make check-asan-findings LOG=<file>` scans a sanitizer log for
  findings, excluding the broken-locks negative control - which always
  produces one, by design, so a plain grep of such a log always matches
  and says nothing.  Both check targets now bracket that step so the
  exclusion is mechanical.
- `test/splitkeeptest.sh`, which drives a deficit large enough to tell
  the two budgets apart.

### Notes
- No wire, config or metric change.  A node running this release
  interoperates with rc31 in every mode.

## 0.3.0-rc31 — 2026-09-10

A memory bound one verb could walk through, and a lagging node that
looked healthy.

### Fixed
- **The arena's ceiling was enforced on one of the two carve paths.**  A
  chunk carve that leaves the reservation tests `arena_mb` and refuses,
  which is what makes `cache full` reach a client; a REGION carve - the
  index a table is built from - fell through to `shm_malloc` with no test
  at all.  So records were bounded and the tables holding them were not.
  Reachable two ways: `create {buckets_log2: 24}` on a 2 GB node left the
  arena holding 2.6 GB, and ordinary traffic reached it through a table
  split, which is what failed rc30's sanitizer run on the public runner
  at 278,688 bytes past a 64 MB ceiling.  A whole index now prices itself
  and tests the ceiling BEFORE its first carve, since regions are never
  freed and a half-built table would leave its segments behind.
- **A table larger than the segment directory is refused rather than
  written past.**  `pcache_htable_new()` allocated
  `nbuckets / PCACHE_SEG_SIZE` segments into a fixed 4,096-entry array,
  so a `size_log2` of 25 wrote past it and 32 was not a defined shift.
  Latent - every caller was gated - and now guarded, with `PCACHE_NSEGS`
  derived from the ceiling so the two cannot drift.
- **`[collection] buckets_log2` parsed 1..24 where the verbs took
  4..24**, so the same value was legal in a config file and refused on
  the wire.  4 is the floor at both.
- **The held-bytes walk mis-timed itself.**  It subtracted the two
  timespec components apart and cast the nanosecond difference to
  unsigned, so about half of all walks reported roughly 1,271 seconds.
  No symptom while nobody read the figure, but the pacing reads it - the
  walk sleeps twenty times its own cost - so the interval pinned to its
  maximum whatever the walk really took.

### Added
- **Replica-lag figures**, because a node that cannot keep up with
  inbound copies still heartbeats, stays a member and answers its own
  client door quickly: `rx_applied` and `rx_applied_ps`, `rx_older_ps`,
  `rx_drops` and `rx_drops_ps`, `rx_queue` against `rx_rcvbuf`, in
  `stats.cluster`, as seven metrics, and as a `replica intake` card.
  Drops are per SOCKET (`SO_RXQ_OVFL`), not the host-wide counter a
  second daemon would pollute, and the queue depth comes from
  `SO_MEMINFO` - `FIONREAD` on a Linux UDP socket answers the size of
  the first datagram, not the queue.  The sample is taken by the thread
  answering the operator, not on a tick: both background threads were
  measured stalling under exactly the load these figures report.
- **The overflow leg and the walk's cost per collection** -
  `overflow` and `held_walk_us` in stats,
  `perfcached_collection_overflow` and
  `perfcached_collection_held_walk_seconds` in the metrics, and an "in
  leg" column that turns warn-coloured once the leg holds more than half
  the table.  Measured: the leg holds 89% of a table under sustained
  writes, at every size from 100k to 900k entries.
- **`create` and `resize` say what an index would cost** when the arena
  cannot hold it, naming what it needs, what is held and the ceiling.

### Changed
- The status page's rolling history keeps 5 minutes, not an hour.  An
  hour on a card a few hundred pixels wide drew three samples per pixel;
  the shorter window also falls inside the branch that draws minute
  ticks, which never ran before.
- README's sizing section carries the measured index table.  The cost is
  flat at 1.5 MB from 2^4 to 2^12 - segments are fixed at 4,096 buckets -
  and about 192 bytes a bucket above it, so 2^24 is 3.0 GB before a
  single record.  The old text said "log2 12 under 1 MB", which was
  wrong.
- README says plainly that an eager fleet under overload is eventually
  consistent, with the repair sweep as the repair.

## 0.3.0-rc30 — 2026-09-10

rc28's daemon, byte for byte, with the memory suites' arena-mapping
reader finished.

### Fixed
- rc29 stopped the reader measuring a neighbouring mapping but only
  tried runs beginning at a group's first mapping, so where an anonymous
  mapping sits immediately before the arena - which is every host but the
  development one - it found nothing and every residency assertion failed
  on MISSING.  It now tries every adjacent sub-run, and its logic is
  exercised against four synthetic layouts: the arena alone between
  guards, a neighbour immediately before it, a pinned reservation split
  into three pieces, and no such mapping at all.  Verified on the build
  host, where rc29 failed, plain and under the sanitizer.

## 0.3.0-rc29 — 2026-09-10

rc28's daemon, byte for byte.

### Fixed
- tailtest and growtest identified the arena in `/proc/<pid>/smaps` by
  taking the most resident anonymous mapping within a few hundred kB of
  its size, and a sanitizer build carries others in that band - so the
  reader sometimes measured a neighbour instead.  That is why the rc27
  and rc28 tag runs failed and passed alternately on identical commits.
  Both now take the adjacent run of mappings summing to exactly
  `arena_reserved`, which S97 publishes, and say so rather than guessing
  when nothing matches.

## 0.3.0-rc28 — 2026-09-10

### Fixed
- S97's lazy reservation could hand out a huge-page arena the pool never
  backed.  The mapping is `MAP_NORESERVE`, so the pool is not charged at
  mmap and the initial commit has to be secured; the kernel was asked to
  populate it and its answer believed.  Under emulation that answer is a
  lie - `madvise` reports success without backing a page - so the first
  write took SIGBUS at startup, which is how rc27's arm64 build died.
  Every huge page of the commit is now checked with `mincore`, and
  anything unverifiable counts as unbacked: the daemon falls back to the
  next page tier and says why.  Proven on the arm64 leg that failed.

## 0.3.0-rc27 — 2026-09-10

rc25's daemon, byte for byte.  Four test suites made honest on a slow,
unprivileged runner and two made runnable on a host that lacks an
optional tool - which is why the full sanitizer gate could not reach its
end here, and why the public runners were finding these instead of this
machine.  Library unchanged (0.2.7).

### Fixed
- probetest counted the overflow leg by subtracting an estimate of the
  last bucket's slots, and over-subtracted into a negative count when the
  table grew between two reads; S121 gave the leg its own cursor space, so
  it is now counted exactly, on a settled table.
- tailtest's bounded waits for the give-back go to 45 and 60 seconds, and
  its failures carry the released bytes and the give-back latch - a
  one-hertz maintenance duty under a sanitizer on two processors gets a
  fraction of the ticks it gets here.
- dumptest skips its compressed case where the `zstd` binary is absent,
  and noiseinterop skips where the host's python `cryptography` cannot
  drive the handshake, rather than reporting either as a failure.

## 0.3.0-rc26 — 2026-09-10

rc25's daemon, byte for byte, with two test suites made honest on a
slow, unprivileged runner.  Library unchanged (0.2.7).

### Fixed
- legwalktest asserted every record once while the maintenance thread was
  still splitting buckets, which is the walk's at-least-once contract
  behaving as documented; it now settles the table first, as dumptest
  does.  growtest asserted the arena mapping's resident bytes under a
  sanitizer that intercepts `mlock()`, reports success and populates
  nothing; the two residency checks now skip loudly when the kernel's
  `VmLck` says no pin happened while the daemon logged one.

## 0.3.0-rc25 — 2026-09-10

Collections that no longer need a config edit and a fleet restart, an
arena whose whole cap is one tier, and a cooperative walk that no longer
hands out its overflow leg in one piece.  Library unchanged (0.2.7).
Gate: the touched suites plain on the build host, then the tag pipeline.

### Added
- S69: collections created and dropped while the daemon serves.  `create
  {col, buckets_log2?}` and `drop {col, force?}` on the JSON door and in
  perfcli, gated by `[daemon] allow_create` (off by default, and it gates
  origination only, so a create still reaches every member and turning
  the gate on needs no fleet restart).  A created collection takes the
  cluster's mode.  The created set is persisted under `state_dir` and
  loaded before the replay and the snapshot import; the WAL carries the
  create and the drop as records of their own; a create or drop is
  announced to every live peer and the whole set is re-announced every
  few seconds, so a node that missed one converges.  A Lamport
  generation per name orders a create against a drop across the fleet.
- S69: a collection can be resized in either direction and renamed while
  it serves.  `resize {col, buckets_log2}` fills a second table at the new
  size from the live one on the maintenance thread, swaps the two in one
  step and collects the swap window's stragglers; deletes reach both
  tables while it runs, so a key deleted after the copier passed it is not
  resurrected.  `rename {col, to}` is one pointer swap, and the WAL
  carries it so a replay meets the old name before the rename and the new
  one after.  Both are announced to the fleet.  `perfcli resize`/`rename`.
- S69: the RESP door maps a database index to a collection name.
  `resp_collections = 0:sbcha, 1:th` makes `SELECT 0` reach `sbcha`; a
  bare name keeps its old meaning.  Until now a collection had to be
  NAMED `0` to be reachable, so a node holding live data reported one
  empty database to every Redis-native monitor - `DBSIZE` zero, `KEYS`
  nothing, `INFO keyspace` a flat line through an outage and a normal
  day alike.  `INFO keyspace` now reports every collection the door can
  reach with its real key count.  coltest.
- S97: growth by 2 MB group under an address-space reservation.  The
  arena reserves the cap (`arena_cap_mb`, else `arena_mb`) as address
  space charged for nothing, commits `arena_mb` at start, and commits
  the rest group by group as a carve reaches it - on the same tier,
  never as 4K pages - populated with the kernel's answer so an empty
  hugetlb pool is a refusal, not a SIGBUS; a punched group drops its
  commit, and a pinned arena gives back too (a run is unpinned before
  its punch).  `stats.memory.arena_committed` / `arena_reserved`,
  metrics `perfcached_arena_committed_bytes` /
  `perfcached_arena_reserved_bytes`.  growtest.

### Fixed
- S121: the cooperative walk budgets the overflow leg instead of draining
  it whole in its final call.  A table at load factor four keeps some
  percent of its records in the overflow chains (nine percent of a
  million), and `scan`, `keys`, the replication sweep's walk and the held
  walk all paid the whole leg in one step; the cursor space now has a
  position inside the leg and each call emits about `count` buckets'
  worth of records from it, whole chains at a time.  Fixed upstream
  first (cachedb_perf) and ported.  `dump`'s tail phase and its
  `tail_limit` are gone: the ordinary cursor carries the leg.
  legwalktest.

## 0.3.0-rc24 — 2026-09-09

rc23 plus the open-connection gauges and the stats reset, and two suites
made independent of the runner's privileges.  Library unchanged
(0.2.7).  Gate: the touched suites plain and under the sanitizer on the
build host, clang-tidy clean on the changed files, the tag pipeline.

### Added
- S123: the doors report what is OPEN now beside the running totals, and
  the totals can be reset.  `stats.native.<dialect>.open` and
  `stats.resp.open` (gauges, metrics `perfcached_connections_open` by
  door and dialect); `stats.since` says what the totals count from.  A
  reset - the JSON verb `reset_stats`, RESP `CONFIG RESETSTAT`, the
  page's "reset stats" button (`POST /reset-stats`, the one mutating
  HTTP route, body-less, token-guarded like the rest) or `perfcli
  reset-stats` - starts every running total again: doors and dialects,
  the collections' table counters (the core's own re-baseline), the
  store's size tallies, the cluster, proxy and WAL counters.  Gauges
  stand: open connections, entries, buckets, memory, sequence numbers,
  roles.  `/metrics` keeps the raw totals (Prometheus counters stay
  monotonic).  The page's door cards lead with "open" and label the
  totals "since start" or "since reset".  openstatstest.

### Fixed
- tailtest runs the daemon plainly when not root (an unprivileged runner
  cannot apply a bounding set, and cannot pin past RLIMIT_MEMLOCK
  either); stalebackfilltest waits for the restarted node's join line
  instead of reading the log once.  Both caught by the public runner
  after rc23.

## 0.3.0-rc23 — 2026-09-09

rc22 plus the loader (the reverse of rc22's dumper), and a fix in the
core's existence probe.  Library unchanged (0.2.7).  Gate: the whole
suite plain and under the sanitizer on the build host, the two tool
suites with leak detection on, clang-tidy clean on the new tool.

### Fixed
- S122: the existence probe reported every record in the overflow leg
  as absent - RESP `EXISTS` and `TYPE` answered nothing for a third of a
  2,000-record table in 256 buckets, the binary `exists` likewise, and
  the proxy-mode holder test on `set`/`del` took a key held in the leg
  for one to forward.  Fixed upstream first (cachedb_perf) and ported
  to the vendored core; probetest.

### Added
- S113: perfload, the loader, and the `restore` verb behind it.  A
  batch of records installed with their own version and absolute expiry
  through the write path (WAL, push), so a fleet of any size or mode
  takes a dump as if a client had written it; policies newer, skip,
  overwrite; expired-at-load skipped and counted; a key another node
  owns refused and named, never forwarded, and the loader re-sends it.
  The tool verifies every chunk first, streams N routed connections,
  resumes by a done file, maps collections, paces, dry-runs, and says
  when the members have caught up.  1M records in 0.8 s on 8 threads
  (1.4x plain pipelined SET); loadtest.
- S112: perfdump, a parallel dumper over the client door, and the `dump`
  verb behind it.  The verb yields a chunk of whole records (key, value,
  ttl, version) from a bucket-bounded key scan and one atomic fetch per
  key, so a chunk never repeats a record; `count` is buckets, `end`
  bounds a bucket range, a slot range splits a keyspace across shard
  nodes, the overflow leg comes out over bounded chunks, and the reply
  grows on the heap.  The tool walks N bucket ranges per collection into
  PCD1 chunk files with checksums and an atomically rewritten manifest,
  compresses through the zstd binary, paces with `--rate`, and
  `--inspect` verifies a dump; doc/perfdump-format.md.  Measured on the
  build host: 704,000 records a second on eight threads.

### Fixed
- walobstest asserted that the WAL fsync detector stays quiet on
  "unthrottled storage" - an assertion about the host, which a
  concurrent CI job made false; it now asserts the flag agrees with the
  figures it is computed from, in one snapshot.

## 0.3.0-rc22 — 2026-09-09

rc21 plus the six page and memory-accounting tasks filed from the
operator's evening on the fleet, and a test-harness sweep.  Library
unchanged (0.2.7).  Gate: the whole suite plain and under the sanitizer
on the build host (67 suites clean on each, the new tail suite
included), every task's suite watched fail first against the rc21
daemon before it passed on the fix.  On the fleet this brings each
node's resident memory down by about 70 MB: the never-carved part of
the reservation goes back to the host.

### Changed
- S117: the headroom a member advertises in its beat - what the fleet
  cards divide and what placement reads - is the ceiling's: `free_mb`
  and `total_mb` carry ceiling minus held, and the ceiling.  They used
  to be the slot pool's, which rose when records were freed and fell
  when the give-back returned their pages to the host, so a node that
  had just handed 84 MB back read as nearly full beside an arena card
  saying 60 %.  Same fields, same units; heldtest asserts each node is
  seen at the headroom it reports for itself.

### Added
- S120: a memory budget the operator can size from.  `stats` reports each
  collection's index regions (exact, noted at creation and growth) and
  its records as the cells they occupy (the walk rounds every record to
  its class), plus a `budget` block against the ceiling; `/metrics` the
  matching gauges; the page a budget card beside memory.  The README's
  new "Sizing" section is the same arithmetic done before the daemon
  exists: index per bucket, the cell ladder, the residue after a burst,
  the refuse-not-evict rule, and what the host sees.
- S115: the trend cards carry axes.  Y labels beside each plot, one per
  gridline, with percentages and counts anchored at zero; time ticks
  under the plot and a mark per minute on short windows; a peak dot
  with its time; a pointer readout with the value and time of the
  sample under the cursor.  A spike's height and duration read off the
  card.
- S116: the sizes column shows what is HELD.  The maintenance thread's
  walk bins every record's value length into the eight classes the
  column already draws and publishes them as `held_hist` beside
  `held_bytes`, so a member whose records all arrived by replication
  shows the same bars as the node that took the writes; the column used
  to count this node's own client writes and stayed blank on every other
  member.  `size_hist` keeps that meaning in `stats`.
- S118: `held` is exactly its parts.  `stats` gains `arena_class_chunks`
  (the chunks the size classes own - live records inside, free cells
  that belong to the class) and `arena_page_slack` (the alignment slot
  of every shm page); `/metrics` the matching gauges; the memory card
  the rows, so structure + class chunks + warm free (+ page slack)
  equals held on every refresh.  ceilingtest asserts the identity
  byte-exact at start, at the ceiling and after the give-back.

### Fixed
- S119: the reservation's never-carved tail is given back.  The arena is
  populated whole at start and the give-back walked only the groups
  below the chunk frontier, so a node that never needed its whole
  reservation kept the rest resident for the life of the process (the
  fleet: 70 MB beside 102 MB held).  The give-back tick now punches the
  groups between the two frontiers once, under its usual latch and
  cooloff; `reclaim.tail_released` says how much.  A new suite starts
  the daemon unpinned and asserts the mapping's resident size from the
  kernel, not a counter.
- Every suite killed its daemons by a pattern bound to the binary's
  name; a run against a renamed daemon outlived its suite and held the
  ports against the next runs.  Every suite now records the pids it
  starts and kills those, each checked against its command line first,
  with the pattern kept as a fallback.

## 0.3.0-rc21 — 2026-09-09

rc20 plus the remedy for the eager write ceiling rc20 measured, and a
test-harness repair.  Library unchanged (0.2.7).  Gate: the whole suite
plain and under the sanitizer on the build host (66 suites clean on
each), the failover suite twice more on its own, and the eager suite's
coalescing assertion watched fail first against the mutex it replaces.

### Changed
- S110: no shared lock on the eager write path.  The push group is per
  worker thread per peer, appended without a lock; the maintenance
  thread asks the workers to flush through a rate-limited broadcast
  instead of taking their groups from them.  On the build host at fifty
  clients the ceiling moved from 349,000 to 1,044,059 SET/s (3.0x; 2.0x
  the same run's Redis at 515,000), p50 2.5 ms, p99 8.8 ms; the futex
  calls per worker per drive fell from 150,000 to 4; one peer alone
  reaches 1,287,001.  The next ceiling is the receiver's single cluster
  thread at full load, filed and not built.

### Fixed
- failovertest's exit trap killed only its first fleet; an early exit
  left the routing fleet's daemons running and blocked the next job's
  busy guard for a quarter of an hour.  The trap covers both fleets and
  the member client.

### Measured, not changed
- Give-back inside a fixed reservation (no cap): a burst of 100,000
  records of four sizes (62.9 MB of values, 94 MB live once cell-rounded)
  took each node from 90 to 186 MB held; after the TTL the sweep cleared
  it within 15 s and the reclaim handed 88-90 MB per node back to the
  kernel, RSS 261 to 173 MB.  Thirteen such bursts in a row: the peak
  identical every time, the settled floor bounded at 101-103 MB, no
  write refused.

### Filed
- From the operator's evening on the fleet: the trend cards carry no
  axes (S115); `sizes` counts client writes on this node only, so a
  push-fed member shows nothing for 100k records it holds (S116); the
  member cards' headroom is the pool's free share and drops when the
  give-back returns memory (S117); the memory card's parts do not add up
  to `held` - the class-owned chunks have no row (S118); the never-carved
  tail of the reservation stays resident, so RSS does not follow `held`
  (S119); a memory budget the operator can size from (S120).

## 0.3.0-rc20 — 2026-09-08

rc19 plus the operator's three reports from the fleet and the first
measurement of the eager write ceiling.  Library unchanged (0.2.7).
Gate: the whole suite plain and under the sanitizer on the build host
(66 suites clean on each), the two new suites failing first against
rc19's daemon.

### Added
- S109: the page's collection size is a figure, not an estimate.  A walk
  in the maintenance thread sums every record's key and value bytes per
  collection, publishes the sum with its age, and paces itself at twenty
  times its own cost between 5 s and 60 s; `held_bytes` and `held_age_s`
  in `stats`, a `perfcached_collection_held_bytes` gauge, the column
  "held" on the page - real on a node whose records all arrived by
  replication, which used to show a dash.  The vendored table core is
  untouched (a first cut that counted inside it was reverted).
- S114: `held` says what it is made of - `arena_regions` (the index
  regions, carved at creation and on growth, never freed) and
  `arena_warm_free` (free slots kept resident) in `stats`, `/metrics`
  and the page's memory card.
- The daemon's threads are named (pc-w<n>, pc-cluster, pc-bulk, pc-beat,
  pc-maint, pc-wal, pc-rdb) so top, perf and a core say which is busy.

### Fixed
- S114: `at_ceiling` stayed true until a chunk carve succeeded, which a
  drained table never needs; the fleet showed "at ceiling" for ten hours
  beside half an arena of headroom.  The spell now ends at the give-back
  tick as soon as a carve would succeed.
- S111: the connection close line and the connections page counted RESP
  commands only; JSON and binary links closed with "0 requests" whatever
  they carried.  Every dialect counts now.

### Measured, not changed
- S110: the eager write ceiling at the README's load shape is 330-381k
  SET/s, latency-bound.  The receiver's cluster thread is at half; the
  sender spends 5.3 us of CPU per record against 2.55 standalone and
  meets a seal held under the per-peer group lock some 3,300 times a
  second.  The remedy - a per-peer push thread fed by per-worker rings -
  is filed, not built.

## 0.3.0-rc19 — 2026-09-07

rc18's batch with the two fixes its tag pipeline demanded.  No library
change (0.2.7); one two-byte fix in the RESP door and one test-harness
repair.  Gate: the touched suites plain and the whole suite under the
sanitizer on the build host (64 suites clean) before the tag - rc18 had
been tagged on a plain run alone, and the sanitizer job is exactly what
caught it.

### Fixed
- `JSON.DEBUG HELP` sent 55 bytes of a 53-byte literal - two bytes of
  whatever followed it in read-only data, silent in a plain build and
  an abort under the sanitizer, which took every later RESP reply with
  it.  Literal replies now take their length from the compiler.
- stalebackfilltest asserted that the sender had dropped its stale
  backfill flag at the instant the other survivor's drop appeared; each
  node drops on its own tick, and the runner's concurrent sanitizer job
  widened that gap.  The test polls for each survivor.

### Filed
- S112 perfdump and S113 perfload (a parallel dumper and loader over the
  client door, mydumper/myloader shape, with `dump` and `restore` verbs);
  S114 (`at_ceiling` stays set until a chunk carve, and the structural
  part of `held` is invisible).

## 0.3.0-rc18 — 2026-09-07 (red on its tag pipeline: a test race and a sanitizer over-read; superseded by rc19)

The batch since rc17: an empty joiner pulls its own bootstrap, the
write-path push coalesces, a client recovers members in the background
and is told when membership changes, and the RESP door speaks
RedisJSON.  libperfd 0.2.4 to 0.2.7.  Gate: the full suite on the build
host (64 suites, plain) and failovertest 51/51 twice; the OpenSIPS
module built from libperfd 0.2.7 runs on six test nodes.

### Added
- S83: a node that joins an eager fleet empty PULLS its bootstrap over
  the bulk plane from a ready peer (records first, lowest id) and is
  ready only when the stream ends; writes that land during the pull
  ride the write-path push; a founder with live peers pulls too.
  Counters `boot_out`, `boot_in`, `boot_failed`; boottest.
- S107: libperfd re-dials a member marked down in the background (1 s
  doubling to 30 s) while every request keeps flowing; every node tells
  its clients `joined` / `expelled` over the push channel, and the
  library dials the newcomer or drops the expelled node completely.
  `perfd_maintain()`, `perfd_recovered()`.
- S105: the write-path push coalesces into one datagram per peer (48 KB
  or 3 ms, whichever first) carrying the message the sweep has always
  sent, so an rc17 receiver reads it; eager SET at depth 64 went from
  289k/s to 349k/s.  Counter `repl_groups`.
- S86: `JSON.SET`, `JSON.GET`, `JSON.DEL`, `JSON.NUMINCRBY`,
  `JSON.ARRAPPEND` and `JSON.DEBUG` on the RESP door, with RedisJSON v2
  reply shapes, `NX`/`XX`, `WRONGTYPE` on a non-JSON value, and `EX` as
  an extension.
- S104: a failover probes a standby before adopting it and re-dials any
  learned member; SO_KEEPALIVE on every link; idle standbys are pinged
  and a dead one retired (libperfd 0.2.4, 0.2.5).
- S106: the identity and mastership-term files are text, versioned and
  checksummed; a damaged file refuses the start; legacy files migrate.
- S108: a joiner the master holds hears `JOIN_WAIT` instead of silence
  and does not found a fleet of its own.
- `make check` refuses to start if a suite in `test/` is not wired in.

### Fixed
- S108: the master read a joiner's identity two bytes off, so a copied
  identity was always admitted; and two claimants of a lapsed identity
  are told apart by the identity's last home.
- Found by S107's verification: a membership push reached only the
  clients of one worker (the RPC indexed the thread table by a worker's
  public idx, which is its slot plus one); a node restarted at the same
  address under a new identity was never announced; a call routed to a
  standby ran none of the handle's maintenance.
- The JSON value validator accepted any bare word as a primitive, so
  `JSON.SET` could store non-JSON; a primitive is `true`, `false`,
  `null` or a number.
- Test harnesses: waluniformtest polls instead of sleeping (red on
  GitHub's slower runner); eagertest's empty-restart leg and
  failovertest's sampling assertions assert properties, not moments.

### Changed
- The `members` reply carries the bootstrap and push-group counters.

### Open, filed
- S109: the page shows no size for a collection whose records arrived
  by replication.  S110: eager writes at depth 64 still run at a fifth
  of the store path.  S111: the connection close line counts RESP
  commands only.

## 0.3.0-rc17 — 2026-09-07

rc16's batch with one more test-harness repair.  No daemon or library
change; rc16's plain test job passed every suite, and the repaired
suite ran on the build host plain and under the sanitizer before the
tag.

### Fixed
- rc16's sanitized test job failed eagertest's empty-restart leg, which
  asserted a zero entry count on the restarted node immediately after
  start.  Nothing persists in that fleet, so the count is non-zero only
  once a peer's refill lands, and every pipeline run has that refill
  landing inside the first sample; the slower sanitized daemon lost the
  race.  The leg now asserts the node's own start kind, "cold", from
  its members entry, the property rather than the outcome.

## 0.3.0-rc16 — 2026-09-07 (red on that harness leg under the sanitizer; superseded by rc17)

rc15's batch with its test harness repaired.  No daemon or library
change; the first candidate since rc13 whose changed suite ran on the
build host before the tag.

### Fixed
- rc15 failed both test jobs on one assertion of the harness's own
  making: the failover test's spread leg expected the main fleet to
  keep two live nodes, and the standby-kill cue added in rc14 leaves it
  one, so round-robin put all twelve clients on the survivor.  The
  spread leg now runs against the fresh three-node fleet the routing
  leg starts, which starts before it.

## 0.3.0-rc15 — 2026-09-07 (red on that harness leg; superseded by rc16)

rc14's batch, compiled.

### Fixed
- rc14 failed its pipeline seven seconds in: the two writes of an error
  string into the 64-byte member reason field lacked the explicit bound
  that tells the compiler the truncation is meant, and the pipeline's
  `-Werror` rejected them.  The local pre-push check had been
  `-fsyntax-only`, which skips the optimiser and with it that whole
  warning class; it is a real compile now.

## 0.3.0-rc14 — 2026-09-07 (never compiled; superseded by rc15)

The client library learns to say what it found, and to notice a link that died while idle.

### Changed
- libperfd 0.2.4: TCP keepalive on every link (`keepalive_s`, 30 s idle,
  10 s interval, three probes) and an idle ping on standbys
  (`idle_ping_ms`, 30 s) that retires a dead one and marks its member
  down with the reason, so a failover never adopts a link that died
  while idle; every link carries the time of its last accepted reply.
  The first slice of S104.
- libperfd 0.2.3: `perfd_server_version()` and `perfd_member_state()`,
  so a client can say what it found without `ss` on the cache hosts;
  the `members` reply carries the daemon's `version` and `rev`.  S70.

## 0.3.0-rc13 — 2026-09-07

rc12's red, root-caused: the last "two senders" defect.

### Fixed
- A peer's record count and start kind are written only by its own
  heartbeat.  The master's keepalive publishes the master itself, and a
  JOIN or an ASSIGN list publishes the members they name, through the
  same path with no identity, zero entries and an unknown start kind,
  and the peer table stored the zeros.  So every non-master's view of
  the master flapped once a second between what its heartbeat said and
  nothing.  The backfill sender election reads that view; at the wrong
  phase it saw the lowest id as empty, skipped it, and the reader
  walked the backfill as well - two senders for one backfill in every
  CI run of eagertest and stalebackfilltest, never on the build host,
  where the two datagrams happened to land the other way round.  What
  a caller does not know it no longer writes.  This is also every
  "two senders" observation in the ledger since S82.

### Changed
- `members` reports, per member, this node's view of its record count
  and start kind, the values the sender election decides from, so a
  flap is visible to an operator rather than inferred from a second
  walker.
- stalebackfilltest probes that view twenty times and requires it
  steady.  A view lags the peer's own count by up to one heartbeat, so
  the probe waits a beat after the fill; the first cut did not and
  read the lag as a flap.

## 0.3.0-rc12 — 2026-09-06

Test-only over rc11; the daemon is byte-for-byte rc11.

### Fixed
- eagertest sampled its quiet window early.  The wait for "the sender's
  completion" grepped node 2's whole log for any `backfilled node …
  after its restart`, and node 2 already carried that line from node
  3's rejoin minutes earlier, so the wait was satisfied instantly and
  had been inert since rc9.  The window then opened when node 1's
  record count was reached, which a second sender's copies can satisfy
  while the first sender's cycle still has a slice to collect: 618 →
  766, one slice, on rc10 and rc11.  The daemon sent nothing after its
  real completion.  The window now opens on the restarted node's own
  completion, from a line written after its restart, allows one sweep
  period for a second sender, and requires every node's counter flat.
  Proven on the build host with the node logs kept.
- stalebackfilltest restarted its target while every node was still
  inside its 30 s cold window, which makes two senders legitimate; it
  now waits the window out, truncates the log on a restart, and awaits
  the sender's completion rather than asserting it.

### Filed
- S103: a second sender walked a backfill in rc11's CI run.  Tolerated
  by design during the holdoff; twice the traffic, not a fault; cause
  unknown without node logs.

## 0.3.0-rc11 — 2026-09-06

rc10's red, root-caused; and the arena's give-back made honest.

### Fixed
- A stale backfill flag fired when the ids reshuffled.  Every live node
  arms a backfill for a peer whose heartbeat says it started cold, but
  only the lowest live id walks it and only the walker clears the flag;
  on every other node it stayed armed.  When the lowest live id later
  left, the stale holder became the designated sender, pushed the whole
  keyspace at a node that had been full for minutes, and logged
  "backfilled node N after its restart" for a restart long past.  That
  was eagertest's 148, the same first 4 MB slice from bucket 0 on
  alternate tags, and the false completion line seen on the fleet.  A
  walk now starts only while its target still reports itself cold; one
  in flight runs to completion; a flag whose target is established is
  dropped with a notice; a reused peer slot no longer inherits its last
  occupant's flag.  Not covered, filed against S83: a sender dying after
  the target's cold window leaves it partial with nobody armed.
- A punched-out hugetlb group was re-committed by faulting.  On tier 1
  the punch had returned that page to the kernel pool, and with the
  pool empty the re-fault was a SIGBUS.  The group is now prefaulted
  with `MADV_POPULATE_WRITE` first (a kernel too old for it consults the
  pool's free count), and a refused re-commit is counted as
  `pool_empty` while the carve carries on to the 4K overflow.  A cold
  group also comes back whole, all eight slots.
- The give-back re-faulted what it had just released.  After a
  successful punch the code rebuilt the eight slot headers "cold",
  eight writes into the memory just given back: on hugetlb that took
  the whole 2 MB page straight back, in a `MADV_HUGEPAGE` region it
  asked for a THP.  `cold_bytes` said cold while RSS said resident.  A
  cold slot now has no header; the cold map is the only truth for it,
  and the header is rebuilt when the group is secured and re-committed.

### Changed
- Give-back is three phases: decide and mark under the arena lock,
  issue the syscalls with it dropped, book what happened under it
  again.  A group in flight sits on neither the warm nor the cold map,
  so a carve cannot take a slot the punch is about to zero.  Runs of
  consecutive groups go out in one `madvise`; `punch_calls` and
  `punch_groups` show the ratio.
- Give-back is bounded per tick by `[memory] shrink_step_mb`, groups
  and pages together; unset, an eighth of the ceiling and never below
  8 MB.  Reported as `reclaim.shrink_step_bytes`.
- The ceiling is a latched state: `stats.memory.at_ceiling` and
  `at_ceiling_since`, set on the first refusal and cleared by the next
  successful carve; the arena card turns red and says for how long.
- The overview card reads "members up 3/3", the fleet's view, instead
  of "peers up 2", this node's.
- `arena_profile` was never an accepted key here; configtest now pins
  its refusal so it cannot become accepted-and-ignored later.

## 0.3.0-rc10 — 2026-09-06

The fleet showed a defect no suite could: every test starts its nodes
into a cluster that already has a master.

### Fixed
- A founding node discarded the id its own identity proposes.
  `ident_proposed_id()` hashes the persisted identity into 1..1023, and
  a master honours that proposal for any joiner whose id is free - that
  is what carries an id across a restart, and what the identity file
  exists for.  `become_master()` never called the allocator: a node that
  found no cluster to join took id 1 and handed out ids from there.  The
  proposal was therefore honoured in exactly the case where a master
  already existed, and discarded in the case where the node was deciding
  for itself.  A whole-fleet restart renumbered the fleet around
  whichever node happened to boot first, and two partitions that each
  found would both claim 1 for two different identities, to be told
  apart on the merge.  **1 remains a legal id**: it is what an identity
  hashing to 1 is granted, and what a node with no durable identity
  still falls out of the allocator's scan as.  It was 1 being the
  default, not the value, that was wrong.  `test/statedirtest.sh` case 6
  covers the founding path, which the suite had never exercised.

### Changed
- The status page reports what durability is DOING rather than that it
  is configured.  `wal on` and `rdb on` named a feature and said nothing
  about whether it was keeping up: a WAL dropping records, overrunning
  its fsync budget or running out of free segments looked exactly like a
  healthy one.  Two cards now carry the counters - records appended and
  dropped, late syncs, overruns, free segments, unsynced records, the
  storage class, fsync now and the probe's percentiles for the WAL;
  snapshots taken, whether one is running, and the last one's age, size
  and duration for the RDB.
- The history graphs say what window they cover.  A series was plotted
  with no time axis at all, so a reader could see a trend without
  knowing whether it spanned five minutes or three hours.  One constant
  now drives the sample timer, each graph's footer and the heading hint,
  so the page cannot describe a cadence it is not keeping.
- The graphs are twice as tall and ruled at the quarters.  At 40px
  successive samples sat within a few pixels of one another: the trend
  was visible and the shape was not.

## 0.3.0-rc9 — 2026-09-06

rc8's gate found a real defect this time, in the rule S82 added.

### Fixed
- A backfill sender had to have restarted a while ago; it did not have
  to hold anything.  The S82 holdoff skips a candidate whose backfill is
  armed and whose fresh stamp is inside 30 s, but emptiness is a state
  and that is a timer: a node whose own fill outlasted the holdoff
  became eligible while still empty, took the role as the lowest id,
  handed over the little it had and cleared its flag - the failure the
  holdoff exists to prevent, one timeout later.  The election now skips
  a candidate reporting zero records, or reporting itself cold, at any
  age; both facts already ride the heartbeat, so there is no new state
  and no wire change.  A candidate holding part of the keyspace while
  still filling remains uncovered and is S83.


## 0.3.0-rc8 — 2026-09-05

rc7's own gate found a false alarm in the test suite rather than a
defect in the daemon; this is that fix, and it is the only change.

### Fixed
- `test/eagertest.sh` started its post-backfill quiet window when the
  restarted node's record COUNT reached the target, but the count is
  reached when the last record lands while the backfill flag clears only
  after a whole cycle has walked cleanly as the sender.  The remainder of
  the in-flight cycle - one 4 MB budget slice - fell inside the window
  and was read as an echo, failing rc5 and rc7 with an identical 342 ->
  490.  The window now opens when the sender reports the backfill
  complete, and the test fails if that report never comes.


## 0.3.0-rc7 — 2026-09-05

Browsing, visibility and identity: the things an operator reaches for
after connecting.  None of it changes the data path.


### Added
- `collections` verb and perfcli command; `keys` with no collection or a
  glob lists every collection the glob names, grouped, bounded across the
  whole result; a single-collection listing names its collection and says
  when it is one node's share of a placement-spread collection; `exit`
  and `quit` end a perfcli session.
- `stats.listeners` and a listeners card on the page: every configured
  door with its kind, address, port, plaintext or not, allow-list size and
  whether a secret is set.  A RESP client on the native door is named in
  the log, with the RESP door's address.
- The query log line says hit, miss, ok or err.
- `stats.cluster.reserved_ids`: a member that leaves keeps its id reserved
  for an hour; a returning identity, or a replacement at the same address,
  gets it back, and a newcomer is not handed it meanwhile.
- Per collection, what is being stored: `stored_bytes`, `stored_n` and a
  log2 `size_hist`, kept at store time from the write stream rather than by
  walking the table.  The collections table gains an estimate of memory
  held (entries x mean stored size, labelled as the estimate it is) and a
  size histogram.


## 0.3.0-rc6 — 2026-09-05

The release rc5 should have been.  rc5's tag pipeline found a real
defect under load - one late acknowledgement made a backfill re-push the
whole keyspace - and this fixes it, together with the batch that
followed rc5: perfcli that names the mistake, a daemon that says who
connected, a heartbeat that carries the HTTP door and uptime, a RESP door
that tells cluster-aware clients the truth and serves the slot map from
a cache, a fleet that is uniform about the WAL, and the licence
boundary checked on every push with the export carrying its notices.

### Fixed
- A late acknowledgement during a backfill or sweep dirtied the whole
  cycle, and the next cycle re-walked the entire keyspace with the
  passive copies included - the echo the passive rule exists to prevent,
  arriving by another door.  A batch whose ack is 1.5 s overdue is now
  re-sent, up to twice, from the window's own bookkeeping; a loss is a
  peer that did not answer three times in five seconds.  `migrate_retx`
  counts re-sends beside `migrate_lost`.
- `INFO server` said `redis_mode:standalone` and `INFO cluster` did not
  exist while `CLUSTER INFO` said the cluster was enabled, so no
  cluster-aware client ever fetched the slot map.  Both now derive from
  the one function `CLUSTER INFO` uses; a clusterless node still says
  standalone.

### Added
- `stats.native` and a card per dialect were rc5; this adds the
  connection log: one NOTICE when a client's dialect settles (binary,
  JSON, RESP, or the RESP door; encrypted or plaintext) and one when it
  closes, with the reason - peer closed, quit, read or write error,
  protocol error, or a handshake that never happened for want of a
  secret.  Twenty lines per ten seconds, then a count.  HTTP polls are
  never logged.
- The heartbeat carries the node's HTTP door and uptime (an additive
  tail, 64 -> 70 bytes); `members` reports `http` and `uptime_s` per
  member and the fleet grid links each node through its own door.
- `CLUSTER SLOTS` and `CLUSTER SHARDS` are served from a cache keyed by
  the membership snapshot and rebuilt only when it changes;
  `stats.resp` gains `slots_hits` and `slots_builds`.
- `lib/NOTICE` names libsodium under the ISC License with its text
  reproduced; the libperfd export now carries `LICENSE` and `NOTICE`
  beside the code, and `synctest` runs on every push, not only on tags.
  `CONTRIBUTING.md` and `lib/README.md` state the boundary rule.

### Changed
- A fleet is uniform about the WAL: a node that logs cannot join a fleet
  that does not, and the reverse, refused at the join with the posture
  named.  Folded into the config digest only when a node logs, so a fleet
  without a WAL keeps the digest it has and a rolling upgrade across this
  change does not split it.  `fsync` policy stays per node.
- perfcli: `-h` with nothing after it is help; an unknown option is
  refused by name; the connect error names the target; with no target
  given the local daemon's config supplies the address and client
  secret; the banner waits for a successful first request, and a drop
  there is diagnosed (no secret, wrong secret, or a plaintext listener);
  an option after the command is called out as the command's argument.

## 0.3.0-rc5 — 2026-09-05

A delivery-and-visibility release inside the 0.3.0 line, and the fix
for what turned the rc4 tag red.  Eager mode now delivers on the write
instead of on a timer; the decision to refill a restarted node moved
from a guess to a fact the node states itself; an operator can finally
watch requests and see what the native door is carrying; and the public
library's export tree no longer crosses a licence boundary.

### Added
- `query_log = off | all | sampled:N` (`[daemon]`): one line per request
  through `LM_INFO` for every dialect on the native door - verb,
  collection, key, hit or miss, latency, client.  Off by default and one
  predictable branch when off; `sampled:N` stamps every line `sample=1/N`
  so a sample cannot be read as the whole; `query_log_keys = no | hashed
  | full` keeps keys out of the journal or prints eight hex digits of a
  hash.  Turning it on below `log_level = info` warns at startup.
- `stats.native`: per-dialect connection and request counters for the
  native door (`json`, `binary`, `resp`), and a card per dialect on the
  built-in page beside the RESP door's own.

### Changed
- Eager mode replicates on the write: a write pushes one group to every
  live peer from the write path, fire-and-forget, with no TTL threshold -
  a 5 s key reaches its replicas as surely as a 5-day one.  The sweep
  every 10 beats is repair behind it, not delivery, and its "dying soon"
  skip is derived from its own cadence.  Counters `repl_pushed`,
  `repl_skipped_dying`; a replication card on the page.
- A client outside `http_allow` gets `HTTP/1.0 403 Forbidden` with a body
  that names the setting, instead of a silent close that curl reports as
  a protocol error and an operator reads as a dead listener.
- The ALIVE heartbeat grows one additive tail byte saying how the node
  started - cold, recovered from a WAL, or established.  A build before
  the byte reads its prefix and is judged as before.
- libperfd 0.2.2: `pc_noise.h` in the exported tree no longer includes a
  GPL header; `tools/sync-libperfd.sh` resolves every include against
  the export set and refuses a licence crossing.

### Fixed
- A restarted node could be left without the fleet's records: the
  backfill was armed on "holds zero records" in the first heartbeat a
  peer read, and a peer's steady sweep could land a couple of records
  before that heartbeat, making an empty node look like a recovered one
  (`test/eagertest.sh`: a 480-record refill stalled at 2).  The node now
  says how it started, and a peer arms the backfill from that - for a
  restart it witnessed, or once it is itself past its own start window.
- The rc4 tag's matrix run failed on i386 and arm32: `statfs.f_type` is
  a plain `int` there and the tmpfs/ramfs compare was signed against
  unsigned under `-Werror`.  The compare is now unsigned on both sides.

### Closed without a change
- "The dashboard starts silently": it does not - config.c logs every HTTP
  listener at NOTICE with its allow-list size and token state.  What was
  silent was the wire (the 403 above).

## 0.3.0-rc4 — 2026-09-05

A fix-and-observability release inside the 0.3.0 line.  Three defects
in how a node comes back into an eager cluster, all found on a real
fleet during rolling restarts, each with a test that fails against the
previous build; the eager mode named as the mode it is; and a stats
document and built-in page that answer the questions an operator
actually has.

### Fixed
- A node that restarted empty could be left without the fleet's records
  for good.  The backfill's designated sender could change hands after
  an ordinary sweep had advanced its mark, and the backfill then
  discarded every passive copy on `wtick <= since`, walked "clean" and
  logged itself complete.  A backfill now walks from zero and only a
  whole cycle as the sender may clear it (`test/backfilltest.sh`,
  phase 1).
- A node that arrived empty could be elected backfill sender for the
  next one and hand it nothing, its own fill landing a moment later as
  copies that are never re-sent.  A peer that arrived empty within
  three sweeps is not a sender candidate (`backfilltest.sh`, phase 2).
- A rejoining node did not keep its id, because identity and the
  mastership term persisted only in the WAL directory and a pure cache
  has none.  `[daemon] state_dir` names where they live; the daemon
  creates it if the parent exists, refuses an unusable one, and accepts
  tmpfs with a warning.  A WAL-only configuration is unchanged;
  introducing `state_dir` beside a WAL carries the existing identity
  and term across once (`test/statedirtest.sh`).
- libperfd (0.2.1): a failed round trip left the pipeline counters set,
  so every later call on the handle failed with `pipeline desync` until
  the process restarted.  The counters are dropped on every failure
  path (`test/wedgetest.c`).  `perfcli` redials once when its handle has
  failed.
- `stats` published `arena_total`/`arena_used`/`arena_free` as zeros on
  a node whose huge-page arena was not the backing in use, which read
  as an arena with nothing left.  They are `null` there, with
  `arena_capacity_valid` beside them.
- The eager-mode bench cells were quantised by `redis-benchmark`'s
  millisecond clock; requests now scale with pipeline depth.

### Changed
- `mode = store | eager | proxy | shard`.  The `eager` key is retired:
  eager was only ever legal beside store, so it is the mode, not a flag
  on one.  The old key is a hard error naming the replacement.  Internal
  state and the wire are unchanged, so a rolling upgrade interoperates.
- `stats.cluster.mode` and each collection's `mode` report `eager` when
  eager is on; `/members` gains `routing.eager` and `routing.mode_name`
  beside the unchanged `routing.mode`.

### Added
- `stats.process`: uptime, pid, threads, cumulative CPU and RSS.  CPU is
  cumulative, never a rate - only the caller knows its polling window.
- `stats.cluster.state_dir` and `state_on_tmpfs`.
- `/stats` on the HTTP door serves the stats document; the `stats` verb
  takes an optional `col`.
- The built-in page is a fleet view: members as a grid legible to 64
  nodes, tinted and labelled by state, a selector to any member,
  collections, cluster-plane cards filed by what they measure, memory
  with live/held/free/ceiling/RSS, and rates differenced from the
  previous sample rather than lifetime ratios.
- Release tarballs per distribution and a Debian package (`debian/`).

## 0.3.0-rc3 — 2026-09-03

rc2 with one test defect fixed. **No product change**: the daemon in
rc3 is byte-for-byte the daemon in rc2 apart from its version string.

rc2's tag run was red, and a red tag certifies nothing — which was the
whole reason rc2 was cut over rc1. `walobstest` asserted that at least
five fsyncs had happened after seventy round-trip writes, and that
number is a property of the *device*, not of the daemon: the pump
group-commits, so on millisecond-class storage each round trip becomes
its own batch while on a microsecond-class CI runner seventy writes
drain as two. The same snapshot passed on one GitHub runner and failed
on another nine seconds apart.

### Fixed
- `walobstest` counts fsyncs it forces with `sync` barriers rather than
  fsyncs that ordinary writes happen to produce, so the count no longer
  depends on how fast the storage is. `PCOBS_TMP` points the fixture at
  a chosen filesystem — with `/dev/shm` the microsecond-class runner
  reproduces locally, which is how the fix was verified.

## 0.3.0-rc2 — 2026-09-03

Cut because rc1's tag had gone 51 commits stale: its CI is red, it
predates the durability fixes below, and every binary since has been
stamped `0.3.0-rc1` while being something materially different. A tag
that certifies nothing is worse than no tag.

Durability is the theme. Two defects here destroyed acknowledged writes
without saying so, and a third would take the host's filesystem down
with a refused start.

### Added
- **A node leaves service when it destroys an acknowledged write.** The
  first dropped ring record marks the node `FAILED`: it stays a member
  and keeps answering reads, but refuses writes and is not selected for
  new work, and in shard mode its keys move to the survivors. A segment
  overrun now takes the same path — the two are one event in different
  costumes. `FAILED` is terminal until an operator restarts the node,
  because recovery would re-shard the fleet a second time.
- **`wal/CONTROL`**, a durable record of the WAL's sequence and each
  segment's span — the missing half of the recycling rule.
- **The daemon measures its own fsyncs** and says once, with both
  numbers, when they cost 4x what the startup probe predicted. `stats`
  gains `wal.observed`.
- **A pre-flight space check**: a WAL that will not fit is refused with
  both sizes and the knobs to change, instead of discovering it by
  filling the filesystem.
- Nightly valgrind over the unit binaries (`tools/nightly-valgrind.sh`,
  schedule-only in the GitLab pipeline). First run 12/12 clean.

### Fixed
- **The WAL sequence survives a restart.** It restarted from zero, so
  the post-recovery checkpoint stamped `wal marker 0` — measured in 11
  of 12 snapshots — and every restart replayed the entire WAL because
  nothing is ever below a marker of zero.
- **The recycling rule is enforced after a restart.** Its per-segment
  spans lived only in RAM, so `seg_hot()` called every segment cold and
  the rule documented in DESIGN section 7 went unenforced: `overruns`
  read 0 while records were overwritten. The restart path also claimed
  its next segment without asking.
- **A failed WAL provision leaves nothing behind.** The partial segment
  used to survive, so a refused start left the volume full after the
  daemon exited and a supervisor's retry re-filled whatever had been
  freed.
- The mastership step-down's equal-term "collision" was a simulator
  artefact, not a defect; the retraction is recorded rather than the
  claim.

### Changed
- **The storage probe reports a burst ceiling, not a sustained rate.**
  It writes at most 16 MB in ~1.2s, so on any host with a write-back
  disk cache — every VM running `cache=writeback` — it measures the
  cache. Measured on Ceph: the probe said SSD-class, 139us p50 and
  ~3467 sustainable synced writes/s where `fio` in the WAL's own shape
  saw 3.7ms and 236/s, and it ranked btrfs above ext4 when ext4 is 1.6x
  faster under durable load. The number now states the volume behind it
  and that it is an upper bound.
- **The per-device probe cache is gone.** It was keyed by a device id
  that a hypervisor-side storage migration does not change, so it
  served numbers for storage that no longer existed. `auto` and
  `always` both measure; `probe = no` still costs nothing.
- `PRODUCTION.md` gains two measured items: verify with fio (or set
  `cache = none`) before enabling `fsync = always`, and put the WAL on
  ext4 rather than btrfs.

### Known
- `check` is intermittent on GitHub-hosted runners (S62), unresolved
  and now parked. Five storages were eliminated before the runner
  measured its own WAL directory at **p50 2us** — memory-backed, and
  roughly 1800x faster than our Ceph — which means every hypothesis
  tested had assumed the wrong premise. The same commits pass on the
  GitLab runner and on the build host.

## 0.3.0-rc1 — 2026-09-02

A release candidate, cut to put the whole tag ladder — including the
four-architecture matrix that S60 added after v0.2.1 — over a real tag
for the first time. Not 0.3.0: S31 (the rtpengine connector) and S36
(discovery without multicast) are deferred, and S52/S54 are blocked.

### Added
- **The S42 fault ladder**: partition, split brain, membership churn, a
  hand-over whose receiver never receives, the tombstone boundary, and a
  seed-replayable mastership simulator (`make check-fault`, plus
  `clustersim` in the ordinary gate).
- **`stats` reports each collection's `mode`** (store / proxy / shard).

### Fixed
- **The mastership step-down (clterm.h rule 3) is wired.** A master that
  observes a higher term now steps down; the term rides `MASTER_ALIVE`
  (35 → 39 bytes, length-gated) and outranks the member count, and a
  lower term never outranks us. Mixed-version fleets fall back to the
  address ranking until upgraded.
- `failovertest`'s round-robin spread leg had never run under ASan — it
  linked with a plain `cc` against a sanitized archive and the failure
  was swallowed as a skip.
- Size constants in `proto.c` are `size_t`, not `int` widened after the
  multiplication.

### Changed
- **The container fleet runs glibc.** The musl Containerfile is deleted:
  a build target kept beside a runtime is one somebody reaches for, and
  `containers-up.sh` had been reaching for it since the day it was
  written.

### Added
- **Metrics endpoint** (`GET /metrics`, OpenMetrics text) and a
  liveness probe (`GET /health`) on an optional `http = addr:port`
  listener. The metric names are a stable contract, pinned by
  `test/httptest.sh`. Guarded by `http_allow` (an off-box listener
  without one is refused at startup) and `http_timeout` (default 5s)
  so a half-finished request cannot hold a slot.
- **Memory pressure is visible before it bites**: `stats.memory` gained
  `tier`, `headroom_pct`, `nomem` and a `reclaim` block, and
  `reclaim_floor_mb` holds a floor under give-back.
- **`perfd_fetch()`** in the OpenSIPS driver: an asynchronous cache
  fetch, so a SIP worker is not blocked for the ~0.7–1 ms a cross-node
  pull costs.
- **MGET through `cache_raw_query()`** in the OpenSIPS driver: N keys
  in one round trip.
- **Status page** (`GET /`) and `GET /members`, served by **every**
  node rather than the master: a page the master serves is missing
  exactly when the master is in trouble, and two nodes disagreeing
  about the membership is how a partition becomes visible. Optional
  `[secrets] http` bearer token guards every route on the door.
- `sysusers.d` declaration, so the account `perfcached.service` runs as
  is actually created.
- `PRODUCTION.md`: the pre-deployment checklist.
- `contrib/haproxy-perfcached.cfg`: a reference front-end for clients
  that cannot learn a topology, replacing a redis-sentinel tier
  outright.

### Changed
- The `sync` barrier's reply now carries `dropped`. A ring-full drop
  discards a record that already holds a sequence number, so the
  barrier could truthfully answer `{"synced":true,"seq":N}` for an `N`
  that included records the WAL never carried (measured: 20 000
  writes, 1 223 dropped, 18 777 replayed after a crash). Callers can
  now tell a full barrier from one covering survivors only.
- Raw secrets are wiped from memory once the PSKs are derived, rather
  than living for the process lifetime.

### Fixed
- **An oversized write through a non-holder is refused, never forked.**
  A `SET` above the peer-forward ceiling (58 000 bytes) arriving at a
  node that does not hold the key could be neither forwarded nor
  refused, so the receiving node stored a divergent local copy: two
  nodes then answered the same `GET` differently. Released as 0.2.1.
- Give-back never actually returned memory on an mlock-pinned node: one
  refused `MADV_DONTNEED` latched the whole reclaim tick off, including
  the page phase that does not use it.

## [0.2.1] — 2026-09-01

Patch release over 0.2.0: the oversized-write fork window above, plus
test-suite hardening. Wire dialect and config digest unchanged, so
0.2.0 and 0.2.1 nodes interoperate and a rolling upgrade does not
split a fleet.

## [0.2.0] — 2026-09-01

The compatibility-promise baseline: daemon plus three clients (libperfd
C, pure-PHP, and any Redis client through the RESP2 door), four cluster
modes (store / eager / proxy / shard), WAL + RDB + recovery,
Noise-encrypted transport, and the Redis observability surface.

From this tag: the binary dialect's v1 is served indefinitely, peer
frames evolve additive-tail only, and incompatible fleets are refused
at join rather than joined wrongly.
