# func_perfd configuration

`perfd.conf` in the Asterisk configuration directory.  Every setting goes
in `[general]`.  Apply a change with `module reload func_perfd`.  If the
file is refused, the log names the setting and line; at load the module
declines, and on a reload the running configuration stays.  An unknown
setting is refused, not ignored.

## Settings

| Setting | Protocol | Default | Values |
|---|---|---|---|
| `protocol` | - | `binary` | `binary` or `resp` |
| `server` | both | required | `host:port`, `[v6]:port` or `unix:/path`; repeatable |
| `secret` | binary | - | a perfcached client secret; repeatable |
| `password` | resp | - | sent as `AUTH` |
| `username` | resp | - | Redis 6+ ACL user; needs `password` |
| `collection` | both | - | used when a function's collection argument is empty |
| `pool` | both | `4` | 1..64 |
| `connect_timeout_ms` | both | `1000` | 1..60000 |
| `io_timeout_ms` | both | `300` | 1..60000 |
| `wait_timeout_ms` | both | `300` | 1..60000 |
| `default_ttl` | both | `0` | 0..2147483647 seconds, 0 = no expiry |
| `max_value` | both | `65536` | 1..67108864 bytes |
| `policy` | binary | `failover` | `failover`, `round_robin`, `least_conn`, `weighted` |
| `spares` | binary | `0` | `none` or -1..64 |
| `route_keys` | binary | `no` | `yes` or `no` |
| `keepalive_s` | binary | `30` | -1..86400, -1 = off |

- **`protocol`** - `binary` is perfcached's native door through libperfd:
  encrypted off loopback, and it knows the fleet.  `resp` is RESP2, to
  perfcached's RESP door or to a Redis server.
- **`server`** - tried in the order written.  With `binary` each line is a
  seed: the fleet's members are learned from whichever one answers.  With
  `resp`, a reconnect after a failure starts at the line after the one that
  failed.
- **`secret`** - required with `binary` unless every server is loopback or a
  unix socket.  Repeat the line to rotate: add the new secret, then remove
  the old one once the daemons have dropped it.
- **`password` / `username`** - `resp` only.  The password crosses the
  network in the clear.  Leave `username` out for Redis older than 6.
- **`collection`** - on perfcached, a collection name.  On Redis, a database
  number (`0`..`15` by default); a name is refused with `invalid DB index`.
- **`pool`** - connections held open.  A call borrows one for its round trip.
- **`connect_timeout_ms`** - per dial attempt.
- **`io_timeout_ms`** - per send and per receive.  This is how long a stalled
  server can hold a call.
- **`wait_timeout_ms`** - how long a call waits for a free connection before
  it is `ERROR`.
- **`default_ttl`** - applies when `PERFD_SET` has no third argument.
- **`max_value`** - a longer value is `ERROR`, never truncated.  Inside
  ordinary dialplan substitution Asterisk cannot pass more than 4095 bytes
  anyway.
- **`policy`** - which member a connection works through: stay where it
  connected (`failover`), or spread by `round_robin`, `least_conn` or
  `weighted` free memory.
- **`spares`** - standby connections per pooled connection, so a node failure
  is a swap: `0` or `-1` = one to every other member, `N` = at most N, `none` = no
  standbys and no fleet learned (no failover).
- **`route_keys`** - send each request to the node that owns its key.  Needs
  spares.
- **`keepalive_s`** - TCP keepalive idle time on binary connections.

Sockets held by one Asterisk: about `pool` x (members) with the default
`spares`, `pool` with `spares = none` or `resp`.

## perfcached

### Server side

On every node, the client secret and the collection the dialplan uses
(on a fleet, also list it in `[cluster] collections`):

```
[secrets]
client = CHANGE-ME-client

[collection calls]
```

For `protocol = resp` also open a RESP listener.  Off-box it needs an
allow-list, and its password must differ from the secrets:

```
[listen]
resp = 192.0.2.11:6379
resp_allow = 192.0.2.0/24

[secrets]
resp = CHANGE-ME-resp-password
```

### Binary

```
[general]
protocol = binary
server = 192.0.2.11:6479
server = 192.0.2.12:6479
secret = CHANGE-ME-client
collection = calls
pool = 4
io_timeout_ms = 300
```

### RESP

```
[general]
protocol = resp
server = 192.0.2.11:6379
server = 192.0.2.12:6379
password = CHANGE-ME-resp-password
collection = calls
pool = 4
```

### Dialplan

```
exten => s,1,Set(PERFD_SET(calls,${UNIQUEID},3600)=${CALLERID(num)})
 same => n,GotoIf($["${PERFDSTATUS}" != "OK"]?failed)
 same => n,Set(caller=${PERFD_GET(calls,${UNIQUEID})})
 same => n,GotoIf($["${PERFDSTATUS}" = "NOTFOUND"]?unknown)
 same => n,Set(seen=${PERFD_EXISTS(,blocked-${CALLERID(num)})})
 same => n,Set(PERFD_DELETE(calls,${UNIQUEID})=)
```

An empty collection argument (as in `PERFD_EXISTS` above) uses
`collection = calls`.

## Redis

A standalone server, or a replicated master behind a VIP whose health check
picks the master.  Redis Cluster, Sentinel discovery and TLS are not
supported.

### perfd.conf

```
[general]
protocol = resp
server = 192.0.2.20:6379
password = CHANGE-ME-redis-password
;username = asterisk            ; Redis 6+ ACL user only
collection = 0
pool = 4
```

A direct master/replica pair works too: list both servers.  A write that
reaches the replica gets `-READONLY`, and the connection re-dials the next
server and retries once.

```
server = 192.0.2.21:6379
server = 192.0.2.22:6379
```

### Dialplan

The collection is a database number:

```
exten => s,1,Set(PERFD_SET(0,cid-${UNIQUEID},600)=${CALLERID(num)})
 same => n,Set(caller=${PERFD_GET(0,cid-${UNIQUEID})})
 same => n,Set(PERFD_DELETE(0,cid-${UNIQUEID})=)
```

## Checking it

```
asterisk -rx "perfd show status"
```

This shows the protocol, the servers, each pooled connection, and counters
for hits, misses, errors, wait timeouts, reconnects and failovers.
Per-call failures are logged as warnings, at most ten a second.
