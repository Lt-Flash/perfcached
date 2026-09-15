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
