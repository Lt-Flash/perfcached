#!/bin/sh
# switchtest.sh - UP5 (0.6.5, DESIGN 12nf): the root helper, contrib/
# perfcached-upgrade, against a temporary prefix layout (/opt/perfcached ->
# perfcached-<v>-old, state_dir in the prefix's var/) and a stub systemctl
# that starts and stops the daemon through the link - run as this user.
#  1. a signed build: result ok, the link on the new prefix, a NEW process
#     ready (its node file), the state carried, the old prefix kept;
#     the same request again: "already running"
#  2. a signature by a key nobody trusts: refused, nothing stopped
#  3. a sha256 that is not the artifact's: refused
#  4. an artifact path out of upgrade/staged: refused
#  5. the request a symlink: refused, its target untouched
#  6. a signed tarball with an entry outside its top directory: refused
#  7. a build that never gets READY: rolled back - the link back, the
#     previous build's new process ready, the result says why
#  8. UPS end to end: a build uploaded to the running node
#     (upgrade_stage), upgrade_switch with no helper enabled refused
#     and then forced, a second switch refused while the request waits;
#     the helper installs it, and the NEW process's upgrade_status names
#     the request and the helper's "ok"
# FAIL-FIRST: before UP5 there is no helper and no node file.
# Usage: test/switchtest.sh [./perfcached] [contrib/perfcached-upgrade]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=$(cd "$(dirname "${1:-./perfcached}")" && pwd)/$(basename "${1:-./perfcached}")
HELPER=${2:-contrib/perfcached-upgrade}
D=$(mktemp -d /var/tmp/pcswitch.XXXXXX)
cleanup() { [ -f "$D/daemon.pid" ] && kill -9 "$(cat "$D/daemon.pid")" 2>/dev/null; rm -rf "$D"; }
trap cleanup EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
command -v openssl > /dev/null || { echo "switchtest: openssl is needed"; exit 1; }

VER=$("$BIN" -V | head -1 | cut -d' ' -f2)
OLD=$D/opt/perfcached-$VER-old
mkdir -p "$OLD/bin" "$OLD/var" "$D/results" "$D/real"
cp "$BIN" "$OLD/bin/perfcached"; cp "$BIN" "$D/real/perfcached"
echo carried > "$OLD/var/carry-check"
ln -s "$OLD" "$D/opt/perfcached"

openssl genpkey -algorithm ed25519 -out "$D/k.pem" 2> /dev/null
openssl genpkey -algorithm ed25519 -out "$D/other.pem" 2> /dev/null
PUB=$(openssl pkey -in "$D/k.pem" -pubout -outform DER 2> /dev/null | tail -c 32 | od -An -tx1 | tr -d ' \n')
cat > "$D/c.conf" <<EOF
[daemon]
workers = 1
state_dir = $D/opt/perfcached/var
[secrets]
client = switch-client-secret
enable = switch-enable
[listen]
tcp = 127.0.0.1:16599
plaintext = loopback
[collection 0]
buckets_log2 = 8
[upgrade]
trusted_keys = $PUB
results_dir = $D/results
EOF
chmod 600 "$D/c.conf"
cat > "$D/upgrade.conf" <<EOF
# the helper's settings for this test - state_dir comes from the config
unit = perfcached-switchtest
link = $D/opt/perfcached
conf = $D/c.conf
ready_s = 15
results = $D/results
systemctl = $D/fakesystemctl
EOF
cat > "$D/fakesystemctl" <<EOF
#!/bin/sh
# the test's unit: the daemon through the link, its pid in a file
case "\$1" in
start)
	"$D/opt/perfcached/bin/perfcached" -f "$D/c.conf" >> "$D/daemon.log" 2>&1 &
	echo \$! > "$D/daemon.pid"; echo start >> "$D/systemctl.log";;
stop)
	if [ -f "$D/daemon.pid" ]; then
		p=\$(cat "$D/daemon.pid"); kill \$p 2>/dev/null; i=0
		while kill -0 \$p 2>/dev/null && [ \$i -lt 100 ]; do sleep 0.1; i=\$((i+1)); done
		kill -9 \$p 2>/dev/null; rm -f "$D/daemon.pid"
	fi
	echo stop >> "$D/systemctl.log";;
