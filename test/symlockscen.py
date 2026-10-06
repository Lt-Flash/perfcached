"""symlockscen.py <resp-port> - the Symfony lock scenario of
test/symlocktest.sh: every step through EVALSHA of the approved scripts,
one line per step, "<step> -> <reply>".  The same steps were run against
redis 8.10.2 with Symfony's bodies loaded (2026-10-05); symlocktest.sh
holds those answers and compares.  One change for perfcached: a key's own
expiry is whole seconds here (members keep ms), so the "key gone" check
after a 300 ms lock waits 1.3 s, not 0.5 s - the member check stays at 0.5."""
import socket, sys, time
from symfony_lock import SHA
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=10); f = s.makefile("rb")
def cmd(*a):
    a = [x if isinstance(x, bytes) else str(x).encode() for x in a]
    s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
    return rd()
def rd():
    l = f.readline().rstrip(b"\r\n"); t, v = l[:1], l[1:].decode()
    if t == b"$": return "nil" if v == "-1" else f.read(int(v) + 2)[:-2].decode()
    if t == b"*": return "[" + ",".join(rd() for _ in range(int(v))) + "]"
    if t == b"-": return "-" + v
    return v
def ev(name, key, *args): return cmd("EVALSHA", SHA[name], 1, key, *args)
mt = lambda: "%.4f" % time.time()
def step(name, val): print("%-40s -> %s" % (name, val)); sys.stdout.flush()
step("probe", ev("probe", "symfony_check_support_time"))
step("exists, absent", ev("exists", "L", mt(), "tokA"))
step("save write tokA 300 s", ev("save", "L", mt(), "tokA", 300000))
p = int(cmd("PTTL", "L")); step("  pttl in (299000,300000]", 299000 < p <= 300000)
step("  TYPE", cmd("TYPE", "L"))
step("save write tokB (held)", ev("save", "L", mt(), "tokB", 300000))
step("save write tokA again (own)", ev("save", "L", mt(), "tokA", 300000))
step("exists tokA", ev("exists", "L", mt(), "tokA"))
step("exists tokB", ev("exists", "L", mt(), "tokB"))
step("refresh tokA 600 s", ev("refresh", "L", mt(), "tokA", 600000))
p = int(cmd("PTTL", "L")); step("  pttl in (599000,600000]", 599000 < p <= 600000)
step("refresh tokB", ev("refresh", "L", mt(), "tokB", 600000))
step("release tokB", ev("release", "L", "tokB"))
step("release tokA", ev("release", "L", "tokA"))
step("  EXISTS", cmd("EXISTS", "L"))
cmd("SET", "S", "x")
step("string key: save", ev("save", "S", mt(), "tokA", 1000))
step("string key: exists", ev("exists", "S", mt(), "tokA"))
step("string key: refresh", ev("refresh", "S", mt(), "tokA", 1000))
step("string key: release", ev("release", "S", "tokA"))
step("  string kept", cmd("GET", "S"))
step("read r1", ev("read", "R", mt(), "r1", 300000))
step("read r2", ev("read", "R", mt(), "r2", 200000))
step("write w over readers", ev("save", "R", mt(), "w", 300000))
step("release r1", ev("release", "R", "r1"))
p = int(cmd("PTTL", "R")); step("  pttl now r2's (199000,200000]", 199000 < p <= 200000)
step("promote r2 to write (sole reader)", ev("save", "R", mt(), "r2", 300000))
step("read r3 under a write", ev("read", "R", mt(), "r3", 300000))
step("r2 reads again (demote)", ev("read", "R", mt(), "r2", 300000))
step("read r3 after the demote", ev("read", "R", mt(), "r3", 300000))
step("release r2", ev("release", "R", "r2"))
step("release r3", ev("release", "R", "r3"))
step("  EXISTS", cmd("EXISTS", "R"))
step("short write e1 300 ms", ev("save", "E", mt(), "e1", 300))
time.sleep(0.5)
step("  after 0.5 s: exists e1", ev("exists", "E", mt(), "e1"))
time.sleep(0.8)
step("  after 1.3 s: EXISTS", cmd("EXISTS", "E"))
step("  after 1.3 s: save e2", ev("save", "E", mt(), "e2", 300000))
step("argv variant: save t1", ev("save_argv", "V", mt(), "t1", 300000))
step("argv variant: exists t1", ev("exists_argv", "V", mt(), "t1"))
cmd("HSET", "H", "f", "v")
step("hash key: save", ev("save", "H", mt(), "tokA", 1000)[:10])
step("EVALSHA unknown", cmd("EVALSHA", "0" * 40, 1, "k")[:9])
