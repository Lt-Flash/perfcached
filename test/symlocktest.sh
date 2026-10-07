#!/bin/sh
# symlocktest.sh - S170a: Symfony's Lock scripts, approved by SHA1 and run
# natively (no Lua) on a lock record.
#
#   1  one node: the scenario in test/symlockscen.py - probe, write and
#      read locks, conflict, re-acquire, promotion and demotion, refresh,
#      release, expiry by the members' ms, the ARGV-time variants, a string
#      key (false, untouched), a hash key (WRONGTYPE) - answers line by
#      line what redis 8.10.2 answered running Symfony's own bodies;
#      SCRIPT LOAD of every Symfony body names it by the same SHA1 the
#      daemon approves; an unknown SHA is NOSCRIPT, an unapproved body is
#      refused on EVAL and SCRIPT LOAD; SCRIPT EXISTS; a lock survives a
#      restart (WAL) and TYPE says zset
#   2  a shard fleet of three: acquire through one non-owner, conflict
#      through the other, release through the other - decided at the owner
#   3  an eager fleet of three, 30 rounds: two clients on two nodes acquire
#      one lock at once - exactly one gets it every round
#   4  the gate: beside a node without the feature (a variant of this tree
#      that does not advertise it, or $2), EVALSHA is refused naming it
# Fail-first: 0.4.6 answers "unknown command" to EVALSHA.
# Usage: test/symlocktest.sh [./perfcached] [older-perfcached-for-part-4]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
OLD=${2:-}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 17821 17822 17823 17881 17882 17883 17891 17892 17893; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "symlocktest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcslock.XXXXXX)
trap 'for f in "$D"/n*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; [ -n "${KEEP:-}" ] && cp -r "$D" "$KEEP"; rm -rf "$D"' EXIT INT TERM

conf() { # conf <n> <mode: none|eager|shard>
	mkdir -p "$D/s$1/wal"
	case $2 in
	none) CL= ;;
	*) CL="[cluster]
multicast = 239.255.80.42:17829
advertise = 127.0.42.$1
mode = $2
collections = 0" ;;
	esac
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = sl-client-secret
cluster = sl-cluster-secret
[listen]
tcp = 127.0.0.1:1782$1
resp = 127.0.0.1:1788$1
http = 127.0.0.1:1789$1
plaintext = loopback
$CL
[collection 0]
buckets_log2 = 12
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
C
	chmod 600 "$D/n$1.conf"
}
start() {
	: > "$D/n$1.log"
	"$2" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 300 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "  node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; return 1
}
stopn() { [ -f "$D/n$1.pid" ] && { p=$(cat "$D/n$1.pid"); kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; rm -f "$D/n$1.pid"; }; }
# r <n> <args...>: one RESP command, flattened; e <n> <script> <key> <args...>: EVALSHA
r() { n=$1; shift; python3 - "1788$n" "$@" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
a = sys.argv[2:]
s.sendall(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x.encode()), x) for x in a)).encode())
f = s.makefile("rb")
def rd():
    l = f.readline().decode().rstrip("\r\n")
    t, v = l[0], l[1:]
    if t == "*": return "[" + ",".join(rd() for _ in range(int(v))) + "]"
    if t == "$": return "nil" if v == "-1" else f.read(int(v) + 2)[:-2].decode()
    return v if t in ":+" else "-" + v
