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
warnings.filterwarnings("ignore")
import js2py

npass = nfail = 0
def ok(m):
    global npass; npass += 1; print("  ok   " + m)
def bad(m):
    global nfail; nfail += 1; print("  FAIL " + m)

# --- traffic on two dialects, so the rows carry a dialect worth printing ---
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
jf = js.makefile("rwb")
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

# S189's convention, on every surface that names a command
plane = ctx.eval("__els['plane']?__els['plane'].innerHTML:''") or ""
m = re.search(r"commands\s*·\s*by calls(.*?)</div></div>", plane, re.S)
card = m.group(1) if m else ""
(ok if card else bad)("the 'commands - by calls' card is on the plane")
(ok if card and "(json)" in card and "(resp)" in card else bad)(
    "and names each verb with its dialect, as the table does (%s)"
    % (re.sub(r"<[^>]*>", " ", card)[:90].strip() if card else "no card"))
(ok if card and "json:" not in card and "json." not in card else bad)(
    "not as a composite name a reader has to parse")

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
print("pagejstest: %d passed, %d failed" % (npass, nfail))
sys.exit(1 if nfail else 0)
PY_EOF
rc=$?
echo "pagejstest: done (rc=$rc)"
exit $rc
