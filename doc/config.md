# Configuration

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

Every option lives, annotated, in
[contrib/perfcached.conf.example](../contrib/perfcached.conf.example);
the single node below is a complete working config, the snippets after
it are what each feature adds to one, and
[contrib/CONFIGURATIONS.md](../contrib/CONFIGURATIONS.md) has eleven whole
files - single nodes and fleets in every mode - each validated with
`perfcached -C`.

## A single node

    # /etc/perfcached/perfcached.conf  (chmod 640)
    [daemon]
    workers = 4

**How many workers.** Set it to the core count. Measured on a 16-core
host: throughput scales close to linearly to 8 workers (6.2x a single
worker), then flattens as the daemon reaches ~14 of 16 cores.
Oversubscribing is harmless rather than helpful - 32 and 64 workers on
16 cores buy 2-6% over 16 and cost ~0.3 MB of RSS each, with no
scheduler thrash. No penalty for guessing high; a real one for low.

    [memory]
    arena_mb = 1024

    [secrets]
    client = pick-a-client-password
    cluster = pick-a-DIFFERENT-cluster-password

    [listen]
    tcp = 0.0.0.0:6479

    [collection sessions]
    buckets_log2 = 18

Validate, then run (`-D` = foreground; omit it to daemonize, or use
the systemd unit):

    perfcached -f /etc/perfcached/perfcached.conf -C   # validate + report
    perfcached -f /etc/perfcached/perfcached.conf -D
    perfcli -p 6479 -a 'pick-a-client-password' ping

For production, install the unit and start it the systemd way:

    cp contrib/perfcached.service /etc/systemd/system/
    systemctl daemon-reload && systemctl enable --now perfcached

The two secrets MUST differ - clients hold the client secret, only
daemons hold the cluster secret, and the daemon refuses to start when
they are equal.  Add a second `client = ...` line to rotate client
passwords with zero downtime (new connections try each in turn).


## Other settings worth knowing

- `max_clients` under `[daemon]` caps client connections across the
  data doors.  Unset, it is the smaller of what the descriptor limit
  allows (minus what the process needs open) and what the memory holds:
  the memory limit, less the floor below, the arena's ceiling and the
  process's own base, at about 6 KB an idle connection.  `/stats` says
  which (`clients.max_basis`).  At the limit a connection is accepted,
  told why and closed, and `/stats` still answers: the RESP door and an
  encrypted native door answer `-ERR max number of clients reached` at
  once - the native client before any handshake, so a storm of refused
  clients costs no cryptography (a library that does not read the line
  sees its handshake fail); a plaintext native door (loopback, unix)
  answers an error frame to the first request.  The refusals are
  counted.
- `memory_floor` under `[daemon]` (default `15%`) is what keeps the
  out-of-memory killer away from the daemon, whatever `max_clients`
  says.  Once a second the daemon reads the memory it may still use -
  `MemAvailable`, or the headroom of a memory-limited cgroup it runs in,
  whichever is tighter - and takes off the arena's not-yet-committed
  ceiling, which the store may still grow into.  While less than the
  floor remains, new connections are refused exactly as at
  `max_clients`; connections already open are untouched, and refusing
  stops once a further 5% of the limit is free again.  `N%` is a share
  of the memory limit (1-50, kept between 128 MB and 4 GB), `N MB` /
  `N GB` an absolute floor, `off` disables the guard.  The state, the
  figures and the refusals are under `clients.memory` on `/stats`, on
  `/metrics` and as a red "memory floor" chip on the page; the log says
  when the guard engages and releases, with the limit and, under a
  cgroup, its usage and inactive file pages, and `clients.refused` is
  raised while it refuses (0.7.4).  Under a cgroup limit that can never
  hold the floor, the arena and the process itself, the daemon refuses
  to start and says the sum; `perfcached -C` says it beforehand.
- An idle connection holds no buffers: after two quiet seconds its
  input and output buffers are released (they grow back on its next
  request), and once a burst of released or closed connections has
  settled the daemon hands the freed heap back to the system
  (`malloc_trim`).  `clients.buffers` on `/stats` counts both.
