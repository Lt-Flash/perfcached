# perfcli command reference

perfcli is the redis-cli analogue: one client, three ways to drive it - a
REPL with its own line editor, a one-shot `perfcli <command words...>`,
or a raw request typed as JSON via `-j` - riding libperfd over the
native binary protocol (the JSON is perfcli's own input syntax, encoded
to the binary request before anything is sent). This page is every
command perfcli understands as a word: its accepted arguments, what it
prints, and where the word-command form is thinner than the method
underneath it - read out of `cli/perfcli.c`'s argument builder and
`src/verbs.c`'s handlers, not from memory or GUIDE.md's shorter tour. A
method usually takes more parameters than perfcli wires up as words;
every table below says so where it matters, with the `-j` form that
reaches the rest.

Reply bodies shown here are the shape the daemon writes for that
request - read out of the handler's reply-writer calls, not
a captured terminal session - using `sessions` / `user:17` as the
running example, as the README does. The REPL's own `help` is the terse
form of the same table. For the Redis-compatible door (a *different*
listener, a *different* protocol) see [REDIS-COMMANDS.md](REDIS-COMMANDS.md);
nothing on this page applies there.

## Reaching it

    perfcli [opts] <command words...>      one-shot, exit code honest
    perfcli [opts] -j '<json request>'      raw request passthrough
    perfcli [opts]                          REPL on a tty, pipe otherwise

With no `-h`/`-p`/`-u` and no `-A`, perfcli tries to find a locally
deployed daemon before giving up: it reads `PERFCACHED_CONF`, then
`/etc/perfcached/perfcached.conf`, then `/opt/perfcached/etc/perfcached.conf`,
then `/etc/perfcached.conf`, and takes the `[listen] tcp` address (a wildcard bind reads as reachable
on `127.0.0.1`) and the `[secrets] client` value from whichever it finds
first. **Options go before the command word; anything after it is an
argument to that command** - `perfcli stats -a secret` sends `-a` and
`secret` to `stats`, not to perfcli, and the client says so rather than
silently connecting unauthenticated.

