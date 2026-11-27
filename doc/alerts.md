# Mail and alerts

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

## Mail

A node can send mail itself.  It needs a build with an SMTP client - `make
menuconfig`, mail = `libcurl` (libcurl's development files: Debian/Ubuntu
`libcurl4-openssl-dev`, Alpine `curl-dev`); a build without one refuses a
`[mail]` section at `-C` and says so, rather than ignoring it.

```
[secrets]
mail = <the SMTP password>

[mail]
smtp = smtp://mail.example.net:587      # STARTTLS, required
user = alerts@example.net
from = perfcached@example.net
to = ops@example.net, oncall@example.net
```

`smtp://` upgrades with STARTTLS and will not send without it;
`smtps://host:465` is TLS from the first byte.  The server's certificate
is checked against the system's CAs, or `ca_file`.  For an internal relay
whose certificate cannot be verified - Postfix's self-signed default, an
expired one, one that names another host - `tls_verify = no` keeps the
encryption and skips the check; `-C` and the log say so, since anyone on
the path could then pose as the server.  The password lives in
`[secrets] mail` - a URL with credentials in it is refused - and appears
in no log line, `/stats` field or `-E` dump.  `to` takes up to 8
addresses.

Sending runs on its own thread with a bounded queue, so a slow or dead
mail server never holds a worker; `timeout_s` (30) bounds one send.
`perfcli -E <enable secret> mailtest` sends one mail now and prints the
server's answer - `{"sent":true,"reply":"250 ..."}`, or `false` with
what the server or the connection said (a wrong password's `535`, a
server offering no STARTTLS, a timeout).  `/stats` `mail` carries
`built`, `ready`, `sent`, `failed`, `dropped` (the queue was full),
`queued`, and `last_error` with its time.

## Settings the fleet must agree on

**Settings the fleet must agree on (0.6.5).**  Each member carries, in
its heartbeat, a hash of each setting whose difference changes what the
fleet does: `[wal] fsync`, whether the HTTP token is set, the `[alerts]`
thresholds, `tombstone_retain_s` and `tombstone_retain_max`, `[cluster]
max_datagram` (0.7.4: a member that sends larger datagrams depends on
fragmentation the others avoid), and for each
read-through collection `upstream_writes`, `upstream_prefixes`,
`upstream_role` and `upstream_unknown` - a member on `allow` beside
members on `through` takes writes that never reach Redis - and which
collections are caches (`durable = no`, `config.caches`).  Per setting,
the value most live members hold is the fleet's (with two members, each
holds its own); every node raises `config.<setting>` naming the member
that holds another, its fleet card says "settings differ from this node",
and `/members` lists them as `config_differs`.  Per-node settings -
addresses, `io`, `workers`, `[mail]` - are not compared, and a member
before 0.6.5 reports none.

## What raises an alert

**What raises an alert.**  Every node checks, once a second, the figures
it already keeps:

