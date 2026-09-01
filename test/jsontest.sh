#!/bin/sh
# jsontest.sh — S26 verification: the JSON path verbs (jget/jset/jdel/
# jincr).  Documents are opaque text in ordinary cells; edits are span
# splices under a per-key mutex stripe.  Proven here:
#  - create via jset $, read back whole and by path (member, index,
#    subtree), missing-leaf vs missing-intermediate vs type errors;
#  - partial edit: replace a leaf, create a NEW member, nx/xx flags;
#  - jincr integer leaves (and the non-number refusal);
#  - jdel of members and array elements keeps the document valid,
#    absent leaves report deleted:false;
#  - TTL survives partial edits unless explicitly overridden;
#  - non-JSON stored values are refused per contract;
#  - the stripe lock: 200 racing jincrs from two connections land
#    EXACTLY 200 - no lost update.
# Usage: test/jsontest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcjs.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

cat > "$D/js.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = js-client-secret
cluster = js-cluster-secret
[listen]
tcp = 127.0.0.1:17021
plaintext = loopback
[collection js]
buckets_log2 = 10
EOF
"$BIN" -f "$D/js.conf" > "$D/js.log" 2>&1 &
P=$!
i=0
while [ $i -lt 60 ]; do
	grep -q "perfcached ready" "$D/js.log" && break
	sleep 0.1; i=$((i+1))
done

CLI="$D/cli.py"
cat > "$CLI" <<'EOF'
import json, socket, sys
s = socket.create_connection(("127.0.0.1", 17021), timeout=5)
f = s.makefile("rwb"); rid = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    rid += 1
    req = json.loads(line); req["id"] = rid; req["jsonrpc"] = "2.0"
    f.write(json.dumps(req).encode()+b"\n"); f.flush()
    r = json.loads(f.readline())
    print(json.dumps(r.get("result", r.get("error"))))
EOF
call() { echo "$1" | python3 "$CLI"; }

# ---- create and read ----------------------------------------------------
R=$(call '{"method":"jset","params":{"col":"js","key":"d1","val":{"a":1,"b":{"c":[1,2,3]},"s":"x"},"ttl":500}}')
echo "$R" | grep -q '"set": true' && ok || bad "jset root: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"d1"}}')
echo "$R" | grep -q '"a": 1' && ok || bad "jget root: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"d1","path":"$.b.c[1]"}}')
echo "$R" | grep -q '"value": 2' && ok || bad "index path: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"d1","path":"$.b.c"}}')
echo "$R" | grep -q '\[1, 2, 3\]' && ok || bad "subtree path: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"d1","path":"$.zz"}}')
echo "$R" | grep -q '"found": false' && ok || bad "missing leaf: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"d1","path":"$.zz.q"}}')
echo "$R" | grep -q 'intermediate' && ok || bad "missing intermediate: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"d1","path":"$.a[0]"}}')
echo "$R" | grep -q 'wrong type' && ok || bad "type error: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"nokey","path":"$.a"}}')
echo "$R" | grep -q '"found": false' && ok || bad "absent key jget: $R"

# ---- partial edits ------------------------------------------------------
R=$(call '{"method":"jset","params":{"col":"js","key":"d1","path":"$.a","val":42}}')
echo "$R" | grep -q '"set": true' && ok || bad "leaf replace: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"d1","path":"$.a"}}')
echo "$R" | grep -q '"value": 42' && ok || bad "leaf readback: $R"
R=$(call '{"method":"jset","params":{"col":"js","key":"d1","path":"$.new","val":{"k":"v"}}}')
echo "$R" | grep -q '"set": true' && ok || bad "member create: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"d1","path":"$.new.k"}}')
echo "$R" | grep -q '"value": "v"' && ok || bad "created member: $R"
R=$(call '{"method":"jset","params":{"col":"js","key":"d1","path":"$.a","val":9,"nx":true}}')
echo "$R" | grep -q 'exists' && ok || bad "nx on existing: $R"
R=$(call '{"method":"jset","params":{"col":"js","key":"d1","path":"$.nope","val":9,"xx":true}}')
echo "$R" | grep -q 'no value at path' && ok || bad "xx on missing: $R"
R=$(call '{"method":"jset","params":{"col":"js","key":"d1","path":"$.esc","val":"a\"b"}}')
echo "$R" | grep -q '"set": true' && ok || bad "escaped string set: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"d1","path":"$.esc"}}')
echo "$R" | grep -q '"a\\\\"b"\|a..b' && ok || bad "escaped readback: $R"

# ---- jincr --------------------------------------------------------------
R=$(call '{"method":"jincr","params":{"col":"js","key":"d1","path":"$.a","by":5}}')
echo "$R" | grep -q '"value": 47' && ok || bad "jincr by 5: $R"
R=$(call '{"method":"jincr","params":{"col":"js","key":"d1","path":"$.a"}}')
echo "$R" | grep -q '"value": 48' && ok || bad "jincr default: $R"
R=$(call '{"method":"jincr","params":{"col":"js","key":"d1","path":"$.s"}}')
echo "$R" | grep -q 'not an integer' && ok || bad "jincr non-number: $R"
R=$(call '{"method":"jincr","params":{"col":"js","key":"nokey","path":"$.a"}}')
echo "$R" | grep -q 'no such key' && ok || bad "jincr absent key: $R"

