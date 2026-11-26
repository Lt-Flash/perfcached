#!/bin/sh
# walsynctest.sh - [wal] fsync = sync: a write's reply leaves only once its record is synced.  With the
# WAL's fdatasync held (syncfailshim walhold) a SET's OK waits on both doors and pipelined replies keep
# their order, another connection's read is not held, and the OK comes once the hold ends; an OK then
# kill -9 keeps the write.  fsync = always answers during the hold (the control).
# Usage: test/walsynctest.sh [./perfcached] [./syncfailshim.so]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
SHIM=${2:-./syncfailshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
[ -f "$SHIM" ] || { echo "walsynctest: $SHIM is missing (make syncfailshim.so)" >&2; exit 1; }
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
TCP=23911 RESP=23912 HTTP=23913
D=
trap '[ -n "$D" ] && { [ -f "$D/n.pid" ] && kill -9 "$(cat "$D/n.pid")" 2>/dev/null; rm -rf "$D"; }' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "walsynctest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcws.XXXXXX)
mkdir -p "$D/wal" "$D/state"

conf() { # conf <fsync>
	cat > "$D/n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = ws-client-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 10
[wal]
dir = $D/wal
probe = no
fsync = $1
segment_mb = 8
segments = 4
save = off
C
	chmod 600 "$D/n.conf"
}
start() { # under the shim; its classes armed by $D/ctl, its real fsync kept ahead of PC_NOSYNC's
	: > "$D/n.log"
	LD_PRELOAD="${SANRT:+$SANRT }$SHIM${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" PC_SYNCFAIL_CTL="$D/ctl" \
		PC_SYNCFAIL_LOG="$D/sync.log" "$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
	echo $! > "$D/n.pid"
	i=0; while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n.log" && return 0
		kill -0 "$(cat "$D/n.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "  the node did not start: $(tail -2 "$D/n.log" | tr '\n' ' ')"
	return 1
}
stop9() { kill -9 "$(cat "$D/n.pid")" 2>/dev/null; wait "$(cat "$D/n.pid")" 2>/dev/null; rm -f "$D/n.pid"; }
stopc() { kill "$(cat "$D/n.pid")" 2>/dev/null; wait "$(cat "$D/n.pid")" 2>/dev/null; rm -f "$D/n.pid"; }
wst() { python3 -c 'import json,sys,urllib.request
w=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())["wal"]
print(w.get(sys.argv[1], "none"))' "$1" 2>/dev/null || echo "?"; }
# the probe: under a held fsync, which replies came within a second, the read elsewhere, then the rest
probe() { python3 - "$RESP" "$TCP" "$1" <<'PY'
import json, pcnative, socket, sys, time
resp, tcp, tag = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
def cmd(*a):
    return b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a)
r = socket.create_connection(("127.0.0.1", resp), 5)
o = socket.create_connection(("127.0.0.1", resp), 5)
n = pcnative.wrap(socket.create_connection(("127.0.0.1", tcp), 5))
open(sys.argv[3] + ".arm", "w").close()
time.sleep(0.3)
# pipelined: a write, then a read of the key written before the hold
r.sendall(cmd(b"SET", b"h1", b"v1") + cmd(b"GET", b"k0"))
n.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "set",
    "params": {"col": "0", "key": "h2", "value": "v2"}}) + "\n").encode()); n.flush()
t0 = time.time()
o.sendall(cmd(b"GET", b"k0"))
o.settimeout(1.0)
try:
    other = o.recv(100)
except socket.timeout:
    other = b""
other_ms = int((time.time() - t0) * 1000)
r.settimeout(1.0)
try:
    early = r.recv(100)
except socket.timeout:
    early = b""
print("OTHER=%s" % other.decode(errors="replace").replace("\r\n", "|"))
print("OTHER_MS=%d" % other_ms)
print("EARLY=%s" % early.decode(errors="replace").replace("\r\n", "|"))
sys.stdout.flush()
# the release: the control file loses its class
open(sys.argv[3] + ".release", "w").close()
r.settimeout(5.0)
late = early
t1 = time.time()
while late.count(b"\r\n") < 3 and time.time() - t1 < 5:
    try:
        b = r.recv(100)
    except socket.timeout:
        break
    if not b:
        break
    late += b
print("ALL=%s" % late.decode(errors="replace").replace("\r\n", "|"))
nat = ""
try:
    m = next(_m for _m in iter(lambda: json.loads(n.readline()), None) if "id" in _m)
    nat = "stored" if (m.get("result") or {}).get("stored") else str(m)[:60]
except Exception as e:
    nat = type(e).__name__
