#!/bin/sh
# downgradetest.sh - UP3 (0.6.5): no silent downgrade.  A build that meets
# state a NEWER build wrote refuses to start, says why, and leaves the
# state as it found it - it never treats a newer format as damage and
# starts as if new.
#  1. A snapshot whose format is one past this build's (re-sealed, so its
#     CRC is good: the version alone must be what refuses it): -R says
#     newer-format, the start refuses with a CRIT naming the format, the
#     directory is byte-for-byte unchanged; the original file back, the
#     node starts and every key is there.
#  2. A WAL record of a type this build does not know, well framed, with
#     a known record AFTER it: -W names it and reads on past it; the start
#     refuses (the WAL unchanged); [wal] unknown_records = skip starts
#     without it and the record after it IS replayed; with skip removed
#     again the next start passes - the skip start's snapshot lies above
#     the unknown record, so it is covered.
#  3. A cluster-id file whose header is version 2: the start refuses
#     rather than joining or founding as if new; the same id under
#     version 1 starts and reports that id (the control).
# FAIL-FIRST: 0.6.4 takes 1 as a corrupt snapshot and starts from the WAL
# alone, 2 as a torn log (the record after it lost, no word said), and 3
# as no cluster id at all - every arm below fails against it.
# Usage: test/downgradetest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

BIN=${1:-./perfcached}
D=$(mktemp -d "${PCREC_TMP:-/var/tmp}/pcdown.XXXXXX")
TCP=16595 HTTP=16596 CPORT=16597
PID=
cleanup() { [ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"; }
trap cleanup EXIT
trap 'exit 1' INT TERM   # a TERM with no trap kills the script before the EXIT trap runs
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

mkconf() { # mkconf <waldir> [extra [wal] lines]
	mkdir -p "$1"
	cat > "$D/c.conf" <<EOF
[daemon]
workers = 2
[memory]
arena_mb = 32
[secrets]
client = dg-client-secret
[listen]
tcp = 127.0.0.1:$TCP
http = 127.0.0.1:$HTTP
plaintext = loopback
[wal]
dir = $1
probe = no
fsync = always
save = off
${2:-}
[collection th]
buckets_log2 = 8
EOF
	chmod 600 "$D/c.conf"
}
# start: must reach ready; refused <log> <what>: must exit WITHOUT reaching
# ready, non-zero, its log naming <what>
start() {
	: > "$D/log"
	"$BIN" -f "$D/c.conf" >> "$D/log" 2>&1 &
	PID=$!
	i=0
	while [ $i -lt 300 ]; do
		grep -q "perfcached ready" "$D/log" && return 0
		kill -0 $PID 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "  daemon did not start:"; tail -5 "$D/log" | sed 's/^/    /'
	return 1
}
refused() { # refused <label> <grep -E pattern>
	: > "$D/log"
	"$BIN" -f "$D/c.conf" >> "$D/log" 2>&1 &
	p=$!
	i=0
	while [ $i -lt 300 ] && kill -0 $p 2>/dev/null; do sleep 0.1; i=$((i+1)); done
	if kill -0 $p 2>/dev/null; then
		kill -9 $p 2>/dev/null; wait $p 2>/dev/null
		bad "$1: still running after 30 s ($(grep -c 'perfcached ready' "$D/log") ready line)"
		return
	fi
	wait $p; rc=$?
	if [ $rc -ne 0 ] && ! grep -q "perfcached ready" "$D/log" && grep -q -E "$2" "$D/log"; then
		ok "$1: refused (exit $rc): $(grep -m1 -E "$2" "$D/log" | cut -c 1-160)"
	else
		bad "$1: exit $rc, ready $(grep -c 'perfcached ready' "$D/log"), log: $(grep -E 'CRIT|ERROR|recover' "$D/log" | head -3 | tr '\n' '|')"
	fi
}
hardkill() { [ -n "$PID" ] && { kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; }; PID=; }
call() { # call <python statements using c(method, **params)>
	python3 - "$TCP" "$1" <<'PY'
import json, socket, sys, time
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), 5)); rid = [0]
def c(m, **p):
    rid[0] += 1
    r = {"jsonrpc": "2.0", "id": rid[0], "method": m}
    if p: r["params"] = p
    f.write(json.dumps(r).encode() + b"\n"); f.flush()
    return json.loads(f.readline())
