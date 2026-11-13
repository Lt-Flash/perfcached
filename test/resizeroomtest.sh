#!/bin/sh
# resizeroomtest.sh - a resize whose copy finds the arena full does not complete, nor drop the original;
# PERFCACHED_TEST_RS_COPY_FAIL=<file> fails "nf:" copies while <file> exists, a state no suite can hold.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -x "$CLI" ] || { echo "resizeroomtest: $CLI not built - SKIPPED, and a skip is not a pass"; exit 0; }
D=$(mktemp -d /var/tmp/pcrsr.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
if ss -ltn 2>/dev/null | grep -qE ":1762[12][[:space:]]"; then
	echo "resizeroomtest: port 17621/17622 already bound" >&2; exit 1
fi
cat > "$D/n.conf" <<EOF
[daemon]
workers = 2
log_level = notice
allow_create = yes
[memory]
arena_mb = 64
[secrets]
client = rsr-client-secret
enable = rsr-enable
[listen]
tcp = 127.0.0.1:17621
resp = 127.0.0.1:17622
plaintext = loopback
[collection 0]
buckets_log2 = 8
autoscale = off
EOF
chmod 600 "$D/n.conf"
: > "$D/nofit"
PERFCACHED_TEST_RS_COPY_FAIL="$D/nofit" "$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0; while [ $i -lt 80 ] && ! grep -q "perfcached ready" "$D/n.log"; do sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { bad "daemon did not start: $(tail -2 "$D/n.log" | tr '\n' ' ' | cut -c1-150)"; echo "resizeroomtest: $pass passed, $fail failed"; exit 1; }

keys() { # keys <set|check> - the 600 keys over RESP; check prints the wrong ones' count and the first few
	python3 - "$1" <<'P'
import socket, sys
k = socket.create_connection(("127.0.0.1", 17622), 10); f = k.makefile("rb")
names = ["nf:%d" % i if i % 3 == 0 else "ok:%d" % i for i in range(600)]
def enc(*a):
    return b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in (y.encode() for y in a))
if sys.argv[1] == "set":
    k.sendall(b"".join(enc("SET", n, "v-" + n) for n in names))
    bad = [n for n in names if not f.readline().startswith(b"+")]
else:
    k.sendall(b"".join(enc("GET", n) for n in names))
    bad = []
    for n in names:
        l = f.readline()
        v = f.read(int(l[1:]) + 2)[:-2].decode() if l.startswith(b"$") and not l.startswith(b"$-1") else None
        if v != "v-" + n: bad.append(n)
print(len(bad), " ".join(bad[:5]))
P
}
set -- $(keys set); [ "$1" = 0 ] || bad "the 600 SETs: $*"
R=$("$CLI" -q -h 127.0.0.1 -p 17621 -E rsr-enable resize 0 10 2>&1 | head -1)
case "$R" in *'"resizing"'*) ;; *) bad "resize refused: $R" ;; esac
sleep 4
if grep -q "resize complete" "$D/n.log"; then
	bad "A. the resize completed while 200 copies could not be made: $(grep -m1 'resize complete' "$D/n.log" | cut -c1-200)"
else
	grep -q "not copied into the new table" "$D/n.log" &&
		ok "A. the resize waits while copies find no room, and says so ($(grep -m1 -o '[0-9]* record(s) not copied' "$D/n.log"))" ||
		bad "A. not complete, but nothing said: $(tail -2 "$D/n.log" | cut -c1-160 | tr '\n' '|')"
fi
set -- $(keys check)
[ "$1" = 0 ] && ok "B. while it waits, all 600 keys read their values" || bad "B. while it waits: $1 of 600 wrong ($2 $3 $4)"
rm -f "$D/nofit"
i=0; while [ $i -lt 100 ] && ! grep -q "resize complete" "$D/n.log"; do sleep 0.2; i=$((i+1)); done
grep -q "resize complete" "$D/n.log" && ok "C. room again: the resize completes ($(grep -m1 -o 'resize complete.*' "$D/n.log" | cut -c1-120))" ||
	bad "C. the resize never completed: $(tail -2 "$D/n.log" | cut -c1-160 | tr '\n' '|')"
set -- $(keys check)
[ "$1" = 0 ] && ok "D. after it, all 600 keys read their values" || bad "D. after the resize: $1 of 600 lost ($2 $3 $4)"
grep -qE "CRIT|ERROR" "$D/n.log" && bad "the node logged: $(grep -m2 -E 'CRIT|ERROR' "$D/n.log" | tr '\n' ' ' | cut -c1-200)"
echo "resizeroomtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
