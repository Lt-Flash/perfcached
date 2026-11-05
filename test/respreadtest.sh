#!/bin/sh
# respreadtest.sh - S474: an eager fleet of three, a fourth joins, one dies and returns, the fourth fails and returns, under live handles.
# Usage: test/respreadtest.sh [./perfcached] [./respreadtest] [./syncfailshim.so]
set -u
BIN=${1:-./perfcached}
RT=${2:-./respreadtest}
SHIM=${3:-./syncfailshim.so}
[ -f "$SHIM" ] || { echo "respreadtest: $SHIM is missing (make syncfailshim.so)" >&2; exit 1; }
case "$SHIM" in /*) ;; *) SHIM="$PWD/$SHIM";; esac
# a dynamic sanitizer runtime must come first in the preload list
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
D=$(mktemp -d /var/tmp/pcrsp.XXXXXX)
P1= P2= P3= P4= RP=
trap 'for p in $P1 $P2 $P3 $P4 $RP; do kill -9 $p 2>/dev/null; done; wait 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
SEC=rsp-client-secret
for p in 19651 19652 19653 19654 19661 19662 19663 19664 19670; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "respreadtest: port $p already bound" >&2; exit 1; }
done
conf() { # conf <i>
	mkdir -p "$D/s$1"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = rsp-cluster-secret
[listen]
tcp = 127.0.165.$1:1965$1
http = 127.0.165.$1:1966$1
plaintext = never
[cluster]
discovery = unicast
port = 19670
seeds = 127.0.165.1
advertise = 127.0.165.$1
mode = eager
collections = c
[collection c]
buckets_log2 = 10
C
	# all log, or the fleet refuses a mixed config; the fourth's refused sync fails it
	mkdir -p "$D/w$1"
	cat >> "$D/n$1.conf" <<C
[wal]
dir = $D/w$1
probe = no
fsync = always
C
	chmod 600 "$D/n$1.conf"
}
start() {
	if [ "$1" = 4 ]; then
		: > "$D/ctl"; : >> "$D/shim"
		PC_SYNCFAIL_CTL="$D/ctl" PC_SYNCFAIL_LOG="$D/shim" LD_PRELOAD="${SANRT:+$SANRT }$SHIM" "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	else
		"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	fi
	eval "P$1=\$!"
}
st() { # st <i> <python expr over r, the /stats document>
	python3 -c 'import json,sys,urllib.request
r=json.load(urllib.request.urlopen("http://127.0.165.%s:1966%s/stats" % (sys.argv[1], sys.argv[1]),timeout=3))
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
wait_ready() { # wait_ready <peers> <i>...
	want=$1; shift
	k=0
	while [ $k -lt 300 ]; do
		good=1
		for i in "$@"; do
			[ "$(st "$i" 'r["state"]')" = ready ] && [ "$(st "$i" 'r["cluster"]["peers_up"]')" = "$want" ] || good=0
		done
		[ $good = 1 ] && return 0
		sleep 0.2; k=$((k+1))
	done
	return 1
}
cue() { # cue <word>: the client asked for it, or it is gone
	k=0
	while [ $k -lt 1200 ]; do
		grep -q "^$1\$" "$D/out" && return 0
		kill -0 "$RP" 2>/dev/null || return 1
		sleep 0.1; k=$((k+1))
	done
	return 1
}
for i in 1 2 3 4; do conf $i; done
for i in 1 2 3; do start $i; sleep 0.3; done
wait_ready 2 1 2 3 || { echo "  FAIL the fleet did not form: $(tail -2 "$D/n1.log")"; exit 1; }
"$RT" 127.0.165.1 127.0.165.4 19651 19652 19653 19654 "$SEC" > "$D/out" 2>&1 &
RP=$!
if cue ADD-NODE; then
	start 4
	wait_ready 3 4 || echo "  ..   the fourth node did not join: $(grep -E 'ERROR|WARNING' "$D/n4.log" | tail -2 | cut -c1-160)"
fi
if cue KILL-NODE; then
	kill -9 "$P2"; wait "$P2" 2>/dev/null; P2=
fi
if cue RESTART-NODE; then
	start 2
fi
if cue FAIL-NODE; then
	echo wal > "$D/ctl"
fi
SAID=
if cue RESTORE-NODE; then
	SAID="node 4 was $(st 4 'r["state"]'), the shim refused $(wc -l < "$D/shim") sync(s)"
	kill -9 "$P4"; wait "$P4" 2>/dev/null
	start 4
fi
wait "$RP"
rc=$?
RP=
grep -vE '^(ADD-NODE|KILL-NODE|RESTART-NODE|FAIL-NODE|RESTORE-NODE)$' "$D/out"
case "$SAID" in
"node 4 was failed, the shim refused "[1-9]*) echo "  ok   $SAID" ;;
*) echo "  FAIL the failure the test relies on did not happen: $SAID"; rc=1 ;;
esac
exit $rc
