#!/bin/sh
# phpfpmtest.sh - PE-2: the extension's handles under a real PHP-FPM pool.
#   A. workers recycled every 10 requests: each holds one handle, dialled once and
#      taken up again on every later request; the daemon sees one connection a worker
#   B. perfcached.prepare_secrets_file: the master derives once, and a worker's first
#      dial skips Argon2id - at most half of A's, which is the control
#   C. the daemon frozen, the one worker killed in the middle of its call: the worker
#      FPM starts in its place answers
#   D. the daemon restarted under a worker: its next request re-dials before its first
#      call and raises nothing, on the same worker
#   E. a call times out against a frozen daemon: that request's next call, on the same
#      handle, works once it thaws
#   F. a list of seeds, the dead one first, workers recycled: every worker answers, on
#      one handle and one dial; the dead one alone fails, which is the control
# Usage: test/phpfpmtest.sh [./perfcached] [php-fpm8.3] [lang/php/modules/perfcached.so]
set -u
BIN=${1:-./perfcached}
FPM=${2:-php-fpm8.3}
EXT=${3:-lang/php/modules/perfcached.so}
command -v "$FPM" >/dev/null 2>&1 || FPM=/usr/sbin/$FPM
case $EXT in /*) ;; *) EXT=$(pwd)/$EXT ;; esac
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcfpm.XXXXXX)
PORT=17157 SEC=php-fpm-secret
PD= PF=
trap '[ -n "$PF" ] && kill -9 $PF 2>/dev/null; [ -n "$PD" ] && kill -CONT $PD 2>/dev/null; [ -n "$PD" ] && kill -9 $PD 2>/dev/null; pkill -9 -f "$D/fpm" 2>/dev/null; [ -n "${KEEP:-}" ] && cp -a "$D" "$KEEP"; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -f "$EXT" ] || { echo "phpfpmtest: no extension at $EXT"; exit 1; }
command -v "$FPM" >/dev/null 2>&1 || { echo "phpfpmtest: no $FPM"; exit 1; }
command -v cgi-fcgi >/dev/null 2>&1 || { echo "phpfpmtest: no cgi-fcgi (libfcgi-bin)"; exit 1; }
ss -ltn 2>/dev/null | grep -qE ":$PORT[[:space:]]" && { echo "phpfpmtest: port $PORT already bound" >&2; exit 1; }
cat > "$D/d.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = php-fpm-cluster
[listen]
tcp = 127.0.0.1:$PORT
plaintext = never
[collection c]
buckets_log2 = 10
C
chmod 600 "$D/d.conf"
daemon() {
	"$BIN" -f "$D/d.conf" >> "$D/d.log" 2>&1 &
	PD=$!
	W=$1; i=0
	while [ $i -lt 150 ] && [ "$(grep -c 'perfcached ready' "$D/d.log")" -lt "$W" ]; do sleep 0.1; i=$((i+1)); done
	[ "$(grep -c 'perfcached ready' "$D/d.log")" -ge "$W" ] || { echo "phpfpmtest: the daemon did not start: $(tail -2 "$D/d.log" | tr '\n' ' ')"; exit 1; }
}
fpm() { # fpm <arm> <pm lines> [extra -d]
	A=$1; P=$2; X=${3:-}
	cat > "$D/fpm$A.conf" <<C
[global]
error_log = $D/fpm$A.log
daemonize = no
pid = $D/fpm$A.pid
[t]
listen = $D/fpm$A.sock
clear_env = no
catch_workers_output = yes
$P
C
	# shellcheck disable=SC2086
	"$FPM" -n -R -F -y "$D/fpm$A.conf" -d extension="$EXT" $X >> "$D/fpm$A.out" 2>&1 &
	PF=$!
	i=0; while [ $i -lt 100 ] && [ ! -S "$D/fpm$A.sock" ]; do sleep 0.1; i=$((i+1)); done
	[ -S "$D/fpm$A.sock" ] || { echo "phpfpmtest: FPM $A did not start: $(tail -3 "$D/fpm$A.log" "$D/fpm$A.out" 2>/dev/null | tr '\n' ' ' | cut -c1-300)"; exit 1; }
}
fpm_stop() { kill "$PF" 2>/dev/null; wait "$PF" 2>/dev/null; PF=; }
req() { # req <arm> [query]: the reply's JSON line
	SCRIPT_FILENAME=$T/phpfpm_req.php SCRIPT_NAME=/r REQUEST_METHOD=GET \
	QUERY_STRING="port=$PORT&sec=$SEC${2:+&$2}" cgi-fcgi -bind -connect "$D/fpm$1.sock" 2>/dev/null | tail -1
}
reqs() { # reqs <arm> <n> <file>
	: > "$3"; i=0; while [ $i -lt "$2" ]; do req "$1" >> "$3"; i=$((i+1)); done
}
J() { # J <line> <python expr over r>
	printf '%s' "$1" | python3 -c 'import json,sys
r=json.loads(sys.stdin.read() or "{}")
print(eval(sys.argv[1]))' "$2" 2>/dev/null || echo "?"
}
STATS='import json,sys,statistics
rows=[json.loads(l) for l in open(sys.argv[1]) if l.strip().startswith("{")]
pids=sorted({r["pid"] for r in rows})
last={}
for r in rows: last[r["pid"]]=r["pool"]
first=[r["pool"]["last_dial_ms"] for r in rows if r["pool"]["reused"]==0 and r["pool"]["dials"]==1]
print(len(rows),sum(1 for r in rows if r["ok"]),len(pids),max(r["pool"]["handles"] for r in rows),
  max(p["dials"] for p in last.values()),sum(p["reused"] for p in last.values()),
  max(r["pool"]["prepared"] for r in rows),int(statistics.median(first)) if first else -1,len(first))
errs=[r.get("error") for r in rows if not r["ok"]]
print(errs[:2] if errs else "")'

daemon 1
echo "--- A. workers recycled every 10 requests"
PM="pm = dynamic
pm.max_children = 3
pm.start_servers = 2
pm.min_spare_servers = 1
pm.max_spare_servers = 3
pm.max_requests = 10"
fpm A "$PM"
B0=$(grep -c "(binary, encrypted)" "$D/d.log")
reqs A 45 "$D/a.rows"
python3 -c "$STATS" "$D/a.rows" > "$D/a.st"
read -r N G W H DI RU PR DA FI < "$D/a.st"
C=$(( $(grep -c "(binary, encrypted)" "$D/d.log") - B0 ))
[ "$N" = 45 ] && [ "$G" = 45 ] && ok "45 requests, all answered" || bad "$N answers, $G right of 45: $(sed -n 2p "$D/a.st" | cut -c1-200)"
[ "$W" -ge 4 ] && ok "recycling at work: $W workers took them" || bad "only $W workers: recycling tested nothing"
[ "$H" = 1 ] && [ "$DI" = 1 ] && [ "$RU" -ge 20 ] \
	&& ok "each worker: one handle, dialled once, taken up $RU times across requests" \
	|| bad "handles $H, dials a worker $DI, reused $RU (want 1, 1, 20 or more)"
# worker_balance may re-dial a connection once; a handle not kept would be 45
[ "$C" -le $((W * 2)) ] && ok "the daemon saw $C connections for $W workers" || bad "the daemon saw $C connections for $W workers: handles are not kept"
[ "$PR" = 0 ] && [ "$FI" -ge 1 ] && ok "control: nothing prepared, a worker's first dial takes ${DA} ms (median of $FI)" || bad "control: prepared $PR, $FI first dials"
fpm_stop

echo "--- B. the master derives once"
umask 077; printf '%s\n' "$SEC" > "$D/secrets"; umask 022
fpm B "$PM" "-d perfcached.prepare_secrets_file=$D/secrets"
reqs B 45 "$D/b.rows"
python3 -c "$STATS" "$D/b.rows" > "$D/b.st"
read -r N G W H DI RU PR DB FB < "$D/b.st"
[ "$G" = 45 ] && [ "$PR" = 1 ] && [ "$FB" -ge 1 ] && [ "$DA" -ge 20 ] && [ $((DB * 2)) -le "$DA" ] \
	&& ok "prepared in the master: a worker's first dial takes ${DB} ms, against ${DA} ms without" \
	|| bad "prepared $PR, $G of 45 right; first dial ${DB} ms against ${DA} ms without (want at most half, and that at least 20 ms): $(sed -n 2p "$D/b.st" | cut -c1-200)"
grep -q "can be read by anyone" "$D/fpmB.log" "$D/fpmB.out" && bad "the warning fired on a 0600 file" || ok "no warning on a file only its owner reads"
fpm_stop

echo "--- C. a worker killed in the middle of its call"
fpm C "pm = static
pm.max_children = 1
pm.max_requests = 0"
R=$(req C); P1=$(J "$R" 'r["pid"]')
[ "$(J "$R" 'r["ok"]')" = True ] || bad "C. the first request: $R"
kill -STOP "$PD"
req C "io=20000" > "$D/c.out" &
RQ=$!
sleep 0.5
kill -9 "$P1" 2>/dev/null
kill -CONT "$PD"
wait "$RQ" 2>/dev/null
i=0; R=; while [ $i -lt 50 ]; do
	R=$(req C); [ "$(J "$R" 'r["ok"]')" = True ] && break
	sleep 0.2; i=$((i+1))
done
P2=$(J "$R" 'r["pid"]')
[ "$(J "$R" 'r["ok"]')" = True ] && [ "$P2" != "$P1" ] && [ "$(J "$R" 'r["pool"]["dials"]')" = 1 ] \
	&& ok "worker $P1 killed mid-call: worker $P2 dialled once and answered" \
	|| bad "after the kill: worker $P1 then $P2: $R"
R=$(req C); [ "$(J "$R" 'r["ok"]')" = True ] && [ "$(J "$R" 'r["pool"]["dials"]')" = 1 ] && [ "$(J "$R" 'r["pool"]["reused"]')" -ge 1 ] \
	&& ok "and it keeps its handle" || bad "the worker after: $R"

echo "--- D. the daemon restarted under a worker"
R=$(req C); P1=$(J "$R" 'r["pid"]'); RD0=$(J "$R" 'r["pool"]["redials"]')
kill -9 "$PD"; wait "$PD" 2>/dev/null
daemon 2
R=$(req C)
[ "$(J "$R" 'r["ok"]')" = True ] && [ "$(J "$R" 'r["pid"]')" = "$P1" ] && [ "$(J "$R" 'r["pool"]["redials"]')" -gt "$RD0" ] \
	&& ok "the same worker re-dialled at the request's start, and nothing was raised ($(J "$R" 'r["pool"]["redials"]') redials)" \
	|| bad "after the restart: $R"

echo "--- E. a call times out, and the same handle recovers"
rm -f "$D/m.failed" "$D/m.thawed"
R=$(req C "io=400")                    # this timeout is its own handle: dialled before the freeze
[ "$(J "$R" 'r["ok"]')" = True ] || bad "E. warming the handle: $R"
kill -STOP "$PD"
req C "op=stall&io=400&mark=$D/m" > "$D/e.out" &
RQ=$!
i=0; while [ $i -lt 100 ] && [ ! -e "$D/m.failed" ]; do sleep 0.1; i=$((i+1)); done
kill -CONT "$PD"
touch "$D/m.thawed"
wait "$RQ" 2>/dev/null
R=$(tail -1 "$D/e.out")
[ "$(J "$R" 'r.get("stalled")')" = yes ] && [ "$(J "$R" 'r["ok"]')" = True ] \
	&& ok "PerfcachedTransportException on the timeout, then the same request's next call answered" \
	|| bad "the stall: $R"
fpm_stop

echo "--- F. a list of seeds, one dead, workers recycled"
fpm F "$PM" "-d perfcached.prepare_secrets_file=$D/secrets"
: > "$D/f.rows"; i=0
while [ $i -lt 45 ]; do req F "seeds=127.0.0.2,127.0.0.2,127.0.0.1" >> "$D/f.rows"; i=$((i+1)); done
python3 -c "$STATS" "$D/f.rows" > "$D/f.st"
read -r N G W H DI RU PR DF FF < "$D/f.st"
[ "$G" = 45 ] && [ "$W" -ge 4 ] && [ "$H" = 1 ] && [ "$DI" = 1 ] \
	&& ok "45 requests over $W workers, each on one handle and one dial" \
	|| bad "$G of 45 right over $W workers, handles $H, dials a worker $DI: $(sed -n 2p "$D/f.st" | cut -c1-200)"
R=$(req F "seeds=127.0.0.2")
[ "$(J "$R" 'r["ok"]')" = False ] && [ "$(J "$R" '"Transport" in r.get("error", "")')" = True ] \
	&& ok "control: the dead seed alone fails" || bad "the dead seed alone: $R"
fpm_stop

for l in "$D"/fpm?.log "$D"/fpm?.out; do
	[ -f "$l" ] && grep -E "SIGSEGV|exited on signal 11|signal 6|zend_mm_heap|double free|AddressSanitizer" "$l" | head -2
done > "$D/crash"
[ -s "$D/crash" ] && bad "a worker crashed: $(head -2 "$D/crash" | cut -c1-200)" || ok "no worker crashed"
echo "phpfpmtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
