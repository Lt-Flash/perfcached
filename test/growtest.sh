#!/bin/sh
# growtest.sh - S97: growth by 2 MB group under an address-space reservation.
# arena_mb 16 with arena_cap_mb 64: the mapping is the CAP (64 MB of VA,
# nothing charged for it), 16 MB is committed and resident at start, and
# writing past 16 MB commits groups inside the reservation - on the same
# tier, never as 4K pages - until the ceiling refuses.  The kernel's own
# figure is asserted: the mapping's Rss from the daemon's smaps follows
# arena_committed, not the reservation.  Fail-first: a build before S97
# has no arena_reserved, and its growth past 16 MB is 4K pages
# (arena_page_slack > 0) with the mapping stuck at 16 MB.
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcgr.XXXXXX)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
P=""
trap '[ -n "$P" ] && grep -qa -- "$D" /proc/$P/cmdline 2>/dev/null && kill -9 "$P"; rm -rf "$D"' EXIT TERM INT
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
# the arena mapping's resident bytes, the kernel's figure from the daemon's
# own smaps.  A pinned reservation is SPLIT into several VMAs - mlock
# marks the committed groups and leaves the rest unlocked - so the reader
# takes the run of ADJACENT read-write anonymous mappings whose sizes add
# up to the reservation (64 MB, up to one alignment group more) and sums
# their Rss; the PROT_NONE guard the reservation was carved from keeps
# unrelated mappings out of the run.
arena_rss() {
	python3 - "$P" <<'PYEOF'
import sys
pid = sys.argv[1]; vmas = []
for line in open("/proc/%s/smaps" % pid):
    if line[0] in "0123456789abcdef" and "-" in line.split()[0]:
        parts = line.split(); lo, hi = [int(x, 16) for x in parts[0].split("-")]
        vmas.append([lo, hi, parts[1].startswith("rw") and (len(parts) < 6 or parts[5] in ("", "[anon]")), 0])
    elif line.startswith("Rss:"): vmas[-1][3] = int(line.split()[1]) * 1024
best = None; i = 0
while i < len(vmas):
    if not vmas[i][2]: i += 1; continue
    j = i; size = 0; rss = 0
    while j < len(vmas) and vmas[j][2] and (j == i or vmas[j][0] == vmas[j - 1][1]):
        size += vmas[j][1] - vmas[j][0]; rss += vmas[j][3]; j += 1
    if 64 << 20 <= size <= 68 << 20 and (best is None or rss > best): best = rss
    i = j
print(best if best is not None else "MISSING")
PYEOF
}
mem() { # mem <key>: stats.memory.<key>
	python3 - "$1" <<'PYEOF'
import json, socket, sys
s = socket.create_connection(("127.0.0.1", 17721), timeout=20); f = s.makefile("rwb")
f.write(b'{"jsonrpc":"2.0","id":1,"method":"stats"}\n'); f.flush()
m = json.loads(f.readline())["result"]["memory"]
print(m.get(sys.argv[1], "MISSING"))
PYEOF
}
fill() { # fill <from> <n> <vlen>: n records of vlen bytes, pipelined; prints stored=<n> refused=<n>
	python3 - "$@" <<'PYEOF'
import json, socket, sys
lo, n, vlen = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
s = socket.create_connection(("127.0.0.1", 17721), timeout=60); f = s.makefile("rwb")
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
# The kernel's own VmLck against what the daemon believes it pinned.  A
# sanitizer intercepts mlock(), reports success and populates NOTHING, so
# the daemon logs "N MB pinned", nothing is resident, and every residency
# assertion below is unanswerable rather than false.  A genuine mlock
# FAILURE is different: the daemon says so and pre-faults with a memset,
# so residency still holds and the assertions still run.
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
[ "$COM" -ge "$HELD" ] && [ "$COM" -le $((HELD + 6 * MB)) ] && ok "committed follows held by whole groups: $COM for $HELD held" || bad "committed $COM against held $HELD"
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
echo "growtest: $pass passed, $fail failed"
[ $fail = 0 ]
