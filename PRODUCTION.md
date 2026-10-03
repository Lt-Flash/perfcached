# Production checklist

One page to walk before perfcached carries real traffic. Every item is
here because getting it wrong has a specific, observed consequence —
the "why" is the point, not the ceremony.

## 1. Memory

- [ ] **Size `arena_mb` from the working set, not from free RAM.** It
      is a hard bound: past it the node *refuses writes* rather than
      growing. That is deliberate, and it makes the failure sudden — a
      fleet is fine until it is shedding.
- [ ] **Watch `perfcached_arena_headroom_ratio` and alert well before
      zero** (0.15 is a reasonable page). By the time
      `perfcached_writes_refused_total` moves you are already dropping
      writes.
- [ ] **Know the live-payload ratio.** Only part of the arena is your
      values; the rest is index and slab slack. Measure it on your own
      key shape before sizing — in one test 15.9 MB of values occupied
      a 64 MB arena.
- [ ] **`LimitMEMLOCK=infinity`** (the shipped unit sets it). The arena
      is pinned so it cannot be swapped — a swapped cache is worse than
      no cache.
- [ ] **Pinning and give-back are mutually exclusive.** A pinned node
      cannot return memory to the host: `MADV_DONTNEED` is refused on
      locked pages. Expect `perfcached_reclaim_giveback_disabled 1` and
      a flat `arena_held` there; set `reclaim_floor_mb` only where
      give-back actually happens.

## 2. Huge pages

- [ ] Optional — the arena degrades THP → 4K cleanly and the daemon
      reports which tier it got. Provision only if you want the tier
      pinned: see `contrib/sysctl-perfcached.conf`.
- [ ] **Read `HugePages_Total` back from `/proc/meminfo` after setting
      it.** A live system routinely under-delivers until caches are
      dropped and memory compacted.
- [ ] In a VM, hugepage availability is the hypervisor's business as
      much as the guest's.

## 3. Durability (WAL and RDB)

- [ ] **Put `[wal] dir` on real storage, not tmpfs.** The daemon warns
      when it detects tmpfs: that protects against a process crash
      only, never against host or power loss.
- [ ] **Let the probe run** (`probe = auto`) unless you have measured
      this device yourself. It picks `wal_fsync` and the segment
      geometry from the storage's actual behaviour, and reports both
      the identity it resolved and what it measured — a mismatch (a
      "SAS spinner" measuring NVMe-fast) is a controller cache, and is
      worth knowing.
- [ ] **Do not trust the probe on a VM with a write-back disk cache.**
      Measured 2026-09-03 on a Proxmox guest (Ceph RBD, `cache =
      writeback`): the probe rated the btrfs volume SSD-class at 139 us
      p50 and ~3467 sustainable synced writes/s, while `fio` doing the
      WAL's own shape (4K append + fdatasync, QD1) measured **3.7 ms
      p50 and 236/s** on that same mount.  A short probe never fills the
      HOST page cache, so it measures the cache.  **Verify with fio
      before enabling `fsync = always`**, or set the disk to `cache =
      none` (O_DIRECT) so guest fsync latency is honest.  See DESIGN
      section 12am.
- [ ] **Put the WAL on ext4 rather than btrfs.**  Same Ceph volume,
      same host, identical fio job (4K randrw, QD1, `--fdatasync=1`,
      4 threads): **ext4 773 IOPS at 4.75 ms sync p50; btrfs 490 IOPS
      at ~8 ms**.  COW commits metadata trees on every fdatasync.  Note
      the probe ranks these two the WRONG WAY ROUND, which is the
      sharpest reason not to choose a filesystem from its verdict.
- [ ] **Alert on `perfcached_wal_dropped_total`.** A ring-full drop is
      an acknowledged write that never reached the WAL. Workers are
      never blocked by design, so pressure shows up here rather than as
      latency - and the FIRST drop marks the node FAILED (reads only,
      refuses writes, stays a member) until an operator restarts it with
      a deeper `ring_kb`, `everysec`, or less load.  With `probe = no`
      nothing sizes the ring: set `ring_kb` yourself on such storage.
