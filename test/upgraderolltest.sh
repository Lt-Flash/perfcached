#!/bin/sh
# upgraderolltest.sh - UP7a (DESIGN 12ns): the roll driver, perfcli upgrade,
# on a three-member loopback fleet with the switch hook in place of the root
# helper.  The "new build" is this same binary restarted by the hook: the
# driver's up-gate is a new pid on the version asked, so the roll is real
# in every step but the install.  Cases, in order:
#   A. a roll: staged on every member (relayed), then one node at a time,
#      members first and the master last, each gated; done
#   B. abort asked while a roll runs: it stops between nodes; resume ends it
#   C. a build that does not come up (the hook kills, does not restart):
#      the driver reverts THAT node (the hook again), says so, stops, and
#      prints the rollback commands; the nodes already rolled stay; resume
#   D. the driver killed mid-roll (SIGKILL after the first gate): resume
#      re-reads the fleet and the journal and rolls only what is left
#   E. rollback: the rolled nodes back in reverse order, the master first
#   F. a member that refuses the artifact (allow_unsigned = no): nothing is
#      switched, the stop names what is missing
#   G. UP12b (12nv): plan reads the candidate's share/perfcached/RELEASE -
#      the tree's own file joins on 2; another version, or a range without
#      the fleet's epoch, is refused before anything is journalled; 2..3
#      names the possible commit; a tarball without the file is allowed
# Usage: test/upgraderolltest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}; CLI=${2:-./perfcli}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
case "$CLI" in /*) ;; *) CLI=$(pwd)/$CLI;; esac
T=$(cd "$(dirname "$0")" && pwd); PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH   # pcnative
D=$(mktemp -d /var/tmp/pcroll.XXXXXX)
trap 'for p in $(cat "$D"/pid* 2>/dev/null); do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 17701 17702 17703; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upgraderolltest: port $p already bound" >&2; exit 1; }
done
VER=$("$BIN" -V | head -1 | awk '{print $2}')
head -c 300000 /dev/urandom > "$D/a.tgz"

node() { # node <n> [extra upgrade lines...]
	n=$1; shift
	mkdir -p "$D/state$n"
	{
		cat <<C
[daemon]
workers = 2
log_level = info
state_dir = $D/state$n
[memory]
arena_mb = 64
[secrets]
client = roll-client-secret
cluster = roll-cluster-secret
enable = roll-enable
[listen]
tcp = 127.0.0.7$n:1770$n
plaintext = loopback
[cluster]
mode = eager
multicast = 239.255.77.71:17671
advertise = 127.0.0.7$n
collections = 0
pull_timeout_ms = 200
negative_ms = 500
tombstone_ms = 2000
[upgrade]
allow_unsigned = yes
C
		for l in "$@"; do printf '%s\n' "$l"; done
		cat <<C
[collection 0]
buckets_log2 = 10
C
	} > "$D/n$n.conf"
	chmod 600 "$D/n$n.conf"
}
start() { # start <n>: the daemon, its pid in $D/pid<n>; waits for ready
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/pid$1"
	i=0; while [ $i -lt 150 ] && ! tail -50 "$D/n$1.log" | grep -q "perfcached ready"; do sleep 0.1; i=$((i+1)); done
	tail -50 "$D/n$1.log" | grep -q "perfcached ready" || { echo "node $1 did not start"; tail -5 "$D/n$1.log"; exit 1; }
}
# the hook: the driver's switch and revert, as the root helper would do them
cat > "$D/hook.sh" <<EOF2
#!/bin/sh
op=\$1 addr=\$2 port=\$3 node=\$4
n=\${addr#127.0.0.7}
echo "\$op \$n" >> "$D/hook.log"
p=\$(cat "$D/pid\$n")
kill \$p 2>/dev/null
i=0; while [ \$i -lt 100 ] && kill -0 \$p 2>/dev/null; do sleep 0.1; i=\$((i+1)); done
if [ "\$op" = switch ] && [ "\${UPROLL_FAIL:-}" = "\$n" ]; then
	echo "not starting node \$n (UPROLL_FAIL)" >> "$D/hook.log"; exit 0
fi
"$BIN" -f "$D/n\$n.conf" >> "$D/n\$n.log" 2>&1 &
echo \$! > "$D/pid\$n"
exit 0
EOF2
chmod 755 "$D/hook.sh"
node 1; node 2; node 3
start 1; start 2; start 3
sleep 3.5

P="$CLI -q -h 127.0.0.71 -p 17701 -E roll-enable"
stats() { python3 - "$@" <<'PY' 2>/dev/null
import json, socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.7%s" % sys.argv[1], 17700 + int(sys.argv[1])), 10))
s = pcnative.call(f, "stats")["result"]
print(eval(sys.argv[2]))
PY
}
i=0; while [ $i -lt 100 ] && [ "$(stats 1 's["cluster"]["peers_up"]')" != 2 ]; do sleep 0.2; i=$((i+1)); done
[ "$(stats 1 's["cluster"]["peers_up"]')" = 2 ] && ok "three members up" || bad "peers_up on node 1: $(stats 1 's["cluster"]["peers_up"]')"
master=$(for n in 1 2 3; do [ "$(stats $n 's["cluster"]["role"]')" = master ] && echo $n; done | head -1)
[ -n "$master" ] && ok "the master is node $master" || bad "no master"
pids() { cat "$D/pid1" "$D/pid2" "$D/pid3" | tr '\n' ' '; }

# ---- A. a roll ---------------------------------------------------------------------
P0=$(pids)
r=$($P upgrade plan "$D/a.tgz" version=$VER switch_cmd="$D/hook.sh" journal="$D/j1" ready_s=20 gate_s=40 2>&1); rc=$?
[ $rc -eq 0 ] && grep -q '^[0-9]* plan artifact=' "$D/j1" && ok "A. plan: the journal holds the artifact, its sha256 and the version" || bad "A. plan rc=$rc: $r"
r=$($P upgrade start journal="$D/j1" 2>&1); rc=$?
[ $rc -eq 0 ] && grep -q 'staged on every member' "$D/j1" && [ "$(grep -c 'event=gated' "$D/j1")" = 3 ] && grep -q '^[0-9]* done version=' "$D/j1" &&
	ok "A. start: staged on every member, three nodes gated, done" || bad "A. start rc=$rc: $(tail -4 "$D/j1" | tr '\n' '|')"
last=$(grep 'event=switching' "$D/j1" | tail -1 | sed 's/.*node=\([0-9]*\).*/\1/')
mnode=$(stats $master 's["cluster"]["node"]' 2>/dev/null || stats $master 's["node"]')
[ "$last" = "$mnode" ] && ok "A. the master (node $mnode) was switched last" || bad "A. last switched node $last, the master was $mnode ($(grep 'event=switching' "$D/j1" | sed 's/.*node=\([0-9]*\).*role=\([a-z]*\).*/\1:\2/' | tr '\n' ' '))"
[ "$(pids)" != "$P0" ] && [ "$(grep -c '^switch ' "$D/hook.log")" = 3 ] && ok "A. the hook switched each node once; every pid is new" || bad "A. hook: $(cat "$D/hook.log" | tr '\n' '|')"
for n in 1 2 3; do [ "$(stats $n 's["state"]')" = ready ] || bad "A. node $n not ready after the roll"; done
r=$($P upgrade status journal="$D/j1" 2>&1)
echo "$r" | grep -q "3 node(s) gated, done" && [ "$(echo "$r" | grep -c ' gated$')" = 3 ] && ok "A. status: 3 gated, done; every node on it" || bad "A. status: $(echo "$r" | tr '\n' '|')"

