#!/bin/sh
# clitest.sh — perfcli end to end: word commands, quoting, raw-JSON
# passthrough, pipe mode, exit codes - over BOTH transports:
#  - a plaintext-loopback daemon (no auth), including -P / the pretty
#    command and a REAL PTY session driving the in-house line editor
#    (history recall via Up-arrow, backspace editing, the persisted
#    0600 history file with duplicate collapse);
#  - a plaintext=never daemon: perfcli -a runs the Noise handshake
#    (client principal) - the CLI is the first out-of-tree consumer of
#    the client-side channel; a wrong secret must fail loudly.
# Usage: test/clitest.sh [./perfcached] [./perfcli]
set -u

BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
D=$(mktemp -d /var/tmp/pccli.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

mkconf() { # mkconf <file> <port> <plaintext>
	cat > "$1" <<EOF
[daemon]
workers = 2
log_level = warn
[memory]
arena_mb = 64
[secrets]
client = cli-test-secret
cluster = cli-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = $3
[collection c]
buckets_log2 = 10
EOF
}
mkconf "$D/pt.conf" 17045 loopback
mkconf "$D/nx.conf" 17046 never
"$BIN" -f "$D/pt.conf" > "$D/pt.log" 2>&1 &
P1=$!
"$BIN" -f "$D/nx.conf" > "$D/nx.log" 2>&1 &
P2=$!
i=0
while [ $i -lt 100 ]; do
	grep -q ready "$D/pt.log" && grep -q ready "$D/nx.log" && break
	sleep 0.1; i=$((i+1))
done

pc() { "$CLI" -p 17045 "$@"; }

# ---- word commands over plaintext --------------------------------------
R=$(pc ping)
echo "$R" | grep -q pong && ok || bad "ping: $R"
R=$(pc set c k1 hello 500)
echo "$R" | grep -q '"stored":true' && ok || bad "set: $R"
R=$(pc get c k1)
echo "$R" | grep -q '"value":"hello"' && ok || bad "get: $R"
R=$(pc set c k2 "two words \"quoted\"")
echo "$R" | grep -q '"stored":true' && ok || bad "quoted set: $R"
R=$(pc get c k2)
echo "$R" | grep -q 'two words' && ok || bad "quoted get: $R"
R=$(pc ttl c k1)
T=$(echo "$R" | sed 's/.*"ttl"://; s/[^0-9-].*//')
[ "$T" -gt 400 ] && [ "$T" -le 500 ] && ok || bad "ttl: $R"
R=$(pc add c ctr 5)
echo "$R" | grep -q '"value":5' && ok || bad "add: $R"
R=$(pc add c ctr)
echo "$R" | grep -q '"value":6' && ok || bad "add default: $R"
R=$(pc exists c k1)
echo "$R" | grep -q '"exists":true' && ok || bad "exists: $R"
R=$(pc del c k1)
echo "$R" | grep -q '"deleted":true' && ok || bad "del: $R"
R=$(pc get c k1)
echo "$R" | grep -q '"found":false' && ok || bad "get after del: $R"
R=$(pc keys c 'k*')
echo "$R" | grep -q 'k2' && ok || bad "keys: $R"
R=$(pc mset c a 1 b 2)
echo "$R" | grep -q '"stored":2' && ok || bad "mset: $R"
R=$(pc mget c a b)
echo "$R" | grep -q '"1"' && ok || bad "mget: $R"
R=$(pc jset c doc '$' '{"n":1,"s":"x"}')
echo "$R" | grep -q '"set":true' && ok || bad "jset: $R"
R=$(pc jincr c doc '$.n' 4)
echo "$R" | grep -q '"value":5' && ok || bad "jincr: $R"
R=$(pc jget c doc '$.s')
echo "$R" | grep -q '"x"' && ok || bad "jget: $R"
R=$(pc stats)
echo "$R" | grep -q '"arena_total"' && ok || bad "stats: $R"

# raw JSON passthrough
R=$(pc -j '{"jsonrpc":"2.0","id":9,"method":"exists","params":{"col":"c","key":"a"}}')
echo "$R" | grep -q '"exists":true' && ok || bad "raw json: $R"

# pipe mode: several commands, one connection
R=$(printf 'set c p1 v1\nget c p1\n# comment\nping\n' | pc -q)
echo "$R" | grep -q '"value":"v1"' && echo "$R" | grep -q pong \
	&& ok || bad "pipe mode: $R"

# exit codes: error member -> 1; usage -> 2; clean result -> 0
pc get nosuchcol k >/dev/null 2>&1 && bad "bad col exit 0" || ok
pc badverb x y >/dev/null 2>&1; [ $? -eq 2 ] && ok || bad "unknown verb exit"
pc get c nokey >/dev/null 2>&1 && ok || bad "found:false must exit 0"

# ---- pretty printing ----------------------------------------------------
R=$(pc -P exists c a)
printf '%s\n' "$R" | grep -q '^  "exists": true' && ok || bad "-P indent: $R"
R=$(printf 'pretty on\nexists c a\n' | pc -q)
printf '%s\n' "$R" | grep -q '^  "exists": true' && ok || bad "pretty cmd: $R"

# ---- the line editor (pty): recall, edit, history file ------------------
python3 - "$CLI" <<'EOF'
import json, os, pty, select, subprocess, sys, time
cli = sys.argv[1]
m, sl = pty.openpty()
env = dict(os.environ, HOME=os.environ.get("TMPDIR", "/var/tmp"))
hist = env["HOME"] + "/.perfcli_history"
try: os.unlink(hist)
except OSError: pass
p = subprocess.Popen([cli, "-p", "17045"], stdin=sl, stdout=sl, stderr=sl,
    env=env, close_fds=True)
os.close(sl)
out = b""
def drain(t=0.4):
    global out
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([m], [], [], 0.1)
        if r:
            try:
                d = os.read(m, 65536)
                if not d: break
                out += d
            except OSError: break
def send(b, t=0.35):
    os.write(m, b); drain(t)
drain(0.7)
send(b'set c pty "edited value"\r')
send(b"get c pty\r")
send(b"\x1b[A\r")                    # Up-arrow recall
send(b"get c ptZ")
send(b"\x7f")                         # backspace fixes the typo
send(b"y\r")
# Ctrl-R reverse search: query 'pty' matches "get c pty"; Enter runs it
send(b"\x12", 0.3)
send(b"pty", 0.3)
send(b"\r", 0.4)
send(b"\x04", 0.5)                    # Ctrl-D
p.wait()
txt = out.decode("utf8", "replace").replace("\r", "")
assert "reverse-i-search" in txt, txt
assert txt.count("edited value") >= 4, txt
assert '"found":false' not in txt, txt
h = open(hist).read().splitlines()
# the edited line equals the recalled one - consecutive-duplicate
# collapse keeps ONE copy: the dedup is the feature under test
assert h == ['set c pty "edited value"', "get c pty"], h
os.unlink(hist)
EOF
[ $? -eq 0 ] && ok || bad "line editor pty session"

# ---- the Noise path -----------------------------------------------------
R=$("$CLI" -p 17046 -a cli-test-secret set c nk noise-value)
echo "$R" | grep -q '"stored":true' && ok || bad "noise set: $R"
R=$("$CLI" -p 17046 -a cli-test-secret get c nk)
echo "$R" | grep -q '"value":"noise-value"' && ok || bad "noise get: $R"
R=$(printf 'set c np pv\nget c np\n' | "$CLI" -p 17046 -a cli-test-secret -q)
echo "$R" | grep -q '"value":"pv"' && ok || bad "noise pipe: $R"
"$CLI" -p 17046 -a wrong-secret ping >/dev/null 2>&1 \
	&& bad "wrong secret accepted" || ok
"$CLI" -p 17046 ping >/dev/null 2>&1 \
	&& bad "plaintext on never-listener accepted" || ok

kill -TERM $P1 $P2 2>/dev/null; wait 2>/dev/null; P1= P2=
echo "clitest: $pass passed, $fail failed"
[ $fail -eq 0 ]
