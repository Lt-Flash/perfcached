#!/bin/sh
# resizedeltest.sh - a delete that lands while the resize's copier has a record in hand is not undone:
# the copy is made under the source bucket's lock, and a delete empties the copier's source first.
# The copier is paused on one key before the swap and one after (PERFCACHED_TEST_RS_COPY_PAUSE),
# and a delete that resolved the old table before the swap is held until after it; a set held
# past the closing drain lands in the live table.
# Usage: test/resizedeltest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrzdel.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 21611 21613; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "resizedeltest: port $p busy" >&2; exit 1; }
done

mkdir -p "$D/s"
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
state_dir = $D/s
allow_create = yes
[memory]
arena_mb = 64
[secrets]
client = rzdel-client-secret
enable = rzdel-enable-secret
[listen]
tcp = 127.0.0.1:21611
http = 127.0.0.1:21613
plaintext = loopback
[collection 0]
buckets_log2 = 16
autoscale = off
CONF
chmod 600 "$D/n.conf"
touch "$D/pause.pre" "$D/pause.post"
: > "$D/n.log"
PERFCACHED_TEST_RS_COPY_PAUSE="$D/pause" "$BIN" -f "$D/n.conf" -D > "$D/n.log" 2>&1 & P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
if ! grep -q "perfcached ready" "$D/n.log"; then
	bad "the daemon did not start: $(tail -1 "$D/n.log")"
	echo "resizedeltest: $pass passed, $fail failed"; exit 1
fi

D="$D" timeout 240 python3 - > "$D/py.out" 2>&1 <<'PY'
import json, os, pcnative, socket, threading, time, urllib.request
D = os.environ["D"]
def conn():
    s = socket.create_connection(("127.0.0.1", 21611), timeout=30)
    return pcnative.wrap(s)
def call(f, m, **p):
    f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}) + "\n").encode()); f.flush()
    return json.loads(f.readline())
def col():
    r = json.load(urllib.request.urlopen("http://127.0.0.1:21613/stats", timeout=5))
    return [c for c in r["collections"] if c["name"] == "0"][0]
def paused(k, secs=20):
    end = time.time() + secs
    while time.time() < end:
        if ("resize copy paused on '%s'" % k) in open(D + "/n.log").read():
            return True
        time.sleep(0.05)
    return False
f = conn()
for k in ("pz:pre", "pz:post", "other"):
    call(f, "set", col="0", key=k, value="v-" + k)
call(f, "enable", secret="rzdel-enable-secret")
r = call(f, "resize", col="0", buckets_log2=12)
print("RESIZE", json.dumps(r.get("result") or r.get("error")))
for k, ph in (("pz:pre", "before the swap"), ("pz:post", "after the swap")):
    if not paused(k):
        print("FAIL the copier never paused on %s (%s): %s" % (k, ph, "no hook line"))
        os.unlink(D + "/pause." + k[3:])
        continue
    if k == "pz:post":
        # held after the swap, pz:post is in the old table only: a dump walked to its end
        # still carries it, from the walker at bucket 0
        seen, cur, n = set(), 0, 0
        while n < 200:
            r = call(f, "dump", col="0", cursor=cur, count=4096)
            res = r.get("result") or {}
            if not res:
                print("FAIL dump during the drain: %s" % json.dumps(r)[:200])
                break
            seen.update(x.get("k") for x in res.get("records", []))
            cur, n = res.get("cursor", 0), n + 1
            if not res.get("more"):
                break
        print(("ok  " if "pz:post" in seen and "other" in seen else "FAIL") +
              " a dump while the resize drains carries the record only its old table holds (saw %s)" % sorted(seen))
    got = {}
    def delete():
        g = conn()
        got["r"] = call(g, "del", col="0", key=k)
    t = threading.Thread(target=delete); t.start()
    time.sleep(0.3)                    # the delete is in, or waits on the copier
    os.unlink(D + "/pause." + k[3:])
    t.join(15)
    print("DEL %s %s" % (k, json.dumps(got.get("r"))))
end = time.time() + 30
while time.time() < end and (col().get("buckets") != 4096 or col().get("resizing_to")):
    time.sleep(0.1)
c = col()
print("SETTLED %s %s" % (c.get("buckets"), c.get("resizing_to")))
for k, ph in (("pz:pre", "before the swap"), ("pz:post", "after the swap")):
    v = (call(f, "get", col="0", key=k).get("result") or {}).get("value")
    print(("ok  " if v is None else "FAIL") + " %s, deleted while the copier held it %s, reads absent once the resize is done (read %r)" % (k, ph, v))
