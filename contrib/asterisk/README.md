# func_perfd - perfcached from the Asterisk dialplan

An optional Asterisk module (Asterisk 22, 23 and 24) with four dialplan
functions that read and write perfcached 0.7.5.1 - or a standalone Redis -
through a pool of connections.  It links libperfd 0.7.6.1 or later; it
carries no copy of it.

| Function | Use | Returns |
|---|---|---|
| `PERFD_GET(collection,key)` | read | the value, or empty |
| `PERFD_SET(collection,key[,ttl])` | write | - |
| `PERFD_EXISTS(collection,key)` | read | `1` or `0` |
| `PERFD_DELETE(collection,key)` | read (deletes) or write (value ignored) | `1` removed, `0` absent |

Every call sets `PERFDSTATUS`: `OK`, `NOTFOUND` (the key was not there) or
`ERROR` (the reason is logged).  The status is how a stored empty value is
told from a missing key.  An empty `collection` uses the one in
`perfd.conf`.  `PERFD_SET` and `PERFD_DELETE` are registered as dangerous
functions, so with `live_dangerously = no` they cannot be reached from
external interfaces.

## Build it

Install `libperfd-dev` (and `libperfd0` where Asterisk runs) from
perfcached's packages, or `make install-lib` in a perfcached checkout;
plus autoconf, automake and pkg-config.  libperfd older than 0.7.6.1 is
refused at build time: the module dials through `perfd_url_connect`.

A patch per release, for the supported branches: `asterisk-22.12.0-`,
`asterisk-23.6.0-` and `asterisk-24.0.0-func_perfd.patch`.

```
cd asterisk-24.0.0
patch -p1 < asterisk-24.0.0-func_perfd.patch
./bootstrap.sh
./configure <your usual options>
make menuselect        # Dialplan Functions -> func_perfd (off by default)
make && make install
```

- Scripted: `menuselect/menuselect --enable func_perfd menuselect.makeopts`
  replaces the interactive step.
- The patch does not carry `configure`; `./bootstrap.sh` regenerates it
  from the patched `configure.ac`.
- configure finds libperfd through pkg-config (`libperfd >= 0.7.6.1`).  For a
  library under a private prefix: `PKG_CONFIG_PATH=<prefix>/lib/pkgconfig
  ./configure ...`, or `--with-perfd=<prefix>`.  Without libperfd,
  menuselect shows the module as unavailable.
- The module and its RESP client compile clean under developer mode's
  warnings (`--enable-dev-mode`).
- A libperfd update with the same SONAME (`libperfd.so.0`) reaches Asterisk
  by installing the new `libperfd0` and restarting Asterisk - no rebuild.

**Another Asterisk release:** the patch is generated, never edited by hand.
From a perfcached checkout, `contrib/asterisk/mkpatch.sh asterisk-X.Y.Z.tar.gz`
adds the module, its RESP client and the sample config, inserts the
libperfd check into `configure.ac`, `makeopts.in` and
`build_tools/menuselect-deps.in`, and appends the module's build rule to
`funcs/Makefile`.

## Configure it

On every perfcached node, the client secret and the collection the
dialplan uses (on a fleet, also list it in `[cluster] collections`):

```
[secrets]
client = client-secret-example

[collection calls]
buckets_log2 = 16
```

`perfd.conf` in the Asterisk configuration directory, for the native door
(fleet-aware: it learns the members from whichever seed answers and fails
over to a standby when a node dies):

```
[general]
protocol = binary
server = 192.0.2.11:6479
server = 192.0.2.12:6479
secret = client-secret-example
collection = calls
```

`protocol = resp` speaks RESP2 to perfcached's Redis door or to a
standalone Redis (for a replicated pair, the master's VIP), with `AUTH
<password>` or `AUTH <user> <password>`; after a failure or a `-READONLY`
from a demoted master it re-dials, starting at the next server, and
retries once.  Redis Cluster, Sentinel discovery and TLS are not
supported.  Every setting, with examples for perfcached and for Redis:
[CONFIGURATION.md](CONFIGURATION.md);
`configs/samples/perfd.conf.sample` documents them too (`make samples`
installs it).

Load it with `module load func_perfd.so`; apply a change with `module
reload func_perfd`.  A file that is refused is logged with the setting and
line, and a reload keeps the running configuration.

## Use it

Illustrative - this dialplan was not run for this page:

```
exten => s,1,Set(PERFD_SET(calls,${UNIQUEID},3600)=${CALLERID(num)})
 same => n,GotoIf($["${PERFDSTATUS}" != "OK"]?failed)
 same => n,Set(caller=${PERFD_GET(calls,${UNIQUEID})})
 same => n,GotoIf($["${PERFDSTATUS}" = "NOTFOUND"]?unknown)
 same => n,Set(PERFD_DELETE(calls,${UNIQUEID})=)
```

**Check it** from both sides: `perfd show status` on the Asterisk CLI
shows the pool and per-operation counters, and on perfcached the key the
dialplan wrote reads back with perfcli, in the same form as any other
(`perfcli -a <client secret> get calls <uniqueid>` answers
`{"found":true,"value":"...","ttl":...}`, or `{"found":false}` once
deleted).  `ERROR` in `PERFDSTATUS` always has a log line with the reason.

## Limits

- **Size.** Inside ordinary dialplan substitution Asterisk reads a function
  into a 4096-byte buffer, so a value longer than 4095 bytes is `ERROR`,
  never a truncated value.  `max_value` (default 65536) bounds it too.
- **Binary values.** A value containing a NUL byte is `ERROR`; store binary
  data through `BASE64_ENCODE` / `BASE64_DECODE`.
- **Timeouts.** A stalled server holds a call for `io_timeout_ms` per send
  and receive (default 300; libperfd's own default is 5000), and a call
  waits at most `wait_timeout_ms` for a free connection.  With a channel,
  the wait runs under autoservice.  Over `binary`, connections dial at the
  same time, never in turn; once a dial found no seed answering, that
  connection's calls are `ERROR` at once for 1 s, doubling to 30 s, until a
  dial succeeds, and each failing seed is logged once per outage.  Over
  `resp` a whole call is bounded by `call_timeout_ms`, and a bulk declared
  far over `max_value` closes the connection undrained.
- **Delete after a lost reply.** A delete retried after a lost reply can
  answer `0` for a key the first attempt removed.
- **Redis collections** are database numbers; a name is refused with
  `invalid DB index`.
- **Reads after a Redis failover.** A pooled connection opened before a VIP
  moved keeps answering reads from the demoted node until a write meets
  `-READONLY` and re-dials.

## Tests

Both run everything locally: servers on loopback, started and stopped by
the script, never a shared Redis.

- `test/resptest.sh <resptool>` - the RESP client against local Redis
  (`resptool` is `test/resptool.c` built with `resp.c`).
- `test/functest.sh <func_perfd.so> [phase...]` - the module in a private
  Asterisk instance with a three-node perfcached fleet and local Redis:
  every function over both protocols, value sizes, configuration refusals,
  node failure and stall, pool exhaustion, reload and unload with calls in
  flight, Redis role swap.

## Licence

`func_perfd.c`, `resp.c` and `resp.h` are GPL-2.0-or-later.  libperfd, which
the module links, is MIT (`lib/LICENSE` in perfcached) with libsodium's
notice; the patch carries none of its files.
