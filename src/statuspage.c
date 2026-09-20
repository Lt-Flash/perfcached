/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* statuspage.c - GENERATED-SHAPED but hand-maintained: the S37/S67
 * page as one C string.  Edit it here; there is no build step and
 * no asset to fetch, which is the point (see statuspage.h).
 *
 * S67: the page was a single table of members, which left the fleet
 * legible and everything else invisible - HEADROOM rendered "-" though
 * /members carried the inputs, collections and the cluster counters had
 * nowhere to appear at all, and the only connection count on the page
 * was the RESP door's, so a fleet of native clients read as zero.
 *
 * It now reads BOTH /members and /stats (the latter added for exactly
 * this), and it stays a per-NODE view on purpose: the fleet grid is the
 * only fleet-wide thing here, because a page that averaged its peers
 * would hide the disagreement that a partition IS.  Peers are reached
 * by opening their own copy - see the node selector.
 *
 * No fonts, scripts or styles are fetched: an air-gapped fleet must get
 * the same page as a connected one.  History is sampled BY THE PAGE and
 * lives in the browser, so the daemon keeps no time series.
 */
#include "statuspage.h"
const char pc_status_page[] =
	"<!doctype html><meta charset=utf-8>\n"
	"<title>perfcached</title>\n"
	"<meta name=viewport content=\"width=device-width,initial-scale=1\">\n"
	"<style>\n"
	/* color-scheme: the palette follows prefers-color-scheme, but without
	 * it the browser draws its own parts - the table's horizontal
	 * scrollbar, the filter input - light on the dark page. */
	":root{color-scheme:light dark;--bg:#f4f6f7;--panel:#fff;--panel2:#eef1f3;--line:#d3dade;\n"
	"--ink:#12191c;--ink2:#4a585e;--ink3:#7d8b91;--accent:#0b6a72;--accents:#d6ecee;\n"
	"--ok:#1a7f4b;--oks:#dcf0e5;--okw:#f3faf6;--warn:#9a6206;--warns:#fbeed6;--warnw:#fdf7ec;\n"
	"--bad:#a32723;--bads:#f8dedd;--badw:#fdf2f1;--idle:#8c979c;--idles:#e6eaec;--bar:#c9d4d8;\n"
	"--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,\"Liberation Mono\",monospace;\n"
	"--sans:system-ui,-apple-system,Segoe UI,Roboto,\"Helvetica Neue\",sans-serif}\n"
	"@media (prefers-color-scheme:dark){:root{--bg:#0d1416;--panel:#161f23;--panel2:#1c282c;\n"
	"--line:#384b52;--ink:#e6eef0;--ink2:#9fb2b8;--ink3:#6d8189;--accent:#5ec8d0;--accents:#12343a;\n"
	"--ok:#5fd39a;--oks:#123329;--okw:#141f1d;--warn:#e0aa53;--warns:#33260f;--warnw:#221d13;\n"
	"--bad:#f0857f;--bads:#3a1a19;--badw:#251817;--idle:#74858b;--idles:#1d282c;--bar:#2c3b41}}\n"
	"*{box-sizing:border-box}\n"
	"body{margin:0;padding:0 20px 48px;background:var(--bg);color:var(--ink);\n"
	"font:14px/1.5 var(--sans);-webkit-font-smoothing:antialiased}\n"
	".wrap{max-width:1500px;margin:0 auto}\n"
	"header{display:flex;flex-wrap:wrap;gap:12px 24px;align-items:baseline;\n"
	"padding:18px 0 13px;border-bottom:1px solid var(--line);margin-bottom:18px}\n"
	".brand{font-family:var(--mono);font-weight:600;font-size:16px;letter-spacing:.14em;\n"
	"text-transform:uppercase;color:var(--accent)}\n"
	".idbits{display:flex;flex-wrap:wrap;gap:5px 18px;color:var(--ink2);font-size:13px}\n"
	".idbits b{font-family:var(--mono);font-weight:500;color:var(--ink)}\n"
	".sp{flex:1 1 auto}\n"
	".stamp{color:var(--ink3);font-size:12px;font-family:var(--mono)}\n"
	".switch{display:flex;align-items:center;gap:7px;font-size:12px;color:var(--ink3)}\n"
	".btn{font:inherit;font-size:12px;font-family:var(--mono);color:var(--ink3);background:transparent;\n"
	"border:1px solid var(--ink3);border-radius:4px;padding:2px 9px;cursor:pointer}\n"
	".btn:hover{opacity:.75}\n"
	".switch select{font:inherit;font-family:var(--mono);font-size:12px;color:var(--ink);\n"
	"background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:4px 8px;cursor:pointer}\n"
	"h2{font-size:11px;letter-spacing:.15em;text-transform:uppercase;color:var(--ink3);\n"
	"font-weight:600;margin:28px 0 10px;display:flex;align-items:baseline;gap:12px}\n"
	"h2 .hint{font-size:11px;letter-spacing:0;text-transform:none;font-weight:400;font-family:var(--sans)}\n"
	".note{color:var(--ink2);font-size:13px;margin:0 0 14px;max-width:74ch}\n"
	".note b{color:var(--ink)}\n"
	".metrics{display:grid;gap:10px;grid-template-columns:repeat(auto-fit,minmax(158px,1fr))}\n"
	".metric{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:11px 13px}\n"
	/* S160: the clients strip is clickable and says so; the chips carry
	 * the dialect split; the panel is the drop-down of connections */
	".metric.click{cursor:pointer}.metric.click:hover{border-color:var(--accent)}\n"
	".chip{display:inline-block;border:1px solid var(--line);border-radius:10px;padding:0 7px;margin:2px 3px 0 0;font-size:11px;color:var(--ink2);font-family:var(--mono)}\n"
	".clpanel{margin:10px 0 4px}.clhead{display:flex;gap:10px;align-items:center;margin-bottom:6px;font-size:12px;color:var(--ink2)}\n"
	".clhead input{font:inherit;padding:3px 7px;border:1px solid var(--line);border-radius:6px;background:var(--panel);color:var(--ink);min-width:260px}\n"
	".mono{font-family:var(--mono);font-variant-numeric:tabular-nums}\n"
	".metric .k{font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--ink3)}\n"
	".metric .v{font-family:var(--mono);font-size:22px;font-variant-numeric:tabular-nums;margin-top:3px}\n"
	".metric .s{font-size:12px;color:var(--ink2);margin-top:2px}\n"
	".legend{display:flex;gap:14px;flex-wrap:wrap;color:var(--ink3);font-size:12px;\n"
	"align-items:center;margin-bottom:11px}\n"
	".legend i{width:9px;height:9px;border-radius:50%;display:inline-block;margin-right:5px}\n"
	".fleet{display:grid;gap:9px}\n"
	".fleet.cf{grid-template-columns:repeat(auto-fill,minmax(212px,1fr))}\n"
	".fleet.cp{grid-template-columns:repeat(auto-fill,minmax(116px,1fr))}\n"
	".node{background:var(--panel);border:1px solid var(--line);border-left:3px solid var(--idle);\n"
	"border-radius:7px;padding:9px 11px;display:block;text-decoration:none;color:inherit}\n"
	"a.node:hover{border-color:var(--accent)}\n"
	"a.node:focus-visible{outline:2px solid var(--accent);outline-offset:2px}\n"
	".node.ready{border-left-color:var(--ok);background:var(--okw)}\n"
	".node.attn{border-color:var(--warn);border-left-color:var(--warn);background:var(--warnw);\n"
	"animation:caution 3.8s ease-in-out infinite}\n"
	"@keyframes caution{0%,100%{box-shadow:0 0 0 1px var(--warn),0 0 5px -3px var(--warn)}\n"
	"50%{box-shadow:0 0 0 1px var(--warn),0 0 15px -2px var(--warn)}}\n"
	".node.failed{border-color:var(--bad);border-left-color:var(--bad);background:var(--badw);\n"
	"animation:alarm 2.4s ease-in-out infinite}\n"
	"@keyframes alarm{0%,100%{box-shadow:0 0 0 1px var(--bad),0 0 9px -3px var(--bad)}\n"
	"50%{box-shadow:0 0 0 1px var(--bad),0 0 22px 1px var(--bad)}}\n"
	"@media (prefers-reduced-motion:reduce){\n"
	".node.failed{animation:none;box-shadow:0 0 0 2px var(--bad),0 0 16px -2px var(--bad)}\n"
	".node.attn{animation:none;box-shadow:0 0 0 2px var(--warn),0 0 12px -3px var(--warn)}}\n"
	".node.self{border-color:var(--accent);box-shadow:0 0 0 1px var(--accent) inset}\n"
	".node .top{display:flex;align-items:baseline;gap:7px;justify-content:space-between}\n"
	".node .id{font-family:var(--mono);font-weight:600;font-size:15px;font-variant-numeric:tabular-nums}\n"
	".chips{display:flex;gap:5px;align-items:baseline}\n"
	".role,.st{font-family:var(--mono);font-size:10px;letter-spacing:.06em;text-transform:uppercase;\n"
	"padding:1px 6px;border-radius:4px;border:1px solid var(--line);color:var(--ink2);white-space:nowrap}\n"
	".st{font-weight:600}\n"
	".st.ready{color:var(--ok);border-color:var(--ok);background:var(--oks)}\n"
	".st.attn{color:var(--warn);border-color:var(--warn);background:var(--warns)}\n"
	".st.failed{color:var(--bad);border-color:var(--bad);background:var(--bads)}\n"
	".role.master{color:var(--ok);border-color:var(--ok);background:var(--oks)}\n"
	".role.backup{color:var(--warn);border-color:var(--warn);background:var(--warns)}\n"
	".hist{display:inline-flex;align-items:flex-end;gap:1px;height:14px}.hist i{display:inline-block;width:5px;background:var(--ink2);opacity:.7}\n"
	".node .addr{font-family:var(--mono);font-size:11.5px;color:var(--ink2);margin-top:3px;\n"
	"white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
	".node .hl{display:flex;justify-content:space-between;align-items:baseline;margin-top:8px;\n"
	"font-size:11px;color:var(--ink3)}\n"
	".node .hl .pct{font-family:var(--mono);color:var(--ink);font-variant-numeric:tabular-nums}\n"
	".track{height:5px;border-radius:3px;background:var(--bar);overflow:hidden;margin-top:4px}\n"
	".track>i{display:block;height:100%;background:var(--accent)}\n"
	".track.low>i{background:var(--warn)}.track.crit>i{background:var(--bad)}\n"
	".you{font-size:10px;color:var(--accent);font-family:var(--mono);margin-top:6px;display:block}\n"
	".fleet.cp .addr,.fleet.cp .hl span:first-child,.fleet.cp .you{display:none}\n"
	".fleet.cp .node{padding:7px 8px}\n"
	".fleet.cp .role:not(.master):not(.backup){display:none}\n"
	".fleet.cp .id{font-size:13px}\n"
	".fleet.cp .role,.fleet.cp .st{font-size:9px;padding:0 4px}\n"
	".scroll{overflow-x:auto;border:1px solid var(--line);border-radius:8px;background:var(--panel)}\n"
	"table{border-collapse:collapse;width:100%;font-size:13px;min-width:620px}\n"
	"th,td{text-align:right;padding:8px 12px;border-bottom:1px solid var(--line);white-space:nowrap}\n"
	"th:first-child,td:first-child{text-align:left}\n"
	"th{font-size:10.5px;letter-spacing:.09em;text-transform:uppercase;color:var(--ink3);\n"
	"font-weight:600;background:var(--panel2)}\n"
	/* S148: "(now)" rides in the header beside the column name, in the
	 * lowercase the uppercase th would otherwise eat. */
	"th .s{text-transform:none;letter-spacing:0;font-weight:400;opacity:.8}\n"
	"tbody tr:last-child td{border-bottom:0}\n"
	"td.n,th.n{font-family:var(--mono);font-variant-numeric:tabular-nums}\n"
	"td .nm{font-family:var(--mono);font-weight:600}\n"
	"td .md{font-size:11px;color:var(--ink3);font-family:var(--mono);margin-left:7px}\n"
	".hit{display:flex;align-items:center;gap:8px;justify-content:flex-end}\n"
	".hit .track{width:78px;margin:0;flex:none}\n"
	/* S148: two figures in one cell - the window rate reads as the
	 * value, the lifetime sits under it in the subtitle size the
	 * metric cards already use for "since start".  S163's fleet cells
	 * (fv()) use the same pair in a bare td with no .hit around it, so
	 * the rule must reach a td too - scoped to .hit alone, the total and
	 * "here" ran together as one number ("6,1292,043 here"). */
	".hit .hv,td .hv{display:flex;flex-direction:column;align-items:flex-end;line-height:1.2}\n"
	".hit .hl,td .hl{font-size:11px;color:var(--ink3);font-family:var(--mono);\n"
	"font-variant-numeric:tabular-nums;white-space:nowrap}\n"
	".dim{color:var(--ink3)}\n"
	".plane{display:grid;gap:9px;grid-template-columns:repeat(auto-fit,minmax(184px,1fr))}\n"
	".pc{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:10px 12px}\n"
	".pc h3{margin:0 0 7px;font-size:10.5px;letter-spacing:.09em;text-transform:uppercase;\n"
	"color:var(--ink3);font-weight:600}\n"
	".kv{display:flex;justify-content:space-between;gap:12px;padding:2px 0;font-size:12.5px}\n"
	".kv span:first-child{color:var(--ink2)}\n"	/* S165: an unknown-commands row stays on ONE line in the narrowest
	 * card - a long name is cut with an ellipsis (the whole of it, the
	 * dialect and the rest are in the hover) and the value never wraps.
	 * The slow log and slowest-calls cards clip the same way: a key has
	 * no space to wrap at, and it pushed the time off the card. */
	".pc.unk .kv>span:first-child,.pc.clip .kv>span:first-child{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
	".pc.unk .kv>span:last-child,.pc.clip .kv>span:last-child{white-space:nowrap;font-variant-numeric:tabular-nums}\n"

	".kv span:last-child{font-family:var(--mono);font-variant-numeric:tabular-nums}\n"
	".kv.w span:last-child{color:var(--warn)}\n"
	/* S131: the same warn colour on a table cell.  .w existed only
	 * for the key/value cards, so a flagged td rendered as an
	 * ordinary one - a flag nobody can see is not a flag. */
	"td.w{color:var(--warn)}\n"
	"td.b{color:var(--bad);font-weight:600}\n"
	".trend{display:grid;gap:9px;grid-template-columns:repeat(auto-fit,minmax(232px,1fr))}\n"
	".tc{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:11px 13px}\n"
	".tc .th{display:flex;justify-content:space-between;align-items:baseline;gap:10px}\n"
	".tc .k{font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--ink3)}\n"
	".tc .now{font-family:var(--mono);font-size:17px;font-variant-numeric:tabular-nums}\n"
	".tc svg{display:block;width:100%;height:80px;margin-top:7px}\n"
	".tc .rg{display:flex;justify-content:space-between;font-size:11px;color:var(--ink3);\n"
	"font-family:var(--mono);margin-top:3px}\n"
	".sl{fill:none;stroke:var(--accent);stroke-width:1.6;vector-effect:non-scaling-stroke}\n"
	".sf{fill:var(--accent);opacity:.13}\n"
	".sg{stroke:var(--line);stroke-width:1;stroke-dasharray:2 4;\n"
	"  vector-effect:non-scaling-stroke;opacity:.9}\n"
	".tc .pl{display:grid;grid-template-columns:54px 1fr;gap:6px;margin-top:7px}\n"
	".tc .pl svg{margin-top:0}\n"
	".tc .yl{position:relative;height:80px;font-size:10px;color:var(--ink3);font-family:var(--mono);font-variant-numeric:tabular-nums}\n"
	".tc .yl span{position:absolute;right:0;transform:translateY(-50%);white-space:nowrap}\n"
	".tc .pw{position:relative}\n"
	".tc .pk{position:absolute;width:7px;height:7px;border-radius:50%;background:var(--accent);transform:translate(-50%,-50%);pointer-events:none}\n"
	".tc .cur{position:absolute;top:0;bottom:0;width:1px;background:var(--ink3);opacity:.8;pointer-events:none}\n"
	".tc .tip{position:absolute;top:4px;font-size:10px;font-family:var(--mono);background:var(--panel);border:1px solid var(--line);\n"
	"  border-radius:4px;padding:1px 5px;transform:translateX(-50%);white-space:nowrap;pointer-events:none}\n"
	".tc .tip.l{transform:none}.tc .tip.r{transform:translateX(-100%)}\n"
	".tc .xl{display:flex;justify-content:space-between;font-size:10px;font-family:var(--mono);color:var(--ink3);margin-left:60px;margin-top:3px}\n"
	".tc .xt{position:relative;height:5px;margin-left:60px}\n"
	".tc .xt i{position:absolute;top:0;width:1px;height:5px;background:var(--line)}\n"
	".err{color:var(--bad)}\n"
	"footer{margin-top:32px;padding-top:13px;border-top:1px solid var(--line);\n"
	"color:var(--ink3);font-size:12px;display:flex;flex-wrap:wrap;gap:6px 18px}\n"
	"code{font-family:var(--mono);font-size:12px;background:var(--panel2);padding:1px 5px;\n"
	"border-radius:4px;border:1px solid var(--line)}\n"
	"</style>\n"
	"<div class=wrap>\n"
	"<header>\n"
	"<div class=brand>perfcached</div>\n"
	"<div class=idbits id=idbits></div>\n"
	"<div class=sp></div>\n"
	"<div class=switch><label for=nodesel>viewing</label><select id=nodesel></select></div>\n"
	"<button id=rst class=btn title=\"start this node's running totals again; open connections, entries and memory are untouched\">reset stats</button>\n"
	"<div class=stamp id=age></div>\n"
	"</header>\n"
	"<div id=alert></div>\n"
	"<h2>this node</h2>\n"
	"<div class=metrics id=metrics></div>\n"
	/* S160: this node's connections, opened from the clients strip.
	 * Read-only by decision: CLIENT KILL on the RESP door is the
	 * auditable, secret-gated way; the page never grows an admin API. */
	"<div class=clpanel id=clpanel hidden><div class=clhead><input id=clfilter placeholder=\"filter by address or name\">\n"
	"<span id=clhint></span><span class=dim>this node only &middot; addresses and names are reconnaissance material</span></div>\n"
	"<div class=scroll><table><thead><tr><th class=n>id</th><th>door</th><th>dialect</th><th>wire</th><th>address</th><th>name</th>\n"
	"<th title=\"connected for; hover for the absolute time\">connected</th><th class=n>idle</th><th class=n>commands</th><th>last</th>\n"
	"<th class=n title=\"bytes staged for the socket - a connection is closed when this reaches the output cap, so this column predicts it\">pending</th>\n"
	"<th class=n>subs</th></tr></thead><tbody id=clrows></tbody></table></div></div>\n"
	"<h2>recent history <span class=hint id=histhint></span></h2>\n"
	"<p class=note>A rolling window sampled by this page, so &quot;is this normal?&quot; can be\n"
	"answered without leaving it. <b>It starts empty on every load and covers this node\n"
	"only</b> &mdash; anything longer, across nodes, or with alerting belongs in Prometheus,\n"
	"which <code>/metrics</code> already serves.</p>\n"
	"<div class=trend id=trend></div>\n"
	"<h2>fleet <span class=hint>as this node sees it</span></h2>\n"
	"<p class=note>Every node serves its own view. <b>If two nodes disagree about the\n"
	"membership, that disagreement is a partition and this is where you see it.</b>\n"
	"Anything not ready is pinned first, then the master, then the rest by id.</p>\n"
	"<div class=legend>\n"
	"<span><i style=\"background:var(--ok)\"></i>ready</span>\n"
	"<span><i style=\"background:var(--warn)\"></i>starting / recovering / draining</span>\n"
	"<span><i style=\"background:var(--bad)\"></i>failed</span>\n"
	"<span><i style=\"background:var(--idle)\"></i>unknown</span>\n"
	"<span class=dim id=density></span></div>\n"
	"<div id=fleet class=\"fleet cf\"></div>\n"
	"<h2>collections <span class=hint id=colmode></span></h2>\n"
	"<div class=scroll><table><thead><tr><th>collection</th><th class=n>entries</th>\n"
	"<th class=n>buckets</th><th class=n>load</th>\n"
	/* S131: the leg is where a table that has stopped growing puts
	 * everything, on a chain per hash bucket under one lock.  Load
	 * alone does not say it, and the leg was reported nowhere at
	 * all until now. */
	"<th class=n title=\"records in the overflow leg rather than in a bucket's slots: near zero while a table keeps up with its load factor, everything once it stops growing\">in leg</th>\n"
	/* S148: the column and the headline card both said "hit rate" and
	 * meant different things.  Both are windowed now; the title says
	 * what the window is and what the figure under it means. */
	"<th title=\"share of client lookups that found the key, over the last poll window (~3 s) - the same window the &quot;hit rate (now)&quot; card uses.  &quot;life&quot; underneath is the ratio since start or last reset: it is a cumulative mean, so it drifts toward the present rate and can never reach it.  Both count LOOKUPS, not keys, so one hot key can carry the figure - read it beside entries, not alone.  Client lookups only (S151): peer pulls and recovery probes are excluded.\">hit rate <span class=s>(now)</span></th>\n"
	/* S148 coverage: the rate counts LOOKUPS, so one hot key hammered
	 * hard reads as a perfect cache beside thousands of records nobody
	 * wants.  Distinct keys SERVED in the window, against entries held,
	 * is the half the ratio cannot express - and the pair is the point:
	 * a high rate with a tiny reach is a hot key, not a healthy cache. */
	"<th title=\"distinct keys actually SERVED from this collection in the last closed window, against the entries it holds.  The hit rate counts LOOKUPS, so one key pulled 100,000 times reads as ~100% beside thousands of records nobody wants; reach is what separates the two.  Approximate on purpose - a 64-register sketch, roughly 13% - and it counts what was FOUND, so a collection whose lookups mostly and correctly miss shows a small reach.\">reach <span class=s>(now)</span></th><th class=n title=\"client lookups only (S151): a peer serving a pull and recovery probing its replay count in /stats hits, not here\">hits</th>\n"
	"<th class=n title=\"client lookups only (S151), as for hits\">misses</th><th class=n>stores</th><th class=n>expired</th>\n"
	"<th class=n title=\"key and value bytes of the records held: exact when taken, from a walk the maintenance thread repeats every few seconds\">held</th>\n"
	"<th title=\"value sizes of the records held, log2 classes from 64 B to 256 KB+: from the same walk as held, so the same on every member\">sizes</th></tr></thead>\n"
	"<tbody id=cols></tbody></table></div>\n"
	/* S160, widened 2026-09-20: the slowest calls were a card in the
	 * plane grid, where the figures took the row's width and the command
	 * name - the left span - was squeezed to nothing, so every row read
	 * as numbers with no verb.  A table of its own, the collections
	 * shape: it scrolls sideways inside its container on a phone, and
	 * the name is a column that cannot collapse. */
	"<h2>commands <span class=hint>slowest calls first - the mean is what it usually costs, the tail is what a client waits for</span></h2>\n"
	"<div class=scroll><table><thead><tr><th>command</th><th class=n>calls</th>\n"
	"<th class=n title=\"total time in the verb divided by its calls: what it usually costs, and blind to a single slow call\">mean</th>\n"
	"<th class=n>p50</th><th class=n>p99</th><th class=n>p99.9</th>\n"
	"<th class=n title=\"the slowest single call, from the log2 latency histogram (S159): the bucket it fell in, so &quot;&#8804; 16.4 ms&quot; means one call took between 8.2 and 16.4 ms.  The percentiles are bucket bounds the same way.\">slowest call</th></tr></thead>\n"
	"<tbody id=cmds></tbody></table></div>\n"
	"<h2>cluster plane</h2>\n"
	"<div class=plane id=plane></div>\n"
	"<footer><span>read-only &mdash; no data can be changed here</span>\n"
	"<span><code>/stats</code> json</span><span><code>/members</code> json</span><span><code>/clients</code> json</span>\n"
	"<span><code>/metrics</code> prometheus</span><span><code>/health</code> liveness</span></footer>\n"
	"</div>\n"
	"<script>\n"
	/* S91: POLL is the sampling cadence AND the tick interval - one
	 * constant, so the axis label can never disagree with the data.
	 *
	 * S130: HMAX was 1,200, an HOUR of samples at that cadence, and the
	 * operator reads these cards for the last few minutes.  An hour
	 * squeezed into a card a few hundred pixels wide draws every spike
	 * two pixels from its neighbour, so a spike and a plateau look
	 * alike.  100 samples is 5m 0s in the header and 4m 57s of span
	 * across the plot, which also keeps xlab() under its 900 s branch,
	 * so the minute ticks are drawn - at an hour they were not. */
	"var HP=(location.port||\"8080\"),last=0,H={},HMAX=100,POLL=3000,P=null;\n"
	/* S148: the per-collection column was the LIFETIME ratio while the
	 * headline beside it was windowed - two numbers, the same words.
	 * PC holds the previous sample per collection NAME so the column
	 * can be differenced exactly as P differences the totals.  PRA is
	 * the reset_at both baselines were taken against: reset-stats
	 * rebases the counters DOWNWARD (pcache_ht_totals subtracts
	 * ht->base), so without this a reset reads as a negative delta. */
	"var PC={},PRA=null;\n"
	/* S160: the last members list (for the fleet-wide clients figure),
	 * whether the connections panel is open, and its last fetch */
	"var MEM=null,CLOPEN=false,CLD=null;\n"
	"function esc(s){return String(s).replace(/[&<>\"]/g,function(c){\n"
	"return {\"&\":\"&amp;\",\"<\":\"&lt;\",\">\":\"&gt;\",\"\\\"\":\"&quot;\"}[c];});}\n"
	/* numbers are grouped the SI way - a narrow no-break space between
	 * groups of three, from five digits up - and never by the browser's
	 * locale: toLocaleString wrote 10134 as "10,134" here and "10.134" in
	 * a German browser, and either reads as ten point something */
	"function nf(v){if(v===undefined||v===null)return \"-\";var n=Number(v),p;if(!isFinite(n))return String(v);\n"
	"p=String(Math.round(Math.abs(n)*1000)/1000).split('.');if(p[0].length>4)p[0]=p[0].replace(/\\B(?=(\\d{3})+$)/g,'\\u202f');\n"
	"return (n<0?'-':'')+p[0]+(p[1]?'.'+p[1]:'');}\n"
	/* S151: the page reads the CLIENT figures when the daemon publishes
	 * them, and the all-origin ones from an older daemon.  Same shape,
	 * so nothing below has to know which it got. */
	"function ch(x){return x.hits_client!==undefined?x.hits_client:(x.hits||0);}\n"
	"function cm(x){return x.misses_client!==undefined?x.misses_client:(x.misses||0);}\n"
	"function cr(x){return x.reach_client!==undefined?x.reach_client:x.reach;}\n"
	"function mb(b){return Math.round((b||0)/1048576);}\n"
	/* a fixed MB scale renders every small arena as a flat 0 MB - two
	 * records held 480 bytes and the page reported nothing at all */
	"function sz(b){if(b===undefined||b===null)return \"\\u2014\";b=+b;\n"
	"if(b<1024)return b+\" B\";\n"
	"if(b<1048576)return (b/1024).toFixed(b<10240?1:0)+\" KB\";\n"
	"if(b<1073741824)return (b/1048576).toFixed(b<10485760?1:0)+\" MB\";\n"
	"return (b/1073741824).toFixed(2)+\" GB\";}\n"
	"function dur(t){if(t===undefined||t===null)return \"\\u2014\";t=+t;\n"
	"var d=Math.floor(t/86400),h=Math.floor(t%86400/3600),m=Math.floor(t%3600/60);\n"
	"if(d)return d+\"d \"+h+\"h\";if(h)return h+\"h \"+m+\"m\";\n"
	"if(m)return m+\"m \"+(t%60)+\"s\";return t+\"s\";}\n"
	"function get(u,cb){var x=new XMLHttpRequest();x.open(\"GET\",u,true);\n"
	"x.onreadystatechange=function(){if(x.readyState!==4)return;\n"
	"if(x.status===200){try{cb(JSON.parse(x.responseText));}catch(e){cb(null);}}else cb(null);};\n"
	"x.send();}\n"
	"function post(u,cb){var x=new XMLHttpRequest();x.open(\"POST\",u,true);\n"
	"x.onreadystatechange=function(){if(x.readyState!==4)return;cb(x.status===200);};x.send();}\n"
	"function push(k,v){if(v===undefined||v===null)return;\n"
	"if(!H[k])H[k]=[];H[k].push(v);if(H[k].length>HMAX)H[k].shift();}\n"
	/* S115: a spike's height and duration read off the card.  Y labels
	 * sit in an HTML column beside the stretched svg (text inside it
	 * would distort), one per gridline; percentages and counts are
	 * anchored at zero so a small value looks small, bytes keep min..max
	 * and the labels say so.  Three time ticks under the plot and a mark
	 * per minute while the window is under fifteen; a peak dot with its
	 * time, and a pointer readout (cursor + value + time) driven by the
	 * listener at the end of the script. */
	"function spark(a,zero){var w=240,h=80,n=a.length,i,lo,hi,p=[],x,y,g=\"\";\n"
	"if(n<2)return '<svg viewBox=\"0 0 240 80\"></svg>';\n"
	"lo=zero?0:Math.min.apply(null,a);hi=Math.max.apply(null,a);if(hi<=lo)hi=lo+1;\n"
	"for(i=0;i<n;i++){x=(i/(n-1))*w;y=h-((a[i]-lo)/(hi-lo))*(h-4)-2;\n"
	"p.push((i?\"L\":\"M\")+x.toFixed(1)+\" \"+y.toFixed(1));}\n"
	"var d=p.join(\" \");\n"
	"for(i=1;i<4;i++){y=(2+((h-4)*i/4)).toFixed(1);\n"
	"g+='<line class=sg x1=\"0\" x2=\"'+w+'\" y1=\"'+y+'\" y2=\"'+y+'\"/>';}\n"
	"return '<svg viewBox=\"0 0 '+w+' '+h+'\" preserveAspectRatio=none aria-hidden=true>'+g+\n"
	"'<path class=sf d=\"M0 '+h+' L'+d.slice(1)+' L'+w+' '+h+' Z\"/><path class=sl d=\"'+d+'\"/></svg>';}\n"
	"function ylab(lo,hi,fm){var o='<div class=yl>',i;\n"
	"for(i=0;i<5;i++)o+='<span style=\"top:'+(2+76*i/4).toFixed(0)+'px\">'+fm(hi-(hi-lo)*i/4)+'</span>';\n"
	"return o+'</div>';}\n"
	"function ago(s){return s?'-'+dur(s):'now';}\n"
	"function xlab(n){var span=(n-1)*POLL/1000,i,t,o='<div class=xl><span>'+ago(span)+\n"
	"'</span><span>'+ago(Math.round(span/2))+'</span><span>now</span></div>';\n"
	"if(span<900){o+='<div class=xt>';for(i=0;i<n;i++){t=(n-1-i)*POLL/1000;\n"
	"if(t&&t%60===0)o+='<i style=\"left:'+(100*i/(n-1)).toFixed(2)+'%\"></i>';}o+='</div>';}\n"
	"return o;}\n"
	"var HV={};\n"
	"function trend(){\n"
	"var defs=[[\"daemon cpu\",\"cpu\",\"%\",1,0,1],[\"arena live\",\"live\",\"\",0,1,0],\n"
	"[\"entries\",\"ent\",\"\",0,0,1],[\"hit rate\",\"hr\",\"%\",1,0,1]],o=\"\",i;\n"
	"for(i=0;i<defs.length;i++){var d=defs[i],a=H[d[1]]||[],k=d[1];\n"
	"if(!a.length){o+='<div class=tc><div class=th><span class=k>'+d[0]+\n"
	"'</span><span class=\"now dim\">-</span></div><div class=\"rg dim\"><span>collecting…</span></div></div>';continue;}\n"
	"var n=a.length,cur=a[n-1],lo=Math.min.apply(null,a),hi=Math.max.apply(null,a),f=d[3],\n"
	"fm=d[4]?sz:function(v){return v.toFixed(f)+d[2];},pi=a.indexOf(hi),px=n>1?100*pi/(n-1):0,\n"
	"band=d[5]?0:lo,top=hi<=band?band+1:hi,hv=HV[k],ex='';\n"
	"if(n>1){ex+='<i class=pk style=\"left:'+px.toFixed(2)+'%;top:2px\" title=\"max '+fm(hi)+' '+ago((n-1-pi)*POLL/1000)+'\"></i>';\n"
	"if(hv!==undefined&&hv<n){var hx=100*hv/(n-1);ex+='<i class=cur style=\"left:'+hx.toFixed(2)+'%\"></i>'+\n"
	"'<b class=\"tip'+(hx<15?' l':hx>85?' r':'')+'\" style=\"left:'+hx.toFixed(2)+'%\">'+fm(a[hv])+' \\u00b7 '+ago((n-1-hv)*POLL/1000)+'</b>';}}\n"
	"o+='<div class=tc data-k=\"'+k+'\"><div class=th><span class=k>'+d[0]+'</span><span class=now>'+\n"
	"fm(cur)+'</span></div><div class=pl>'+(n>1?ylab(band,top,fm):'<div class=yl></div>')+\n"
	"'<div class=pw data-k=\"'+k+'\">'+spark(a,d[5])+ex+'</div></div>'+(n>1?xlab(n):'')+\n"
	"'<div class=rg><span>min '+fm(lo)+'</span><span>'+(n>1?dur((n-1)*POLL/1000)+' ago \\u2192 now':'first sample')+\n"
	"'</span><span>max '+fm(hi)+'</span></div></div>';}\n"
	"document.getElementById(\"trend\").innerHTML=o;\n"
	"document.getElementById(\"histhint\").textContent=\n"
	"  'since this page was opened \\u00b7 '+(POLL/1000)+' s samples \\u00b7 keeps '+\n"
	"  dur(HMAX*POLL/1000)+' \\u2014 not a metrics store';}\n"
	"var RANK={failed:0,draining:1,recovering:1,starting:1,ready:3};\n"
	"function drawFleet(m){MEM=m;\n"
	"var i,sel=document.getElementById(\"nodesel\"),o=\"\";\n"
	"m=m.slice().sort(function(a,b){\n"
	"var ra=RANK[a.state]===undefined?2:RANK[a.state],rb=RANK[b.state]===undefined?2:RANK[b.state];\n"
	"if(ra!==rb)return ra-rb;\n"
	"if(!!a.master!==!!b.master)return a.master?-1:1;\n"
	"if(!!a.backup!==!!b.backup)return a.backup?-1:1;\n"
	"return a.node-b.node;});\n"
	"var cp=m.length>24,el=document.getElementById(\"fleet\");\n"
	"el.className=\"fleet \"+(cp?\"cp\":\"cf\");\n"
	"document.getElementById(\"density\").textContent=cp?\"compact tiles (>24 nodes)\":\"\";\n"
	"sel.innerHTML=m.slice().sort(function(a,b){return a.node-b.node;}).map(function(d){\n"
	"return '<option value=\"http://'+esc(d.addr)+':'+(d.http||HP)+'/\"'+(d.self?\" selected\":\"\")+\">node \"+\n"
	"esc(d.node)+\" · \"+esc(d.addr)+(d.self?\" (this node)\":\"\")+\" · \"+esc(d.state||\"?\")+\n"
	"\"</option>\";}).join(\"\");\n"
	"sel.onchange=function(){if(this.value)location.href=this.value;};\n"
	"for(i=0;i<m.length;i++){var d=m[i],st=d.state||\"unknown\",\n"
	"attn=(st!==\"ready\"&&st!==\"failed\")?\" attn\":\"\",\n"
	"role=d.master?\"master\":d.backup?\"backup\":\"member\",\n"
	"hp=(d.total_mb?Math.round(100*d.free_mb/d.total_mb):null),\n"
	"tk=hp===null?\"\":(hp<15?\"crit\":hp<35?\"low\":\"\"),\n"
	"u=\"http://\"+esc(d.addr)+\":\"+(d.http||HP)+\"/\";\n"
	"o+=(d.self?\"<div\":'<a href=\"'+u+'\"')+' class=\"node '+esc(st)+attn+(d.self?\" self\":\"\")+\n"
	"'\" title=\"node '+esc(d.node)+\" — \"+esc(d.addr)+\":\"+esc(d.port)+\" — \"+esc(st)+'\">'+\n"
	"'<div class=top><span class=id>'+esc(d.node)+'</span><span class=chips>'+\n"
	"'<span class=\"st '+(st===\"ready\"?\"ready\":st===\"failed\"?\"failed\":\"attn\")+'\">'+esc(st)+\n"
	"'</span><span class=\"role '+role+'\">'+role+'</span></span></div>'+\n"
	"'<div class=addr>'+esc(d.addr)+'</div>'+\n"
	"'<div class=addr>up '+(d.uptime_s>=0?dur(d.uptime_s):\"\\u2014\")+'</div>'+\n"
	"'<div class=hl><span>headroom</span><span class=pct>'+(hp===null?\"-\":hp+\"%\")+'</span></div>'+\n"
	"'<div class=\"track '+tk+'\"><i style=\"width:'+(hp===null?0:hp)+'%\"></i></div>'+\n"
	"(d.self?'<span class=you>this node</span></div>':\"</a>\");}\n"
	"el.innerHTML=o;}\n"
	"function drawStats(s){\n"
	"var c=s.cluster||{},m=s.memory||{},r=s.resp||{},pr=s.process||{},nt=s.native||{},\n"
	/* S123: the door cards lead with what is open NOW; the totals say
	 * what they count from - the start, or the last reset */
	"sn=s.since||{},sl=sn.reset_at?\"since reset\":\"since start\",\n"
	"cl=s.collections||[],i,ent=0,hits=0,mis=0;\n"
	/* reset-stats rebases the counters DOWNWARD, so a stale baseline
	 * reads as a negative delta; drop both when reset_at moves. */
	"if(sn.reset_at!==PRA){P=null;PC={};PRA=sn.reset_at;}\n"
	"for(i=0;i<cl.length;i++){ent+=cl[i].entries||0;hits+=ch(cl[i]);mis+=cm(cl[i]);}\n"
	/* hits/misses and cpu are CUMULATIVE counters.  Plotting the
	 * lifetime ratio draws a running average that converges by
	 * construction: after an hour a burst of misses cannot move it,
	 * so the line flattens exactly as it stops being able to report
	 * anything.  Both are differenced against the previous sample,
	 * which makes them rates over the poll window. */
	"var hrl=(hits+mis)?100*hits/(hits+mis):null;\n"
	"var cms=(pr.cpu_user_ms||0)+(pr.cpu_sys_ms||0),now=Date.now(),hr=null,cpu=null;\n"
	"if(P){var dh=hits-P.h,dm=mis-P.m,dt=now-P.t;\n"
	"if(dh+dm>0)hr=100*dh/(dh+dm);\n"
	"if(dt>0&&pr.cpu_user_ms!==undefined){cpu=100*(cms-P.c)/dt;if(cpu<0)cpu=0;}}\n"
	"P={h:hits,m:mis,c:cms,t:now};\n"
	"push(\"live\",m.arena_live||0);push(\"ent\",ent);\n"
	"if(hr!==null)push(\"hr\",hr);if(cpu!==null)push(\"cpu\",cpu);\n"
	"document.getElementById(\"idbits\").innerHTML=\n"
	"\"<span>node <b>\"+esc(c.node===undefined?\"-\":c.node)+\"</b></span>\"+\n"
	/* WHICH MACHINE.  The node id is a per-fleet handle and is reused
	 * when a member leaves; the identity is what says this is the same
	 * machine as yesterday, and its UUID version says where it came
	 * from - 8 derived from the platform, 7 randomly minted.  An
	 * operator staring at two nodes claiming one id needs this, and it
	 * was nowhere on the page. */
	"(c.identity_uuid?\"<span title='\"+(c.identity_ver===8?\"derived from the platform - a clone of this machine gets a different one\":c.identity_ver===7?\"randomly minted - a CLONE OF THIS DISK WOULD CARRY IT\":\"unknown provenance\")+\"'>identity <b>\"+esc(c.identity_uuid.slice(0,8))+\"</b><span class=s> v\"+esc(c.identity_ver)+\"</span></span>\":\"\")+\n"
	"\"<span>role <b>\"+esc(c.role||\"standalone\")+\"</b></span>\"+\n"
	"\"<span>\"+esc(s.version||\"\")+\" <b>\"+esc(s.rev||\"\")+\"</b></span>\"+\n"
	"\"<span>state <b>\"+esc(s.state||\"?\")+\"</b></span>\";\n"
	/* S93: the FLEET's view, not this node's.  "peers up 2" on a healthy
	 * three-node cluster reads as though a node were missing - the
	 * reader has to know the count excludes self before it means
	 * anything.  What an operator actually wants is how many members are
	 * up out of how many the cluster has.  Self is up by construction
	 * (it just served this page), so members-up is peers_up + 1, and the
	 * denominator is the published map's member count.  A peer that is
	 * up but not yet IN that map would read 3/2, which is nonsense, so
	 * the total is clamped up to the live count; the map seq underneath
	 * dates the denominator for anyone who needs to see it lag. */
	"var mup=(c.peers_up||0)+1,mtot=(c.map&&c.map.nodes)||0;\n"
	"var hasmap=!!(c.map&&c.map.valid&&c.map.nodes);\n"
	"if(mtot<mup)mtot=mup;\n"
	/* S160: the FRACTION form - this node over the fleet, the denominator
	 * summed from the map each member gossips its count into, so at most
	 * one heartbeat stale and never a fetch from another node; absent on
	 * a standalone daemon rather than "123 / 123".  "fleet-wide" stays
	 * on the label so the figure cannot be read as a limit. */
	"var clo=s.clients||{},clfl=null,clrep=0,cltot=0,k;\n"
	"if(MEM&&MEM.length>1){for(k=0;k<MEM.length;k++){cltot++;if(MEM[k].clients){clrep++;clfl=(clfl||0)+(MEM[k].clients.open||0);}}}\n"
	"var clch='<span class=chip>binary '+nf(nt.binary?nt.binary.open:0)+'</span><span class=chip>json '+nf(nt.json?nt.json.open:0)+\n"
	"'</span><span class=chip>resp '+nf((r.open||0)+(nt.resp?nt.resp.open||0:0))+'</span>'+(clo.max?'<span class=chip>max '+nf(clo.max)+'</span>':'');\n"
	"document.getElementById(\"metrics\").innerHTML=\n"
	"'<div class=metric><div class=k>entries</div><div class=v>'+nf(ent)+\n"
	"'</div><div class=s>across '+cl.length+' collection'+(cl.length===1?\"\":\"s\")+'</div></div>'+\n"
	"'<div class=metric><div class=k>hit rate (now)</div><div class=v>'+\n"
	"(hr===null?\"\\u2014\":hr.toFixed(1)+\"%\")+'</div><div class=s>'+\n"
	"(hrl===null?'no reads yet':'since start '+hrl.toFixed(1)+'% \\u00b7 all collections')+\n"
	"'</div></div>'+\n"
	"'<div class=metric><div class=k>daemon cpu</div><div class=v>'+\n"
	"(cpu===null?\"\\u2014\":cpu.toFixed(1)+\"%\")+'</div><div class=s>'+\n"
	"(pr.threads?nf(pr.threads)+' threads \\u00b7 ':'')+(cms/1000).toFixed(0)+'s total</div></div>'+\n"
	"'<div class=metric><div class=k>resident memory</div><div class=v>'+sz(pr.rss_bytes)+\n"
	"'</div><div class=s>kernel rss \\u00b7 live '+sz(m.arena_live)+'</div></div>'+\n"
	/* S98: at the ceiling is a state with a clock, and the card says so */
	"'<div class=metric><div class=k>arena headroom</div><div class=v'+(m.at_ceiling?' style=\"color:var(--bad)\"':'')+'>'+\n"
	"(m.headroom_pct===undefined?\"\\u2014\":m.headroom_pct+\"%\")+'</div><div class=s>'+\n"
	"(m.at_ceiling?'AT CEILING for '+dur(Math.max(0,Math.floor(Date.now()/1000)-m.at_ceiling_since))+' \\u00b7 ':'')+\n"
	"sz(m.arena_held)+' held of '+sz(m.arena_max)+' ceiling</div></div>'+\n"
	"'<div class=metric><div class=k>uptime</div><div class=v>'+dur(pr.uptime_s)+\n"
	"'</div><div class=s>'+(pr.pid?'pid '+nf(pr.pid):'')+'</div></div>'+\n"
	"'<div class=\"metric click\" id=clmetric title=\"click for this node&#39;s connections\"><div class=k>clients</div><div class=v>'+nf(clo.open)+\n"
	"(clfl!==null?' <span style=\"font-size:14px;color:var(--ink2)\">/ '+nf(clfl)+' fleet-wide'+(clrep<cltot?' ('+clrep+' of '+cltot+' reporting)':'')+'</span>':'')+\n"
	"'</div><div class=s>'+clch+'</div></div>'+\n"
	"'<div class=metric><div class=k>members up</div><div class=v'+\n"
	"(mup<mtot?' style=\"color:var(--bad)\"':'')+'>'+\n"
	"(hasmap?nf(mup)+'/'+nf(mtot):nf(mup))+\n"
	/* S157: the copy factor the fleet cannot honour, in the colour a node
	 * down gets - the reader's question is "are my copies there" */
	"(c.replicas_short>0?' <span style=\"color:var(--bad)\">'+c.replicas_short+' cop'+(c.replicas_short===1?'y':'ies')+' short</span>':'')+\n"
	"'</div><div class=s>'+(hasmap?\"map seq \"+nf(c.map.seq)+\", term \"+nf(c.term):\n"
	"(c.map?\"no cluster map yet\":\"unclustered\"))+'</div></div>'+\n"
	"'<div class=metric><div class=k>memory tier</div><div class=v style=\"font-size:15px\">'+\n"
	"esc((m.tier||\"-\").split(\" \")[0])+'</div><div class=s>'+esc(m.tier||\"\")+'</div></div>';\n"
	/* every collection in a cluster runs the mode the config digest
	 * agreed on, so printing it against each row repeats one fact N
	 * times.  It moves to the heading unless a row disagrees, which
	 * is worth seeing precisely because it should not happen. */
	/* S163: the FLEET's figures per collection first, this node's share
	 * beside them - S93's frame rule applied to the rows.  S164: the daemon
	 * computes them (/stats rows carry `fleet`), because summing the map
	 * here counted a held record once per member holding it: 2,043 eager
	 * entries on each of three nodes read 6,129 fleet-wide.  The page shows
	 * the figure and says which rule made it; a row without `fleet` (a
	 * lone member, or a build before S164) shows this node alone. */
	"var FCn=null,FCt=0,fk;\n"
	"for(fk=0;fk<cl.length;fk++){var fq=cl[fk].fleet;if(fq&&fq.members>1){FCt=Math.max(FCt,fq.members);FCn=FCn===null?fq.reporting:Math.min(FCn,fq.reporting);}}\n"
	"function fv(tot,here,lb){return '<span class=hv><span class=n>'+(lb?'\\u2265 ':'')+nf(tot)+'</span><span class=hl>'+nf(here)+' here</span></span>';}\n"
	"var FB={fullest:'eager: every member holds every entry, so the fleet holds what its fullest member holds',at_least:'store: a pulled entry is kept where it was read, so the fleet holds at least what its fullest member holds',per_k:'spread: K copies of every entry, so the copies over K',sum:'one copy of every entry, on one member'};\n"
	"var o=\"\",j,um=cl.length?cl[0].mode:null,uni=true;\n"
	"for(j=0;j<cl.length;j++)if(cl[j].mode!==um)uni=false;\n"
	"document.getElementById(\"colmode\").textContent=\n"
	"(cl.length?(uni?\"all in \"+um+\" mode\":\"MIXED MODES\"):\"\")+(FCn!==null?' \\u00b7 fleet figures first, this node beside ('+FCn+' of '+FCt+' members reporting)':'');\n"
	/* S148: rt is now the WINDOW rate - the same poll-to-poll delta the
	 * headline card uses - and lf is the lifetime ratio that used to be
	 * the only figure here.  The two disagreed hard on live data:
	 * collection `0` on 245 read 63.8% lifetime while the last 20 s
	 * were 6.4%, and a cumulative mean can never come back, so the
	 * only symptom was "the rate is falling slowly on every
	 * collection".  A window with no reads in it is NOT 0% - it is no
	 * sample, and saying 0 would invent a fault; wn carries that
	 * apart.  The colour band follows the window, since that is what
	 * an operator is being asked to react to. */
	"var NC={};\n"
	"for(j=0;j<cl.length;j++){var x=cl[j],t=ch(x)+cm(x),\n"
	"lf=t?100*ch(x)/t:null,pv=PC[x.name],rt=null,wn=-1,\n"
	/* S180: the LOAD FACTOR is entries against buckets x SLOTS.  It
	 * divided by buckets alone, so a table at half its capacity read
	 * "300.00%" - the operator saw it and asked for thresholds on a
	 * number that was six times too large.  Amber at 70%, red at 90%:
	 * past the grow target a bucket miss goes to the overflow leg,
	 * which is one lock for the whole table. */
	"ld=x.buckets?100*x.entries/(x.buckets*(x.slots||6)):0;\n"
	"var ldk=ld>=90?' b':ld>=70?' w':'';\n"
	"if(pv){var dh=ch(x)-pv.h,dm=cm(x)-pv.m;\n"
	"if(dh>=0&&dm>=0){wn=dh+dm;if(wn>0)rt=100*dh/wn;}}\n"
	"NC[x.name]={h:ch(x),m:cm(x)};\n"
	"var k=rt===null?\"\":(rt<25?\"crit\":rt<60?\"low\":\"\"),FG=x.fleet&&x.fleet.members>1?x.fleet:null,LB=FG&&FG.basis==='at_least';\n"
	"o+='<tr><td><span class=nm>'+esc(x.name)+'</span>'+\n"
	"(uni?'':'<span class=md>'+esc(x.mode||\"\")+'</span>')+\n"
	"'</td><td class=n'+(FG?' title=\"'+(FB[FG.basis]||FG.basis)+' ('+nf(FG.copies)+' copies across '+FG.members+' members)\"':'')+'>'+(FG?fv(FG.entries,x.entries,LB):nf(x.entries))+'</td><td class=\"n dim\">'+nf(x.buckets)+\n"
	"'</td><td class=\"n'+(ldk||' dim')+'\"'+(ld>=70?' title=\"'+(ld>=90?'critical - ':'')+'entries against buckets x slots; past the grow target a bucket miss goes to the overflow leg, one lock for the whole table\"':'')+'>'+(ld<0.01&&!x.entries?\"—\":ld.toFixed(2)+\"%\")+\n"
	"'</td><td class=\"n'+(x.overflow>0&&x.entries&&x.overflow>x.entries/2?' w':' dim')+'\"'+\n"
	"(x.held_walk_us?' title=\"last held walk '+(x.held_walk_us/1000000).toFixed(2)+' s \u2014 every other maintenance duty waits behind it\"':'')+'>'+\n"
	"(x.overflow===undefined?\"\u2014\":nf(x.overflow))+'</td><td><div class=hit>'+\n"
	"(rt===null?'<div class=track></div>':'<div class=\"track '+k+'\"><i style=\"width:'+\n"
	"rt.toFixed(0)+'%\"></i></div>')+'<span class=hv><span class='+(rt===null?'\"n dim\"':'n')+'>'+\n"
	"(rt!==null?rt.toFixed(1)+'%':lf===null?'\u2014':wn===0?'no reads':'\u2014')+'</span><span class=hl>'+\n"
	"(lf===null?'never read':'life '+lf.toFixed(1)+'%')+'</span></span></div></td>'+\n"
	"'<td><div class=hit>'+(cr(x)===undefined?'<span class=dim>\u2014</span>':\n"
	"'<span class=hv><span class=n>'+nf(cr(x))+'</span><span class=hl>'+\n"
	"(x.entries?(100*cr(x)/x.entries).toFixed(cr(x)*100<x.entries?2:1)+'% of held':'of 0 held')+\n"
	"'</span></span>')+'</div></td><td class=n>'+\n"
	"(FG?fv(FG.hits,ch(x)):nf(ch(x)))+'</td><td class=n>'+(FG?fv(FG.misses,cm(x)):nf(cm(x)))+'</td><td class=n'+(FG?' title=\"client writes, each counted once on the node the client wrote to\"':'')+'>'+\n"
	"(FG?fv(FG.stores,x.stores_client):nf(x.stores))+\n"
	"'</td><td class=\"n dim\">'+(FG?fv(FG.expired,x.expired,LB):nf(x.expired))+'</td><td class=n>'+\n"
	"(x.held_age_s===undefined||x.held_age_s<0?'<span class=dim>\u2014</span>':sz(x.held_bytes))+\n"
	/* S116: the sizes HELD, from the walk - not this node's client writes,
	 * which left a push-fed member's column blank */
	"'</td><td>'+(x.held_age_s===undefined||x.held_age_s<0?'<span class=dim>\u2014</span>':hist(x.held_hist))+'</td></tr>';}\n"
	"PC=NC;\n"
	"document.getElementById(\"cols\").innerHTML=o||\n"
	"'<tr><td colspan=13 class=dim>no collections</td></tr>';\n"
	/* S67: eight bars, one per size class, scaled to the fullest */
	"function hist(a){if(!a)return'';var m=0,i,o='<span class=hist>';for(i=0;i<a.length;i++)if(a[i]>m)m=a[i];\n"
	"if(!m)return'<span class=dim>\u2014</span>';var L=['64','256','1K','4K','16K','64K','256K','more'];\n"
	"for(i=0;i<a.length;i++)o+='<i title=\"'+L[i]+': '+nf(a[i])+'\" style=\"height:'+Math.max(1,Math.round(14*a[i]/m))+'px\"></i>';return o+'</span>';}\n"
	"function bud(s){var b=s.budget||{},c=s.collections||[],r=[],i,x;\n"
	"for(i=0;i<c.length;i++){x=c[i];if(x.index_bytes===undefined)continue;\n"
	"r.push([esc(x.name),'index '+sz(x.index_bytes)+' \\u00b7 records '+(x.held_age_s===undefined||x.held_age_s<0?'\\u2014':sz(x.held_cells))]);}\n"
	"r.push([\"index, all\",b.index===undefined?\"\\u2014\":sz(b.index)]);\n"
	"r.push([\"records, all (as cells)\",b.records===undefined?\"\\u2014\":sz(b.records)]);\n"
	"r.push([\"of the ceiling\",b.ceiling?Math.round(100*((b.index||0)+(b.records||0))/b.ceiling)+\"%\":\"\\u2014\",\n"
	"b.ceiling&&((b.index||0)+(b.records||0))/b.ceiling>0.85]);\n"
	"r.push([\"reservation\",b.reservation===undefined?\"\\u2014\":sz(b.reservation)]);return r;}\n"
	/* S160: the commands card, from S159's /stats rows - top verbs by
	 * calls (with the MEAN), the slowest calls, and the slow log's tail;
	 * same poll.  A mean over tens of thousands of calls hides a single
	 * 10 ms one, so the slowest card reads each verb's histogram for its
	 * WORST bucket (S159's log2 buckets: k holds up to 2^k us, the top
	 * one everything past 32.8 ms) and sorts on that. */
	"function lat(u){u=+u;return !(u>=0)?'\\u2014':u<1000?nf(Math.round(u))+' \\u03bcs':u<1e5?(u/1000).toFixed(1)+' ms':u<1e6?Math.round(u/1000)+' ms':(u/1e6).toFixed(1)+' s';}\n"
	"function worst(x){var h=x.hist||[],k;for(k=h.length-1;k>=0;k--)if(h[k])return k;return -1;}\n"
	"function wmax(x){var k=worst(x),n=(x.hist||[]).length;return k<0?'\\u2014':k==n-1?'> '+lat(Math.pow(2,k-1)):'\\u2264 '+lat(Math.pow(2,k));}\n"
	"function wspan(x){var k=worst(x),n=(x.hist||[]).length;return k<0?'none':k==n-1?'over '+lat(Math.pow(2,k-1)):k?lat(Math.pow(2,k-1))+' to '+lat(Math.pow(2,k)):'up to 1 us';}\n"
	"function lat1(u){u=+u;return u<1000?u.toFixed(1)+' \\u03bcs':lat(u);}\n"
	/* the slowest-calls TABLE (its own section under collections): every
	 * verb that has been called, worst call first, then p99, then mean */
	"function drawCmds(s){var cm=(s.commands||[]).filter(function(x){return x.calls>0;}),o='',i,x;\n"
	"cm.sort(function(x,y){return worst(y)-worst(x)||y.p99_us-x.p99_us||y.usec/y.calls-x.usec/x.calls;});\n"
	"for(i=0;i<cm.length;i++){x=cm[i];\n"
	"o+='<tr><td>'+esc(x.name)+'</td><td class=n>'+nf(x.calls)+'</td><td class=n>'+lat1(x.usec/x.calls)+\n"
	"'</td><td class=n>'+lat(x.p50_us)+'</td><td class=n>'+lat(x.p99_us)+'</td><td class=n>'+lat(x.p999_us)+\n"
	"'</td><td class=n title=\"the slowest call fell '+wspan(x)+'\">'+wmax(x)+'</td></tr>';}\n"
	"document.getElementById(\"cmds\").innerHTML=o||'<tr><td colspan=7 class=dim>no commands yet</td></tr>';}\n"
	"function cmdcards(s){var cm=(s.commands||[]).slice(),sl=(s.slowlog||[]).slice(0,6),now=Date.now()/1000,a;if(!cm.length&&!sl.length)return'';\n"
	"a=cm.slice().sort(function(x,y){return y.calls-x.calls;}).slice(0,8).map(function(x){return [esc(x.name),nf(x.calls)+' \\u00b7 '+(x.calls?(x.usec/x.calls).toFixed(1):'0')+' \\u03bcs'];});\n"
	"return card('commands \\u00b7 by calls',a.length?a:[['none yet','']])+\n"
	"card('slow log \\u00b7 tail',sl.length?sl.map(function(e){var t=esc((e.argv||[]).join(' '));\n"
	"return ['<span title=\"'+t+' \\u00b7 '+lat(e.usec)+(e.addr?' \\u00b7 from '+esc(e.addr)+(e.name?' ['+esc(e.name)+']':''):'')+(e.ts?' \\u00b7 '+esc(new Date(e.ts*1000).toLocaleString()):'')+'\">'+t+'</span>',\n"
	"lat(e.usec)+(e.ts?' \\u00b7 '+ago1(now-e.ts):'')];}):[['empty','']],'clip');}\n"
	/* S165: the commands clients sent that no door implements, fleet-wide
	 * when this node is clustered - the daemon folds every member's table
	 * (/stats unknown_commands), the page only renders it.  A row is a
	 * name, never an argument; the hover carries who sent it last. */
	"function ago1(t){t=Math.max(0,Math.floor(t));return t<60?t+'s':t<3600?Math.floor(t/60)+'m':t<86400?Math.floor(t/3600)+'h':Math.floor(t/86400)+'d';}\n"
	"function unkcard(s){var u=s.unknown_commands,now=Date.now()/1000,rows;if(!u)return'';\n"
	"rows=(u.rows||[]).slice(0,8).map(function(x){return ['<span title=\"'+esc(x.name)+' ('+esc(x.dialect)+')'+(u.members>1?' \\u00b7 '+nf(x.here)+' of '+nf(x.count)+' here':'')+\n"
	"' \\u00b7 first seen '+ago1(now-x.first_s)+' ago \\u00b7 last from '+esc(x.addr)+(x.client?' ['+esc(x.client)+']':'')+(u.members>1?' via node '+x.node:'')+'\">'+esc(x.name)+'</span>',\n"
	"nf(x.count)+' \\u00b7 '+ago1(now-x.last_s),true];});\n"
	"if(!rows.length)rows=[['none','']];if(u.other)rows.push(['other names',nf(u.other),true]);\n"
	"if(u.preauth)rows.push(['before login',nf(u.preauth)]);\n"
	"if(u.members>1&&u.reporting<u.members)rows.push(['reporting',u.reporting+' of '+u.members,true]);\n"
	"return card('unknown commands',rows,'unk');}\n"
	/* the plane's cards ordered by their own height, tallest first, so a
	 * grid row holds cards of one size instead of one long card leaving
	 * its neighbours half empty.  Measured with the grid at align-items
	 * start (stretched, every card in a row reads the row's height), in
	 * 8 px steps so a sub-pixel difference is not a size; equal cards
	 * keep the order they are written in, so the grouping below survives
	 * among cards of a size and a redraw moves a card only when it
	 * changes height. */
	"function sizePlane(){var p=document.getElementById(\"plane\"),k=[].slice.call(p.children),i;\n"
	"p.style.alignItems=\"start\";for(i=0;i<k.length;i++){k[i].__h=Math.round(k[i].offsetHeight/8);k[i].__i=i;}\n"
	"p.style.alignItems=\"\";k.sort(function(x,y){return y.__h-x.__h||x.__i-y.__i;});\n"
	"for(i=0;i<k.length;i++)p.appendChild(k[i]);}\n"
	"function card(t,rows,cls){var h='<div class=\"pc'+(cls?' '+cls:'')+'\"><h3>'+t+\"</h3>\",z;\n"
	"for(z=0;z<rows.length;z++)h+='<div class=\"kv'+(rows[z][2]?\" w\":\"\")+'\"><span>'+rows[z][0]+\n"
	"\"</span><span>\"+rows[z][1]+\"</span></div>\";return h+\"</div>\";}\n"
	"document.getElementById(\"plane\").innerHTML=\n"
	"card(\"pulls\",[[\"sent\",nf(c.pull_sent)],[\"hits\",nf(c.pull_hits)],\n"
	"[\"misses\",nf(c.pull_misses),c.pull_misses>0],[\"timeouts\",nf(c.pull_timeouts),c.pull_timeouts>0],\n"
	"[\"served for peers\",nf(c.pull_served)]])+\n"
	"card(\"forwards\",[[\"sent\",nf(c.fwd_sent)],[\"served\",nf(c.fwd_served)],\n"
	"[\"no route\",nf(c.fwd_no_route),c.fwd_no_route>0],[\"send failures\",nf(c.fwd_send_fail),c.fwd_send_fail>0]])+\n"
	"card(\"replication\",[[\"pushed on write\",nf(c.repl_pushed)],[\"swept out\",nf(c.repl_out)],\n"
	"[\"repair skipped (dying)\",nf(c.repl_skipped_dying)],[\"migrated in\",nf(c.migrated_in)],\n"
	"[\"migrated out\",nf(c.migrated_out)],[\"lost\",nf(c.migrate_lost),c.migrate_lost>0]])+\n"
	/* S125: the receive side, which nothing else on this page shows.  A
	 * node that cannot keep up with inbound replicas heartbeats
	 * normally, stays a member and answers its client door quickly, so
	 * every other card here reads green while it drifts out of date.
	 * The rate leads (what it IS doing), the queue says how close the
	 * buffer is to full, and drops are records already lost and waiting
	 * on the repair sweep - flagged, because that is the line between
	 * "behind" and "inconsistent until the sweep runs". */
	"card(\"replica intake\",[[\"applied /s\",nf(c.rx_applied_ps)],\n"
	"[\"applied, total\",nf(c.rx_applied)],\n"
	"[\"refused as older /s\",nf(c.rx_older_ps),c.rx_older_ps>0],\n"
	"[\"receive queue\",c.rx_rcvbuf?(sz(c.rx_queue)+\" of \"+sz(c.rx_rcvbuf)):sz(c.rx_queue),\n"
	"  !!(c.rx_rcvbuf&&c.rx_queue>c.rx_rcvbuf/2)],\n"
	"[\"DROPPED /s\",nf(c.rx_drops_ps),c.rx_drops_ps>0],\n"
	"[\"dropped, total\",nf(c.rx_drops),c.rx_drops>0]])+\n"
	"card(\"backpressure\",[[\"pending peak\",nf(c.pend_peak)],[\"pending max\",nf(c.pend_max)],\n"
	"[\"exhausted\",nf(c.pend_exhausted),c.pend_exhausted>0],[\"socket buffer\",mb(c.rcvbuf)+\" MB\"]])+\n"
	"card(\"negative cache\",[[\"hits\",nf(c.neg_hits)],[\"tombstones sent\",nf(c.tomb_sent)],\n"
	"[\"applied\",nf(c.tomb_applied)]])+\n"
	/* memory: held/max/live and the kernel's own figure are backing-
	 * independent, so they always mean something.  total/used/free
	 * describe the huge-page arena and arrive as null when that is
	 * not the backing in use - rendered as a quantity they would read
	 * as an arena with nothing left. */
	"card(\"memory\",[[\"live\",sz(m.arena_live)],[\"held\",sz(m.arena_held)],\n"
	"[\"of which structure\",m.arena_regions===undefined?\"\\u2014\":sz(m.arena_regions)],\n"
	/* S118: the rows under held add up to it - structure + class chunks
	 * (live inside) + warm free, plus the shm pages' alignment slots when
	 * there are any */
	"[\"of which class chunks (live inside)\",m.arena_class_chunks===undefined?\"\\u2014\":sz(m.arena_class_chunks)],\n"
	"[\"of which free, warm\",m.arena_warm_free===undefined?\"\\u2014\":sz(m.arena_warm_free)]]\n"
	".concat(m.arena_page_slack>0?[[\"of which page slack\",sz(m.arena_page_slack)]]:[]).concat([\n"
	"[\"ceiling\",sz(m.arena_max)],[\"headroom\",(m.headroom_pct===undefined?\"\\u2014\":\n"
	"m.headroom_pct+\"%\"),m.headroom_pct<15],[\"free\",m.arena_capacity_valid?sz(m.arena_free):\n"
	"\"not measured\"],[\"resident (rss)\",sz(pr.rss_bytes)],\n"
	"[\"writes refused\",nf(m.nomem),m.nomem>0]]))+\n"
	/* S120: what the collections cost, from the daemon's exact figures -
	 * each table's index regions and its records as the cells they occupy */
	"card(\"memory budget\",bud(s))+\n"
	"card(\"process\",[[\"uptime\",dur(pr.uptime_s)],[\"threads\",nf(pr.threads)],\n"
	"[\"cpu user\",((pr.cpu_user_ms||0)/1000).toFixed(1)+\"s\"],\n"
	"[\"cpu system\",((pr.cpu_sys_ms||0)/1000).toFixed(1)+\"s\"],[\"pid\",nf(pr.pid)]])+\n"
	/* S89: the doors, from stats.listeners; a non-native door with no
	 * secret is the condition the startup WARNING names - show it */
	"card(\"listeners\",(s.listeners||[]).map(function(l){return [l.kind+\" \"+esc(l.addr)+\":\"+l.port,\n"
	"(l.plaintext?\"plaintext\":\"encrypted\")+(l.allow?\", allow-list \"+l.allow:\"\")+(l.secret?\"\":\", NO SECRET\"),\n"
	"!l.secret&&l.kind!==\"native\"];}))+\n"
	"card(\"native \\u00b7 json\",[[\"open\",nf(nt.json?nt.json.open:null)],\n"
	"[\"connections \"+sl,nf(nt.json?nt.json.conns:null)],\n"
	"[\"requests \"+sl,nf(nt.json?nt.json.requests:null)]])+\n"
	"card(\"native \\u00b7 binary\",[[\"open\",nf(nt.binary?nt.binary.open:null)],\n"
	"[\"connections \"+sl,nf(nt.binary?nt.binary.conns:null)],\n"
	"[\"requests \"+sl,nf(nt.binary?nt.binary.requests:null)]])+\n"
	"card(\"resp door\",[[\"open\",nf(r.open)],[\"clients \"+sl,nf(r.conns)],[\"requests \"+sl,nf(r.requests)],\n"
	"[\"rejected\",nf(r.rejected),r.rejected>0],\n"
	"[\"auth failures\",nf(r.authfail),r.authfail>0],\n"
	"[\"resp via native door\",nf(nt.resp?nt.resp.requests:null)]])+\n"
	"cmdcards(s)+unkcard(s)+\n"
	/* PS7: the pub/sub card.  Subscribers, channels and patterns are
	 * gauges (what is attached NOW); the rest are totals.  A relay gap
	 * and a publish too large to relay are both "a subscriber on another
	 * node did not hear it", so they carry the warn colour - they are the
	 * only figures here that mean something is wrong. */
	"(!s.pubsub?\"\":card(\"pub/sub\",[[\"subscribers\",nf(s.pubsub.subscribers)],\n"
	"[\"channels\",nf(s.pubsub.channels)],[\"patterns\",nf(s.pubsub.patterns)],\n"
	"[\"published\",nf(s.pubsub.published)],[\"delivered locally\",nf(s.pubsub.delivered)],\n"
	"[\"keyspace events\",nf(s.pubsub.keyspace_events)],\n"
	"[\"slow subscribers closed\",nf(s.pubsub.slow_kills),s.pubsub.slow_kills>0],\n"
	"[\"deliveries lost to allocation\",nf(s.pubsub.alloc_failed),s.pubsub.alloc_failed>0],\n"
	"[\"dropped at a full worker queue\",nf(s.pubsub.queue_dropped),s.pubsub.queue_dropped>0],\n"
	"[\"publishers paused on a full queue\",nf(s.pubsub.publish_paused)],\n"
	"[\"queued now\",sz(s.pubsub.queue_bytes)],\n"
	"[\"UDP push streams\",nf(s.pubsub.udp?s.pubsub.udp.streams:null)],\n"
	"[\"UDP streams pruned\",nf(s.pubsub.udp?s.pubsub.udp.pruned_no_ack+s.pubsub.udp.pruned_no_progress:null),!!(s.pubsub.udp&&s.pubsub.udp.pruned_no_ack+s.pubsub.udp.pruned_no_progress>0)]])+\n"
	"card(\"pub/sub \\u00b7 fleet relay\",[[\"sent to peers\",nf(s.pubsub.relay_sent)],\n"
	"[\"received from peers\",nf(s.pubsub.relay_recv)],\n"
	"[\"lost (left the window unseen)\",nf(s.pubsub.relay_lost),s.pubsub.relay_lost>0],\n"
	"[\"duplicates dropped\",nf(s.pubsub.relay_duplicates),s.pubsub.relay_duplicates>0],\n"
	"[\"too large to relay\",nf(s.pubsub.relay_dropped),s.pubsub.relay_dropped>0],\n"
	"[\"relays this node asks for\",s.pubsub.relay_mode||\"all\"],\n"
	"[\"not sent: the peer holds no match\",nf(s.pubsub.relay_skipped)]]))+\n"
	"card(\"cluster auth\",[[\"bad auth\",nf(c.bad_auth),c.bad_auth>0],\n"
	"[\"lamport rejected\",nf(c.lamport_rejected),c.lamport_rejected>0],\n"
	"[\"term rejected\",nf(c.term_rejected),c.term_rejected>0],\n"
	"[\"map refused\",nf(c.map?c.map.refused:null),!!(c.map&&c.map.refused>0)]])+\n"
	/* S91: durability was one line saying "on".  stats already carries
	 * the fsync mode, the startup probe, the OBSERVED fsync latency, the
	 * loss counters and the whole RDB save record - an operator who has
	 * just turned the WAL on cannot see any of it.  Two cards, and the
	 * three things that mean something is wrong are flagged: dropped
	 * (acknowledged writes that never reached the log), overruns, and a
	 * probe that underestimated the device. */
	"card(\"durability \\u00b7 wal\", !s.wal ? [[\"wal\",\"off\"]] : [\n"
	"[\"fsync\",s.wal.fsync||\"-\"],\n"
	"[\"records appended\",nf(s.wal.appended)],\n"
	"[\"bytes appended\",sz(s.wal.bytes)],\n"
	"[\"DROPPED (acked, unlogged)\",nf(s.wal.dropped),s.wal.dropped>0],\n"
	"[\"late\",nf(s.wal.late),s.wal.late>0],\n"
	"[\"segment overruns\",nf(s.wal.overruns),s.wal.overruns>0],\n"
	"[\"free segments\",nf(s.wal.free_segments)],\n"
	/* S145: one "unsynced" figure spanned two states with different
	 * consequences and misled the rc5/rc6 diagnosis.  STAGED is acked and
	 * still in a producer ring - lost on kill -9, DROPPED if the ring
	 * fills - so it is the one that is flagged.  UNSYNCED is in the file
	 * and not yet fsynced: a small non-zero reading is what an everysec
	 * WAL looks like, and it clears on the next pump, so it is NOT
	 * flagged.  An older daemon publishes neither; then the old combined
	 * figure shows under the honest name. */
	"[\"staged (acked, not yet in the log)\",\n"
	"  nf(s.wal.staged!==undefined?s.wal.staged:((s.wal.last_seq||0)-(s.wal.synced_seq||0))),\n"
	"  (s.wal.staged!==undefined?s.wal.staged:((s.wal.last_seq||0)-(s.wal.synced_seq||0)))>0],\n"
	"[\"unsynced (in the log, not yet fsynced)\",\n"
	"  s.wal.unsynced!==undefined?nf(s.wal.unsynced):\"\\u2014\"],\n"
	"[\"storage\",(s.wal.storage_class||\"?\")+\" \"+(s.wal.fstype||\"\")],\n"
	/* S148 family: this row said "fsync now" and showed neither.  avg is
	 * the mean over every fsync since start and max is a high-water
	 * mark that never decays, so a single 117 ms stall on .247 sat
	 * under the word "now" for the rest of the process lifetime and
	 * read as a live condition.  What IS now is the EWMA - and it is
	 * the figure "probe underestimated" tests (recent >= 4x probe
	 * p50, wal.c), so showing it lets the reader check that flag
	 * instead of trusting it. */
	"[\"fsync now (ewma)\",s.wal.observed?\n"
	"  (nf(s.wal.observed.fsync_recent_us)+\" \\u00b5s\"):\"\\u2014\"],\n"
	"[\"fsync since start (avg/max)\",s.wal.observed?\n"
	"  (nf(s.wal.observed.fsync_avg_us)+\" / \"+nf(s.wal.observed.fsync_max_us)+\" \\u00b5s\"):\"\\u2014\"],\n"
	"[\"probe p50/p99\",s.wal.probe?\n"
	"  (nf(s.wal.probe.fsync_p50_us)+\" / \"+nf(s.wal.probe.fsync_p99_us)+\" \\u00b5s, \"+nf(s.wal.probe.seq_mb_s)+\" MB/s\"):\"\\u2014\"],\n"
	"[\"probe underestimated\",(s.wal.observed&&s.wal.observed.probe_underestimated)?\"YES\":\"no\",\n"
	"  !!(s.wal.observed&&s.wal.observed.probe_underestimated)]])+\n"
	"card(\"durability \\u00b7 rdb\", !s.rdb ? [[\"rdb\",\"off\"]] : [\n"
	"[\"snapshots\",nf(s.rdb.saves)],\n"
	"[\"running now\",s.rdb.running?\"yes\":\"no\"],\n"
	"[\"last snapshot\",s.rdb.last_unix?dur(Math.max(0,Math.floor(Date.now()/1000)-s.rdb.last_unix))+\" ago\":\"never\",\n"
	"  !s.rdb.last_unix],\n"
	"[\"last size\",sz(s.rdb.last_bytes)],\n"
	"[\"last duration\",(s.rdb.last_dur_ms===undefined||s.rdb.last_dur_ms===null)?\"\\u2014\":lat(1000*s.rdb.last_dur_ms)]]);\n"
	"drawCmds(s);sizePlane();trend();}\n"
	/* S160: the clients panel.  TOP LEVEL, not inside drawStats(): the
	 * panel's click handler and tick() call loadClients() from outside it,
	 * and nested there it never existed for them - a ReferenceError on
	 * every open and every poll, so the table stayed empty from S160 on.
	 * clientstest only asked whether the page TEXT carried the functions. */
	"function loadClients(){get('clients',function(d){if(d)CLD=d;drawClients();});}\n"
	"function drawClients(){var d=CLD||{clients:[],total:0,shown:0},f=(document.getElementById('clfilter').value||'').toLowerCase(),rows=d.clients||[],o='',i,x,n=0,idle=0;\n"
	"for(i=0;i<rows.length;i++){x=rows[i];if(f&&String(x.addr).toLowerCase().indexOf(f)<0&&String(x.name||'').toLowerCase().indexOf(f)<0&&String(x.lib_name||'').toLowerCase().indexOf(f)<0)continue;n++;\n"
	/* a connection that has sent nothing since connect has no dialect yet
	 * (it is learned from the first request's bytes), no last command and
	 * - native - no name: libperfd keeps a standby connection to EVERY
	 * member (S104), so most such rows are standbys waiting for a failover.
	 * Say that instead of a '?' and three blank cells. */
	"var nu=!x.cmds&&x.dialect==='?';if(nu)idle++;\n"
	"o+='<tr><td class=n>'+nf(x.id)+'</td><td>'+esc(x.door)+'</td><td>'+(nu?'<span class=dim title=\"no request since it connected - the dialect is learned from the first one; libperfd keeps a standby connection to every member\">idle</span>':esc(x.dialect))+'</td><td>'+(x.encrypted?'encrypted':'plain')+'</td><td class=mono>'+esc(x.addr)+'</td><td>'+(x.name?esc(x.name):x.lib_name?'':'<span class=dim>\\u2014</span>')+\n"
	/* CLIENT SETINFO: the library beside the name - behind a proxy it is
	 * the one thing that says which client a row is */
	"(x.lib_name?' <span class=dim>'+esc(x.lib_name)+(x.lib_ver?' '+esc(x.lib_ver):'')+'</span>':'')+\n"
	"'</td><td title=\"'+new Date(Date.now()-1000*(x.age_s||0)).toLocaleString()+'\">'+dur(x.age_s)+'</td><td class=n>'+dur(x.idle_s)+'</td><td class=n>'+nf(x.cmds)+'</td><td class=mono>'+(x.last_cmd?esc(x.last_cmd):'<span class=dim>\\u2014</span>')+\n"
	"'</td><td class=n'+(x.pending>0?' style=\"color:var(--warn)\"':'')+'>'+sz(x.pending)+'</td><td class=n>'+nf(x.subs)+'</td></tr>';}\n"
	"document.getElementById('clrows').innerHTML=o||'<tr><td colspan=12 class=dim>no connections</td></tr>';\n"
	"document.getElementById('clhint').textContent='showing '+n+' of '+nf(d.total)+(d.shown<d.total?' (the newest '+d.shown+' fetched)':'')+(idle?' \\u00b7 '+idle+' idle since connect':'')+(f?' \\u00b7 filtered':'');}\n"
	"function tick(){\n"
	"get(\"stats\",function(s){\n"
	"if(!s){document.getElementById(\"alert\").innerHTML=\n"
	"'<p class=err>this node did not answer</p>';return;}\n"
	"document.getElementById(\"alert\").innerHTML=\"\";drawStats(s);last=Date.now();});\n"
	"get(\"members\",function(d){if(d&&d.members)drawFleet(d.members);});if(CLOPEN)loadClients();}\n"
	"document.getElementById('metrics').addEventListener('click',function(e){var t=e.target;while(t&&t.id!=='clmetric')t=t.parentNode;if(!t)return;\n"
	"CLOPEN=!CLOPEN;document.getElementById('clpanel').hidden=!CLOPEN;if(CLOPEN)loadClients();});\n"
	"document.getElementById('clfilter').oninput=function(){drawClients();};\n"
	"setInterval(function(){document.getElementById(\"age\").textContent=\n"
	"last?\"updated \"+Math.round((Date.now()-last)/1000)+\"s ago · every 3s\":\"\";},1000);\n"
	"(function(){var t=document.getElementById(\"trend\");\n"
	"t.addEventListener(\"mousemove\",function(e){var pw=e.target.closest?e.target.closest(\".pw\"):null;if(!pw)return;\n"
	"var k=pw.getAttribute(\"data-k\"),a=H[k]||[];if(a.length<2)return;var r=pw.getBoundingClientRect();\n"
	"var i=Math.round((e.clientX-r.left)/r.width*(a.length-1));if(i<0)i=0;if(i>a.length-1)i=a.length-1;\n"
	"if(HV[k]!==i){HV[k]=i;trend();}});\n"
	"t.addEventListener(\"mouseleave\",function(){var had=false,k;for(k in HV){had=true;}HV={};if(had)trend();});})();\n"
	"document.getElementById(\"rst\").onclick=function(){\n"
	"if(!confirm(\"Reset this node's running totals?\\nOpen connections, entries and memory are untouched.\"))return;\n"
	"post(\"reset-stats\",function(ok){if(!ok)alert(\"the node refused the reset\");tick();});};\n"
	"tick();setInterval(tick,POLL);\n"
	"</script>\n"
	"\n"
	;
const unsigned int pc_status_page_len = sizeof pc_status_page - 1;
