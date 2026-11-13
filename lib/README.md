<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="../contrib/brand/perfd-mark-dark.svg">
    <img src="../contrib/brand/perfd-mark-light.svg" width="96" alt="libperfd - the compass">
  </picture>
  <br>libperfd
</h1>

The client library for perfcached, as a self-contained export: a C API
over the native binary protocol (the fixed-layout data verbs and
`PC_VERB_CMD`, whose parameters and reply are trees - `src/ptree.[ch]`
in the source tree), with the Noise handshake, the slot placement and
the key mixing the daemon uses, so a client can place keys itself.
JSON appears only at the API edge (`perfd_command` and friends take and
return JSON text; the library converts), never on the wire.

## Building and linking

`make libperfd.a libperfd.so` builds both from the same MIT sources;
`make install-lib` (and `make install`, which runs it) puts
`libperfd.so.<version>` (SONAME `libperfd.so.0`), `libperfd.a`,
`perfd.h` and `lib/pkgconfig/libperfd.pc` under the prefix
(`PREFIX`, `/usr/local` by default; run `ldconfig` after installing
there).  `perfd.h` is the only header installed: `src/ptree.h`, which
the tree forms (`perfd_command_tree` and friends) need, stays in the
source tree, and `libperfd.so` exports no tree functions.  The Debian
packages split it: `libperfd0` holds the shared library, `libperfd-dev`
the header, the archive, the development link and the `.pc`, under
`/usr`.  Link with pkg-config, shared by default:

```
cc -o app app.c $(pkg-config --cflags --libs libperfd)                # shared
cc -o app app.c $(pkg-config --cflags libperfd) $(pkg-config --variable=libdir libperfd)/libperfd.a -lsodium -lpthread   # static
```

The SONAME changes only when the ABI breaks (a symbol removed, a struct
reshaped); a new library version with the same SONAME is a drop-in
replacement - install it and restart the program.  That holds because:

- `perfd_opts` carries its size: initialise it with `PERFD_OPTS_INIT`
  (or `perfd_opts_init()` from C++ or FFI) and the library reads only
  the fields your build knows;
- every buffer the library hands out is released with
  `perfd_free_value()`, never libc `free()`;
- errors carry a kind, `perfd_error_kind()` - branch on that, not on the
  text;
- the library logs through `perfd_set_log()` if you give it a callback
  (errors to stderr otherwise);
- it exports `perfd_*` and nothing else.

`test/abitest.sh` holds the shared library to that: a caller built
against the frozen header of its SONAME (`test/abi/`) must still run
against what the tree builds, and no exported symbol may disappear.

### Reads that allocate nothing (libperfd 0.6.0)

`perfd_get`, `perfd_jget` and `perfd_hget` hand back a value the
library allocated.  Their `_buf` twins - `perfd_get_buf`,
`perfd_jget_buf`, `perfd_hget_buf` (`PERFD_HAS_GET_BUF`) - copy it into a
buffer you own instead, so a read on a warm handle allocates nothing:
1 found, 0 absent, -1 error, and -2 when the buffer is too small, with
`*vlen` set to the size needed and the buffer left as it was.  The copy is
binary-safe and not NUL-terminated.

### A multi-key read across a fleet (libperfd 0.7.2)

`perfd_mget` asks one node for many keys; the node looks up the ones it
does not hold on the nodes that do, all at once and under one deadline.
A key whose node did not answer in time fails the whole call
(`PERFD_E_REFUSED`: retry), never as a miss.  `perfd_mget_ex`
(`PERFD_HAS_MGET_EX`) says it per key instead: `PERFD_MGET_FOUND`,
`PERFD_MGET_ABSENT` or `PERFD_MGET_UNANSWERED` in a `status` array the
caller passes, so the answered keys are kept and only the unanswered
ones need asking again.

### Connections spread over the daemon's workers (libperfd 0.7.4)

The kernel gives each new connection to one of the daemon's workers by
hashing its addresses, and a connection stays on that worker for its life.
A client that opens a few long-lived connections - one per process of a
proxy, say - can find two on one worker and none on another.  After it
connects, a handle asks the daemon (`conninfo`) how many connections each
worker holds; while its own worker holds more than one beyond the
least-loaded one, it dials again (a new source port hashes elsewhere) and
keeps a connection that landed well, trying up to eight times a worker.
Standby connections are placed the same way.  It is on by default;
`opts.worker_balance = -1` turns it off.  A daemon older than 0.7.4, a
Unix-socket connection, and the event-loop connect (`perfd_connect_async`)
are left as they land.

