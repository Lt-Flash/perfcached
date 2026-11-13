#!/bin/sh
# growtest.sh - growth by 2 MB group under an address-space reservation: writing past arena_mb
# commits groups inside the cap's mapping, never 4K pages, and the mapping's Rss follows it.
BIN=${1:-./perfcached}
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pcgr.XXXXXX)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
P=""
trap '[ -n "$P" ] && grep -qa -- "$D" /proc/$P/cmdline 2>/dev/null && kill -9 "$P"; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 16
arena_cap_mb = 64
[secrets]
client = gr-client-secret
cluster = gr-cluster-secret
[listen]
tcp = 127.0.0.1:17721
plaintext = loopback
[collection c]
buckets_log2 = 12
CONF
chmod 640 "$D/n.conf"
"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" 2>/dev/null && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { echo "node did not start"; cat "$D/n.log"; exit 1; }
# the arena mapping's resident bytes, found by the SIZE published as arena_reserved (a sanitizer
# build has look-alike mappings); mlock splits it, so an adjacent run summing to it counts
arena_rss() {
	python3 - "$P" "$(mem arena_reserved)" <<'PYEOF'
import sys
pid, want = sys.argv[1], sys.argv[2]
if want in ("MISSING", "None", ""):
    print("MISSING"); raise SystemExit
want = int(want)
vmas = []
for line in open("/proc/%s/smaps" % pid):
    if line[0] in "0123456789abcdef" and "-" in line.split()[0]:
        p = line.split(); lo, hi = [int(x, 16) for x in p[0].split("-")]
        anon = p[1].startswith("rw") and (len(p) < 6 or p[5] in ("", "[anon]"))
        vmas.append([lo, hi, anon, 0])
    elif line.startswith("Rss:") and vmas:
        vmas[-1][3] = int(line.split()[1]) * 1024
# every adjacent sub-run, from every start: an anonymous mapping right before the
# arena makes no prefix of its group sum to the reservation
hits = []
for i in range(len(vmas)):
    if not vmas[i][2]:
        continue
    size = rss = 0
    for j in range(i, len(vmas)):
        if not vmas[j][2] or (j > i and vmas[j][0] != vmas[j - 1][1]):
            break
        size += vmas[j][1] - vmas[j][0]; rss += vmas[j][3]
        if size == want:
            hits.append(rss); break
        if size > want:
            break
print(hits[0] if len(hits) == 1 else ("AMBIGUOUS" if hits else "MISSING"))
PYEOF
}
mem() { # mem <key>: stats.memory.<key>
	python3 - "$1" <<'PYEOF'
import json, pcnative, socket, sys
s = socket.create_connection(("127.0.0.1", 17721), timeout=20); f = pcnative.wrap(s)
f.write(b'{"jsonrpc":"2.0","id":1,"method":"stats"}\n'); f.flush()
m = json.loads(f.readline())["result"]["memory"]
print(m.get(sys.argv[1], "MISSING"))
PYEOF
}
col() { # col <key>: stats.collections[0].<key>
	python3 - "$1" <<'PYEOF'
import json, pcnative, socket, sys
s = socket.create_connection(("127.0.0.1", 17721), timeout=30); f = pcnative.wrap(s)
f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "stats"}) + "\n").encode()); f.flush()
c = json.loads(f.readline())["result"]["collections"][0]
print(c.get(sys.argv[1], "MISSING"))
PYEOF
}
fill() { # fill <from> <n> <vlen>: n records of vlen bytes, pipelined; prints stored=<n> refused=<n>
	python3 - "$@" <<'PYEOF'
import json, pcnative, socket, sys
lo, n, vlen = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
s = socket.create_connection(("127.0.0.1", 17721), timeout=60); f = pcnative.wrap(s)
reqs = [json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "c", "key": "gk%d" % i, "value": "x" * vlen, "ttl": 600}}) for i in range(lo, lo + n)]
f.write(("\n".join(reqs) + "\n").encode()); f.flush()
stored = refused = 0
for _ in reqs:
    r = json.loads(f.readline())
    if r.get("result", {}).get("stored"): stored += 1
    else: refused += 1
print("stored=%d refused=%d" % (stored, refused))
PYEOF
}
MB=1048576
# the kernel's VmLck against what the daemon pinned: a sanitizer's mlock() populates nothing, so
# the residency assertions below are unanswerable there, not false
LCK=$(awk '/^VmLck:/{print $2}' /proc/$P/status 2>/dev/null)
CLAIMED=$(grep -c "MB pinned from swapping" "$D/n.log")
FAILED_PIN=$(grep -c "mlock of the" "$D/n.log")
RSS_OK=1
if [ "${LCK:-0}" = 0 ] && [ "${CLAIMED:-0}" -gt 0 ] && [ "${FAILED_PIN:-0}" = 0 ]; then
	RSS_OK=0
	echo "  ..   SKIP the residency checks: the daemon logged a pin but VmLck is 0 kB - an interceptor (sanitizer?) no-op'd mlock(), so nothing was populated and RSS cannot answer"