- A client that pipelines faster than it reads is throttled, not
  closed.  Once 4 MB of replies wait for it, its further requests wait
  too and its socket is not read - TCP holds the client back - until
  the replies drain below 1 MB; then it is served from where it stopped.
  The first time a connection is throttled the log says
  `throttling client <ip:port>: N MB of replies unread`, and its
  disconnect line ends `, throttled N times`.  A client that reads
  nothing for `output_stall_s` under `[daemon]` (default 30) meanwhile
  is closed as a slow consumer.  `clients.output` on `/stats` and
  `/metrics` count the throttled connections, the pauses and the
  stall closes.  The 8 MB output cap still closes a pub/sub subscriber
  that does not read its messages - those are not requests it paced.
- `cluster_accept` under `[secrets]` (0.7.4) is a second cluster
  secret every frame and handshake is also opened with - never sealed
  with - so the cluster secret rotates without a flag day: three rolling
  restarts, `cluster` = old and `cluster_accept` = new on every node,
  then `cluster` = new and `cluster_accept` = old, then without it.
- A fleet stanza runs unchanged on a single node: without a
  `[cluster]` section, a collection's `mode` and `pull` are accepted,
  ignored and named once at warning level, and the cluster secret is
  optional.  With a `[cluster]` section, a `replicas` the live members
  cannot honour is logged once and shown as `replicas_short` on
  `/stats`, `/metrics` and the status page.
- `plaintext = loopback` under `[listen]` allows plaintext binary
  frames and RESP on loopback listeners (127.0.0.0/8, `[::1]`) and unix
  sockets only - handy for debugging (netcat or redis-cli can drive the
  RESP side; the native protocol needs perfcli or libperfd); the LAN
  stays on the Noise channel.  The default (`never`) asks every client
  for the secret.
- **`encrypt = no | yes`** under `[listen]` (0.7.4): whether the native
  door's requests are encrypted once a client is in.  Every connection
  still opens with the client secret's Noise handshake - the secret is
  never sent and a wrong one is refused - and under the default `no`
  a client that asks (libperfd and perfcli do, unless told not to)
  then carries its requests in the clear: no seal and no open per
  request, about half the CPU a request on both sides.  Anyone on the
  path can read that traffic and also change it - a request or a reply
  altered in flight is not detected, since only the handshake is
  authenticated - and the node says so at start for each such door.
  Off a network you trust, use `yes`.  `yes` keeps every connection encrypted; one listener can
  differ from the rest with `tcp = <addr:port> encrypt=yes` (or `no`).
  A client keeps its own connection encrypted with
  `perfd_opts.encrypt = 1` or `perfcli --encrypt`; `enable` is refused
  on a plaintext connection, since its secret grants DDL (perfcli's
  `-E` and `-e` imply `--encrypt`).  The cluster plane is not affected:
  it is always encrypted.
- **`resp = <addr:port>`** adds a dedicated listener for Redis clients
  that must reach the cluster over a network.  It is RESP2 ONLY - the
  native protocol (and with it the admin verbs) is refused on it -
  and because a Redis client cannot speak the Noise channel it is
  plaintext unless given `tls` (below), so it is guarded instead: `resp_allow = <cidr>[,...]` is
  REQUIRED off-box (the daemon refuses to start without it),
  `[secrets] resp` adds a Redis `AUTH` password (without one, `-C` and
  the start warn, 0.7.5: any client the allow-list admits reads and
  writes; an HTTP door off loopback without `[secrets] http` is warned
  about the same way), and
  `resp_collections` bounds which collections it can see and maps the
  Redis database index onto them: an entry `0:sessions` makes `SELECT 0`
  reach the collection `sessions`, and a bare name is reachable by that
  name.  Without a mapping a collection has to be NAMED `0` to be
  reachable at all.  The map can be changed on a running fleet (0.7.1):
  `perfcli map <index> <collection>`, `unmap <index>` and `maps` (see
  PERFCLI-COMMANDS.md) - every connection sees the change on its next
  command, every member takes it, it is kept in `<state_dir>/respmap`
  across restarts and outranks `resp_collections` for its index until it
  is unmapped; the page shows each collection's DB index and, under the
  dashboard tool, map and unmap.  `stats` reports a `resp` block (connections,
  allow-list rejections, auth failures, the map as it holds: `map`,
  `map_overrides`).  The door also serves the
  Redis observability surface - section-faithful `INFO` (commandstats
  and latencystats included), `CLIENT LIST`/`SETNAME`, `SLOWLOG`, and
  the `CLUSTER` family (`SLOTS`/`SHARDS`/`KEYSLOT`/`NODES`) - so
  Grafana's redis-datasource and cluster-aware Redis clients work
  against it unmodified (a key on the wrong node is forwarded by
  default; `resp_redirect = moved` answers it with a genuine `-MOVED`
  instead, which is the only thing that makes a cluster-aware client
  refresh its slot table); `TIME`, `EXPIREAT`/`PEXPIREAT` and `MEMORY
  USAGE` round out the tooling set.  The command rows count every
  door: RESP commands under their Redis names, the native door as
  `cmd:<method>` (a method) and `bin:<verb>` (a fixed-layout data
  verb), each row with a log2 latency
  histogram behind it - and the same rows are `commands` and `slowlog`
  on `/stats`, `perfcached_command_calls_total{cmd}` /
  `perfcached_command_usec_total{cmd}` and the
  `perfcached_command_latency_seconds{cmd}` histogram on `/metrics`.
