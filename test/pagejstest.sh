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
# S341: the fleet COUNT beside the chip - members ready of the fleet's
# size.  PROD 2026-10-07: "this status never changes when some of the nodes
# are missing or recovering" - a member that left is calm in the S335 chip
# and one missing from the list is in no chip at all.
def hfl(members, fl=None):
    ctx.execute("FLINFO=%s; __e=null; try{ drawFleet(%s); }catch(e){ __e=String(e); }"
                % (json.dumps(fl), json.dumps(members)))
    return (ctx.eval("__els['hfl']?__els['hfl'].hidden:null"),
            ctx.eval("__els['hfl']?__els['hfl'].className:null"),
            ctx.eval("__els['hfl']?__els['hfl'].textContent:'(no fleet count on this page)'"),
            ctx.eval("__e"), ctx.eval("__els['hal']?__els['hal'].hidden:null"))
three = [mem(1, "ready", True), mem(2, "ready"), mem(3, "ready")]
h = hfl(three)
(ok if h[0] is False and h[1] == "hal" and h[2] == "fleet 3/3 ready" and not h[3] else bad)(
    "three ready: 'fleet 3/3 ready', calm (%s)" % (h[:4],))
h = hfl([mem(1, "ready", True), mem(2, "gone", gr="goodbye"), mem(3, "ready")])
(ok if h[1] == "hal amber" and h[2] == "fleet 2/3 — node 2 left" and h[4] is True else bad)(
    "a member that left: the count goes AMBER and says so, the S335 chip stays quiet (%s)" % (h,))
h = hfl([mem(1, "ready", True), mem(2, "ready"), mem(3, "recovering")])
(ok if h[1] == "hal amber" and h[2] == "fleet 2/3 — node 3 recovering" else bad)(
    "a member recovering: amber '2/3 - node 3 recovering' (%s)" % (h[:3],))
h = hfl([mem(1, "ready", True), mem(2, "failed"), mem(3, "ready")])
(ok if h[1] == "hal red" and "node 2 failed" in h[2] else bad)("a member failed: RED (%s)" % (h[:3],))
h = hfl([mem(1, "ready", True), mem(2, "ready")], {"expect": 3, "seen_max": 2})
(ok if h[1] == "hal amber" and h[2] == "fleet 2/3 — 1 missing" else bad)(
    "two listed against [cluster] expect 3: amber '2/3 - 1 missing' (%s)" % (h[:3],))
h = hfl([mem(1, "ready", True), mem(2, "ready")], {"expect": 0, "seen_max": 3})
(ok if h[1] == "hal amber" and h[2] == "fleet 2/3 — 1 missing" else bad)(
    "two listed, three seen since start: amber '1 missing' (%s)" % (h[:3],))
h = hfl([mem(1, "ready", True), mem(2, "failed"), mem(3, "gone", gr="silent")])
(ok if h[1] == "hal red" and h[2].startswith("fleet 1/3") else bad)(
    "one of three ready - below a majority: RED (%s)" % (h[:3],))
lf = json.loads(members).get("fleet") if isinstance(members, str) else None
(ok if isinstance(lf, dict) and isinstance(lf.get("expect"), int) and isinstance(lf.get("seen_max"), int) else bad)(
    "the live /members carries the fleet's size (fleet = %s)" % (lf,))
h = hfl([mem(1, "ready", True)], {"expect": 0, "seen_max": 1})
(ok if h[0] is True else bad)("a single node: no fleet count (%s)" % (h[:3],))
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
# S366: refusals at the memory floor are inside clients.refused.  The floor
# gets its own chip - in the colour of trouble, its figures in the hover -
# and the limit chip counts only the limit's own refusals, so a node far
# from max_clients does not read "limit 136,696 - 7 refused".
mg = json.loads(stats)
mg["clients"]["refused"] = 7; mg["clients"]["win_refused"] = 7
mg["clients"]["memory"] = {"guard": True, "refusing": True, "refused": 7, "win_refused": 7,
                           "floor": 161061270, "arena_reserve": 0, "available": 100 << 20,
                           "limit": 1 << 30, "source": "meminfo"}
chips, e = clients_card(mg)
txt = re.sub(r"<[^>]*>", " ", chips)
(ok if not e and re.search(r'var\(--bad\)[^>]*title="[^"]*less than 154 MB stays free[^"]*">memory floor \u00b7 7 refused in 5 min<', chips) else bad)(
    "refusing at the memory floor: a red 'memory floor - 7 refused in 5 min' chip, the floor in its hover (%s)" % (e or txt.strip()))
