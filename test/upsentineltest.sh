#!/bin/sh
# upsentineltest.sh - the upstream names one sentinel and the collection reads the servers it
# watches: topology, s_down, failovers, sentinel auth, a frozen sentinel, pod churn (cases A-M).
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcupsen.XXXXXX)
PS=17956 PS2=17957 PM=17958 P1=17959 P2=17960 TCP=17966 RESP=17967 HTTP=17968
P3=19635 P4=19636 DEAD=21610           # L: two more replicas, and 30 addresses nothing answers at
PID= FS= FS2= FM= F1= F2= F3= F4=
trap 'kill $PID 2>/dev/null; for p in $FS $FS2 $FM $F1 $F2 $F3 $F4; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $PS $PS2 $PM $P1 $P2 $P3 $P4 $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upsentineltest: port $p already bound" >&2; exit 1; }
done
smap() { # smap <master port> <replicas json> [another master json] [event seq] - what S and S2 say
	echo '{"redis_mode": "sentinel", "sentinel_event_seq": '"${4:-0}"', "sentinel": {"mym": {"ip": "127.0.0.1", "port": '$1', "replicas": '$2', "sentinels": [["127.0.0.1", '$PS2']]}'"${3:-}"'}}' > "$D/ups/ctl.json"
	echo '{"redis_mode": "sentinel", "sentinel": {"mym": {"ip": "127.0.0.1", "port": '$1', "replicas": '$2', "sentinels": [["127.0.0.1", '$PS']]}'"${3:-}"'}}' > "$D/ups2/ctl.json"
}
fakes() {
	for p in $FS $FS2 $FM $F1 $F2; do kill -9 $p 2>/dev/null; wait $p 2>/dev/null; done
	for n in s s2 m 1 2; do
		rm -rf "$D/up$n"; mkdir -p "$D/up$n"
		python3 -c 'import json,sys; json.dump({"k%d" % i: {"type": "string", "value": sys.argv[1]} for i in range(20)}, open(sys.argv[2], "w"))' "$n" "$D/up$n/data.json"
	done
	smap $PM '[["127.0.0.1", '$P1'], ["127.0.0.1", '$P2']]' "${1:-}"
	echo '{"role": "master"}' > "$D/upm/ctl.json"
	echo '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}' > "$D/up1/ctl.json"
	echo '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}' > "$D/up2/ctl.json"
	python3 "$T/fakeupstream.py" $PS "$D/ups" > "$D/fs.log" 2>&1 & FS=$!
	python3 "$T/fakeupstream.py" $PS2 "$D/ups2" > "$D/fs2.log" 2>&1 & FS2=$!
	python3 "$T/fakeupstream.py" $PM "$D/upm" > "$D/fm.log" 2>&1 & FM=$!
	python3 "$T/fakeupstream.py" $P1 "$D/up1" > "$D/f1.log" 2>&1 & F1=$!
	python3 "$T/fakeupstream.py" $P2 "$D/up2" > "$D/f2.log" 2>&1 & F2=$!
	i=0; while [ $i -lt 50 ] && ! { [ -f "$D/ups/ready" ] && [ -f "$D/ups2/ready" ] && [ -f "$D/upm/ready" ] && [ -f "$D/up1/ready" ] && [ -f "$D/up2/ready" ]; }; do sleep 0.1; i=$((i+1)); done
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
arena_mb = ${ARENA:-32}
[secrets]
client = upsen-client-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 8
upstream = redis://127.0.0.1:$PS
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
c=[c for c in s["collections"] if c["name"]=="0"][0]["upstream"]
u={x["addr"].rsplit(":",1)[1]: x for x in c["upstreams"]}
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

fakes
start a
if waitfor 'sorted(k for k in u)' "['$PS', '$PM', '$P1', '$P2']" 15; then
	w=$(up 'u["'$PS'"]["why"], u["'$PM'"]["origin"], u["'$P1'"]["origin"], u["'$P2'"]["origin"]')
	r=$(reads); t=$(up 'c["sentinel"]')
	case "$w|$r|$t" in
	"('a sentinel: the topology comes from it', 'sentinel', 'sentinel', 'sentinel')|"1=*" 2="*"|mym: master 127.0.0.1:$PM, replica 127.0.0.1:$P1, replica 127.0.0.1:$P2")
		ok "A. one sentinel named: M, R1, R2 added, S not read, reads from the replicas ($r)";;
	*) bad "A. $w | reads $r | sentinel '$t'";;
	esac
else
	bad "A. the servers S watches were not added: $(up 'sorted(u)') - sentinel '$(up 'c["sentinel"]')'"
