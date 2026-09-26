#!/bin/sh
# jdeltest.sh - S201: a native-door jdel at the ROOT removes the key.
#
# Found 2026-09-20 gating cachedb_perfd for the staging RGSs: their
# de-registration is `JSON.DEL <key>`, and on the native door the record
# survived - every root jdel answered -32022 "bad path" and left the key,
# on a fresh document, an edited one, an implicit path and a plain value
# alike - while the RESP door's JSON.DEL removed it.  The native door
# sent a root delete through pc_jp_del, which cannot delete the root;
# the RESP door had special-cased `$` into a key removal all along.
#
# Both doors on one daemon, so the two are compared in one run.
# Usage: test/jdeltest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcjd.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT TERM INT

cat > "$D/a.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = jd-client-secret
[listen]
tcp = 127.0.0.1:17926
resp = 127.0.0.1:17924
http = 127.0.0.1:17925
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 600 "$D/a.conf"
"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/a.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/a.log" || { echo "daemon did not start"; cat "$D/a.log"; exit 1; }

python3 - <<'PY_EOF'
import json, socket, sys

npass = nfail = 0
def ok(m):
    global npass; npass += 1; print("  ok   " + m)
def bad(m):
    global nfail; nfail += 1; print("  FAIL " + m)

s = socket.create_connection(("127.0.0.1", 17926), 8); s.settimeout(10)
f = s.makefile("rwb"); rid = [0]
def req(method, params):
    rid[0] += 1
    f.write((json.dumps({"jsonrpc": "2.0", "id": rid[0], "method": method,
                         "params": params}) + "\n").encode()); f.flush()
    r = json.loads(f.readline())
    return r["result"] if "result" in r else {"error": r.get("error")}
def exists(k):
    return req("get", {"col": "0", "key": k}).get("found") is True

r = socket.create_connection(("127.0.0.1", 17924), 8); r.settimeout(10)
rf = r.makefile("rwb")
def resp(*a):
    rf.write(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
    rf.flush()
    line = rf.readline()
    if line[:1] == b"$":
        n = int(line[1:]); return rf.read(n + 2)[:n].decode() if n >= 0 else None
    return line.strip().decode()

doc = {"status": "Registered", "ua": "Probe 1.0"}

# A: fresh root document -> jdel $ (the brief's case A)
req("jset", {"col": "0", "key": "pa", "path": "$", "val": doc})
x = req("jdel", {"col": "0", "key": "pa", "path": "$"})
(ok if x == {"deleted": True} else bad)("root jdel on a fresh document answers deleted:true (%r)" % x)
(ok if not exists("pa") else bad)("and the key is gone")

# A2: the implicit root (the driver sends no path for JSON.DEL key)
req("jset", {"col": "0", "key": "pa2", "path": "$", "val": doc})
x = req("jdel", {"col": "0", "key": "pa2"})
(ok if x == {"deleted": True} and not exists("pa2") else bad)(
    "jdel with no path is the root, and removes the key (%r)" % x)

# B: an edited document -> jdel $ (case B)
req("jset", {"col": "0", "key": "pb", "path": "$", "val": doc})
req("jset", {"col": "0", "key": "pb", "path": "$.status", "val": "Unreachable"})
x = req("jdel", {"col": "0", "key": "pb", "path": "$"})
(ok if x == {"deleted": True} and not exists("pb") else bad)(
    "root jdel after a partial-path edit removes the key (%r)" % x)

# C: a TTL on the key changes nothing (case C)
req("jset", {"col": "0", "key": "pc", "path": "$", "val": doc})
req("expire", {"col": "0", "key": "pc", "ttl": 300})
x = req("jdel", {"col": "0", "key": "pc", "path": "$"})
(ok if x == {"deleted": True} and not exists("pc") else bad)(
    "root jdel on a key with a TTL removes it (%r)" % x)

# a plain (non-JSON) value is a key like any other at the root
req("set", {"col": "0", "key": "pp", "value": "hello"})
x = req("jdel", {"col": "0", "key": "pp", "path": "$"})
(ok if x == {"deleted": True} and not exists("pp") else bad)(
    "root jdel on a plain value removes the key, as DEL would (%r)" % x)

# absent key: deleted:false, no error - as the native del and RESP JSON.DEL
x = req("jdel", {"col": "0", "key": "nope", "path": "$"})
(ok if x == {"deleted": False} else bad)("root jdel on an absent key is deleted:false, not an error (%r)" % x)

# a sub-path jdel is still a document EDIT, not a removal
req("jset", {"col": "0", "key": "ps", "path": "$", "val": doc})
x = req("jdel", {"col": "0", "key": "ps", "path": "$.ua"})
g = req("get", {"col": "0", "key": "ps"})
(ok if x == {"deleted": True} and g.get("found") and "ua" not in json.loads(g["value"])
    and json.loads(g["value"]).get("status") == "Registered" else bad)(
    "a sub-path jdel edits the document and keeps the key (%r, %s)" % (x, g.get("value")))

# cross-door: the RESP door's JSON.DEL and the native root jdel agree
req("jset", {"col": "0", "key": "px", "path": "$", "val": doc})
(ok if resp("JSON.DEL", "px") == ":1" and not exists("px") else bad)(
    "RESP JSON.DEL key removes a natively written document")
resp("JSON.SET", "py", "$", json.dumps(doc))
x = req("jdel", {"col": "0", "key": "py", "path": "$"})
(ok if x == {"deleted": True} and resp("EXISTS", "py") == ":0" else bad)(
    "native root jdel removes a RESP-written document, and RESP sees it gone (%r)" % x)

print("jdeltest: %d passed, %d failed" % (npass, nfail))
sys.exit(1 if nfail else 0)
PY_EOF
rc=$?
echo "jdeltest: done (rc=$rc)"
exit $rc