(ok if "limit" not in txt else bad)("and no limit chip for refusals that were the floor's (%s)" % txt.strip())
mg["clients"]["refused"] = 10; mg["clients"]["win_refused"] = 10
mg["clients"]["memory"]["refusing"] = False
chips, e = clients_card(mg)
(ok if re.search(r">limit [\d,\s\u00a0\u202f]+ \u00b7 3 refused in 5 min<", chips) and "memory floor \u00b7 7" in chips else bad)(
    "10 refused, 7 at the floor: the limit chip says 3, the floor chip still shows its 7 while they are in the window (%s)" % re.sub(r"<[^>]*>", " ", chips).strip())

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
    # S351: the line is the last five minutes - the daemon's win_ figures,
    # set apart from the lifetime totals so the test can tell which it read
    cs[0]["upstream"] = {"host": "10.9.9.13<img src=x onerror=x()>", "port": 6379, "tracking": True,
                         "fetches": 91000, "absent": 90990, "fill_string": 10, "fill_hash": 0, "fill_json": 0,
                         "shadow_hits": 925, "negative_hits": 994000, "shadow_entries": 48,
                         "fetch_us_avg": 999, "errors": 0, "timeouts": 0,
                         "win_fetches": 1000, "win_absent": 990, "win_fill_string": 10, "win_fill_hash": 0,
                         "win_fill_json": 0, "win_shadow_hits": 25, "win_negative_hits": 4000,
                         "win_errors": 0, "win_timeouts": 0, "win_fetch_n": 1000, "win_fetch_us_sum": 303000}
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
    # S348: a collection that refuses writes says so on its line, with what
    # it refused in the window
    cs[0]["upstream"]["writes"] = "refuse"; cs[0]["upstream"]["win_writes_refused"] = 14
    ctx.execute("__els['cols']&&(__els['cols'].innerHTML=''); __e=null; try{ drawStats(%s); }catch(e){ __e=String(e); }" % json.dumps(rt))
    u2 = (re.findall(r"<tr class=up>.*?</tr>", ctx.eval("__els['cols']?__els['cols'].innerHTML:''") or "") or [""])[0]
    (ok if "<b>read-only</b> (14 writes refused)" in u2 else bad)("a read-only read-through says so: read-only (14 writes refused) (%s)" % u2[:160])
# S347: the headline hit rate is the CACHES' rate; a read-through
# collection - mostly lookups of keys the upstream does not have - gets its
# own figure, the share answered here without the upstream.  PROD 10-07/08:
# the card read 6-12% beside th at 100.0% and prod-redis at 0.1%.
def coll(base, name, h, m, up=None):
    c = json.loads(json.dumps(base)); c["name"] = name
    c["hits_client"] = h; c["misses_client"] = m; c["hits"] = h; c["misses"] = m
    # S351: the card's 5-minute line reads the daemon's windows; here they
    # are what the second render adds, as a window would be
    c["win_hits_client"] = c.get("_wh", 0); c["win_misses_client"] = c.get("_wm", 0)
    c.pop("fleet", None); c.pop("upstream", None)
    if up is not None:
        c["upstream"] = {"host": "10.9.9.13", "port": 6379, "tracking": True, "fetches": up,
                         "absent": up, "fill_string": 0, "fill_hash": 0, "fill_json": 0,
                         "shadow_hits": h, "negative_hits": m, "shadow_entries": 1,
                         "fetch_us_avg": 300, "errors": 0, "timeouts": 0}
    return c
def win(c, h, m, f=None):
    c["win_hits_client"] = h; c["win_misses_client"] = m
    if f is not None:
        c["upstream"]["win_fetches"] = f
    return c
b0 = json.loads(stats); base = (b0.get("collections") or [{}])[0]
def card(st):
    ctx.execute("__e=null; try{ drawStats(%s); }catch(e){ __e=String(e); }" % json.dumps(st))
    mh = ctx.eval("__els['metrics']?__els['metrics'].innerHTML:''") or ""
    m = re.search(r'<div class=metric title="([^"]*)"><div class=k>hit rate \(now\)</div><div class=v>([^<]*)</div><div class=s>([^<]*)</div>', mh)
    return (m.groups() if m else None), ctx.eval("__e")
s1 = json.loads(stats); s1["since"] = {"reset_at": 77, "s": 100}
s1["collections"] = [coll(base, "th", 1000, 0), coll(base, "pr", 10, 5000, up=200)]
card(s1)                                   # the window's first sample
s2 = json.loads(json.dumps(s1))
s2["collections"] = [win(coll(base, "th", 1073, 0), 73, 0), win(coll(base, "pr", 11, 5522, up=226), 1, 522, 26)]
c2, e5 = card(s2)
(ok if c2 and not e5 and c2[1] == "100.0%" else bad)(
    "the card is the caches' rate: th's 100.0%%, not the lookup-weighted 12%% (%s, %s)" % (c2, e5))