# ---- B. abort, then resume ----------------------------------------------------------------
$P upgrade plan "$D/a.tgz" version=$VER switch_cmd="$D/hook.sh" journal="$D/j2" ready_s=20 gate_s=40 > /dev/null 2>&1
# the driver clears a stale abort when it starts, so the abort is asked while it runs
$P upgrade start journal="$D/j2" > "$D/d2.out" 2>&1 &
DP=$!
# once the first node is switching, inside the loop: a fixed 3 s (rc15) and
# the "start" line (rc16) both let the roll finish on GitLab cluster2
i=0; while [ $i -lt 600 ] && ! grep -q 'event=switching' "$D/j2"; do sleep 0.1; i=$((i+1)); done
$P upgrade abort journal="$D/j2" > "$D/abort.out" 2>&1
wait $DP; rc=$?
[ $rc -eq 1 ] && grep -q 'stop reason=abort' "$D/j2" && [ "$(grep -c 'event=gated' "$D/j2")" -le 1 ] && ok "B. abort asked while the roll ran: it stopped between nodes ($(grep -c 'event=gated' "$D/j2") gated)" || bad "B. abort rc=$rc (asked after $((i / 10)) s: $(tr '\n' ' ' < "$D/abort.out")): $(tr '\n' '|' < "$D/j2")"
r=$($P upgrade resume journal="$D/j2" 2>&1); rc=$?
[ $rc -eq 0 ] && [ "$(grep -c 'event=gated' "$D/j2")" = 3 ] && ok "B. resume after the abort: the roll finished" || bad "B. resume rc=$rc: $(tail -3 "$D/j2" | tr '\n' '|')"

