#!/bin/sh
# statedirtest.sh — S80: a rejoining node keeps its identity and its id,
# because its state lives in an admin-named directory, not in the WAL.
#
#  1. [daemon] state_dir set, no WAL: a clean restart (SIGTERM, so a
#     GOODBYE goes out) comes back with the SAME identity and the SAME
#     node id, identity_durable true.  This is the fleet's case and the
#     one that failed for a month.
#  2. no state_dir, no WAL: the identity CHANGES, and the log says which
#     key is unset.  Ephemeral remains legal; it must not be silent.
#  3. state_dir on tmpfs: survives the daemon restart; the log says tmpfs.
#  4. [wal] dir and no state_dir: the WAL directory keeps serving as the
#     state directory (an upgrade changes nothing) and the log says so.
#  5. state_dir introduced beside an existing WAL: the identity already
#     in the WAL directory is carried across, not silently reset.
#  6. S92: a node that FOUNDS adopts the id its identity proposes,
#     instead of stamping itself node 1.  Cases 1-5 all rejoin a LIVE
#     master, which has always honoured a proposal; the founder grants
#     its own id and was the one path that threw the proposal away.
# Two nodes throughout so a master is there to grant the rejoiner its
# id; node 2 is the one restarted.
# Usage: test/statedirtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcsd.XXXXXX)
TM=$(mktemp -d /dev/shm/pcsd.XXXXXX 2>/dev/null || mktemp -d /tmp/pcsd.XXXXXX)
P1= P2= P3=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; [ -n "$P2" ] && kill -9 $P2 2>/dev/null; [ -n "$P3" ] && kill -9 $P3 2>/dev/null; [ -n "$P7" ] && kill -9 $P7 2>/dev/null; rm -rf "$D" "$TM"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

conf() { # conf <n> <port> <extra-daemon-lines> <extra-sections>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
$3
[memory]
arena_mb = 64
[secrets]
client = sd-client-secret
cluster = sd-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = loopback
[cluster]
multicast = 239.255.77.49:17149
advertise = 127.0.1.3$1
mode = store
collections = c
[collection c]
buckets_log2 = 10
$4
EOF
}
start() { # start <n>
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 80 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		eval "kill -0 \$P$1" 2>/dev/null || return 1
		sleep 0.1; i=$((i+1))
	done
	return 1
}
stop() { # stop <n>  - clean: SIGTERM, GOODBYE goes out
	eval "kill -TERM \$P$1" 2>/dev/null; eval "wait \$P$1" 2>/dev/null; eval "P$1="
}
CLI="$D/cli.py"
cat > "$CLI" <<'EOF'
import json, socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=8)
f = s.makefile("rwb")
req = {"jsonrpc":"2.0","id":1,"method":"stats"}
f.write(json.dumps(req).encode()+b"\n"); f.flush()
r = json.loads(f.readline())["result"]["cluster"]
print(r["identity"], r["node"], r["identity_durable"])
EOF
ident() { python3 "$CLI" "$1" 2>/dev/null; }   # "identity node durable"
settle() { sleep 2.5; }

conf 1 17091 "" ""; start 1 || { echo "node 1 did not start"; tail -4 "$D/n1.log"; exit 1; }
settle

# ---- 1. state_dir set: same identity, same id after a clean restart ----
mkdir -p "$D/state2"
conf 2 17092 "state_dir = $D/state2" ""
if ! start 2; then
	bad "case 1: config with state_dir REJECTED: $(tail -1 "$D/n2.log")"
else
	settle; B=$(ident 17092)
	stop 2; sleep 1; start 2 || bad "case 1: no restart"; settle; A=$(ident 17092)
	echo "  case 1: before [$B]  after [$A]"
	[ -n "$B" ] && [ "$B" = "$A" ] && ok || bad "case 1: identity/id changed across a clean restart: [$B] -> [$A]"
	echo "$A" | grep -q " True$" && ok || bad "case 1: identity not reported durable: $A"
	[ -f "$D/state2/node-identity" ] && ok || bad "case 1: no node-identity file in state_dir"
	grep -qi "tmpfs\|not set" "$D/n2.log" && bad "case 1: spurious state warning: $(grep -i "tmpfs\|not set" "$D/n2.log" | head -1)" || ok
	stop 2
fi

# ---- 2. no state_dir: ephemeral, and the log says the key --------------
conf 2 17092 "" ""; start 2 || bad "case 2: no start"; settle; B=$(ident 17092)
stop 2; sleep 1; start 2 || bad "case 2: no restart"; settle; A=$(ident 17092)
echo "  case 2: before [$B]  after [$A]"
[ "$(echo "$B" | cut -d" " -f1)" != "$(echo "$A" | cut -d" " -f1)" ] && ok || bad "case 2: identity unexpectedly survived with no state_dir"
grep -q "state_dir is not set" "$D/n2.log" && ok || bad "case 2: no warning naming state_dir"
stop 2

