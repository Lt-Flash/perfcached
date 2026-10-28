#!/bin/sh
# iostattest.sh - 0.6.0 step 1 (DESIGN 12li): the system-call instruments
# S361-S364 are judged with.
#  1. the workers: a burst of 400 pipelined SETs on one connection arrives
#     in far fewer read() calls than requests, and wakes the worker far
#     fewer times; every counter moves; /stats io and /metrics carry them
#  1b. one request at a time (depth 1): about ONE read() a request - a
#     short read ends the drain (0.6.0, DESIGN 12lk); before, every request
#     cost a second, empty read() to collect EAGAIN (2.0 a request)
#  2. the cluster plane (three eager nodes): the writer's replication goes
#     out as UDP datagrams, ONE sendto() EACH today - datagrams per syscall
#     exactly 1.0, the figure S364's batching is meant to raise; a member
#     receives them, and its receive syscalls also count the empty poll
#     that ends each drain (rx_syscalls > rx_datagrams)
#  3. a stats reset starts /stats io again; /metrics never falls
# The workers' figures are the ACTIVE backend's (/stats io.backend, S361):
# epoll's read() calls and waits, or an io_uring worker's receive
# completions and io_uring_enter() calls - with read() at 0.
# Usage: test/iostattest.sh ./perfcached
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
D=$(mktemp -d /var/tmp/pcios.XXXXXX)
P1= P2= P3=
trap 'for p in $P1 $P2 $P3; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19611 19612 19613 19621 19622 19623; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "iostattest: port $p already bound" >&2; exit 1; }
done

conf() { # conf <n>
	mkdir -p "$D/s$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = ios-client-secret
cluster = ios-cluster-secret
[listen]
tcp = 127.0.0.1:1961$1
http = 127.0.0.1:1962$1
plaintext = loopback
[cluster]
multicast = 239.255.78.31:19540
advertise = 127.0.82.$1
mode = eager
collections = 0
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"
}
st() { # st <node> <python expr over s> - a figure from /stats
	python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:1962%s/stats" % sys.argv[1], timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null
}
metric() { # metric <node> <series>
	python3 -c 'import sys,urllib.request
for l in urllib.request.urlopen("http://127.0.0.1:1962%s/metrics" % sys.argv[1], timeout=5).read().decode().splitlines():
    if l.startswith(sys.argv[2] + " "): print(l.split()[-1])' "$1" "$2" 2>/dev/null
}

for n in 1 2 3; do conf $n; done
"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 & P1=$!
"$BIN" -f "$D/n2.conf" > "$D/n2.log" 2>&1 & P2=$!
"$BIN" -f "$D/n3.conf" > "$D/n3.log" 2>&1 & P3=$!
i=0; while [ $i -lt 200 ]; do
	r=$(for n in 1 2 3; do st $n 's["state"]+":"+str(s.get("cluster",{}).get("peers_up"))'; done | tr '\n' ' ')
	[ "$r" = "ready:2 ready:2 ready:2 " ] && break
	sleep 0.1; i=$((i+1))
done
[ "$r" = "ready:2 ready:2 ready:2 " ] && ok "three eager nodes formed" || { bad "the fleet did not form: $r"; echo "iostattest: $pass passed, $fail failed"; exit 1; }

# ---- 1. the workers -------------------------------------------------------------
BK=$(st 1 's["io"]["backend"]["active"]')
if [ "$BK" = io_uring ]; then        # S361: the same figures, an io_uring worker's
	K_IN=uring_receives K_WAIT=uring_enters K_EV=uring_completions W_IN="receive completions" W_WAIT="io_uring_enter() calls"
	K_OUT=uring_sends W_OUT="io_uring sends"
else
	K_IN=reads K_WAIT=epoll_waits K_EV=epoll_events W_IN="read() calls" W_WAIT="epoll waits"
	K_OUT=writes W_OUT="write() calls"
fi
echo "  ..   the workers run $BK"
RR0=$(st 1 's["io"]["reads"]')
R0=$(st 1 's["io"]["'$K_IN'"]'); W0=$(st 1 's["io"]["'$K_WAIT'"]'); E0=$(st 1 's["io"]["'$K_EV'"]'); X0=$(st 1 's["io"]["'$K_OUT'"]')
WW0=$(st 1 's["io"]["writes"]')
# each pair from ONE /stats: two reads straddle the ALIVE frames sent between them
read UT0 UD0 <<EOF
$(st 1 '"%d %d" % (s["io"]["cluster_udp"]["tx_syscalls"], s["io"]["cluster_udp"]["tx_datagrams"])')
EOF
read VR0 VD0 <<EOF
$(st 2 '"%d %d" % (s["io"]["cluster_udp"]["rx_syscalls"], s["io"]["cluster_udp"]["rx_datagrams"])')
EOF
python3 - <<'PY' > "$D/burst.out" 2>&1
import socket
s = socket.create_connection(("127.0.0.1", 19611), 10)
f = s.makefile("rb")
n = 400
s.sendall(b"".join(b"*3\r\n$3\r\nSET\r\n$%d\r\nk%d\r\n$5\r\nhello\r\n" % (len(b"k%d" % i), i) for i in range(n)))
ok = sum(1 for _ in range(n) if f.readline().startswith(b"+OK"))
print("ok", ok)
PY
grep -q "^ok 400$" "$D/burst.out" && ok "400 pipelined SETs answered" || bad "the burst: $(cat "$D/burst.out")"
sleep 1                                            # replication settles
R1=$(st 1 's["io"]["'$K_IN'"]'); W1=$(st 1 's["io"]["'$K_WAIT'"]'); E1=$(st 1 's["io"]["'$K_EV'"]'); X1=$(st 1 's["io"]["'$K_OUT'"]')
dr=$((R1 - R0)); dw=$((W1 - W0)); de=$((E1 - E0)); dx=$((X1 - X0))
[ "$dr" -ge 1 ] && [ "$dr" -lt 100 ] && ok "1. 400 requests came in $dr $W_IN (pipelined)" || bad "1. $K_IN moved by $dr for 400 requests"
[ "$dw" -ge 1 ] && [ "$de" -ge 1 ] && [ "$dw" -lt 400 ] && ok "1. $dw $W_WAIT returned $de events for them" || bad "1. $K_WAIT $dw, $K_EV $de"
if [ "$BK" = io_uring ]; then
	RR1=$(st 1 's["io"]["reads"]'); WW1=$(st 1 's["io"]["writes"]')
	[ "$RR1" = "$RR0" ] && [ "$WW1" = "$WW0" ] && ok "1. and no read() or write() call at all" ||
		bad "1. an io_uring worker called read() ($RR0 -> $RR1) or write() ($WW0 -> $WW1)"
