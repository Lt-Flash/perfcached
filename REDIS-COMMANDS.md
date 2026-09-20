# The Redis commands perfcached answers

perfcached speaks RESP2 itself, so unmodified Redis clients connect to
it as if it were Redis.  This page is the whole surface: every command
the door answers, its accepted forms, what it replies, and where it
differs from Redis - read out of the dispatch table and the handlers,
not from memory.  What is not here is not supported, and a client gets
`-ERR unknown command 'x'` for it.

## Reaching the door

Two listeners speak RESP:

- **`resp = <addr:port>`** under `[listen]` - the dedicated Redis door.
  RESP2 only; the native dialects and the admin verbs are refused on it.
  A Redis client cannot speak the Noise channel, so this listener is
  plaintext and guarded instead: off loopback `resp_allow = <cidr>[,…]`
  is required or the daemon refuses to start, `[secrets] resp` adds a
  Redis `AUTH` password, and `resp_collections` bounds which collections
  the door can see.
- **the native `tcp` listener**, which sniffs the dialect per message -
  a RESP client is served there too, but only where plaintext is
  allowed (`plaintext = loopback`: 127.0.0.1 and the unix socket).

**Collections are Redis databases.**  A connection starts on the
collection named `0`.  `SELECT n` moves it to the collection named `n`,
or to the one `resp_collections` maps `n` onto (`resp_collections =
0:sessions, 1:counters` makes `SELECT 0` reach `sessions`); a bare name
in that list is reachable by name.  A collection that is not declared
must be *named* `0`, `1`, … to be reachable at all.

**Authentication.**  With `[secrets] resp` set, every command but
`AUTH`, `HELLO` and `QUIT` answers `-NOAUTH Authentication required.`
until `AUTH` succeeds.  Without it, `AUTH` answers `-ERR Client sent
AUTH, but no password is set`, as Redis does.

**A node that is not ready** - still recovering its data - answers
every data command with `-LOADING node is not READY (recovering) …`,
Redis's own code for the condition; connection and cluster commands
still work, so a client can find another node.

**There are no `MOVED` or `ASK` redirects.**  A key that belongs to
another node is served through this one - forwarded on write, pulled on
read - and a cluster-aware client that reads the slot map (below) is
never redirected, only faster.  A forward that cannot be parked answers
`-TRYAGAIN cluster busy, retry`, Redis Cluster's own retryable code.

**Limits.**  Keys up to 4,096 bytes (`-ERR key too long`), values up to
64 KB (`-ERR value too large`), a pattern for `KEYS`/`SCAN MATCH` up to
255 bytes.  Expiry granularity is one second: `PX`, `PEXPIRE` and
`PEXPIREAT` round *up* to the next second, `PTTL` is seconds × 1000.
Values are strings; the type of every existing key is `string`.

## Connection and server

