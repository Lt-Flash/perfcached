#!/bin/sh
# profile-limit.sh - 0.6.0 step 2's instrument (DESIGN 12li): what a node
# spends its CPU on while a load drives it to its limit, so S361-S364 are
# built only where the network and the system calls are a real share.
#
# Runs ON the node, as root, while a load generator elsewhere saturates it
# (pcbench / perfload from client hosts - the benchmarks are client-bound,
# so one client host is rarely enough).  It does not generate load.
#
#   sh profile-limit.sh [seconds=30] [http port=8080] [outdir=/var/tmp/pcprof.<time>]
#   PID=<pid> picks the daemon when several run on the host (a local fleet)
#
# Over the same window it takes:
#   - /stats before and after: requests (both doors, every dialect), the
#     workers' epoll waits / events and client read() / write() calls, the
#     cluster plane's and the pub/sub relay's UDP syscalls against their
#     datagrams (src/iostat.c), the process's user and system CPU;
#   - perf record -g on the daemon (when perf is installed): the samples'
#     split between the kernel and the daemon, and the top symbols.
# and writes summary.txt with the figures that decide:
#   requests/s and CPU% (user, sys); system calls per request; events per
#   epoll wait; datagrams per UDP syscall; the kernel's share of samples.
set -u
DUR=${1:-30}
HTTP=${2:-8080}
OUT=${3:-/var/tmp/pcprof.$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT" || exit 1
PID=${PID:-$(pgrep -x perfcached | head -1)}
[ -n "$PID" ] && [ -d "/proc/$PID" ] || { echo "profile-limit: no perfcached running (PID '$PID')"; exit 1; }
snap() {
	python3 -c 'import json,sys,urllib.request
print(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=5).read().decode())' "$HTTP"
}
snap > "$OUT/stats0.json" || { echo "profile-limit: /stats on :$HTTP not readable"; exit 1; }
date +%s.%N > "$OUT/t0"
if command -v perf > /dev/null 2>&1; then
	perf record -F 499 -g -p "$PID" -o "$OUT/perf.data" -- sleep "$DUR" > "$OUT/perf-record.log" 2>&1
else
	echo "perf is not installed - the /stats figures only" > "$OUT/perf-record.log"
	sleep "$DUR"
fi
date +%s.%N > "$OUT/t1"
snap > "$OUT/stats1.json"
if [ -s "$OUT/perf.data" ]; then
	perf report -i "$OUT/perf.data" --stdio --no-children -g none --sort dso 2>/dev/null | grep -v '^#' | grep . | head -12 > "$OUT/perf-dso.txt"
	perf report -i "$OUT/perf.data" --stdio --no-children -g none --sort sym 2>/dev/null | grep -v '^#' | grep '%' | head -30 > "$OUT/perf-sym.txt"
fi
python3 - "$OUT" <<'PY' | tee "$OUT/summary.txt"
import json, sys
d = sys.argv[1]
a, b = (json.load(open("%s/stats%d.json" % (d, i))) for i in (0, 1))
dt = float(open(d + "/t1").read()) - float(open(d + "/t0").read())
def g(s, *k):
    for x in k:
        s = s.get(x, {}) if isinstance(s, dict) else {}
    return s if isinstance(s, (int, float)) else 0
def dl(*k): return g(b, *k) - g(a, *k)
req = dl("resp", "requests") + dl("native", "binary", "requests") + dl("native", "resp", "requests")
cu, cs = dl("process", "cpu_user_ms"), dl("process", "cpu_sys_ms")
w, e, r, wr = dl("io", "epoll_waits"), dl("io", "epoll_events"), dl("io", "reads"), dl("io", "writes")
ue, uc, ur = dl("io", "uring_enters"), dl("io", "uring_completions"), dl("io", "uring_receives")   # S361
bk = b.get("io", {}).get("backend", {})
print("window %.1f s, node %s %s (%s), io %s%s" % (dt, b.get("version"), b.get("rev"), b.get("process", {}).get("build"),
    bk.get("active", "epoll"), " (%s)" % bk["buffers"] if bk.get("active") == "io_uring" else ""))
print("requests      %d  = %.0f/s" % (req, req / dt if dt else 0))
print("cpu           user %.0f%%  sys %.0f%%  (of one core; sys share %.0f%%)" % (
    100 * cu / 1000 / dt, 100 * cs / 1000 / dt, 100 * cs / (cu + cs) if cu + cs else 0))
if ue:
    print("workers       %d io_uring_enter(), %d completions (%.1f per enter; %d receives); %d read(), %d write()" % (
        ue, uc, uc / ue if ue else 0, ur, r, wr))
else:
    print("workers       %d epoll waits, %d events (%.1f per wait); %d read(), %d write()" % (w, e, e / w if w else 0, r, wr))
print("per request   %.3f syscalls (reads + writes + waits + io_uring enters)" % ((r + wr + w + ue) / req if req else 0))
for plane, k in (("cluster", "cluster_udp"), ("relay", "relay_udp")):
    ts, td = dl("io", k, "tx_syscalls"), dl("io", k, "tx_datagrams")
    rs, rd = dl("io", k, "rx_syscalls"), dl("io", k, "rx_datagrams")
    print("%-13s tx %d datagrams in %d syscalls (%.2f/call), rx %d in %d (%.2f/call)" % (
        plane + " udp", td, ts, td / ts if ts else 0, rd, rs, rd / rs if rs else 0))
try:
    print("perf by dso:")
    print("".join("  " + l for l in open(d + "/perf-dso.txt")))
except OSError:
    print("  (no perf samples)")
PY
echo "profile-limit: $OUT"