(ok if c2 and "caches" in c2[2] and "read-through 95.0% answered here" in c2[2] else bad)(
    "and the read-through collection has its own figure: 523 lookups, 26 fetched = 95.0%% answered here (%s)" % (c2[2] if c2 else None,))
(ok if c2 and c2[0].startswith("last 5 min") and "th: 100.0% of 73 lookups" in c2[0] and "pr (read-through): 0.2% found of 523 lookups, 95.0% answered here" in c2[0] else bad)(
    "the hover names each collection's share (%s)" % (c2[0][:160] if c2 else None,))
s3 = json.loads(json.dumps(s2)); s3["collections"] = [win(coll(base, "pr", 11, 5522, up=226), 1, 522, 26)]
c3, _ = card(s3)
(ok if c3 and "all collections" in c3[2] and "answered here" not in c3[2] else bad)(
    "only read-through collections: the card falls back to all of them (%s)" % (c3[2] if c3 else None,))
# S351 (12js reverted): the headings say "last 5 min" and the figures under
# them are the daemon's five-minute windows - a fleet-shaped row whose
# lifetime totals are in the millions shows its window, and a counter the
# daemon has no window for yet shows a dash, never a total.
sw = json.loads(stats); sw["since"] = {"reset_at": 0, "s": 99999, "win_s": 300}
for x in sw.get("collections", []):
    x["hits_client"] = 4567890; x["misses_client"] = 1234567; x["stores_client"] = 7654321; x["expired"] = 3333333
    x["win_hits_client"] = 4321; x["win_misses_client"] = 123; x["win_stores_client"] = 765; x["win_expired"] = None
    x["fleet"] = {"basis": "fullest", "members": 3, "reporting": 3, "entries": 3, "copies": 9,
                  "expired": 9999999, "hits": 13703670, "misses": 3703701, "stores": 22962963, "removes": 0,
                  "win_expired": None, "win_hits": 12963, "win_misses": 369, "win_stores": 2295, "win_removes": 0}
ctx.execute("__els['cols']&&(__els['cols'].innerHTML=''); __e=null; try{ drawStats(%s); }catch(e){ __e=String(e); }" % json.dumps(sw))
e6 = ctx.eval("__e"); ch6 = ctx.eval("__els['cols']?__els['cols'].innerHTML:''") or ""
cells = re.findall(r'<span class=hv><span class=n>([^<]*)</span><span class=hl>([^<]*) here</span></span>', ch6)
(ok if not e6 and ("12\u202f963", "4321") in cells and ("369", "123") in cells and ("2295", "765") in cells else bad)(
    "fleet cells show the five-minute windows (12,963 / 4,321 here), not the lifetime (13,703,670) (%s; %s)" % (e6 or "clean", cells[:6]))
(ok if ("-", "-") in cells and "9\u202f999\u202f999" not in ch6 and "3\u202f333\u202f333" not in ch6 else bad)(
    "a counter with no window yet shows a dash, never its total (%s)" % ([c for c in cells if c[0] == "-"][:2],))
(ok if "13\u202f703\u202f670" not in ch6 and "4\u202f567\u202f890" not in ch6 else bad)("no lifetime total reaches the table")
(ok if "<h2>collections <span class=s>counters: last 5 min</span>" in page and "<h2>cluster plane <span class=s>counters: last 5 min</span></h2>" in page else bad)(
    "the collections and cluster-plane headings both say counters: last 5 min")
(ok if "since start" not in re.sub(r"/\\*.*?\\*/", "", script, flags=re.S) else bad)(
    "no 'since start' label is left anywhere in the page's script")
# S350: the WAL card's times in the unit a reader thinks in.  PROD
# 2026-10-08 during a backup: "fsync now 104,010 us", "since start
# 12,827 / 1,488,827 us", "probe 3,357 / 5,016 us" - the operator had to ask
# which unit, and whether the max was over a million of them.
wt = json.loads(stats)
wt["wal"] = {"fsync": "everysec", "appended": 10, "bytes": 1000, "dropped": 0, "late": 0,
             "overruns": 0, "heals": 0, "heals_recent": 0, "free_segments": 8, "staged": 0, "unsynced": 89,
             "observed": {"fsync_n": 6833, "fsync_avg_us": 999, "fsync_recent_us": 104010,
                          "fsync_max_us": 9999999, "probe_p50_us": 3357, "probe_underestimated": True,
                          "win_fsync_n": 100, "win_fsync_sum_us": 1282700, "win_fsync_max_us": 1488827},
             "probe": {"fsync_p50_us": 3357, "fsync_p99_us": 5016, "seq_mb_s": 274}}
