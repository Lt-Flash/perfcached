#!/usr/bin/env python3
# stallprobe.py — the KEYS/SCAN outage-mechanism probe.
#
# One connection samples GET and SET round-trips at ~1ms cadence; two
# seconds in, a second connection fires the storm (KEYS or a full SCAN
# iteration).  If the server executes the storm on its only execution
# thread (Redis), every sampled op during it stalls for the storm's
# whole duration - the production 40s outage in miniature.  A server
# that walks lock-free on one worker (perfcached) must stay flat.
#
# usage: stallprobe.py resp|perf <host> <port> <col> keys|scan [nkeys]
# The keyspace (k%08u, filled beforehand) is sampled uniformly.
import json, random, socket, sys, time

proto, host, port, col, storm = sys.argv[1], sys.argv[2], int(sys.argv[3]), \
    sys.argv[4], sys.argv[5]
nkeys = int(sys.argv[6]) if len(sys.argv) > 6 else 200000

def connect():
    s = socket.create_connection((host, port), timeout=60)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s

# ---- minimal dual-protocol client ---------------------------------------

class Resp:
    def __init__(self):
        self.s = connect(); self.f = self.s.makefile("rwb")
    def cmd(self, *args):
        out = b"*%d\r\n" % len(args)
        for a in args:
            if isinstance(a, str): a = a.encode()
            out += b"$%d\r\n%s\r\n" % (len(a), a)
        self.f.write(out); self.f.flush()
        return self.reply()
    def reply(self):
        line = self.f.readline()
        t, rest = line[:1], line[1:-2]
        if t == b"+": return rest
        if t == b":": return int(rest)
        if t == b"-": raise RuntimeError(rest.decode())
        if t == b"$":
            n = int(rest)
            if n < 0: return None
            d = self.f.read(n + 2); return d[:-2]
        if t == b"*":
            n = int(rest)
            return [self.reply() for _ in range(n)]
        raise RuntimeError("bad reply %r" % line)
    def get(self, k): return self.cmd("GET", k)
    def set(self, k, v): return self.cmd("SET", k, v)
    def storm_keys(self):
        # drain the multi-bulk reply RAW, counting newlines: building
        # 200k python objects through 400k readline calls once billed
        # ~10s of CLIENT time to the storm window (keys carry no
        # newlines, so 2 lines per element is exact)
        self.f.write(b"*2\r\n$4\r\nKEYS\r\n$1\r\n*\r\n"); self.f.flush()
        hdr = self.f.readline()
        if hdr[:1] != b"*":
            raise RuntimeError("bad KEYS reply %r" % hdr)
        want = int(hdr[1:-2])
        seen = 0
        while seen < want * 2:
            chunk = self.f.read1(1 << 20)
            if not chunk:
                raise RuntimeError("eof in KEYS reply")
            seen += chunk.count(b"\n")
        return want
    def storm_scan(self):
        total = 0; cur = "0"
        while True:
            r = self.cmd("SCAN", cur, "COUNT", "1000")
            cur = r[0].decode(); total += len(r[1])
            if cur == "0": return total

class Perf:
    def __init__(self):
        self.s = connect(); self.f = self.s.makefile("rwb"); self.n = 0
    def rpc(self, method, params):
        self.n += 1
        self.f.write(json.dumps({"jsonrpc": "2.0", "id": self.n,
            "method": method, "params": params}).encode() + b"\n")
        self.f.flush()
        r = json.loads(self.f.readline())
        if "error" in r: raise RuntimeError(r["error"])
        return r["result"]
    def get(self, k): return self.rpc("get", {"col": col, "key": k})
    def set(self, k, v):
        return self.rpc("set", {"col": col, "key": k, "value": v})
    def storm_keys(self):
        # the daemon CAPS one keys reply at 100k (a bounded-reply guard
        # Redis lacks); "fullwalk" storms the whole table via a
        # no-match pattern - the complete O(N) walk, empty reply
        if storm == "fullwalk":
            return len(self.rpc("keys", {"col": col,
                "match": "no-such-prefix*", "limit": 100000})["keys"])
        # a 100k listing also exceeds the 1MB reply cap (bounded
        # replies by design; SCAN is the enumeration path) - the
        # capped storm lists 50k
        return len(self.rpc("keys", {"col": col,
            "limit": min(nkeys * 2, 50000)})["keys"])
    def storm_scan(self):
        total = 0; cur = 0
        while True:
            r = self.rpc("scan", {"col": col, "cursor": cur,
                "count": 1000})
            cur = r.get("cursor", 0); total += len(r.get("items", []))
            if cur == 0: return total

