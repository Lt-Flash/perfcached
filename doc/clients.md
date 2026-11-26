# Clients

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

## perfcached-dashboard

The status page, served by its own process (0.6.6).  The daemon's HTTP
door served the same page until 0.7.0, but every action a dashboard offers
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

**The enable secret never crosses the network in the clear** (0.7.4).  The
session prompt sends it to the tool, so the tool takes it only over its
own TLS or from this host - a browser here, or a proxy here that ends
TLS; plain HTTP from another host is refused (403), and the address is
the connection's own, never X-Forwarded-For.  A proxy says how its browser
came in X-Forwarded-Proto (the shipped nginx and HAProxy configs set it), and
anything but https is refused too - so the admin image without a mounted
certificate (plain 8080) serves the page but opens no session.

**The admin image.**  `contrib/docker/admin/` builds an image with the
dashboard, perfcli and nginx in front (`tools/image-admin.sh`, podman or
docker): `podman run -p 8080:8080
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

## libperfd

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../contrib/brand/perfd-mark-dark.svg">
  <img src="../contrib/brand/perfd-mark-light.svg" width="72" align="right" alt="libperfd - the compass">
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

**Cluster-aware.** By default (`opts.spares` 0) the library learns the
fleet on connect, keeps standby connections open to the other nodes,
and swaps onto one when the node it is using dies - a failover costs a
`send()` on an established socket, not a TCP+Noise handshake;
`PERFD_SPARES_NONE` makes a bare handle.
`opts.policy` picks where a client works: `FAILOVER` (default),
`ROUND_ROBIN` (independent random start per client - a thousand
clients spread with no coordination), `LEAST_CONN` or `WEIGHTED` (by
the free arena each node reports); these three also move a live handle -
when a member comes up, one handle in N goes to it at its next call
(libperfd 0.7.5).  Idempotent verbs are replayed
across a failover; `add`/`sub` are NOT - the caller is told, because a
double increment is worse than a visible error.  A member that is back
up but not READY yet (restarted, replaying and pulling its bootstrap, or
still `connecting` after its admission, 0.7.5)
refuses every request with "node is not READY (recovering or connecting) - retry, or
use another member"; nothing was applied, so the library (0.7.0) takes
the request to another member - any verb, a counter update too - and
marks the refusing one down until the next members refresh sees it
ready.  A handle with `opts.pin` (0.7.6; perfcli sets it) never moves:
every call goes to the node it dialled, and the refusal is the caller's.
`perfd_member_count`,
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

## Redis pub/sub

Every door publishes and subscribes on one global channel space:
`SUBSCRIBE`, `PSUBSCRIBE`, `PUBLISH` and `PUBSUB` on the RESP door work
as on Redis, with Redis's subscribed-mode rules and message frames, and
a subscriber that cannot keep up is closed at the output cap the way
Redis closes it.  The
native door has five methods - `subscribe`, `psubscribe`,
`unsubscribe`, `punsubscribe` and `publish` - with messages delivered as
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
[REDIS-COMMANDS.md](../REDIS-COMMANDS.md); the figures are the `pubsub`
block on `/stats` and `/metrics`.
