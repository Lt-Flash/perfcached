# Perfcached for PHP

A PHP extension over [libperfd](../../lib/README.md): the native door, the
Noise handshake, encryption by default, and failover to the other members.
PHP 8.1 or later, NTS builds only (a libperfd handle belongs to one thread).

## Build

```sh
sudo apt-get install libperfd-dev php8.3-dev   # or: make libperfd.so in this tree
cd lang/php
phpize8.3
./configure --with-php-config=php-config8.3    # --with-perfcached=DIR for elsewhere
make
```

`DIR` is libperfd's prefix, or a perfcached source tree with `libperfd.so`
built (`--with-perfcached=../..` from here); `make phpext` at the top of the
tree does all of this against the tree's own library.

Load `modules/perfcached.so` with `extension=` in the SAPI's `php.ini`.

On Debian 13 with Sury's PHP 8.3, `lang/php/debian` builds the package
`php8.3-perfcached`, which depends on `libperfd0` and enables the extension for
every SAPI: `dpkg-buildpackage -us -uc -b` in `lang/php`, with `libperfd-dev` installed.

## Use

```php
$pc = new Perfcached('10.0.0.1', 6479, ['secrets' => ['the-client-secret']]);
$pc->set('sessions', 'k', $bytes, 300);   // collection, key, value, ttl seconds (0 = none)
$v = $pc->get('sessions', 'k');           // string, or null on a miss
```

`host` can be a list, on one port, of up to 16 names, and it should be one under
PHP-FPM: a worker recycled while its only seed is down cannot connect.

```php
$pc = new Perfcached(['10.0.0.1', '10.0.0.2', '10.0.0.3'], 6479, ['secrets' => ['the-client-secret']]);
```

The list is for dialling: a process starts at the seed its PID picks and takes the
next when one does not answer, and a wrong secret stops there.  A seed that does
not answer costs at most 500 ms while another may answer, and `connect_timeout_ms`
once none has.  Once connected the handle works from the member list it learned,
so the list need not name every node.

A value is bytes both ways.  A collection, key or field is a C string: a NUL
in one is a `ValueError`.  A record - key, value and a 28-byte header - is at
most 262,080 bytes, so key and value together at most 262,052; a larger value
is refused (`PerfcachedServerException`, "value too large").  A write that a
node must forward to the key's owner (a shard fleet, without `route_keys`)
carries at most 58,000 bytes of value.

A ttl is whole seconds, 0 (no expiry) to 2,147,483,647; any other is a
`ValueError`, except that `add` takes a negative ttl as "keep the key's expiry".

| Method | Answers |
|---|---|
| `set(col, key, value, ttl = 0)` | nothing |
| `get(col, key)` | the value, or null |
| `del(col, key)`, `exists(col, key)` | bool |
| `ttl(col, key)` | seconds left, -1 no expiry, -2 no such key |
| `expire(col, key, ttl)` | bool: the key existed |
| `add(col, key, by = 1, ttl = 0)`, `sub(col, key, by = 1)` | the new value |
| `mget(col, keys)` | key => value or null |
| `keys(col, match = null, limit = 0, &complete = null)` | a list of keys; `complete` false when it is only the answering node's share (a sharded collection on a fleet) |
| `hset`, `hsetnx(col, key, field, value)` | bool: the field is new, or was set |
| `hget(col, key, field)` | the value, or null |
| `hdel`, `hexists(col, key, field)` | bool |
| `hlen(col, key)` | the field count |
| `hincrby(col, key, field, by = 1)` | the new value |
| `hgetall(col, key)` | field => value |
| `jget(col, key, path = '$')` | the document or the fragment at `path` as JSON text, or null |
| `jset(col, key, path, json, ttl = 0)` | nothing; `json` is JSON text, refused when it is not |
| `jdel(col, key, path = null)` | bool: something was deleted (no path: the whole document) |
| `jincr(col, key, path, by = 1)` | the new number at `path` |
| `setNx(col, key, value, ttl = 0)` | bool: stored (the key was absent) |
| `setIfEq(col, key, value, expected, ttl = 0)` | bool: stored (the key held `expected`, byte for byte) |
| `delIfEq(col, key, expected)` | bool: deleted (the key held `expected`) |
| `publish(channel, payload)` | the receivers on the node reached |
| `subscribe(channels, callback, timeout = 0)`, `psubscribe(patterns, callback, timeout = 0)` | the messages handed to `callback`; see below |
| `members()`, `ownerOf(col, key)`, `activeNode()`, `failovers()`, `routing()`, `serverVersion()` | the fleet as this handle sees it |
| `maintain()`, `ping()`, `close()` | nothing |
| `stats(col = null)` | the daemon's stats document, as an array |

An `mget` with a key whose node did not answer in time throws
`PerfcachedRefusedException`, and never reads as a miss: ask again.

## Options

The third argument of the constructor:

