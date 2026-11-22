# Operating it

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

## Watching it

`http = <addr:port>` under `[listen]` serves `/stats` (what the `stats`
verb answers, as JSON), `/metrics` for Prometheus, `/members`,
`/clients` (this node's connections: the table behind the page's clients
strip) and `/health`.  The status page - one page per node, the fleet's
view - is perfcached-dashboard's since 0.7.0 ([clients.md](clients.md); `GET /` on the
daemon answers 410).  The page's headline strip reads `clients 123 / 1,000
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
`http_allow` bounds who may reach it and `[secrets] http` adds a token (over
TLS with `http = <addr:port> tls`, under "TLS on the RESP and HTTP doors" in [config.md](config.md)) -
sent as `Authorization: Bearer <token>`, or by a browser as its own login
(any user name, the token as the password: the 401 asks for it).
`http_rate` (20 requests a second per address, loopback exempt) answers
an address that asks faster with 429, and `http_cache_ms` (1000) builds
`/stats` and `/metrics` at most once in that long for any number of
readers - a page polls every 3 s, and loopback always reads them fresh.
`perfcli stats` gives the same figures over the native door.


## Collections at runtime

A collection can be created and dropped while the daemon serves, so a
new keyspace does not need a config edit on every node and a fleet
restart:

    perfcli -E <enable> create sessions        # 2^12 buckets by default
    perfcli -E <enable> create sessions 16     # or say the size
    perfcli -E <enable> drop sessions          # refused while it holds records
    perfcli -E <enable> drop sessions force    # dropped with them

A collection can also be resized in either direction and renamed, both
while it serves:

    perfcli -E <enable> resize sessions 16     # either direction
    perfcli -E <enable> rename sessions_restored sessions

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
`stats.resp.enable_fails`, and after `enable_lockout_after` of them from
one address within `enable_lockout_s` seconds (5 in 60 by default) that
address's `enable` is refused for the window - "too many wrong enable
secrets from this address" - whatever sends them; `resp.enable_lockouts`
and `resp.enable_refused` count the lockouts and the refusals.

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

## Reloading the configuration (0.7.1)

`systemctl reload perfcached` reads `/etc/perfcached/perfcached.conf`
again and applies what a running daemon can take, without a restart:

- `[listen] resp_allow` and `http_allow` - the next connection is checked
  against the new list;
- `[secrets] http`, `resp` and `enable` - the next request or AUTH;
- `[secrets] client` - every handshake after the reload needs one of the
  new secrets; a connection already open keeps its session;
- `[daemon] log_level` and `slowlog_usec`;
- `[listen] auth_needs_tls` - the next AUTH or request (0.7.5);
- `[listen] tls_cert`, `tls_key` and `tls_min` - read again on every
  reload while a TLS door is open, changed or not, since a renewal
  changes the files and not the config; the next connection gets the new
  pair (0.7.5);
- the files a `rediss://` upstream names (`upstream_tls_ca`, `_cert`,
  `_key`) - read again on every reload; naming other files takes a
  restart (0.7.5).

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

## Open connections, running totals, and resetting them

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

## Which builds a node trusts

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

## Upgrades

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
previous minor talks to both sides of a roll - except against 0.7.5,
where a libperfd older than 0.7.6 does not route by key: every request
takes the forward hop, and a value over 58,000 bytes is refused.

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

**Rolling a fleet (0.7.0).**  `perfcli upgrade` is the roll
driver: a program with a journal and no state of its own, reading the
fleet before every step.

    perfcli -h <seed host> -p <port> -E <enable> upgrade plan perfcached-0.7.0.tar.gz version=0.7.0
    perfcli -h <seed host> -p <port> -E <enable> upgrade start
    perfcli -h <seed host> -p <port> -E <enable> upgrade status | abort | resume | rollback

`plan` records the artifact, its sha256, the version and the seed in
`~/.perfcached/upgrade-<host>_<port>.journal` (the seed's address;
`journal=` names another).
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
roll between nodes (it connects to nothing, so it works while the seed
itself is restarting); `resume` re-reads the fleet and the journal and rolls
only what is left; `rollback` reverts the rolled nodes in reverse order,
the master first; `status` shows the plan, the journal and every node's
live version, state and roll state.  A node that is unreachable is never
skipped: the roll stops until it is back.  `switch_cmd=<program>` replaces
the daemon's switch and revert verbs with a program of yours (`<program>
switch|revert <addr> <port> <node> <artifact> <sha256>`) - for a fleet on one
host's loopback, or a package host's package manager.  A roll leaves the
fleet on its committed wire epoch: once every node runs a build that speaks a newer one beside it,
`upgrade commit epoch=<n>` raises it ([modes.md](modes.md), "The wire epoch").  A release
that speaks no epoch of the fleet's, as 0.7.5 does not of 0.7.4's, is no
roll: `upgrade plan` refuses it, and every node is stopped and started on
the new build.

**Going back (0.7.0).**  The helper keeps the prefix it switched
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
forward again.  This is what the roll driver uses when a gate fails on
a node.

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
