#!/bin/sh
# stagetest.sh - UP5/UP6n (0.6.5, DESIGN 12nf): who vouches for a build.
#  K. perfcached -K <artifact> - offline, what the root helper asks the
#     INSTALLED binary:
#     1. a test key's signature: refused with no configuration (only the
#        release key is built in), GOOD under -f with that key in
#        [upgrade] trusted_keys, the sha256 hashlib's;
#     2. the hex form of the same signature reads the same;
#     3. one byte of the artifact changed: BAD;
#     4. no .sig: NONE, refused - accepted only with allow_unsigned = yes;
#     5. a 63-byte .sig: UNREADABLE;
#     6. -E names the built-in release key - DESIGN 12nf's - and the
#        configured ones; a 63-character key is refused at -C.
#  S. upgrade_stage / upgrade_status on a running node (UP6n):
#     1. unprivileged: refused;
#     2. a signed build in chunks: published as upgrade/staged/<sha256>
#        .tar.gz byte for byte, its .sig and .meta beside it, listed by
#        upgrade_status with version, size and signer - and -K offline
#        reads what was staged as GOOD (the helper's view of it);
#     3. signed by a key nobody trusts, or a sha256 that is not the
#        file's: refused at the end, nothing staged, the part removed;
#     4. unsigned: refused at begin (allow_unsigned = no);
#     5. a chunk out of order: refused; a second upload while one is in
#        progress: refused, then abort, then it begins;
#     6. three builds staged: the oldest pruned, two kept;
#     7. perfcli upgrade-stage <tarball> <version>: the same, from the
#        command line, and upgrade_status names it.
# FAIL-FIRST: 0.6.4 has no -K, no [upgrade] and no upgrade_stage - every
# check fails.
# Usage: test/stagetest.sh [./perfcached] [./perfcli]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
D=$(mktemp -d /var/tmp/pcstage.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
RELEASE_KEY=5646e8e4594a40773a6d98522a26d151c7bdf266d0d80e95d0865073be369def

command -v openssl > /dev/null || { echo "stagetest: openssl is needed to make a test key"; exit 1; }
openssl genpkey -algorithm ed25519 -out "$D/k.pem" 2> /dev/null || { echo "stagetest: openssl cannot make an ed25519 key"; exit 1; }
PUB=$(openssl pkey -in "$D/k.pem" -pubout -outform DER 2> /dev/null | tail -c 32 | od -An -tx1 | tr -d ' \n')
[ ${#PUB} -eq 64 ] || { echo "stagetest: the test key's public half: '$PUB'"; exit 1; }
conf() { # conf [upgrade lines...]
	cat > "$D/c.conf" <<EOF
[daemon]
workers = 1
[secrets]
client = stage-client-secret
[listen]
tcp = 127.0.0.1:16598
plaintext = loopback
[collection 0]
buckets_log2 = 8
[upgrade]
EOF
	for l in "$@"; do printf '%s\n' "$l" >> "$D/c.conf"; done
	chmod 600 "$D/c.conf"
}
head -c 2000000 /dev/urandom > "$D/a.tar.gz"
openssl pkeyutl -sign -inkey "$D/k.pem" -rawin -in "$D/a.tar.gz" -out "$D/a.tar.gz.sig" 2> /dev/null
[ "$(wc -c < "$D/a.tar.gz.sig")" -eq 64 ] || { echo "stagetest: openssl made no 64-byte signature"; exit 1; }
SHA=$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$D/a.tar.gz")

# ---- K1. the built-in key only, then trusted_keys ------------------------------------
r=$("$BIN" -K "$D/a.tar.gz"); rc=$?
[ $rc -eq 1 ] && echo "$r" | grep -q "signature: BAD - none of 1 key(s) (1 built in, 0 in trusted_keys) verifies it - refused" &&
	ok "K1. a test key's signature, no configuration: refused - only the release key is built in" || bad "K1. no config: exit $rc: $r"
conf "trusted_keys = $PUB"
r=$("$BIN" -f "$D/c.conf" -K "$D/a.tar.gz"); rc=$?
[ $rc -eq 0 ] && echo "$r" | grep -q "signature: GOOD, trusted_keys\[0\] $(echo "$PUB" | cut -c 1-8)" &&
	echo "$r" | grep -q "sha256 $SHA" && echo "$r" | grep -q "size 2000000 bytes" &&
	ok "K1. the same key in trusted_keys: GOOD, signer and sha256 named" || bad "K1. trusted: exit $rc: $r"
r=$("$BIN" -K "$D/a.tar.gz" -f "$D/c.conf"); rc=$?
[ $rc -eq 0 ] && ok "K1. -K before -f reads the configuration too" || bad "K1. -K before -f: exit $rc: $r"

# ---- K2. the hex form --------------------------------------------------------------
cp "$D/a.tar.gz.sig" "$D/raw.sig"
od -An -tx1 "$D/raw.sig" | tr -d ' \n' > "$D/a.tar.gz.sig"; echo >> "$D/a.tar.gz.sig"
r=$("$BIN" -f "$D/c.conf" -K "$D/a.tar.gz"); rc=$?
[ $rc -eq 0 ] && echo "$r" | grep -q "signature: GOOD" && ok "K2. the signature as 128 hex characters reads the same" || bad "K2. hex: exit $rc: $r"
cp "$D/raw.sig" "$D/a.tar.gz.sig"

# ---- K3. one byte changed ------------------------------------------------------------
cp "$D/a.tar.gz" "$D/a.orig"
python3 -c 'import sys
f = open(sys.argv[1], "r+b"); f.seek(1000000); b = f.read(1); f.seek(1000000); f.write(bytes([b[0] ^ 1]))' "$D/a.tar.gz"
r=$("$BIN" -f "$D/c.conf" -K "$D/a.tar.gz"); rc=$?
[ $rc -eq 1 ] && echo "$r" | grep -q "signature: BAD - none of 2 key(s) (1 built in, 1 in trusted_keys) verifies it - refused" &&
	ok "K3. one bit of the artifact changed: BAD, refused" || bad "K3. changed: exit $rc: $r"
cp "$D/a.orig" "$D/a.tar.gz"

# ---- K4. unsigned ------------------------------------------------------------------
mv "$D/a.tar.gz.sig" "$D/held.sig"
r=$("$BIN" -f "$D/c.conf" -K "$D/a.tar.gz"); rc=$?
[ $rc -eq 1 ] && echo "$r" | grep -q "signature: NONE (.*a.tar.gz.sig does not exist) - refused" &&
	ok "K4. no signature: refused" || bad "K4. unsigned: exit $rc: $r"
conf "trusted_keys = $PUB" "allow_unsigned = yes"
r=$("$BIN" -f "$D/c.conf" -K "$D/a.tar.gz"); rc=$?
[ $rc -eq 0 ] && echo "$r" | grep -q "signature: none - accepted, \[upgrade\] allow_unsigned = yes" &&
	ok "K4. allow_unsigned = yes: accepted, and said" || bad "K4. allow_unsigned: exit $rc: $r"
conf "trusted_keys = $PUB"
mv "$D/held.sig" "$D/a.tar.gz.sig"

# ---- K5. a short signature file ------------------------------------------------------
head -c 63 "$D/raw.sig" > "$D/a.tar.gz.sig"
r=$("$BIN" -f "$D/c.conf" -K "$D/a.tar.gz"); rc=$?
[ $rc -eq 1 ] && echo "$r" | grep -q "signature: UNREADABLE (.*not 64 bytes, nor 128 hex characters) - refused" &&
	ok "K5. a 63-byte signature file: UNREADABLE, refused" || bad "K5. short: exit $rc: $r"
cp "$D/raw.sig" "$D/a.tar.gz.sig"

# ---- K6. the keys in the configuration dump ------------------------------------------
r=$("$BIN" -f "$D/c.conf" -E 2>&1)
echo "$r" | grep -q "^# built in: release key $RELEASE_KEY$" && ok "K6. -E names the built-in release key, DESIGN 12nf's" ||
	bad "K6. -E built-in: $(echo "$r" | grep -A3 '^\[upgrade\]' | tr '\n' '|')"
echo "$r" | grep -q "^trusted_keys = $PUB$" && echo "$r" | grep -q "^allow_unsigned = no$" && echo "$r" | grep -q "^stage_mb = 256$" &&
	ok "K6. -E: trusted_keys, allow_unsigned no, stage_mb 256" || bad "K6. -E: $(echo "$r" | grep -A5 '^\[upgrade\]' | tr '\n' '|')"
conf "trusted_keys = $(echo "$PUB" | cut -c 2-)"
r=$("$BIN" -f "$D/c.conf" -C 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$r" | grep -q "is not an ed25519 public key (64 hex characters)" && ok "K6. a 63-character key: refused at -C" ||
	bad "K6. short key: exit $rc: $(echo "$r" | tail -2 | tr '\n' '|')"

# ---- S. staging on a running node ------------------------------------------------
openssl genpkey -algorithm ed25519 -out "$D/other.pem" 2> /dev/null
conf "trusted_keys = $PUB"
sed -i -e "s|^\[daemon\]$|&\nstate_dir = $D/st|" -e 's/^client = stage-client-secret$/&\nenable = stage-enable/' "$D/c.conf"
mkdir -p "$D/st"
"$BIN" -f "$D/c.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0; while [ $i -lt 300 ] && ! grep -q "perfcached ready" "$D/n.log"; do sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { bad "S. the node did not start: $(tail -3 "$D/n.log" | tr '\n' '|')"; echo "stagetest: $pass passed, $fail failed"; exit 1; }
cat > "$D/up.py" <<'PY'
# up.py <port> <enable|-> <file> <sig file|-> <version> [sha256|-] [chunk] [skip]
import base64, hashlib, json, socket, sys
import pcnative
port, en, path, sigp, ver = sys.argv[1:6]
sha = sys.argv[6] if len(sys.argv) > 6 and sys.argv[6] != "-" else None
chunk = int(sys.argv[7]) if len(sys.argv) > 7 else 200000
skip = len(sys.argv) > 8
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(port)), 10))
if en != "-":
    pcnative.call(f, "enable", secret=en)
data = open(path, "rb").read()
def call(**kw):
    r = pcnative.call(f, "upgrade_stage", **kw)
    if "error" in r:
        e = r["error"]
        print(json.dumps({"error": e.get("message") if isinstance(e, dict) else e, "op": kw.get("op")}))
        sys.exit(1)
    return r["result"]
p = {"op": "begin", "size": len(data), "sha256": sha or hashlib.sha256(data).hexdigest(), "version": ver}
if sigp != "-":
    p["sig"] = open(sigp, "rb").read().hex()
uid = call(**p)["id"]
off = 0
if skip:                                    # a chunk past the start, first
    call(op="chunk", id=uid, off=chunk, data=base64.b64encode(data[chunk:2 * chunk]).decode())
while off < len(data):
    off = call(op="chunk", id=uid, off=off, data=base64.b64encode(data[off:off + chunk]).decode())["got"]
print(json.dumps(call(op="end", id=uid)))
PY
up() { python3 "$D/up.py" 16598 "$@" 2>&1; }
status() { python3 - "$1" <<'PY' 2>&1
import json, socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 16598), 10))
s = pcnative.call(f, "upgrade_status")["result"]
print(eval(sys.argv[1]))
PY
}
STG=$D/st/upgrade/staged