esac
EOF
chmod 755 "$D/fakesystemctl"
UPG=$D/opt/perfcached/var/upgrade
node() { sed -n "s/^$1=//p" "$UPG/node" 2>/dev/null; }
waitready() { # waitready <seconds> - the node file says ready, its pid alive
	i=0; while [ $i -lt $(($1 * 10)) ]; do
		[ "$(node state)" = ready ] && kill -0 "$(node pid)" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1)); done; return 1
}
# mk <top> <key> [extra-top] - a tarball of <top>/bin/perfcached (the real
# binary, or with NEVER=1 one that answers -V/-C/-K/-E and never serves),
# signed by <key>; prints its sha256
mk() {
	rm -rf "$D/build"; mkdir -p "$D/build/$1/bin"
	if [ -n "${NEVER:-}" ]; then
		printf '#!/bin/sh\ncase " $* " in *" -V "*|*" -C "*|*" -K "*|*" -E "*) exec "%s" "$@";; esac\nexec sleep 3600\n' \
			"$D/real/perfcached" > "$D/build/$1/bin/perfcached"
		chmod 755 "$D/build/$1/bin/perfcached"
	else
		cp "$BIN" "$D/build/$1/bin/perfcached"
	fi
	[ -n "${3:-}" ] && { mkdir -p "$D/build/$3"; echo x > "$D/build/$3/stray"; }
	tar -C "$D/build" -czf "$D/a.tar.gz" "$1" ${3:-}
	openssl pkeyutl -sign -inkey "$D/$2.pem" -rawin -in "$D/a.tar.gz" -out "$D/a.tar.gz.sig" 2> /dev/null
	sha256sum "$D/a.tar.gz" | cut -c 1-64
}
# req <id> <sha> [artifact] [sha in the request] - stage it as the daemon will
req() {
	mkdir -p "$UPG/staged"
	cp "$D/a.tar.gz" "$UPG/staged/$2.tar.gz"; cp "$D/a.tar.gz.sig" "$UPG/staged/$2.tar.gz.sig"
	printf 'id=%s\nartifact=%s\nsha256=%s\nversion=%s\n' "$1" "${3:-staged/$2.tar.gz}" "${4:-$2}" "$VER" > "$UPG/request"
}
run() { PCUPG_CONF=$D/upgrade.conf sh "$HELPER" > "$D/helper.$1.log" 2>&1; }
res() { sed -n "s/^$2=//p" "$D/results/result-$1" 2>/dev/null; }

"$D/fakesystemctl" start
waitready 20 && ok "0. the daemon writes its node file: ready, pid $(node pid), rev $(node rev), version $(node version)" ||
	{ bad "0. no node file ready: $(cat "$UPG/node" 2>/dev/null | tr '\n' ' ')"; echo "switchtest: $pass passed, $fail failed"; exit 1; }
[ "$(node version)" = "$VER" ] || bad "0. node file version $(node version), want $VER"

# ---- 1. a signed build ---------------------------------------------------------
P0=$(node pid)
S=$(mk "perfcached-$VER-new" k)
req r1 "$S"
run 1; rc=$?
[ $rc -eq 0 ] && [ "$(res r1 status)" = ok ] && ok "1. a signed build: result ok ($(res r1 reason))" ||
	bad "1. exit $rc, result: $(cat "$D/results/result-r1" 2>/dev/null | tr '\n' ' ') log: $(tail -3 "$D/helper.1.log" | tr '\n' '|')"
[ "$(readlink "$D/opt/perfcached")" = "$D/opt/perfcached-$VER-new" ] && ok "1. the link is on the new prefix" ||
	bad "1. link: $(readlink "$D/opt/perfcached")"
[ "$(node pid)" != "$P0" ] && waitready 2 && [ "$(readlink "/proc/$(node pid)/exe")" = "$D/opt/perfcached-$VER-new/bin/perfcached" ] &&
	ok "1. a NEW process from the new prefix is ready (pid $P0 -> $(node pid))" ||
	bad "1. process: node pid $(node pid) (was $P0), exe $(readlink "/proc/$(node pid)/exe" 2>/dev/null)"
