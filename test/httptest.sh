#!/bin/sh
# httptest.sh — S46: a stable, machine-readable metrics surface.
#
# The task's own words: the JSON stats shape "has moved twice this
# week", so STABLE is the operative word.  These checks exist to make
# a rename break a test rather than an operator's dashboard: every
# name asserted below is part of the contract from here on.
#
#   1. the endpoint answers OpenMetrics text on GET /metrics
#   2. the contract names are all present, with HELP/TYPE
#   3. the numbers track reality (entries follow writes)
#   4. /health is a liveness probe an orchestrator can use
#   5. an unknown path is a clean 404, not a hang or a crash
#   6. an off-box metrics listener without an allow-list is REFUSED
#      at config time - it is plaintext and unauthenticated, so the
#      network is its only protection (the RESP listener's rule)
# Usage: test/httptest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
SEC=met-client-secret
D=$(mktemp -d /var/tmp/pcmet.XXXXXX)
# daemons die by the pids this script started, never by a pattern: a pattern
# assumes the binary's name, and a fail-first run against a renamed daemon
# outlived the old trap and held the suite's ports against the next runs
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

mk() { # mk <n> <listen-extra>
	mkdir -p "$D/w$1"
	cat > "$D/n$1.conf" <<CFGEOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 32
[secrets]
client = $SEC
cluster = met-cluster-secret
[listen]
plaintext = loopback
tcp = 127.0.65.$1:1797$1
$2
[wal]
dir = $D/w$1
segment_mb = 8
probe = no
save = off
[collection m]
buckets_log2 = 12
CFGEOF
}

start() {
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}

# a bare HTTP/1.0 GET; prints status line + body
http() { # http <path>
	python3 - "$1" <<'PYEOF'
import socket, sys
try:
    s = socket.create_connection(("127.0.65.1", 19651), timeout=5)
    s.sendall(("GET %s HTTP/1.0\r\nHost: x\r\n\r\n" % sys.argv[1]).encode())
    buf = b""
    while True:
        b = s.recv(65536)
        if not b:
            break
        buf += b
    sys.stdout.write(buf.decode("utf-8", "replace"))
except Exception as e:
    print("HTTPFAIL %s" % e)
PYEOF
}

fill() { # fill <n>
	python3 - "$1" <<'PYEOF'
import json, socket, sys
s = socket.create_connection(("127.0.65.1", 17971), timeout=10)
f = s.makefile("rwb"); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"] = p
    f.write((json.dumps(r)+"\n").encode()); f.flush()
    return json.loads(f.readline())
for i in range(int(sys.argv[1])):
    call("set", col="m", key="mk%04d" % i, value="v%04d" % i)
print("filled")
PYEOF
}

mk 1 "http = 127.0.65.1:19651
http_timeout = 2"
start 1 || { echo "daemon did not start:"; tail -5 "$D/n1.log"; exit 1; }

# ---- 1. the endpoint answers -----------------------------------------
R=$(http /metrics)
case "$R" in
	"HTTP/1.0 200"*|"HTTP/1.1 200"*) ok "GET /metrics answers 200";;
	HTTPFAIL*) bad "GET /metrics did not answer ($R)";;
	*) bad "GET /metrics answered: $(echo "$R" | head -1)";;
esac
echo "$R" | grep -qi "^content-type: *text/plain" \
	&& ok "the content type is text/plain" \
	|| bad "wrong or missing content-type"

# ---- 2. the CONTRACT: these names may not move silently ---------------
missing=
for m in perfcached_build_info \
         perfcached_arena_held_bytes \
         perfcached_arena_max_bytes \
         perfcached_arena_live_bytes \
         perfcached_arena_headroom_ratio \
         perfcached_writes_refused_total \
         perfcached_reclaim_released_bytes_total \
         perfcached_collection_entries \
         perfcached_wal_appended_total \
         perfcached_wal_dropped_total \
         perfcached_wal_full_in_seconds \
         perfcached_rdb_last_duration_seconds \
         perfcached_rdb_saves_total \
         perfcached_rdb_last_success_age_seconds \
         perfcached_rdb_in_progress \
         perfcached_uptime_seconds; do
	echo "$R" | grep -q "^$m" || missing="$missing $m"