print(rd())
PY
}
sha() { python3 -c "from symfony_lock import SHA; print(SHA['$1'])"; }
e() { n=$1; s=$(sha "$2"); shift 2; r "$n" EVALSHA "$s" 1 "$@"; }
now() { python3 -c 'import time; print("%.4f" % time.time())'; }
fleet() {
	i=0; while [ $i -lt 120 ]; do
		good=1
		for x in "$@"; do
			n=$(curl -s "http://127.0.0.1:1789$x/members" | python3 -c 'import json,sys; print(sum(1 for m in json.load(sys.stdin).get("members",[]) if m.get("gone_s",-1) < 0))' 2>/dev/null)
			[ "$n" = "$#" ] || good=0
		done
		[ $good = 1 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
fwd_sent() { curl -s "http://127.0.0.1:1789$1/stats" | python3 -c '
import json, sys
def find(x, k):
    if isinstance(x, dict):
        if k in x: return x[k]
        for v in x.values():
            r = find(v, k)
            if r is not None: return r
print(find(json.load(sys.stdin), "fwd_sent"))'; }
owned_by() {
	k=1; while [ $k -le 60 ]; do
		key="$2$k"; b=$(fwd_sent "$1")
		r "$1" SET "$key" probe > /dev/null
		a=$(fwd_sent "$1"); r "$1" DEL "$key" > /dev/null
		[ "$a" = "$b" ] && { echo "$key"; return 0; }
		k=$((k+1))
	done
	return 1
}
eq() { [ "$2" = "$3" ] && ok "$1: $2" || bad "$1: got '$2', want '$3'"; }

# ---- 1. one node -----------------------------------------------------------
echo "part 1: one node - the scenario against redis 8.10.2's answers"
conf 1 none; start 1 "$BIN" || exit 1
python3 "$(dirname "$0")/symlockscen.py" 17881 > "$D/scen.out" 2>&1
cat > "$D/want" <<'W'
probe -> 1
exists, absent -> nil
save write tokA 300 s -> 1
pttl in (299000,300000] -> True
TYPE -> zset
save write tokB (held) -> nil
save write tokA again (own) -> 1
exists tokA -> 1
exists tokB -> nil
refresh tokA 600 s -> 1
pttl in (599000,600000] -> True
refresh tokB -> nil
release tokB -> nil
release tokA -> 1
EXISTS -> 0
string key: save -> nil
string key: exists -> nil
string key: refresh -> nil
string key: release -> nil
string kept -> x
read r1 -> 1
read r2 -> 1
write w over readers -> nil
release r1 -> 1
pttl now r2's (199000,200000] -> True
promote r2 to write (sole reader) -> 1
read r3 under a write -> nil
r2 reads again (demote) -> 1
read r3 after the demote -> 1
release r2 -> 1
release r3 -> 1
EXISTS -> 0
short write e1 300 ms -> 1
after 0.5 s: exists e1 -> nil
after 1.3 s: EXISTS -> 0
after 1.3 s: save e2 -> 1
argv variant: save t1 -> 1
argv variant: exists t1 -> 1
hash key: save -> -WRONGTYPE
EVALSHA unknown -> -NOSCRIPT
W
sed -E 's/^ +//; s/ +-> /\t/' "$D/scen.out" > "$D/got.tsv"
sed -E 's/ -> /\t/' "$D/want" > "$D/want.tsv"
while IFS="$(printf '\t')" read -r step want; do
	got=$(awk -F'\t' -v s="$step" '$1 == s {print $2; exit}' "$D/got.tsv")
	[ "$got" = "$want" ] && ok "$step -> $got" || bad "$step: got '$got', Redis said '$want'"
done < "$D/want.tsv"
[ "$(wc -l < "$D/got.tsv")" -ge "$(wc -l < "$D/want.tsv")" ] || bad "the scenario stopped early: $(tail -2 "$D/scen.out" | tr '\n' ' ')"
for s in probe save save_argv read read_argv refresh refresh_argv release exists exists_argv; do
	got=$(python3 - "$s" <<'PY'
import socket, sys
from symfony_lock import BODIES
b = BODIES[sys.argv[1]].encode()
s = socket.create_connection(("127.0.0.1", 17881), timeout=10)
s.sendall(b"*3\r\n$6\r\nSCRIPT\r\n$4\r\nLOAD\r\n$%d\r\n%s\r\n" % (len(b), b))
f = s.makefile("rb"); l = f.readline()
print(f.read(int(l[1:]) + 2)[:-2].decode() if l[:1] == b"$" else l.decode().strip())
PY
)
	[ "$got" = "$(sha "$s")" ] || bad "SCRIPT LOAD of Symfony's $s body: '$got', want $(sha "$s")"
done
ok "SCRIPT LOAD names all ten Symfony bodies by their SHA1"
case "$(r 1 SCRIPT LOAD 'return 1')" in *"not approved"*) ok "SCRIPT LOAD of another body: refused";; *) bad "SCRIPT LOAD of another body: $(r 1 SCRIPT LOAD 'return 1')";; esac
case "$(r 1 EVAL 'return 1' 0)" in *"not approved"*) ok "EVAL of another body: refused";; *) bad "EVAL of another body: $(r 1 EVAL 'return 1' 0)";; esac
eq "SCRIPT EXISTS approved, unknown" "$(r 1 SCRIPT EXISTS "$(sha save)" 0000000000000000000000000000000000000000)" "[1,0]"
eq "EVAL by body runs it" "$(python3 - <<'PY'
import socket
from symfony_lock import BODIES
b = BODIES["probe"].encode()
s = socket.create_connection(("127.0.0.1", 17881), timeout=10)
s.sendall(b"*4\r\n$4\r\nEVAL\r\n$%d\r\n%s\r\n$1\r\n1\r\n$26\r\nsymfony_check_support_time\r\n" % (len(b), b))
print(s.makefile("rb").readline().decode().strip())
PY
)" ":1"
eq "a lock to keep across a restart" "$(e 1 save K "$(now)" tokK 300000)" 1
r 1 PING > /dev/null; sleep 1.5
stopn 1; start 1 "$BIN" || exit 1
eq "after a restart: still held, still a zset" "$(e 1 exists K "$(now)" tokK)$(r 1 TYPE K)$(e 1 save K "$(now)" tokZ 1000)" 1zsetnil
stopn 1