v = (call(f, "get", col="0", key="other").get("result") or {}).get("value")
print(("ok  " if v == "v-other" else "FAIL") + " a key nobody deleted came through the resize (read %r)" % v)
# a delete that resolved the old table before the swap and lands after it: the copy the
# copier made into the new table goes too
for k in ("pz:pre", "pz:stale"):
    call(f, "set", col="0", key=k, value="v-" + k)
for ph in ("pre", "del"):
    open(D + "/pause." + ph, "w").close()
r = call(f, "resize", col="0", buckets_log2=13)
print("RESIZE2", json.dumps(r.get("result") or r.get("error")))
got = {}
def stale():
    g = conn()
    got["r"] = call(g, "del", col="0", key="pz:stale")
def logged(t, secs=30):
    end = time.time() + secs
    while time.time() < end:
        if t in open(D + "/n.log").read():
            return True
        time.sleep(0.05)
    return False
held = paused("pz:pre")
t = threading.Thread(target=stale); t.start()
dp = logged("delete paused on 'pz:stale'")
os.unlink(D + "/pause.pre")
sw = logged("resized to 2^13 buckets")
os.unlink(D + "/pause.del")
t.join(40)
print("DEL2 %s held %s paused %s swapped %s" % (json.dumps(got.get("r")), held, dp, sw))
if not (held and dp and sw):
    print("FAIL the stale delete was not staged across the swap (copier paused %s, delete paused %s, swap logged %s)" % (held, dp, sw))
end = time.time() + 30
while time.time() < end and (col().get("buckets") != 8192 or col().get("resizing_to")):
    time.sleep(0.1)
c = col()
print("SETTLED2 %s %s" % (c.get("buckets"), c.get("resizing_to")))
v = (call(f, "get", col="0", key="pz:stale").get("result") or {}).get("value")
print(("ok  " if v is None else "FAIL") + " pz:stale, deleted through the table resolved before the swap, reads absent after it (read %r)" % v)
r2 = (got.get("r") or {}).get("result") or {}
print(("ok  " if (r2.get("deleted") or r2.get("removed")) else "FAIL") + " that delete was acknowledged (%s)" % json.dumps(got.get("r")))
# a SET that resolved the old table before the swap and lands after the closing drain: it reaches
# the live table, not the emptied old one about to be retired
call(f, "set", col="0", key="pz:pre", value="v-pz:pre")
call(f, "set", col="0", key="pz:late", value="v-old")
for ph in ("pre", "set"):
    open(D + "/pause." + ph, "w").close()
r = call(f, "resize", col="0", buckets_log2=14)
print("RESIZE3", json.dumps(r.get("result") or r.get("error")))
got3 = {}
def late():
    g = conn()
    got3["r"] = call(g, "set", col="0", key="pz:late", value="v-new")
held = paused("pz:pre")
t = threading.Thread(target=late); t.start()
sp = logged("set paused on 'pz:late'")
os.unlink(D + "/pause.pre")
done3 = logged("resize complete at 2^14")
os.unlink(D + "/pause.set")
t.join(40)
print("SET3 %s held %s paused %s closed %s" % (json.dumps(got3.get("r")), held, sp, done3))
if not (held and sp and done3):
    print("FAIL the late set was not staged across the closing drain (copier paused %s, set paused %s, resize closed %s)" % (held, sp, done3))
v = (call(f, "get", col="0", key="pz:late").get("result") or {}).get("value")
print(("ok  " if v == "v-new" else "FAIL") + " pz:late, set through the table resolved before the swap and applied after the closing drain, reads the new value (read %r)" % v)
PY
grep -E "^RESIZE|^DEL|^SETTLED|^SET3" "$D/py.out" | sed 's/^/  # /'
grep -q "^RESIZE.*resizing" "$D/py.out" && ok "a shrink to 2^12 accepted" || bad "the resize: $(grep -m1 '^RESIZE' "$D/py.out") $(tail -2 "$D/py.out")"
grep -q "^SETTLED 4096 None" "$D/py.out" && ok "the resize finished" || bad "the resize did not finish: $(grep '^SETTLED ' "$D/py.out")"
grep -q "^SETTLED2 8192 None" "$D/py.out" && ok "the second resize finished" || bad "the second resize did not finish: $(grep '^SETTLED2' "$D/py.out")"
while IFS= read -r l; do
	case "$l" in
		"ok  "*) ok "${l#ok  }" ;;
		"FAIL "*) bad "${l#FAIL }" ;;
	esac
done < "$D/py.out"
grep -qE "^(ok  |FAIL )" "$D/py.out" || bad "the python driver did not finish: $(tail -3 "$D/py.out")"
echo "resizedeltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