done
[ -z "$missing" ] && ok "every contract metric is present" \
	|| bad "missing metrics:$missing"
echo "$R" | grep -q "^# HELP perfcached_arena_held_bytes" \
	&& echo "$R" | grep -q "^# TYPE perfcached_arena_held_bytes gauge" \
	&& ok "metrics carry HELP and TYPE" \
	|| bad "HELP/TYPE missing - not OpenMetrics"

# ---- 3. the numbers track reality -------------------------------------
fill 50 >/dev/null
sleep 1
# S115: the page's trend cards carry axes - Y labels beside the plot, time
# ticks under it, a peak marker and a pointer readout.  The served script
# must contain the renderers and the listener; a build before S115 has none.
PG=$(http /)
# S206: the stylesheet is well-formed - no rule opens inside another.
# The .mark and .cmp rules once sat between the two source lines of the
# .brand rule, inside its open brace; Chrome let the anchor through and
# dropped the compass, which rendered black and unsized.  A rule that
# opens at depth 1 is the whole defect, so that is what is checked.
CSSOK=$(printf '%s' "$PG" | python3 -c '
import re,sys
h=sys.stdin.read(); m=re.search(r"<style>(.*?)</style>",h,re.S); css=m.group(1) if m else ""
css=re.sub(r"/\*.*?\*/","",css,flags=re.S)
stack=[]; nested=[]; last=0
for i,ch in enumerate(css):
    if ch=="{":
        prelude=css[last:i].strip()
        kind="at" if prelude.startswith("@") else "rule"
        if kind=="rule" and stack and stack[-1]=="rule":
            nested.append(prelude[-30:])
        stack.append(kind); last=i+1
    elif ch=="}":
        if stack: stack.pop()
        last=i+1
    elif ch==";": last=i+1
ok = not stack and not nested and ".mark{" in css and ".cmp{" in css
print("ok" if ok else "nested=%s open=%d"%(nested,len(stack)))
')
[ "$CSSOK" = ok ] && ok "the stylesheet is well-formed: no rule opens inside another, .mark and .cmp present" \
	|| bad "the stylesheet has a rule opening inside another ($CSSOK)"

# S210: a member in trouble BLINKS - a steps() timing under a second on
# both alarm rules, in hot tokens that exist in both themes; the gone
# card does not blink; reduced motion holds the bright ring.
S210=$(printf '%s' "$PG" | python3 -c '
import re,sys
h=sys.stdin.read(); m=re.search(r"<style>(.*?)</style>",h,re.S); css=m.group(1) if m else ""
css=re.sub(r"/\*.*?\*/","",css,flags=re.S)
def rule(sel):
    m=re.search(re.escape(sel)+r"\{([^}]*)\}",css); return m.group(1) if m else ""
def anim(sel):
    m=re.search(r"animation:\s*(\w+)\s+([\d.]+)(m?s)\s+steps\(",rule(sel)); return (m.group(1), float(m.group(2))*(1 if m.group(3)=="s" else .001)) if m else (None,None)
fa=anim(".node.failed"); aa=anim(".node.attn")
hot=css.count("--badhot:")>=2 and css.count("--warnhot:")>=2
usehot="var(--badhot)" in rule("@keyframes "+str(fa[0])+"{0%") if fa[0] else False
gone="animation" not in rule(".node.gone")
rm=re.search(r"prefers-reduced-motion:reduce\)\s*\{(.*?)\}\s*\}",css,re.S); rmb=rm.group(1) if rm else ""
held="animation:none" in rmb and "--badhot" in rmb and "--warnhot" in rmb
print("ok" if fa[0] and aa[0] and fa[1]<1 and aa[1]<=1 and fa[1]<aa[1] and hot and gone and held else
      "failed=%s attn=%s hot=%s gone=%s held=%s"%(fa,aa,hot,gone,held))
')
[ "$S210" = ok ] && ok "S210: failed and attn blink with a steps() timing under a second (failed the faster), in hot tokens declared for both themes; gone does not; reduced motion holds the bright ring" \
	|| bad "S210: $S210"

# S208: the header aligns on centre and both marks share one declared size.
S208=$(printf '%s' "$PG" | python3 -c '
import re,sys
h=sys.stdin.read(); m=re.search(r"<style>(.*?)</style>",h,re.S); css=m.group(1) if m else ""
css=re.sub(r"/\*.*?\*/","",css,flags=re.S)
def rule(sel):
    m=re.search(r"(?<![\w.-])"+re.escape(sel)+r"\{([^}]*)\}",css); return m.group(1) if m else ""
hdr=rule("header"); mark=rule(".mark"); cmp=rule(".switch .cmp"); ctl=rule(".ctl")
mk=re.search(r"width:([^;]+);height:([^;]+)",mark); cm=re.search(r"width:([^;]+);height:([^;]+)",cmp)
same=mk and cm and mk.group(1)==cm.group(1)==mk.group(2)==cm.group(2) and "var(--markpx)" in mk.group(1)
px=re.search(r"--markpx:(\d+)px",css)
print("ok" if "align-items:center" in hdr and "baseline" not in hdr and same and px and 60<=int(px.group(1))<=72
      and "flex-basis:100%" in ctl and "margin-left:auto" not in ctl and "<div class=ctl>" in h and "(this node)" not in h else
      "hdr=%r same=%s px=%s ctl=%r"%(hdr[:60],same,px.group(1) if px else None,ctl[:40]))
')
[ "$S208" = ok ] && ok "S208/S227: the header aligns on centre, both marks share --markpx (60-72 px since S227), the controls are one group on a row of its own under the brand (it jumped rows when the identity filled in), the option no longer says (this node)" \
	|| bad "S208: $S208"

# S227: the marks' INK is centred in their boxes (both drawings sit one
# unit low in a 0 0 32 32 box: the anchor's ink spans y 3..31, the
# compass ring is centred at y 17), and the wordmark's line box is trimmed
# to its cap height so centring meets the capitals' middle
S227=$(printf '%s' "$PG" | python3 -c '
import re,sys
h=sys.stdin.read()
mark=re.search(r"<svg class=mark viewBox=\"([^\"]*)\"",h); cmpv=re.search(r"<div class=switch><svg class=cmp viewBox=\"([^\"]*)\"",h)
css=re.search(r"<style>(.*?)</style>",h,re.S); css=re.sub(r"/\*.*?\*/","",css.group(1) if css else "",flags=re.S)
brand=re.search(r"(?<![\w.-])\.brand\{([^}]*)\}",css)
okv=mark and cmpv and mark.group(1)=="0 1 32 32" and cmpv.group(1)=="0 1 32 32"
okt=brand and "text-box:trim-both cap alphabetic" in brand.group(1)
print("ok" if okv and okt else "mark=%s compass=%s brand-trim=%s"%(mark and mark.group(1), cmpv and cmpv.group(1), bool(okt)))
')
[ "$S227" = ok ] && ok "S227: both header marks draw their ink centred (viewBox 0 1 32 32) and the wordmark is trimmed to cap height" \
	|| bad "S227: $S227"
# S236: the compass's ring and arrows are the anchor's ink, and the header
# ring is drawn at the anchor's stroke width (the operator: they were
# grey and heavier, the anchor white)
S236=$(printf '%s' "$PG" | python3 -c '
import re,sys
h=sys.stdin.read()
css=re.search(r"<style>(.*?)</style>",h,re.S); css=re.sub(r"/\*.*?\*/","",css.group(1) if css else "",flags=re.S)
def rule(sel):
    m=re.search(r"(?<![\w.-])"+re.escape(sel)+r"\{([^}]*)\}",css); return m.group(1).strip() if m else None
mk,ci,cf=rule(".mark .ink"),rule(".cmp .ink"),rule(".cmp .inkf")
ring=re.search(r"<div class=switch><svg class=cmp[^>]*><circle class=ink [^>]*stroke-width=([0-9.]+)",h)
anchor=re.findall(r"<svg class=mark[^>]*>(.*?)</svg>",h,re.S)
aw=set(re.findall(r"class=ink [^>]*stroke-width=([0-9.]+)",anchor[0])) if anchor else set()
ok=mk=="stroke:var(--ink)" and ci=="stroke:var(--ink)" and cf=="fill:var(--ink)" and ring and aw=={ring.group(1)}
print("ok" if ok else "anchor=%s ring=%s arrows=%s ring-width=%s anchor-widths=%s"%(mk,ci,cf,ring and ring.group(1),sorted(aw)))
')
[ "$S236" = ok ] && ok "S236: the compass ring and arrows use the anchor's ink, and the header ring its stroke width" \
	|| bad "S236: $S236"
# S243: a counter that went BACKWARDS (a restart, reset_stats) restarts
# its window instead of being differenced against another run's samples
# ("pushed on write -2 418 242" until F5).  Behaviour checked by running
# the page's own push()/wv() in node: 100,200,300,50,80 gave
# 100,100,200,-50,-20 before and 100,100,200,50,30 after; CI has no JS
# runtime, so here the rule itself is asserted in the served page.
printf '%s' "$PG" | python3 -c '
import re,sys
h=sys.stdin.read(); m=re.search(r"function wv\(k,v\)\{(.*?)return a&&a\.length>1\?v-a\[0\]:v;\}",h,re.S)
sys.exit(0 if m and "v<a[a.length-1])H[k]=[]" in m.group(1) else 1)' \
	&& ok "S243: a windowed counter that goes backwards (a restart) restarts its window - no negative figures" \
	|| bad "S243: wv() differences a restarted counter against the previous run's samples"
# S239: each collection row carries its autoscale chip - red when the mode
# blocks growth and keys are in the leg, amber when WARN says auto would
# shrink it - and the leg cell goes red on the same condition.  Rendering
# checked in node (auto -> plain, warn 2^17 target 12 -> amber "warn ->
# 2^12", off with a leg -> red); here the rules are asserted present.
printf '%s' "$PG" | python3 -c '
import re,sys
h=sys.stdin.read()
a=re.search(r"function asc\(x\)\{(.*?)</span>\x27;\}",h,re.S)
ok=a and "red=m!==\x27auto\x27&&(x.overflow>0||up)" in a.group(1) and "up=t&&Math.pow(2,t)>x.buckets" in a.group(1) and "sh=m===\x27warn\x27&&t&&Math.pow(2,t)<x.buckets" in a.group(1) \
   and "asc(x)+" in h and "x.overflow>0&&x.autoscale&&x.autoscale!==\x27auto\x27?\x27 b\x27" in h \
   and re.search(r"td \.as\.b\{[^}]*var\(--bad\)",h) and re.search(r"td \.as\.w\{[^}]*var\(--warn\)",h)
sys.exit(0 if ok else 1)' \
	&& ok "S239: each collection shows its autoscale mode - amber when warn would shrink it, red when growth is blocked and it has too many keys (past the grow line, or in the leg)" \
	|| bad "S239: the collections card lacks the autoscale chip or its amber/red rules"

# S202: the page carries the anchor as its favicon - contrib/brand/
# favicon.svg inlined as a data URI, so no second route and no second
# request.  Decoded, it must be that SVG, not a stale or truncated copy.
FAV=$(printf '%s' "$PG" | grep -oE '<link rel=icon type="image/svg\+xml" href="data:image/svg\+xml;base64,[A-Za-z0-9+/=]+"' | head -1 | sed -E 's/.*base64,([A-Za-z0-9+\/=]+)".*/\1/')
if [ -n "$FAV" ] && printf '%s' "$FAV" | base64 -d 2>/dev/null | grep -q "perfcached favicon - the anchor"; then
	ok "the page carries the anchor favicon as an inline SVG data URI"
else
	bad "the page has no decodable anchor favicon (link present: $([ -n "$FAV" ] && echo yes || echo no))"
fi
missing=""
for m in "function ylab(" "function xlab(" "class=yl" "class=xl" "class=pk" '"mousemove"'; do
	echo "$PG" | grep -qF -- "$m" || missing="$missing $m"
done
[ -z "$missing" ] && ok "the page carries the S115 axis renderers and the pointer readout" \
	|| bad "the page lacks:$missing (S115)"
# S130: the rolling window is FIVE MINUTES, not an hour.  HMAX and POLL are
# the only two constants behind it - the header text, the axis labels and
# the tick branch all quote them - so asserting the constant is asserting
# the window.  A build before S130 says HMAX=1200, an hour of samples drawn
# three to a pixel.  The window must also stay under xlab()'s 900 s branch,
# which is what draws the minute ticks at all.
echo "$PG" | grep -qF "HMAX=100," \
	&& ok "the page keeps 5 minutes of history (HMAX=100 at POLL=3000)" \
	|| bad "S130: the page's history window is not 100 samples: $(echo "$PG" | sed -n 's/.*\(HMAX=[0-9]*\).*/\1/p' | head -1)"
echo "$PG" | grep -qF "if(span<900)" \
	&& ok "and the window is inside the branch that draws minute ticks" \
	|| bad "S130: the minute-tick branch is gone"
# S163: a fleet cell (fv()) stacks the fleet total over this node's share
# with the same .hv/.hl pair the hit-rate cell uses, but it sits in a bare
# td.  A stacking rule scoped to .hit alone never reaches it, and the two
# figures render inline as one number ("6,1292,043 here").
echo "$PG" | grep -qE 'td \.hv\{display:flex;flex-direction:column' \
	&& echo "$PG" | grep -qE 'td \.hl\{font-size:11px' \
	&& ok "the fleet cells stack the total over this node's share" \
	|| bad "S163: the .hv/.hl stacking rule does not reach a plain td"
# the cluster plane's cards are ordered by height after every redraw, so a
# grid row holds cards of one size (a build before this writes them in a
# fixed order and one long card leaves its row's neighbours half empty)
echo "$PG" | grep -qF "function sizePlane(" && echo "$PG" | grep -qF "sizePlane();trend();}" \
	&& ok "the plane's cards are sorted by their height" \
	|| bad "the plane's cards are not sorted by height"
# S214: ... ONCE.  Re-deciding the order on every poll moved a cell each
# time its height crossed a step; planeOrder keeps it until a cell comes
# or goes or the plane changes width (pagejstest drives the function)
echo "$PG" | grep -qF "function planeOrder(keys,hs,w)" && echo "$PG" | grep -qF "o=planeOrder(keys,hs,p.clientWidth);" \
	&& ok "the order of the cells is decided once and kept" \
	|| bad "the plane is re-sorted on every redraw - cells will move as their heights drift"
# the slow log card clips a long row (a key has nowhere to wrap, and the
# time was pushed off the card).  The slowest calls are NOT a card: in a
# 184px grid column the figures took the width and the command name was
# squeezed to nothing, so they are a table of their own under
# collections, where the name is a column that cannot collapse.
echo "$PG" | grep -qF ".pc.clip .kv>span:first-child{min-width:0;overflow:hidden;text-overflow:ellipsis" \
	&& echo "$PG" | grep -qF "card('slow log \\u00b7 tail'" && echo "$PG" | grep -qF "[['empty','']],'clip',6)" \
	&& ok "a slow log row clips instead of pushing its time off the card" \
	|| bad "a slow log row can push its time off the card"
# S214: the plane is re-ordered by height on every redraw, so a card whose
# row count follows the traffic changed rank and jumped about the page.
# The cards that vary are padded to a fixed height (commands 8 rows, slow
# log 6, unknown commands 8), and memory + backpressure share one stacked
# cell.  pagejstest RENDERS these and counts the rows, but its engine is
# only on the dev host; this is the check that runs everywhere.
echo "$PG" | grep -qF "function card(t,rows,cls,min)" && echo "$PG" | grep -qF "'clip',8)+" \
	&& echo "$PG" | grep -qF "while(rows.length<8)rows.push(" \
	&& ok "the cards whose rows follow the traffic are padded to a fixed height" \
	|| bad "a traffic-dependent card is not padded - it will change rank and move on redraw"
echo "$PG" | grep -qF ".stk{display:flex;flex-direction:column;gap:9px}" && echo "$PG" | grep -qF ".stk>.pc:last-child{flex:1}" \
	&& echo "$PG" | grep -qF "GROUPS=[['memory','backpressure']," && echo "$PG" | grep -qF "['memory budget','cluster auth']" \
	&& echo "$PG" | grep -qF 'document.getElementById("plane").innerHTML=compose();' \
	&& ok "short cards share cells: the plane is composed from the GROUPS table" \
	|| bad "the plane is not composed from the GROUPS table - short cards each take a cell of their own"
echo "$PG" | grep -qF "<tbody id=cmds>" && echo "$PG" | grep -qF "function drawCmds(s)" \
	&& echo "$PG" | grep -qF "drawCmds(s);sizePlane();" \
	&& echo "$PG" | grep -qF "function wmax(" && echo "$PG" | grep -qF "'</td><td class=n title=\"the slowest call fell '+wspan(x)+'\">'+wmax(x)+" \
	&& ! echo "$PG" | grep -qF "card('commands \\u00b7 slowest calls'" \
	&& ok "the slowest calls are a table under collections, with the command as its own column" \
	|| bad "the slowest calls are still a card in the plane grid, where the command name collapses"
# numbers are grouped the SI way (a narrow no-break space, from five
# digits) and never by the browser's locale: toLocaleString wrote 10134
# as "10,134", which reads as ten point something - and as "10.134" in a
# German browser.  A microsecond is written with the micro sign.
echo "$PG" | grep -qF "function nf(" && ! echo "$PG" | grep -qF "Number(v).toLocaleString()" \
	&& echo "$PG" | grep -qF "'\\u202f'" && echo "$PG" | grep -qF "' \\u03bcs'" \
	&& ! echo "$PG" | grep -qE "[)'] us['\" ]|' us \\\\u00b7" \
	&& ok "numbers are grouped with a narrow space, never the locale's comma, and microseconds read us as the micro sign" \
	|| bad "the page groups numbers by the browser's locale, or still writes microseconds as 'us'"
# the dark palette rides prefers-color-scheme; without color-scheme the
# browser draws the table scrollbars light on the dark page
echo "$PG" | grep -qF ":root{color-scheme:light dark;" \
	&& ok "the page declares both color schemes for the browser's own parts" \
	|| bad "the page does not declare color-scheme: light dark"
R2=$(http /metrics)
E=$(echo "$R2" | sed -n 's/^perfcached_collection_entries{collection="m"} *//p')
[ "${E:-0}" -ge 50 ] 2>/dev/null \
	&& ok "collection entries followed the writes ($E)" \
	|| bad "entries did not track the 50 writes ($E)"
U=$(echo "$R2" | sed -n 's/^perfcached_uptime_seconds *//p')
case "$U" in ''|*[!0-9.]*) bad "uptime is not a number ($U)";;
	*) ok "uptime is reported ($U)";; esac

# ---- 4. liveness ------------------------------------------------------
H=$(http /health)
case "$H" in "HTTP/1.0 200"*|"HTTP/1.1 200"*) ok "GET /health answers 200";;
	*) bad "GET /health: $(echo "$H" | head -1)";; esac

# ---- 5. an unknown path is a clean 404 --------------------------------
N=$(http /nope)
case "$N" in *" 404"*) ok "an unknown path is a clean 404";;
	HTTPFAIL*) bad "an unknown path broke the connection ($N)";;
	*) bad "unknown path answered: $(echo "$N" | head -1)";; esac

