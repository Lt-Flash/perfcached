<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="../contrib/brand/perfd-mark-dark.svg">
    <img src="../contrib/brand/perfd-mark-light.svg" width="96" alt="libperfd - the compass">
  </picture>
  <br>libperfd
</h1>

The C client for perfcached: **libperfd 0.7.6.1**, for perfcached 0.7.5.1.
Blocking calls with one thread per handle, pipelining, an opt-in
event-loop surface, and a handle that follows the fleet - standby
connections, failover, routing each key to its node.  It speaks the
native binary protocol behind the Noise handshake, with the slot
placement and key mixing the daemon uses, so a client can place keys
itself.  JSON appears only at the API edge (`perfd_command` and friends), never on the wire.  The API
is [perfd.h](perfd.h); this page is the tour.

## Build and link

Packages: `libperfd0` (the shared library, SONAME `libperfd.so.0`) and
`libperfd-dev` (`perfd.h`, `libperfd.a`, the development link and
`libperfd.pc`).  From a perfcached tree: `make libperfd.a libperfd.so`,
and `make install-lib` (also run by `make install`) installs them under
`PREFIX` (`/usr/local` by default; run `ldconfig` after).

```
cc -o app app.c $(pkg-config --cflags --libs libperfd)                # shared
cc -o app app.c $(pkg-config --cflags libperfd) $(pkg-config --variable=libdir libperfd)/libperfd.a -lsodium -lpthread   # static
```