fi
smap $PM '[["127.0.0.1", '$P1'], ["127.0.0.1", '$P2', "slave,s_down"]]'
if waitfor 'u["'$P2'"]["why"]' "gone from the topology" 15; then
	r=$(reads)
	[ "$r" = "1=20" ] && ok "B. R2 held s_down: gone from the topology, reads from R1 only ($r)" || bad "B. R2 gone, but reads $r"
else
	bad "B. R2 held s_down: $(up 'u["'$P2'"]["state"], u["'$P2'"]["why"]')"
fi
kill -9 $FS; wait $FS 2>/dev/null; FS=
echo '{"role": "master"}' > "$D/up1/ctl.json"
echo '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$P1'}' > "$D/upm/ctl.json"
echo '{"redis_mode": "sentinel", "sentinel": {"mym": {"ip": "127.0.0.1", "port": '$P1', "replicas": [["127.0.0.1", '$PM']], "sentinels": [["127.0.0.1", '$PS']]}}}' > "$D/ups2/ctl.json"
if waitfor 'c["sentinel"]' "mym: master 127.0.0.1:$P1, replica 127.0.0.1:$PM" 15; then
	waitfor 'u["'$P1'"]["role"], u["'$PM'"]["role"]' "('master', 'replica')" 15   # ROLE, every 10 s
	r=$(reads)
	[ "$r" = "m=20" ] && ok "C. S killed, a failover told by S2 alone: followed, reads from the old master now a replica ($r)" ||
		bad "C. followed S2, but reads $r"
else
	bad "C. S killed, S2's failover: sentinel '$(up 'c["sentinel"]')'"
fi
kill -9 $PID; wait $PID 2>/dev/null
"$BIN" -f "$D/a.conf" > "$D/f.log" 2>&1 & PID=$!
i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/f.log"; do sleep 0.1; i=$((i+1)); done
n=$(grep -c "the last topology - 2 server(s), 2 sentinel(s) - tried at the start" "$D/f.log")
if [ "$n" = 1 ] && waitfor 'c["sentinel"]' "mym: master 127.0.0.1:$P1, replica 127.0.0.1:$PM" 15; then
	ok "F. restarted with S dead: the kept topology tried at the start, S2 answers"
else
	bad "F. restarted with S dead: logged $n, sentinel '$(up 'c["sentinel"]')', upstreams $(up 'sorted(u)')"
fi

fakes ', "other": {"ip": "127.0.0.1", "port": '$P2'}'
start d
sleep 3
t=$(up 'c["sentinel"]'); w=$(up 'sorted(u)')
case "$t" in *"watches mym, other - name one with upstream_sentinel_master"*|*"watches other, mym - name one with upstream_sentinel_master"*)
	[ "$w" = "['$PS']" ] && ok "D. two masters, none named: nothing added, said why" || bad "D. two masters: added $w";;
*) bad "D. two masters, none named: sentinel '$t', upstreams $w";;
esac
start d2 "upstream_sentinel_master = mym"
waitfor 'sorted(k for k in u)' "['$PS', '$PM', '$P1', '$P2']" 15 && ok "D. upstream_sentinel_master = mym: its servers added" ||
	bad "D. upstream_sentinel_master = mym: $(up 'sorted(u)'), sentinel '$(up 'c["sentinel"]')'"

fakes
start e "upstream_discover = no"
sleep 3
t=$(up 'c["sentinel"]'); w=$(up 'sorted(u)')
[ "$w" = "['$PS']" ] && [ "$t" = "mym: master 127.0.0.1:$PM, replica 127.0.0.1:$P1, replica 127.0.0.1:$P2" ] &&
	ok "E. upstream_discover = no: nothing added, what S said shown" || bad "E. upstream_discover = no: $w, sentinel '$t'"
fakes
start g
if waitfor 'c["sentinel"]' "mym: master 127.0.0.1:$PM, replica 127.0.0.1:$P1, replica 127.0.0.1:$P2" 15; then
	sleep 1                        # past the first round: the next refresh is ~9 s away
	t0=$(date +%s%N)
	smap $P1 '[["127.0.0.1", '$PM'], ["127.0.0.1", '$P2']]' "" 1
	if waitfor 'c["sentinel"]' "mym: master 127.0.0.1:$P1, replica 127.0.0.1:$PM, replica 127.0.0.1:$P2" 2; then
		ok "G. a failover S published: followed in $(( ($(date +%s%N) - t0) / 1000000 )) ms, not at the next refresh"
	else
		bad "G. a failover S published: not followed within 2 s (sentinel '$(up 'c["sentinel"]')')"
	fi
else
	bad "G. the first topology: sentinel '$(up 'c["sentinel"]')'"
