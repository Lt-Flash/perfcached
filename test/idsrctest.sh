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
		env -u PERFCACHED_PLATFORM_KEY "$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
	else
		PERFCACHED_PLATFORM_KEY="$1" "$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
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

echo "idsrctest: $pass passed, $fail failed"
[ $fail -eq 0 ]
