#!/bin/sh
# readbusytest.sh - a read through a non-owner whose pull or forward to the owner times out is refused
# as retryable (RESP -TRYAGAIN, native "cluster busy", libperfd PERFD_E_REFUSED), never answered absent;
# with the owner back the same reads answer the values.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
GB=${2:-./getbusy}
D=$(mktemp -d /var/tmp/pcreadbusy.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -CONT "$(cat "$f")" 2>/dev/null && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
if "$BIN" -V 2>/dev/null | grep -q standalone; then
	echo "  SKIP the standalone edition refuses a [cluster] section"
	echo "readbusytest: 0 passed, 0 failed, 1 skipped"; exit "${PC_SKIP_FAILS:-0}"
fi
[ -x "$GB" ] || { echo "readbusytest: $GB is missing (make getbusy)"; exit 1; }
for p in 28711 28712 28721 28722 28731 28732; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "readbusytest: port $p busy" >&2; exit 1; }
done

conf() {
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = rb-client-secret
cluster = rb-cluster-secret
[listen]
tcp = 127.0.0.1:2871$1
resp = 127.0.0.1:2872$1
http = 127.0.0.1:2873$1
plaintext = loopback
[cluster]
multicast = 239.255.97.36:28790
advertise = 127.0.236.$1
pull_timeout_ms = 300
[collection 0]
buckets_log2 = 10
mode = shard
C
	chmod 600 "$D/n$1.conf"
}
start() {
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 300 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' '|')"; return 1
}
st() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:2873%s/stats" % sys.argv[1],timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"; }

# reads: every read of the fill through node 1 at once - RESP, native and the binary GET (JSON native
# only: the RESP door serves no JSON on a key another node holds);
# prints one line per class: "<read> <values> <absent> <retryable> <other errors> [an other error]"
reads() {
	python3 - "$GB" <<'PY'
import json, socket, subprocess, sys
import pcnative
NS, NH, NJ = 40, 20, 20
# one connection per read, all sent before any answer is read: a door serves a connection's parked
# reads one after another, and the frozen owner is purged 6 s on
def enc(*a):
    out = b"*%d\r\n" % len(a)
    for x in a:
        x = x.encode() if isinstance(x, str) else x
        out += b"$%d\r\n%s\r\n" % (len(x), x)
    return out
def parse(f):
    h = f.readline(); t, b = h[:1], h[1:].rstrip(b"\r\n")
    if t == b"+": return ("ok", b)
    if t == b"-": return ("err", b.decode())
    if t == b":": return ("int", int(b))
    if t == b"$":
        n = int(b)
        return ("nil", None) if n < 0 else ("val", f.read(n + 2)[:-2])
    return ("bad", h)
bins = [subprocess.Popen([sys.argv[1], "127.0.0.1", "28711", "0", "s%02d" % i],
                         stdout=subprocess.PIPE, text=True) for i in range(10)]
cmds = [("GET", enc("GET", "s%02d" % i)) for i in range(NS)]
cmds += [("EXISTS", enc("EXISTS", "s%02d" % i)) for i in range(NS)]
cmds += [("HGET", enc("HGET", "h%02d" % i, "f")) for i in range(NH)]
rconns = []
for name, c in cmds:
    s = socket.create_connection(("127.0.0.1", 28721), 10); s.settimeout(20)
    s.sendall(c)
    rconns.append((name, s, s.makefile("rb")))
reqs = [("get", {"col": "0", "key": "s%02d" % i}) for i in range(NS)]
reqs += [("exists", {"col": "0", "key": "s%02d" % i}) for i in range(NS)]
reqs += [("jget", {"col": "0", "key": "j%02d" % i}) for i in range(NJ)]
nconns = []
for m, p in reqs:
    n = pcnative.wrap(socket.create_connection(("127.0.0.1", 28711), 10))
    n.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}) + "\n").encode())
    n.flush()
    nconns.append((m, n))
tally = {}
def note(name, cls, txt=""):
    t = tally.setdefault(name, [0, 0, 0, 0, ""])
    t[cls] += 1
    if cls == 3 and not t[4]:
        t[4] = txt
for name, s, f in rconns:
    k, v = parse(f)
    s.close()
    if k == "err":
        note("resp-" + name, 2 if v.startswith("TRYAGAIN") else 3, v)
    elif k == "nil" or (name == "EXISTS" and v == 0):
        note("resp-" + name, 1)
    else:
        note("resp-" + name, 0)
for m, n in nconns:
    r = json.loads(n.readline())
    e = r.get("error") or (r.get("result") or {}).get("error")
    res = r.get("result") or {}
    if e:
        msg = e.get("message", str(e)) if isinstance(e, dict) else str(e)
        note("native-" + m, 2 if "cluster busy" in msg else 3, msg)
    elif res.get("found") is False or res.get("exists") is False:
        note("native-" + m, 1)
    else:
        note("native-" + m, 0)