`PERFD_VERSION` is the version you compiled against, `perfd_version()`
the one you linked - compare them to catch a stale archive.  A newer
library with the same SONAME is a drop-in: install it and restart the
program ([ABI](#abi)).

## Connect, set, get

```c
#include <perfd.h>
#include <stdio.h>

int main(void)
{
	const char *secrets[] = { "client-secret-example", NULL };
	perfd_opts o = PERFD_OPTS_INIT;
	o.secrets = secrets;

	perfd_t *p = perfd_connect("192.0.2.11", 6479, &o);
	if (!p) {
		fprintf(stderr, "connect: %s\n", perfd_error(NULL));
		return 1;
	}
	if (perfd_set(p, "sessions", "user:17", "hello", 5, 300) != 0)
		fprintf(stderr, "set: %s\n", perfd_error(p));

	void *val; size_t vlen; long long ttl;
	int r = perfd_get(p, "sessions", "user:17", &val, &vlen, &ttl);
	if (r == 1) {                       /* 1 found, 0 miss, -1 error */
		printf("%.*s ttl %lld\n", (int)vlen, (char *)val, ttl);
		perfd_free_value(val);
	}
	perfd_free(p);
	return 0;
}
```

`secrets` is a NULL-terminated list tried in order, so a secret rotates
by adding the new one first and dropping the old one later; NULL means
plaintext, for a `plaintext = loopback` door.  Deriving a secret's key
costs about 150 ms of CPU and a 64 MiB mapping (Argon2id, on purpose);
the library does it once per secret per process, and a server that
forks can call `perfd_psk_prepare(secrets)` in the parent so no worker
derives.  `perfd_connect_unix(path, &o)` dials a Unix socket.

## The options

`perfd_opts`, initialised with `PERFD_OPTS_INIT` (or
`perfd_opts_init(&o)`; C++ and FFI callers `perfd_opts_init_sz()`); a
field left 0 takes its default.

| option | default | what it does |
|---|---|---|
| `secrets` | NULL (plaintext) | client secrets, tried in order |
| `connect_timeout_ms` | 5000 | the whole dial, TCP and handshake |
| `io_timeout_ms` | 5000 | each send and receive |
| `policy` | `PERFD_POLICY_FAILOVER` | where the handle works: `FAILOVER` stays where it connected; `ROUND_ROBIN` (random start per client), `LEAST_CONN` (most free arena) and `WEIGHTED` (random by free bytes) spread, and move one handle in N to a member that comes up, at its next call |
| `spares` | 0 = one standby per other member (at most 8) | standby connections; `PERFD_SPARES_NONE` = a bare handle that learns no fleet |
| `refresh_ms` | 30000 | how often the member list is learned again |
| `route_keys` | 0 | 1 = send each request to the node that holds its key (`shard`, `spread`, `store`) |
| `route_hints` | 0 | 1 = in `proxy` mode, remember which node answered each key |
| `encrypt` | 0 | 0 = ask the node for plaintext after the handshake (the node may refuse, `[listen] encrypt = yes`); 1 = stay encrypted - `perfd_enable` needs it |
| `pin` | 0 | 1 = every call goes to the node dialled: the fleet is still learned (`perfd_member_*`), but no policy, routing, refusal, dead link or expulsion moves the handle, and `perfd_redial` comes back to that node only (perfcli pins) |
| `name` | NULL | the name the handle's connections carry on the daemon (`clients`, log lines); printable ASCII, no spaces or quotes, at most 47 characters |
| `keepalive_s` | 30 (-1 off) | TCP keepalive idle |
| `idle_ping_ms` | 30000 (-1 off) | a ping on an idle standby; one that fails is retired |
| `worker_balance` | 0 = on (-1 off) | the kernel gives a connection to one daemon worker by hashing its addresses, for its life; after connecting, the handle asks the daemon (`conninfo`) and dials again (a new source port, up to eight times) while its worker holds more than one connection beyond the least-loaded one - standbys too; Unix sockets and `perfd_connect_async` stay as they land |
| `eager_push` | 0 | event loop: 1 = write inside every `perfd_submit()` |

## The calls

Every typed call returns -1 on an error (`perfd_error(p)` says why), else
the value below.  Values are binary-safe; a buffer the library hands out
is released with `perfd_free_value()`, never libc `free()`.

| call | returns |
|---|---|
| `perfd_set(p, col, key, val, vlen, ttl)` | 0 |
| `perfd_set_cond(..., PERFD_NX \| PERFD_XX)` | 1 stored, 0 declined (the lock idiom) |
| `perfd_set_if` / `perfd_del_if` (`IFEQ`, `IFNE`) | 1 applied, 0 did not hold |
| `perfd_get(p, col, key, &val, &vlen, &ttl)` | 1 found (`ttl` -1 = no expiry), 0 miss |
| `perfd_get_buf`, `perfd_jget_buf`, `perfd_hget_buf` | as `get`, into your buffer: -2 too small (`*vlen` = size needed) - a read that allocates nothing |
| `perfd_del` | 1 deleted, 0 absent |
| `perfd_exists` | 1 / 0 |
| `perfd_ttl` | seconds left, -1 no expiry, -2 no such key (an error is also -1: check `perfd_error_kind`) |
| `perfd_expire` | 0 the key existed, 1 no such key |
| `perfd_add` / `perfd_sub` | 0, the new value in `*newval` |
| `perfd_hset`, `hsetnx`, `hget`, `hdel`, `hexists`, `hlen`, `hincrby`, `hgetall` | as Redis's hash commands; `hgetall` calls you back per field |
| `perfd_jget`, `jset`, `jdel`, `jincr` | JSON path verbs; fragments travel as JSON text, a NULL path is `$` |
| `perfd_rl_hit(p, col, key, window_ms, limit, &count)` | 1 within the limit, 0 over (a sliding-window rate limit) |
| `perfd_keys(p, col, match, limit, &keys)` | the count (`perfd_free_keys` after) |
| `perfd_ping` | 0 |
| `perfd_publish`, `perfd_subscribe`, `perfd_psubscribe` and the unsubscribes | pub/sub; deliveries reach the notify hook |
| `perfd_command(p, method, params_json)` | any method, the reply as JSON text |

**Several keys.**  `perfd_mget` asks one node for many keys; the node
looks up the ones it does not hold on the nodes that do, all at once
under one deadline.  A key whose node did not answer in time fails the
whole call with `PERFD_E_REFUSED` - retry - never reads as a miss.
`perfd_mget_ex` gives each key its own status instead
(`PERFD_MGET_FOUND`, `PERFD_MGET_ABSENT`, `PERFD_MGET_UNANSWERED`), so
only the unanswered ones need asking again.

**Pipelining.**  `perfd_append()` queues requests, `perfd_flush()`
writes them, `perfd_next_reply()` returns each reply in request order
(NULL with `perfd_error` for an error); typed calls in between are
refused.  `perfd_append_tree` and friends are the byte-exact tree forms
(`src/ptree.h`, in the static archive only).

## The event loop

`perfd_connect_async()` returns a handle in `PERFD_ST_CONNECTING`.
Watch `perfd_fd(p)` for `perfd_events(p)` (re-read after every call),
call `perfd_read_ready()` / `perfd_write_ready()`, and once
`perfd_state(p)` is `PERFD_ST_READY` submit:

```c
perfd_submit_kv(p, PERFD_V_SET, "sessions", "user:17", "hello", 5, 0, 300, done, ctx);
perfd_submit(p, "stats", NULL, done, ctx);   /* any method; reply as JSON text */
```

The callback owns `result` (`perfd_free_value`); NULL means failed and
`errmsg` says why.  Replies arrive in arrival order; nothing blocks and
nothing calls back from a thread of its own.  `perfd_push()` writes what
`perfd_submit()` queued when `eager_push` is 0.  An async handle opens no
standbys: to route by key, keep a handle per node and pick it with
`perfd_owner_of()`.  An async handle that routes by key learns the fleet again every
`refresh_ms`, and at once after a membership push counts another fleet than its own; a member
that joins or turns ready opens no standby behind its caller, who polls
one fd.

The blocking connect tries each of `opts.secrets` in turn; the event-loop
connect (`perfd_connect_async`) tries the first only, since a second dial
would change `perfd_fd()` under the caller's loop.  A node that refuses
that secret now fails the handle as `PERFD_E_AUTH`, not
`PERFD_E_TRANSPORT` "connection closed", and `perfd_secret_index(p)` says
which secret it tried: free the handle and dial again with
`opts.secrets + perfd_secret_index(p) + 1` while one is left.  On a
handle that connected it names the secret in use (`-1` in plaintext).

```c
/* ... the loop reports PERFD_ST_FAILED ... */
if (perfd_error_kind(p) == PERFD_E_AUTH && opts.secrets[perfd_secret_index(p) + 1]) {
	opts.secrets += perfd_secret_index(p) + 1;
	perfd_free(p);
	p = perfd_connect_async(host, port, &opts);
}
```

## URLs, conditional JSON sets, describing a handle

A client configured by one string can hand it to `perfd_url_parse`:

```
perfd[:group]://[user[:secret]@]host:port[,host:port...]/collection[?opt,opt...]
```

Hosts are names, IPv4 addresses or `[IPv6]` literals; every seed names its
port (1-65535); up to 8 seeds.  The secret is the password field, or the
user field when there is no password; `%XX` escapes a `:`, `@` or `/` in
either.  The options, split by `,` or `&`, are `route=0|1` (default 1),
`encrypt=0|1` (default 1), `spares=-1..8` (0 none, -1 one per member, the
default), `policy=failover|rr|least|weighted` (default failover) and
`dial_wait_ms=-1|1..60000`: how long an async pool's get waits on a new
connection before it fails while the dial goes on in the background (default
500, never past connect_timeout; -1 waits all of it).  The
parse is strict: an unknown option, a repeated one or a bad value is an
error, and the reason comes back in the caller's buffer.
`perfd_url_collection`, `perfd_url_secret`, `perfd_url_group` and
`perfd_url_seed` read the parts back; `perfd_url_apply` writes the four
options (the URL's or their defaults), `dial_wait_ms` when the URL gives it, and the secret into a `perfd_opts`,
touching only the fields the caller's struct holds - timeouts, name and the
rest stay the caller's.

`perfd_url_connect(u, opts)` makes a blocking handle from the seeds, the
first tried chosen by the process id so a pre-forked server spreads its
children.  A seed that fails is logged once (through `perfd_set_log`, at
warning level) and its return once, so an outage costs two lines, not one
per call.  After a dial that no seed answered, the next calls fail at once
with `PERFD_E_BACKOFF` and "backing off" in `perfd_error(NULL)`: for 1 s,
doubling to 30 s, until a dial succeeds.  The state lives in the
`perfd_url`, per process - a forked child starts without its parent's
hold - and one thread at a time may use it.

```c
char err[256];
perfd_url *u = perfd_url_parse("perfd://app:s3cret@10.0.0.1:6380,10.0.0.2:6380/sessions",
	err, sizeof err);
perfd_t *p = u ? perfd_url_connect(u, NULL) : NULL;
if (!p && perfd_error_kind(NULL) == PERFD_E_BACKOFF)
	/* a recent dial found every seed down: answer from elsewhere */;
```

`perfd_jset_ex(p, col, key, path, json, ttl, flags)` is `perfd_jset` with
`PERFD_JSET_NX` (only where the path holds nothing) or `PERFD_JSET_XX`
(only where it holds a value), as `JSON.SET ... NX|XX`: 1 set, 0 declined
(and no error left on the handle), -1 an error.  An `NX` below a key that
does not exist is the daemon's error, not a decline.  A conditional set is
never sent again after a failover: replayed after its reply was lost, it
would meet its own write and answer declined.

`perfd_describe(p, buf, cap)` writes one line for a log -
`perfcached 0.7.5.1, 3 members, routing on, 2 standbys` - and returns the
length it needed, as `snprintf` does.  `perfd_fleet_changes(p)` is a
counter that moves whenever the handle's member list, or a member's state
as the handle sees it (ready, draining, down), changes: compare two
readings to know when to log or re-read `perfd_member_info`.

`perfd_exec(p, col, argc, argv, argvlen)` runs a Redis-style command
through this library's verbs, each on its key's node, and answers a typed
`perfd_reply` (nil, integer, string, status, error or array; free it with
`perfd_reply_free`) shaped as perfcached's RESP door answers the same
command.  It covers the keyed data commands: `GET`, `SET` (with `EX`, `PX`,
`NX`, `XX`, `IFEQ`, `IFNE`), `SETEX`, `PSETEX`, `DEL`, `UNLINK`, `EXISTS`,
`EXPIRE`, `PEXPIRE`, `TTL`, `PTTL`, `INCR`, `INCRBY`, `DECR`, `DECRBY`,
`MGET`, `MSET`, every hash command (`HSET` ... `HSCAN`), and `JSON.GET`,
`JSON.SET` (with `NX`, `XX`, `EX`) and `JSON.DEL`.  Any other command is an
error reply, "unsupported command" - never a guess.  Arguments are
binary-safe (`argvlen` NULL reads them as C strings) except that a key
cannot hold a NUL byte.  A daemon's error is an error reply in the door's
words; NULL means the call itself failed (`perfd_error`).  Two differences
from the door: `MSET` is a set per key, each on its own node, not one
atomic write (the door has no `MSET`), and a multi-key `DEL` counts the
keys actually deleted.

```c
const char *argv[] = { "SET", "session:42", "{...}", "EX", "600", "NX" };
perfd_reply *r = perfd_exec(p, "sessions", 6, argv, NULL);
if (!r)
	/* transport: perfd_error(p) */;
else if (r->type == PERFD_REPLY_NIL)
	/* NX did not hold */;
perfd_reply_free(r);
```

## An async pool, one descriptor per request

Some event loops suspend a task on one file descriptor and resume it when
that descriptor turns readable, so every request in flight needs a socket
of its own.  `perfd_apool` keeps those sockets for a process
(`PERFD_HAS_APOOL`):

```c
const char *hosts[] = { "10.0.0.1", "10.0.0.2" };
int ports[] = { 6380, 6380 };
perfd_opts o = PERFD_OPTS_INIT;          /* secrets, timeouts, route_keys = 1 */
perfd_apool *pool = perfd_apool_new_seeds(hosts, ports, 2, "sessions", &o, 16);

int fd = perfd_apool_get(pool, key, on_reply, task);
if (fd >= 0)
	suspend_on(fd);                  /* on readability: perfd_apool_ready(pool, fd) */
else if (fd == -1)
	fall_back(perfd_error(NULL));    /* on_reply was not called */
/* PERFD_APOOL_DONE: on_reply already ran */
```

- Every GET has an event-loop handle of its own.  With `opts.route_keys`
  the handle is pinned to the key's owner, which a separate handle on a
  seed learns (again every `refresh_ms` and on membership pushes);
  without routing, or before the fleet is known, it goes to a seed.  The
  seeds are tried from a start rotated by the process id.
- The callback gets the GET reply as `perfd_submit_kv` gives it -
  `[u8 found][u32 ttl][value]`, one zero byte for a miss - or NULL and
  the error.  It runs once at most, from `perfd_apool_ready()` (1 = it
  ran, 0 = not yet), `perfd_apool_wait()` or, rarely, inside
  `perfd_apool_get()` itself.
- `perfd_apool_wait(pool, fd, timeout_ms)` blocks for a caller that
  cannot suspend; when the time passes the request is cancelled.
  `perfd_apool_cancel(pool, fd)` gives up on a request: its callback
  never runs, and its handle is never used again - a reply that arrives
  later is not read, and the handle is replaced at a later get.  Stop
  watching the descriptor before that get, which may reuse its number.
- A new handle must be ready within `connect_timeout_ms`, TCP and
  handshake.  When no node answers, the pool's dials back off for 1 s,
  doubling to 30 s, and a get fails at once meanwhile.  A handle that
  failed is replaced; an idle one whose node closed it is found closed
  and replaced before it is used.  When all `max_conns` handles exist
  and the get needs a node none of the idle ones is on, the least
  recently used idle handle is closed and dialled to that node; when
  every handle is busy the get fails (`PERFD_E_STATE`).
- After `fork()` the pool belongs to the process that uses it: the
  first call in the child closes the child's copies of the parent's
  sockets, without a byte sent, and dials its own; the parent's
  requests are not the child's and their callbacks never run there.
- `perfd_apool_new(u, opts, max_conns)` builds the same pool from a parsed
  `perfd_url`: its seeds, collection, options and secret over `opts`.
- `perfd_apool_node(pool, fd, ...)` names the node a request went to,
  `perfd_apool_stats()` counts handles, gets, dials, rebuilds,
  evictions, cancels and fork resets.  A pool is not thread-safe: keep
  one per thread or process.

## In a fleet

By default a handle learns the members on connect and keeps a standby
connection to the others, so losing its node costs a `send()` on an
established socket, not a new handshake.

- **Moving.**  A `ROUND_ROBIN`, `LEAST_CONN` or `WEIGHTED` handle moves
  while connected: one handle in N goes to a member that comes up, at
  its next call, and a handle leaves a member that stops serving.  A
  member starting, recovering, healing, failed or stalled stays in the
  handle's table and gets no traffic.  `FAILOVER` stays where it
  connected; a handle routing by key follows the owner; one holding
  subscriptions stays with them.  The daemon tells its clients when its
  state changes, so handles leave a FAILED or HEALING node on the next
  beat.  A standby's dial runs in the maintenance after a call, never
  holding the caller; a new handle waits briefly for the standbys that
  answer.
- **Failover.**  Idempotent calls are replayed on the new node;
  `perfd_add`/`perfd_sub`, conditional sets, hash writes and rate-limit
  hits are not - the caller gets the error, because a double increment
  is worse than a visible one.  A routed counter update whose standby
  dies mid-request fails with `PERFD_E_TRANSPORT` rather than risk a
  second apply.
- **Refusals that applied nothing** - a node not READY (recovering,
  connecting), FAILED or HEALING, or a `forward failed` - are
  `PERFD_E_REFUSED`, and the handle takes the call to another member at
  once, whatever the verb, marking the refusing one down until it is
  ready.  A handle holding subscriptions stays put.  A re-send that
  fails too reports both members' answers, the first's first.
- **Dead links.**  Before sending on a link quiet for 100 ms the handle
  checks it, so a link a restart closed is replaced before anything is
  sent (a pinned handle fails with `PERFD_E_TRANSPORT`, nothing sent).
  A replaced link - closed while idle, or its node expelled - raises
  privilege again before the call.
- **Routing.**  With `route_keys = 1` each request goes to its key's
  node and the daemon's forward hop disappears; a wrong guess is
  forwarded, never wrong (`perfd_route_missed()` counts the forwards; a
  reply that says the handle is behind makes it re-learn the members,
  `perfd_moved_hints()`).  `proxy` keys are not computable:
  `route_hints = 1` remembers who answered each key in a fixed 4,096-entry
  table (`perfd_hint_hits()`, `perfd_hint_stale()`).  libperfd 0.7.6.1
  routes against perfcached 0.7.5.x (wire epoch 4); `perfd_wire_epoch()`
  shows what the daemon advertised.
- **A frozen member** that accepts the connection and says nothing costs
  at most `connect_timeout_ms`, reported as `PERFD_E_TRANSPORT`.
- **Watch it**: `perfd_member_count`, `perfd_member_info`,
  `perfd_member_state` (active, standby, down with why, not-ready,
  idle), `perfd_active_node`, `perfd_spare_count`, `perfd_failovers`,
  `perfd_server_version`, `perfd_routing`.
- **An idle handle acts on nothing**: call `perfd_maintain(p)` from a
  timer, a few times a second, to drain pushes and re-dial down members.
  A handle that FAILED is poisoned; `perfd_redial(p)` tries the members
  it learned, the seed last.

**Notifications.**  `perfd_set_notify(p, cb, ctx)` hands the hook every
push as JSON text (not NUL-terminated, valid during the call):

- membership: `{"notify":"membership","event":"joined"|"expelled"|"state","addr","port","node","identity","members","state"}` -
  the handle has already acted (dialled the newcomer, dropped the
  expelled one) when the hook sees it;
- pub/sub: `{"method":"message","params":{"channel","payload"}}` and
  `{"method":"pmessage","params":{"pattern","channel","payload"}}`.  A
  payload that is not UTF-8 shows U+FFFD in the hook's text; binary
  payloads go over UDP pub/sub (`perfd_pubsub_udp()`, then
  `perfd_pubsub_udp_ready()` on readability and at least once a second;
  [doc/clients.md](../doc/clients.md#redis-pubsub)).

## Errors

Branch on `perfd_error_kind(p)`, never on the text:

| kind | meaning |
|---|---|
| `PERFD_E_TRANSPORT` | connect, send, receive, a lost connection (the handle is poisoned) |
| `PERFD_E_AUTH` | the handshake: wrong or missing secret |
| `PERFD_E_REFUSED` | the daemon applied nothing: limits, not READY, busy, a forward lost, an mget timeout, a read whose owner did not answer in time - retry |
| `PERFD_E_SERVER` | any other error the daemon answered (`value too large`, ...) |
| `PERFD_E_ARG` | a bad argument |
| `PERFD_E_NOMEM` | out of memory, or the crypto library failed |
| `PERFD_E_PROTOCOL` | a reply the library could not read |
| `PERFD_E_STATE` | the call does not fit the handle's state (`perfd_enable` on a plaintext link, a typed call inside a pipeline) |
| `PERFD_E_BACKOFF` | a recent dial found no seed answering: the call failed at once, nothing sent (1 s, doubling to 30 s) |

`perfd_set_log(cb, ctx)`, set once before the first connect, takes the
library's log lines (errors go to stderr otherwise).

## Privilege and the admin verbs

`perfd_enable(p, "<enable secret>")` raises privilege on a handle with
`encrypt = 1` (on a plaintext link it fails with `PERFD_E_STATE` and the
secret is not sent); the handle raises it again by itself after a
re-dial, and an `enable` or `disable` sent through `perfd_command` is
kept the same way.

The operations verbs answer the daemon's reply as JSON text
(`perfd_free_value`) or NULL with its words in `perfd_error()`:
`perfd_stats(p, col)`, `perfd_members`, `perfd_clients(p, cap)`,
`perfd_collections`, `perfd_create`, `perfd_drop`, `perfd_resize`,
`perfd_rename`, `perfd_map` / `perfd_unmap` / `perfd_maps` (the RESP
door's database map on the running fleet), `perfd_reset_stats` and
`perfd_reset_stats_fleet` (this node and every live peer as of one
moment), `perfd_save`, `perfd_sync`, `perfd_load`, `perfd_mailtest`,
`perfd_upfill` / `perfd_updrain` (`PERFD_SWEEP_RUN`, `_CHECK`, `_STOP`),
`perfd_cluster_epoch_commit`, `perfd_upgrade_status`,
`perfd_upgrade_switch(p, sha256, force)`, `perfd_upgrade_revert(p,
force)` and `perfd_fleetstop(p, timeout_ms)` (it answers once the peers
have gone - give the handle an io timeout above it).  The DDL, `map`,
`unmap`, `fleetstop` and `mailtest` need `perfd_enable` first; create,
drop, rename, map and unmap also `[daemon] allow_create = yes` on the
node.  `perfd_upgrade_stage` ships a release artifact - begin, 192 KB
chunks, end - with an optional 64-byte ed25519 signature and a progress
callback; `perfd_upgrade_stage_file` does it from a file with
`<path>.sig` beside it (a package install refuses it).  perfcli and
perfcached-dashboard are built on these calls.

## ABI

The SONAME changes only when the ABI breaks.  A newer library with the
same SONAME runs an older program because:

- `perfd_opts` carries its size (`PERFD_OPTS_INIT`), and the library
  reads only the fields the program's build knows;
- every buffer the library hands out is released with
  `perfd_free_value()`;
- errors carry a kind (`perfd_error_kind()`);
- the library logs only through `perfd_set_log()` or to stderr;
- it exports `perfd_*` and nothing else.

`test/abitest.sh` holds the shared library to that: a caller built
against the frozen header of its SONAME (`test/abi/`) must still run
against what the tree builds, and no exported symbol may disappear.
`perfd.h` is the only header installed; `src/ptree.h`, which the tree
forms need, stays in the source tree, and `libperfd.so` exports no tree
functions.

## Licence

**MIT** - see `LICENSE` here.  The MIT set is what the library is built
from: `PERFD_SRCS` in the Makefile and every file the compiler reads for
them (`make print-libperfd` gives the sources and flags).
`test/mitboundtest.sh`, in `make check-fast`, asks the compiler for that
list and fails if any file in it lacks an `SPDX-License-Identifier: MIT`
line, or if the archive holds an object not built from `PERFD_SRCS`.

The sources the library shares with the daemon log through
`#include <compat/dprint.h>`; the include path picks the logger -
`lib/shim/compat/dprint.h` (MIT, through `lib/perfd-log.c`) for the
library, the daemon's GPL `src/compat/dprint.h` for the daemon.  The
library's build never opens the daemon's file.

Consumers link the installed library; none carries a copy of its
source.  The one third-party dependency is libsodium (ISC) - see
`NOTICE`.  Effective licence of a redistributed libperfd: MIT plus that
notice.

The daemon, the cluster plane and everything else under `src/` are
GPL-2.0-or-later (`COPYING` at the repository root).  A header that
both sides need goes on the MIT side from day one, or stays daemon-only
with the minimal piece duplicated on the MIT side - never a GPL include
from an MIT header, not even temporarily.