| alert | level | raised | cleared |
|---|---|---|---|
| the node FAILED / HEALING | critical / warning (critical after 10 minutes HEALING) | at once | when it is not |
| the node's reconcile has no witness - every live peer restarted after it went down: restart this node (`node.reconcile`, 0.6.4; from 0.7.5 a restart copies a survivor's state or keeps its own, and no pass waits) | warning | after 30 s | 30 s after it resumes or ends |
| the WAL dropped or overran records | warning (critical for the third loss within an hour) | the second it happens | after 5 quiet minutes |
| WAL fsync's mean over the last minute above 4x the start-up probe | warning | after 5 minutes of it | after a minute below |
| the WAL filling faster than snapshots free it - the whole log fills faster than one snapshot takes, or its free segments last under twice the last snapshot's duration (`wal.capacity`, 0.7.2) | warning | after a minute of the first, 5 minutes of the second | after 5 minutes without |
| writes refused, the arena full | critical | the second it happens | after 5 quiet minutes |
| the arena holding over `[alerts] arena_warn_pct` (65) / `arena_crit_pct` (75) of its ceiling (`memory.arena`) | warning / critical | after a minute | after a minute below |
| the host's CPU, the disk holding the WAL (else the `state_dir`) or the host's memory over `[alerts] cpu_warn_pct`, `io_warn_pct`, `memory_warn_pct` (65) / the `_crit_pct` (75) over the last minute (`host.cpu`, `host.io`, `host.memory`) | warning / critical | after 5 minutes (memory: a minute) | after a minute below |
| the TLS doors' certificate within 14 days of its expiry, or past it (`tls.cert`, 0.7.5) | warning / critical | at once | a minute after a renewal |
| new connections refused at the memory floor (`clients.refused`, 0.7.4) - what is available, the floor, the arena's reserve and the limit with where it comes from | warning | the second the guard refuses | after a minute admitting again |
| a read-through upstream unreachable, or its tracking off; the text gives the reason - unreachable, or under `upstream_role = replica` a master or not a replica | warning | after a minute | after a minute back |
| no upstream of a read-through collection serves, of several (`upstream.<col>`) | critical | after a minute | after a minute back |
| a read-through collection failed over from its primary (`upstream.<col>.failover`), or reading from the master because no replica serves (`upstream.<col>.master`) | warning | after 10 s | after 10 s back |
| another perfcached reads a read-through collection through this node - a daisy-chain (`chain.<col>`) | warning | at once | after 5 quiet minutes |
| a write-through collection without a master: every write refused (`write.<col>`) | critical | after 10 s | after 10 s back |
| no RedisJSON on the write-through master (`write.<col>.json`) | warning | after 10 s | after a minute back |
| writes refused at a full write-through queue, or not applied after waiting too long in it (`write.<col>.queue`) | warning | after 10 s | after 5 minutes without |
| a write-through collection not following other clients' changes in Redis - broadcast tracking off or refused (`write.<col>.tracking`) | warning | after a minute | after a minute back |
| commands perfcached does not know over a tenth of the writes in 5 min, of 20 or more (`write.<col>.passed`) | warning | after a minute | after 5 minutes below |
| on epoll, the busiest worker over `[alerts] epoll_busy_pct` (80) of its time outside its wait, over a minute - with the change to make | warning | after 5 minutes of it | after a minute below |
| the network link under the node's address over `[alerts] link_warn_pct` (80) / `link_crit_pct` (95) of its speed in its busier direction, over a minute (`host.link`, 0.7.4) - the device, the path to it (a VLAN to its bond to the active slave) and what to change; the host's other traffic counts, since it shares the link. The member's dashboard card shows its link and blinks amber or red with it | warning / critical | after 5 minutes of it; critical at once when cluster datagrams drop while it is at the critical level | after a minute below |
| deletes or expiries the tombstone cap dropped before `tombstone_retain_s` in the last 5 minutes, the text saying how many of each (`cluster.tombstones`) | warning | after 10 s | 5 minutes after the 5-minute count is back to 0 |
| a member back after a silence longer than the retained deletes cover (`cluster.peer_absent`) - or, where members acknowledge deletes (0.7.3), one asked to rebuild for it | warning | once | - |
| a member whose mark of a node's deletes has stood still for 30 s while the others confirm them - every delete since is kept for it (`cluster.acks_stuck`, 0.7.3) | warning | after 10 s | after a minute moving |
| on the master, a joiner still `connecting` 10 s after its admission - a pair of links with it has not answered both ways; the text names the pairs and what did not answer: the cluster socket, the relay port, the frame lane, the relay port's TCP (`clrpt`) or a member's bulk listener (`cluster.admission_stuck`, 0.7.5) | warning | at once | 30 s after none is held |
| a mastership term this node follows could not be written to its state directory (full, read-only, a failing disk) - it serves and follows, but takes no mastership until the term is on disk, retrying every beat (`cluster.term_unpersisted`, 0.7.5) | critical | at once | 10 s after it is written |
| a member silent, failed, stalled, gone or dropped (`fleet.node<id>`) - a member that left stays listed while its id is held for it, a week; one removed for good is cleared with `perfcli -E <enable secret> forget <id>` on each node, which also takes it out of the fleet's size; members missing against the fleet's size (`fleet.missing`); fewer ready than a majority (`fleet.majority`) | warning; warning; critical | after 30 s (a goodbye: a minute); after a minute; after a minute | after 30 s |
| a setting the fleet must agree on held differently by a member (`config.<setting>`, 0.6.5) | warning | after a minute | after a minute agreeing |
| a member holding other autoscale modes than this node's, or one too old to take them while modes are set (`cluster.autoscale`, 0.7.2) | warning | after 30 s | after a minute agreeing |
| the node started (ready to serve) | info; warning after an unclean stop | once | - |