### Plaintext after the handshake (libperfd 0.7.4)

After the Noise handshake a handle asks the node to carry its requests in
the clear - still authenticated by the handshake, but readable on the
network.  A node with `[listen] encrypt = yes`, or one older than 0.7.4,
keeps the connection encrypted.  `opts.encrypt = 1` never asks, and
`perfd_enable` needs it: on a connection in the clear it fails with
`PERFD_E_STATE` and the enable secret is not sent.

### Handles that follow the fleet (libperfd 0.7.5)

A handle with `PERFD_POLICY_ROUND_ROBIN`, `LEAST_CONN` or `WEIGHTED` used to
choose its member only at connect.  It now moves while connected: when a
member comes up, one handle in N goes to it at its next call, so the shares
even out with no rush, and a handle leaves a member that stops serving.  A
member starting, recovering, healing, failed or stalled stays in the handle's
table and gets no traffic.  `PERFD_POLICY_FAILOVER`, the default, stays where
it connected; a handle routing by key follows the owner instead, and one
holding subscriptions stays with them.

A member can take the connection and then say nothing, as a frozen daemon's
kernel does.  A standby's dial runs in the maintenance after a call, so it no
longer holds the caller; a new handle waits briefly for the standbys that
answer.  `connect_timeout_ms` bounds the whole dial, TCP and handshake, within
`io_timeout_ms`, and no answer is `PERFD_E_TRANSPORT`, not `PERFD_E_AUTH`.

### Refusals and closed links (libperfd 0.7.6)

A write refused by a node that is FAILED or HEALING applied nothing, as a
not-READY refusal does: the handle moves to another member at once,
whatever the verb, and both refusals are `PERFD_E_REFUSED` now, not
`PERFD_E_SERVER`.  So is the daemon's `forward failed`: the node answers
it before the forward is sent, so nothing was applied - retry.  A handle
that holds subscriptions is not moved by a refusal, which would end them
unsaid.

Before it sends on a link that has been quiet for 100 ms, a handle looks
at it: a link a restart closed is replaced before anything is sent, so
even a counter update moves (a pinned handle fails with
`PERFD_E_TRANSPORT`, nothing sent).  A routed counter update
(`perfd_add`, `perfd_sub`) whose standby dies mid-request is not sent
again on the active link - it may have been applied - and fails with
`PERFD_E_TRANSPORT`, as one on a lost active link does.

An event-loop handle that routes by key learns the fleet again every
`refresh_ms`, and at once after a membership push counts another fleet
than its own; a member that joins or turns ready opens no standby behind
its caller, who polls one fd.  The daemon (0.7.5) tells its own clients
when its state changes, so handles leave a FAILED or HEALING node on the
next beat, not at the member-list refresh.

A link replaced before a call - closed while idle, or its node expelled -
is named and raised again as every other move is, so a privileged call
after a quiet node's restart carries the privilege the handle raised.

libperfd 0.7.6 routes by key against perfcached 0.7.5, which speaks cluster
wire epoch 4 alone, and against nothing older.  An older libperfd does
not route against 0.7.5: every request takes the forward hop, and a
value larger than a forward carries (58,000 bytes) is refused.

### Notifications

`perfd_set_notify(p, cb, ctx)` hands the hook every push the daemon
sends, as JSON text:

- membership - `{"notify":"membership","event":"joined"|"expelled"|"state",
  "addr","port","node","identity","members","state"}` - when a node joins,
  is expelled or changes state (`recovering`, `ready`, `draining`, ...).
  The handle acts first - a blocking handle dials a newcomer, every handle
  drops an expelled member's connection, standby and routing weight - and
  the hook sees it after.
- pub/sub deliveries - `{"method":"message","params":{"channel","payload"}}`
  and `{"method":"pmessage","params":{"pattern","channel","payload"}}`.  The
  payload is bytes on the wire; in the hook's text a payload that is not
  UTF-8 shows U+FFFD in place of the bad bytes, so binary payloads go over
  UDP pub/sub or are kept UTF-8.

A handle with no traffic acts on nothing: call `perfd_maintain()` from a
timer, a few times a second, to drain pushes and run the recovery without
a request.

### The admin verbs (libperfd 0.6.6)