# ---- C. a build that does not come up: revert that node, stop ----------------------------------
$P upgrade plan "$D/a.tgz" version=$VER switch_cmd="$D/hook.sh" journal="$D/j3" ready_s=5 gate_s=20 > /dev/null 2>&1
# the driver rolls members by CLUSTER node id, lowest first (the master last): the config index whose
# id is the lowest among the members is the one it switches first
first=$(for n in 1 2 3; do [ "$(stats $n 's["cluster"]["role"]')" = master ] || echo "$(stats $n 's["cluster"]["node"]') $n"; done | sort -n | head -1 | cut -d' ' -f2)
r=$(UPROLL_FAIL=$first $P upgrade start journal="$D/j3" 2>&1); rc=$?
[ $rc -eq 1 ] && grep -q 'reason=not-up-in-time' "$D/j3" && grep -q 'event=reverted' "$D/j3" && grep -q 'stop reason=node-[0-9]*-reverted' "$D/j3" &&
	ok "C. the node did not come up in ready_s: reverted by the hook, the roll stopped" || bad "C. rc=$rc: $(tail -4 "$D/j3" | tr '\n' '|')"
echo "$r" | grep -q 'rollback this node:' && echo "$r" | grep -q 'upgrade rollback journal=' && ok "C. the rollback commands were printed" || bad "C. output: $(echo "$r" | tail -4 | tr '\n' '|')"
[ "$(grep -c 'event=gated' "$D/j3")" = 0 ] && ok "C. nothing was gated (the first node failed)" || bad "C. gated: $(grep -c 'event=gated' "$D/j3")"
sleep 2; [ "$(stats $first 's["state"]')" = ready ] && ok "C. node $first is ready again on the old build" || bad "C. node $first: $(stats $first 's["state"]')"
r=$($P upgrade resume journal="$D/j3" 2>&1); rc=$?
[ $rc -eq 0 ] && [ "$(grep -c 'event=gated' "$D/j3")" = 3 ] && ok "C. resume: the roll finished" || bad "C. resume rc=$rc: $(tail -3 "$D/j3" | tr '\n' '|')"