[ "$(cat "$D/opt/perfcached-$VER-new/var/carry-check" 2>/dev/null)" = carried ] && [ -x "$OLD/bin/perfcached" ] &&
	ok "1. the state carried into the new prefix; the old prefix kept" || bad "1. carry: $(ls "$D/opt/perfcached-$VER-new/var" 2>&1 | tr '\n' ' ')"
[ ! -e "$UPG/request" ] && [ -z "$(ls "$D/results" | grep -v '^result-')" ] && [ ! -e "$D/results/work" ] &&
	ok "1. the request consumed; root's work directory cleaned" || bad "1. left behind: $(ls -a "$UPG" "$D/results" | tr '\n' ' ')"
req r1b "$S"
run 1b; [ "$(res r1b status)" = ok ] && res r1b reason | grep -q '^already running' && ok "1. the same build again: ok, already running" ||
	bad "1. again: $(cat "$D/results/result-r1b" 2>/dev/null | tr '\n' ' ')"

# ---- 2-6. refusals: nothing stopped -------------------------------------------------
: > "$D/systemctl.log"; P1=$(node pid); L1=$(readlink "$D/opt/perfcached")
refused() { # refused <id> <reason pattern> <label>
	if [ "$(res "$1" status)" = refused ] && res "$1" reason | grep -q -E "$2" &&
	   [ ! -s "$D/systemctl.log" ] && [ "$(node pid)" = "$P1" ] && [ "$(readlink "$D/opt/perfcached")" = "$L1" ]; then
		ok "$3: refused ($(res "$1" reason | cut -c 1-120)); nothing stopped, the link unchanged"
	else
		bad "$3: $(cat "$D/results/result-$1" 2>/dev/null | tr '\n' ' ') systemctl: $(tr '\n' ' ' < "$D/systemctl.log") log: $(tail -2 "$D/helper.$1.log" | tr '\n' '|')"
	fi
}
S=$(mk "perfcached-$VER-other" other)
req r2 "$S"; run r2
refused r2 "refuses the signature: signature: BAD" "2. signed by a key nobody trusts"
S=$(mk "perfcached-$VER-x" k)
req r3 "$(echo "$S" | tr 0-9a-f 1-9a-f0)"         # staged under, and requested as, another sha256
run r3; refused r3 "sha256 mismatch - the artifact is not the build requested" "3. a sha256 that is not the artifact's"
req r4 "$S" "../../../etc/hostname"
run r4; refused r4 "artifact must be staged/<sha256>.tar.gz" "4. an artifact path out of upgrade/staged"
echo "do not touch" > "$D/target"
ln -s "$D/target" "$UPG/request"
run r5
[ "$(cat "$D/results/result-unreadable" 2>/dev/null | sed -n 's/^status=//p')" = refused ] && [ "$(cat "$D/target")" = "do not touch" ] &&
	[ ! -s "$D/systemctl.log" ] && ok "5. the request a symlink: refused, its target untouched" ||
	bad "5. symlink: $(cat "$D/results/result-unreadable" 2>/dev/null | tr '\n' ' '), target '$(cat "$D/target")'"
S=$(mk "perfcached-$VER-y" k "perfcached-$VER-z")
req r6 "$S"; run r6
refused r6 "outside" "6. a signed tarball with an entry outside its top directory"

# ---- 7. never READY: rolled back -------------------------------------------------
S=$(NEVER=1 mk "perfcached-$VER-never" k)
req r7 "$S"; run r7; rc=$?
[ $rc -ne 0 ] && [ "$(res r7 status)" = rolled-back ] && res r7 reason | grep -q "not ready in 15s" &&
	res r7 reason | grep -q "is running again" && ok "7. a build that never gets READY: rolled back ($(res r7 reason | cut -c 1-140))" ||
	bad "7. exit $rc: $(cat "$D/results/result-r7" 2>/dev/null | tr '\n' ' ') log: $(tail -3 "$D/helper.r7.log" | tr '\n' '|')"