- **TLS on the RESP and HTTP doors (0.7.5).**  `resp = <addr:port> tls`
  and `http = <addr:port> tls` serve the same door over TLS, so the
  Redis `AUTH` password and the HTTP token stay off the wire; a
  plaintext loopback door can stay beside a TLS one.  `[listen] tls_cert`
  (PEM, the chain after the certificate) and `tls_key` name the pair -
  the certificate must name the address clients dial, an IP SAN for an
  IP - and `tls_min = 1.2 | 1.3` (1.2) the oldest version taken.  It
  needs a build with OpenSSL: `PC_TLS = auto`, the default, uses it when
  pkg-config finds it, and `perfcached -V` prints `tls: OpenSSL ...` or
  `tls: off`.  The native door has Noise and takes no `tls`.  `-C` and
  the start load the pair and check that the key matches.  A reload reads
  the pair again, so a renewal is `systemctl reload perfcached`: the next
  connection gets the new certificate, the open ones keep theirs, and a
  pair that does not load keeps the old one and fails the reload with
  OpenSSL's reason.  `/stats` has a `tls` block (the library, the
  certificate, `days_left`, handshakes, failed ones and the last reason;
  the dashboard's `tls` card shows the days left, amber inside 14),
  and the alert `tls.cert` is a warning inside 14 days of the expiry,
  critical once it has passed.  A plaintext RESP or HTTP door off loopback
  with `[secrets] resp` or `http` is warned about at start;
  `auth_needs_tls = yes` refuses `AUTH` and the token there (`-ERR AUTH
  needs TLS here ...`, HTTP 403) and takes them over TLS and from
  loopback.  Clients:

      redis-cli --tls --cacert /etc/ssl/perfcached-ca.pem -h 192.0.2.31 -p 6380 --askpass

      # redis-py
      redis.Redis(host="192.0.2.31", port=6380, password=pw,
                  ssl=True, ssl_ca_certs="/etc/ssl/perfcached-ca.pem")

      // phpredis 5.3 or later
      $r->connect("tls://192.0.2.31", 6380, 2.5, null, 0, 0,
                  ["stream" => ["cafile" => "/etc/ssl/perfcached-ca.pem"]]);
      // predis
      new Predis\Client("tls://192.0.2.31:6380?ssl[cafile]=/etc/ssl/perfcached-ca.pem");

      # Prometheus
      - job_name: perfcached
        scheme: https
        tls_config: { ca_file: /etc/prometheus/perfcached-ca.pem }
        authorization: { credentials_file: /etc/prometheus/perfcached-token }
        static_configs: [{ targets: ["192.0.2.31:8443"] }]