fi
fakes
start i
if waitfor 'sorted(k for k in u)' "['$PS', '$PM', '$P1', '$P2']" 15; then
	echo '{"redis_mode": "sentinel", "sentinel": {"mym": {"ip": "127.0.0.1", "port": '$PM', "replicas": [["127.0.0.1", '$P1', "slave,s_down"], ["127.0.0.1", '$P2', "slave,disconnected"]], "sentinels": [["127.0.0.1", '$PS2']]}}}' > "$D/ups/ctl.json"
	sleep 12                       # past S's next round (every 10 s)
	w=$(up 'u["'$P1'"]["why"], u["'$P2'"]["why"]'); r=$(reads)
	case "$w|$r" in
	*"gone from the topology"*) bad "I. S alone said every replica down: retired ($w)";;
	*"|1="*" 2="*) ok "I. S alone says every replica is down, S2 does not: none retired, reads from both ($r)";;
	*) bad "I. S alone, every replica down: $w, reads $r";;
	esac
	echo '{"redis_mode": "sentinel", "sentinel": {"mym": {"ip": "127.0.0.1", "port": '$PM', "replicas": [["127.0.0.1", '$P1', "slave,s_down"], ["127.0.0.1", '$P2', "slave,disconnected"]], "sentinels": [["127.0.0.1", '$PS']]}}}' > "$D/ups2/ctl.json"
	if waitfor 'u["'$P1'"]["why"], u["'$P2'"]["why"]' "('gone from the topology', 'gone from the topology')" 15; then
		ok "I. S2 says so too: both replicas gone"
	else
		bad "I. S and S2 agree every replica is down: $(up 'u["'$P1'"]["why"], u["'$P2'"]["why"]')"
	fi
else
	bad "I. the first topology: $(up 'sorted(u)')"
fi
addpw() { # addpw <ctl.json> <password>
	python3 -c 'import json,sys; c=json.load(open(sys.argv[1])); c["password"]=sys.argv[2]; json.dump(c, open(sys.argv[1], "w"))' "$1" "$2"
}
fakes
for n in s s2; do addpw "$D/up$n/ctl.json" sentpw; done
for n in m 1 2; do addpw "$D/up$n/ctl.json" datapw; done
start h "upstream_password = datapw" "upstream_sentinel_password = sentpw"
if waitfor 'sorted(k for k in u)' "['$PS', '$PM', '$P1', '$P2']" 15; then
	r=$(reads)
	case "$r" in "1="*" 2="*) ok "H. sentinels with their own password, upstream_password refused by them: found, servers read ($r)";;
	*) bad "H. sentinels with their own password: found, but reads $r";; esac
else
	bad "H. sentinels with their own password, upstream_password refused by them: $(up 'sorted(u)'), S '$(up 'u["'$PS'"]["why"]')'"
fi
fakes
for n in s s2; do addpw "$D/up$n/ctl.json" sentpw; done
start h2 "upstream_sentinel_password = sentpw"
if waitfor 'sorted(k for k in u)' "['$PS', '$PM', '$P1', '$P2']" 15; then
	ok "H. sentinels with their own password, no upstream_password (NOAUTH): found"
else
	bad "H. sentinels with their own password, no upstream_password: $(up 'sorted(u)'), S '$(up 'u["'$PS'"]["why"]')'"
fi
setctl() { # setctl <ctl.json> <python statements on c>
	python3 -c 'import json,sys; c=json.load(open(sys.argv[1])); exec(sys.argv[2]); json.dump(c, open(sys.argv[1], "w"))' "$1" "$2"
}
TOPO0="mym: master 127.0.0.1:$PM, replica 127.0.0.1:$P1, replica 127.0.0.1:$P2"
fakes
start j
if waitfor 'c["sentinel"]' "$TOPO0" 15; then
	sleep 1                        # past the first round: the next refresh is ~9 s away
	r0=$(grep -c "^SENTINEL MASTERS\$" "$D/ups/counts")
	setctl "$D/ups/ctl.json" 'c["sentinel_event_seq"] = 1; c["sentinel_event_count"] = 5'
	sleep 3
	r1=$(grep -c "^SENTINEL MASTERS\$" "$D/ups/counts"); n=$((r1 - r0))
	[ $n -ge 1 ] && [ $n -le 2 ] && ok "J. a burst of five events from S: $n round(s) for them, not five" ||
		bad "J. a burst of five events from S: $n rounds (want 1; 2 at most, a refresh among them)"
else
	bad "J. the first topology: sentinel '$(up 'c["sentinel"]')'"