# ---- D. the driver killed after the first gate; resume -------------------------------------------
$P upgrade plan "$D/a.tgz" version=$VER switch_cmd="$D/hook.sh" journal="$D/j4" ready_s=20 gate_s=40 > /dev/null 2>&1
h0=$(grep -c '^switch ' "$D/hook.log")
$P upgrade start journal="$D/j4" > "$D/d4.out" 2>&1 &
DP=$!
i=0; while [ $i -lt 600 ] && [ "$(grep -c 'event=gated' "$D/j4" 2>/dev/null)" -lt 1 ]; do sleep 0.1; i=$((i+1)); done
kill -9 $DP 2>/dev/null; wait $DP 2>/dev/null
g1=$(grep -c 'event=gated' "$D/j4")
[ "$g1" -ge 1 ] && [ "$g1" -le 2 ] && ok "D. the driver was killed after $g1 node(s) gated" || bad "D. gated before the kill: $g1"
sleep 3
r=$($P upgrade resume journal="$D/j4" 2>&1); rc=$?
h1=$(grep -c '^switch ' "$D/hook.log")
[ $rc -eq 0 ] && [ "$(grep -c 'event=gated' "$D/j4")" = 3 ] && [ $((h1 - h0)) -eq 3 ] &&
	ok "D. resume rolled only what was left: 3 gated in all, the hook switched each node once" || bad "D. resume rc=$rc, hook switches $((h1 - h0)): $(tail -3 "$D/j4" | tr '\n' '|')"

# ---- E. rollback -----------------------------------------------------------------------------
r=$($P upgrade rollback journal="$D/j4" 2>&1); rc=$?
[ $rc -eq 0 ] && [ "$(grep -c 'event=reverted' "$D/j4")" = 3 ] && grep -q 'rollback done' "$D/j4" && ok "E. rollback: three nodes reverted, done" || bad "E. rollback rc=$rc: $(tail -4 "$D/j4" | tr '\n' '|')"
firstback=$(grep 'event=reverting' "$D/j4" | head -1 | sed 's/.*node=\([0-9]*\).*/\1/')
lastrolled=$(grep 'event=gated' "$D/j4" | tail -1 | sed 's/.*node=\([0-9]*\).*/\1/')
[ "$firstback" = "$lastrolled" ] && ok "E. the last rolled node (the master) went back first" || bad "E. first reverted $firstback, last rolled $lastrolled"
r=$($P upgrade status journal="$D/j4" 2>&1)
echo "$r" | grep -q "0 node(s) gated" && ok "E. status after the rollback: nothing gated" || bad "E. status: $(echo "$r" | head -2 | tr '\n' '|')"

# ---- F. a member that refuses the artifact: nothing switched --------------------------------------
kill $(cat "$D/pid3") 2>/dev/null; sleep 1
node 3 "allow_unsigned = no"; start 3; sleep 3
head -c 300000 /dev/urandom > "$D/b.tgz"
$P upgrade plan "$D/b.tgz" version=$VER switch_cmd="$D/hook.sh" journal="$D/j5" ready_s=20 gate_s=40 > /dev/null 2>&1
h0=$(grep -c '^switch ' "$D/hook.log")
r=$($P upgrade start journal="$D/j5" 2>&1); rc=$?
h1=$(grep -c '^switch ' "$D/hook.log")
[ $rc -eq 1 ] && grep -q 'stop reason=not-staged-everywhere' "$D/j5" && grep -q 'event=failed reason=not-staged' "$D/j5" && [ $((h1 - h0)) -eq 0 ] &&
	ok "F. a member refused the artifact: the stop names it, nothing was switched" || bad "F. rc=$rc, switches $((h1 - h0)): $(tail -3 "$D/j5" | tr '\n' '|')"

