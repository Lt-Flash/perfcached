#!/bin/sh
# clientstest.sh - S160: /clients, the JSON table behind the status page's
# clients strip.  One connection per door lands in the table with its
# door, dialect and wire; a named RESP client carries its name; a
# subscriber its subscription count; a closed connection leaves; a
# subscriber that stops reading shows pending output bytes and is then
# gone (closed at the output cap, PC_MAX_OUTQ); the total agrees with
# /stats clients.open, which excludes HTTP as the table does; the page
# carries the strip and the panel.
# Fail-first: v0.4.0-rc1 answers 404 to /clients.
# Usage: test/clientstest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pccl.XXXXXX)
P1=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
cat > "$D/a.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = cl-client-secret
cluster = cl-cluster-secret
[listen]
tcp = 127.0.0.1:17781
resp = 127.0.0.1:17782
http = 127.0.0.1:18781
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 600 "$D/a.conf"
"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P1=$!
i=0
while [ $i -lt 80 ]; do grep -q "perfcached ready" "$D/a.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/a.log" || { echo "daemon did not start"; cat "$D/a.log"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" python3 - <<'PY'
import json, os, socket, struct, time, urllib.request, urllib.error
res = open(os.environ["RESFILE"], "a")
def check(cond, name): res.write(("P " if cond else "F ") + name + "\n")
TCP, RESP, HTTP = 17781, 17782, 18781
def get(path):
    try:
        return json.load(urllib.request.urlopen("http://127.0.0.1:%d%s" % (HTTP, path), timeout=5))
    except urllib.error.HTTPError as e:
        return {"_http": e.code}
    except Exception:
        return {}
def clients(): return get("/clients")
def rows(): return clients().get("clients", [])
def wait_total(want, secs):
    for _ in range(int(secs * 10)):
        if clients().get("total") == want: return True
        time.sleep(0.1)
    return False
MAGIC = 0x9E
def freq(payload, rid): return struct.pack("<BBBBIQ", MAGIC, 1, 1, 0, len(payload), rid) + payload
def vh(verb, col, key, extra=b"", val=b""): return bytes([verb, len(col)]) + struct.pack("<H", len(key)) + extra + col + key + val
def resp(*a): return ("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode()

j = socket.create_connection(("127.0.0.1", TCP), timeout=5); jf = j.makefile("rwb")
jf.write(b'{"jsonrpc":"2.0","id":1,"method":"ping"}\n'); jf.flush(); jf.readline()
b = socket.create_connection(("127.0.0.1", TCP), timeout=5)
b.sendall(freq(vh(2, b"0", b"k"), 7)); b.recv(4096)
nr = socket.create_connection(("127.0.0.1", TCP), timeout=5); nr.sendall(b"PING\r\n"); nr.recv(100)
r = socket.create_connection(("127.0.0.1", RESP), timeout=5); r.sendall(resp("CLIENT", "SETNAME", "probe")); r.recv(100)
sub = socket.create_connection(("127.0.0.1", RESP), timeout=5); sub.sendall(resp("SUBSCRIBE", "ch")); sub.recv(200)

check(wait_total(5, 3), "/clients counts the five data-door connections and not the HTTP fetch itself (total %r)" % clients().get("total"))
d = clients(); rs = d.get("clients", [])
pairs = sorted((x.get("door"), x.get("dialect")) for x in rs)
check(pairs == [("native", "binary"), ("native", "json"), ("native", "resp"), ("resp", "resp"), ("resp", "resp")],
      "every row carries its door and dialect: native/json, native/binary, native/resp, resp/resp x2 (%r)" % (pairs,))
check(all(x.get("encrypted") is False for x in rs) and all(str(x.get("addr", "")).startswith("127.0.0.1:") for x in rs),
      "plaintext loopback rows read encrypted=false with the client address")
check(d.get("shown") == 5 and all(set(x.keys()) >= {"id", "door", "dialect", "encrypted", "addr", "name", "age_s", "idle_s", "cmds", "last_cmd", "pending", "subs"} for x in rs),
      "rows carry the twelve columns the panel renders")
named = [x for x in rs if x.get("name") == "probe"]
check(len(named) == 1 and named[0]["door"] == "resp" and named[0]["cmds"] == 1 and named[0]["last_cmd"] == "client",
      "CLIENT SETNAME is visible as the row's name, with its command count and last command")
subs = [x for x in rs if x.get("subs") == 1]
check(len(subs) == 1 and subs[0]["last_cmd"] == "subscribe", "the subscriber's row carries subs=1")
jrow = [x for x in rs if x.get("dialect") == "json"]
check(len(jrow) == 1 and jrow[0]["last_cmd"] == "ping" and jrow[0]["cmds"] == 1 and jrow[0]["pending"] == 0,
      "the JSON row: last command ping, one command, nothing pending")
st = get("/stats")
check(st.get("clients", {}).get("open") == 5, "/stats clients.open agrees with the table's total (%r)" % st.get("clients", {}).get("open"))
ids = [x["id"] for x in rs]
check(ids == sorted(ids, reverse=True), "active first: rows are ordered newest touch first (ids %r)" % ids)

jf.close(); j.close()
check(wait_total(4, 3), "a closed connection leaves the table (total %r)" % clients().get("total"))
check(not [x for x in rows() if x.get("dialect") == "json"], "and it was the JSON row that left")
sub.sendall(resp("UNSUBSCRIBE", "ch")); sub.recv(200); time.sleep(0.2)
check(not [x for x in rows() if x.get("subs")], "UNSUBSCRIBE takes the count back to 0")

# a subscriber that stops reading: pending bytes climb, then the cap closes it
slow = socket.socket(); slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
slow.connect(("127.0.0.1", RESP)); slow.sendall(resp("SUBSCRIBE", "big")); slow.recv(200)
pub = socket.create_connection(("127.0.0.1", RESP), timeout=10); pf = pub.makefile("rwb")
payload = "x" * 65536
def publish(n):
    for _ in range(n):
        pf.write(resp("PUBLISH", "big", payload)); pf.flush(); pf.readline()
publish(96)                      # 6 MB staged, under the 8 MB cap
time.sleep(0.3)
srow = [x for x in rows() if x.get("subs") == 1]
check(len(srow) == 1 and srow[0]["pending"] > 0, "a subscriber that stops reading shows pending output bytes (%r)" % (srow[0]["pending"] if srow else None))
publish(64)                      # past the cap
gone = False
for _ in range(50):
    if not [x for x in rows() if x.get("subs") == 1]: gone = True; break
    time.sleep(0.1)
check(gone, "and is gone once the output cap closes it")
check(get("/stats").get("pubsub", {}).get("slow_kills", 0) >= 1, "counted as a slow kill on /stats")

# CLIENT SETINFO - a library's name and version, what a client library sends
# on connect; the first gap S165's unknown-command card caught on staging
li = socket.create_connection(("127.0.0.1", RESP), timeout=5); lf = li.makefile("rwb")
def lcmd(*a):
    lf.write(resp(*a)); lf.flush(); return lf.readline().decode().rstrip("\r\n")
check(lcmd("CLIENT", "SETINFO", "LIB-NAME", "redis-py") == "+OK" and lcmd("client", "setinfo", "lib-ver", "5.0.1") == "+OK",
      "CLIENT SETINFO LIB-NAME and lib-ver answer OK, attribute case-insensitive")
check(lcmd("CLIENT", "SETINFO", "LIB-NAME", "two words").startswith("-ERR"), "a value with a space is refused")
check(lcmd("CLIENT", "SETINFO", "LIB-NAME", "bad\x01").startswith("-ERR"), "a value with a control byte is refused")
check(lcmd("CLIENT", "SETINFO", "LIB-COLOUR", "x").startswith("-ERR"), "an attribute other than LIB-NAME / LIB-VER is refused")
check(lcmd("CLIENT", "SETINFO", "LIB-NAME").startswith("-ERR"), "the wrong number of arguments is refused")
lr = [x for x in rows() if x.get("lib_name") == "redis-py"]
check(len(lr) == 1 and lr[0].get("lib_ver") == "5.0.1", "/clients shows the library and its version on that row, the refusals changed nothing (%r)" % lr)
lf.write(resp("CLIENT", "LIST")); lf.flush()
hdr = lf.readline().decode()
body = lf.read(int(hdr[1:]) + 2).decode("utf-8", "replace") if hdr.startswith("$") else ""
check("lib-name=redis-py lib-ver=5.0.1" in body, "CLIENT LIST carries lib-name= and lib-ver= (%r)" % [l for l in body.splitlines() if "redis-py" in l])
unk = get("/stats").get("unknown_commands", {}).get("rows", [])
check(not [r for r in unk if r.get("name", "").startswith("CLIENT SETINFO")], "and CLIENT SETINFO is no longer an unknown command")
lf.close(); li.close()

page = urllib.request.urlopen("http://127.0.0.1:%d/" % HTTP, timeout=5).read().decode("utf-8", "replace")
check("id=clmetric" in page and "fleet-wide" in page and "id=clpanel" in page and "id=clfilter" in page,
      "the page carries the clients strip, the panel and its filter")
check("cmdcards(" in page and "slow log" in page, "the page carries the commands cards")
# the panel's functions must be TOP LEVEL: the click handler and tick() call
# loadClients() from outside drawStats(), and declared inside it (S160 to
# 069480f) they did not exist for them - the table never filled.  No JS
# engine here, so the placement is what is checked: after drawStats' end.
end_ds = page.find("sizePlane();trend();}")
check(end_ds > 0 and page.find("function loadClients(") > end_ds and page.find("function drawClients(") > end_ds,
      "loadClients / drawClients are declared after drawStats() ends, where the panel can call them")
check("/clients" in page, "the footer names /clients")
PY
while IFS= read -r l; do
	case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac
done < "$RES"
[ -s "$RES" ] || bad "driver produced no results"
echo "clientstest: $pass passed, $fail failed"
[ $fail -eq 0 ]
