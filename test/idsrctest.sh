#!/bin/sh
# idsrctest.sh - S143: [cluster] identity_source names ONE source and
# refuses to start without it.
#
# A pod has no DMI uuid and no stable MAC, so the cascade fell through to
# the identity file - which a cloned image or restored volume carries -
# and a clone joined as the same member.  identity_source = env reads
# $PERFCACHED_PLATFORM_KEY (a Deployment sets it to metadata.uid through
# the downward API: a rescheduled pod is a NEW member); file uses the
# identity file alone (a StatefulSet's PVC keeps it).
#   1  env, key A: the node starts; restarted with key A it is the same
#      identity, with key B a different one - even with the same state
#      directory (the platform key is the authority, the file a cache)
#   2  env with the variable unset: the start is REFUSED, and says why
#   3  file: the identity survives a restart from the file, and the
#      variable is ignored
# S326 - a Proxmox VM's node was minted at random while its DMI uuid and
# its MAC sat there, unique (auto derived only with a site_salt, and the
# shipped unit's non-root daemon cannot read the 0400 uuid):
#   4  auto, no site_salt: derived (v8) from the platform, the same across
#      a restart; another state directory on the machine, another member
#      (each mints its own node-salt); the SAME state directory moved to
#      another path keeps its identity (the salt moves with it)
#   5  a MINTED (v7) identity already on disk is kept under auto - an
#      upgrade must not make the node a new member - and the log says the
#      platform is there and how to switch
#   6  identity_source = dmi as a non-root user, /sys closed to it: the
#      copy in $RUNTIME_DIRECTORY (the unit's ExecStartPre) is read
#   7  inside a container (container=docker) auto does not take the DMI
#      uuid - it is the host's - and says so
#   8  a LEGACY sixteen-byte identity whose version nibble reads 8 is kept
#      under auto - no node salt beside it, so this scheme did not derive
#      it - and no salt is minted, so the next start keeps it again
#   9  an identity derived under a site_salt that is then removed is kept
#      the same way
# 4, 5, 7, 8 and 9 need a platform id this process can see; they say so
# and skip where there is none.  FAIL-FIRST: before S326 auto derives
# nothing without a salt (4, 5), dmi cannot read the copy (6), and the
# container is not noticed (7); 331cde9 (v0.5.1's first cut) kept only a
# v7 identity, so it re-derives 8 and 9.
# Usage: test/idsrctest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcids.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

conf() { # conf <identity_source>
	cat > "$D/n.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 16
[secrets]
client = id-client-secret
cluster = id-cluster-secret
[listen]
tcp = 127.0.0.1:18451
http = 127.0.0.1:18452
plaintext = loopback
[cluster]
discovery = unicast
port = 18440
advertise = 127.0.38.1
identity_source = $1
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n.conf"; mkdir -p "$D/state"
}
# run <env-key or "-" for unset> -> the identity, or "REFUSED"
run() {
	: > "$D/n.log"
	if [ "$1" = - ]; then
		env -u PERFCACHED_PLATFORM_KEY ${RUNAS:-} "$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
	else
		PERFCACHED_PLATFORM_KEY="$1" ${RUNAS:-} "$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
	fi
	PID=$!
	k=0
	while [ $k -lt 100 ]; do
		grep -q "perfcached ready" "$D/n.log" && break
		kill -0 $PID 2>/dev/null || break
		sleep 0.1; k=$((k+1))
	done
	if ! kill -0 $PID 2>/dev/null; then
		wait $PID 2>/dev/null; PID=
		echo REFUSED
		return
	fi
	python3 -c 'import json,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:18452/stats",timeout=5).read())
print(r["cluster"].get("identity"))' 2>/dev/null || echo "?"
	kill $PID; wait $PID 2>/dev/null; PID=
}

echo "--- 1: identity_source = env"
conf env
A1=$(run pod-uid-aaaa); A2=$(run pod-uid-aaaa); B1=$(run pod-uid-bbbb)
[ -n "$A1" ] && [ "$A1" != REFUSED ] && [ "$A1" = "$A2" ] && ok "the same key is the same identity across a restart ($A1)" \
	|| bad "key A gave '$A1' then '$A2'"
[ "$B1" != REFUSED ] && [ "$B1" != "$A1" ] && ok "another key (a rescheduled pod) is another identity, same state directory" \
	|| bad "key B gave '$B1' (key A '$A1')"
grep -q "identity derived from env PERFCACHED_PLATFORM_KEY" "$D/n.log" && ok "the log names the source" \
	|| bad "no 'identity derived from env' line"

echo "--- 2: env with the variable unset"
R=$(run -)
[ "$R" = REFUSED ] && ok "the start is refused" || bad "it started anyway (identity '$R')"
grep -q "identity_source = env PERFCACHED_PLATFORM_KEY, but PERFCACHED_PLATFORM_KEY is not set - refusing" "$D/n.log" \
	&& ok "and says why" || bad "no refusal reason: $(grep -m1 CRIT "$D/n.log")"