fi
RES=$(mem arena_reserved); COM=$(mem arena_committed); RSS=$(arena_rss); HELD=$(mem arena_held); REG=$(mem arena_regions)
echo "  ..   at start: reserved $RES committed $COM held $HELD regions $REG mapping rss $RSS"
[ "$RES" = $((64 * MB)) ] && ok "the reservation is the cap: 64 MB of address space" || bad "arena_reserved $RES (want $((64 * MB)))"
# the initial commit, plus the groups the tables (carved from the top) sit in
REGG=$(( (REG + 2 * MB - 1) / (2 * MB) * 2 * MB ))
[ "$COM" = $((16 * MB + REGG)) ] && ok "the commit is arena_mb plus the tables' groups: 16 MB + $REGG" || bad "arena_committed $COM (want $((16 * MB + REGG)): 16 MB + the tables' $REGG)"
if [ "$RSS_OK" = 1 ]; then
	[ "$RSS" != MISSING ] && [ "$RSS" -ge $((COM - 2 * MB)) ] && [ "$RSS" -le $((COM + 4 * MB)) ] && ok "the mapping is resident to the commit, not the reservation: rss $RSS" || bad "mapping rss $RSS with $COM committed of 64 MB"
fi
grep -q "64 MB reserved on" "$D/n.log" && grep -q "16 MB committed" "$D/n.log" && ok "the start line says reserved and committed apart" || bad "start line: $(grep 'huge-page arena' "$D/n.log" | cut -c1-120)"

# past the initial commit: 30,000 x 1 KB = ~32 MB of records
F=$(fill 0 30000 1024); echo "  ..   $F"
[ "$F" = "stored=30000 refused=0" ] && ok "30,000 records of 1 KB stored past the initial commit" || bad "fill: $F"
COM=$(mem arena_committed); HELD=$(mem arena_held); SLACK=$(mem arena_page_slack); RSS=$(arena_rss)
echo "  ..   after 32 MB: committed $COM held $HELD page slack $SLACK mapping rss $RSS"
[ "$HELD" -gt $((16 * MB)) ] && ok "held grew past the initial commit ($HELD)" || bad "held $HELD"
[ "$SLACK" = 0 ] && ok "no 4K pages: the growth is inside the reservation (page slack 0)" || bad "page slack $SLACK: growth left the reservation for 4K pages"
# committed runs AHEAD of held by design: the reclaim tick keeps PCACHE_AHEAD_GROUPS + 1 groups
# (10 MB) committed above the frontier while a carve is recent, as right after this fill
[ "$COM" -ge "$HELD" ] && [ "$COM" -le $((HELD + 12 * MB)) ] && ok "committed follows held by whole groups plus the ahead window: $COM for $HELD held" || bad "committed $COM against held $HELD (window is 10 MB + a partial group)"
if [ "$RSS_OK" = 1 ]; then
	[ "$RSS" != MISSING ] && [ "$RSS" -ge $((COM - 8 * MB)) ] && [ "$RSS" -le $((COM + 8 * MB)) ] && ok "the mapping is resident to the commit: rss $RSS, committed $COM" || bad "mapping rss $RSS, committed $COM"
fi

# to the ceiling: another 40,000 x 1 KB cannot all fit under 64 MB
F=$(fill 30000 40000 1024); echo "  ..   $F"
case "$F" in *"refused="[1-9]*) ok "the ceiling refused what the cap could not hold ($F)";; *) bad "no refusal at the cap: $F";; esac
HELD=$(mem arena_held); COM=$(mem arena_committed); RES=$(mem arena_reserved); NOMEM=$(mem nomem)
echo "  ..   at the cap: held $HELD committed $COM reserved $RES nomem $NOMEM"
[ "$HELD" -le $((64 * MB)) ] && [ "$COM" -le $((64 * MB)) ] && ok "held and committed stay under the cap" || bad "held $HELD committed $COM over 64 MB"
[ "$RES" = $((64 * MB)) ] && ok "the reservation did not move" || bad "reserved $RES"
[ "${NOMEM:-0}" -gt 0 ] && ok "the refusals are counted (nomem $NOMEM)" || bad "nomem $NOMEM"
# the overflow leg and the cost of the walk across it, reported: a table with most records on
# one chain per bucket otherwise looks like one at its target load factor
OVF=$(col overflow); ENT=$(col entries); WLK=$(col held_walk_us); BKT=$(col buckets)
echo "  ..   entries $ENT in $BKT buckets, in the leg $OVF, last held walk ${WLK}us"
# the load factor after this burst does not tell a good build from a bad one (41k records is
# too small a deficit); splitkeeptest.sh drives one big enough
[ "$OVF" != MISSING ] && ok "the overflow leg is reported ($OVF of $ENT entries)" \
	|| bad "S131: collections carry no overflow figure"
[ "$WLK" != MISSING ] && ok "the held walk's cost is reported (${WLK}us)" \
	|| bad "S131: collections carry no held_walk_us"
# the walk's duration is a real elapsed time, not a component-wise timespec subtraction's artefact
{ [ "$WLK" != MISSING ] && [ "$WLK" -lt 60000000 ]; } \
	&& ok "and it is a plausible elapsed time, not the unsigned-wrap artefact" \
	|| bad "held_walk_us is ${WLK}us (over 60 s for a table this size: the timespec subtraction wrapped)"
echo "growtest: $pass passed, $fail failed"
[ $fail = 0 ]