# ---- 6. a half-open request must not hold the slot for ever ----------
# The slowloris shape: connect, send a partial head, never finish.
# http_timeout is 2s in this fixture, so the daemon must close it;
# without the sweep the read below blocks until the client's own
# timeout and the connection leaks in the meantime.
SLOW=$(python3 - <<'PYEOF2'
import socket, time
try:
    s = socket.create_connection(("127.0.65.1", 19651), timeout=15)
    s.sendall(b"GET /metrics HTTP/1.0\r\n")   # unfinished head
    t0 = time.time()
    s.settimeout(12)
    d = s.recv(4096)                            # EOF when the daemon closes
    print("closed-after=%.1f" % (time.time() - t0) if d == b"" else "answered")
except socket.timeout:
    print("HELD-OPEN")
except Exception as e:
    print("ERR %s" % e)
PYEOF2
)
case "$SLOW" in
	closed-after*) ok "a half-open request head is closed ($SLOW)";;
	HELD-OPEN) bad "a partial request head held the connection open -
	         a slowloris keeps a worker slot for free";;
	*) bad "half-open probe: $SLOW";;
esac

# ---- 7. S37: the status page and the members view --------------------
P=$(http /)
case "$P" in
	"HTTP/1.0 200"*) ok "GET / answers 200";;
	*) bad "GET /: $(echo "$P" | head -1)";;