fi
fakes
start k
i=0; while [ $i -lt 50 ] && ! grep -q "^SUBSCRIBE\$" "$D/ups/counts" 2>/dev/null; do sleep 0.2; i=$((i+1)); done
if waitfor 'c["sentinel"]' "$TOPO0" 15 && grep -q "^SUBSCRIBE\$" "$D/ups/counts"; then
	setctl "$D/ups2/ctl.json" 'c["sentinel_event_seq"] = 0'
	kill -STOP $FS
	i=0; while [ $i -lt 125 ] && ! grep -q "^SUBSCRIBE\$" "$D/ups2/counts" 2>/dev/null; do sleep 0.2; i=$((i+1)); done
	if grep -q "^SUBSCRIBE\$" "$D/ups2/counts" 2>/dev/null; then
		ok "K. S frozen: the subscription moved to S2, which answers the rounds"
		t0=$(date +%s%N)
		setctl "$D/ups2/ctl.json" 'c["sentinel"]["mym"]["port"] = '$P1'; c["sentinel"]["mym"]["replicas"] = [["127.0.0.1", '$PM'], ["127.0.0.1", '$P2']]; c["sentinel_event_seq"] = 1'
		if waitfor 'c["sentinel"]' "mym: master 127.0.0.1:$P1, replica 127.0.0.1:$PM, replica 127.0.0.1:$P2" 6; then
			ok "K. a failover S2 published: followed in $(( ($(date +%s%N) - t0) / 1000000 )) ms (S's 2 s timeout first)"
		else
			bad "K. a failover S2 published: not followed within 6 s (sentinel '$(up 'c["sentinel"]')')"
		fi
	else
		bad "K. S frozen for 25 s: no SUBSCRIBE reached S2 - the subscription stayed on S, whose events never come"
	fi
	kill -CONT $FS
else
	bad "K. the first topology: sentinel '$(up 'c["sentinel"]')', S subscribed: $(grep -c "^SUBSCRIBE\$" "$D/ups/counts" 2>/dev/null)"
fi
for n in 3 4; do
	rm -rf "$D/up$n"; mkdir -p "$D/up$n"
	python3 -c 'import json,sys; json.dump({"k%d" % i: {"type": "string", "value": sys.argv[1]} for i in range(20)}, open(sys.argv[2], "w"))' "$n" "$D/up$n/data.json"
	echo '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}' > "$D/up$n/ctl.json"
done
python3 "$T/fakeupstream.py" $P3 "$D/up3" > "$D/f3.log" 2>&1 & F3=$!
python3 "$T/fakeupstream.py" $P4 "$D/up4" > "$D/f4.log" 2>&1 & F4=$!
churn() { # churn <leg> <arena MB> <what stops the slots: full or room> - six rounds of five new replicas, then R3 and R4
	fakes
	ARENA=$2; start $1; ARENA=
	if waitfor 'sorted(k for k in u)' "['$PS', '$PM', '$P1', '$P2']" 15; then
		seq=1 port=$DEAD
		for r in 1 2 3 4 5 6; do       # 4 + 30 addresses: past the 32 slots by two
			reps=
			for j in 1 2 3 4 5; do reps="$reps${reps:+, }[\"127.0.0.1\", $port]"; port=$((port + 1)); done
			smap $PM "[$reps]" "" $seq; seq=$((seq + 1))
			sleep 0.5
		done
		n6=$(up 'len(u)')
		full=$(grep -c "more upstreams than" "$D/$1.log") room=$(grep -c "no room in the arena for" "$D/$1.log")
		case "$3:$n6:$full:$room" in
		full:32:1:0|room:[12][0-9]:0:1|room:3[01]:0:1) ;;
		*) bad "$1. the premise: $n6 slots, $full slot-limit and $room arena warning(s) after six rounds on $2 MB (want the $3 one first)"; return;;
		esac
		sleep 6                        # past the quarantine: 2 x upstream_timeout_ms + 1 s
		smap $PM '[["127.0.0.1", '$P3'], ["127.0.0.1", '$P4']]' "" $seq
		if waitfor '"'$P3'" in u and "'$P4'" in u and u["'$P3'"]["why"] != "gone from the topology"' True 15; then
			sleep 2
			r=$(reads)
			case "$r" in
			"3="*" 4="*) ok "$1. addresses that keep changing, $2 MB: $n6 slots used ($3), then R3 and R4 took gone ones; reads from them ($r), $(up 'len(u)') slots";;
			*) bad "$1. R3 and R4 took gone slots, but the reads: $r (want 3= and 4=); R3 $(up 'u["'$P3'"]["state"], u["'$P3'"]["why"]')";;
			esac
		else
			bad "$1. $n6 slots used ($3, $2 MB), then R3 and R4 named: not added - $(up 'len(u)') slots, $(grep -c "more upstreams than" "$D/$1.log") slot-limit and $(grep -c "no room in the arena for" "$D/$1.log") arena warning(s)"
		fi
	else
		bad "$1. the first topology: $(up 'sorted(u)')"
	fi
}
churn L 64 full
churn M 24 room
echo "upsentineltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