# ---- 2. a shard fleet ------------------------------------------------------
echo "part 2: a shard fleet of three - decided at the owner"
for n in 1 2 3; do rm -rf "$D/s$n"; conf $n shard; done
for n in 1 2 3; do start $n "$BIN" || exit 1; done
fleet 1 2 3 || { bad "the shard fleet did not form"; echo "symlocktest: $pass passed, $fail failed"; exit 1; }
sleep 2
L=$(owned_by 1 slock) || { bad "no key of 60 is owned by node 1"; echo "symlocktest: $pass passed, $fail failed"; exit 1; }
echo "  key $L is owned by node 1"
eq "acquire through node 2" "$(e 2 save "$L" "$(now)" tokA 300000)" 1
eq "conflict through node 3" "$(e 3 save "$L" "$(now)" tokB 300000)" nil
eq "exists through node 3" "$(e 3 exists "$L" "$(now)" tokA)" 1
eq "the owner holds a lock record" "$(r 1 TYPE "$L")" zset
eq "release through node 3" "$(e 3 release "$L" tokA)" 1
eq "free again, through node 2" "$(e 2 save "$L" "$(now)" tokB 300000)" 1
e=$(cat "$D"/n*.log | grep -cE ' (ERROR|CRIT)')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || bad "$e ERROR/CRIT: $(grep -hE ' (ERROR|CRIT)' "$D"/n*.log | head -2)"
for n in 1 2 3; do stopn $n; done

# ---- 3. an eager fleet race --------------------------------------------------
echo "part 3: an eager fleet of three - 30 races, one holder each"
for n in 1 2 3; do rm -rf "$D/s$n"; conf $n eager; done
for n in 1 2 3; do start $n "$BIN" || exit 1; done
fleet 1 2 3 || { bad "the eager fleet did not form"; echo "symlocktest: $pass passed, $fail failed"; exit 1; }
sleep 2
python3 - > "$D/race.out" 2>&1 <<'PY'
import socket, threading, time
from symfony_lock import SHA
def conn(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=10); return s, s.makefile("rb")
def ev(c, *a):
    s, f = c
    a = ["EVALSHA", SHA["save"], "1"] + list(a)
    s.sendall(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
    l = f.readline().decode().rstrip("\r\n")
    if l.startswith("$"):
        n = int(l[1:]); return None if n < 0 else f.read(n + 2)[:-2].decode()
    return l
c2, c3 = conn(17882), conn(17883)
bad = 0
for rnd in range(30):
    key = "race%d" % rnd; res = {}
    bar = threading.Barrier(2)
    def go(name, c, tok):
        bar.wait(); res[name] = ev(c, key, "%.4f" % time.time(), tok, "300000")
    t2 = threading.Thread(target=go, args=("B", c2, "tokB"))
    t3 = threading.Thread(target=go, args=("C", c3, "tokC"))
    t2.start(); t3.start(); t2.join(); t3.join()
    wins = [n for n, v in res.items() if v == ":1"]
    if len(wins) != 1:
        bad += 1; print("ROUND %d: %s" % (rnd, res))
print("BAD", bad)
PY
b=$(sed -n 's/^BAD //p' "$D/race.out")
[ "$b" = 0 ] && ok "30 rounds: exactly one of two racing clients got the lock each time" \
	|| bad "$b of 30 rounds wrong: $(grep ROUND "$D/race.out" | head -3 | tr '\n' ' ') $(tail -2 "$D/race.out" | tr '\n' ' ')"
for n in 1 2 3; do stopn $n; done

# ---- 4. the gate ---------------------------------------------------------------
if [ -z "$OLD" ]; then
	echo "--- building the variant that does not advertise the lock feature"
	mkdir -p "$D/t"
	tar -cf - --exclude=./.git --exclude='*.o' --exclude='*.a' . 2>/dev/null |
		(cd "$D/t" && tar -xf -) || { bad "cannot copy the tree"; exit 1; }
	sed -i 's/CLMEMB_FEAT_SLOCK;                             \/\* S170a \*\//0;/' "$D/t/src/cluster.c"
	grep -q 'CLMEMB_FEAT_HREAD |  /\* S314, S319 \*/' "$D/t/src/cluster.c" && ! grep -q 'CLMEMB_FEAT_SLOCK;' "$D/t/src/cluster.c" \
		|| { bad "the variant still advertises the lock feature"; exit 1; }
	if make -C "$D/t" -j"$(nproc 2>/dev/null || echo 4)" perfcached > "$D/build.old.log" 2>&1; then
		OLD="$D/t/perfcached"
	else
		bad "the variant did not build: $(tail -3 "$D/build.old.log" | tr '\n' ' ')"
	fi
fi
if [ -n "$OLD" ] && [ -x "$OLD" ]; then
	echo "part 4: the gate - node 3 does not advertise the lock feature"
	for n in 1 2 3; do rm -rf "$D/s$n"; conf $n eager; done
	start 1 "$BIN" && start 2 "$BIN" && start 3 "$OLD" || exit 1
	fleet 1 2 3 || { bad "the mixed fleet did not form"; echo "symlocktest: $pass passed, $fail failed"; exit 1; }
	sleep 2
	g=$(e 1 save G "$(now)" tokA 300000)
	case "$g" in *"need every node in the fleet on 0.5.0 or later - node "*) ok "4. refused, naming the node: $g";;
		*) bad "4. with an older member EVALSHA answered: $g";; esac
	eq "4. and nothing was stored" "$(r 1 EXISTS G)" 0
	for n in 1 2 3; do stopn $n; done
fi
echo "symlocktest: $pass passed, $fail failed"
[ $fail -eq 0 ]