[ "$(readlink "$D/opt/perfcached")" = "$L1" ] && waitready 2 && [ "$(node pid)" != "$P1" ] &&
	ok "7. the link back on $(basename "$L1"), a new process of it ready" || bad "7. after: link $(readlink "$D/opt/perfcached"), node $(tr '\n' ' ' < "$UPG/node")"
[ -d "$D/opt/perfcached-$VER-never" ] && ok "7. the refused prefix is kept for a look, not deleted" || bad "7. the never-ready prefix is gone"

# ---- 8. UPS end to end ------------------------------------------------------------
waitready 5 || bad "8. no ready node before the arm"
S=$(mk "perfcached-$VER-e2e" k)
nat() { # nat <python statements; c(method, **params) is a privileged call>
	python3 - "$1" <<'PY' 2>&1
import base64, json, socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 16599), 10))
pcnative.call(f, "enable", secret="switch-enable")
def c(m, **kw):
    r = pcnative.call(f, m, **kw)
    return r.get("result", r.get("error"))
exec(sys.argv[1])
PY
}
r=$(nat '
data = open("'"$D"'/a.tar.gz", "rb").read()
sig = open("'"$D"'/a.tar.gz.sig", "rb").read().hex()
uid = c("upgrade_stage", op="begin", size=len(data), sha256="'"$S"'", sig=sig, version="'"$VER"'")["id"]
off = 0
while off < len(data):
    off = c("upgrade_stage", op="chunk", id=uid, off=off, data=base64.b64encode(data[off:off + 200000]).decode())["got"]
print(json.dumps(c("upgrade_stage", op="end", id=uid)))')
echo "$r" | grep -q "\"sha256\": \"$S\"" && [ -f "$UPG/staged/$S.tar.gz" ] && ok "8. the build uploaded to the running node and staged" || bad "8. stage: $r"
r=$(nat 'print(json.dumps(c("upgrade_switch", sha256="'"$S"'")))')
echo "$r" | grep -q 'the upgrade helper is not enabled' && [ ! -e "$UPG/request" ] && ok "8. upgrade_switch with no helper enabled: refused, nothing written" || bad "8. unforced: $r"
r=$(nat 'print(json.dumps(c("upgrade_switch", sha256="'"$S"'", force=True)))')
RID=$(echo "$r" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("request", ""))' 2>/dev/null)
[ -n "$RID" ] && grep -q "^id=$RID$" "$UPG/request" && grep -q "^artifact=staged/$S.tar.gz$" "$UPG/request" && grep -q "^version=$VER$" "$UPG/request" &&
	ok "8. forced: upgrade/request written for the helper (request $RID)" || bad "8. forced: $r; request: $(tr '\n' ' ' < "$UPG/request" 2>/dev/null)"
r=$(nat 'print(json.dumps(c("upgrade_switch", sha256="'"$S"'", force=True)))')
echo "$r" | grep -q 'already waiting' && ok "8. a second switch while the request waits: refused" || bad "8. second: $r"
P8=$(node pid)
run 8; rc=$?
[ $rc -eq 0 ] && [ "$(res "$RID" status)" = ok ] && [ "$(readlink "$D/opt/perfcached")" = "$D/opt/perfcached-$VER-e2e" ] &&
	ok "8. the helper installed it: result ok, the link on the new prefix" || bad "8. helper: exit $rc, $(tr '\n' ' ' < "$D/results/result-$RID" 2>/dev/null) log $(tail -2 "$D/helper.8.log" | tr '\n' '|')"
waitready 10
r=$(nat 's = c("upgrade_status"); q = s["requested"] or {}; print(q.get("id"), (q.get("result") or {}).get("status"), s["request_waiting"], s["helper"])')
[ "$(node pid)" != "$P8" ] && [ "$r" = "$RID ok False absent" ] && ok "8. the NEW process's upgrade_status: request $RID, the helper's ok, nothing waiting" ||
	bad "8. after: pid $P8 -> $(node pid), status '$r'"

echo "switchtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