print("NATIVE=%s" % nat)
PY
}
# probe's arm/release markers drive the shim's control file
drive() { # drive <tag>: walhold armed at <tag>.arm, cleared at <tag>.release
	( while [ ! -f "$1.arm" ]; do sleep 0.05; done; echo walhold > "$D/ctl"
	  while [ ! -f "$1.release" ]; do sleep 0.05; done; : > "$D/ctl" ) &
}
resp1() { python3 - "$RESP" "$@" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 5)
a = [x.encode() for x in sys.argv[2:]]
s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
print(s.recv(200).decode(errors="replace").replace("\r\n", "|"))
PY
}
val() { sed -n "s/^$1=//p" "$D/$2" | head -1; }

# ---- 0. the config ----------------------------------------------------------------------------------
conf sync
"$BIN" -C -f "$D/n.conf" > "$D/c.out" 2>&1 && ok "0. perfcached -C takes fsync = sync" \
	|| { bad "0. -C refuses fsync = sync: $(tail -1 "$D/c.out")"; echo "walsynctest: $pass passed, $fail failed"; exit 1; }

# ---- 1. fsync = sync, the device's sync held ------------------------------------------------------------
: > "$D/ctl"
start || { bad "1. the node did not start"; echo "walsynctest: $pass passed, $fail failed"; exit 1; }
[ "$(wst fsync)" = sync ] && ok "1. /stats wal.fsync says sync" || bad "1. wal.fsync is $(wst fsync)"
[ "$(resp1 SET k0 v0)" = "+OK|" ] && ok "1. a write with nothing held answers OK" || bad "1. SET k0: $(resp1 SET k0 v0)"
drive "$D/p1"
probe "$D/p1" > "$D/p1.out" 2>&1
case "$(val EARLY p1.out)" in "") ok "1. no reply within 1 s while the WAL's fsync is held - the SET's OK and the GET behind it wait";;
	*) bad "1. replies came during the hold: $(val EARLY p1.out)";; esac
case "$(val OTHER p1.out)" in '$2|v0|') ok "1. another connection's read is not held ($(val OTHER_MS p1.out) ms)";;
	*) bad "1. the other connection's GET: '$(val OTHER p1.out)' after $(val OTHER_MS p1.out) ms";; esac
[ "$(val ALL p1.out)" = '+OK|$2|v0|' ] && ok "1. once the hold ends: +OK, then the GET's v0, in order" \
	|| bad "1. after the release: '$(val ALL p1.out)'"
[ "$(val NATIVE p1.out)" = stored ] && ok "1. the native door's set answers stored after the release" \
	|| bad "1. native set: $(val NATIVE p1.out)"
grep -q '^walhold ' "$D/sync.log" && ok "1. the shim held a WAL sync (the hold was real)" || bad "1. the shim never held a WAL sync"
h=$(wst sync_holds)
case "$h" in ''|*[!0-9]*) bad "1. wal.sync_holds: $h";; *) [ "$h" -ge 1 ] && ok "1. wal.sync_holds counts it ($h)" || bad "1. wal.sync_holds $h";; esac

# ---- 2. an OK, then kill -9: the write is there after the restart -----------------------------------
[ "$(resp1 SET k9 v9)" = "+OK|" ] && stop9
start || { bad "2. no restart"; echo "walsynctest: $pass passed, $fail failed"; exit 1; }
[ "$(resp1 GET k9)" = '$2|v9|' ] && ok "2. OK then kill -9 at once: k9 is there after the restart" \
	|| bad "2. after kill -9: GET k9 = $(resp1 GET k9)"
stopc

# ---- 3. the control: fsync = always answers while the sync is held ---------------------------------------
conf always
: > "$D/ctl"; rm -f "$D"/p3.*
start || { bad "3. the node did not start"; echo "walsynctest: $pass passed, $fail failed"; exit 1; }
resp1 SET k0 v0 > /dev/null
drive "$D/p3"
probe "$D/p3" > "$D/p3.out" 2>&1
case "$(val EARLY p3.out)" in '+OK|'*) ok "3. fsync = always answers within the hold (the ack is at the ring)";;
	*) bad "3. always during the hold: '$(val EARLY p3.out)'";; esac
stopc

e=$(grep -E ' (ERROR|CRIT)' "$D/n.log" | grep -cv 'an fsync has been stuck')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || bad "$e ERROR/CRIT lines: $(grep -E ' (ERROR|CRIT)' "$D/n.log" | head -2)"
echo "walsynctest: $pass passed, $fail failed"
[ $fail -eq 0 ]
