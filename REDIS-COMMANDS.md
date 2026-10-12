# The Redis commands perfcached answers

perfcached's own clients speak its native binary protocol.  For
applications written against Redis it also has a Redis
door that answers RESP2, so unmodified Redis clients can use it too.
This page is that door's whole surface: every command it answers, its accepted forms, what it replies, and where it
differs from Redis - read out of the dispatch table and the handlers,
not from memory.  What is not here is not supported, and a client gets
`-ERR unknown command 'x'` for it.

## Reaching the door

Two listeners speak RESP:

- **`resp = <addr:port>`** under `[listen]` - the dedicated Redis door.
  RESP2 only; the native protocol and the admin verbs are refused on it.
  A Redis client cannot speak the Noise channel, so this listener is
  plaintext and guarded instead: off loopback `resp_allow = <cidr>[,…]`
  is required or the daemon refuses to start, `[secrets] resp` adds a
  Redis `AUTH` password, and `resp_collections` bounds which collections
  the door can see.
- **the native `tcp` listener**, which tells binary frames and RESP
  apart by the first byte of each message - a RESP client is served
  there too, but only where plaintext is
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

**Keys another node owns.**  By default a key that belongs to another
node is served through this one - forwarded on write, pulled on read -
so a plain client never sees a redirect.  With `[listen] resp_redirect
= moved`, a `shard` or `spread` node instead answers a key it does not
own with a genuine `-MOVED <slot> <addr:port>` and does not forward it;
that is what makes a cluster-aware client refresh its slot table.
There are no `ASK` redirects.  A forward that cannot be parked answers
`-TRYAGAIN cluster busy, retry`, Redis Cluster's own retryable code.