A `create`/`drop`/`resize`/`rename`/`map`/`unmap`/`restore` call additionally needs
**privilege**, raised on the connection with `-E <secret>` (the
`[secrets] enable` value - not the client secret `-a` takes) or `-e` to
be prompted; privilege dies with the connection, so a session that
redials after a dropped link (perfcli redials once, automatically, and
says so) re-raises it automatically only because `-E`/`-e` was given at
startup - a bare `enable` typed later reaches the same effect for the
rest of that session (see [Privilege](#privilege-enable--disable)
below).

`pretty on` / `pretty off` / bare `pretty` (or `-P` at startup)
re-indents every JSON reply for the rest of the session. `exit` or
`quit` leaves the REPL; `help` prints the terse built-in form of this
page; `Ctrl-D` also quits. History persists 0600 in `~/.perfcli_history`
on a tty, duplicates collapsed.

**Exit codes**: `0` the command answered without error, `1` the daemon
answered with an error or a command in a pipe/REPL failed, `2` a
usage or connection problem (bad flags, wrong secret, no route to the
daemon) - checked before anything was sent.

### Options

| flag | meaning |
|---|---|
| `-h host` | daemon address (default: the local daemon's config, else `127.0.0.1`); `-h` alone, or followed by another flag, is `--help` instead |
| `-p port` | daemon port (default `6479`) |
| `-u path` | unix socket instead of TCP |
| `-a secret` | client secret (`[secrets] client`); also `PERFCLI_AUTH` |
| `-A` | prompt for the client secret (no echo) |
| `-E secret` | raise privilege on this connection (`[secrets] enable`) |
| `-e` | prompt for the enable secret (no echo); also `PERFCLI_ENABLE` |
| `-q` | quiet: result to stdout only, no prompt or banner |
| `-P` | pretty-print every result |
| `-j json` | send one raw request, typed as JSON, and exit |
| `-V` | print `perfcli <version> (libperfd <version>)` and exit |
| `--help`, `-?` | usage |

## Connection and diagnostics

| command | reply | notes |
|---|---|---|
| `ping` | `{"pong":true}` | the word-command form never sends `echo` - `perfcli -j '{"method":"ping","params":{"echo":"hi"}}'` answers `{"pong":true,"echo":"hi"}` and is also the transport/codec diagnostic (works with no secret on a plaintext door, and is what perfcli itself sends once at startup to prove the session before printing the banner) |
| `help` | (local, prints the built-in command list) | never reaches the daemon |
| `pretty [on\|off]` | (local) | toggles re-indented JSON; bare `pretty` flips it |
| `exit`, `quit` | (local, REPL only) | closes the connection |

```
$ perfcli -p 6479 ping
{"pong":true}
$ perfcli -p 6479 -j '{"method":"ping","params":{"echo":"hi"}}'
{"pong":true,"echo":"hi"}
```

## Keys and values

`<col>` is a collection name, not a glob, for every command in this
section (a `get`/`set`/etc. against a name that does not exist is `no
such collection`, not auto-created - see [Collections](#collections)
for creating one). Keys and values are bytes on the wire - there is no
encoding step and no `enc` companion to look for: what perfcli sends
is the bytes it was given, and a reply's `value` is printed as a JSON
string, with bytes that are not UTF-8 shown escaped. Keys travel the
same way, so a binary key is as reachable from perfcli as it is from
libperfd.

| command | reply | notes |
|---|---|---|
| `get <col> <key>` | `{"found":true,"value":"…","ttl":N}` or `{"found":false}` | `ttl` is `-1` when the key never expires; a miss on this node may still be answered via cluster pull, invisibly to the caller |
| `set <col> <key> <value> [ttl]` | `{"stored":true}` | `{"stored":false}` means the write was PARKED on a cluster forward, not that it failed - retry; `ttl` omitted or `0` means no expiry |
| `del <col> <key>` | `{"deleted":true}` or `{"deleted":false}` | `false` for a key that did not exist |
| `exists <col> <key>` | `{"exists":true,"ver":N}` or `{"exists":false}` | `ver` is the record's version counter, not its value - lets a caller compare two copies without moving the bytes |
| `ttl <col> <key>` | `{"ttl":N}` | `-2` no such key, `-1` never expires, else seconds remaining |
| `expire <col> <key> <ttl>` | `{"updated":true}` or `{"updated":false}` | re-arms the TTL in place - no value rewrite; `false` = no such key |
| `add <col> <key> [by] [ttl]` | `{"value":N}` | `by` defaults to `1`; a missing key starts at `0`; `ttl` only applies on the key's first touch |
| `sub <col> <key> [by] [ttl]` | `{"value":N}` | exactly `add` with `by` negated |

```
$ perfcli -p 6479 set sessions user:17 "some value" 300
{"stored":true}
$ perfcli -p 6479 get sessions user:17
{"found":true,"value":"some value","ttl":300}
$ perfcli -p 6479 ttl sessions user:17
{"ttl":300}
$ perfcli -p 6479 exists sessions user:17
{"exists":true,"ver":1}
$ perfcli -p 6479 expire sessions user:17 60
{"updated":true}
$ perfcli -p 6479 add sessions hits
{"value":1}
$ perfcli -p 6479 add sessions hits 10
{"value":11}
$ perfcli -p 6479 del sessions user:17
{"deleted":true}
$ perfcli -p 6479 get sessions user:17
{"found":false}
```

A write that could not be applied because this node has failed its WAL
answers with an error rather than a `stored`/`updated` field at
all - see [Errors](#errors).

## Bulk keys

| command | reply | notes |
|---|---|---|
| `mget <col> <key> [key ...]` | `{"values":[{"found":true,"value":"…","ttl":N}, {"found":false}, …]}` | one element per key, same order, each shaped exactly like `get`'s reply |
| `mset <col> <key> <value> [<key> <value> ...]` | `{"stored":N,"dropped":N}` | pairs, not a shared value; every pair gets the SAME `ttl` if one is added via `-j` (the word-command form has no `ttl` slot at all - every `mset` key is stored with no expiry); refused whole on a **shard** collection ("use pipelined set") |
| `keys [col-glob] [pattern] [limit]` | see below | `col-glob` matches **collection names**, `pattern` matches **key names** inside them - two different arguments, two different things they glob; **and two different reply shapes depending on `col-glob`**, below |
| `scan <col> [cursor] [match] [count]` | `{"items":[{"k":"…","ttl":N}, …],"cursor":N,"more":bool}` | cursored, Redis-`SCAN`-style; **the word-command form never requests values** - every item is key+ttl only, even though the method also accepts `"values":true` |

```
$ perfcli -p 6479 mset sessions a 1 b 2 c 3
{"stored":3,"dropped":0}
$ perfcli -p 6479 mget sessions a b nosuchkey
{"values":[{"found":true,"value":"1","ttl":-1},{"found":true,"value":"2","ttl":-1},{"found":false}]}
$ perfcli -p 6479 scan sessions
{"items":[{"k":"a","ttl":-1},{"k":"b","ttl":-1},{"k":"c","ttl":-1}],"cursor":0,"more":false}
$ perfcli -p 6479 -j '{"method":"scan","params":{"col":"sessions","values":true}}'
{"items":[{"k":"a","v":"1","ttl":-1},{"k":"b","v":"2","ttl":-1},{"k":"c","v":"3","ttl":-1}],"cursor":0,"more":false}
```

**`keys`'s two shapes.** A *literal* collection name (`keys sessions`)
answers the single-collection form:

    {"collection":"sessions","complete":true,"scope":"node","keys":["a","b","c"],"truncated":false}

`complete:false` means this reply is only this node's share of a
placement-spread collection, not the whole keyspace. Anything else -
bare `keys`, `keys *`, or a `col-glob` containing `*`/`?`/`[` - answers
the grouped, every-matching-collection form instead:

    $ perfcli -p 6479 keys
    {"collections":{"sessions":["a","b","c"],"counters":["hits"]},"truncated":false,"truncated_collections":[],"matched":2}

Here `[pattern]` is a key glob applied inside each matched collection,
and `[limit]` (default 100) is **per collection**, not shared across
the reply - a collection that hits its cap is named in
`truncated_collections` rather than silently dropping keys from the
count. A binary key that is not clean UTF-8 is shown escaped in either
shape's key array.

## JSON documents

Paths are `$`, `.name` / bare `name` (equivalent), and `[index]`; a
document is one opaque JSON value per key, under the same value-size
limit as a plain string.

| command | reply | notes |
|---|---|---|
| `jget <col> <key> [path]` | `{"found":true,"value":<fragment>}` or `{"found":false}` | no path = the whole document |
| `jset <col> <key> <path> <raw-json> [ttl]` | `{"set":true}` | `<raw-json>` is **unescaped, literal JSON** - quote it for the shell, not for JSON; `-j` also offers `nx`/`xx`/`mkpath` that the word-command form has no slot for |
| `jdel <col> <key> [path]` | `{"deleted":true}` or `{"deleted":false}` | no path = deletes the whole key |
| `jincr <col> <key> <path> [by]` | `{"value":N}` | `by` defaults to `1`; errors (not an error reply, an in-band one - see notes below) if the target is not numeric |
| `jarrappend <col> <key> <path> <raw-json>` | `{"count":N}` | `<raw-json>` may itself be an array literal to append several elements in one call; `count` is the array's new length |

```
$ perfcli -p 6479 jset sessions user:17 '$' \
    '{"name":"ann","roles":["admin","ops"],"quota":{"used":3,"max":10}}'
{"set":true}
$ perfcli -p 6479 -P jget sessions user:17 '$.quota'
{
  "found": true,
  "value": {
    "used": 3,
    "max": 10
  }
}
$ perfcli -p 6479 jincr sessions user:17 quota.used
{"value":4}
$ perfcli -p 6479 jarrappend sessions user:17 roles '"support"'
{"count":3}
$ perfcli -p 6479 jdel sessions user:17 quota
{"deleted":true}
```

On a **proxy** collection where this node is not the holder, or a
**shard** collection where it is not the owner, a JSON op is forwarded
to the node that is - and if that forward could not be parked, the
reply comes back with the verb's OWN false/absent shape
(`{"found":false}` for `jget`, `{"set":false}` for `jset`,
`{"deleted":false}` for `jdel`, `{"error":"cluster busy"}` embedded in
an otherwise-normal reply for `jincr`/`jarrappend`) rather than an
error reply - a client checking only for the top-level `error` key
will miss this and should treat these specific false/absent shapes on a
clustered collection as "retry", not "confirmed no".

## Collections

Administrative: **`create`, `drop`, `resize` and `rename` all need
`[daemon] allow_create = yes` on the node AND a privileged connection**
(`-E`/`-e` - see [Privilege](#privilege-enable--disable)). A collection
created anywhere reaches every cluster member regardless of that
member's own `allow_create` setting - the gate controls where DDL may
*originate*, not whether it is *honoured*.

| command | reply | notes |
|---|---|---|
| `collections` | `{"collections":[{…name, mode, entries, buckets…}]}` | one element per collection - `mode` is one of `store`, `proxy`, `shard`, `eager`, `spread`; a worked example is below |
| `create <col> [buckets_log2]` | `{"created":true,"col":"…","buckets":N}` | `buckets_log2` defaults to `12` (4,096 buckets); valid range `4..24` |
| `drop <col> [force]` | `{"dropped":true,"col":"…","records":N}` | a non-empty collection without `force` (`force`, `yes` or `1`) answers `{"dropped":false,"entries":N,"why":"…"}` instead of an error |
| `resize <col> <buckets_log2>` | `{"resizing":true,"col":"…","buckets":N}` | starts a background migration and returns immediately; `stats` shows progress; refuses a size the collection is already at, or smaller than what the splitter would grow it straight back to |
| `rename <col> <to>` | `{"renamed":true,"col":"…","to":"…"}` | one pointer swap - a concurrent reader sees the old name or the new one, never a half-written state |

```
$ perfcli -E 's3cret' -p 6479 create staging 10
{"created":true,"col":"staging","buckets":1024}
$ perfcli -E 's3cret' -p 6479 collections
{"collections":[{"name":"sessions","mode":"store","entries":3,"buckets":4096},{"name":"staging","mode":"store","entries":0,"buckets":1024}]}
$ perfcli -E 's3cret' -p 6479 resize staging 12
{"resizing":true,"col":"staging","buckets":4096}
$ perfcli -E 's3cret' -p 6479 rename staging archive
{"renamed":true,"col":"staging","to":"archive"}
$ perfcli -E 's3cret' -p 6479 drop archive
{"dropped":false,"entries":0,"why":"the collection is not empty - pass force to drop it with its records"}
$ perfcli -E 's3cret' -p 6479 drop archive force
{"dropped":true,"col":"archive","records":0}
```

Without `allow_create` on the node: `creating and dropping collections
is not enabled here - set [daemon] allow_create = yes on this node`.
Without privilege: `this operation needs a privileged connection - call
enable with the [secrets] enable value first` (or, if the node has no
`[secrets] enable` configured at all, `no [secrets] enable is
configured on this node, so privilege cannot be raised and this
operation is unreachable`).


### The RESP door's DB-index map at runtime (0.7.1)

`resp_collections` (`[listen]`) maps the RESP door's DB indexes onto
collections when the node starts.  `map`, `unmap` and `maps` change and
read that map **on the running fleet**:

| command | reply | notes |
|---|---|---|
| `map <index> <col>` | `{"mapped":true,"index":N,"col":"…","was":"…"}` | DB `index` (0..63) answers as collection `col` on every member; `was` is what it reached before (`""` when nothing) |
| `unmap <index>` | `{"unmapped":true,"index":N,"col":"…","was":"…"}` | the index answers as `resp_collections` says again; `col` is what that is |
| `maps` | `{"config":"…","overrides":N,"maps":[{"index":N,"col":"…","source":"runtime\|config\|open","exists":true}]}` | every index the door answers and where it came from; `open` is a node with no `resp_collections`, where each index reaches the collection of its own name (only those that exist are listed) |

- `map` and `unmap` need `[daemon] allow_create = yes` and privilege, as
  `create`; `maps` needs neither.  `index` is a whole decimal number, so
  `map cache 0` (the words swapped) is refused by perfcli before it
  is sent.
- **Every connection sees the change on its next command** - no reconnect,
  no `SELECT`.  A `MULTI` block or a pipeline that straddles the change
  runs its earlier commands against the old map and its later ones against
  the new.
- **Fleet-wide and persistent.**  The change reaches every member at once,
  is written to `<state_dir>/respmap` on each, and survives a restart; a
  member that was down when it was made takes it from its peers when it
  rejoins (the newer change wins).  It is refused while any live member
  runs a build before 0.7.1 (`every member must run 0.7.1 or later to map
  a RESP index at runtime - node N is older`).
- **A runtime map outranks `resp_collections`** for its index, across
  restarts too, until it is unmapped.  `perfcached -C` says when one is
  held, and a `reload` with `resp_collections` changed is refused with a
  pointer to `perfcli map`.
- One collection may answer on several indexes (`0` and `1` both
  `cache`); `INFO keyspace` reports it under the runtime index.  A
  collection the map stops naming is no longer reachable over RESP.
- The map names a collection, not a table: renaming or dropping a mapped
  collection leaves the index pointing at the old name (`maps` shows
  `"exists": false`) until it is mapped again.

```
$ perfcli -E 's3cret' -p 6479 maps
{"config":"0, 1:cache, 2:sessions","overrides":0,"maps":[{"index":0,"col":"0","source":"config","exists":true},{"index":1,"col":"cache","source":"config","exists":true},{"index":2,"col":"sessions","source":"config","exists":true}]}
$ perfcli -E 's3cret' -p 6479 map 0 cache
{"mapped":true,"index":0,"col":"cache","was":"0"}
$ perfcli -E 's3cret' -p 6479 unmap 0
{"unmapped":true,"index":0,"col":"0","was":"cache"}
```

## Privilege (`enable` / `disable`)

Not a perfcli word command - `build()` has no `enable` branch, so typing
`enable ...` in the REPL is "unknown command". Reach it three ways:
`-E <secret>` or `-e` at startup (the normal path - see
[Reaching it](#reaching-it)), or, mid-session, the raw form:

```
$ perfcli -p 6479
perfcached 127.0.0.1:6479 - 'help' for commands, ^D quits
perfcached> {"method":"enable","params":{"secret":"s3cret"}}
{"privileged":true}
perfcached> {"method":"disable"}
{"privileged":false}
```

`enable`/`disable` are available on the native door only, by design -
refused on the RESP door, since the RESP door may run plaintext
off-box under an allow-list and this secret grants keyspace deletion.
Privilege lives on the connection and is lost on redial; perfcli itself
re-raises it automatically after an automatic reconnect only when `-E`
was given at startup.

## Maintenance and persistence

| command | reply | notes |
|---|---|---|
| `stats [col]` | a large document: node version/rev/state, per-door and per-collection counters, memory/arena, WAL, cluster | no argument = the whole node; `stats sessions` narrows it to one collection's numbers |
| `name <name>` | `{"named":"…"}` | names this connection (CLIENT SETNAME's native form, 0.7.0): shown as the row's `name` in `clients` and carried by the node's enable, sweep and reset log lines (`name@ip:port`).  perfcli names its connection `perfcli@<host>` unless `-N <name>`; libperfd sends `opts.name` after every dial.  Printable ASCII without spaces or quotes |
| `clients` (`-j '{"method":"clients","params":{"cap":N}}'`; no word form yet) | `{"clients":[...]}` | 0.6.6: this node's connections as `/clients` renders them - door, protocol, wire, pending bytes, subscriptions - on the native door, for a dashboard that reads it; at most `cap` rows (200, up to 4096) |
| `members` | `{"members":[{…per-member fields, below…}]}` | the fleet as a client sees it - enough to pre-warm connections and weight them by load |
| `sketch` (`-j '{"method":"sketch","params":{"col":"sessions"}}'`; no word form) | `{"p":12,"hash":"fnv1a64+fmix64","walked_at":1792372527,"registers":"<base64, 4096 bytes>"}` | 0.7.1 (DESIGN 12nx): the HyperLogLog sketch of the keys this node held at its last held walk of the collection - one byte a register, the same hash on every node, so sketches from several members merge by the register-wise maximum into the sketch of their union (what `perfcached-dashboard` does for `store` mode's distinct keys).  Unprivileged.  An error until the collection's next walk after a start, a create or a stats reset (`reset stats`) |
| `save` | `{"started":true}` or `{"started":false,"reason":"already running"}` | starts an RDB snapshot in the background; the error `persistence is not configured` if no `[rdb]`/`[wal]` section is set up |
| `sync [timeout_ms]` | `{"synced":true,"seq":N,"dropped":N}` | a WAL barrier - blocks until everything appended so far has reached the platter; **the word-command form never sends `timeout_ms`** - use `-j` to bound the wait; `dropped` is the fleet-wide count of records the WAL ring discarded under backpressure, not zero just because `synced` is true |
| `load` | `{"loaded":N,"skipped_existing":N,"skipped_expired":N}` | imports the on-disk snapshot **additively** - an existing key always wins, an already-expired record is skipped and counted |
| `probe [secs]` | `{…,"recommend":{…},"observed":{…}}` | re-measures WAL storage IOPS/fsync latency on demand; **blocks the request for its duration and perturbs live fsync latency while it runs** - `secs` capped at `30`; full shape below |
| `updrain <col> [stop]` | `{"col":"sessions","drain":"started"}` (or `"stopping"`) | **needs privilege** (`-E`): the read-through drain sweep - reads every key the collection's upstream holds under its `upstream_prefixes` and takes over (writes here, replicated) each one the fleet does not hold, has not written and has not deleted; at most `upstream_drain_rate` keys and (0.6.5) `upstream_drain_mb_rate` MB of values a second; refused with `this collection has no upstream`, `a drain sweep is already running`, or (`stop`) `no drain sweep is running`; progress is `upstream.drain` in `stats` (`state` running/done/stopped/failed, `read`, `taken_over`, `fleet_had`, `gone`, `wrongtype`, `too_big`, `errors`); run it on ONE node |
| `upfill <col> [stop\|check]` | `{"col":"sessions","upfill":"started"}` (or `"checking"`, `"stopping"`) | **needs privilege** (`-E`), 0.6.4: on a collection with `upstream_writes = through`, writes each key the fleet owned before the switch (written here under `allow`, which Redis never had) to the upstream's master with `NX` - `SET ... NX [PX]` for a string or a counter, `JSON.SET $ ... NX` and then a `PEXPIRE` for a document - through the writer, so in order with the clients' writes; where Redis has the key its value wins: it is read back and replaces the fleet's record on every member.  0.6.5: the sweep first compares the two and counts Redis's as `redis_same`, `redis_retimed` (the same value with another expiry) or `redis_differed`, naming each key that differed in the log (the first 100 of a sweep, never a value).  `check` (0.6.5) reads each key from the master instead and makes the same comparison, writing and applying nothing; `missing` counts the keys a run would write.  Hashes, rate-limit and lock records are skipped.  At most `upstream_drain_rate` keys and (0.6.5) `upstream_drain_mb_rate` MB of values a second, and half of `upstream_write_queue` at once; refused with `this collection has no upstream`, `... needs upstream_writes = through`, `an upfill sweep is already running`, (`stop`) `no upfill sweep is running`, or (`check`) `upfill check reads the upstream's master - the writer has none now ...`; progress is `upstream.write_through.upfill` in `stats` (`state`, `mode`, `found`, `written`, `redis_had`, `redis_same`, `redis_retimed`, `redis_differed`, `missing`, `compared_text`, `compared_walked`, `uncompared`, `skipped`, `gone`, `errors`; `redis_had` = same + retimed + differed + uncompared = compared_text + compared_walked + uncompared), and `upstream.write_through.fleet_owned` counts the keys still owned (a minute apart); run it on ONE node |
| `upgrade-stage <tarball> <version>` | `{"sha256":"…","signer":"the release key 5646e8e4 (built in)"}` | **needs privilege** (`-E`), 0.6.5: sends the tarball and `<tarball>.sig` beside it (64 raw bytes, or 128 hex characters) to this node, 192 KB at a time, through the `upgrade_stage` method (`begin`, `chunk`s, `end`).  The node verifies the WHOLE file when the last chunk is in - the sha256 perfcli computed, then the signature against the release key built into it and `[upgrade] trusted_keys` - and only then keeps it in `<state_dir>/upgrade/staged/`; refused with the reason (`the signature does not verify - refused`, `sha256 mismatch - refused`, `unsigned - refused ([upgrade] allow_unsigned = no)`, `another upload is in progress`, `larger than [upgrade] stage_mb`), and a refusal keeps nothing.  At most two builds stay staged |
| `upgrade plan <tarball> version=<v> [switch_cmd=<program>] [journal=<file>] [ready_s=<n>] [gate_s=<n>]` | text: the plan and the next command | **needs privilege** (`-E`), 0.7.0 (UP7a, DESIGN 12ns): writes the plan line (artifact, sha256, version, seed, gates) to the journal; 0.7.1 (UP12b): reads the tarball's `share/perfcached/RELEASE` first and refuses a tarball of another version than `version=`, or one whose wire epochs exclude the fleet's committed epoch (exit 1, nothing journalled), names a possible commit when the candidate speaks a newer epoch, and says "not checked" for a tarball without the file (before 0.7.1) |
| `upgrade start\|resume\|status\|abort\|rollback [journal=<file>]` | the journal lines as they are written (`start`/`resume`/`rollback`), a table (`status`) | the roll driver: stage on the seed and wait for every member; then one node at a time, members first, the master last, each gated (restart-safe before; a new pid on the version asked, ready with peers up, the peers' records kept after); the first failure reverts that node and stops with the rollback commands printed; exit 0 done, 1 stopped, 2 usage |
| `upgrade-revert [force]` | `{"request":"<id>","to":"/opt/perfcached-0.6.6","helper":"enabled"}` | **needs privilege** (`-E`), 0.7.0 (UP7b): asks the helper to switch this node back to the prefix it kept before its last switch (`upgrade_status.previous.from`); refused on a package or local install, with no switch on record, while a request waits, or when this node does not run the build the helper switched to (`force` overrides that and the helper check) |
| `fleet-status` | a table, not JSON | this node and every member the connection learned, each dialled: node id, `addr:port`, version, the state this handle sees (`this`, `active`, `standby`, `idle`, `down`), the install mode (`package`, `prefix`, `local` - PD-9) and the versions staged there, newest first.  Exit 1 when a member could not be dialled.  UP8's `upgrade status`, first piece (0.7.0) |
| `upgrade_status` | `{"staged":[{"sha256":"…","version":"0.6.5","size":2994718,"signer":"…","staged":N}],"upload":null,"live_prefix":"/opt/perfcached-0.7.0","previous":{"from":"…","to":"…","at":N},"switching":null,…}` | the builds staged on this node, newest first, the upload in progress (`id`, `got`, `size`, `idle_s`) or `null`, `staged_total` and `refused_total` since start, `stage_mb`, `allow_unsigned` |
| `reload` | `{"applied":true,"message":"reloaded: resp_allow, the HTTP token"}` | **needs privilege** (`-E`), 0.7.1 (DESIGN 12nw): reads this node's config file again and applies what a running daemon can take (`[listen] resp_allow`, `http_allow`; `[secrets] http`, `resp`, `enable`, `client`; `[daemon] log_level`, `slowlog_usec`); refused whole, `applied` false and the message naming every other key that changed; answered when done.  `systemctl reload` and SIGHUP run the same reload |
| `mailtest` | `{"sent":true,"reply":"250 2.0.0 queued"}` | **needs privilege** (`-E`): sends one test mail through this node's `[mail]` and answers the SMTP server's reply - `sent` `false` with what the server or the connection said (a wrong password's `535`, no STARTTLS on offer, a timeout) is a result, not an error; refused with the reason when this build has no SMTP client or the node has no `[mail]`; waits for the answer, up to `[mail] timeout_s` + 5 s |
| `reset-stats` (or `reset_stats`) | `{"reset":true,"at":N}` | zeroes the running totals on **this node only** (the native verb also takes `{"fleet": true}`, 0.7.0 - this node and every live peer as of one moment, what the dashboard's button sends; the daemon's HTTP door takes no POST any more); live gauges (arena size, entry counts) are untouched; the RESP door's `CONFIG RESETSTAT` and the status page's reset button do the same thing |

Each element of `members`: `addr`, `port` (client port), `node` (id),
`self`/`master`/`backup` (bool), `role` (`master`/`backup`/`member`),
`state` (the node's own readiness), `mem_tier`, `free_mb`/`total_mb`,
`http` (status-page port), `uptime_s`, `entries` (this node's own view -
what the sender election decides from), `start` (`cold`/`warm`), and,
gossiped from a build new enough to send it, a `clients` object.

`probe`'s full reply nests `sync_bs`/`qd`/`seq_bs`/`seq_mb_s`/
`fsync_p50_us`/`fsync_p99_us`/`sync_iops`/`probed_secs` at the top,
a `recommend` object (`fsync`, `max_durable_wps`,
`max_durable_wps_is_upper_bound`, `segment_mb`, and, once there is
observed traffic to base it on, `wal_total_mb`/`segments`/`basis`), and
an `observed` object (`fsync_n`, `fsync_avg_us`, `fsync_max_us`,
`probe_underestimated`) - see the storage design notes this module was
built against for what each figure is for.

```
$ perfcli -p 6479 stats sessions
{"version":"…","rev":"…","state":"ready", … per-collection and cluster figures … }
$ perfcli -p 6479 members
{"members":[{"addr":"192.0.2.10","port":6479,"node":1,"self":true,"master":true,"backup":false,"role":"master","state":"ready","mem_tier":"core","free_mb":15000,"total_mb":16000,"http":8479,"uptime_s":3600,"entries":3,"start":"warm"}]}
$ perfcli -p 6479 save
{"started":true}
$ perfcli -p 6479 sync
{"synced":true,"seq":42,"dropped":0}
$ perfcli -p 6479 -j '{"method":"sync","params":{"timeout_ms":5000}}'
{"synced":true,"seq":42,"dropped":0}
$ perfcli -p 6479 load
{"loaded":3,"skipped_existing":0,"skipped_expired":0}
$ perfcli -p 6479 probe 5
{"cached":false,"sync_bs":4096,"qd":1, … ,"recommend":{"fsync":"everysec","max_durable_wps":…}, …}
$ perfcli -E 's3cret' -p 6479 reset-stats
{"reset":true,"at":1234567890}
$ perfcli -E 's3cret' -p 6479 updrain sessions
{"col":"sessions","drain":"started"}
```

## Not perfcli words: `dump` / `restore`, and pub/sub

Two families exist as native methods but have **no perfcli word** -
`build()` does not dispatch them, so they are reachable only via `-j`
(or not intended for interactive use at all):

- **`dump`/`restore`** are `perfdump`/`perfload`'s own wire protocol - a
  chunked, cursor-driven bulk transfer shaped for a parallel dumper, not
  a hand-typed command. Use [`perfdump`](GUIDE.md#perfdump) and
  [`perfload`](GUIDE.md#perfload) instead of `-j`-ing these directly;
  the format is documented in
  [doc/perfdump-format.md](doc/perfdump-format.md).
- **`subscribe` / `psubscribe` / `unsubscribe` / `punsubscribe` /
  `publish`** exist on the native door (see the daemon's pub/sub
  design) but perfcli's REPL has no subscribed mode to receive
  deliveries in - a `subscribe` from perfcli registers the interest and
  the connection then sits there; messages arrive as unsolicited
  NOTIFY frames perfcli was not written to print specially. Reachable
  for a quick check:

  ```
  $ perfcli -p 6479 -j '{"method":"subscribe","params":{"channel":"news"}}'
  {"subscribed":1}
  $ perfcli -p 6479 -j '{"method":"publish","params":{"channel":"news","payload":"hello"}}'
  {"receivers":1}
  ```

  A real subscriber should use libperfd (which has a notify hook for
  exactly this) or the RESP door's `SUBSCRIBE`, documented in
  [REDIS-COMMANDS.md](REDIS-COMMANDS.md#pubsub).

## Errors

perfcli prints an error reply's message text to **stderr** (the daemon
answers a failed method with an error frame carrying the message and
nothing else - there are no numeric error codes) and exits `1`; a few
conditions are
"soft" - a normal-shaped, exit-`0` reply whose body signals the
non-outcome instead (see the false/absent shapes called out per command
above).

| message | when |
|---|---|
| `node is not READY (recovering) - retry, or use another member` | any data command reaches a node still loading its snapshot/WAL |
| `node is FAILED (its WAL is losing acknowledged writes) - reads still served here, send writes to another member` | a write reaches a node that has stopped accepting them after a WAL overrun |
| `creating and dropping collections is not enabled here - set [daemon] allow_create = yes on this node` | `create`/`drop`/`resize`/`rename` on a node without `[daemon] allow_create` |
| `this operation needs a privileged connection - call enable with the [secrets] enable value first` | the same commands, `map`/`unmap`, or `restore`, on an unprivileged connection |
| `changing the RESP DB-index map is not enabled here - set [daemon] allow_create = yes on this node (the gate of every collection change)` | `map`/`unmap` on a node without `[daemon] allow_create` |
| `every member must run 0.7.1 or later to map a RESP index at runtime - node N is older` | `map`/`unmap` while a live member predates them |
| `DB index N is not mapped at runtime - it answers as resp_collections maps it` | `unmap` of an index no runtime map holds |
| `no [secrets] enable is configured on this node, so privilege cannot be raised and this operation is unreachable` | `-E`/`-e`/`enable` on a node with no `[secrets] enable` set at all |
| `missing col` / `no such collection` | a bad or absent collection name |
| `method not found` | a typo'd method via `-j` (perfcli's own word dispatch instead says `unknown command 'x' (try help)`, to stderr, without a round trip) |
| *(connection refused / timed out / no route)* | no daemon at the given address - perfcli additionally suggests `-h`/`-u` when no target was given and no config was readable |
| *(handshake failed)* | wrong `-a` secret against an encrypted listener, or a secret given against a plaintext one - perfcli tells the two apart in its own message |

## See also

- [GUIDE.md](GUIDE.md#tools) - perfcli in context next to `perfdump`/`perfload`/libperfd
- [REDIS-COMMANDS.md](REDIS-COMMANDS.md) - the RESP-compatible door, a different listener and protocol
- [doc/perfdump-format.md](doc/perfdump-format.md) - the bulk-transfer file format `dump`/`restore` serve
- `cli/perfcli.c`, `src/verbs.c` - source of everything on this page
