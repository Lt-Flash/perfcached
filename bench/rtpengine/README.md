# rtpengine ↔ perfcached — the RESP surface, settled from the wire

Two small rigs that settle the RESP command surface from the wire
instead of from guesswork, and then prove rtpengine actually runs on
perfcached. Both are written to be re-runnable against a DIFFERENT
rtpengine version — which is exactly what you should do before adding
any RESP command for it.

Requires `rtpengine` and `redis-cli`/`redis-server` on the box.

## 1. `capture-redis.sh` — what does rtpengine actually issue?

Starts an isolated Redis (port 6399, own dir) and an isolated
rtpengine (`table = -1`, ng on 22223, media 41000–41020) pointed at
it, records `redis-cli MONITOR`, and drives a full call lifecycle
(offer → answer → re-offer → delete) through the ng protocol.

**A live rtpengine and a live Redis on the same host are never
touched** — different table, different ports, different database.

Result against **rtpengine 10.5.6.4** (2026-08-27), the complete
vocabulary observed:

| command | when | perfcached |
|---------|------|-----------|
| `PING` | every connection | ✅ |
| `SELECT n` | after connect | ✅ (db index = the collection NAMED n) |
| `INFO` | startup, to learn the replication role | ✅ (must carry `role:master` — see below) |
| `TYPE calls` | startup, legacy set-format probe | ✅ (`none` = new format) |
| `KEYS *` | startup restore | ✅ |
| `SET <call-id> <json>` | offer / answer / re-offer | ✅ |
| `EXPIRE <call-id> 86400` | after every write | ✅ |

**No hash commands.** Each call is one opaque JSON blob under its
call-id, so the HSET/HGETALL mapping question the task was filed with
is moot for this version. Also worth noting for the project's founding
motivation: rtpengine fires a blocking `KEYS *` at the shared Redis on
every startup.

## 2. `interop-perfcached.sh` — does rtpengine run on perfcached?

Same isolated rtpengine, backend swapped to a perfcached listening on
`127.0.0.1:6499` with collections named `0` and `1` (rtpengine is
configured for db 1). Legs: start, a call lifecycle, then an rtpengine
restart to force the `KEYS *` + `GET` restore path.

Result (2026-08-27): all three pass — rtpengine connects "in master
mode", the call lands in perfcached with the TTL rtpengine asked for,
and after a restart rtpengine restores the call from perfcached
(`list` → `calls: [s29-capture-call-1]`).

### The finding that made it work

rtpengine calls `INFO` **only** to decide whether the server is a
master, and without a role line it refuses to start:

    Asking Redis whether it's master or slave...
    Failed to connect to Redis 127.0.0.1:6499
    Fatal error: Cannot start up without running Redis ... write database!

perfcached's `INFO` therefore reports a `# Replication` section with
`role:master` and no slaves. `test/resptest.sh` pins it.

### Reaching it over a network

A Redis client cannot speak the Noise channel, so RESP only works on a
plaintext listener, and `plaintext` is `never | loopback`. Run the
client on the same host as a perfcached node (it clusters onward over
the encrypted peer plane), or tunnel — the interop run used
`ssh -L 6499:127.0.0.1:6499 <daemon-host>` because rtpengine and the
daemon lived on different machines. Serving RESP across a LAN would
need a new listener policy; that decision has not been taken.