**Limits.**  Keys up to 4,096 bytes (`-ERR key too long`).  A record -
key, value and a 28-byte header - up to 262,080 bytes, so a value of
just under 256 KB (`-ERR value too large`); a JSON document up to 64 KB.
A pattern for `KEYS`/`SCAN MATCH` up to
255 bytes.  Expiry granularity is one second: `PX`, `PEXPIRE` and
`PEXPIREAT` round *up* to the next second, `PTTL` is seconds × 1000.
A key holds a string (counters are strings) or a JSON document.

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
| `INFO [section]` | bulk, Redis's `# Section` format | sections: `server` (reports `redis_version:7.0.0` for tooling, `redis_mode`, `perfcached_version`, `perfcached_dialect:resp2-compat`), `replication` (`role:master`, no replicas), `cluster` (`cluster_enabled`), `clients`, `memory` (`used_memory` = the arena's held bytes), `stats` (`keyspace_hits`/`misses` summed over collections, and more), `keyspace` (one `dbN:keys=…` line per collection), `commandstats` (every door: RESP commands under their Redis names, the native door as `cmd:<method>` and `bin:<verb>`), `latencystats` (`latencystat_<cmd>:p50=,p99=,p99.9=` in ms, Redis 7's shape - each figure the log2 bucket bound the percentile falls under, 1 us to 32.768 ms; 32.768 means above the last bucket).  `all` or `everything` returns every section; a bare `INFO` returns the default set, which leaves out `commandstats` and `latencystats` as Redis does |
| `CLIENT LIST` | bulk, one line per connection | `CLIENT SETNAME name` is `+OK`; `CLIENT GETNAME` answers an empty string; other subcommands are `-ERR unsupported CLIENT subcommand` |
| `CONFIG GET pattern` | an empty array | nothing is exposed; `CONFIG RESETSTAT` resets the running totals (`+OK`); `CONFIG SET` is `-ERR unsupported CONFIG subcommand` |
| `SLOWLOG GET [count]` | Redis's slowlog entry array | entries slower than `[daemon] slowlog_usec`, from every door - a native entry's argv is `cmd:<method>` or `bin:<verb>`, collection, key; `count` defaults to 10, `-1` gives up to 512; `SLOWLOG LEN`, `SLOWLOG RESET`; the newest 32 are `slowlog` on `/stats` |
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
| `GET key` | bulk, or nil on a miss | on a cluster a miss on this node may be answered from a peer; a read that could not be parked answers nil.  On a JSON document: `-WRONGTYPE Operation against a key holding the wrong kind of value` (see *Types* below) |
| `SET key value [EX seconds \| PX milliseconds] [NX \| XX \| IFEQ cmp \| IFNE cmp]` | `+OK`, or nil when the condition declined | `NX` stores only if the key is absent (an expired key is absent), `XX` only if present. `IFEQ cmp` (Redis 8.4) stores only if the key holds a string equal to `cmp`, `IFNE cmp` only if it is absent or holds a different one; a key holding a hash or a JSON document is `WRONGTYPE`, and a counter compares as its decimal text. Two conditions together are `-ERR syntax error`; `IFDEQ`/`IFDNE` (digest compares) are refused - there is no `DIGEST`. In a cluster every conditional set of a key is decided by one node, the key's slot owner, so two clients on two nodes cannot both be granted `SET k v EX n NX`, and of two racing `SET k v IFEQ old` exactly one wins; a proxy or pull-only cluster refuses them. A token lock: acquire with `SET k token EX n NX`, refresh with `SET k token IFEQ token EX n`, release with `DELEX k IFEQ token`. `GET`, `KEEPTTL`, `EXAT`, `PXAT` are `-ERR unsupported SET option`; `PX` rounds up to whole seconds; an expiry ≤ 0 is `-ERR invalid expire time in 'set' command` |
| `SETEX key seconds value`, `PSETEX key milliseconds value` | `+OK` | as `SET … EX/PX` |
| `MGET key [key …]` | array of bulk or nil | nil for a JSON document, as Redis |
| `DEL key [key …]`, `UNLINK key [key …]` | integer: keys removed | a key over 4,096 bytes in a multi-key call is skipped, not an error |
| `DELEX key [IFEQ cmp \| IFNE cmp]` | integer: 1 deleted, 0 not | Redis 8.4's conditional delete: `IFEQ` deletes only if the key holds a string equal to `cmp`, `IFNE` only if it holds a different one (an absent key is 0 either way). Without a condition it is `DEL` of one key, of any type. A condition on a key that is not a string is `-ERR Key should be of string type if conditions are specified`. `IFDEQ`/`IFDNE` are refused (no `DIGEST`). Decided on the key's slot owner in a cluster, as `SET NX` is |
| `EXISTS key [key …]` | integer: how many exist | |
| `TYPE key` | `+string`, `+ReJSON-RL` or `+none` | `ReJSON-RL` for a key a JSON command wrote, whichever `redis_types` |
| `TTL key`, `PTTL key` | integer | `-2` no such key, `-1` no expiry, else the remaining seconds (`PTTL`: × 1000) |
| `EXPIRE key seconds`, `PEXPIRE key ms` | `1` set, `0` no such key | a value ≤ 0 deletes the key (`1` if it existed); `NX`/`XX`/`GT`/`LT` are not accepted |
| `EXPIREAT key unix-seconds`, `PEXPIREAT key unix-ms` | as above | converted against the node's clock |
| `INCR key`, `DECR key`, `INCRBY key n`, `DECRBY key n` | integer: the new value | a missing key starts at 0; a value that is not an integer is `-ERR value is not an integer or out of range`, a JSON document `-WRONGTYPE …`; serialized at the owner on a cluster |
| `KEYS pattern` | array of keys | glob patterns; `*` walks everything.  Cooperative: the walk runs a chunk per event-loop turn, so it does not stall the worker's other connections.  Hard cap 100,000 keys; past the reply buffer it is `-ERR reply too large`.  At-least-once across a table split, like Redis `SCAN` |
| `SCAN cursor [MATCH pattern] [COUNT n] [TYPE t]` | `[next-cursor, [keys…]]` | `COUNT` 1..16384, default 128 (`-ERR invalid COUNT`); `TYPE` is accepted and ignored - keys of both types are returned; `cursor` 0..2³²−1 (`-ERR invalid cursor`); anything else is `-ERR syntax error` |
| `MEMORY USAGE key [SAMPLES n]` | integer: bytes, or nil | 24 + key length + value length; `SAMPLES` is accepted and ignored; other `MEMORY` subcommands are refused |

## JSON (RedisJSON compatible)

Paths are `$`, `.name` (a dotted path from the root) and `[index]`; a
path given without a leading `$` or `.` is taken as `.path`.  A `$` path
answers RedisJSON v2 style (an array of matches); a `.path` answers the
bare value.  The document is stored as one value of up to 64 KB, and
is a TYPE of its own: `TYPE` reports `ReJSON-RL`.

**Types (`redis_types`).**  A key written by a JSON command is a JSON
document; a key written by `SET`, `SETEX`, `PSETEX` or `INCR` is a string -
whatever its bytes are, so `SET k '{"a":1}'` is a string.  A `SET` over a
document makes it a string, as in Redis.  The type travels with the key
through the WAL, the snapshot, replication, migration and `dump`.  Per
collection (default `[daemon] redis_types`, default `strict`):

- `strict` - Redis 8's rules, on every door (RESP, native):
  `GET`, `INCR` and the like on a document answer `-WRONGTYPE Operation
  against a key holding the wrong kind of value`; `MGET` answers nil for
  it; every JSON command on a string answers `-Existing key has wrong
  Redis type` (RedisJSON's words, no error code).  A client that probes a
  key with `GET` and switches to `JSON.GET` on `WRONGTYPE` works.
- `loose` - perfcached's behaviour before 0.4.0: a document is served as its
  text by the string commands, and the JSON commands read any string that
  parses as JSON.  `TYPE` still reports the recorded type.

| command | reply | notes |
|---|---|---|
| `JSON.SET key path value [NX \| XX] [EX seconds]` | `+OK`, or nil when `NX`/`XX` declined | `EX` is an extension: the store sets a TTL in the same call; without it a field update keeps the key's expiry.  Other options are `-ERR unsupported JSON.SET option` |
| `JSON.GET key [path]` | bulk JSON, or nil | no path = the whole document; a string under `strict` is `-Existing key has wrong Redis type` |
| `JSON.DEL key [path]` | integer: 1 deleted, 0 not found | no path = the whole key |
| `JSON.NUMINCRBY key path number` | bulk: the new number | `-ERR value is not an integer (…)` when the target is not numeric |
| `JSON.ARRAPPEND key path value [value …]` | integer: the array's new length | |
| `JSON.DEBUG HELP` | the two lines below | the capability probe RedisJSON-aware clients send at connect |
| `JSON.DEBUG MEMORY key [path]` | integer: bytes of the fragment | `-WRONGTYPE …` on a key that is not a JSON document |

A JSON command on a key that another node holds (proxy and shard
placement) is `-ERR JSON on a key another node holds is not served on
the RESP door - dial that node, or use the native door`, which
forwards it.  `-ERR path does not exist` and `-ERR bad path` are what
they say.

## Hashes

A hash is one record holding every field (insertion order), so the
whole hash - names and values - stays under 58,000 bytes; a write that
would grow it past that is refused with `-ERR hash too large - this is a
cache`.  Writes run on the key's deciding node, so two clients writing
different fields of one hash through different nodes both land.  `TYPE`
answers `hash`; string commands on it, and hash commands on a string,
answer `WRONGTYPE`; the last `HDEL` deletes the key; `HSET` keeps the
key's TTL.  Every node of a fleet must run 0.4.4 or later.

| command | reply | notes |
|---|---|---|
| `HSET key field value [field value ...]`, `HMSET` | fields added / `+OK` | |
| `HSETNX key field value` | `1` set, `0` the field existed | |
| `HGET key field`, `HMGET key field [field ...]` | bulk or nil / array | |
| `HDEL key field [field ...]` | fields removed | |
| `HEXISTS`, `HLEN`, `HSTRLEN` | integer | `0` for an absent key or field |
| `HKEYS`, `HVALS`, `HGETALL` | array, insertion order | `HGETALL` is field, value, field, value ... |
| `HINCRBY key field n` | the new integer | Redis's errors: `hash value is not an integer`, `increment or decrement would overflow` |
| `HINCRBYFLOAT key field n` | the new value, Redis's formatting (`10.5`, `5200`) | `hash value is not a float`, `value is not a valid float`, `increment would produce NaN or Infinity` |
| `HRANDFIELD key [count [WITHVALUES]]` | bulk / array | a negative count may repeat fields |
| `HSCAN key cursor [MATCH p] [COUNT n] [NOVALUES]` | `[0, [...]]` | everything in one call, cursor `0` - a hash is one small record |

The native protocol has every command as `hcmd {col, key, cmd, args}`,
which answers the command's own reply (an integer, a string, nil, an
array, `OK`, or the command's error) - perfcli shows it as JSON.

## Rate limiting (a perfcached extension)

Not a Redis command: one call for what a Redis client does as
`MULTI` / `ZADD k now now` / `ZREMRANGEBYSCORE k -inf now-window` /
`ZCOUNT` / `EXPIRE` / `EXEC` - a sliding-window counter - in one round
trip, decided on one node so hits through every node count together.

| command | reply | notes |
|---|---|---|
| `RL.HIT key window_ms [limit]` | array `[count, allowed]` | Records this hit, drops hits older than `window_ms`, and answers how many are in the window and `1` if that is within `limit` (`allowed` is always `1` without one).  An over-limit hit is recorded too, as the ZADD-first idiom does.  `window_ms` 1..86400000, `limit` 1..5999; the key expires a moment after its window.  `TYPE` answers `ratelimit`, and `GET` on it is `WRONGTYPE` |

The key's slot owner decides it, in shard and eager collections; a
proxy or pull-only cluster answers `-ERR RL.HIT needs a shard or eager
collection`.  Every node of a fleet must run 0.4.4 or later: until then
it answers `-ERR RL.HIT needs every node in the fleet on 0.4.4 or
later`.  The native protocol has it as `rlhit {col, key, window_ms,
limit}` -> `{"count": n, "allowed": bool}`.

## Symfony Lock scripts (approved scripts only)

perfcached runs no Lua and no client-supplied script.  It recognises the
Lua scripts of Symfony's Lock component (its `RedisStore`, write and read
locks, as every Symfony release from 5.4 to 7.4 sends them) by the SHA1 of
their bodies and runs each as its own built-in code, with the same
answers, so an unmodified Symfony application can keep its locks here.

| Command | Reply | Notes |
|---|---|---|
| `EVALSHA sha1 1 key [arg …]`, `EVAL script 1 key [arg …]` | the script's answer: `:1` (true) or nil (false) | One of the approved Symfony scripts, with exactly one key. Any other script is refused: `EVALSHA` of an unknown SHA1 answers `-NOSCRIPT No matching script. Please use EVAL.`, `EVAL` of another body `-ERR script not approved on this server ...`. The lock lives in a record of its own (`TYPE` says `zset`); a key holding a string makes the scripts answer false, as Symfony's own type check does, and a hash or a document is `WRONGTYPE`. Each member keeps its expiry in milliseconds; the key's own expiry is whole seconds, so an empty lock key can linger for under a second. In a cluster a lock is decided on its key's slot owner (shard or eager), and every node must run 0.5.0 or later - an older member is named in the refusal |
| `SCRIPT LOAD script` | the SHA1, as Redis gives it | Only an approved body; another is refused, so a client never believes it loaded a script that will not run |
| `SCRIPT EXISTS sha1 [sha1 …]` | array: 1 approved, 0 not | |
| `SCRIPT FLUSH [ASYNC \| SYNC]` | `+OK` | Nothing is cached, so nothing changes |

## Cluster

The door publishes the topology a Redis Cluster client expects, over a
Redis-compatible slot: `crc16(key) % 16384`, with `{hash tags}` handled
including the edge cases.  A cluster-aware client therefore places keys
itself and sends each to its owner - the measured difference is in
[GUIDE.md](GUIDE.md#measured).  A node forwards a key it does not own,
or answers `-MOVED` with `resp_redirect = moved` (above).

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
| `-MOVED <slot> <addr:port>` | `resp_redirect = moved`, and the key belongs to another node; send it there |
| `-WRONGTYPE …` | a string command on a JSON document (`redis_types = strict`) |
| `-Existing key has wrong Redis type` | a JSON command on a string (`redis_types = strict`) |
| `-ERR unknown command 'x'` | everything not on this page |

## Not supported, and what a client gets

Everything below answers `-ERR unknown command` unless stated.  What
each release adds is in the [ChangeLog](ChangeLog).

- **Strings:** `MSET`, `MSETNX`, `SETNX` (use `SET key value NX`), `GETSET`, `GETDEL`,
  `GETEX`, `APPEND`, `STRLEN`, `SETRANGE`, `GETRANGE`, `INCRBYFLOAT`.
  `SET` options `GET`, `KEEPTTL`, `EXAT`, `PXAT` are
  `-ERR unsupported SET option`.  `DIGEST`, and the `IFDEQ`/`IFDNE`
  digest conditions of `SET` and `DELEX`.
- **Keys:** `PERSIST`, `EXPIRETIME`, `RENAME`, `RENAMENX`, `TOUCH`,
  `RANDOMKEY`, `OBJECT`, `DUMP`/`RESTORE` (the native protocol has its own),
  `MOVE`, `COPY`, `SWAPDB`.  `EXPIRE`'s `NX`/`XX`/`GT`/`LT` flags.
- **Data types:** lists, sets, sorted sets, streams, bitmaps,
  HyperLogLog, geo.  Of the hash family, per-field expiry
  (`HEXPIRE`, `HPEXPIRE`, `HTTL`, `HPERSIST`, `HGETEX`, `HSETEX`, `HGETDEL`).
- **Transactions and scripting:** `MULTI`/`EXEC`/`DISCARD`/`WATCH`,
  `FUNCTION`, `EVAL_RO`/`EVALSHA_RO`, and every script other than the
  approved Symfony Lock scripts above.
- **Sharded pub/sub:** `SSUBSCRIBE`, `SUNSUBSCRIBE`, `SPUBLISH`.
- **Server:** `FLUSHDB`/`FLUSHALL` (their own error), `SAVE`/`BGSAVE`
  (the native `save` verb), `MONITOR`, `DEBUG`, `WAIT`, `ACL`,
  `CONFIG SET`, `CLIENT` beyond `LIST`/`SETNAME`/`GETNAME` (no
  `CLIENT KILL` or `CLIENT ID`),
  `CLUSTER COUNTKEYSINSLOT` and the cluster management commands,
  RESP3 (`HELLO 3`).