echo "--- 3: identity_source = file"
rm -rf "$D/state"; conf file
F1=$(run pod-uid-cccc); F2=$(run pod-uid-dddd)
[ -n "$F1" ] && [ "$F1" != REFUSED ] && [ "$F1" = "$F2" ] && ok "the file's identity survives a restart, whatever the variable says ($F1)" \
	|| bad "file mode gave '$F1' then '$F2'"

# what the daemon's auto cascade can see from here: a DMI uuid outside a
# container, else a non-loopback MAC (S326)
in_ctr() { [ -n "${container:-}" ] || [ -e /.dockerenv ] || [ -e /run/.containerenv ] || [ -s /run/systemd/container ]; }
dmi_ok() { ! in_ctr && [ -r /sys/class/dmi/id/product_uuid ] && [ -n "$(cat /sys/class/dmi/id/product_uuid 2>/dev/null)" ]; }
mac_ok() { for i in /sys/class/net/*; do [ "${i##*/}" = lo ] && continue
	m=$(cat "$i/address" 2>/dev/null); [ -n "$m" ] && [ "$m" != 00:00:00:00:00:00 ] && return 0; done; return 1; }
plat_ok() { dmi_ok || mac_ok; }
v() { printf '%s' "$1" | cut -c13; }    # the uuid version nibble

echo "--- 4: auto, no site_salt (S326)"
if plat_ok; then
	rm -rf "$D/state"; conf auto
	P1=$(run -); P2=$(run -)
	SRC=$(grep -o "identity derived from the platform ([^)]*)" "$D/n.log" | tail -1)
	[ -n "$SRC" ] && [ "$P1" = "$P2" ] && [ "$P1" != REFUSED ] && [ "$(v "$P1")" = 8 ] \
		&& ok "auto derives a v8 identity with no site_salt - '$SRC' - the same across a restart ($P1)" \
		|| bad "auto, no salt: '$P1' then '$P2', log '${SRC:-no derivation line}' (want one v8 identity, derived)"
	sed "s|^state_dir = .*|state_dir = $D/state2|" "$D/n.conf" > "$D/n2.conf" && cat "$D/n2.conf" > "$D/n.conf"; mkdir -p "$D/state2"
	Q1=$(run -)
	[ "$Q1" != REFUSED ] && [ "$(v "$Q1")" = 8 ] && [ "$Q1" != "$P1" ] \
		&& ok "another state directory on the same machine is another member ($Q1)" \
		|| bad "state2 gave '$Q1' (state gave '$P1'; want a different v8)"
	rm -rf "$D/state3"; cp -a "$D/state" "$D/state3"; rm -f "$D/state3/perfcached.lock"
	sed "s|^state_dir = .*|state_dir = $D/state3|" "$D/n.conf" > "$D/n2.conf" && cat "$D/n2.conf" > "$D/n.conf"
	R1=$(run -)
	[ "$R1" = "$P1" ] && [ "$(stat -c %a "$D/state3/node-salt" 2>/dev/null)" = 600 ] \
		&& ok "the same state directory moved to another path keeps its identity - node-salt (0600) moved with it" \
		|| bad "moved state directory gave '$R1' (want '$P1'); node-salt mode $(stat -c %a "$D/state3/node-salt" 2>/dev/null)"
else
	echo "  skip 4: no platform id visible here (no readable DMI uuid outside a container, no NIC MAC)"
fi

echo "--- 5: a minted identity on disk, then auto (S326)"
if plat_ok; then
	rm -rf "$D/state"; conf file
	M1=$(run -)
	conf auto
	M2=$(run -)
	[ "$(v "$M1")" = 7 ] && [ "$M2" = "$M1" ] \
		&& grep -q "identity $M1 was not derived from this machine (minted at random, or under a salt this node no longer has) - a CLONE of this disk would carry it.  This machine offers .*: set \[cluster\] identity_source = \(dmi\|mac\)" "$D/n.log" \
		&& ok "the minted identity is kept (no new member on an upgrade) and the log offers the switch: $(grep -o 'This machine offers [^:]*: set [^(]*' "$D/n.log" | head -1)" \
		|| bad "minted '$M1' then '$M2' under auto (want the same v7, and the 'was not derived from this machine' line)"
else
	echo "  skip 5: no platform id visible here"
fi

echo "--- 6: identity_source = dmi from \$RUNTIME_DIRECTORY, /sys closed (S326)"
RUNAS=
if [ "$(id -u)" = 0 ] && command -v setpriv >/dev/null 2>&1; then
	RUNAS="setpriv --reuid=65534 --regid=65534 --clear-groups"
fi
# the premise: /sys is CLOSED to the daemon's user (the daemon reads /sys
# first and the copy only when it cannot).  A CI runner whose image leaves
# product_uuid world-readable would derive from the real uuid both times.
sys_open() { if [ -n "$RUNAS" ]; then $RUNAS sh -c 'cat /sys/class/dmi/id/product_uuid' >/dev/null 2>&1
	else cat /sys/class/dmi/id/product_uuid >/dev/null 2>&1; fi; }