- [ ] **`sync` is a barrier, not a guarantee of completeness.** Its
      reply carries `dropped` — a receipt that covers survivors only is
      not a full barrier.
- [ ] Network-backed storage (iSCSI, NFS, NVMe-oF, shared LVM) *stalls*
      rather than failing: 30–120 s hangs are the failure mode. The
      daemon reports the resolved device chain at startup — read it and
      confirm it says what you think it does.

## 4. The cluster plane

- [ ] **Pick the discovery mode for the network.**  Multicast (the
      default) needs every node on one L2 segment with multicast working
      on it - sent with a TTL of 1, it never crosses a router, and a
      cloud VPC does not carry it at all.  Where it does not work, use
      `discovery = unicast` (below).  Confirm before deploying, not
      after.  The two modes do not hear each other's membership
      traffic: switch a fleet between them by stopping every node and
      starting them all with the new config.
- [ ] **Across routed links, only with `[cluster] multicast_ttl` and
      multicast routing** (PIM, or static routes such as smcroute's),
      and then all of: every node's `advertise` address reachable from
      every other node with no NAT between them; the cluster port, the
      pub/sub relay port and the bulk TCP port passed by the routers and
      firewalls; the multicast group's scope allowed through (the
      examples use 239.0.0.0/8, admin-scoped).  Verified with one
      router in a lab rig (formation, and a router restart inside the
      dead-node margins).  NOT measured over WAN links: beats are 1 Hz,
      a master is presumed dead after 8 s and a peer after 10 s - check
      what loss and multicast-route convergence do to those margins on
      your network before relying on it.
- [ ] **In a cloud VPC or Kubernetes: `discovery = unicast`.**  The
      network carries no multicast.  Open between every pair of nodes:
      the cluster port (UDP and TCP), the frame lane (TCP, the cluster
      port + 2), the pub/sub relay port (UDP, the cluster port + 1).
      Address the nodes directly (instance or pod IPs), never through a
      load balancer or a Service's virtual IP.  Seed with names that
      resolve to several members - for Kubernetes a headless Service
      with `publishNotReadyAddresses: true`.  Set `expect` to the fleet
      size for cold starts.  Identity: `identity_source = env` with
      `PERFCACHED_PLATFORM_KEY` = `metadata.uid` for a Deployment, the
      identity file on a persistent volume for a StatefulSet.  Watch for
      `cluster.foreign_clusters` (a second cluster on your seeds) and
      `lane.udp_blocked_saves` (UDP filtered on some path).  The mesh
      sends one heartbeat per node per peer per second: it suits a fleet
      of a few dozen nodes.
- [ ] **Check unicast discovery on the running fleet**, not only the
      config: `/stats cluster.discovery` - `seeds_resolved` equal to
      `seeds` (a seed that does not resolve is retried every 5 s, and
      said once), `known` at least the fleet size; `cluster.lane` -
      `peers_up` toward every peer once large frames have flowed,
      `dropped_*` flat.  A peer whose UDP is filtered but whose TCP
      works is kept and switched to TCP with an ERROR (`lane.forced`,
      `udp_blocked_saves`) - fix the filter: everything to that peer
      then rides one TCP connection.  Tested in lab network namespaces
      (fragments dropped, UDP blocked, seed names moved); not yet in a
      real cloud deployment.
- [ ] **A paused VM or starved container is not a dead fleet** (both
      modes): a node whose own loop stalled holds purges and elections
      for as long as it was stalled (`cluster.self_late` counts it), and
      a member that loses only its own path to the master waits up to
      30 s for the master instead of electing (`cluster.election_holds`).
      A real master failure still elects - after the 8 s margin plus
      that wait when another member keeps hearing the old master.
- [ ] **One cluster id per fleet.**  A node remembers the cluster it
      belongs to (`cluster-id` in its state directory) and will not join
      another; a node moved between fleets needs that file deleted.
      Consider `[cluster] cluster_id = <name>` per environment, so a
      staging node cannot join production even on a shared group or
      seed list.  A second cluster heard on the same group or seeds is
      an ERROR and `/stats cluster.foreign_clusters` - never a merge.
