#!/bin/sh
# privmovetest.sh - a node-targeted call does not fail over; a fleet handle's does, raising privilege
# on the new node first.  Usage: test/privmovetest.sh [./perfcached] [./privmovetest] [./syncfailshim.so] [./perfcli]
set -u
BIN=${1:-./perfcached}
PM=${2:-./privmovetest}
SHIM=${3:-./syncfailshim.so}
CLI=${4:-./perfcli}
[ -f "$SHIM" ] || { echo "privmovetest: $SHIM is missing (make syncfailshim.so)" >&2; exit 1; }
case "$SHIM" in /*) ;; *) SHIM="$PWD/$SHIM";; esac
# a dynamic sanitizer runtime must come first in the preload list
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
D=$(mktemp -d /var/tmp/pcpm.XXXXXX)
P1= P2=
trap 'for p in $P1 $P2; do kill -9 $p 2>/dev/null; done; wait 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
SEC=pm-client-secret EN=pm-enable-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19871 19872 19875 19876 19879; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "privmovetest: port $p already bound" >&2; exit 1; }
done
conf() { # conf <i>
	mkdir -p "$D/s$1" "$D/w$1"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = pm-cluster-secret
enable = $EN
[listen]
tcp = 127.0.187.$1:1987$1
http = 127.0.187.$1:1987$(($1 + 4))
plaintext = never
[cluster]
discovery = unicast
port = 19879
seeds = 127.0.187.1
advertise = 127.0.187.$1
mode = eager
collections = c
[collection c]
buckets_log2 = 10
[wal]
dir = $D/w$1
probe = no
fsync = always
C
	chmod 600 "$D/n$1.conf"
}
start() {
	: > "$D/ctl$1"; : > "$D/shim$1"
	PC_SYNCFAIL_CTL="$D/ctl$1" PC_SYNCFAIL_LOG="$D/shim$1" LD_PRELOAD="${SANRT:+$SANRT }$SHIM" \
		"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
}
st() { # st <i> <python expr over r, the /stats document>
	python3 -c 'import json,sys,urllib.request
r=json.load(urllib.request.urlopen("http://127.0.187.%s:1987%d/stats" % (sys.argv[1], int(sys.argv[1]) + 4),timeout=3))
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
cli() { # cli <i> <request> [priv]: perfcli's answer, or its error
	printf '%s\n' "$2" | "$CLI" -h 127.0.187.$1 -p 1987$1 -a "$SEC" -q ${3:+-E "$EN"} 2>&1
}
restore() { # restore <key> <value>: the request
	echo '{"method":"restore","params":{"col":"c","policy":"newer","records":[{"k":"'$1'","v":"'$2'","ver":1}]}}'
}
get() { cli "$1" '{"method":"get","params":{"col":"c","key":"'$2'"}}'; }
fail_node() { # fail_node <i>: its WAL syncs refused, and writes until it is FAILED
	echo wal > "$D/ctl$1"
	k=0
	while [ $k -lt 50 ]; do
		cli "$1" '{"method":"set","params":{"col":"c","key":"f'$1'-'$k'","value":"x"}}' > /dev/null
		[ "$(st "$1" 'r["state"]')" = failed ] && return 0
		sleep 0.2; k=$((k+1))
	done
	return 1
}
field() { echo "$2" | sed -n "s/.* $1=\\([^ ]*\\).*/\\1/p"; }

conf 1; conf 2
start 1; sleep 0.5; start 2
k=0
until [ "$(st 1 'r["state"]')$(st 2 'r["state"]')" = readyready ] && \
      [ "$(st 1 'r["cluster"]["peers_up"]')$(st 2 'r["cluster"]["peers_up"]')" = 11 ]; do
	k=$((k+1)); [ $k -ge 300 ] && { bad "the fleet did not form: $(tail -2 "$D/n2.log")"; exit 1; }
	sleep 0.2
done

fail_node 1 && [ -s "$D/shim1" ] \
	&& ok "node 1 failed: the shim refused $(wc -l < "$D/shim1") WAL sync(s)" \
	|| { bad "node 1 did not fail (state $(st 1 'r["state"]'), shim refused $(wc -l < "$D/shim1")) - nothing below tests a refusal"; exit 1; }

echo "--- 1: perfcli names node 1"
R=$(cli 1 "$(restore k1 cli)" priv)
echo "$R" | grep -q 'not READY' && ! echo "$R" | grep -q privileged \
	&& ok "1. perfcli -E restore to node 1 answers node 1's refusal ($R)" \
	|| bad "1. perfcli -E restore to node 1 answered: $R"
