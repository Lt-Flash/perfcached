#!/bin/sh
# updiscovertest.sh - a master's ROLE names its replicas and a node listing only the master reads
# from them, unless the master answers from behind a proxy: then auto lists them and does not use them.
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcupdisc.XXXXXX)
PM=17974 P1=17975 P2=17976 TCP=17977 RESP=17978 HTTP=17979
PID= FM= F1= F2=
trap 'kill $PID 2>/dev/null; for p in $FM $F1 $F2; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $PM $P1 $P2 $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "updiscovertest: port $p already bound" >&2; exit 1; }
done
fakes() { # fakes <M's extra ctl json, e.g. , "laddr": "..."> - M naming R1 and R2
	for p in $FM $F1 $F2; do kill -9 $p 2>/dev/null; wait $p 2>/dev/null; done
	for n in m 1 2; do
		rm -rf "$D/up$n"; mkdir -p "$D/up$n"
		python3 -c 'import json,sys; json.dump({"k%d" % i: {"type": "string", "value": sys.argv[1]} for i in range(20)}, open(sys.argv[2], "w"))' "$n" "$D/up$n/data.json"
	done
	echo '{"role": "master", "replicas": [["127.0.0.1", '$P1'], ["127.0.0.1", '$P2']]'"$1"'}' > "$D/upm/ctl.json"
	echo '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}' > "$D/up1/ctl.json"
	echo '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}' > "$D/up2/ctl.json"
	python3 "$T/fakeupstream.py" $PM "$D/upm" > "$D/fm.log" 2>&1 & FM=$!
	python3 "$T/fakeupstream.py" $P1 "$D/up1" > "$D/f1.log" 2>&1 & F1=$!
	python3 "$T/fakeupstream.py" $P2 "$D/up2" > "$D/f2.log" 2>&1 & F2=$!
	i=0; while [ $i -lt 50 ] && ! { [ -f "$D/upm/ready" ] && [ -f "$D/up1/ready" ] && [ -f "$D/up2/ready" ]; }; do sleep 0.1; i=$((i+1)); done
}
start() { # start <name> [extra collection lines...]
	n=$1; shift
	[ -n "$PID" ] && { kill $PID 2>/dev/null; wait $PID 2>/dev/null; }     # a bare wait waits for the fakes too
	rm -rf "$D/state"; mkdir -p "$D/state"
	{
		cat <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 32
[secrets]
client = updisc-client-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 8
upstream = redis://127.0.0.1:$PM
upstream_role = replica
upstream_prefixes = *
upstream_timeout_ms = 2000
upstream_negative_ms = 1000
C
		for l in "$@"; do echo "$l"; done
	} > "$D/$n.conf"
	chmod 600 "$D/$n.conf"
	"$BIN" -f "$D/$n.conf" > "$D/$n.log" 2>&1 & PID=$!
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/$n.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/$n.log" || { bad "the node ($n) did not start: $(tail -2 "$D/$n.log")"; exit 1; }
}
up() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
u={x["addr"].rsplit(":",1)[1]: x for x in [c for c in s["collections"] if c["name"]=="0"][0]["upstream"]["upstreams"]}
print(eval(sys.argv[1]))' "$1" 2>/dev/null; }
waitfor() { # waitfor <python expr> <want> <secs>
	i=0; while [ $i -lt $(($3 * 5)) ] && [ "$(up "$1")" != "$2" ]; do sleep 0.2; i=$((i+1)); done
	[ "$(up "$1")" = "$2" ]
}
reads() { python3 - $RESP <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10); f = s.makefile("rb")
got = {}
for i in range(20):
    k = b"k%d" % i
    s.sendall(b"*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n" % (len(k), k))
    l = f.readline()
    v = f.read(int(l[1:]) + 2)[:-2].decode() if l.startswith(b"$") and not l.startswith(b"$-1") else l.decode().strip()[:40]
    got[v] = got.get(v, 0) + 1
print(" ".join("%s=%d" % kv for kv in sorted(got.items())))
PY
}

fakes ""
start a
if waitfor 'sorted(k for k in u)' "['$PM', '$P1', '$P2']" 10; then
	w=$(up 'u["'$PM'"]["direct"], u["'$P1'"]["origin"], u["'$P2'"]["origin"], u["'$PM'"]["origin"]')
	r=$(reads)
	case "$w $r" in
	"(True, 'role', 'role', 'configured') "1=*" 2="*|"(True, 'role', 'role', 'configured') "1=20|"(True, 'role', 'role', 'configured') "2=20)
		ok "A. direct: R1 and R2 added from M's ROLE, every read from a replica ($r)";;
	*) bad "A. direct: $w, reads $r";;
	esac
else
	bad "A. the replicas M's ROLE names were not added: $(up 'sorted(u)')"
fi
echo '{"role": "master", "replicas": [["127.0.0.1", '$P1']]}' > "$D/upm/ctl.json"
if waitfor 'u["'$P2'"]["why"]' "gone from the topology" 20; then
	r=$(reads)
	[ "$r" = "1=20" ] && ok "B. R2 dropped from M's ROLE: gone from the topology, reads from R1 only ($r)" || bad "B. R2 gone, but reads $r"
else
	bad "B. R2 dropped from M's ROLE: $(up 'u["'$P2'"]["state"], u["'$P2'"]["why"]')"
fi

fakes ', "laddr": "10.9.9.9:6379"'
start c
sleep 3
w=$(up 'sorted(u), u["'$PM'"]["direct"], u["'$PM'"]["behind"]')
n=$(grep -c "answers from another address - a proxy - so the 2 replica(s) its ROLE names are shown, not used" "$D/c.log")
[ "$w" = "(['$PM'], False, '127.0.0.1:$P1,127.0.0.1:$P2')" ] && [ "$n" = 1 ] &&
	ok "C. behind a proxy (auto): nothing added, M not direct, its replicas listed behind it, logged once" ||
	bad "C. behind a proxy: $w, logged $n time(s)"

start d "upstream_discover = yes"
waitfor 'sorted(k for k in u)' "['$PM', '$P1', '$P2']" 10 && ok "D. upstream_discover = yes behind the proxy: R1 and R2 added" ||
	bad "D. upstream_discover = yes: $(up 'sorted(u)')"

fakes ""
start e "upstream_discover = no"
sleep 3
w=$(up 'sorted(u), u["'$PM'"]["behind"], u["'$PM'"]["direct"]')
[ "$w" = "(['$PM'], '', None)" ] && ok "E. upstream_discover = no: nothing added, nothing shown" || bad "E. upstream_discover = no: $w"
fakes ""
start f "upstream_writes = through"
sleep 3
w=$(up 'sorted(u), u["'$PM'"]["direct"], u["'$PM'"]["behind"]')
n=$(grep -c "a write-through collection reads only the upstreams its config names" "$D/f.log")
[ "$w" = "(['$PM'], True, '127.0.0.1:$P1,127.0.0.1:$P2')" ] && [ "$n" = 1 ] &&
	ok "F. write-through under auto, M direct: nothing added, its replicas listed behind it, logged once" ||
	bad "F. write-through under auto: $w, logged $n time(s)"
start g "upstream_writes = through" "upstream_discover = yes"
waitfor 'sorted(k for k in u)' "['$PM', '$P1', '$P2']" 10 && ok "G. write-through with upstream_discover = yes: R1 and R2 added" ||
	bad "G. write-through, upstream_discover = yes: $(up 'sorted(u)')"
echo "updiscovertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