| Option | Meaning |
|---|---|
| `secrets` | a string, or a list of at most 8 tried in order (rotation); none = plaintext, for a loopback door. A secret holding a NUL is a `ValueError`. A `plaintext = loopback` door takes no handshake: a secret fails there with `PerfcachedAuthException` |
| `encrypt` | default true: stay encrypted after the handshake.  false lets a node that allows it drop to plaintext |
| `unix` | a socket path, in place of host and port (a host must still be given; it is not dialled) |
| `persistent` | default true: keep the handle for this process's next request.  false: it closes with the object |
| `connect_timeout_ms`, `io_timeout_ms` | 5000 each: a dial (TCP and handshake), and each send or receive |
| `policy` | `Perfcached::POLICY_FAILOVER` (default, stays where it connected), `POLICY_ROUND_ROBIN`, `POLICY_LEAST_CONN`, `POLICY_WEIGHTED`: these spread at connect, and when a member comes up 1 in N handles moves to it |
| `spares` | standby connections: 0 = one per other member, at most 8; `Perfcached::SPARES_NONE` = a bare handle |
| `route_keys`, `route_hints` | send a request to the node that should hold its key |
| `refresh_ms`, `keepalive_s`, `idle_ping_ms`, `worker_balance`, `name` | as `perfd_opts` in `perfd.h` |

An unknown option is a `ValueError`.

## Under PHP-FPM

A handle is kept per worker and per (seeds, port, options), across requests, and
a `new Perfcached(...)` with the same arguments takes it up again.  At that point
it is looked at: a dead socket is dialled again before your first call, not found
by it.  A process that forks dials its own: two processes must never advance one
Noise channel.

Deriving a secret's key costs about 150 ms of CPU and a 64 MiB mapping, and a
worker recycled by `pm.max_requests` would pay it again at its first connect.
Have the master do it once, before it forks.  In FPM's `conf.d`, not the CLI's:

```ini
extension=perfcached.so
perfcached.prepare_secrets_file=/etc/perfcached/php-secrets
```

The file holds the secrets, one a line, readable by root alone: the master reads
it before it drops privileges, and warns when anyone but its owner may read it.
The application still passes `secrets` as usual.
`Perfcached::pool()` says what this process's pool has done.

## With Symfony

`symfony/` is a composer package, `perfcached/symfony-cache`: `PerfcachedAdapter`,
a cache pool on a collection, for `symfony/cache` 5.4, 6.4 and 7.

```php
$pc = new \Perfcached($host, 6479, ['secrets' => [$secret]]);
$cache = new \Perfcached\Symfony\PerfcachedAdapter($pc, 'collection', 'namespace', 3600);
$rates = $cache->get('rates.au', function (ItemInterface $item) { /* ... */ });
```

A value past a record is not stored and an error is logged, to the pool's logger
or else to PHP's error log: Symfony would only see a miss, and run the callback
every time.

`clear()` deletes the namespace's keys as the connected node lists them.  On a
fleet whose collection is sharded (not eager) that node holds only its share:
`clear()` deletes that share, logs a warning and returns false, never true while
other nodes still hold keys, up or down.  Extension 0.2.0 and later says so through
`keys()`'s `complete`; with an older extension `clear()` cannot tell and returns true.

`PerfcachedLockStore` is a `symfony/lock` store (5.4, 6.4 and 7) on a collection,
as `RedisStore` does it: a lock is a key holding a random token, taken with
`setNx` and a ttl, refreshed with `setIfEq` and released with `delIfEq` only
while the key still holds that token.  A ttl is whole seconds: a fraction is
rounded up and one second added, since the daemon's clock ticks in whole
seconds and a ttl of N lasts between N-1 and N of them - the key outlives the
lock, never the other way round.

```php
$locks = new \Symfony\Component\Lock\LockFactory(
    new \Perfcached\Symfony\PerfcachedLockStore($pc, 'collection', 'lock:'));
$lock = $locks->createLock('billing-run', 300);
if ($lock->acquire()) { /* ... */ $lock->release(); }
```

## Pub/sub

`publish` reaches subscribers on either door: a RESP `SUBSCRIBE` (phpredis,
predis, a Node Redis client) on any node gets it, and so does `subscribe` here.

`subscribe` and `psubscribe` are for a long-running CLI worker, as in phpredis:
they open a connection of their own (the handle's seeds and options, never the
pool's), subscribe, and call `callback($handle, $channel, $message)` (with
`$pattern` before `$channel` for `psubscribe`) for each message, until the
callback returns `false`, throws, or `timeout` seconds pass (0: no limit).  They
answer the number of messages handed over; the subscriptions end with the call.
A message reaches PHP as text: a NUL byte or a byte that is not UTF-8 in a
payload does not arrive as sent.

```php
$pc->subscribe(['dev_device_updates'], function ($pc, $channel, $message) {
    $event = json_decode($message, true);
    /* ... */
    return true;                   // false ends the loop
});
```

## Errors

A bad argument is a `ValueError`.  Every other failure throws a
`PerfcachedException`, a `RuntimeException`, with `perfd_error_kind` as its
code: `PerfcachedTransportException`,
`PerfcachedAuthException`, `PerfcachedRefusedException`,
`PerfcachedServerException` or `PerfcachedArgumentException`, or the base class
for the rest.

MIT licensed: see [LICENSE](LICENSE).