- **IPv6 on the client doors (0.7.2).**  `tcp`, `resp` and `http` take
  an IPv6 address: `[::1]:6480` listens on IPv6 loopback, `[::]:6480` on
  every address of both families - an IPv4 client of it arrives as
  `::ffff:a.b.c.d` (the door turns `IPV6_V6ONLY` off itself, whatever
  the host's `bindv6only` says).  `resp_allow` and `http_allow` take IPv6
  prefixes beside the IPv4 ones (`fd00::/8`, `[::1]/128`; a bare address
  is `/128`), and an IPv4 client of a `[::]` door is checked against the
  IPv4 entries.  `plaintext = loopback` counts `::1` as loopback.  An
  upstream may be written `redis://[::1]:6379`, and perfcli, perfdump,
  perfload and libperfd take `[v6]:port` and IPv6 literals.  Before any
  of this the node checks that the host has IPv6 at all - an IPv6 socket
  and a bind to `[::1]`, never a kernel version or a sysctl alone.
  Where that fails (`ipv6.disable=1` at boot, `disable_ipv6 = 1`, a
  container without IPv6) an explicit IPv6 listener is refused at `-C`
  and at start with the reason, and every name the node resolves - the
  listeners, the upstreams, the library's connects - asks for IPv4 only,
  so no AAAA query goes out.  `-C` prints `net: IPv6 is usable on this
  host` or `... is not usable ... - <why>`; `/stats` has it as
  `io.ipv6`.  The cluster stays IPv4 in 0.7.2: members advertise and
  reach each other over IPv4, and clients reach them over either.
- `arena_cap_mb` lets the arena grow elastically under pressure - by
  2 MB group inside one address-space reservation, on the same tier as
  the initial commit; `reclaim_*` returns idle groups to the kernel.
- **Two ways to run the memory (0.7.5).**  *Held*, the default: the
  arena is locked (`pin = auto`, or `require` to refuse a start that
  cannot lock it), on huge pages where the host has them - never
  swapped.  The give-back still runs: an idle 2 MB group is unlocked
  before it goes back to the kernel and locked again when records bring
  it back, so on transparent huge pages `arena_committed` follows the
  load and `perfcached_reclaim_giveback_disabled` stays 0 (it turns 1
  only once the kernel has refused a give-back).  *Unlocked*: `pin = no`
  with `huge_pages = off`, `arena_mb` at the steady working set and
  `arena_cap_mb` for the bursts - nothing is locked, at the price of
  pages the kernel may swap and growth that faults pages in.  Either
  way `reclaim_*` returns free 2 MB groups once they have been quiet for
  `reclaim_quiet_s` (5 s) and nothing was carved for `reclaim_cooloff_s`
  (10 s), keeping `reclaim_keep` (1) drained chunk per size class.  A
  reserved huge-page pool stays reserved whatever the arena hands back -
  the host gains nothing - so the unlocked way runs on the 4K tier.
- A reserved hugepage pool makes the arena's top tier deterministic -
  see [contrib/sysctl-perfcached.conf](../contrib/sysctl-perfcached.conf),
  and read back `HugePages_Total` after applying: live hosts routinely
  under-deliver the reservation until memory is compacted.
- `log_events = miss, expired, store, remove` (per collection, any
  subset) logs one line at NOTICE per event with the key, the door and
  the peer, and for a miss whether the key was absent or expired.  It
  is a per-collection mask, not a level: `log_level` is a ceiling,
  `[daemon] log_events` is the default, `log_events_rate` (100/s)
  bounds it and the daemon reports what it suppressed,
  `log_events_hash = yes` logs a hash instead of the key.
- `perfcached -E -f <conf>` dumps the normalized effective config with
  secrets masked.


## Example configurations

[contrib/CONFIGURATIONS.md](../contrib/CONFIGURATIONS.md) - eleven complete
files, each validated with `perfcached -C`:

- **single node**: minimal; for Redis clients (the RESP door with its
  guards, `SELECT n` onto named collections, the status page, a local
  socket); durable (identity, WAL, snapshots, runtime DDL behind the
  `enable` secret, a sampled query log); elastic memory (a capped arena
  that gives back, an index that grows and shrinks).
- **fleets**: eager; store (pull-on-miss); proxy (the capacity plane);
  shard behind cluster-aware Redis clients; spread with `replicas`.
- **two fleets on one host**: a second daemon in a second cluster.
- **a cloud or Kubernetes**: an eager fleet found by unicast seeds, no
  multicast.
