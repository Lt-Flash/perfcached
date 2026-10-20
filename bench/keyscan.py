#!/usr/bin/env python3
"""keyscan.py - the keys/scan bench's client half (bench/keyscanbench.sh).

Speaks RESP to any server (Redis or perfcached's Redis door) and does one
of three things:

  load  <host> <port> <dataset>      build the dataset's keyspace
  keys  <host> <port> <dataset> <kind> <calls> <rate>
                                     KEYS <pattern> <calls> times, at
                                     <rate>/s (0 = back to back), and
                                     report per-call latency
  scan  <host> <port> <dataset> <kind> <count> <walks>
                                     the same lookup as a SCAN walk:
                                     MATCH <pattern> COUNT <count>
                                     until cursor 0, <walks> times
  ping  <host> <port> -              PING, for readiness
  njwt  <host> <port> <dataset>      how many jwt keys (pcbench -n)

The keyspaces follow the SHAPES of two production Redis instances
(sampled 2026-10-01): the classes of key, their proportions, their
lengths, the share with a TTL, and the KEYS pattern one of them really
sends.  The names are neutral stand-ins of the same length - no key,
value or prefix of theirs is in here.  Same seed, same keys, on every
server, so every arm walks the same table.

  mixed    73,733 keys, half with a TTL: 51% svc:auth:access_token:jwt_
           <40-char token>, 30% svc:web:sessions:<26-char token>, 9%
           tenant1:account_statistics.item_count_per_day.*, 6% tenant1:
           account_outbound_reqs_counter_<n>, ~2.6% other statistics, the
           rest small classes incl. ~180 tenant1:tmp_resolvers_<id>_<n>_
           <token>.  Its scan traffic is the instance's real one, KEYS
           "tenant1:tmp_resolvers_<id>_<n>_*" - a lookup that matches
           about one key and walks all 73,733.
  tokens   38,272 keys: 96% svc:auth:access_token:jwt_<token>, 3%+1%
           per-tenant flags (appx_*USER-OPT-ENABLED-*), a few counters.
           That instance issues no KEYS or SCAN; its pattern here (one
           tenant's flags) is the obvious lookup, NOT an observed one.

The jwt keys are <JWT_PFX><%08u>, the shape pcbench -K produces, so the
background GET load lands on the session keys the walk steps over.
"""
import random, socket, sys, time

JWT_PFX = "svc:auth:access_token:jwt_Qx7Lm2Vp9Rt4Wz8Kc3Nb6Hd1Fg5Js0Ya"   # + %08u = 40-char token
TTL = 86400                       # long enough that nothing expires mid-run
TOKCH = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"

def tok(r, n):
    return "".join(r.choice(TOKCH) for _ in range(n))

def phone(r):
    return "100%08d" % r.randrange(10 ** 8)

def dataset(name):
    """[(key, value, ttl_or_0)], plus the (kind -> [patterns]) to query"""
    r = random.Random(20261001)
    keys, pats = [], {}
    if name == "mixed":
        total = 73733
        def add(n, fn, ttl_share):
            for i in range(n):
                keys.append((fn(i), "v" * 64, TTL if r.random() < ttl_share else 0))
        njwt = int(total * 0.510)
        add(njwt, lambda i: "%s%08u" % (JWT_PFX, i), 1.0)
        add(int(total * 0.299), lambda i: "svc:web:sessions:" + tok(r, 26), 0.6)
        add(int(total * 0.089), lambda i: "tenant1:account_statistics.item_count_per_day.%d_%d_2026-%02d-%02d+10" %
            (r.randrange(1, 9000), r.randrange(1, 400), r.randrange(1, 13), r.randrange(1, 29)), 0.2)
        add(int(total * 0.060), lambda i: "tenant1:account_outbound_reqs_counter_%d" % (100000 + i), 0.0)
        add(int(total * 0.026), lambda i: "tenant1:account_statistics.top_items.%d_%d_2026-09-%02d+10-2026-10-%02d+10" %
            (r.randrange(1, 9000), r.randrange(1, 400), r.randrange(1, 29), r.randrange(1, 29)), 0.2)
        callers = []
        for i in range(180):
            p, n = phone(r), r.randrange(1, 20)
            callers.append((p, n))
            keys.append(("tenant1:tmp_resolvers_%s_%d_%s" % (p, n, tok(r, 40)), "v" * 32, TTL))
        small = ["tenant1:kb_article_%d", "tenant1:ratelim_svc_requests_by_ip_10.%d.1.1",
                 "tenant1:access_message_throttling.account_admins_%d",
                 "tenant1:ratelim_svc_requests_by_account_admins_%d", "tenant1:requester_info_%d"]
        while len(keys) < total:
            keys.append((r.choice(small) % r.randrange(1, 10 ** 6), "v" * 32, 0))
        # the observed lookup: one caller's keys, by phone and id
        pats["narrow"] = ["tenant1:tmp_resolvers_%s_%d_*" % c for c in callers]
        # a broad one, for the reply-size end: one class of 6%
        pats["broad"] = ["tenant1:account_outbound_reqs_counter_*"]
    elif name == "tokens":
        total = 38272
        njwt = int(total * 0.956)
        for i in range(njwt):
            keys.append(("%s%08u" % (JWT_PFX, i), "v" * 64, TTL))
        tenants = list(range(100, 160))
        for i in range(int(total * 0.033)):
            t = r.choice(tenants)
            keys.append(("appx_au_%d_cluster_USER-OPT-ENABLED-%d-%d" % (t, r.randrange(1, 9999), r.randrange(100, 999)), "1", 0))
        for i in range(int(total * 0.008)):
            keys.append(("appx_cluster_USER-OPT-ENABLED-%d-%d" % (r.randrange(1, 9999), r.randrange(100, 999)), "1", 0))
        while len(keys) < total:
            t = r.choice(tenants)
            keys.append((r.choice(["appx_au_%d_cluster_webhook_errors_count",
                                   "appx_au_%d_cluster_webhook_errors_meta"]) % t, "3", 0))
        # NOT observed in production: one tenant's flags
        pats["narrow"] = ["appx_au_%d_cluster_USER-OPT-ENABLED-*" % t for t in tenants]
        pats["broad"] = ["appx_*"]
    else:
        sys.exit("keyscan: unknown dataset %s" % name)
    # one key per name: a duplicate would make DBSIZE disagree between arms
    seen, uniq = set(), []
    for k in keys:
        if k[0] not in seen:
            seen.add(k[0]); uniq.append(k)
    return uniq, pats