r=$(up - "$D/a.tar.gz" "$D/a.tar.gz.sig" 0.6.5)
echo "$r" | grep -q -i -E 'privileg|enable' && ok "S1. upgrade_stage without privilege: refused" || bad "S1. unprivileged: $r"

r=$(up stage-enable "$D/a.tar.gz" "$D/a.tar.gz.sig" 0.6.5)
echo "$r" | grep -q "\"sha256\": \"$SHA\"" && echo "$r" | grep -q "trusted_keys\[0\] $(echo "$PUB" | cut -c 1-8)" &&
	ok "S2. a signed build in 200 KB chunks: staged, the signer named ($r)" || bad "S2. stage: $r"
cmp -s "$D/a.tar.gz" "$STG/$SHA.tar.gz" && cmp -s "$D/a.tar.gz.sig" "$STG/$SHA.tar.gz.sig" && grep -q '^version=0.6.5$' "$STG/$SHA.meta" &&
	ok "S2. upgrade/staged/<sha256>.tar.gz byte for byte, its .sig and .meta beside it" || bad "S2. files: $(ls "$STG" 2>&1 | tr '\n' ' ')"
[ "$(status '[(e["version"], e["size"], e["signer"][:15]) for e in s["staged"]]')" = "[('0.6.5', 2000000, 'trusted_keys[0]')]" ] &&
	[ "$(status 's["upload"], s["staged_total"]')" = "(None, 1)" ] && ok "S2. upgrade_status lists it: version, size, signer; no upload open" ||
	bad "S2. status: $(status 's')"