# ---- jdel ---------------------------------------------------------------
R=$(call '{"method":"jdel","params":{"col":"js","key":"d1","path":"$.b.c[0]"}}')
echo "$R" | grep -q '"deleted": true' && ok || bad "array elem del: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"d1","path":"$.b.c"}}')
echo "$R" | grep -q '\[ *2, 3\]\|\[2, 3\]' && ok || bad "array after del: $R"
R=$(call '{"method":"jdel","params":{"col":"js","key":"d1","path":"$.s"}}')
echo "$R" | grep -q '"deleted": true' && ok || bad "member del: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"d1","path":"$.s"}}')
echo "$R" | grep -q '"found": false' && ok || bad "member gone: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"d1"}}')
echo "$R" | grep -q '"b":' && ok || bad "doc still valid after dels: $R"
R=$(call '{"method":"jdel","params":{"col":"js","key":"d1","path":"$.s"}}')
echo "$R" | grep -q '"deleted": false' && ok || bad "absent leaf del: $R"
R=$(call '{"method":"jdel","params":{"col":"js","key":"nokey","path":"$.s"}}')
echo "$R" | grep -q '"deleted": false' && ok || bad "absent key del: $R"

# ---- jarrappend ---------------------------------------------------------
R=$(call '{"method":"jset","params":{"col":"js","key":"arr","val":{"l":[1,2]}}}')
echo "$R" | grep -q '"set": true' && ok || bad "arr seed: $R"
R=$(call '{"method":"jarrappend","params":{"col":"js","key":"arr","path":"$.l","val":3}}')
echo "$R" | grep -q '"count": 3' && ok || bad "append: $R"
R=$(call '{"method":"jarrappend","params":{"col":"js","key":"arr","path":"$.l","val":{"x":4}}}')
echo "$R" | grep -q '"count": 4' && ok || bad "append obj: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"arr","path":"$.l[3].x"}}')
echo "$R" | grep -q '"value": 4' && ok || bad "appended readable: $R"
R=$(call '{"method":"jarrappend","params":{"col":"js","key":"arr","path":"$.l[0]","val":9}}')
echo "$R" | grep -q 'wrong type' && ok || bad "append non-array: $R"

# ---- mkpath -------------------------------------------------------------
R=$(call '{"method":"jset","params":{"col":"js","key":"arr","path":"$.deep.er.est","val":7,"mkpath":true}}')
echo "$R" | grep -q '"set": true' && ok || bad "mkpath set: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"arr","path":"$.deep.er.est"}}')
echo "$R" | grep -q '"value": 7' && ok || bad "mkpath readback: $R"
R=$(call '{"method":"jget","params":{"col":"js","key":"arr"}}')
echo "$R" | grep -q '"deep"' && ok || bad "doc valid after mkpath: $R"
R=$(call '{"method":"jset","params":{"col":"js","key":"arr","path":"$.deep.er.other","val":8,"mkpath":true}}')
echo "$R" | grep -q '"set": true' && ok || bad "mkpath partial-existing: $R"
R=$(call '{"method":"jset","params":{"col":"js","key":"arr","path":"$.l[9].q","val":1,"mkpath":true}}')
echo "$R" | grep -q 'intermediate' && ok || bad "mkpath refuses index: $R"

# ---- ttl preserved on edits --------------------------------------------
R=$(call '{"method":"ttl","params":{"col":"js","key":"d1"}}')
T=$(echo "$R" | python3 -c 'import json,sys; print(json.load(sys.stdin)["ttl"])')
[ "$T" -gt 400 ] && [ "$T" -le 500 ] && ok || bad "ttl not preserved ($T)"
R=$(call '{"method":"jset","params":{"col":"js","key":"d1","path":"$.a","val":1,"ttl":900}}')
R=$(call '{"method":"ttl","params":{"col":"js","key":"d1"}}')
T=$(echo "$R" | python3 -c 'import json,sys; print(json.load(sys.stdin)["ttl"])')
[ "$T" -gt 800 ] && ok || bad "explicit ttl ignored ($T)"

# ---- non-JSON stored value refused -------------------------------------
call '{"method":"set","params":{"col":"js","key":"plain","value":"not json at all }{"}}' >/dev/null
R=$(call '{"method":"jget","params":{"col":"js","key":"plain","path":"$.a"}}')
echo "$R" | grep -q 'not JSON' && ok || bad "non-JSON doc: $R"

# ---- bad path / bad val -------------------------------------------------
R=$(call '{"method":"jget","params":{"col":"js","key":"d1","path":"a.b"}}')
echo "$R" | grep -q 'bad path' && ok || bad "bad path: $R"

# ---- the stripe lock: 200 racing jincrs, zero lost ---------------------
call '{"method":"jset","params":{"col":"js","key":"ctr","val":{"n":0}}}' >/dev/null
python3 - <<'EOF'
import json, socket, threading
def worker():
    s = socket.create_connection(("127.0.0.1", 17021), timeout=10)
    f = s.makefile("rwb")
    for i in range(100):
        f.write(json.dumps({"jsonrpc":"2.0","id":i,"method":"jincr",
            "params":{"col":"js","key":"ctr","path":"$.n"}}).encode()+b"\n")
        f.flush()
        json.loads(f.readline())
ts = [threading.Thread(target=worker) for _ in range(2)]
for t in ts: t.start()
for t in ts: t.join()
EOF
R=$(call '{"method":"jget","params":{"col":"js","key":"ctr","path":"$.n"}}')
echo "$R" | grep -q '"value": 200' && ok || bad "lost updates: $R"

kill -TERM $P 2>/dev/null; wait 2>/dev/null; P=
echo "jsontest: $pass passed, $fail failed"
[ $fail -eq 0 ]