# the cards are built inside drawStats() and land in #plane
ctx.execute("__els['plane']&&(__els['plane'].innerHTML=''); __e=null; try{ drawStats(%s); }catch(e){ __e=String(e); }" % json.dumps(wt))
e4 = ctx.eval("__e")
pl = ctx.eval("__els['plane']?__els['plane'].innerHTML:''") or ""
m = re.search(r"<h3>durability \u00b7 wal</h3>(.*?)</div></div>", pl)
wc = m.group(1) + "</div>" if m else ""
(ok if wc else bad)("the WAL card is on the plane (%d bytes of plane)" % len(pl))
rows = dict(re.findall(r'<div class="kv(?: w)?"><span>([^<]*)</span><span>([^<]*)</span></div>', wc))
want = {"fsync now (ewma)": "104 ms", "fsync (avg / max)": "12.8 ms / 1.5 s",
        "probe p50/p99": "3.4 ms / 5.0 ms, 274 MB/s"}
got = dict((k, rows.get(k)) for k in want)
(ok if not e4 and got == want else bad)("the WAL card shows its times in us / ms / s (%s; got %s)" % (e4 or "clean", got))
# S365: connects per second on the clients card.  The clients figure is a
# gauge - open at the poll - and a storm of one-millisecond connections
# (5,000 a second on 245, 10-09) moved it from 75 to 94 at most.  The rate
# differences both doors' accept counters between polls; the average is
# the daemon's five-minute windows.  The door cards say what their figure
# counts: "connects (5 min)", not "connections" / "clients".
ctx.execute("var __now=1000000; Date.now=function(){return __now;};")
cs = json.loads(stats); cs["since"] = {"reset_at": 4242, "s": 1000, "win_s": 300}
cs.setdefault("resp", {}); cs.setdefault("native", {})
cs["resp"]["conns"] = 100000; cs["native"]["conns"] = 50000
cs["resp"]["win_conns"] = 60000; cs["native"]["win_conns"] = 30000
def conn_line(st):
    ctx.execute("__els['plane']&&(__els['plane'].innerHTML=''); __e=null; try{ drawStats(%s); }catch(e){ __e=String(e); }" % json.dumps(st))
    mh = ctx.eval("__els['metrics']?__els['metrics'].innerHTML:''") or ""
    m = re.search(r"<div class=k>clients</div>.*?<div class=s>connects ([^<]*)</div>", mh, re.S)
    return (m.group(1) if m else None), ctx.eval("__e")
l1, e7 = conn_line(cs)                     # a new reset_at: no baseline yet
ctx.execute("__now+=2000;")
cs2 = json.loads(json.dumps(cs)); cs2["resp"]["conns"] += 6000; cs2["native"]["conns"] += 4000
l2, e8 = conn_line(cs2)
(ok if not e7 and l1 == "— · 5 min avg 300/s" else bad)(
    "first sample: no rate yet; the 5 min average is the daemon's windows, 90,000 in 300 s = 300/s (%s, %s)" % (l1, e7))
(ok if not e8 and l2 == "5000/s · 5 min avg 300/s" else bad)(
    "10,000 connects across both doors in 2 s read 5000/s (%s, %s)" % (l2, e8))
pl = ctx.eval("__els['plane']?__els['plane'].innerHTML:''") or ""
nb = re.search(r"<h3>native · binary</h3>(.*?)</div></div>", pl); rd = re.search(r"<h3>resp door</h3>(.*?)</div></div>", pl)
lab = lambda m: re.findall(r'<div class="kv(?: w)?"><span>([^<]*)</span>', m.group(1)) if m else []
(ok if "connects (5 min)" in lab(nb) and "connections" not in lab(nb) else bad)(
    "the native door card says 'connects (5 min)' (%s)" % lab(nb))
(ok if "connects (5 min)" in lab(rd) and "clients" not in lab(rd) else bad)(
    "the resp door card says 'connects (5 min)', not 'clients' (%s)" % lab(rd))
ctx.execute("__now+=2000;")
cs3 = json.loads(json.dumps(cs2)); cs3["since"]["reset_at"] = 4343; cs3["resp"]["conns"] = 10; cs3["native"]["conns"] = 5
l3, e9 = conn_line(cs3)
(ok if not e9 and l3 and l3.startswith("—") else bad)(
    "after a stats reset the counters fall: no negative rate, a dash until the next sample (%s)" % (l3,))
print("pagejstest: %d passed, %d failed" % (npass, nfail))
sys.exit(1 if nfail else 0)
PY_EOF
rc=$?
echo "pagejstest: done (rc=$rc)"
exit $rc