class Conn:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), 10)
        self.s.settimeout(120)
        self.f = self.s.makefile("rb")   # read-only: writes go through sendall
    def send(self, *args):
        out = [b"*%d\r\n" % len(args)]
        for a in args:
            a = a if isinstance(a, bytes) else str(a).encode()
            out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        self.s.sendall(b"".join(out))
    def reply(self):
        line = self.f.readline()
        t, rest = line[:1], line[1:-2]
        if t == b"+": return rest
        if t == b"-": raise RuntimeError(rest.decode())
        if t == b":": return int(rest)
        if t == b"$":
            n = int(rest)
            if n < 0: return None
            d = self.f.read(n + 2)
            return d[:-2]
        if t == b"*":
            n = int(rest)
            return None if n < 0 else [self.reply() for _ in range(n)]
        raise RuntimeError("bad reply %r" % line)
    def call(self, *args):
        self.send(*args)
        return self.reply()

def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] if v else 0.0

def main():
    cmd, host, port, ds = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    if cmd == "ping":                    # readiness, for the driver
        sys.exit(0 if Conn(host, port).call("PING") == b"PONG" else 1)
    keys, pats = dataset(ds)
    if cmd == "njwt":                    # pcbench -n: how many jwt keys
        print(sum(1 for k in keys if k[0].startswith(JWT_PFX)))
        return
    c = Conn(host, port)
    if cmd == "load":
        B = 1000
        for i in range(0, len(keys), B):
            for k, v, ttl in keys[i:i + B]:
                if ttl: c.send("SET", k, v, "EX", ttl)
                else:   c.send("SET", k, v)
            for _ in keys[i:i + B]:
                c.reply()
        n = c.call("DBSIZE")
        print("loaded %d keys, DBSIZE %d" % (len(keys), n))
        sys.exit(0 if n == len(keys) else 1)
    kind = sys.argv[5]
    plist = pats[kind]
    if cmd == "keys":
        calls, rate = int(sys.argv[6]), float(sys.argv[7])
        lat, matched = [], 0
        t0 = time.monotonic()
        for i in range(calls):
            if rate > 0:   # open loop: a slow call does not push the schedule
                due = t0 + i / rate
                d = due - time.monotonic()
                if d > 0: time.sleep(d)
            a = time.monotonic()
            r = c.call("KEYS", plist[i % len(plist)])
            lat.append((time.monotonic() - a) * 1000.0)
            matched += len(r)
        el = time.monotonic() - t0
        print("keys %s calls=%d rate=%g p50_ms=%.3f p99_ms=%.3f max_ms=%.3f matched_per_call=%.1f elapsed_s=%.2f" %
              (kind, calls, rate, pct(lat, 0.5), pct(lat, 0.99), max(lat), matched / calls, el))
    elif cmd == "scan":
        count, walks = int(sys.argv[6]), int(sys.argv[7])
        times, rts, found = [], [], 0
        for w in range(walks):
            p = plist[w % len(plist)]
            a = time.monotonic(); cur, n, got = b"0", 0, set()
            while True:
                cur, ks = c.call("SCAN", cur, "MATCH", p, "COUNT", count)
                n += 1; got.update(ks)
                if cur == b"0": break
            times.append((time.monotonic() - a) * 1000.0); rts.append(n)
            found += len(got)
            # the walk must find what KEYS finds - a fast scan that misses
            # keys would be the wrong answer quickly
            want = set(c.call("KEYS", p))
            if got != want:
                print("scan MISMATCH: %s found %d, KEYS %d" % (p, len(got), len(want)))
                sys.exit(1)
        print("scan %s count=%d walks=%d p50_ms=%.3f p99_ms=%.3f calls_per_walk=%d matched_per_walk=%.1f" %
              (kind, count, walks, pct(times, 0.5), pct(times, 0.99), int(sum(rts) / len(rts)), found / walks))
    else:
        sys.exit("keyscan: unknown command %s" % cmd)

if __name__ == "__main__":
    main()
