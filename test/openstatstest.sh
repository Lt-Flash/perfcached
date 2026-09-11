#!/bin/sh
# openstatstest.sh - S123: the doors report what is OPEN now beside the
# running totals, and the totals can be reset - from the JSON door
# (reset_stats), the RESP door (CONFIG RESETSTAT), the page (POST
# /reset-stats, the only mutating HTTP route) and perfcli (reset-stats).
# Two binary connections, a JSON one and a RESP-door one are held open and
# the stats block must count them open by dialect; a JSON line on a
# binary-memoed connection is a JSON request on no JSON connection; closing
# one takes it out of the gauge and not out of the total; a reset zeroes
# every total and stamps `since`, and leaves the gauges - open connections,
# entries - alone.  Fail-first: a build before S123 has no `open`, no
# `since`, answers method-not-found to reset_stats and 405 to the POST.
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
D=$(mktemp -d /var/tmp/pcos.XXXXXX)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 32
[secrets]
client = os-client-secret
cluster = os-cluster-secret
[listen]
tcp = 127.0.0.1:17691
resp = 127.0.0.1:17692
http = 127.0.0.1:17693
plaintext = loopback
[collection c]
buckets_log2 = 8
CONF
"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
PIDS="$PIDS $!"
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" 2>/dev/null && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { echo "node did not start"; cat "$D/n.log"; exit 1; }
R=$(python3 - "$CLI" <<'PYEOF'
import json, socket, struct, sys, subprocess, os, time
cli = sys.argv[1]
def jconn():
    s = socket.create_connection(("127.0.0.1", 17691), timeout=20); return s, s.makefile("rwb")
rid = [0]
def call(f, m, **p):
    rid[0] += 1; f.write((json.dumps({"jsonrpc": "2.0", "id": rid[0], "method": m, "params": p} if p else {"jsonrpc": "2.0", "id": rid[0], "method": m}) + "\n").encode()); f.flush()
    return json.loads(f.readline())
def bconn():
    s = socket.create_connection(("127.0.0.1", 17691), timeout=20)
    s.sendall(struct.pack("<BBBBIQ", 0x9E, 1, 1, 0, 1, 1) + b"\x01")      # PING, no echo
    h = s.recv(16); assert len(h) == 16 and h[0] == 0x9E and h[2] == 2, h
    return s
def rconn():
    s = socket.create_connection(("127.0.0.1", 17692), timeout=20); f = s.makefile("rwb")
    f.write(b"*1\r\n$4\r\nPING\r\n"); f.flush(); assert f.readline().strip() == b"+PONG"
    return s, f
def resp(f, *args):
    f.write(("*%d\r\n" % len(args) + "".join("$%d\r\n%s\r\n" % (len(a), a) for a in args)).encode()); f.flush()
    return f.readline().decode().strip()
def http(method, path, extra=""):
    s = socket.create_connection(("127.0.0.1", 17693), timeout=20)
    s.sendall(("%s %s HTTP/1.1\r\nHost: x\r\n%s\r\n" % (method, path, extra)).encode())
    d = b""
    while True:
        c = s.recv(65536)
        if not c: break
        d += c
    s.close(); return d.decode("latin1")
out = {}
sc, sf = jconn()                       # the stats connection: itself a JSON connection
b1 = bconn(); b2 = bconn()
j1, jf = jconn(); call(jf, "ping")
r1, rf = rconn()
def stats():
    return call(sf, "stats")["result"]
st = stats(); nt = st["native"]; rs = st["resp"]
out["open1"] = "bin=%s/%s/%s json=%s/%s resp=%s/%s/%s" % (nt["binary"].get("open"), nt["binary"]["conns"], nt["binary"]["requests"], nt["json"].get("open"), nt["json"]["conns"], rs.get("open"), rs["conns"], rs["requests"])
jreq0 = nt["json"]["requests"]
# a JSON line on a binary-memoed connection
b1.sendall(b'{"jsonrpc":"2.0","id":9,"method":"members"}\n'); b1.recv(65536)
st = stats(); nt = st["native"]
# two JSON requests since the last reading: the previous stats call itself, and the members line
out["mixed"] = "json_req_delta=%d json_open=%s json_conns=%s bin_open=%s bin_conns=%s" % (nt["json"]["requests"] - jreq0, nt["json"].get("open"), nt["json"]["conns"], nt["binary"].get("open"), nt["binary"]["conns"])
b2.close(); time.sleep(0.3)
st = stats(); nt = st["native"]
out["closed"] = "bin_open=%s bin_conns=%s" % (nt["binary"].get("open"), nt["binary"]["conns"])
call(sf, "set", col="c", key="k1", value="v1")
st = stats(); col = [c for c in st["collections"] if c["name"] == "c"][0]
out["before_reset"] = "stores=%s entries=%s size_hist=%s since_reset_at=%s" % (col.get("stores"), col.get("entries"), sum(col.get("size_hist", [])), st.get("since", {}).get("reset_at"))
r = call(sf, "reset_stats")
out["reset_reply"] = "reset=%s at_ok=%s" % (r.get("result", {}).get("reset"), r.get("result", {}).get("at", 0) > 1700000000)
st = stats(); nt = st["native"]; rs = st["resp"]; col = [c for c in st["collections"] if c["name"] == "c"][0]; sn = st.get("since", {})
out["after_reset"] = "bin=%s/%s/%s json_open=%s json_conns=%s resp=%s/%s/%s stores=%s entries=%s size_hist=%s since_ok=%s since_s=%s" % (
    nt["binary"].get("open"), nt["binary"]["conns"], nt["binary"]["requests"], nt["json"].get("open"), nt["json"]["conns"], rs.get("open"), rs["conns"], rs["requests"],
    col.get("stores"), col.get("entries"), sum(col.get("size_hist", [])), sn.get("reset_at", 0) > 1700000000, sn.get("s"))
out["resp_resetstat"] = resp(rf, "CONFIG", "RESETSTAT") + " / get:" + resp(rf, "CONFIG", "GET", "x")[:1]
h = http("POST", "/reset-stats"); out["http_post"] = h.split("\r\n")[0] + " " + ("json" if '"reset":true' in h else "nojson")
out["http_get_reset"] = http("GET", "/reset-stats").split("\r\n")[0]
out["http_post_other"] = http("POST", "/stats").split("\r\n")[0]
out["http_post_body"] = http("POST", "/reset-stats", "Content-Length: 3\r\n\r\nabc").split("\r\n")[0]
out["http_put"] = http("PUT", "/reset-stats").split("\r\n")[0]
page = http("GET", "/")
out["page"] = "button=%s post=%s since=%s open=%s" % ("reset stats</button>" in page, "post(\"reset-stats\"" in page or "post(\\\"reset-stats\\\"" in page, "since start" in page, '"open"' in page or "[\"open\"" in page or "open" in page)
m = http("GET", "/metrics")
out["metrics"] = 'perfcached_connections_open{door="native",dialect="binary"} 1' in m and 'door="resp"' in m
env = dict(os.environ); env.pop("PERFCLI_AUTH", None)
c = subprocess.run([cli, "-h", "127.0.0.1", "-p", "17691", "-q", "reset-stats"], capture_output=True, text=True, env=env, timeout=20)
out["cli"] = ("reset" in c.stdout and "true" in c.stdout, c.returncode)
c2 = subprocess.run([cli, "-h", "127.0.0.1", "-p", "17691", "-q", "reset_stats"], capture_output=True, text=True, env=env, timeout=20)
out["cli2"] = ("reset" in c2.stdout and "true" in c2.stdout, c2.returncode)
for k, v in out.items(): print("%s: %s" % (k, v))
PYEOF
)
echo "$R" | sed 's/^/  ..   /'
g() { echo "$R" | grep "^$1: " | sed "s/^$1: //"; }
[ "$(g open1)" = "bin=2/2/2 json=2/2 resp=1/1/1" ] && ok "open by dialect: 2 binary, 2 JSON (one is the stats connection), 1 on the RESP door; the totals agree" || bad "open counts: $(g open1)"
[ "$(g mixed)" = "json_req_delta=2 json_open=2 json_conns=2 bin_open=2 bin_conns=2" ] && ok "a JSON line on a binary-memoed connection is one JSON request (beside the stats call's own) and no JSON connection, open or total" || bad "mixed dialect: $(g mixed)"
[ "$(g closed)" = "bin_open=1 bin_conns=2" ] && ok "closing a binary connection takes it out of open and not out of the total" || bad "after close: $(g closed)"
case "$(g before_reset)" in "stores=1 entries=1 size_hist=1 since_reset_at=0") ok "before the reset: one store counted, since.reset_at is 0 (the totals count from the start)";; *) bad "before reset: $(g before_reset)";; esac
[ "$(g reset_reply)" = "reset=True at_ok=True" ] && ok "reset_stats answers reset:true with the unix time" || bad "reset reply: $(g reset_reply)"
case "$(g after_reset)" in "bin=1/0/0 json_open=2 json_conns=0 resp=1/0/0 stores=0 entries=1 size_hist=0 since_ok=True since_s="[0-9]*) ok "after the reset every total is 0, since is stamped, and the gauges stand: open 1/2/1, entries 1" || bad "after reset: $(g after_reset)";; esac
[ "$(g resp_resetstat)" = "+OK / get:*" ] && ok "RESP CONFIG RESETSTAT answers +OK and CONFIG GET still answers" || bad "RESP: $(g resp_resetstat)"
case "$(g http_post)" in *"200 OK json") ok "POST /reset-stats answers 200 with reset:true";; *) bad "POST: $(g http_post)";; esac
case "$(g http_get_reset)" in *"404"*) ok "GET /reset-stats is not a route (404)";; *) bad "GET /reset-stats: $(g http_get_reset)";; esac
case "$(g http_post_other)" in *"404"*) ok "POST anywhere else is 404";; *) bad "POST /stats: $(g http_post_other)";; esac
case "$(g http_post_body)" in *"400"*) ok "a POST with a body is refused (400)";; *) bad "POST with body: $(g http_post_body)";; esac
case "$(g http_put)" in *"405"*) ok "PUT is 405";; *) bad "PUT: $(g http_put)";; esac
[ "$(g page)" = "button=True post=True since=True open=True" ] && ok "the page carries the reset button, the POST helper and the since label" || bad "page: $(g page)"
[ "$(g metrics)" = "True" ] && ok "/metrics exposes perfcached_connections_open by door and dialect" || bad "metrics: $(g metrics)"
[ "$(g cli)" = "(True, 0)" ] && ok "perfcli reset-stats sends reset_stats and prints the reply" || bad "perfcli reset-stats: $(g cli)"
[ "$(g cli2)" = "(True, 0)" ] && ok "perfcli reset_stats (the method's own spelling) works too" || bad "perfcli reset_stats: $(g cli2)"
echo "openstatstest: $pass passed, $fail failed"
[ $fail = 0 ]