if { [ "$(id -u)" != 0 ] || [ -n "$RUNAS" ]; } && sys_open; then
	echo "  skip 6: /sys/class/dmi/id/product_uuid is readable by the daemon's user here - the copy would not be read"
	RUNAS=
elif [ "$(id -u)" != 0 ] || [ -n "$RUNAS" ]; then
	rm -rf "$D/state" "$D/rt"; conf dmi; mkdir -p "$D/rt"
	printf 'aaaaaaaa-1111-2222-3333-444444444444\n' > "$D/rt/dmi-product-uuid"
	chmod 755 "$D" "$D/rt"; chmod 644 "$D/rt/dmi-product-uuid"
	[ -n "$RUNAS" ] && chown -R 65534:65534 "$D/state" "$D/n.conf"
	export RUNTIME_DIRECTORY="$D/rt"
	U1=$(run -); U2=$(run -)
	printf 'bbbbbbbb-1111-2222-3333-444444444444\n' > "$D/rt/dmi-product-uuid"
	U3=$(run -)
	unset RUNTIME_DIRECTORY
	[ "$U1" != REFUSED ] && [ "$U1" = "$U2" ] && [ "$U3" != REFUSED ] && [ "$U3" != "$U1" ] && [ "$(v "$U1")" = 8 ] \
		&& ok "a non-root daemon derives from the uuid's copy in \$RUNTIME_DIRECTORY ($U1), and another uuid is another identity" \
		|| bad "dmi via the copy: '$U1' '$U2' then '$U3' (want one v8, then another): $(grep -m1 -E 'CRIT|refusing' "$D/n.log" | cut -c1-140)"
	RUNAS=
else
	echo "  skip 6: root without setpriv - cannot close /sys to the daemon"
fi

echo "--- 7: inside a container, auto does not take the DMI uuid (S326)"
if [ "$(id -u)" = 0 ] && [ -n "$(cat /sys/class/dmi/id/product_uuid 2>/dev/null)" ] && ! in_ctr; then
	rm -rf "$D/state"; conf auto
	export container=docker
	C1=$(run -)
	unset container
	! grep -q "identity derived from the platform (dmi" "$D/n.log" \
		&& grep -q "inside a container" "$D/n.log" || grep -q "identity derived from the platform (nic-mac" "$D/n.log" \
		&& ok "with container=docker the DMI uuid is not used: $(grep -o -E 'identity derived from the platform \([^)]*\)|no platform id \([^)]*\)' "$D/n.log" | head -1)" \
		|| bad "inside a container: $(grep -E 'identity derived|no platform id' "$D/n.log" | head -1 | cut -c1-160)"
else
	echo "  skip 7: needs root and a readable DMI uuid outside a container"
fi

echo "--- 8: a legacy sixteen-byte identity, version nibble 8, then auto (S326)"
# The legacy form is sixteen random bytes: its version nibble is anything,
# so "keep it only if it is v7" re-derived it fifteen times in sixteen -
# the node a new member on upgrade (statedirtest case 7, red on GitHub
# for v0.5.1).  What marks an identity THIS scheme derived is the node
# salt beside it; with none, the identity came from before and is kept.
if plat_ok; then
	rm -rf "$D/state"; conf auto
	printf '\021\042\063\104\125\146\212\273\314\335\356\377\001\002\003\004' > "$D/state/node-identity"
	LEG=1122334455668abbccddeeff01020304
	L1=$(run -); L2=$(run -)
	[ "$L1" = "$LEG" ] && [ "$L2" = "$LEG" ] && [ ! -e "$D/state/node-salt" ] \
		&& grep -q "identity $LEG was not derived from this machine" "$D/n.log" \
		&& ok "a legacy identity reading as v8 is kept across two starts, no salt minted, the switch offered" \
		|| bad "legacy v8-looking identity: '$L1' then '$L2' (want $LEG twice), node-salt $([ -e "$D/state/node-salt" ] && echo minted || echo absent)"
else
	echo "  skip 8: no platform id visible here"
fi

echo "--- 9: derived under a site_salt, the salt then removed (S326)"
if plat_ok; then
	rm -rf "$D/state"; conf auto
	awk '{ print } /^identity_source = auto$/ { print "site_salt = idsrc-old-site-salt" }' "$D/n.conf" > "$D/n2.conf" && cat "$D/n2.conf" > "$D/n.conf"
	S1=$(run -)
	conf auto
	S2=$(run -)
	[ "$(v "$S1")" = 8 ] && [ "$S2" = "$S1" ] && [ ! -e "$D/state/node-salt" ] \
		&& grep -q "identity $S1 was not derived from this machine" "$D/n.log" \
		&& ok "an identity derived under a removed site_salt is kept, and the log offers the switch ($S1)" \
		|| bad "site_salt removed: '$S1' then '$S2' (want one v8 kept), node-salt $([ -e "$D/state/node-salt" ] && echo minted || echo absent)"
else
	echo "  skip 9: no platform id visible here"
fi

echo "idsrctest: $pass passed, $fail failed"
[ $fail -eq 0 ]
