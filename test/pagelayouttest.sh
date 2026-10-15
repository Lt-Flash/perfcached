#!/bin/sh
# pagelayouttest.sh - S342: the collections table fits its panel at desktop
# widths, in a real browser.
#
# PROD 2026-10-07, the operator: "fix scrolling, it shouldn't appear" - the
# collections table was 1,377 px in a 1,238 px panel at 1280 px and SIZES sat
# under a horizontal scrollbar.  A layout cannot be asserted in js2py
# (pagejstest.sh), so this drives headless Chromium through Playwright.
#
# One daemon with four collections, one of them named with S343's 32
# characters.  Its /stats is served to the page FLEET-SHAPED - each
# collection with three members' figures, 8-digit counters (so the "N here"
# lines are as wide as a busy fleet makes them) and one read-through line -
# by routing the page's own request: the shape PROD shows, without a fleet.
# At 1280, 1440 and 1920 px: the panel does not scroll sideways, the page
# body does not either, and the long name's row is drawn.
# Playwright is not on the CI runner, so the suite SKIPS loudly there rather
# than passing without running.  PW_PY names a python that has it.
# Usage: test/pagelayouttest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
PY=${PW_PY:-python3}
"$PY" -c "import playwright.sync_api" 2>/dev/null || {
	echo "pagelayouttest: SKIPPED - no Playwright for $PY (set PW_PY, or pip install playwright && python -m playwright install chromium)"
	echo "pagelayouttest: the page's layout was NOT measured here"
	exit 0
}