exec(sys.argv[2])
PY
}
val() { call "r = c('get', col='th', key='$1')['result']; print(r.get('value') if r.get('found') else None)" 2>/dev/null; }
# the state a refusal must not touch: every file but the directory lock,
# which the first start in a directory creates (S290)
sums() { (cd "$1" && find . -type f ! -name perfcached.lock | sort | xargs sha256sum) | sha256sum | cut -c 1-16; }
# CRC32C as the WAL and the snapshot seal with it (Castagnoli, reflected)
cat > "$D/crc.py" <<'PY'
T = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ (0x82F63B78 if c & 1 else 0)
    T.append(c)
def crc32c(b):
    c = 0xFFFFFFFF
    for x in b:
        c = T[(c ^ x) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF
assert crc32c(b"123456789") == 0xE3069283
PY

# ---- 1. a snapshot of a newer format -------------------------------------------------
mkconf "$D/w1"
start || exit 1
call "
for i in range(20): c('set', col='th', key='k%02d' % i, value='v%02d' % i)
assert c('save')['result']['started'] is True
while c('stats')['result']['rdb']['saves'] < 1: time.sleep(0.2)
c('set', col='th', key='after', value='t1')
time.sleep(1.2)" || bad "1. writes"
hardkill
"$BIN" -R "$D/w1" | grep -q "crc ok" && ok "1. control: -R reads this build's snapshot" || bad "1. control -R: $("$BIN" -R "$D/w1")"
cp "$D/w1/dump.rdb" "$D/dump.orig"
python3 - "$D/w1/dump.rdb" "$D/crc.py" <<'PY'
import struct, sys
exec(open(sys.argv[2]).read())
d = bytearray(open(sys.argv[1], "rb").read())
fmt = struct.unpack_from("<I", d, 4)[0]
struct.pack_into("<I", d, 4, fmt + 1)
struct.pack_into("<I", d, len(d) - 4, crc32c(bytes(d[:-4])))
open(sys.argv[1], "wb").write(d)
print(fmt + 1)
PY
r=$("$BIN" -R "$D/w1")
[ "$r" = "rdb: INVALID (newer-format)" ] && ok "1. -R: $r" || bad "1. -R on the newer snapshot: $r"
s0=$(sums "$D/w1")
refused "1. start on the newer snapshot" "snapshot .* is format [0-9]+ - written by a NEWER build"
[ "$(sums "$D/w1")" = "$s0" ] && ok "1. the refusal left the directory as it was" || bad "1. the directory changed under the refusal"
cp "$D/dump.orig" "$D/w1/dump.rdb"
start || exit 1
[ "$(val k07)" = v07 ] && [ "$(val after)" = t1 ] && ok "1. the original snapshot back: the node starts, snapshot and WAL keys there" ||
	bad "1. after the restore: k07 $(val k07), after $(val after)"
hardkill

# ---- 2. a WAL record of an unknown type ----------------------------------------------
mkconf "$D/w2"
start || exit 1
call "
c('set', col='th', key='a', value='1')
c('set', col='th', key='after', value='v1')
time.sleep(1.2)" || bad "2. writes"
hardkill
# after the last record: one of type 200 that does not parse as any of this
# build's layouts, then a copy of the last record ('after') with its value
# v2 and its seq and version one higher - then a zero length, the log's end
python3 - "$D/w2" "$D/crc.py" <<'PY'
import glob, struct, sys
exec(open(sys.argv[2]).read())
best = None
for seg in glob.glob(sys.argv[1] + "/wal-*.seg"):
    h = open(seg, "rb").read(8)
    magic, gen = struct.unpack("<II", h)
    if magic == 0x4C574350 and (best is None or gen > best[1]):
        best = (seg, gen)
seg, gen = best
d = open(seg, "rb").read()
off, last = 8, None
while off + 12 <= len(d):
    ln = struct.unpack_from("<I", d, off)[0]
    if ln == 0 or struct.unpack_from("<I", d, off + 8)[0] != gen:
        break
    last = (off, ln)
    off += 12 + ln
lo, ln = last
p = bytearray(d[lo + 12:lo + 12 + ln])
typ, collen, klen = p[0], p[1], struct.unpack_from("<H", p, 2)[0]
hdr = 33 if typ == 4 else 32 if typ == 5 else 24
key = bytes(p[hdr + collen:hdr + collen + klen])
assert typ == 4 and key == b"after" and p[-2:] == b"v1", (typ, key, bytes(p[-2:]))
def rec(payload):
    g = struct.pack("<I", gen)
    return struct.pack("<II", len(payload), crc32c(g + payload)) + g + payload
unk = bytes([200, 0, 0, 0]) + struct.pack("<I", 0) + b"a newer build's layout.." + b"\0" * 8
q = bytearray(p)
struct.pack_into("<Q", q, 16, struct.unpack_from("<Q", q, 16)[0] + 1)
struct.pack_into("<Q", q, 24, struct.unpack_from("<Q", q, 24)[0] + 1)
q[-2:] = b"v2"
with open(seg, "r+b") as f:
    f.seek(off)
    f.write(rec(unk) + rec(bytes(q)) + b"\0\0\0\0")
PY
[ $? -eq 0 ] || bad "2. crafting the records"
w=$("$BIN" -W "$D/w2")
echo "$w" | grep -q "type=200 unknown" && echo "$w" | tail -1 | grep -q "stop=end, seq-order=ascending, unknown-type=1" &&
	[ "$(echo "$w" | grep -c 'th/after')" = 2 ] && ok "2. -W names the unknown record and reads on past it ($(echo "$w" | tail -1))" ||
	bad "2. -W: $(echo "$w" | tail -4 | tr '\n' '|')"
s0=$(sums "$D/w2")
refused "2. start on the unknown record" "holds 1 record\\(s\\) of type 200, which this build does not know"
[ "$(sums "$D/w2")" = "$s0" ] && ok "2. the refusal left the WAL as it was" || bad "2. the WAL changed under the refusal"
mkconf "$D/w2" "unknown_records = skip"
start || exit 1
grep -q "1 WAL record(s) of type 200, unknown to this build, were SKIPPED" "$D/log" && ok "2. skip: started, the skipped record said" ||
	bad "2. skip: $(grep -E 'recover' "$D/log" | head -3 | tr '\n' '|')"
[ "$(val a)" = 1 ] && [ "$(val after)" = v2 ] && ok "2. skip: the record AFTER the unknown one was replayed (after = v2)" ||
	bad "2. skip: a $(val a), after $(val after) (v1 = replay stopped at the unknown record)"
hardkill
mkconf "$D/w2"
still=$("$BIN" -W "$D/w2" | grep -c "type=200 unknown")
if start && [ "$(val after)" = v2 ]; then
	if [ "$still" -gt 0 ]; then
		grep -q "lie under the snapshot's marker" "$D/log" &&
			ok "2. skip removed: the next start passes - the skip start's snapshot covers the unknown record" ||
			bad "2. skip removed: started, but not because the record was covered: $(grep -E 'recover' "$D/log" | head -3 | tr '\n' '|')"
	else
		ok "2. skip removed: the next start passes (the segment holding the unknown record was recycled)"
	fi
else
	bad "2. skip removed: after $(val after), $(grep -E 'recover|CRIT' "$D/log" | head -3 | tr '\n' '|')"
fi
hardkill

# ---- 3. a cluster-id file of a newer version -----------------------------------------
CID=6f1c2e5a-0b7d-4c3e-9a21-5d8e7f601234
mkconf "$D/w3"
mkdir -p "$D/s"
sed -i "s|^\\[daemon\\]$|&\nstate_dir = $D/s|" "$D/c.conf"
sed -i "s|^\\[secrets\\]$|&\ncluster = dg-cluster-secret|" "$D/c.conf"
cat >> "$D/c.conf" <<EOF
[cluster]
discovery = unicast
port = $CPORT
seeds =
expect = 1
advertise = 127.0.0.1
mode = eager
collections = th
EOF
printf 'perfcached-cluster-id 2\n%s\n' "$CID" > "$D/s/cluster-id"
# the cluster-id file itself - the refused start may create the directory's
# lock and other state beside it before the id is read
s0=$(sha256sum < "$D/s/cluster-id")
refused "3. start on a version-2 cluster id" "cluster-id is version 2, written by a NEWER build"
[ "$(sha256sum < "$D/s/cluster-id")" = "$s0" ] && ok "3. the refusal left the cluster-id file as it was" || bad "3. the cluster-id file changed under the refusal"
printf 'perfcached-cluster-id 1\n%s\n' "$CID" > "$D/s/cluster-id"
if start; then
	got=$(python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1],timeout=5).read())
print(r["cluster"]["cluster_id"])' "$HTTP" 2>/dev/null)
	[ "$(echo "$got" | tr -d -)" = "$(echo "$CID" | tr -d -)" ] && ok "3. control: the same id under version 1 starts, cluster_id $got" ||
		bad "3. control: cluster_id '$got', want $CID"
else
	bad "3. control: the version-1 file did not start"
fi
hardkill

echo "downgradetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