G=$(get 2 k1)
echo "$G" | grep -qE '"found": ?false' \
	&& ok "1. nothing landed on node 2 ($G)" || bad "1. node 2 holds k1: $G"

echo "--- 2: a fleet handle moves, raised"
O=$("$PM" 127.0.187.1 19871 "$SEC" "$EN" k2 2>&1)
G=$(get 2 k2)
b=$(field before "$O"); a=$(field after "$O"); f=$(field failovers "$O")
case "$O" in "RESULT rc=0 "*) t=1;; *) t=0;; esac
[ $t = 1 ] && [ -n "$b" ] && [ "$b" != "$a" ] && [ "${f:-0}" -ge 1 ] && echo "$O" | grep -qE '"stored": ?1[,}]' \
	&& echo "$G" | grep -qE '"value": ?"moved"' \
	&& ok "2. the restore moved from node $b to node $a with privilege raised there, and landed ($G)" \
	|| bad "2. the fleet handle: $O; node 2 get k2: $G"

echo "--- 2b: raised with a plain enable command, raised again after the move"
O=$("$PM" 127.0.187.1 19871 "$SEC" "$EN" k2b plain 2>&1)
G=$(get 2 k2b)
b=$(field before "$O"); a=$(field after "$O")
case "$O" in "RESULT rc=0 "*) t=1;; *) t=0;; esac
[ $t = 1 ] && [ -n "$b" ] && [ "$b" != "$a" ] && echo "$O" | grep -qE '"stored": ?1[,}]' \
	&& echo "$G" | grep -qE '"value": ?"moved"' \
	&& ok "2b. a plain enable command: the restore moved from node $b to node $a raised, and landed ($G)" \
	|| bad "2b. the plain-enable handle: $O; node 2 get k2b: $G"

echo "--- 2c: a plain disable command stays dropped across the move"
O=$("$PM" 127.0.187.1 19871 "$SEC" "$EN" k2c disable 2>&1)
G=$(get 2 k2c)
b=$(field before "$O"); a=$(field after "$O")
case "$O" in *"SET rc=0 "*"RESULT rc=-1 "*"err="*privileged*) t=1;; *) t=0;; esac
[ $t = 1 ] && [ -n "$b" ] && [ "$b" != "$a" ] && echo "$G" | grep -qE '"found": ?false' \
	&& ok "2c. disabled, then moved from node $b to node $a by a set: the restore there was refused unprivileged ($G)" \
	|| bad "2c. the disabled handle: $(echo "$O" | tr '\n' ' '); node 2 get k2c: $G"

fail_node 2 && [ -s "$D/shim2" ] \
	&& ok "node 2 failed too: the shim refused $(wc -l < "$D/shim2") WAL sync(s)" \
	|| bad "node 2 did not fail (state $(st 2 'r["state"]')) - check 3 tests nothing"

echo "--- 3: both refuse"
O=$("$PM" 127.0.187.1 19871 "$SEC" "$EN" k3 2>&1)
n=$(echo "$O" | grep -o 'not READY' | wc -l); b=$(field before "$O")
case "$O" in "RESULT rc=-1 "*"err=node $b: "*"; then node "*) t=1;; *) t=0;; esac
[ $t = 1 ] && [ "$n" = 2 ] && ! echo "$O" | grep -q privileged \
	&& ok "3. the error names both refusals, node 1's first ($O)" \
	|| bad "3. the fleet handle, both refusing: $O"

# expected CRITs: the refused sync, the FAILED it causes, and the node.state and fleet.majority
# alerts that follow; any other is a failure
SYNCRIT='CRIT.*wal: [a-z_]*sync failed (.*) - marking this node FAILED'
for i in 1 2; do
	grep -h 'an fsync has been stuck' "$D/n$i.log" 2>/dev/null | head -n 1 | sed "s/^/  ..   not counted, the runner's storage stalled: /"
	grep -q "$SYNCRIT" "$D/n$i.log" \
		|| bad "node $i: no CRIT names the refused sync: $(grep -m1 CRIT "$D/n$i.log" | cut -c1-160)"
	c=$(grep CRIT "$D/n$i.log" | grep -v -e "$SYNCRIT" \
		-e 'CRIT.*node state -> FAILED: the device refused a WAL write or sync' \
		-e 'CRIT.*alert: node FAILED: the device refused a write-ahead-log write or sync' \
		-e 'CRIT.*alert: only [0-9]* of the fleet.s [0-9]* members are ready - below a majority' \
		-e 'an fsync has been stuck')
	[ -n "$c" ] && bad "node $i logged another CRIT: $(echo "$c" | cut -c1-160 | tr '\n' '|')"
done
echo "privmovetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