D=$(mktemp -d /var/tmp/pclay.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT TERM INT
LONG=abcdefghijklmnopqrstuvwxyz012345
cat > "$D/a.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = lay-client-secret
[listen]
tcp = 127.0.0.1:17866
http = 127.0.0.1:17865
plaintext = loopback
[collection 0]
buckets_log2 = 10
[collection th]
buckets_log2 = 10
[collection prod-redis]
buckets_log2 = 10
[collection $LONG]
buckets_log2 = 10
CONF
chmod 600 "$D/a.conf"
"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/a.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/a.log" || { echo "daemon did not start"; cat "$D/a.log"; exit 1; }

LONG="$LONG" "$PY" - <<'PY_EOF'
import json, os, sys
from playwright.sync_api import sync_playwright
LONG = os.environ["LONG"]
npass = nfail = 0
def ok(m):
    global npass; npass += 1; print("  ok   " + m)
def bad(m):
    global nfail; nfail += 1; print("  FAIL " + m)

def fleet_shaped(route):
    r = route.fetch()
    st = r.json()
    for k, x in enumerate(st.get("collections", [])):
        big = 87654321 - k * 1111111           # 8 digits: as wide as a busy fleet
        x["entries"] = big // 3
        x["fleet"] = {"basis": "fullest", "members": 3, "reporting": 3, "entries": big, "copies": 3 * big,
                      "expired": big, "hits": big, "misses": big, "stores": big, "removes": big // 9}
        for f in ("hits_client", "misses_client", "stores_client", "stores", "expired", "hits", "misses"):
            x[f] = big // 3
        if x["name"] == "prod-redis":
            x["upstream"] = {"host": "10.9.9.13", "port": 6379, "tracking": True, "fetches": 112498,
                             "absent": 110833, "fill_string": 1665, "fill_hash": 0, "fill_json": 0,
                             "shadow_hits": 2129, "negative_hits": 2306401, "shadow_entries": 8032,
                             "fetch_us_avg": 291, "errors": 0, "timeouts": 0,
                             # 0.6.2: write-through's row - a table by command and the
                             # last errors (its table took the page's 100% width and,
                             # indented, overflowed the panel: a scrollbar on 245)
                             "writes": "through", "mode": "balance", "role_required": "replica",
                             "upstreams": [{"addr": "10.9.9.%d:6379" % i, "role": "replica" if i != 14 else "master",
                                            "state": "serving" if i != 14 else "not used", "why": "" if i != 14 else "a master",
                                            "tracking": i != 14, "win_fetches": big // 3, "win_fetch_n": big // 3,
                                            "win_fetch_us_sum": big * 100} for i in (13, 14, 15)],
                             "write_through": {"target": "10.9.9.14:6379", "state": "ok", "json": "yes", "queued": 3,
                                 "win_forwarded": big, "win_n": big, "win_us_sum": big * 500, "win_applied": big,
                                 "win_not_applied": big // 7, "win_redis_errors": big // 9, "win_failed": big // 11,
                                 "win_refused": big // 13, "win_read_back_values": big, "win_read_back_gone": big // 5,
                                 "win_read_back_released": big // 17, "win_max_us": 123456,
                                 "by_command": [{"name": n, "passed": n == "lpush", "win_applied": big // (i + 1),
                                                 "win_not_applied": big // 7, "win_redis_errors": big // 9,
                                                 "win_failed": big // 11, "win_n": big, "win_us_sum": big * 1500}
                                                for i, n in enumerate(("json.set", "set", "expire", "del", "lpush"))],
                                 "last_errors": [{"at": 1, "age_s": 65, "kind": "redis", "command": "json.set",
                                                  "key": "k" * 63, "message": "ERR " + "new objects must be created at the root " * 4}]}}
    route.fulfill(status=200, content_type="application/json", body=json.dumps(st))

MEASURE = """() => {
  const b = document.querySelector('#cols'); const p = b.closest('.scroll'); const t = p.querySelector('table');
  const o = t.style.width; t.style.width = 'min-content'; const mc = Math.round(t.getBoundingClientRect().width); t.style.width = o;
  const names = [...b.querySelectorAll('td .nm')].map(e => e.textContent);
  const wt = b.querySelectorAll('table.wtc tr').length;
  const pr = [...b.querySelectorAll('tr')].find(r => r.querySelector('td .nm') && r.querySelector('td .nm').textContent.trim() === 'prod-redis');
  const lb = pr ? [...pr.querySelectorAll('td span')].find(e => e.textContent.trim() === 'lb') : null;
  const lbline = lb ? Math.abs(lb.getBoundingClientRect().top - pr.querySelector('td .nm').getBoundingClientRect().top) < 6 : null;
  return {sw: p.scrollWidth, cw: p.clientWidth, mc: mc,
          body: document.documentElement.scrollWidth > window.innerWidth, names: names,
          here: b.querySelectorAll('.hl').length, wt: wt, lbline: lbline};
}"""
with sync_playwright() as pw:
    br = pw.chromium.launch()
    for w in (1280, 1440, 1920):
        pg = br.new_page(viewport={"width": w, "height": 1000})
        pg.route(lambda u: u.split("?")[0].endswith("/stats"), fleet_shaped)
        pg.goto("http://127.0.0.1:17865/", wait_until="domcontentloaded")
        try:
            pg.wait_for_function("document.querySelectorAll('#cols td .nm').length >= 4", timeout=15000)
        except Exception as e:
            bad("%d px: the collections table was not drawn (%s)" % (w, str(e)[:80])); pg.close(); continue
        pg.wait_for_timeout(300)
        m = pg.evaluate(MEASURE)
        if w == 1280:
            (ok if LONG in "".join(m["names"]).replace("\n", "") and m["here"] >= 8 else bad)(
                "the fixture is drawn: the %d-character name and the fleet's 'here' lines (%d)" % (len(LONG), m["here"]))
            (ok if m["wt"] == 6 else bad)("the write-through row's table by command is drawn (%d rows of 6)" % m["wt"])
        (ok if m["sw"] <= m["cw"] else bad)(
            "%d px: the collections panel does not scroll sideways (table %d in a %d px panel; narrowest it can be %d)"
            % (w, m["sw"], m["cw"], m["mc"]))
        (ok if not m["body"] else bad)("%d px: nor does the page" % w)
        # 0.6.2: the full-width rows (read-through, its links, writes) no
        # longer size the columns - their text squeezed the name column and
        # a 10-character name's "lb" fell to a second line at 1280
        (ok if m["lbline"] else bad)("%d px: prod-redis's 'lb' sits on its name's line (%s)" % (w, m["lbline"]))
        # 0.6.2: every card on the plane holds its content - nothing past
        # its edge (listeners overflowed by 14 px; text cut on purpose,
        # with an ellipsis, does not count) - and the slow log and alerts
        # sections do not scroll sideways
        cv = pg.evaluate("""() => { const out = [];
          const clipped = (e, c) => { for (let p = e.parentElement; p && p !== c; p = p.parentElement)
            if (getComputedStyle(p).overflow === 'hidden') return true; return false; };
          for (const c of document.querySelectorAll('.plane .pc')) { const r = c.getBoundingClientRect(); let ov = 0;
            for (const e of c.querySelectorAll('*')) { const q = e.getBoundingClientRect();
              if (q.width && q.right > r.right + 1 && !clipped(e, c)) ov = Math.max(ov, Math.round(q.right - r.right)); }
            if (ov) out.push([c.querySelector('h3').textContent, ov]); }
          const sc = ['slwrap', 'alwrap'].map(i => document.getElementById(i)).filter(x => x && x.scrollWidth > x.clientWidth).map(x => x.id);
          return {cards: document.querySelectorAll('.plane .pc').length, over: out, scroll: sc}; }""")
        (ok if cv["cards"] >= 12 and not cv["over"] and not cv["scroll"] else bad)(
            "%d px: all %d cards hold their content, the slow log and alerts do not scroll (over: %s, scrolling: %s)"
            % (w, cv["cards"], cv["over"], cv["scroll"]))
        c0 = pg.evaluate("() => Math.round(document.querySelector('#cols tr:not(.up) td').getBoundingClientRect().width)")
        pg.add_style_tag(content="#cols tr.up{display:none}")
        pg.wait_for_timeout(100)
        c1 = pg.evaluate("() => Math.round(document.querySelector('#cols tr:not(.up) td').getBoundingClientRect().width)")
        (ok if abs(c0 - c1) <= 1 else bad)(
            "%d px: the full-width rows do not size the columns (name column %d px with them, %d without)" % (w, c0, c1))
        pg.close()
    br.close()
print("pagelayouttest: %d passed, %d failed" % (npass, nfail))
sys.exit(1 if nfail else 0)
PY_EOF