r=$("$BIN" -f "$D/c.conf" -K "$STG/$SHA.tar.gz"); rc=$?
[ $rc -eq 0 ] && echo "$r" | grep -q "signature: GOOD" && ok "S2. -K reads the staged build and its .sig as GOOD - what the helper will check" ||
	bad "S2. -K on the staged: exit $rc: $r"

head -c 300000 /dev/urandom > "$D/b.tar.gz"
openssl pkeyutl -sign -inkey "$D/other.pem" -rawin -in "$D/b.tar.gz" -out "$D/b.sig" 2> /dev/null
r=$(up stage-enable "$D/b.tar.gz" "$D/b.sig" 0.6.6)
echo "$r" | grep -q '"error": "the signature does not verify - refused", "op": "end"' && [ "$(ls "$STG" | grep -c '\.tar\.gz$')" = 1 ] &&
	[ -z "$(ls "$D/st/upgrade/staging")" ] && ok "S3. signed by a key nobody trusts: refused at the end, nothing staged, the part gone" ||
	bad "S3. untrusted: $r; staged $(ls "$STG" | tr '\n' ' '); staging $(ls "$D/st/upgrade/staging" | tr '\n' ' ')"
openssl pkeyutl -sign -inkey "$D/k.pem" -rawin -in "$D/b.tar.gz" -out "$D/b.sig" 2> /dev/null
r=$(up stage-enable "$D/b.tar.gz" "$D/b.sig" 0.6.6 "$SHA")
echo "$r" | grep -q '"error": "sha256 mismatch - refused", "op": "end"' && [ "$(ls "$STG" | grep -c '\.tar\.gz$')" = 1 ] &&
	ok "S3. a sha256 that is not the file's: refused at the end" || bad "S3. sha mismatch: $r"
