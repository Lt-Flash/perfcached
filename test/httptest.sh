#!/bin/sh
# httptest.sh — S46: a stable, machine-readable metrics surface.
#
# The task's own words: the JSON stats shape "has moved twice this
# week", so STABLE is the operative word.  These checks exist to make
# a rename break a test rather than an operator's dashboard: every
# name asserted below is part of the contract from here on.
#
#   1. the endpoint answers OpenMetrics text on GET /metrics
#   2. the contract names are all present, with HELP/TYPE
#   3. the numbers track reality (entries follow writes)
#   4. /health is a liveness probe an orchestrator can use
#   5. an unknown path is a clean 404, not a hang or a crash
#   6. an off-box metrics listener without an allow-list is REFUSED
#      at config time - it is plaintext and unauthenticated, so the
#      network is its only protection (the RESP listener's rule)
# Usage: test/httptest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
SEC=met-client-secret
D=$(mktemp -d /var/tmp/pcmet.XXXXXX)
trap 'pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

mk() { # mk <n> <listen-extra>
	mkdir -p "$D/w$1"
	cat > "$D/n$1.conf" <<CFGEOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 32
[secrets]
client = $SEC
cluster = met-cluster-secret
[listen]
plaintext = loopback
tcp = 127.0.65.$1:1797$1
$2
[wal]
dir = $D/w$1
segment_mb = 8
probe = no
save = off
[collection m]
buckets_log2 = 12
CFGEOF
}

start() {
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}

# a bare HTTP/1.0 GET; prints status line + body
http() { # http <path>
	python3 - "$1" <<'PYEOF'
import socket, sys
try:
    s = socket.create_connection(("127.0.65.1", 19651), timeout=5)
    s.sendall(("GET %s HTTP/1.0\r\nHost: x\r\n\r\n" % sys.argv[1]).encode())
    buf = b""
    while True:
        b = s.recv(65536)
        if not b:
            break
        buf += b
    sys.stdout.write(buf.decode("utf-8", "replace"))
except Exception as e:
    print("HTTPFAIL %s" % e)
PYEOF
}

fill() { # fill <n>
	python3 - "$1" <<'PYEOF'
import json, socket, sys
s = socket.create_connection(("127.0.65.1", 17971), timeout=10)
f = s.makefile("rwb"); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"] = p
    f.write((json.dumps(r)+"\n").encode()); f.flush()
    return json.loads(f.readline())
for i in range(int(sys.argv[1])):
    call("set", col="m", key="mk%04d" % i, value="v%04d" % i)
print("filled")
PYEOF
}

mk 1 "http = 127.0.65.1:19651
http_timeout = 2"
start 1 || { echo "daemon did not start:"; tail -5 "$D/n1.log"; exit 1; }

# ---- 1. the endpoint answers -----------------------------------------
R=$(http /metrics)
case "$R" in
	"HTTP/1.0 200"*|"HTTP/1.1 200"*) ok "GET /metrics answers 200";;
	HTTPFAIL*) bad "GET /metrics did not answer ($R)";;
	*) bad "GET /metrics answered: $(echo "$R" | head -1)";;
esac
echo "$R" | grep -qi "^content-type: *text/plain" \
	&& ok "the content type is text/plain" \
	|| bad "wrong or missing content-type"

# ---- 2. the CONTRACT: these names may not move silently ---------------
missing=
for m in perfcached_build_info \
         perfcached_arena_held_bytes \
         perfcached_arena_max_bytes \
         perfcached_arena_live_bytes \
         perfcached_arena_headroom_ratio \
         perfcached_writes_refused_total \
         perfcached_reclaim_released_bytes_total \
         perfcached_collection_entries \
         perfcached_wal_appended_total \
         perfcached_wal_dropped_total \
         perfcached_uptime_seconds; do
	echo "$R" | grep -q "^$m" || missing="$missing $m"
done
[ -z "$missing" ] && ok "every contract metric is present" \
	|| bad "missing metrics:$missing"
echo "$R" | grep -q "^# HELP perfcached_arena_held_bytes" \
	&& echo "$R" | grep -q "^# TYPE perfcached_arena_held_bytes gauge" \
	&& ok "metrics carry HELP and TYPE" \
	|| bad "HELP/TYPE missing - not OpenMetrics"

# ---- 3. the numbers track reality -------------------------------------
fill 50 >/dev/null
sleep 1
R2=$(http /metrics)
E=$(echo "$R2" | sed -n 's/^perfcached_collection_entries{collection="m"} *//p')
[ "${E:-0}" -ge 50 ] 2>/dev/null \
	&& ok "collection entries followed the writes ($E)" \
	|| bad "entries did not track the 50 writes ($E)"
U=$(echo "$R2" | sed -n 's/^perfcached_uptime_seconds *//p')
case "$U" in ''|*[!0-9.]*) bad "uptime is not a number ($U)";;
	*) ok "uptime is reported ($U)";; esac

