#!/bin/sh
# resptest.sh <resptool binary> [redis-server] - func_perfd's RESP client (resp.c) against Redis, no
# Asterisk; LOCAL Redis only: started here on 127.0.0.1, no persistence, killed by pid on exit.
set -u

TOOL=${1:?usage: resptest.sh <resptool> [redis-server]}
REDIS=${2:-redis-server}
CLI=redis-cli
GREP=/bin/grep
D=$(mktemp -d /var/tmp/resptest.XXXXXX)
PA= PB= BG=
cleanup() {
	for p in $BG $PA $PB; do
		[ -n "$p" ] && kill -CONT "$p" 2>/dev/null
		[ -n "$p" ] && kill -9 "$p" 2>/dev/null
	done
	rm -rf "$D"
}
trap cleanup EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "ok   $1"; }
bad() { fail=$((fail+1)); echo "FAIL $1"; }

free_port() {
	p=$1
	while ss -ltn | awk '{print $4}' | $GREP -q ":$p\$"; do p=$((p+1)); done
	echo "$p"
}
loopback() {
	case "$1" in 127.0.0.1:*) ;; *) echo "refusing non-loopback server $1"; exit 1 ;; esac
}
start_redis() { # start_redis <port> <var> [extra args...]
	port=$1 var=$2
	shift 2
	"$REDIS" --port "$port" --bind 127.0.0.1 --save '' --appendonly no \
		--dir "$D" --dbfilename "dump$port.rdb" --daemonize no "$@" \
		> "$D/redis$port.log" 2>&1 &
	eval "$var=\$!"
	i=0
	while [ $i -lt 50 ]; do
		$CLI -p "$port" -a pw12345678 --no-auth-warning PING 2>/dev/null | $GREP -q PONG && return 0
		$CLI -p "$port" PING 2>/dev/null | $GREP -q PONG && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "redis on $port did not start"; cat "$D/redis$port.log"; exit 1
}
rc() { rp=$1; shift; $CLI -p "$rp" -a pw12345678 --no-auth-warning "$@" 2>/dev/null; }

A=$(free_port 16410)
start_redis "$A" PA --requirepass pw12345678 --masterauth pw12345678
B=$(free_port $((A+1)))
start_redis "$B" PB --requirepass pw12345678 --masterauth pw12345678 --replicaof 127.0.0.1 "$A"
SA=127.0.0.1:$A SB=127.0.0.1:$B
loopback "$SA"; loopback "$SB"
DEAD=$(free_port 16490)
T() { "$TOOL" "$@" 2>&1; }

# ---- basics --------------------------------------------------------------
out=$(T -s "$SA" -p pw12345678 -C 0 SET k1 hello)
[ "$out" = "STATUS OK
END reconnects 0 server $SA" ] && ok "SET with AUTH" || bad "SET with AUTH: $out"
out=$(T -s "$SA" -p pw12345678 -C 0 GET k1 | head -1)
[ "$out" = "BULK 5 hello" ] && ok "GET hit" || bad "GET hit: $out"
out=$(T -s "$SA" -p pw12345678 -C 0 GET nokey | head -1)
[ "$out" = "NIL" ] && ok "GET miss is NIL" || bad "GET miss: $out"
out=$(T -s "$SA" -p wrongpass -C 0 GET k1 | head -1)
echo "$out" | $GREP -q "^FAIL 127.0.0.1:$A: AUTH refused: " && ok "wrong password refused ($out)" || bad "wrong password: $out"
out=$(T -s "$SA" -u someone -p pw12345678 -C 0 GET k1 | head -1)
echo "$out" | $GREP -qE "AUTH refused: (ERR wrong number of arguments|WRONGPASS)" && ok "username refused (Redis 5: no ACL; 6+: no such user) with the server's text" || bad "username: $out"
out=$(T -s "$SA" -p pw12345678 -C calls GET k1 | head -1)
echo "$out" | $GREP -qE "SELECT calls refused: ERR (invalid DB index|value is not an integer)" && ok "named collection refused" || bad "named collection: $out"
out=$(T -o -s "$SA" -p pw12345678 -C calls | head -1)
echo "$out" | $GREP -qE "^OPEN 1 .*(invalid DB index|value is not an integer)" && ok "open reports the refused SELECT, session still open" || bad "open with name: $out"

# ---- collections, TTL, EXISTS, DEL ---------------------------------------
T -s "$SA" -p pw12345678 -C 3 SET k3 three > /dev/null
[ "$(rc "$A" -n 3 GET k3)" = "three" ] && [ -z "$(rc "$A" -n 0 GET k3)" ] \
	&& ok "collection 3 is db 3, not db 0" || bad "collection 3"
T -s "$SA" -p pw12345678 -C 0 SET kt v EX 3600 > /dev/null
ttl=$(rc "$A" TTL kt)
[ "$ttl" -ge 3590 ] 2>/dev/null && [ "$ttl" -le 3600 ] && ok "SET EX 3600 -> TTL $ttl" || bad "TTL: $ttl"
out=$(T -s "$SA" -p pw12345678 -C 0 EXISTS k1 | head -1)
[ "$out" = "INTEGER 1" ] && ok "EXISTS present" || bad "EXISTS present: $out"
out=$(T -s "$SA" -p pw12345678 -C 0 EXISTS nokey | head -1)
[ "$out" = "INTEGER 0" ] && ok "EXISTS absent" || bad "EXISTS absent: $out"
out=$(T -s "$SA" -p pw12345678 -C 0 -n 2 DEL k1 | head -2 | tr '\n' '|')
[ "$out" = "INTEGER 1|INTEGER 0|" ] && ok "DEL twice: 1 then 0" || bad "DEL twice: $out"

# ---- a bulk over the bound is drained, and the stream stays in step ------
big=$(printf '%0200d' 7)
T -s "$SA" -p pw12345678 -C 0 SET big "$big" > /dev/null
out=$(T -s "$SA" -p pw12345678 -C 0 -m 100 -n 3 GET big | tr '\n' '|')
[ "$out" = "OVERSIZE 200|OVERSIZE 200|OVERSIZE 200|END reconnects 0 server $SA|" ] \
	&& ok "oversize drained, 3 in a row on one connection" || bad "oversize: $out"
out=$(T -s "$SA" -p pw12345678 -C 0 -m 200 GET big | head -1)
[ "$out" = "BULK 200 $big" ] && ok "a bulk exactly at the bound is returned" || bad "bound: $(echo "$out" | cut -c1-40)"

# ---- a server that declares a huge bulk, or trickles one: the call ends, bounded ----
cat > "$D/trickle.py" <<'EOF'
import socket, sys, threading, time
port, head, total = int(sys.argv[1]), sys.argv[2].encode(), int(sys.argv[3])
def serve(c):
    try:
        c.recv(4096)
        c.sendall(head + b"\r\n")
        for _ in range(total):
            c.sendall(b"x")
            time.sleep(0.2)
    except OSError:
        pass
    c.close()
ls = socket.socket()
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("127.0.0.1", port))
ls.listen(8)
while True:
    c, _ = ls.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