[ "$(status 's["refused_total"]')" = 2 ] && ok "S3. refused_total 2" || bad "S3. refused_total $(status 's["refused_total"]')"

r=$(up stage-enable "$D/b.tar.gz" - 0.6.6)
echo "$r" | grep -q '"error": "unsigned - refused (\[upgrade\] allow_unsigned = no)", "op": "begin"' &&
	ok "S4. unsigned: refused at begin" || bad "S4. unsigned: $r"

r=$(up stage-enable "$D/b.tar.gz" "$D/b.sig" 0.6.6 - 100000 skip)
echo "$r" | grep -q 'out of order' && ok "S5. a chunk past the offset the node holds: refused" || bad "S5. out of order: $r"
r=$(up stage-enable "$D/b.tar.gz" "$D/b.sig" 0.6.6)
U=$(status 's["upload"]["id"] if s["upload"] else ""')
echo "$r" | grep -q 'another upload is in progress' && [ -n "$U" ] && ok "S5. a second upload while one is open: refused" || bad "S5. second: $r (open: '$U')"
r=$(python3 - "$U" <<'PY' 2>&1
import json, socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 16598), 10))
pcnative.call(f, "enable", secret="stage-enable")
print(json.dumps(pcnative.call(f, "upgrade_stage", op="abort", id=sys.argv[1]).get("result")))
PY
)
r2=$(up stage-enable "$D/b.tar.gz" "$D/b.sig" 0.6.6)
[ "$r" = '{"aborted": true}' ] && echo "$r2" | grep -q '"sha256"' && ok "S5. abort, then the same upload begins and stages" || bad "S5. abort: $r, then: $r2"

sleep 1.1                                     # the staged builds are told apart by time
head -c 100000 /dev/urandom > "$D/c.tar.gz"
openssl pkeyutl -sign -inkey "$D/k.pem" -rawin -in "$D/c.tar.gz" -out "$D/c.sig" 2> /dev/null
r=$(up stage-enable "$D/c.tar.gz" "$D/c.sig" 0.6.7)
[ "$(ls "$STG" | grep -c '\.tar\.gz$')" = 2 ] && [ ! -e "$STG/$SHA.tar.gz" ] && [ ! -e "$STG/$SHA.meta" ] &&
	[ "$(status '[e["version"] for e in s["staged"]]')" = "['0.6.7', '0.6.6']" ] &&
	ok "S6. a third build staged: the oldest pruned with its .sig and .meta, two kept (newest first)" ||
	bad "S6. prune: $r; staged $(ls "$STG" | tr '\n' ' ')"
head -c 500000 /dev/urandom > "$D/d.tar.gz"
openssl pkeyutl -sign -inkey "$D/k.pem" -rawin -in "$D/d.tar.gz" -out "$D/d.tar.gz.sig" 2> /dev/null
DS=$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$D/d.tar.gz")
r=$("$CLI" -q -h 127.0.0.1 -p 16598 -E stage-enable upgrade-stage "$D/d.tar.gz" 0.6.8 2>&1); rc=$?
[ $rc -eq 0 ] && echo "$r" | grep -q "$DS" && echo "$r" | grep -q 'trusted_keys\[0\]' && cmp -s "$D/d.tar.gz" "$STG/$DS.tar.gz" &&
	ok "S7. perfcli upgrade-stage: staged byte for byte, the signer named" || bad "S7. perfcli: exit $rc: $r"
r=$("$CLI" -q -h 127.0.0.1 -p 16598 upgrade_status 2>&1)
echo "$r" | grep -q '0\.6\.8' && echo "$r" | grep -q "$DS" && ok "S7. perfcli upgrade_status names it" || bad "S7. upgrade_status: $r"
r=$("$CLI" -q -h 127.0.0.1 -p 16598 -E stage-enable upgrade-stage "$D/d.tar.gz" '0.6"8' 2>&1); rc=$?
[ $rc -eq 2 ] && echo "$r" | grep -q 'the version is' && ok "S7. a version with a quote: refused by perfcli before it is sent" || bad "S7. odd version: exit $rc: $r"
grep -q -E 'ERROR|CRIT' "$D/n.log" && bad "S. the node logged: $(grep -m2 -E 'ERROR|CRIT' "$D/n.log" | tr '\n' '|')" || ok "S. no ERROR or CRIT in the node's log"

echo "stagetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