| command | reply | notes |
|---|---|---|
| `PING [message]` | `+PONG`, or the message as a bulk | |
| `ECHO message` | the message | |
| `QUIT` | `+OK`, then the connection closes | |
| `AUTH password` / `AUTH username password` | `+OK` | the username is accepted and ignored; a wrong password is `-WRONGPASS invalid username-password pair or user is disabled.` and is counted in `stats.resp.auth_fails` |
| `HELLO [protover]` | a 14-element map: `server perfcached`, `version`, `proto 2`, `id 0`, `mode standalone`, `role master`, `modules []` | protocol 2 only; `HELLO 3` is `-NOPROTO unsupported protocol version`.  `mode` and `role` are fixed strings for client compatibility; the truth is in `INFO cluster` and `CLUSTER INFO` |
| `SELECT index` | `+OK` | the collection named or mapped by `index`; `-ERR DB index is out of range` when no such collection is declared or the door may not see it |
| `DBSIZE` | integer | the number of records in the selected collection |
| `TIME` | `[seconds, microseconds]` | |
| `INFO [section]` | bulk, Redis's `# Section` format | sections: `server` (reports `redis_version:7.0.0` for tooling, `redis_mode`, `perfcached_version`, `perfcached_dialect:resp2-compat`), `replication` (`role:master`, no replicas), `cluster` (`cluster_enabled`), `clients`, `memory` (`used_memory` = the arena's held bytes), `stats` (`keyspace_hits`/`misses` summed over collections, and more), `keyspace` (one `dbN:keys=…` line per collection), `commandstats` (every door: RESP commands under their Redis names, the native doors as `json.<verb>` and `bin.<verb>`), `latencystats` (`latencystat_<cmd>:p50=,p99=,p99.9=` in ms, Redis 7's shape - each figure the log2 bucket bound the percentile falls under, 1 us to 32.768 ms; 32.768 means above the last bucket).  `all` or `everything` returns every section; a bare `INFO` returns the default set, which leaves out `commandstats` and `latencystats` as Redis does |
| `CLIENT LIST` | bulk, one line per connection | `CLIENT SETNAME name` is `+OK`; `CLIENT GETNAME` answers an empty string; other subcommands are `-ERR unsupported CLIENT subcommand` |
| `CONFIG GET pattern` | an empty array | nothing is exposed; `CONFIG RESETSTAT` resets the running totals (`+OK`); `CONFIG SET` is `-ERR unsupported CONFIG subcommand` |
| `SLOWLOG GET [count]` | Redis's slowlog entry array | entries slower than `[daemon] slowlog_usec`, from every door - a native entry's argv is `json.<verb>` or `bin.<verb>`, collection, key; `count` defaults to 10, `-1` gives up to 512; `SLOWLOG LEN`, `SLOWLOG RESET`; the newest 32 are `slowlog` on `/stats` |
| `COMMAND` | an empty array | enough for `redis-cli` to start; no subcommands |
| `FLUSHDB`, `FLUSHALL` | `-ERR FLUSHDB is not supported (delete keys explicitly)` | deliberately: no verb empties a collection from the Redis door |

## Pub/sub

Channels are one global string space on every door: Redis pub/sub
ignores `SELECT`, so the db-to-collection mapping never applies here,
and a subscriber on the RESP door hears a publish from any door.  A
subscription lives on the connection that made it and dies with it.

| command | reply | notes |
|---|---|---|
| `SUBSCRIBE channel [channel ...]` | one `["subscribe", channel, count]` per channel | `count` is the connection's subscriptions, channels and patterns together, as Redis counts them |
| `UNSUBSCRIBE [channel ...]` | one `["unsubscribe", channel, count]` per channel dropped | with no argument every channel goes, one confirmation each; with nothing subscribed the channel is nil and the count 0 |
| `PSUBSCRIBE pattern [pattern ...]` / `PUNSUBSCRIBE [pattern ...]` | as above with `psubscribe` / `punsubscribe` | patterns match as Redis matches them: `*`, `?`, `[abc]`, `[^a]`, `[a-z]`, `\` |
| `PUBLISH channel message` | integer | the number of subscribers on **this node** that received it, as Redis Cluster answers; on a fleet the message also goes to every live peer for its own subscribers (`stats.pubsub.relay_sent`), at-most-once with gaps counted (`relay_lost`) - a payload over a datagram, about 65,000 bytes, is delivered locally only and counted in `relay_dropped`; a channel under `__pc.` is refused as reserved for the daemon's own events |
| delivery | `["message", channel, payload]` or `["pmessage", pattern, channel, payload]` | on the subscribing connection, byte-clean; a subscriber that does not read is closed at the output cap, which is Redis's client-output-buffer-limit behaviour, and counted in `stats.pubsub.slow_kills` |
| `PUBSUB CHANNELS [pattern]` | array of channel names with at least one subscriber | |
| `PUBSUB NUMSUB [channel ...]` | `[channel, count, ...]` | O(1) per name |
| `PUBSUB NUMPAT` | integer | distinct patterns subscribed |
| `RESET` | `+RESET` | leaves subscribed mode, back to db 0 |
| `SSUBSCRIBE`, `SUNSUBSCRIBE`, `SPUBLISH` | `-ERR unknown command` | sharded pub/sub is not offered: a classic publish already reaches every node |

**Keyspace notifications** are Redis's, per collection: with
`[collection X] notify_events = store, remove, expired, miss` (or
`all`) a mutation publishes `__keyspace@<collection>__:<key>` with the
event as payload and `__keyevent@<collection>__:<event>` with the key,
events `set`, `del`, `expired` and `keymiss`, so `PSUBSCRIBE
__keyevent@0__:*` works as on Redis.  They are emitted by the node that
applies the change and never relayed: attach to one node for that
node's stream, to every node for the fleet's, as with Redis Cluster's
primaries.

**Subscribed mode** is exactly Redis's: once a connection holds a
subscription, only `SUBSCRIBE`, `UNSUBSCRIBE`, `PSUBSCRIBE`,
`PUNSUBSCRIBE`, `PING`, `QUIT` and `RESET` are accepted, anything else
is `-ERR Can't execute 'get': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE /
PING / QUIT / RESET are allowed in this context`, and `PING` answers
the array form `["pong", ""]`.  `HELLO 3` is refused as everywhere on
this door.  Accepted sockets carry TCP keepalive (`[listen]
keepalive_s`, default 300 as Redis's `tcp-keepalive`), so a subscriber
whose host died silently is found by the kernel; the daemon itself
never closes an idle connection.

## Keys and strings

| command | reply | notes |
|---|---|---|
| `GET key` | bulk, or nil on a miss | on a cluster a miss on this node may be answered from a peer; a read that could not be parked answers nil |
| `SET key value [EX seconds \| PX milliseconds]` | `+OK` | `EX`/`PX` only - `NX`, `XX`, `GET`, `KEEPTTL`, `EXAT`, `PXAT` are `-ERR unsupported SET option`; `PX` rounds up to whole seconds; an expiry ≤ 0 is `-ERR invalid expire time in 'set' command` |
| `SETEX key seconds value`, `PSETEX key milliseconds value` | `+OK` | as `SET … EX/PX` |
| `MGET key [key …]` | array of bulk or nil | |
| `DEL key [key …]`, `UNLINK key [key …]` | integer: keys removed | a key over 4,096 bytes in a multi-key call is skipped, not an error |
| `EXISTS key [key …]` | integer: how many exist | |
| `TYPE key` | `+string` or `+none` | a JSON document is a string too |
| `TTL key`, `PTTL key` | integer | `-2` no such key, `-1` no expiry, else the remaining seconds (`PTTL`: × 1000) |
| `EXPIRE key seconds`, `PEXPIRE key ms` | `1` set, `0` no such key | a value ≤ 0 deletes the key (`1` if it existed); `NX`/`XX`/`GT`/`LT` are not accepted |
| `EXPIREAT key unix-seconds`, `PEXPIREAT key unix-ms` | as above | converted against the node's clock |
| `INCR key`, `DECR key`, `INCRBY key n`, `DECRBY key n` | integer: the new value | a missing key starts at 0; a value that is not an integer is `-ERR value is not an integer or out of range`; serialized at the owner on a cluster |
| `KEYS pattern` | array of keys | glob patterns; `*` walks everything.  Cooperative: the walk runs a chunk per event-loop turn, so it does not stall the worker's other connections.  Hard cap 100,000 keys; past the reply buffer it is `-ERR reply too large`.  At-least-once across a table split, like Redis `SCAN` |
| `SCAN cursor [MATCH pattern] [COUNT n] [TYPE t]` | `[next-cursor, [keys…]]` | `COUNT` 1..16384, default 128 (`-ERR invalid COUNT`); `TYPE` is accepted and ignored (strings only); `cursor` 0..2³²−1 (`-ERR invalid cursor`); anything else is `-ERR syntax error` |
| `MEMORY USAGE key [SAMPLES n]` | integer: bytes, or nil | 24 + key length + value length; `SAMPLES` is accepted and ignored; other `MEMORY` subcommands are refused |

## JSON (RedisJSON compatible)

Paths are `$`, `.name` (a dotted path from the root) and `[index]`; a
path given without a leading `$` or `.` is taken as `.path`.  A `$` path
answers RedisJSON v2 style (an array of matches); a `.path` answers the
bare value.  The document is stored as one value, under the same 64 KB
limit; `TYPE` reports it as a string.

| command | reply | notes |
|---|---|---|
| `JSON.SET key path value [NX \| XX] [EX seconds]` | `+OK`, or nil when `NX`/`XX` declined | `EX` is an extension: the store sets a TTL in the same call; without it a field update keeps the key's expiry.  Other options are `-ERR unsupported JSON.SET option` |
| `JSON.GET key [path]` | bulk JSON, or nil | no path = the whole document |
| `JSON.DEL key [path]` | integer: 1 deleted, 0 not found | no path = the whole key |
| `JSON.NUMINCRBY key path number` | bulk: the new number | `-ERR value is not an integer (…)` when the target is not numeric |
| `JSON.ARRAPPEND key path value [value …]` | integer: the array's new length | |
| `JSON.DEBUG HELP` | the two lines below | the probe OpenSIPS's `cachedb_redis` sends at connect |
| `JSON.DEBUG MEMORY key [path]` | integer: bytes of the fragment | `-WRONGTYPE …` on a key that is not a JSON document |

A JSON command on a key that another node holds (proxy and shard
placement) is `-ERR JSON on a key another node holds is not served on
the RESP door - dial that node, or use the native dialect`, which
forwards it.  `-ERR path does not exist` and `-ERR bad path` are what
they say.

## Cluster

The door publishes the topology a Redis Cluster client expects, over a
Redis-compatible slot: `crc16(key) % 16384`, with `{hash tags}` handled
including the edge cases.  A cluster-aware client therefore places keys
itself and sends each to its owner - the measured difference is in the
README - and nothing is ever redirected.

| command | reply | notes |
|---|---|---|
| `CLUSTER INFO` | bulk, Redis's `key:value` lines | `cluster_enabled:1`, `cluster_state:ok`, all 16384 slots assigned, `cluster_known_nodes` = the fleet size; on a standalone node `cluster_enabled:0`, `cluster_state:fail` |
| `CLUSTER NODES` | bulk, one line per member: id, `addr:port@cport`, flags (`myself,master` / `master`), … slot ranges | |
| `CLUSTER SLOTS` | array of `[start, end, [addr, port, id]]` | one owner per range, every node a master |
| `CLUSTER SHARDS` | Redis 7's shard array | `slots`, then `nodes` with `port`, `role master`, `replication-offset 0` |
| `CLUSTER KEYSLOT key` | integer | the slot, hash tags honoured |
| `CLUSTER MYID` | bulk: this node's 40-character id | `-ERR this node is not in a cluster` standalone |
| `CLUSTER COUNTKEYSINSLOT slot` | `-ERR unsupported CLUSTER subcommand` | not served |

`NODES`, `SLOTS` and `SHARDS` on a node that is not clustered answer
`-ERR This instance has cluster support disabled`, the Redis text.

## Errors and codes a client should handle

| reply | when |
|---|---|
| `-NOAUTH Authentication required.` | a password is set and the connection has not authenticated |
| `-WRONGPASS …` | `AUTH` with the wrong password |
| `-NOPROTO unsupported protocol version` | `HELLO 3` |
| `-LOADING node is not READY (recovering) …` | a data command on a node still recovering; retry, or use another node |
| `-TRYAGAIN cluster busy, retry` | a write that had to be forwarded and could not be parked (`[cluster] max_pending`); the request has not happened and a retry is correct |
| `-ERR holder timed out` | a forwarded request whose answer missed its deadline; **not** retryable for `INCR` - the holder may have applied it |
| `-ERR forward failed` | the owner could not be reached |
| `-ERR node is FAILED (its WAL is losing acknowledged writes) …` | the node refused writes after a WAL overrun; reads still work; an operator fixes it |
| `-ERR cache full` | the arena is at its ceiling; nothing is evicted |
| `-ERR value too large`, `-ERR key too long` | the limits above |
| `-ERR busy` | the request would have needed to wait on a peer where the door cannot wait |
| `-ERR reply too large` | a `KEYS` or `SCAN` reply past the buffer |
| `-WRONGTYPE …` | a JSON command on a non-JSON value |
| `-ERR unknown command 'x'` | everything not on this page |

## Not supported, and what a client gets

Everything below answers `-ERR unknown command` unless stated.  Some
of it is on the roadmap (pub/sub is filed against the fleet's real
traffic; sorted sets have a pinned design) - see [CHANGELOG.md](CHANGELOG.md)
for what lands when.

- **Strings:** `MSET`, `MSETNX`, `SETNX` (use `SET`), `GETSET`, `GETDEL`,
  `GETEX`, `APPEND`, `STRLEN`, `SETRANGE`, `GETRANGE`, `INCRBYFLOAT`.
  `SET` options `NX`, `XX`, `GET`, `KEEPTTL`, `EXAT`, `PXAT` are
  `-ERR unsupported SET option`.
- **Keys:** `PERSIST`, `EXPIRETIME`, `RENAME`, `RENAMENX`, `TOUCH`,
  `RANDOMKEY`, `OBJECT`, `DUMP`/`RESTORE` (the native dialect has its own),
  `MOVE`, `COPY`, `SWAPDB`.  `EXPIRE`'s `NX`/`XX`/`GT`/`LT` flags.
- **Data types:** hashes, lists, sets, sorted sets, streams, bitmaps,
  HyperLogLog, geo.
- **Transactions and scripting:** `MULTI`/`EXEC`/`DISCARD`/`WATCH`,
  `EVAL`/`EVALSHA`/`SCRIPT`/`FUNCTION`.
- **Messaging:** `PUBLISH`, `SUBSCRIBE`, `PSUBSCRIBE`, `PUBSUB`.
- **Server:** `FLUSHDB`/`FLUSHALL` (their own error), `SAVE`/`BGSAVE`
  (the native `save` verb), `MONITOR`, `DEBUG`, `WAIT`, `ACL`, `RESET`,
  `CONFIG SET`, `CLIENT` beyond `LIST`/`SETNAME`/`GETNAME`,
  `CLUSTER COUNTKEYSINSLOT` and the cluster management commands,
  RESP3 (`HELLO 3`).
