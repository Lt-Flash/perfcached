#!/bin/sh
# pagejstest.sh - S196: RUN the status page's script, do not grep it.
#
# S194 defined wq()/wcalls() inside drawCmds() while wmax()/wspan() - both
# top level - call wq().  Every render then died on
# `ReferenceError: wq is not defined` in the row loop, before the table was
# written.  get() wraps the WHOLE callback in try/catch and reports a failed
# callback as a failed fetch, so the page said "this node did not answer" and
# nobody saw the real error for a day.  Every page assertion in this suite
# until now was a grep for a substring, and a substring cannot tell you a
# function is out of scope.
#
# This drives the real page against the daemon's own /stats through js2py
# (ES5.1, pure python) with a stub DOM, and asserts what a browser would see.
# js2py is not on the CI runner, so the suite SKIPS loudly there rather than
# passing without running.
# Usage: test/pagejstest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

python3 -c "import js2py" 2>/dev/null || {
	echo "pagejstest: SKIPPED - no js2py on this host (pip3 install js2py to run it)"
	echo "pagejstest: the page's script was NOT executed here"
	exit 0
}

D=$(mktemp -d /var/tmp/pcjs.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT TERM INT

cat > "$D/a.conf" <<CONF
[daemon]
workers = 2
log_level = notice
slowlog_usec = 0
[memory]
arena_mb = 64
[secrets]
client = js-client-secret
[listen]
resp = 127.0.0.1:17804
tcp = 127.0.0.1:17806
http = 127.0.0.1:17805
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 600 "$D/a.conf"
"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/a.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/a.log" || { echo "daemon did not start"; cat "$D/a.log"; exit 1; }

PCJS_DIR="$D" python3 - <<'PY_EOF'
import json, os, re, socket, sys, urllib.request, warnings
import pcnative
warnings.filterwarnings("ignore")
import js2py

npass = nfail = 0
def ok(m):
    global npass; npass += 1; print("  ok   " + m)
def bad(m):
    global nfail; nfail += 1; print("  FAIL " + m)

# --- traffic on two dialects (the RESP door, then CMD frames on the native
# door), so the rows carry a dialect worth printing ---
s = socket.create_connection(("127.0.0.1", 17804), 8); s.settimeout(20)
f = s.makefile("rwb")
def resp(*a):
    f.write(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
    f.flush()
    line = f.readline()
    if line[:1] == b"$":
        n = int(line[1:])
        if n >= 0: f.read(n + 2)
    return line
resp("AUTH", "js-client-secret")
for i in range(25): resp("SET", "k%d" % i, "v%d" % i)
for i in range(40): resp("GET", "k%d" % (i % 25))

js = socket.create_connection(("127.0.0.1", 17806), 8); js.settimeout(20)
jf = pcnative.wrap(js)                 # each line goes out as one CMD frame (S317)
def jreq(method, params, rid=[0]):
    rid[0] += 1
    jf.write((json.dumps({"jsonrpc": "2.0", "id": rid[0], "method": method,
                          "params": params}) + "\n").encode()); jf.flush()
    return json.loads(jf.readline())
for i in range(12): jreq("get", {"col": "0", "key": "k%d" % i})

page = urllib.request.urlopen("http://127.0.0.1:17805/", timeout=10).read().decode("utf-8", "replace")
stats = urllib.request.urlopen("http://127.0.0.1:17805/stats", timeout=10).read().decode()
members = urllib.request.urlopen("http://127.0.0.1:17805/members", timeout=10).read().decode()
script = re.search(r"<script>(.*)</script>", page, re.S).group(1)

STUB = """
var __els={}, __touched=[];
function El(id){ this.id=id; this.innerHTML=''; this.textContent=''; this.hidden=false;
  this.children=[]; this.style={}; this.offsetHeight=100; this.__h=0; this.__i=0;
  this.classList={contains:function(c){return false;},toggle:function(c){return false;},
                  add:function(c){},remove:function(c){}};
  this.addEventListener=function(a,b){}; this.appendChild=function(k){};
  this.getAttribute=function(a){return null;};
  this.getBoundingClientRect=function(){return {left:0,width:100,top:0,height:10};};
  this.closest=function(q){return null;}; }
var document={ getElementById:function(id){ __touched.push(id);
    if(!__els[id]) __els[id]=new El(id); return __els[id]; },
  querySelectorAll:function(q){ return []; }, addEventListener:function(a,b){} };
var location={port:"17805"};
// js2py's native Math.min/Math.max raise TypeError when applied to a
// one-element array (Math.min.apply(null,[x])), which is valid JS the
// sparklines do on a single sample.  These are the ES5 semantics, written
// out, so the interpreter's gap cannot be read as a page fault.
Math.min=function(){var i,m=Infinity;for(i=0;i<arguments.length;i++)if(arguments[i]<m)m=+arguments[i];return m;};
Math.max=function(){var i,m=-Infinity;for(i=0;i<arguments.length;i++)if(arguments[i]>m)m=+arguments[i];return m;};
function setInterval(f,ms){return 0;} function setTimeout(f,ms){return 0;}
function confirm(m){return false;} function alert(m){}
var __DATA={};
function XMLHttpRequest(){ var self=this;
  this.readyState=0; this.status=0; this.responseText='';
  this.open=function(m,u,a){ self.__u=u; };
  this.send=function(){ self.readyState=4;
    if(__DATA[self.__u]!==undefined){ self.status=200; self.responseText=__DATA[self.__u]; }
    else { self.status=404; self.responseText=''; }
    if(self.onreadystatechange) self.onreadystatechange(); }; }
"""
ctx = js2py.EvalJs()
ctx.execute(STUB)
ctx.execute("__DATA['stats']=%s; __DATA['members']=%s;" % (json.dumps(stats), json.dumps(members)))
try:
    ctx.execute(script)
    loaded = None
except Exception as e:
    loaded = str(e)[:200]
(ok if loaded is None else bad)("the page's script loads without throwing (%s)" % (loaded or "clean"))

# The renderer is called DIRECTLY: get() catches everything the callback
# throws and turns it into "this node did not answer", so a render error is
# invisible through the polling path.  That is the bug this suite exists for.
ctx.execute("__touched=[]; var __e=null;"
            "try{ drawStats(JSON.parse(__DATA['stats'])); }"
            "catch(e){ __e=(e&&e.name?e.name+': ':'')+(e&&e.message?e.message:String(e)); }")
err = ctx.eval("__e")
(ok if not err else bad)("drawStats renders the live /stats without throwing (%s)"
                         % (err or "clean"))
if err:
    print("       last elements touched: %s" % ctx.eval("__touched.slice(-6).join(',')"))

# The header alarm: an operator must see a host in trouble without
# scrolling to the fleet.  This node's state blinks as its card would, and
# a chip beside it names the OTHER hosts in trouble, worst first, at the
# cards' colours (failed red, any other state but ready amber, a member
# that went silent amber, one that said goodbye calm).
def hal(members):
    ctx.execute("__e=null; try{ drawFleet(%s); }catch(e){ __e=String(e); }" % json.dumps(members))
    return (ctx.eval("__els['hal']?__els['hal'].hidden:null"),
            ctx.eval("__els['hal']?__els['hal'].className:null"),
            ctx.eval("__els['hal']?__els['hal'].textContent:'(no header alarm on this page)'"),
            ctx.eval("__e"))
def mem(node, st, me=False, gr=None):
    d = {"node": node, "addr": "127.0.0.%d" % node, "port": 1, "http": 2, "state": st,
         "self": me, "uptime_s": 10, "gone_s": (5 if st == "gone" else -1)}
    if gr:
        d["gone_reason"] = gr
    return d
h = hal([mem(1, "ready", True), mem(2, "ready"), mem(3, "ready")])
(ok if h[0] and not h[3] else bad)("a healthy fleet: no header alarm (%s)" % (h,))
h = hal([mem(1, "ready", True), mem(3, "recovering"), mem(2, "failed")])
(ok if not h[0] and h[1] == "hal red" and "node 2 failed" in h[2] and "node 3 recovering" in h[2]
    and h[2].index("node 2") < h[2].index("node 3") else bad)(
    "a failed and a recovering member: the header chip blinks RED and names both, worst first (%s)" % (h,))
h = hal([mem(1, "ready", True), mem(2, "healing")])
(ok if not h[0] and h[1] == "hal amber" else bad)("a member healing: the chip blinks AMBER (%s)" % (h,))
h = hal([mem(1, "failed", True), mem(2, "ready")])
(ok if h[0] else bad)("this node's own trouble is the header state's, not the chip's (%s)" % (h,))
h = hal([mem(1, "ready", True), mem(2, "gone", gr="goodbye"), mem(3, "gone", gr="silent")])
(ok if not h[0] and h[1] == "hal amber" and "node 3 silent" in h[2] and "node 2" not in h[2] else bad)(
    "a member that said goodbye is calm, one that went silent is amber (%s)" % (h,))
h = hal([mem(1, "ready", True)] + [mem(n, "failed") for n in range(2, 8)])
(ok if not h[0] and "+3 more" in h[2] else bad)("six in trouble: three named, '+3 more' (%s)" % (h[2],))
def state_cls(state):
    st = json.loads(stats); st["state"] = state
    ctx.execute("__e=null; try{ drawStats(%s); }catch(e){ __e=String(e); }" % json.dumps(st))
    mm = re.search(r'<span class="(hstate[^"]*)">state <b>', ctx.eval("__els['idbits'].innerHTML") or "")
    return (mm.group(1).strip() if mm else None), ctx.eval("__e")
(ok if state_cls("failed") == ("hstate red", None) else bad)(
    "this node FAILED: the header state blinks red (%s)" % (state_cls("failed"),))
(ok if state_cls("healing") == ("hstate amber", None) else bad)(
    "this node healing: the header state blinks amber (%s)" % (state_cls("healing"),))
(ok if state_cls("ready") == ("hstate", None) else bad)(
    "this node ready: the header state is calm (%s)" % (state_cls("ready"),))

# S264: the clients card.  Its chips add up to the open figure, and the
# connection limit - derived from the descriptor limit, 524,165 on the
# test fleet - is not a chip beside the dialects ("max 524,165" read as a
# peak) unless it matters.
def clients_card(st):
    ctx.execute("__e=null; try{ drawStats(%s); }catch(e){ __e=String(e); }" % json.dumps(st))
    mh = ctx.eval("__els['metrics']?__els['metrics'].innerHTML:''") or ""
    mm = re.search(r"<div class=k>clients</div><div class=v>(.*?)</div><div class=s>(.*?)</div>", mh, re.S)
    return (mm.group(2) if mm else ""), ctx.eval("__e")
def chipsum(chips):
    return sum(int(n.replace(",", "")) for n in re.findall(r">(?:binary|resp|idle) ([\d,]+)<", chips))
live_st = json.loads(stats)
chips, e = clients_card(live_st)
(ok if chips and not e and "max " not in chips and "limit" not in chips else bad)(
    "the clients card shows no 'max' chip, and no limit while far from it (%s)" % (re.sub(r"<[^>]*>", " ", chips).strip() or e))
(ok if chipsum(chips) == live_st["clients"]["open"] else bad)(
    "its chips add up to the open figure: %d of %d" % (chipsum(chips), live_st["clients"]["open"]))
held = json.loads(stats)
held["clients"]["open"] += 3                 # three that have not sent a request
held["clients"]["max"] = held["clients"]["open"]
chips, e = clients_card(held)
(ok if "idle 3<" in chips and chipsum(chips) == held["clients"]["open"] else bad)(
    "three connections with no request yet read 'idle 3', and the chips still add up (%s)" % re.sub(r"<[^>]*>", " ", chips).strip())
(ok if re.search(r"var\(--bad\)[^>]*>limit ", chips) else bad)(
    "at the limit, the limit is on the card in the colour of trouble")

html = ctx.eval("__els['cmds']?__els['cmds'].innerHTML:''") or ""
rows = html.count("<tr")
# S197: the rows are the verbs the WINDOW saw - the daemon is seconds
# old here, so that is every verb called - and nothing else
live = [c for c in json.loads(stats).get("commands", []) if c.get("win_calls", 0) > 0]
(ok if rows == len(live) and rows > 1 else bad)(
    "the commands table draws one row per verb the window saw: %d rows for %d verbs" % (rows, len(live)))
(ok if "nothing called" not in html and "\u2014" not in html and "no commands yet" not in html else bad)(
    "no placeholder and no dashed row with verbs in hand")
(ok if re.search(r"get</td>|>get<", html) or "get" in html else bad)(
    "the RESP get is among them")

# S189's convention, on every surface that names a command: a CMD request
# is the row cmd:<method>, drawn as "get (cmd)" (S317); a RESP name is bare
plane = ctx.eval("__els['plane']?__els['plane'].innerHTML:''") or ""
m = re.search(r"commands\s*·\s*by calls(.*?)</div></div>", plane, re.S)
card = m.group(1) if m else ""
(ok if card else bad)("the 'commands - by calls' card is on the plane")
(ok if card and "(cmd)" in card and "(resp)" in card else bad)(
    "and names each verb with its dialect, as the table does (%s)"
    % (re.sub(r"<[^>]*>", " ", card)[:90].strip() if card else "no card"))
(ok if card and "cmd:" not in card else bad)(
    "not as a composite name a reader has to parse")

# S214: the plane is re-ordered by card height on every redraw, so a card
# whose row count follows the traffic changes rank and jumps about the
# page.  The cards that vary are always their full height now - the
# commands card 8 rows, the slow log 6 - whatever the window holds.
def rows_of(html, title):
    mm = re.search(re.escape(title) + r"</h3>(.*?)</div></div>", html, re.S)
    return (mm.group(1) + "</div>").count('class="kv') if mm else -1
rows_full = rows_of(plane, "5 min")
(ok if rows_full == 8 else bad)(
    "the commands card is 8 rows tall with a busy window (%d)" % rows_full)
(ok if rows_of(plane, "tail") >= 6 else bad)(
    "the slow-log card is at least 6 rows tall (%d)" % rows_of(plane, "tail"))
(ok if re.search(r'<div class="stk"><div class="pc"><h3>memory</h3>.*?</div></div>'
                 r'<div class="pc"><h3>backpressure</h3>', plane, re.S) else bad)(
    "memory and backpressure share one stacked cell")
# ... and so do the rest of the short cards, from the GROUPS table: every
# group is ONE cell holding its cards in the table's order, and no card was
# lost or doubled by the regrouping.  The cells are read off the page's own
# table, not written down here: S317 took 'native - json' off the page and
# with it the table's one trio, and a test that still wanted a trio failed
# on every page since (the DOM half only runs where js2py is, never in CI)
titles = re.findall(r"<h3>(.*?)</h3>", plane)
(ok if len(titles) >= 18 and len(set(titles)) == len(titles) else bad)(
    "every card is on the plane exactly once after the regrouping (%d cards, %d distinct)"
    % (len(titles), len(set(titles))))
def stacks(html):
    return [re.findall(r"<h3>(.*?)</h3>", c)
            for c in re.findall(r'<div class="stk">(.*?)</div></div></div>', html, re.S)]
# GROUPS lives inside drawStats(), so it is read out of the script as served
gm = re.search(r"GROUPS=(\[\[.*?\]\]);", script, re.S)
groups = json.loads(ctx.eval("JSON.stringify(%s)" % gm.group(1))) if gm else []
want = [g for g in ([t for t in g if t in titles] for g in groups) if len(g) >= 2]
got = stacks(plane)
missing = sorted(set(t for g in groups for t in g) - set(titles))
(ok if groups and got == want and len(want) == len(groups) else bad)(
    "the short cards share cells as the GROUPS table says: %d stacks of %s cards, for %d groups%s"
    % (len(got), [len(c) for c in got], len(groups),
       "; not on the plane: %s" % missing if missing else
       "" if got == want else "; got %s" % got))
# the table has no trio today, so the page is run again with one: a trio
# is one cell of three, a title that is not on the plane is skipped, a
# group left with one card draws it bare, and every card is still there once
p3, e3 = "", "no GROUPS table in the script"
if gm:
    c3 = js2py.EvalJs()
    c3.execute(STUB)
    c3.execute("__DATA['stats']=%s; __DATA['members']=%s;" % (json.dumps(stats), json.dumps(members)))
    c3.execute(script.replace(gm.group(0), "GROUPS=%s;" % json.dumps(
        [["memory", "no such card", "backpressure", "process"], ["listeners", "no such card"]]), 1))
    c3.execute("var __e=null; try{ drawStats(JSON.parse(__DATA['stats'])); }catch(e){ __e=String(e); }")
    p3, e3 = c3.eval("__els['plane']?__els['plane'].innerHTML:''") or "", c3.eval("__e")
t3 = re.findall(r"<h3>(.*?)</h3>", p3)
(ok if not e3 and stacks(p3) == [["memory", "backpressure", "process"]]
       and sorted(t3) == sorted(titles) else bad)(
    "a trio in the table is one cell of three, and a lone survivor stands bare (%s)"
    % (e3 or stacks(p3)))
(ok if re.search(r'<div class="stk"><div class="pc"><h3>memory budget</h3>.*?<h3>cluster auth</h3>', plane, re.S) else bad)(
    "memory budget stands over cluster auth")
# ... and the ORDER of the cells, once decided, is kept: heights that
# drift between polls must not move anything; a new cell or a new width may
o1 = list(ctx.eval("planeOrder(['a','b','c'],[10,30,20],900)"))
o2 = list(ctx.eval("planeOrder(['a','b','c'],[99,1,50],900)"))
o3 = list(ctx.eval("planeOrder(['a','b','c','d'],[99,1,50,70],900)"))
o4 = list(ctx.eval("planeOrder(['a','b','c','d'],[1,2,3,4],1400)"))
(ok if o1 == ['b','c','a'] else bad)("the first draw orders the cells tallest first (%s)" % o1)
(ok if o2 == o1 else bad)("heights that change between polls move nothing (%s)" % o2)
(ok if o3 == ['a','d','c','b'] else bad)("a cell that appears re-decides the order (%s)" % o3)
(ok if o4 == ['d','c','b','a'] else bad)("and so does a new plane width (%s)" % o4)

# and with NOTHING in the window, the table says so - through the same
# renderer, on a /stats whose window counts are zeroed
zeroed = json.loads(stats)
for c in zeroed.get("commands", []):
    for k in list(c):
        if k.startswith("win_"): c[k] = 0
ctx.execute("__els['cmds'].innerHTML=''; __e=null; try{ drawStats(%s); }catch(e){ __e=String(e); }"
            % json.dumps(zeroed))
err2 = ctx.eval("__e")
html2 = ctx.eval("__els['cmds'].innerHTML") or ""
label2 = ctx.eval("__els['cmdtog'].textContent") or ""
(ok if not err2 and "nothing called in the last 5 min" in html2 and html2.count("<tr") == 1 else bad)(
    "an empty window renders one placeholder row that says so (%s)" % (err2 or html2[:60]))
(ok if label2 == "nothing in the last 5 min" else bad)(
    "and the fold label agrees (%r)" % label2)
plane2 = ctx.eval("__els['plane']?__els['plane'].innerHTML:''") or ""
rows_empty = rows_of(plane2, "5 min")
(ok if rows_empty == 8 and rows_empty == rows_full else bad)(
    "and the commands card is the SAME height with an empty window "
    "(%d rows vs %d) - its rank on the plane cannot change" % (rows_empty, rows_full))
# S340: a read-through collection gets its own line under its row - what the
# upstream answered (fetched found/absent, from fetched copies, absent from
# the negative cache, copies held), which the row's counters cannot say.
# PROD 2026-10-07: prod-redis read 0 hits while the SBC blocked devices
# from keys it had read through.  A plain collection gets no such line, and
# the upstream's host is text, never markup.
rt = json.loads(stats)
cs = rt.get("collections", [])
(ok if cs else bad)("the live /stats has a collection to dress as read-through (%d)" % len(cs))
if cs:
    cs[0]["upstream"] = {"host": "10.9.9.13<img src=x onerror=x()>", "port": 6379, "tracking": True,
                         "fetches": 1000, "absent": 990, "fill_string": 10, "fill_hash": 0, "fill_json": 0,
                         "shadow_hits": 25, "negative_hits": 4000, "shadow_entries": 48,
                         "fetch_us_avg": 303, "errors": 0, "timeouts": 0}
    ctx.execute("__els['cols']&&(__els['cols'].innerHTML=''); __e=null; try{ drawStats(%s); }catch(e){ __e=String(e); }"
                % json.dumps(rt))
    e3 = ctx.eval("__e")
    ch = ctx.eval("__els['cols']?__els['cols'].innerHTML:''") or ""
    up = re.findall(r"<tr class=up>.*?</tr>", ch)
    (ok if not e3 and len(up) == 1 else bad)(
        "one read-through line, under the collection that has an upstream (%s; %d lines)" % (e3 or "clean", len(up)))
    u = up[0] if up else ""
    # a read that waited for a fetch is counted again where it ends (a copy
    # or an absent mark), so the fetches are a SUBSET of the lookups
    want = ["read-through", "tracking on", "<b>4025</b> lookups = <b>25</b> with a value + <b>4000</b> absent",
            "<b>1000</b> of them fetched from the upstream (10 found, 990 absent, avg 303", "<b>48</b> copies held"]
    miss = [w for w in want if w not in u]   # nf() groups only 5+ digits
    (ok if u and not miss else bad)("the line adds up: 25 with a value + 4,000 absent = 4,025 lookups, 1,000 of them fetched (missing: %s)"
                                    % (miss or "none"))
    (ok if u and "<img" not in u and "&lt;img" in u else bad)("the upstream host is escaped, not markup (%s)" % u[:120])
print("pagejstest: %d passed, %d failed" % (npass, nfail))
sys.exit(1 if nfail else 0)
PY_EOF
rc=$?
echo "pagejstest: done (rc=$rc)"
exit $rc