# ---- G. UP12b: the candidate's own wire epochs, read from the tarball before anything is staged ----
# share/perfcached/RELEASE is what `make install` writes (perfcached.release); the suite builds
# tarballs around it and around edited copies.  The fleet speaks epoch 2 here.
REL=$(dirname "$BIN")/perfcached.release
reltgz() { # reltgz <name> <release file>: a prefix tarball with that RELEASE
	rm -rf "$D/rt"; mkdir -p "$D/rt/perfcached-x/share/perfcached" "$D/rt/perfcached-x/bin"
	cp "$2" "$D/rt/perfcached-x/share/perfcached/RELEASE"; cp "$BIN" "$D/rt/perfcached-x/bin/perfcached"
	tar -C "$D/rt" -czf "$D/$1.tgz" perfcached-x
}
if [ -f "$REL" ]; then
	grep -q "^version=$VER\$" "$REL" && grep -qE '^wire_epoch_min=[0-9]+$' "$REL" && grep -qE '^wire_epoch_max=[0-9]+$' "$REL" &&
		grep -qE '^revision=.' "$REL" && grep -qE '^edition=(clustered|standalone)$' "$REL" &&
		ok "G. perfcached.release names the version -V says, the revision, the edition and the epochs" || bad "G. perfcached.release: $(tr '\n' '|' < "$REL")"
	reltgz g1 "$REL"
	r=$($P upgrade plan "$D/g1.tgz" version=$VER switch_cmd="$D/hook.sh" journal="$D/jg1" 2>&1); rc=$?
	[ $rc -eq 0 ] && echo "$r" | grep -q "the candidate $VER (.*) speaks wire epochs 2\.\.2 - it joins on 2" && grep -q 'candidate_epochs=2\.\.2' "$D/jg1" &&
		ok "G. the tree's own RELEASE: plan reads 2..2, joins on 2, journals it" || bad "G. own RELEASE rc=$rc: $r"
	sed "s/^version=.*/version=9.9.9/" "$REL" > "$D/rel"; reltgz g2 "$D/rel"
	r=$($P upgrade plan "$D/g2.tgz" version=$VER switch_cmd="$D/hook.sh" journal="$D/jg2" 2>&1); rc=$?
	[ $rc -eq 1 ] && echo "$r" | grep -q "is version 9.9.9 .* not the version=$VER asked - refused" && [ ! -s "$D/jg2" ] &&
		ok "G. a tarball of another version than version= asked: refused, nothing journalled" || bad "G. version mismatch rc=$rc: $r"
	sed -e "s/^wire_epoch_min=.*/wire_epoch_min=3/" -e "s/^wire_epoch_max=.*/wire_epoch_max=3/" "$REL" > "$D/rel"; reltgz g3 "$D/rel"
	r=$($P upgrade plan "$D/g3.tgz" version=$VER switch_cmd="$D/hook.sh" journal="$D/jg3" 2>&1); rc=$?
	[ $rc -eq 1 ] && echo "$r" | grep -q "speaks wire epochs 3\.\.3 and the fleet speaks 2 - it could not join" && [ ! -s "$D/jg3" ] &&
		ok "G. a candidate at 3..3 against a fleet on 2: refused before staging, nothing journalled" || bad "G. 3..3 rc=$rc: $r"
	sed -e "s/^wire_epoch_max=.*/wire_epoch_max=3/" "$REL" > "$D/rel"; reltgz g4 "$D/rel"
	r=$($P upgrade plan "$D/g4.tgz" version=$VER switch_cmd="$D/hook.sh" journal="$D/jg4" 2>&1); rc=$?
	[ $rc -eq 0 ] && echo "$r" | grep -q "speaks wire epochs 2\.\.3 - it joins on 2; a commit to 3 is possible once every node runs it" &&
		[ "$(echo "$r" | grep -c '^note: node [0-9]* .* speaks up to 2 - it must be on the candidate before a commit to 3')" = 3 ] &&
		ok "G. a candidate at 2..3: allowed, the commit to 3 named, each of the 3 members noted" || bad "G. 2..3 rc=$rc: $r"
else
	bad "G. $REL is missing - make perfcached.release (check-g-cluster2 builds it)"
fi
r=$($P upgrade plan "$D/b.tgz" version=$VER switch_cmd="$D/hook.sh" journal="$D/jg5" 2>&1); rc=$?
[ $rc -eq 0 ] && echo "$r" | grep -q "the candidate says nothing about its wire epochs .* not checked" &&
	ok "G. a tarball without RELEASE (before 0.7.1): allowed and said so" || bad "G. no RELEASE rc=$rc: $r"

echo "upgraderolltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