fi
[ "$dx" -ge 1 ] && [ "$dx" -lt 400 ] && ok "1. and $dx $W_OUT carried the 400 replies" || bad "1. $K_OUT moved by $dx"
[ -n "$(metric 1 perfcached_io_epoll_waits_total)" ] && [ -n "$(metric 1 'perfcached_io_syscalls_total{call="read"}')" ] &&
	ok "1. /metrics carries the worker counters" || bad "1. /metrics lacks perfcached_io_*"

# ---- 1b. depth 1 ------------------------------------------------------------------
R0=$(st 1 's["io"]["'$K_IN'"]')
python3 - <<'PY' > "$D/d1.out" 2>&1
import socket
s = socket.create_connection(("127.0.0.1", 19611), 10)
f = s.makefile("rb")
ok = 0
for i in range(300):
    s.sendall(b"*2\r\n$3\r\nGET\r\n$%d\r\nk%d\r\n" % (len(b"k%d" % i), i))
    l = f.readline()
    if l.startswith(b"$5"):
        f.readline(); ok += 1
print("ok", ok)
PY
R1=$(st 1 's["io"]["'$K_IN'"]'); dr=$((R1 - R0))
grep -q "^ok 300$" "$D/d1.out" || bad "1b. the depth-1 GETs: $(cat "$D/d1.out")"
# 300 requests; a little slack for the /stats polls' own connections
[ "$dr" -ge 300 ] && [ "$dr" -le 330 ] && ok "1b. 300 depth-1 requests took $dr $W_IN (~1.0 each; epoll drained to EAGAIN took 2.0)" ||
	bad "1b. 300 depth-1 requests took $dr $W_IN (want ~300, not ~600)"

# ---- 2. the cluster plane ---------------------------------------------------------
read UT1 UD1 <<EOF
$(st 1 '"%d %d" % (s["io"]["cluster_udp"]["tx_syscalls"], s["io"]["cluster_udp"]["tx_datagrams"])')
EOF
read VR1 VD1 <<EOF
$(st 2 '"%d %d" % (s["io"]["cluster_udp"]["rx_syscalls"], s["io"]["cluster_udp"]["rx_datagrams"])')
EOF
dut=$((UT1 - UT0)); dud=$((UD1 - UD0)); dvr=$((VR1 - VR0)); dvd=$((VD1 - VD0))
[ "$dud" -gt 0 ] && ok "2. the writer sent $dud cluster datagrams" || bad "2. no cluster datagrams sent ($dud)"
[ "$dut" = "$dud" ] && ok "2. in $dut sendto() calls - 1.0 datagrams per syscall, the figure S364 is to raise" ||
	bad "2. $dut syscalls for $dud datagrams (want equal until S364)"
[ "$dvd" -gt 0 ] && [ "$dvr" -gt "$dvd" ] && ok "2. a member received $dvd in $dvr recvmsg() calls (each drain ends in an empty poll)" ||
	bad "2. member rx: $dvr syscalls, $dvd datagrams"
[ -n "$(metric 1 'perfcached_udp_datagrams_total{plane="cluster",dir="tx"}')" ] &&
	ok "2. /metrics carries the UDP counters" || bad "2. /metrics lacks perfcached_udp_datagrams_total"

# ---- 3. reset ----------------------------------------------------------------------------
M0=$(metric 1 'perfcached_udp_syscalls_total{plane="cluster",dir="tx"}')
python3 -c 'import socket
s=socket.create_connection(("127.0.0.1",19611),5); s.sendall(b"*2\r\n$6\r\nCONFIG\r\n$9\r\nRESETSTAT\r\n"); print(s.recv(64))' > /dev/null 2>&1
S1=$(st 1 's["io"]["cluster_udp"]["tx_syscalls"]'); M1=$(metric 1 'perfcached_udp_syscalls_total{plane="cluster",dir="tx"}')
[ "${S1:-x}" -lt "$UT1" ] 2>/dev/null && ok "3. a stats reset starts /stats io again ($UT1 -> $S1)" || bad "3. /stats io after a reset: $S1 (was $UT1)"
[ "${M1:-0}" -ge "${M0:-1}" ] && ok "3. /metrics did not fall ($M0 -> $M1)" || bad "3. /metrics fell: $M0 -> $M1"

e=$(cat "$D"/n*.log | grep -cE ' (ERROR|CRIT)')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || bad "$e ERROR/CRIT: $(grep -hE ' (ERROR|CRIT)' "$D"/n*.log | head -2)"
echo "iostattest: $pass passed, $fail failed"
[ $fail -eq 0 ]