A level says how soon a person must act, not how bad the event was:
**critical** - now, the node cannot recover by itself; **warning** -
today, the node is handling it (a lost WAL record heals by a snapshot, a
node back after a crash is serving again); **info** - routine.  A
self-healing alert turns critical when the healing stalls.  An unclean
stop is told by the WAL: a clean stop snapshots after the WAL
is flushed, so a start that replays records above the last snapshot
follows a crash, a `kill -9` or a power loss.  Each alert is logged (at
NOTICE, WARNING or CRITICAL by its level, and its clear at NOTICE) and kept in `/stats`
`alerts` - `active`, and `recent`, the last 100 raised and cleared,
newest first, each saying what became of it by mail.  With `[mail]`,
alerts at `level` or above (`warning` by default; `info` mails a clean
start too, `critical` only what needs someone now) are mailed: the same alert
at most once per `min_interval_s` (900) - a repeat inside it is kept as
`suppressed` - unless it got worse; everything raised or cleared within
`digest_s` (60) goes out as one mail; and a clear is mailed when its raise
was.  Without `[mail]` the ring says `no mailer`.

A mail has a plain-text part and an HTML part (inline styles, no
images or links to fetch) and reads like this:

```
From: "cache1" <perfcached@example.net>
Subject: WARNING: WAL fsync slow on cache1
```

The subject is `<STATE>: <title> on <host>`, at most 78 characters -
CRITICAL, WARNING, INFO, or RESOLVED - with `(+N)` when the mail carries
N more; From's name is the host the alerts are about.  The body says,
per alert, what happened, its status (a new problem, still open,
resolved, or an event), when it started, what the node does by itself,
what to do, and the alert's ID (`wal.fsync`, `upstream.sessions`, ...).  A
RESOLVED mail says what was wrong at its worst, the value now and how
long it lasted, and answers its problem's mail (`In-Reply-To`), so a mail
client shows the two together.

## Mail in a fleet

**In a fleet, `[mail]` on one node is enough.**  A node that can mail
says so in its heartbeat, and every node agrees on one MAILER: the
lowest node id among the ready members that can.  A node without
`[mail]` sends the alerts it would mail to that node over the cluster
plane, and the mailer sends them as mail about that host - From
its name, its node id beside it - and one mail per host; the SMTP
password stays on the nodes whose config holds it.  What happens to the
fleet itself - a member silent, failed or stalled, gone or missing
against the fleet's size, or fewer ready than a majority (critical) -
is seen by every member and mailed by the mailer alone, once.  Put
`[mail]` on two or more nodes and any of them can carry the fleet.  When
no member can mail, a node says "no mailer reachable" in its log (at most
every ten minutes) and keeps its alerts in the ring.  A network split
elects a mailer on each side, so a split fleet may send a mail from each
- two rather than none.  The status page shows where mail goes - "mail
this node / via node N / none" in the header - and an alerts card with
what is raised now and the recent ring.  `/stats` carries
`alerts.mailer`, `alerts.fleet_mailer` and, in `mail`, `relayed_in`,
`relayed_out` and `relay_dropped`; `/metrics` has
`perfcached_alerts_active{level}`, `perfcached_mail_ready`,
`perfcached_mail_{sent,failed,dropped}_total` and
`perfcached_alert_relays_total{direction}`.  A node watches at most 64
alert conditions at once (a cleared one's place is reused once
`min_interval_s` has passed since its last mail); a condition that finds every place taken
is not watched - logged as a WARNING naming it, at most every ten
minutes - and counted in `/stats` `alerts.keys_refused`.  The fleet's
own `fleet.missing` and `fleet.majority` are judged before the
per-member alerts, so a large fleet's members cannot crowd them out.