C = Resp if proto == "resp" else Perf

# ---- the storm PROCESS ---------------------------------------------------
# A separate process, not a thread: the storm's reply parsing must not
# share the sampler's GIL, or parse work inflates the sampled RTTs and
# the numbers stop being about the server.

def run_storm(q):
    # Every exit path must put SOMETHING on the queue.  The parent waits
    # on q.get(), so a storm that raises - a server that refuses the
    # verb, a closed socket - is indistinguishable from a storm still
    # running: one probe sat blocked for seven hours on zero seconds of
    # CPU because KEYS came back "-ERR reply too large" and the
    # traceback went to a pipe nobody was reading.
    try:
        c = C()
        time.sleep(2.0)
        t0 = time.monotonic()
        n = c.storm_scan() if storm == "scan" else c.storm_keys()
        t1 = time.monotonic()
        q.put((t0, t1, n))
    except BaseException as e:
        q.put(("ERROR", "%s: %s" % (type(e).__name__, e), 0))

import multiprocessing
import queue
# explicit fork context: python 3.14 defaults to forkserver, which
# re-imports this guard-less script and recurses.  Linux-only tool,
# no threads are alive at fork time - fork is the right method.
mp = multiprocessing.get_context("fork")
mq = mp.Queue()
st = mp.Process(target=run_storm, args=(mq,))
st.start()

# ---- the sampler ---------------------------------------------------------

c = C()
val = "V" * 256
samples = []                            # (t, rtt_us, kind)
end = time.monotonic() + 6.0
i = 0
while time.monotonic() < end:
    k = "k%08u" % random.randrange(nkeys)
    kind = "set" if (i & 7) == 7 else "get"   # 1 in 8 ops writes
    t0 = time.monotonic()
    (c.set(k, val) if kind == "set" else c.get(k))
    t1 = time.monotonic()
    samples.append((t0, (t1 - t0) * 1e6, kind))
    i += 1
    time.sleep(0.0005)
try:
    storm_t0, storm_t1, storm_n = mq.get(timeout=120)
except queue.Empty:
    sys.exit("stallprobe: the storm produced no result in 120s - giving up "
             "rather than blocking forever")
if storm_t0 == "ERROR":
    sys.exit("stallprobe: the storm FAILED, no measurement taken: %s" % storm_t1)
st.join()

def pct(v, p):
    return sorted(v)[min(len(v) - 1, int(len(v) * p))] if v else 0
def bucket(pred):
    return [r for (t, r, k) in samples if pred(t)]

pre = bucket(lambda t: t < storm_t0)
dur = bucket(lambda t: storm_t0 <= t <= storm_t1)
post = bucket(lambda t: t > storm_t1)
stalls10 = sum(1 for r in dur if r > 10000)
stalls1 = sum(1 for r in dur if r > 1000)
print("%s %s over %d keys: storm took %.1f ms, returned %d" % (
    proto, storm.upper(), nkeys, (storm_t1 - storm_t0) * 1e3, storm_n))
print("  sampler baseline : p50 %5.0f us  p99 %6.0f us  (%d ops)" % (
    pct(pre, .5), pct(pre, .99), len(pre)))
print("  DURING the storm : p50 %5.0f us  p99 %6.0f us  max %8.0f us  "
    "(%d ops; >1ms: %d, >10ms: %d)" % (pct(dur, .5), pct(dur, .99),
    max(dur) if dur else 0, len(dur), stalls1, stalls10))
print("  after            : p50 %5.0f us  (%d ops)" % (
    pct(post, .5), len(post)))
