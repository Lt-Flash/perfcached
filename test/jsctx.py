"""jsctx.py - a persistent JavaScript context for the page suites, with the
two calls they make of js2py: EvalJs().execute(code) and .eval(expr).

Backed by node - one `vm` context per EvalJs, kept in one node process, each
call one JSON line each way - the engine the CI runners carry (Debian's
nodejs; GitHub's ubuntu image has it).  js2py (ES5.1, pure python) where
node is not.  js2py was the only engine the suite knew, and the runners never
had it: the page's script did not run in CI at all, and a function defined
out of the fleet table's scope reached a live dashboard (12nx, 2026-10-19,
'dfull is not defined').

    available()   the engine this host has: "node", "js2py" or None
"""
import json
import shutil
import subprocess

NODE = shutil.which("node") or shutil.which("nodejs")

# One line in: {"op": "exec"|"eval", "code": ...}; one line out: {"ok": true,
# "v": value} or {"ok": false, "e": "Name: message"}.  A value goes back as
# JSON: undefined as null, and what JSON cannot carry (a cycle, a function)
# as its String().
_DRIVER = r"""
const vm = require("vm");
const ctx = vm.createContext({});
const rl = require("readline").createInterface({ input: process.stdin, crlfDelay: Infinity });
function enc(v) {
  if (v === undefined) return null;
  try { return JSON.parse(JSON.stringify(v)); } catch (e) { return String(v); }
}
rl.on("line", (l) => {
  let m, r;
  try {
    m = JSON.parse(l);
    const v = vm.runInContext(m.code, ctx, { filename: "page.js" });
    r = { ok: true, v: m.op === "eval" ? enc(v) : null };
  } catch (e) {
    r = { ok: false, e: (e && e.name ? e.name + ": " : "") + (e && e.message ? e.message : String(e)) };
  }
  process.stdout.write(JSON.stringify(r) + "\n");
});
"""


class JsError(Exception):
    """a JavaScript exception, as 'Name: message'"""


def available():
    if NODE:
        return "node"
    try:
        import js2py  # noqa: F401
        return "js2py"
    except Exception:
        return None


class _NodeCtx:
    def __init__(self):
        self.p = subprocess.Popen([NODE, "-e", _DRIVER], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)

    def _call(self, op, code):
        self.p.stdin.write(json.dumps({"op": op, "code": code}) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise JsError("node exited (rc %s)" % self.p.poll())
        r = json.loads(line)
        if not r["ok"]:
            raise JsError(r["e"])
        return r["v"]

    def execute(self, code):
        self._call("exec", code)

    def eval(self, expr):
        return self._call("eval", expr)

    def __del__(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=5)
        except Exception:
            pass


def EvalJs():
    if NODE:
        return _NodeCtx()
    import js2py
    return js2py.EvalJs()