# ---- 4. liveness ------------------------------------------------------
H=$(http /health)
case "$H" in "HTTP/1.0 200"*|"HTTP/1.1 200"*) ok "GET /health answers 200";;
	*) bad "GET /health: $(echo "$H" | head -1)";; esac

# ---- 5. an unknown path is a clean 404 --------------------------------
N=$(http /nope)
case "$N" in *" 404"*) ok "an unknown path is a clean 404";;
	HTTPFAIL*) bad "an unknown path broke the connection ($N)";;
	*) bad "unknown path answered: $(echo "$N" | head -1)";; esac

# ---- 6. a half-open request must not hold the slot for ever ----------
# The slowloris shape: connect, send a partial head, never finish.
# http_timeout is 2s in this fixture, so the daemon must close it;
# without the sweep the read below blocks until the client's own
# timeout and the connection leaks in the meantime.
SLOW=$(python3 - <<'PYEOF2'
import socket, time
try:
    s = socket.create_connection(("127.0.65.1", 19651), timeout=15)
    s.sendall(b"GET /metrics HTTP/1.0\r\n")   # unfinished head
    t0 = time.time()
    s.settimeout(12)
    d = s.recv(4096)                            # EOF when the daemon closes
    print("closed-after=%.1f" % (time.time() - t0) if d == b"" else "answered")
except socket.timeout:
    print("HELD-OPEN")
except Exception as e:
    print("ERR %s" % e)
PYEOF2
)
case "$SLOW" in
	closed-after*) ok "a half-open request head is closed ($SLOW)";;
	HELD-OPEN) bad "a partial request head held the connection open -
	         a slowloris keeps a worker slot for free";;
	*) bad "half-open probe: $SLOW";;
esac

# ---- 7. S37: the status page and the members view --------------------
P=$(http /)
case "$P" in
	"HTTP/1.0 200"*) ok "GET / answers 200";;
	*) bad "GET /: $(echo "$P" | head -1)";;
esac
echo "$P" | grep -qi "^content-type: *text/html" \
	&& ok "the page is served as html" \
	|| bad "wrong content-type for the page"
# no external assets: the daemon cannot fetch them and must not try
echo "$P" | grep -qiE "src=[\"']?https?://|href=[\"']?https?://" \
	&& bad "the page references an external asset" \
	|| ok "the page has no external assets"
M=$(http /members)
echo "$M" | grep -q '"members"' \
	&& ok "/members returns the fleet as JSON" \
	|| bad "/members did not return members JSON"
echo "$M" | grep -qi "^content-type: *application/json" \
	&& ok "/members is application/json" \
	|| bad "/members has the wrong content-type"

# ---- 8. the token guards EVERY route, not just the topology ----------
mk 3 "http = 127.0.65.3:19653
http_timeout = 2"
python3 - "$D/n3.conf" <<'PYEOF3'
import sys
p = sys.argv[1]
s = open(p).read().replace("[listen]", "[secrets]\nhttp = s3cr3t-token\n[listen]", 1)
open(p, "w").write(s)
PYEOF3
start 3 || { bad "the tokened node did not start"; }
tok() { # tok <path> <token|->
	python3 - "$1" "$2" <<'PYEOF4'
import socket, sys
path, tk = sys.argv[1], sys.argv[2]
req = "GET %s HTTP/1.0\r\nHost: x\r\n" % path
if tk != "-":
    req += "Authorization: Bearer %s\r\n" % tk
req += "\r\n"
try:
    s = socket.create_connection(("127.0.65.3", 19653), timeout=5)
    s.sendall(req.encode())
    buf = b""
    while True:
        b = s.recv(65536)
        if not b:
            break
        buf += b
    sys.stdout.write(buf.decode("utf-8", "replace").split("\r\n")[0])
except Exception as e:
    print("HTTPFAIL %s" % e)
PYEOF4
}
case "$(tok /members -)" in
	*401*) ok "no token is refused";;
	*) bad "an untokened /members was served: $(tok /members -)";;
esac
case "$(tok /members wrong-token)" in
	*401*) ok "a wrong token is refused";;
	*) bad "a wrong token was accepted";;
esac
case "$(tok /members s3cr3t-token)" in
	*200*) ok "the right token is accepted";;
	*) bad "the right token was refused: $(tok /members s3cr3t-token)";;
esac
# the hole with a tidy name: /metrics discloses the fleet too
case "$(tok /metrics -)" in
	*401*) ok "the token guards /metrics as well";;
	*) bad "/metrics bypassed the token";;
esac

# ---- 9. an off-box listener without an allow-list is refused ----------
mk 2 "http = 10.9.9.9:19652"
"$BIN" -f "$D/n2.conf" -C >/dev/null 2>&1 \
	&& bad "an off-box http listener without http_allow started" \
	|| ok "an off-box http listener needs http_allow"

echo "httptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