# ---- 3. state_dir on tmpfs: survives, and the log says tmpfs ------------
if [ "$(stat -f -c %T "$TM" 2>/dev/null)" = "tmpfs" ]; then
	conf 2 17092 "state_dir = $TM" ""; start 2 || bad "case 3: no start"; settle; B=$(ident 17092)
	stop 2; sleep 1; start 2 || bad "case 3: no restart"; settle; A=$(ident 17092)
	echo "  case 3: before [$B]  after [$A]"
	[ -n "$B" ] && [ "$B" = "$A" ] && ok || bad "case 3: identity/id did not survive on tmpfs: [$B] -> [$A]"
	grep -q "is on tmpfs" "$D/n2.log" && ok || bad "case 3: no tmpfs warning"
	stop 2
else
	echo "  case 3: skipped ($TM is not tmpfs here)"
fi

# ---- 4. [wal] dir only: the WAL dir keeps serving as state -------------
mkdir -p "$D/wal2"
conf 2 17092 "" "[wal]
dir = $D/wal2
probe = no
save = off"
start 2 || bad "case 4: no start"; settle; B=$(ident 17092)
stop 2; sleep 1; start 2 || bad "case 4: no restart"; settle; A=$(ident 17092)
echo "  case 4: before [$B]  after [$A]"
[ -n "$B" ] && [ "$B" = "$A" ] && ok || bad "case 4: identity did not persist in the WAL dir: [$B] -> [$A]"
grep -q "using the WAL directory" "$D/n2.log" && ok || bad "case 4: no notice about using the WAL dir"
[ -f "$D/wal2/node-identity" ] && ok || bad "case 4: no node-identity in the WAL dir"
stop 2

# ---- 5. state_dir introduced beside the WAL: carried, not reset ---------
mkdir -p "$D/state2b"
conf 2 17092 "state_dir = $D/state2b" "[wal]
dir = $D/wal2
probe = no
save = off"
start 2 || bad "case 5: no start"; settle; C=$(ident 17092)
echo "  case 5: wal-era [$A]  with state_dir [$C]"
[ "$(echo "$A" | cut -d" " -f1)" = "$(echo "$C" | cut -d" " -f1)" ] && ok || bad "case 5: identity was RESET when state_dir was introduced: $A -> $C"
grep -q "carried node-identity" "$D/n2.log" && ok || bad "case 5: no notice about carrying the identity across"
[ -f "$D/state2b/node-identity" ] && ok || bad "case 5: identity not present in the new state_dir"
stop 2

# ---- 6. S92: the founder adopts its own proposed id --------------------
# Its own multicast group, so it is genuinely alone and takes the
# founding path rather than joining node 1's cluster.
mkdir -p "$D/state3"
cat > "$D/n3.conf" <<EOF
[daemon]
workers = 2
log_level = info
state_dir = $D/state3
[memory]
arena_mb = 64
[secrets]
client = sd-client-secret
cluster = sd-cluster-secret
[listen]
tcp = 127.0.0.1:17093
plaintext = loopback
[cluster]
multicast = 239.255.77.50:17150
advertise = 127.0.1.33
mode = store
collections = c
[collection c]
buckets_log2 = 10
EOF
# A proposal that happened to BE 1 would pass on the broken binary too,
# so mint until the hash lands elsewhere (1 chance in 1023 each try).
PROP= ; i=0
while [ $i -lt 6 ]; do
	rm -f "$D/state3/node-identity" "$D/state3/node-term"
	start 3 || break
	settle
	PROP=$(sed -n 's/.*proposed node id \([0-9][0-9]*\).*/\1/p' "$D/n3.log" | head -1)
	stop 3
	[ -n "$PROP" ] && [ "$PROP" != "1" ] && break
	i=$((i+1))
done
if [ -z "$PROP" ] || [ "$PROP" = "1" ]; then
	bad "case 6: could not mint an identity proposing anything but 1 (got '$PROP')"
else
	start 3 || bad "case 6: node 3 did not start"
	settle
	FID=$(python3 "$CLI" 17093 2>/dev/null | cut -d" " -f2)
	echo "  case 6: proposed $PROP, founded as ${FID:-?}"
	grep -q "founded the cluster" "$D/n3.log" && ok \
		|| bad "case 6: node 3 did not found a cluster (did it see another one?)"
	[ "$FID" = "$PROP" ] && ok \
		|| bad "case 6: the founder took id '$FID', not its proposal $PROP"
	stop 3; sleep 1; start 3 || bad "case 6: no restart"; settle
	AID=$(python3 "$CLI" 17093 2>/dev/null | cut -d" " -f2)
	[ "$AID" = "$PROP" ] && ok \
		|| bad "case 6: the founder's id changed across a restart: $PROP -> '$AID'"
	stop 3
