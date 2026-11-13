#!/bin/sh
# asyncstatetest.sh - an async handle routes to a node that joins once it says ready, by pushes or by
# refresh_ms, never before (test/asyncstatetest.c).  Usage: test/asyncstatetest.sh [./perfcached] [./asyncstatetest]
set -u
BIN=${1:-./perfcached}
AT=${2:-./asyncstatetest}
D=$(mktemp -d /var/tmp/pcast.XXXXXX)
P1= P2= P3= CP=
trap 'for p in $P1 $P2 $P3 $CP; do kill -9 $p 2>/dev/null; done; wait 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
SEC=ast-client-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19480 19481 19482 19483 19489; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "asyncstatetest: port $p already bound" >&2; exit 1; }
done
for i in 1 2 3; do
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = ast-cluster-secret
[listen]
tcp = 127.0.171.$i:19480
http = 127.0.171.$i:1948$i
plaintext = never
[cluster]
discovery = unicast
port = 19489
seeds = 127.0.171.1
advertise = 127.0.171.$i
mode = eager
collections = c
[collection c]
buckets_log2 = 10
C
	chmod 600 "$D/n$i.conf"
done
up() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & eval "P$1=\$!"; }
st() { # st <i> <python expr over r, the /stats document>
	python3 -c 'import json,sys,urllib.request
r=json.load(urllib.request.urlopen("http://127.0.171.%s:1948%s/stats" % (sys.argv[1], sys.argv[1]),timeout=3))
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
listed() { python3 -c 'import json,urllib.request
r=json.load(urllib.request.urlopen("http://127.0.171.1:19481/members",timeout=3))
print(sum(1 for m in r.get("members") or [] if m.get("port")))' 2>/dev/null || echo "?"; }
now_ms() { date +%s%3N; }

up 1; sleep 0.3; up 2
k=0
while [ $k -lt 150 ]; do
	[ "$(st 1 'r["state"]')$(st 2 'r["state"]')" = readyready ] && [ "$(listed)" = 2 ] && break
	sleep 0.2; k=$((k+1))
done
[ "$(listed)" = 2 ] || { echo "  FAIL nodes 1 and 2 did not form: $(tail -2 "$D/n1.log")"; exit 1; }

arm() { # arm <name> <push> <refresh_ms>
	A=$1 T= T1= N= S=
	"$AT" 127.0.171.1 19480 "$SEC" 127.0.171.3 "$2" "$3" 12000 > "$D/$A.out" 2>&1 &
	CP=$!
	k=0
	while [ $k -lt 100 ] && ! grep -q '^learned \|^FAIL' "$D/$A.out"; do sleep 0.1; k=$((k+1)); done
	L=$(grep '^learned ' "$D/$A.out")
	case "$L" in
	*" members=2 n=0") ;;
	*) bad "$A: the handle did not learn nodes 1 and 2 alone: ${L:-$(cat "$D/$A.out")}"; kill $CP 2>/dev/null; CP=; return ;;
	esac
	up 3
	T=
	k=0
	while [ $k -lt 400 ]; do
		[ "$(st 3 'r["state"]')" = ready ] && { T=$(now_ms); break; }
		sleep 0.05; k=$((k+1))
	done
	wait $CP
	CP=
	[ -n "$T" ] || { bad "$A: node 3 never said ready: $(tail -2 "$D/n3.log")"; return; }
	grep -q '^end ' "$D/$A.out" || { bad "$A: the client did not finish: $(tail -2 "$D/$A.out")"; return; }
	T1=$(awk '/^t=/ { split($1, a, "="); split($2, b, "="); if (b[2] > 0) { print a[2]; exit } }' "$D/$A.out")
	N=$(sed -n 's/^end n=\([0-9]*\) .*/\1/p' "$D/$A.out")
	S=$(sed -n 's/^end .* sockets=\([0-9]*\)$/\1/p' "$D/$A.out")
	R=never; [ -n "$T1" ] && R="+$((T1 - T)) ms"
	echo "    $A: node 3 ready at +0 ms, first routed to at $R, at the end $N of 3000 keys, $S socket(s)"
}
down3() {
	kill "$P3" 2>/dev/null; wait "$P3" 2>/dev/null; P3=
	k=0
	while [ $k -lt 150 ] && [ "$(listed)" != 2 ]; do sleep 0.2; k=$((k+1)); done
	[ "$(listed)" = 2 ] || bad "node 3 still listed 30 s after it stopped"
}

arm A 0 60000
[ -n "${N:-}" ] && { [ -z "$T1" ] && [ "$N" = 0 ] && ok "A: pushes ignored and no refresh due: node 3 never routed to (the control)" \
	|| bad "A: node 3 routed to with no push and no refresh: first at ${T1:-?}, $N at the end"; }
down3
arm B 1 60000
[ -n "${N:-}" ] && {
	[ -n "$T1" ] && [ "$T1" -ge $((T - 150)) ] && ok "B: no key routed to node 3 before it said ready" \
		|| bad "B: node 3 routed to before it said ready, or never (first at ${T1:-never}, ready at $T)"
	[ -n "$T1" ] && [ "$T1" -le $((T + 3000)) ] && [ "$N" -ge 600 ] && [ "$N" -le 1400 ] && \
		ok "B: by the state push, node 3 routed to within 3 s of ready ($N of 3000 keys)" \
		|| bad "B: node 3 not routed to by the push within 3 s of ready (first at ${T1:-never}, ready at $T, $N at the end)"
	[ "$S" = 1 ] && ok "B: the async handle holds its one socket - no standby opened behind its caller" \
		|| bad "B: the async handle holds $S sockets - a standby its caller never polls"
}
down3
arm C 0 2000
[ -n "${N:-}" ] && {
	[ -n "$T1" ] && [ "$T1" -ge $((T - 150)) ] && [ "$T1" -le $((T + 3500)) ] && [ "$N" -ge 600 ] && [ "$N" -le 1400 ] && \
		ok "C: pushes ignored, the refresh at 2 s routes to node 3 within 3.5 s of ready ($N of 3000 keys)" \
		|| bad "C: no refresh brought node 3 in (first at ${T1:-never}, ready at $T, $N at the end)"
}
if [ $fail -gt 0 ]; then
	for f in A B C; do [ -f "$D/$f.out" ] && { echo "  --- $f"; sed 's/^/    /' "$D/$f.out" | tail -12; }; done
	echo "  --- n1"; grep -a -E 'cluster:|joined|expelled|state' "$D/n1.log" | tail -15 | cut -c1-200 | sed 's/^/    /'
fi
echo "asyncstatetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