The upgrade's staging (0.7.0): `perfd_upgrade_stage` ships a release artifact's bytes to the node - begin, 192 KB chunks, end - with an optional 64-byte ed25519 signature and a progress callback, answering the end reply (`sha256`, `signer`, `relayed_to`, `older_members`); `perfd_upgrade_stage_file` does it from a file with `<path>.sig` beside it.  Privileged; a package install refuses it.  `perfcli upgrade-stage` is this call.

The operations verbs (0.7.0): `perfd_collections`, `perfd_create`, `perfd_drop`, `perfd_resize`, `perfd_rename`, `perfd_map` / `perfd_unmap` / `perfd_maps` (0.7.1: the RESP door's DB-index map on the running fleet), `perfd_reset_stats` (and `perfd_reset_stats_fleet`, 0.7.0: this node and every live peer as of one moment - the dashboard's button), `perfd_save`, `perfd_sync`, `perfd_load`, `perfd_mailtest`, `perfd_upgrade_status`, `perfd_upgrade_revert` (0.7.0: `force` as an int), `perfd_upgrade_switch` (0.7.0: the staged build's sha256, `force`), `perfd_fleetstop` - each answers the daemon's reply as JSON text (free it with `perfd_free_value`) or NULL with the daemon's words in `perfd_error()`; the DDL (map and unmap included), `fleetstop` and `mailtest` need `perfd_enable` first, and create, drop, rename, map and unmap also `[daemon] allow_create = yes` on the node asked.  `perfd_fleetstop` answers once the peers have gone - give the handle an io timeout above its `timeout_ms`.

`perfd_opts.name` (0.7.0): the name the handle's connections carry on the daemon - sent after every dial and re-dial, shown by `clients`, and printed in the node's enable, sweep and reset log lines as `name@ip:port`.  A courtesy, not a capability: an older daemon's refusal is not reported.

`perfd_opts.pin` (0.7.6): every call goes to the node the handle dialled.  The fleet is still learned (`perfd_member_*`), but no policy, key routing, not-READY refusal, dead link or expulsion moves the handle, and `perfd_redial` comes back to that node only - its answer is the caller's.  perfcli pins.  An unpinned handle that moves on a refusal or a dead link raises privilege again on the adopted link before the re-send, and a re-send that fails too reports both members' answers, the first's first.

What perfcli and perfcached-dashboard need, typed, so neither builds a
request by hand: `perfd_stats(p, col)`, `perfd_members(p)` and
`perfd_clients(p, cap)` answer the trees the status page reads, rendered
as JSON text (free with `perfd_free_value`); `perfd_enable(p, secret)`
raises privilege on a handle with `opts.encrypt = 1`, and the handle
raises it again by itself after a
re-dial (0.7.6: so does an `enable` sent with `perfd_command`, and a
`disable` sent there stops it); `perfd_cluster_epoch_commit(p, epoch)` asks the master to commit the
fleet's wire epoch (0.7.1); `perfd_upfill(p, col, what)` and `perfd_updrain(p, col, what)`
run, check (upfill) or stop a sweep.  A refusal comes back as NULL or -1
with the daemon's words in `perfd_error(p)`.

## Licence

**MIT** - see `LICENSE` here.  The MIT set is what the library is built
from: `PERFD_SRCS` in the Makefile and every file the compiler reads for
them (`make print-libperfd` gives the sources and flags).
`test/mitboundtest.sh`, in `make check-fast`, asks the compiler for that
list on every push and fails if any file in it lacks an
`SPDX-License-Identifier: MIT` line, or if the archive holds an object not
built from `PERFD_SRCS`.

The sources the library shares with the daemon log through
`#include <compat/dprint.h>`; the include path picks the logger -
`lib/shim/compat/dprint.h` (MIT, through `lib/perfd-log.c`) for the
library, the daemon's GPL `src/compat/dprint.h` for the daemon.  The
library's build never opens the daemon's file.  Before 0.5.0 the archive
carried the daemon's GPL `compat.o`: a crossing in the shipped `.a`, found
while building the shared library.

Consumers link the installed library; none carries a
copy of its source.  The one third-party dependency is libsodium (ISC) -
see `NOTICE`.  Effective licence of a redistributed libperfd: MIT plus
that notice.

## Where the rest lives

The daemon, the cluster plane and everything else under `src/` are
GPL-2.0-or-later (`COPYING` at the repository root).  A header that
both sides need goes on the MIT side from day one, or stays daemon-only
with the minimal piece duplicated on the MIT side - never a GPL include
from an MIT header, not even temporarily.