fi

# ---- 7. S106: the legacy binary files are read once and rewritten as text
# Its own multicast group: it founds alone, so a persisted term of 5 is
# not measured against node 1's.
mkdir -p "$D/state7"
head -c 16 /dev/urandom > "$D/state7/node-identity"
LEG=$(od -An -tx1 "$D/state7/node-identity" | tr -d ' \n')
printf 'PCTM\005\000\000\000' > "$D/state7/node-term"   # 'PCTM' + term 5, little-endian
cat > "$D/n7.conf" <<EOF
[daemon]
workers = 2
log_level = info
state_dir = $D/state7
[memory]
arena_mb = 64
[secrets]
client = sd-client-secret
cluster = sd-cluster-secret
[listen]
tcp = 127.0.0.1:17097
plaintext = loopback
[cluster]
multicast = 239.255.77.51:17151
advertise = 127.0.1.37
mode = store
collections = c
[collection c]
buckets_log2 = 10
EOF
start 7 || bad "case 7: no start on the legacy files: $(tail -2 "$D/n7.log" | tr '\n' ' ')"
settle
A=$(ident 17097)
echo "  case 7: legacy [$LEG]  reported [$A]"
[ "$(echo "$A" | cut -d" " -f1)" = "$LEG" ] && ok || bad "case 7: the legacy identity was not kept: [$LEG] -> [$A]"
head -1 "$D/state7/node-identity" | grep -q '^perfcached-node-identity 1$' && ok \
	|| bad "case 7: identity file not rewritten as text: $(head -c 40 "$D/state7/node-identity" | od -c | head -1)"
grep -q "^identity $LEG\$" "$D/state7/node-identity" && ok || bad "case 7: the text file does not carry the identity in hex"
grep -q "legacy sixteen-byte form" "$D/n7.log" && ok || bad "case 7: no notice about the rewrite"
grep -q "mastership term 5 (persisted)" "$D/n7.log" && ok || bad "case 7: the legacy term 5 was not read: $(grep "mastership term" "$D/n7.log" | head -1)"
head -1 "$D/state7/node-term" | grep -q '^perfcached-node-term 1$' && ok || bad "case 7: term file not rewritten as text"
grep -qE '^term [0-9]+$' "$D/state7/node-term" && ok || bad "case 7: no term line in the text term file"
stop 7; sleep 1
start 7 || bad "case 7: no restart on the text form: $(tail -2 "$D/n7.log" | tr '\n' ' ')"
settle
B=$(ident 17097)
[ -n "$A" ] && [ "$A" = "$B" ] && ok || bad "case 7: identity/id changed across the text round trip: [$A] -> [$B]"
grep -q "legacy sixteen-byte form" "$D/n7.log" && bad "case 7: rewrote the identity again on the second start" || ok
stop 7; sleep 1

# ---- 8. S106: a damaged file refuses start, and never mints over it ----
cp "$D/state7/node-identity" "$D/id.good"; cp "$D/state7/node-term" "$D/term.good"
refuses() { # refuses <label> <what the log must name>
	if start 7; then
		bad "case 8: started on $1"; stop 7; sleep 1
	else
		grep -q "refusing to start" "$D/n7.log" && grep -q "$2" "$D/n7.log" && ok \
			|| bad "case 8: $1: no refusal naming $2: $(grep -iE "CRIT|ERR" "$D/n7.log" | tail -1)"
	fi
}
: > "$D/state7/node-identity";                                           refuses "an empty identity file" "node-identity"
printf 'perfcached-node-identity 1\nidentity %s\ncheck 0000000000000000\n' "$LEG" > "$D/state7/node-identity"
                                                                         refuses "an identity with a wrong check" "does not validate"
{ cat "$D/id.good"; echo "extra"; } > "$D/state7/node-identity";        refuses "an identity file with a fourth line" "node-identity"
cp "$D/id.good" "$D/state7/node-identity"
sed 's/^check .*/check 0000000000000000/' "$D/term.good" > "$D/state7/node-term"
                                                                         refuses "a term with a wrong check" "node-term"
cp "$D/term.good" "$D/state7/node-term"
start 7 || bad "case 8: no start after restoring the files: $(tail -2 "$D/n7.log" | tr '\n' ' ')"
settle
C=$(ident 17097)
[ "$(echo "$C" | cut -d" " -f1)" = "$LEG" ] && ok || bad "case 8: the identity changed after the refusals - something minted: [$LEG] -> [$C]"
stop 7

echo "statedirtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
