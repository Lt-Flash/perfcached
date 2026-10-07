# func_perfd - perfcached from the Asterisk dialplan

An optional Asterisk module with four dialplan functions that read and write
perfcached - or a standalone Redis - through a pool of connections:

| Function | Use | Returns |
|---|---|---|
| `PERFD_GET(collection,key)` | read | the value, or empty |
| `PERFD_SET(collection,key[,ttl])` | write | - |
| `PERFD_EXISTS(collection,key)` | read | `1` or `0` |
| `PERFD_DELETE(collection,key)` | read (deletes) or write (value ignored) | `1` removed, `0` absent |

Every call sets `PERFDSTATUS`: `OK`, `NOTFOUND` (the key was not there) or
`ERROR` (the reason is logged).  The status is how a stored empty value is told
from a missing key.  An empty `collection` uses the one in `perfd.conf`.

```
same => n,Set(PERFD_SET(calls,${UNIQUEID},3600)=${CALLERID(num)})
same => n,Set(caller=${PERFD_GET(calls,${UNIQUEID})})
same => n,GotoIf($["${PERFDSTATUS}" = "NOTFOUND"]?unknown)
same => n,Set(PERFD_DELETE(calls,${UNIQUEID})=)
```

`PERFD_SET` and `PERFD_DELETE` are registered as dangerous functions, so with
`live_dangerously = no` they cannot be reached from external interfaces.

## Protocols

The protocol is chosen in `perfd.conf`, never per call.

- **`binary`** - perfcached's native door through libperfd, the perfcached C
  client.  Encrypted off loopback (a client secret), and fleet-aware: it
  learns the members from the seed it reaches, fails over to a standby when
  a node dies, and can route each key to the node that owns it.
- **`resp`** - RESP2, to perfcached's RESP door or to a standalone Redis
  (for a replicated pair, the master's VIP).  `AUTH <password>`, or
  `AUTH <user> <password>` when `username` is set (Redis 6+).  After a
  failure, or a `-READONLY` from a demoted master, the connection is re-dialled
  starting at the server after the one that failed and the command is retried
  once.  Redis Cluster, Sentinel discovery and TLS are not supported.

## Building (Asterisk 22.10.1)

Needs `libsodium-dev` (and `libsodium23` at run time) and autoconf/automake.

```
cd asterisk-22.10.1
patch -p1 < asterisk-22.10.1-func_perfd.patch
./bootstrap.sh
./configure <your usual options>
make menuselect        # Dialplan Functions -> func_perfd (off by default)
make && make install
```

For a scripted build, `menuselect/menuselect --enable func_perfd
menuselect.makeopts` replaces the interactive step.  The patch does not carry
`configure`; `./bootstrap.sh` regenerates it from the patched `configure.ac`.
Without libsodium, menuselect shows the module as unavailable.

Developer-mode builds (`./configure --enable-dev-mode`) are not supported: the
vendored libperfd is compiled unmodified, and one of its internal functions
trips `-Wmissing-format-attribute`, which developer mode turns into an error.

The patch is generated, never edited by hand.  For another release, from a
perfcached checkout:

```
contrib/asterisk/mkpatch.sh asterisk-X.Y.Z.tar.gz
```

It exports libperfd with `tools/sync-libperfd.sh`, adds the module, its RESP
client and the sample config, and inserts the libsodium check into
`configure.ac`, `makeopts.in` and `build_tools/menuselect-deps.in`.

## Configuration

`configs/samples/perfd.conf.sample` documents every setting; `make samples`
installs it.  [CONFIGURATION.md](CONFIGURATION.md) lists them with examples
for perfcached and for Redis.  A change applies with `module reload func_perfd`; a file that is
refused is logged with the setting and line, and a reload keeps the running
configuration.  `perfd show status` shows the pool and per-operation counters.

## Limits

- **Size.** Inside ordinary dialplan substitution Asterisk reads a function
  into a 4096-byte buffer, so a value longer than 4095 bytes is `ERROR`, never
  a truncated value.  `max_value` (default 65536) bounds it too.
- **Binary values.** A value containing a NUL byte is `ERROR`; store binary
  data through `BASE64_ENCODE` / `BASE64_DECODE`.
- **Timeouts.** A stalled server holds a call for `io_timeout_ms` per send and
  receive (default 300; libperfd's own default is 5000), and a call waits at
  most `wait_timeout_ms` for a free connection.  With a channel, the wait runs
  under autoservice.
- **Delete after a lost reply.** A delete retried after a lost reply can
  answer `0` for a key the first attempt removed.
- **Redis collections** are database numbers; a name is refused with
  `invalid DB index`.
- **Reads after a Redis failover.** A pooled connection opened before a VIP
  moved keeps answering reads from the demoted node until a write meets
  `-READONLY` and re-dials.

## Tests

Both run everything locally: servers on loopback, started and stopped by the
script, never a shared Redis.

- `test/resptest.sh <resptool>` - the RESP client against local Redis
  (`resptool` is `test/resptool.c` built with `resp.c`).
- `test/functest.sh <func_perfd.so> [phase...]` - the module in a private
  Asterisk instance with a three-node perfcached fleet and local Redis:
  every function over both protocols, value sizes, configuration refusals,
  node failure and stall, pool exhaustion, reload and unload with calls in
  flight, Redis role swap.

## Licence

`func_perfd.c`, `resp.c`, `resp.h` and `dprint.h` are GPL-2.0-or-later.  The
libperfd files the patch adds under `funcs/perfd/lib` and `funcs/perfd/src`
are MIT, with their `LICENSE` and `NOTICE`.