- [ ] **A node identity a clone cannot copy.**  Each node derives its
      identity from the machine (DMI uuid, else NIC MAC) and caches it
      in its state directory; a container has neither, and a copied
      volume carries the cached file.  Name the source with
      `[cluster] identity_source` so a missing one refuses the start:
      `env` (Kubernetes Deployment: `PERFCACHED_PLATFORM_KEY` from
      `metadata.uid`), `file` (a StatefulSet's persistent volume), or
      `dmi` / `mac` on VMs and hosts.
- [ ] **Every node must agree on mode and the collection set** — the
      config digest refuses a mismatched join rather than joining
      wrongly.
- [ ] **Upgrade the whole fleet together.** Formats can change in any
      release: stop every node, clear its state, start the new version
      on all of them. Do not run two versions in one fleet.
- [ ] `net.core.rmem_max` ≥ 8 MB, or leave `CAP_NET_ADMIN` in place so
      the daemon can force its own receive buffer. A dropped forward
      datagram is a refused client write — forwards have no retry.
- [ ] Proxy-mode collections **lose their keys when a node dies**; the
      peers cannot rehydrate them. WAL and RDB matter more there.

## 5. Exposure

- [ ] **Client and cluster secrets must differ** — the daemon refuses
      to start otherwise. Keep the config `0640`; it warns when the
      file is world-readable.
- [ ] **The RESP door is plaintext and has no handshake.** Off-box it
      requires `resp_allow`, and it should also have a password. It
      reads *and writes* data.
- [ ] **The HTTP door is plaintext.** It serves the status page,
      `/members`, `/metrics` and `/health`. Off-box it requires
      `http_allow`; prefer binding it to loopback with a TLS-terminating
      proxy in front — that also buys better auth than we would write
      (OIDC, mTLS, whatever you already run).
- [ ] **`[secrets] http` is a second layer, not the first.** Over
      plaintext the token is replayable by anyone who can see the
      traffic. It exists so one mis-scoped firewall rule does not
      immediately hand over the fleet's topology.
- [ ] **Rotate secrets with the list form**: clients accept several
      passwords at once, so add the new one everywhere, then drop the
      old one — no flag day.

## 6. If you front the fleet with HAProxy

- [ ] Use `contrib/haproxy-perfcached.cfg` as the starting point: one
      backend, every node, `balance roundrobin`. There is no master to
      prefer and no sentinel tier to keep.
- [ ] **Do not use `option redis-check` on its own.** It sends PING,
      and a node answers PONG while it is still loading — so a warming
      node passes the check and receives clients that then get
      `-LOADING`. The reference config asks a data question too.
- [ ] `resp_allow` must cover the HAProxy machines, and set a
      `[secrets] resp` password if the door is reachable off-box.
- [ ] **Never put a cluster-aware client behind the VIP.** It learns
      the real node addresses through the proxy and then bypasses it;
      if only the VIP is routable from that host, it breaks.
- [ ] Raise `timeout client`/`timeout server` — a cache connection is
      idle between bursts, and the default silently closes pooled
      connections.

## 7. Before you call it done

- [ ] `systemd-sysusers` has run, so the `perfcached` account exists.
- [ ] The unit starts from cold **and** survives `systemctl restart`
      under load.
- [ ] One daemon per `state_dir` and per `[wal] dir` - enforced: each
      holds a lock file (`perfcached.lock`) for its life, and a second
      daemon on the same directory is refused with the holder's pid
      (after waiting up to 5 s, so a restart that overlaps its dying
      predecessor still starts).
- [ ] The host has a working wait-online service, or you accept the
      daemon's own wait.  The unit orders after
      `network-online.target`, but that target is reached at once where
      no wait-online service is enabled (many minimal containers:
      `systemctl is-enabled systemd-networkd-wait-online` says
      `disabled`).  perfcached then waits up to `address_wait_s`
      (default 30 s) for a configured address to appear, and says so in
      a NOTICE rather than failing the start.
- [ ] `GET /metrics` is being scraped, and one dashboard shows
      headroom, refusals, WAL drops and cluster node count.
- [ ] You have restored from an RDB snapshot once, on purpose, before
      needing to.