EOF
TR=$(free_port 16470)
python3 "$D/trickle.py" "$TR" '$9223372036854775807' 100000 &
BG=$!
sleep 0.5
t0=$(date +%s%N)
out=$(timeout 10 "$TOOL" -s "127.0.0.1:$TR" -t 300 -c 300 -m 100 GET k 2>&1 | head -1)
t1=$(date +%s%N)
ms=$(( (t1 - t0) / 1000000 ))
[ "$out" = "OVERSIZE 9223372036854775807" ] && [ "$ms" -lt 1000 ] \
	&& ok "a declared bulk far over the bound: OVERSIZE at once, not drained ($ms ms)" || bad "huge declared bulk: '$out' in $ms ms"
kill "$BG" 2>/dev/null; wait "$BG" 2>/dev/null
python3 "$D/trickle.py" "$TR" '$1000' 1000 &
BG=$!
sleep 0.5
t0=$(date +%s%N)
out=$(timeout 10 "$TOOL" -s "127.0.0.1:$TR" -t 300 -c 300 GET k 2>&1 | head -1)
t1=$(date +%s%N)
ms=$(( (t1 - t0) / 1000000 ))
echo "$out" | $GREP -q "^FAIL .*call took longer than" && [ "$ms" -lt 3000 ] \
	&& ok "a bulk trickled a byte per 200 ms: the call gives up at its deadline ($ms ms)" || bad "trickled bulk: '$out' in $ms ms"
kill "$BG" 2>/dev/null; wait "$BG" 2>/dev/null
BG=

# ---- a dead first server: the next one answers ---------------------------
out=$(T -s "127.0.0.1:$DEAD" -s "$SA" -p pw12345678 -C 0 GET big | tail -1)
[ "$out" = "END reconnects 0 server $SA" ] && ok "dead first server skipped" || bad "dead first: $out"

# ---- a stalled server costs one io timeout per wait, not libperfd's 5 s --
kill -STOP "$PA"
t0=$(date +%s%N)
out=$(T -s "$SA" -p pw12345678 -t 300 -c 300 -C 0 GET big | head -1)
t1=$(date +%s%N)
kill -CONT "$PA"
ms=$(( (t1 - t0) / 1000000 ))
echo "$out" | $GREP -q "timed out after 300 ms" && [ "$ms" -lt 2000 ] \
	&& ok "stalled server: '$out' in ${ms} ms" || bad "stalled server: $out in ${ms} ms"

# ---- -READONLY: re-dial from the next server and retry -------------------
sleep 1
T -s "$SA" -s "$SB" -p pw12345678 -C 0 -n 40 -w 100 SET swap v > "$D/swap.out" &
BG=$!
sleep 1
rc "$B" REPLICAOF NO ONE > /dev/null
rc "$A" REPLICAOF 127.0.0.1 "$B" > /dev/null
wait "$BG"
BG=
nok=$($GREP -c "^STATUS OK" "$D/swap.out")
nbad=$($GREP -vc "^STATUS OK\|^END" "$D/swap.out")
last=$(tail -1 "$D/swap.out")
if [ "$nok" -eq 40 ] && [ "$nbad" -eq 0 ] && [ "$last" = "END reconnects 1 server $SB" ]; then
	ok "roles swapped mid-run: 40/40 OK, one reconnect, ended on $SB"
else
	bad "roles swapped: ok=$nok other=$nbad last='$last'"; cat "$D/swap.out"
fi

echo "resptest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