for b in bins:
    out = b.communicate(timeout=30)[0]
    for line in out.splitlines():
        w = line.split(None, 3)
        if len(w) >= 2 and w[1] == "hit": note("bin-get", 0)
        elif len(w) >= 2 and w[1] == "miss": note("bin-get", 1)
        elif len(w) >= 3 and w[1] == "err":
            note("bin-get", 2 if w[2] == "3" else 3, line)
        else: note("bin-get", 3, line)
for name in sorted(tally):
    t = tally[name]
    print(name, t[0], t[1], t[2], t[3], t[4][:100])
PY
}

conf 1; conf 2
start 1 || exit 1
start 2 || exit 1
i=0; while [ $i -lt 60 ] && [ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 1 'r["cluster"]["peers_up"]')$(st 2 'r["cluster"]["peers_up"]')" != readyready11 ]; do sleep 0.5; i=$((i+1)); done
[ "$(st 1 'r["state"]')$(st 2 'r["state"]')" = readyready ] && ok "the pair formed" || bad "the pair did not form"
sleep 2                                  # both use the same map before the fill
python3 - <<'PY' > "$D/fill.out"
import socket
s = socket.create_connection(("127.0.0.1", 28721), 10); s.settimeout(20)
f = s.makefile("rb")
def cmd(*a):
    out = b"*%d\r\n" % len(a)
    for x in a:
        x = x.encode()
        out += b"$%d\r\n%s\r\n" % (len(x), x)
    s.sendall(out)
    return f.readline()
bad = 0
for i in range(40):
    bad += not cmd("SET", "s%02d" % i, "v%02d" % i).startswith(b"+OK")
for i in range(20):
    bad += not cmd("HSET", "h%02d" % i, "f", "v%02d" % i).startswith(b":")
# JSON on a key another node holds is the native door's (the RESP door refuses it)
import json, pcnative
n = pcnative.wrap(socket.create_connection(("127.0.0.1", 28711), 10))
for i in range(20):
    n.write((json.dumps({"jsonrpc": "2.0", "id": i + 1, "method": "jset", "params": {"col": "0", "key": "j%02d" % i, "val": {"a": i}}}) + "\n").encode())
n.flush()
for i in range(20):
    r = json.loads(n.readline())
    bad += bool(r.get("error") or (r.get("result") or {}).get("error"))
print(bad)
PY
[ "$(cat "$D/fill.out")" = 0 ] && ok "40 strings and 20 hashes over RESP, 20 JSON documents natively, written through node 1" || bad "the fill: $(cat "$D/fill.out") write(s) refused"
reads > "$D/before.out" 2>&1
awk '$3 != 0 || $4 != 0 || $5 != 0 { b = 1 } END { exit b }' "$D/before.out" && ok "with both up every read through node 1 answers the value" \
	|| bad "reads with both up: $(tr '\n' '|' < "$D/before.out")"
N2=$(st 2 'r["collections"][0]["entries"]')
[ "${N2:-0}" -gt 0 ] 2>/dev/null && ok "node 2 owns $N2 of the 80 keys" || bad "node 2 owns ${N2} keys - the arm tests nothing"

# node 2 frozen past pull_timeout_ms: its keys cannot be read through node 1, and must not read absent
kill -STOP "$(cat "$D/n2.pid")"
sleep 0.5
reads > "$D/frozen.out" 2>&1
kill -CONT "$(cat "$D/n2.pid")"
while read -r name v a r o e; do
	if [ "$a" = 0 ] && [ "$o" = 0 ] && [ "$r" -gt 0 ]; then
		ok "owner frozen, $name: $v value(s), $r refused as retryable, none absent"
	else
		bad "owner frozen, $name: $v value(s), $a answered ABSENT, $r retryable, $o other error(s)${e:+ ($e)} - a timed-out pull or forward must not read as a miss"
	fi
done < "$D/frozen.out"
[ "$(wc -l < "$D/frozen.out")" -ge 7 ] || bad "the reads under the freeze did not all run: $(tr '\n' '|' < "$D/frozen.out")"

# thawed: the same reads answer the values again
i=0; while [ $i -lt 20 ]; do
	reads > "$D/after.out" 2>&1
	awk '$3 != 0 || $4 != 0 || $5 != 0 { b = 1 } END { exit b }' "$D/after.out" && break
	sleep 1; i=$((i+1))
done
awk '$3 != 0 || $4 != 0 || $5 != 0 { b = 1 } END { exit b }' "$D/after.out" && ok "node 2 thawed: every read answers the value again" \
	|| bad "after the thaw: $(tr '\n' '|' < "$D/after.out")"
echo "readbusytest: $pass passed, $fail failed"
[ $fail -eq 0 ]