esac
echo "$P" | grep -qi "^content-type: *text/html" \
	&& ok "the page is served as html" \
	|| bad "wrong content-type for the page"
# no external assets: the daemon cannot fetch them and must not try
echo "$P" | grep -qiE "src=[\"']?https?://|href=[\"']?https?://" \
	&& bad "the page references an external asset" \
	|| ok "the page has no external assets"
# S91: the page renders the WAL and RDB detail, not just "on"
PG=$(http /)
for f in "durability \\u00b7 wal" "durability \\u00b7 rdb" "DROPPED (acked, unlogged)" "probe underestimated" "last snapshot" "ago \\u2192 now" "histhint" "POLL=3000" "height:80px" "class=sg" "members up" "AT CEILING"; do
	echo "$PG" | grep -qF "$f" && ok "the page carries: $f" || bad "S91: the page lacks: $f"
done
# S89: /stats names the doors, this one included
S=$(http /stats | sed -n '/^{/,$p' | python3 -c '
import json,sys
d=json.load(sys.stdin); ls=d.get("listeners",[])
h=[l for l in ls if l.get("kind")=="http"]
print("ok" if h and h[0]["port"]==19651 and h[0]["plaintext"] is True else "bad:%s" % ls[:3])' 2>/dev/null)
[ "$S" = ok ] && ok "/stats lists the HTTP door with its port" || bad "S89: /stats listeners: $S"
M=$(http /members)
echo "$M" | grep -q '"members"' \
	&& ok "/members returns the fleet as JSON" \
	|| bad "/members did not return members JSON"
echo "$M" | grep -qi "^content-type: *application/json" \
	&& ok "/members is application/json" \
	|| bad "/members has the wrong content-type"
C=$(http /clients)
echo "$C" | grep -q '"clients"' \
	&& ok "/clients returns this node's connections as JSON (S160)" \
	|| bad "/clients did not return clients JSON"
echo "$C" | grep -qi "^content-type: *application/json" \
	&& ok "/clients is application/json" \
	|| bad "/clients has the wrong content-type"

# ---- 8. the token guards EVERY route, not just the topology ----------
mk 3 "http = 127.0.65.3:19653
http_timeout = 2"
python3 - "$D/n3.conf" <<'PYEOF3'
import sys
p = sys.argv[1]
s = open(p).read().replace("[listen]", "[secrets]\nhttp = s3cr3t-token\n[listen]", 1)
open(p, "w").write(s)
PYEOF3
start 3 || { bad "the tokened node did not start"; }
tok() { # tok <path> <token|->
	python3 - "$1" "$2" <<'PYEOF4'
import socket, sys
path, tk = sys.argv[1], sys.argv[2]
req = "GET %s HTTP/1.0\r\nHost: x\r\n" % path
if tk != "-":
    req += "Authorization: Bearer %s\r\n" % tk
req += "\r\n"
try:
    s = socket.create_connection(("127.0.65.3", 19653), timeout=5)
    s.sendall(req.encode())
    buf = b""
    while True:
        b = s.recv(65536)
        if not b:
            break
        buf += b
    sys.stdout.write(buf.decode("utf-8", "replace").split("\r\n")[0])
except Exception as e:
    print("HTTPFAIL %s" % e)
PYEOF4
}
case "$(tok /members -)" in
	*401*) ok "no token is refused";;
	*) bad "an untokened /members was served: $(tok /members -)";;
