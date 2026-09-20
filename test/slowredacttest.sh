#!/bin/sh
# slowredacttest.sh - a credential never reaches the slow log.
#
# The slow ring keeps a command's first arguments, and three things read
# it: SLOWLOG GET, /stats ("slowlog") and the page's slow log card, whose
# hover shows the whole command line.  An AUTH slower than the threshold
# - a SET on the 245-247 fleet took 10.1 ms in the first burst after a
# restart, and AUTH runs in the same bursts - stored the RESP password in
# all three.  The ring now stores "(redacted)" for every argument of AUTH
# and for the two after an AUTH inside HELLO.
#
# slowlog_usec = 0 logs every command, so each case below is certainly
# IN the ring; the suite then asserts both halves, because an absent
# password proves nothing if the entry was never written:
#   - the AUTH and HELLO entries exist, with "(redacted)" where the
#     secrets were and the argument count unchanged;
#   - the secrets appear nowhere in SLOWLOG GET's raw reply, in /stats,
#     or in the daemon's log;
#   - an ordinary command's arguments are still recorded as sent.
# Usage: test/slowredacttest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcsr.XXXXXX)
P1=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; rm -rf "$D"' EXIT TERM INT

PW=sr-resp-password-7c41e9
cat > "$D/a.conf" <<EOF
[daemon]
workers = 2
log_level = info
slowlog_usec = 0
[memory]
arena_mb = 32
[secrets]
client = sr-client-secret
cluster = sr-cluster-secret
resp = $PW
[listen]
tcp = 127.0.0.1:17696
resp = 127.0.0.1:17697
http = 127.0.0.1:17698
plaintext = loopback
[collection 0]
buckets_log2 = 12
EOF
chmod 600 "$D/a.conf"

"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P1=$!
i=0
while [ $i -lt 80 ]; do
	grep -q "perfcached ready" "$D/a.log" && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/a.log" || { echo "daemon did not start"; cat "$D/a.log"; exit 1; }

python3 - "$D" "$PW" <<'PYEOF'
import socket, sys

D, PW = sys.argv[1], sys.argv[2].encode()
WRONG = b"sr-mistyped-password-3b0d"      # a typo is someone's password too
USER1 = b"sr-acl-user-5e2a"               # AUTH <user> <pw>
USER2 = b"sr-hello-user-91cf"             # HELLO 2 AUTH <user> <pw>
USER3 = b"sr-hello-user-noversion-44d8"   # HELLO AUTH <user> <pw>
SECRETS = [PW, WRONG, USER1, USER2, USER3]
R = b"(redacted)"
pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)

s = socket.create_connection(("127.0.0.1", 17697), 8)
s.settimeout(10)
f = s.makefile("rwb")
def cmd(args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out
def parse():
    h = f.readline()
    t, body = h[:1], h[1:].rstrip(b"\r\n")
    if t == b":": return int(body)
    if t in (b"+", b"-"): return t + body
    if t == b"$":
        n = int(body)
        return None if n < 0 else f.read(n + 2)[:-2]
    if t == b"*":
        return [parse() for _ in range(int(body))]
    raise ValueError(h)
def call(args):
    s.sendall(cmd(args)); f.flush()
    return parse()

r = call([b"AUTH", PW])
(ok if r == b"+OK" else bad)("AUTH <password> answered %r" % (r,))
r = call([b"AUTH", USER1, PW])
(ok if r == b"+OK" else bad)("AUTH <user> <password> answered %r" % (r,))
r = call([b"AUTH", WRONG])
(ok if isinstance(r, bytes) and r.startswith(b"-WRONGPASS") else bad)("a wrong password is refused: %r" % (r,))
r = call([b"auth", PW])
(ok if r == b"+OK" else bad)("lower-case auth answered %r" % (r,))
r = call([b"HELLO", b"2", b"AUTH", USER2, PW])
(ok if isinstance(r, list) else bad)("HELLO 2 AUTH <user> <password> answered the HELLO map")
r = call([b"HELLO", b"AUTH", USER3, PW])
(ok if isinstance(r, bytes) and r.startswith(b"-") else bad)("HELLO without a version is refused: %r" % (r,))
r = call([b"SET", b"sr-plain-key", b"sr-plain-value"])
(ok if r == b"+OK" else bad)("SET answered %r" % (r,))

# ---- SLOWLOG GET: the raw reply, then its entries ----
ents = call([b"SLOWLOG", b"GET", b"128"])
flat = []
def walk(x):
    if isinstance(x, list):
        for y in x: walk(y)
    elif isinstance(x, bytes):
        flat.append(x)
walk(ents)
blob = b"\n".join(flat)
leaks = [x for x in SECRETS if x in blob]
(ok if not leaks else bad)("SLOWLOG GET holds none of the five secrets" + ("" if not leaks else ": found %r" % leaks))
argvs = [e[3] for e in ents if isinstance(e, list) and len(e) == 6]
def has(want):
    return any(a == want for a in argvs)
(ok if has([b"AUTH", R]) else bad)("AUTH <password> is in the log as [AUTH, (redacted)]")
(ok if has([b"AUTH", R, R]) else bad)("AUTH <user> <password> is [AUTH, (redacted), (redacted)] - the count kept")
(ok if sum(1 for a in argvs if a == [b"AUTH", R]) >= 2 else bad)("the refused AUTH is redacted too")
(ok if has([b"auth", R]) else bad)("lower-case auth is redacted, the verb kept as sent")
(ok if has([b"HELLO", b"2", b"AUTH", R]) else bad)("HELLO 2 AUTH <user> ...: the user redacted, the version and AUTH kept")
(ok if has([b"HELLO", b"AUTH", R, R]) else bad)("HELLO AUTH <user> <password>: both redacted without a version")
(ok if has([b"SET", b"sr-plain-key", b"sr-plain-value"]) else bad)("an ordinary command's arguments are still recorded as sent")

# ---- /stats: the JSON the page renders from ----
h = socket.create_connection(("127.0.0.1", 17698), 8)
h.settimeout(10)
h.sendall(b"GET /stats HTTP/1.0\r\nHost: x\r\n\r\n")
body = b""
while True:
    d = h.recv(65536)
    if not d: break
    body += d
leaks = [x for x in SECRETS if x in body]
(ok if b"200" in body.split(b"\r\n", 1)[0] and not leaks else bad)("/stats holds none of the secrets" + ("" if not leaks else ": found %r" % leaks))
(ok if b'"(redacted)"' in body and b'"sr-plain-value"' in body else bad)("/stats carries the redacted entries and the ordinary one")

log = open(D + "/a.log", "rb").read()
leaks = [x for x in SECRETS if x in log]
(ok if not leaks else bad)("the daemon's log holds none of the secrets" + ("" if not leaks else ": found %r" % leaks))

print("slowredacttest: %d passed, %d failed" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
PYEOF
rc=$?
exit $rc
