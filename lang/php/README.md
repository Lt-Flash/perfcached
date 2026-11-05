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
in one is a `ValueError`.  The largest record, key and value together, is
262,080 bytes; a larger value is refused.

| Method | Answers |
|---|---|
| `set(col, key, value, ttl = 0)` | nothing |
| `get(col, key)` | the value, or null |
| `del(col, key)`, `exists(col, key)` | bool |
| `ttl(col, key)` | seconds left, -1 no expiry, -2 no such key |
| `expire(col, key, ttl)` | bool: the key existed |
| `add(col, key, by = 1, ttl = 0)`, `sub(col, key, by = 1)` | the new value |
| `mget(col, keys)` | key => value or null |
| `keys(col, match = null, limit = 0)` | a list of keys |
| `hset`, `hsetnx(col, key, field, value)` | bool: the field is new, or was set |
| `hget(col, key, field)` | the value, or null |
| `hdel`, `hexists(col, key, field)` | bool |
| `hlen(col, key)` | the field count |
| `hincrby(col, key, field, by = 1)` | the new value |
| `hgetall(col, key)` | field => value |
| `members()`, `ownerOf(col, key)`, `activeNode()`, `failovers()`, `routing()`, `serverVersion()`, `maintain()`, `ping()`, `close()` | the fleet as this handle sees it |
| `stats(col = null)` | the daemon's stats document, as an array |

An `mget` with a key whose node did not answer in time throws, and never reads
as a miss: ask again.

## Options

The third argument of the constructor:

| Option | Meaning |
|---|---|
| `secrets` | a string, or a list tried in order (rotation); none = plaintext, for a loopback door. A `plaintext = loopback` door takes no handshake: a secret fails there with `PerfcachedAuthException` |
| `encrypt` | default true: stay encrypted after the handshake.  false lets a node that allows it drop to plaintext |
| `unix` | a socket path, in place of host and port |
| `persistent` | default true: keep the handle for this process's next request.  false: it closes with the object |
| `connect_timeout_ms`, `io_timeout_ms` | 5000 each: a dial (TCP and handshake), and each send or receive |
| `policy` | `Perfcached::POLICY_FAILOVER` (default, stays where it connected), `POLICY_ROUND_ROBIN`, `POLICY_LEAST_CONN`, `POLICY_WEIGHTED`: these spread at connect, and when a member comes up 1 in N handles moves to it |
| `spares` | standby connections: 0 = one per other member; `Perfcached::SPARES_NONE` = a bare handle |
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
it before it drops privileges.  The application still passes `secrets` as usual.
`Perfcached::pool()` says what this process's pool has done.

## With Symfony

`symfony/` is a composer package, `perfcached/symfony-cache`: `PerfcachedAdapter`,
a cache pool on a collection, for `symfony/cache` 5.4 and later.

```php
$pc = new \Perfcached($host, 6479, ['secrets' => [$secret]]);
$cache = new \Perfcached\Symfony\PerfcachedAdapter($pc, 'collection', 'namespace', 3600);
$rates = $cache->get('rates.au', function (ItemInterface $item) { /* ... */ });
```

A value past a record is not stored and an error is logged, to the pool's logger
or else to PHP's error log: Symfony would only see a miss, and run the callback
every time.

## Errors

Every failure throws a `PerfcachedException`, a `RuntimeException`, with
`perfd_error_kind` as its code: `PerfcachedTransportException`,
`PerfcachedAuthException`, `PerfcachedRefusedException`,
`PerfcachedServerException` or `PerfcachedArgumentException`, or the base class
for the rest.

MIT licensed: see [LICENSE](LICENSE).