esac
case "$(tok /members wrong-token)" in
	*401*) ok "a wrong token is refused";;
	*) bad "a wrong token was accepted";;
esac
case "$(tok /clients -)" in
	*401*) ok "/clients sits behind the same token (S160)";;
	*) bad "an untokened /clients was served: $(tok /clients -)";;
esac
case "$(tok /members s3cr3t-token)" in
	*200*) ok "the right token is accepted";;
	*) bad "the right token was refused: $(tok /members s3cr3t-token)";;
esac
# the hole with a tidy name: /metrics discloses the fleet too
case "$(tok /metrics -)" in
	*401*) ok "the token guards /metrics as well";;
	*) bad "/metrics bypassed the token";;
esac

# ---- 9. an off-box listener without an allow-list is refused ----------
mk 2 "http = 10.9.9.9:19652"
"$BIN" -f "$D/n2.conf" -C >/dev/null 2>&1 \
	&& bad "an off-box http listener without http_allow started" \
	|| ok "an off-box http listener needs http_allow"

# ---- 10. a refused client is TOLD it is refused (S66) ------------------
# A listener whose allow-list excludes the loopback source.  The client
# must get a 403 whose body names http_allow - not an empty response,
# which a browser cannot tell from a dead daemon.
http_port() { # http_port <port> <path>
	python3 - "$1" "$2" <<'PYHTTP'
import socket, sys
try:
    s = socket.create_connection(("127.0.65.1", int(sys.argv[1])), timeout=5)
    s.sendall(("GET %s HTTP/1.0\r\nHost: x\r\n\r\n" % sys.argv[2]).encode())
    buf = b""
    while True:
        b = s.recv(65536)
        if not b:
            break
        buf += b
    sys.stdout.write(buf.decode("utf-8", "replace"))
except Exception as e:
    print("HTTPFAIL %s" % e)
PYHTTP
}
# node 4: the tokened node above is still up as node 3 on 17973
mk 4 "http = 127.0.65.1:19654
http_allow = 10.255.255.0/24"
start 4 || bad "the http_allow node did not start: $(grep -E "ERROR" "$D/n4.log" | tail -1)"
R=$(http_port 19654 /)
case "$R" in
*"403 Forbidden"*"not in http_allow"*) ok "a refused client gets a 403 that names http_allow";;
*) bad "a refused client got: $(echo "$R" | head -2 | tr '\n' ' ')"
   echo "       node4 log: $(grep -E "ERROR|WARNING" "$D/n4.log" 2>/dev/null | tail -2 | tr '\n' ' ')";;
esac
case "$R" in
*"Content-Length: 46"*) ok "the 403 carries its length";;
*) bad "the 403 has no or a wrong Content-Length: $(echo "$R" | /bin/grep -i length)";;
esac

echo "httptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
