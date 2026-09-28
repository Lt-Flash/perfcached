#!/bin/sh
# S153: per-key event logging, selected per collection.
#
# One node, two collections.  `a` logs everything, `b` logs nothing, and
# the assertion that matters is the negative one: a hook that leaks past
# its mask is a production flood, not a feature.  Then the two ceilings -
# rate and level - and the key hash.
BIN=${1:-./perfcached}; CLI=${2:-./perfcli}
D=$(mktemp -d /var/tmp/pcevent.XXXXXX)
P=""
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

conf() { # conf <log_level> <hash>
	cat > "$D/n.conf" <<EOC
[daemon]
workers = 2
log_level = $1
log_events_rate = 5
log_events_hash = $2
state_dir = $D/var
[memory]
arena_mb = 64
[secrets]
client = ev-client-secret
cluster = ev-cluster-secret
[listen]
tcp = 127.0.0.1:17901
plaintext = loopback
[collection a]
buckets_log2 = 12
log_events = miss, expired, store, remove
[collection b]
buckets_log2 = 12
EOC
}
start() {
	mkdir -p "$D/var"
	chmod 640 "$D/n.conf"
	"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
	P=$!
	# NOT the "perfcached ready" line: at log_level = warn it is not printed
	i=0; while [ $i -lt 100 ] && ! "$CLI" -q -h 127.0.0.1 -p 17901 ping >/dev/null 2>&1; do sleep 0.1; i=$((i+1)); done
	"$CLI" -q -h 127.0.0.1 -p 17901 ping >/dev/null 2>&1 || { bad "node did not start"; tail -3 "$D/n.log"; exit 1; }
}
stop() { kill $P 2>/dev/null; wait $P 2>/dev/null; P=""; }
c() { "$CLI" -q -h 127.0.0.1 -p 17901 "$@" >/dev/null 2>&1; }
ev() { grep -c "event=$1" "$D/n.log"; }

conf notice no; start

# ---- the four events on `a`, each with its origin -----------------------
c set a k1 v1 600
[ "$(grep -c 'event=store col=a key=k1 bytes=2 ttl=600 door=json peer=127.0.0.1:' "$D/n.log")" = 1 ] \
	&& ok "store: one line, with bytes, ttl, door and peer" \
	|| bad "store line: $(grep 'event=store' "$D/n.log" | head -1)"
c get a nope1
[ "$(grep -c 'event=miss cause=absent col=a key=nope1 door=json' "$D/n.log")" = 1 ] \
	&& ok "miss on a key that never existed says cause=absent" \
	|| bad "absent miss: $(grep 'event=miss' "$D/n.log" | head -1)"
# cause=expired - a miss on a record still PRESENT but past its TTL - is
# only observable between the TTL and the sweep that reaps it, which on a
# live node is under a second and not a window a shell test can hit.  It
# is asserted deterministically in origintest, where the clock moves and
# the sweep does not.  What IS observable here is the sweep's own event.
c set a short v 1
i=0; while [ $i -lt 100 ] && [ "$(grep -c 'event=expired col=a key=short door=sweep' "$D/n.log")" = 0 ]; do sleep 0.1; i=$((i+1)); done
[ "$(grep -c 'event=expired col=a key=short door=sweep' "$D/n.log")" -ge 1 ] \
	&& ok "the sweep reports the reap (after $((i/10))s)" \
	|| bad "no expired event from the sweep in 10s"
c del a k1
[ "$(grep -c 'event=remove col=a key=k1 door=json' "$D/n.log")" = 1 ] \
	&& ok "remove: one line" || bad "remove line: $(grep 'event=remove' "$D/n.log" | head -1)"

# ---- THE NEGATIVE CONTROL: `b` has no mask and must say nothing ----------
c set b k1 v1 600; c get b nope; c set b short v 1; c del b k1
sleep 2; c get b short
[ "$(grep -c 'col=b' "$D/n.log")" = 0 ] \
	&& ok "a collection WITHOUT log_events logged nothing through the same traffic" \
	|| bad "col=b leaked $(grep -c 'col=b' "$D/n.log") line(s): $(grep 'col=b' "$D/n.log" | head -2 | tr '\n' '|')"

# ---- the rate ceiling: 5/s, so a burst of 40 misses must be cut and SAID --
before=$(ev "miss cause=absent col=a")
i=0; while [ $i -lt 40 ]; do c get a burst$i; i=$((i+1)); done
after=$(ev "miss cause=absent col=a")
[ $((after - before)) -lt 40 ] \
	&& ok "40 misses in a burst produced $((after - before)) lines, not 40" \
	|| bad "the rate limit did not engage: $((after - before)) lines"
i=0; while [ $i -lt 40 ] && [ "$(grep -c 'event=miss col=a suppressed=' "$D/n.log")" = 0 ]; do sleep 0.1; i=$((i+1)); done
[ "$(grep -c 'event=miss col=a suppressed=' "$D/n.log")" -ge 1 ] \
	&& ok "and the suppression is itself reported: $(grep -o 'suppressed=[0-9]*' "$D/n.log" | head -1)" \
	|| bad "lines were dropped silently - no suppressed= report"
stop

# ---- the key hash ----------------------------------------------------------
conf notice yes; start
c get a secret-looking-key
[ "$(grep -c 'event=miss cause=absent col=a key=#[0-9a-f]\{16\} ' "$D/n.log")" = 1 ] \
	&& ok "log_events_hash: the line carries a hash, not the key" \
	|| bad "hash line: $(grep 'event=miss' "$D/n.log" | head -1)"
[ "$(grep -c 'secret-looking-key' "$D/n.log")" = 0 ] \
	&& ok "and the key itself is nowhere in the log" || bad "the key leaked into the log"
stop

# ---- the level ceiling ------------------------------------------------------
conf warn no; start
c get a nope2; c set a k2 v 60
[ "$(grep -c 'event=' "$D/n.log")" = 0 ] \
	&& ok "log_level = warn silences events like any other NOTICE" \
	|| bad "events emitted above the level: $(grep -c 'event=' "$D/n.log")"
stop

echo "eventtest: $pass passed, $fail failed"
[ $fail = 0 ]
