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
	/* S202: the anchor as the favicon - contrib/brand/favicon.svg, the
	 * small cut, inlined as a data URI so the page needs no second
	 * route and no second request.  Light and dark via the SVG's own
	 * prefers-color-scheme.  Regenerate with `base64 -w0` when the
	 * file changes; httptest decodes it and checks it is the anchor. */
	"<link rel=icon type=\"image/svg+xml\" href=\"data:image/svg+xml;base64,"
	"PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAzMiAz"
	"MiIgd2lkdGg9IjMyIiBoZWlnaHQ9IjMyIiByb2xlPSJpbWciIGFyaWEtbGFiZWw9InBlcmZjYWNo"
	"ZWQiPgogIDx0aXRsZT5wZXJmY2FjaGVkIGZhdmljb24gLSB0aGUgYW5jaG9yLCBzbWFsbCBjdXQ8"
	"L3RpdGxlPgogIDxkZXNjPlRoZSBhbmNob3IgYXQgMTYtMzIgcHg6IHN0b2NrLCBzaGFuaywgdHdv"
	"IHJpc2luZyBhcm1zLCBvbmUgbGl0IGNlbGwgYXQgdGhlIHJpbmcgYW5kIHRocmVlIGdyZXkgY2Vs"
	"bHMuICBObyBmbHVrZSB0aXBzIGF0IHRoaXMgc2l6ZSAtIHRoZSBmb3VyIGNlbGxzIGFuZCB0aGUg"
	"c3RvY2sgY2FycnkgaXQuPC9kZXNjPgogIDxzdHlsZT4KICAgIC5pbmsgeyBzdHJva2U6ICMxYjFm"
	"MjQ7IH0KICAgIC5rZXB0IHsgZmlsbDogIzlhYTRiMjsgfQogICAgLmZvdW5kIHsgZmlsbDogIzJm"
	"NmRmNjsgfQogICAgQG1lZGlhIChwcmVmZXJzLWNvbG9yLXNjaGVtZTogZGFyaykgewogICAgICAu"
	"aW5rIHsgc3Ryb2tlOiAjZTZlOWVlOyB9CiAgICAgIC5rZXB0IHsgZmlsbDogIzZiNzQ4MjsgfQog"
	"ICAgfQogIDwvc3R5bGU+CiAgPGxpbmUgY2xhc3M9ImluayIgeDE9IjE2IiB5MT0iOSIgeDI9IjE2"
	"IiB5Mj0iMjciIHN0cm9rZS13aWR0aD0iMS41IiBzdHJva2UtbGluZWNhcD0icm91bmQiLz4KICA8"
	"bGluZSBjbGFzcz0iaW5rIiB4MT0iOSIgeTE9IjEzIiB4Mj0iMjMiIHkyPSIxMyIgc3Ryb2tlLXdp"
	"ZHRoPSIxLjUiIHN0cm9rZS1saW5lY2FwPSJyb3VuZCIvPgogIDxwYXRoIGNsYXNzPSJpbmsiIGQ9"
	"Ik0xNiAyOCBRMTAgMjkgNyAyNCIgZmlsbD0ibm9uZSIgc3Ryb2tlLXdpZHRoPSIxLjUiIHN0cm9r"
	"ZS1saW5lY2FwPSJyb3VuZCIvPgogIDxwYXRoIGNsYXNzPSJpbmsiIGQ9Ik0xNiAyOCBRMjIgMjkg"
	"MjUgMjQiIGZpbGw9Im5vbmUiIHN0cm9rZS13aWR0aD0iMS41IiBzdHJva2UtbGluZWNhcD0icm91"
	"bmQiLz4KICA8cmVjdCBjbGFzcz0iZm91bmQiIHg9IjEzLjUiIHk9IjMiIHdpZHRoPSI1IiBoZWln"
	"aHQ9IjUiIHJ4PSIxLjUiLz4KICA8cmVjdCBjbGFzcz0ia2VwdCIgeD0iMyIgeT0iMjAiIHdpZHRo"
	"PSI1IiBoZWlnaHQ9IjUiIHJ4PSIxIi8+CiAgPHJlY3QgY2xhc3M9ImtlcHQiIHg9IjI0IiB5PSIy"
	"MCIgd2lkdGg9IjUiIGhlaWdodD0iNSIgcng9IjEiLz4KICA8cmVjdCBjbGFzcz0ia2VwdCIgeD0i"
	"MTMuNSIgeT0iMjYiIHdpZHRoPSI1IiBoZWlnaHQ9IjUiIHJ4PSIxIi8+Cjwvc3ZnPgo="
	"\">\n"
	"<meta name=viewport content=\"width=device-width,initial-scale=1\">\n"
	"<style>\n"
	/* color-scheme: the palette follows prefers-color-scheme, but without
	 * it the browser draws its own parts - the table's horizontal
	 * scrollbar, the filter input - light on the dark page. */
	":root{color-scheme:light dark;--bg:#f4f6f7;--panel:#fff;--panel2:#eef1f3;--line:#d3dade;\n"
	"--ink:#12191c;--ink2:#4a585e;--ink3:#7d8b91;--accent:#0b6a72;--accents:#d6ecee;\n"
	"--ok:#1a7f4b;--oks:#dcf0e5;--okw:#f3faf6;--warn:#9a6206;--warns:#fbeed6;--warnw:#fdf7ec;\n"
	"--bad:#a32723;--bads:#f8dedd;--badw:#fdf2f1;--north:#d6453d;--idle:#8c979c;--idles:#e6eaec;--bar:#c9d4d8;\n"
	/* S210: the blink's colours - saturated, for the alarm only; the
	 * text tokens above stay the muted shades a warning figure uses */
	"--badhot:#e8261e;--warnhot:#f0a000;\n"
	/* S382: an ACTIVE upstream link is raised - light mode by a shade
	 * under it (a white highlight reads as nothing on the white panel),
	 * dark mode by a highlight on top */
	"--emb:inset 0 -2px 0 rgba(0,0,0,.16),0 1px 2px rgba(0,0,0,.18);\n"
	"--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,\"Liberation Mono\",monospace;\n"
	"--sans:system-ui,-apple-system,Segoe UI,Roboto,\"Helvetica Neue\",sans-serif}\n"
	"@media (prefers-color-scheme:dark){:root{--bg:#0d1416;--panel:#161f23;--panel2:#1c282c;\n"
	"--line:#384b52;--ink:#e6eef0;--ink2:#9fb2b8;--ink3:#6d8189;--accent:#5ec8d0;--accents:#12343a;\n"
	"--ok:#5fd39a;--oks:#123329;--okw:#141f1d;--warn:#e0aa53;--warns:#33260f;--warnw:#221d13;\n"
	"--bad:#f0857f;--bads:#3a1a19;--badw:#251817;--north:#ef6b64;--idle:#74858b;--idles:#1d282c;--bar:#2c3b41;\n"
	"--badhot:#ff3b30;--warnhot:#ffb520;\n"
	"--emb:inset 0 1px 0 rgba(255,255,255,.13),inset 0 -2px 0 rgba(0,0,0,.5)}}\n"
	"*{box-sizing:border-box}\n"
	"body{margin:0;padding:0 20px 48px;background:var(--bg);color:var(--ink);\n"
	"font:14px/1.5 var(--sans);-webkit-font-smoothing:antialiased}\n"
	".wrap{max-width:1500px;margin:0 auto}\n"
	/* S208: centre, not baseline - an inline SVG has no text baseline, so
	 * the anchor, the compass, the select and the button each settled
	 * where their own box put them.  The controls are ONE group.
	 * S227: and that group has a row of its OWN, left-aligned under the
	 * anchor.  Right-aligned beside the identity it fitted until the
	 * identity arrived - the page draws the header, then fills node,
	 * identity, role, version and state a moment later - and at 64 px
	 * marks the filled row no longer fitted, so the whole group jumped to
	 * a second line a second after load (the operator's report).  Always
	 * on its own line, nothing on the first row can move it. */
	"header{display:flex;flex-wrap:wrap;gap:12px 24px;align-items:center;\n"
	"padding:16px 0 12px;border-bottom:1px solid var(--line);margin-bottom:18px}\n"
	".ctl{display:flex;flex-wrap:wrap;align-items:center;gap:10px 16px;flex-basis:100%;justify-content:flex-start}\n"
	/* S227: text-box trims the wordmark's line box to its CAP height, so
	 * the header's centring meets the capitals' own middle - the untrimmed
	 * box reserves descender room a capital never uses, and the mark read
	 * low against PERFCACHED.  A browser without it keeps the old box. */
	".brand{display:flex;align-items:center;gap:14px;font-family:var(--mono);font-weight:600;font-size:16px;letter-spacing:.14em;text-box:trim-both cap alphabetic;\n"
	"text-transform:uppercase;color:var(--accent)}\n"
	/* S206: these two rule sets once sat BETWEEN the two lines of the
	 * .brand rule - inside its open brace.  Chrome's recovery let the
	 * anchor through and dropped the compass, which rendered black and
	 * unsized.  httptest now parses the served stylesheet and refuses a
	 * rule that opens inside another. */
	/* S203: the anchor beside the wordmark - the same mark as the favicon,
	 * inline so its inks follow the page's tokens: ink for the strokes,
	 * ink3 for the three kept cells, the accent for the found cell, which
	 * is also the wordmark's colour. */
	/* S208: one declared size for both header marks (--markpx).  S227: 64 px,
	 * so each mark's cells read as squares (the anchor's are 10 px) - at
	 * 38 they read as dots, and the cells are the point of both marks. */
	":root{--markpx:64px}\n"
	".mark{width:var(--markpx);height:var(--markpx);flex:none}.mark .ink{stroke:var(--ink)}.mark .kept{fill:var(--ink3)}.mark .found{fill:var(--accent)}\n"
	/* S204: the compass is the CLIENT's mark - it goes where something finds
	 * a node: the clients panel and the node switch.  Same tokens as the
	 * anchor, plus a north red for the needle. */
	".cmp{width:18px;height:18px;flex:none;vertical-align:-4px}.cmp .ink{stroke:var(--ink)}.cmp .inkf{fill:var(--ink)}.cmp .kept{fill:var(--ink3)}.cmp .found{fill:var(--accent)}.cmp .north{fill:var(--north)}\n"
	/* S207: beside the wordmark row the compass is the anchor's size; in
	 * the clients panel's 12 px header it stays small. */
	".switch .cmp{width:var(--markpx);height:var(--markpx);vertical-align:0}\n"
	".idbits{display:flex;flex-wrap:wrap;gap:5px 18px;color:var(--ink2);font-size:13px}\n"
	".idbits b{font-family:var(--mono);font-weight:500;color:var(--ink)}\n"
	/* the header ALARM: an operator who opened the page must see a host
	 * in trouble without scrolling to the fleet.  The cards' own blink
	 * (S210's keyframes and cadences: red .7 s, amber 1 s, hard on/off),
	 * on THIS node's state in the header, and on a chip beside it that
	 * names the other hosts in trouble and jumps to them.  At rest the
	 * state has a transparent frame and no padding change, so nothing on
	 * the identity row moves when an alarm starts or stops (S227). */
	".hstate{border:1px solid transparent;border-radius:5px;padding:0 6px;margin:0 -7px}\n"
	".hstate.red{animation:alarm .7s steps(1,end) infinite}\n"
	".hstate.amber{animation:caution 1s steps(1,end) infinite}\n"
	".hal{font-family:var(--mono);font-size:12px;color:var(--ink);text-decoration:none;white-space:nowrap;\n"
	"border:1px solid var(--line);border-radius:6px;padding:2px 9px}\n"
	".hal[hidden]{display:none}\n"
	".hal.red{animation:alarm .7s steps(1,end) infinite}\n"
	".hal.amber{animation:caution 1s steps(1,end) infinite}\n"
	".hal:focus-visible{outline:2px solid var(--accent);outline-offset:2px}\n"
	"@media (prefers-reduced-motion:reduce){\n"
	".hstate.red,.hal.red{animation:none;border-color:var(--badhot);box-shadow:0 0 0 3px var(--badhot);background:var(--bads)}\n"
	".hstate.amber,.hal.amber{animation:none;border-color:var(--warnhot);box-shadow:0 0 0 3px var(--warnhot);background:var(--warns)}}\n"
	".stamp{color:var(--ink3);font-size:12px;font-family:var(--mono)}\n"
	/* S357: a debug or sanitizer build, marked steadily - not an alarm,
	 * but a node left on one must not pass for a release */
	".bflav{border:1px solid var(--warnhot);background:var(--warns);border-radius:5px;padding:0 6px}\n"
	".switch{display:flex;align-items:center;gap:8px;font-size:12px;color:var(--ink3)}\n"
	".btn{font:inherit;font-size:12px;font-family:var(--mono);color:var(--ink3);background:transparent;\n"
	"border:1px solid var(--ink3);border-radius:4px;padding:2px 9px;cursor:pointer}\n"
	".btn:hover{opacity:.75}\n"
	".btn.act{padding:0 6px;margin-left:4px;font-size:11px;vertical-align:baseline}\n"
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
	/* S210: a member in trouble BLINKS - hard on/off (steps(1), no
	 * easing) in a saturated colour, border, tint and a wide halo
	 * together, the worse state at the faster cadence so the two are
	 * told apart at a glance.  The slow eased swell it replaces read as
	 * ambience on a wall of nine cards.  "gone" (S192) does not blink:
	 * it is information, not an alarm. */
	".node.attn{border-color:var(--warnhot);border-left-color:var(--warnhot);background:var(--warnw);\n"
	"animation:caution 1s steps(1,end) infinite}\n"
	"@keyframes caution{0%{box-shadow:0 0 0 3px var(--warnhot),0 0 24px 2px var(--warnhot);border-color:var(--warnhot);background:var(--warns)}\n"
	"50%{box-shadow:0 0 0 1px var(--warn),0 0 0 0 transparent;border-color:var(--warn);background:var(--warnw)}}\n"
	".node.failed,.node.nowit{border-color:var(--badhot);border-left-color:var(--badhot);background:var(--badw);\n"
	"animation:alarm .7s steps(1,end) infinite}\n"
	"@keyframes alarm{0%{box-shadow:0 0 0 3px var(--badhot),0 0 28px 3px var(--badhot);border-color:var(--badhot);background:var(--bads)}\n"
	"50%{box-shadow:0 0 0 1px var(--bad),0 0 0 0 transparent;border-color:var(--bad);background:var(--badw)}}\n"
	/* held bright, no blink */
	"@media (prefers-reduced-motion:reduce){\n"
	"tr.up .upl.bad{animation:none;box-shadow:0 0 0 3px var(--badhot),0 0 24px 2px var(--badhot);background:var(--bads)}\n"
	".node.failed,.node.nowit{animation:none;box-shadow:0 0 0 3px var(--badhot),0 0 24px 2px var(--badhot);background:var(--bads)}\n"
	".node.attn{animation:none;box-shadow:0 0 0 3px var(--warnhot),0 0 20px 2px var(--warnhot);background:var(--warns)}}\n"
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
	/* 10-19 (the operator: the mode line "too small and transparent"): the
	 * fleet's mode as a badge of its own beside the FLEET and COLLECTIONS
	 * headings, the role chips' shape at a heading's size; red when the
	 * collections disagree */
	".modeb{display:inline-block;vertical-align:middle;margin-left:10px;font-family:var(--mono);font-size:12px;font-weight:600;letter-spacing:.08em;text-transform:uppercase;padding:2px 9px;border-radius:5px;border:1px solid var(--accent);color:var(--accent);background:var(--accents)}\n"
	".modeb.bad{color:var(--bad);border-color:var(--bad);background:var(--bads)}\n"
	".modeb[hidden]{display:none}\n"
	/* 10-19 (the operator: "needs a separator under the table"): the stage
	 * row under the fleet table and the create button under the
	 * collections table each start below a rule of their own */
	/* ... and the fleet table keeps a gap from the cards above it */
	"#fleetwrap{margin-top:16px}\n"
	/* 12nx: a collection's records and its Redis copies on lines of their
	 * own, the per-member toggle, and the per-member sub-table */
	".cpl{display:block;font-family:var(--mono);font-size:11px;color:var(--ink3);white-space:nowrap}\n"
	".mtog{font-family:var(--mono);font-size:11px;color:var(--accent);cursor:pointer;margin-left:8px;white-space:nowrap}\n"
	"tr.mrow[hidden]{display:none}\n"
	"table.mt{margin-top:6px;border-collapse:collapse;font-variant-numeric:tabular-nums}table.mt td,table.mt th{padding:3px 14px 3px 0;text-align:left;font-size:12px}table.mt .n{text-align:right}\n"
	"#stagewrap{margin-top:14px;padding-top:12px;border-top:1px solid var(--line)}\n"
	"hr.sep{border:0;border-top:1px solid var(--line);margin:14px 0 12px}\n"
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
	/* S239: the autoscale mode on every row; amber when WARN says auto
	 * would shrink it, red while growth is blocked and keys sit in the
	 * leg */
	"td .as{font-size:11px;font-family:var(--mono);margin-left:7px;padding:0 5px;border:1px solid var(--line);border-radius:4px;color:var(--ink3)}\n"
	"td .as.w{color:var(--warn);border-color:var(--warn);background:var(--warnw)}\n"
	"td .as.b{color:var(--bad);border-color:var(--bad);background:var(--badw)}\n"
	/* S340: a read-through collection's own line under its row - what the
	 * upstream answered, which the row's own counters cannot say */
	"tr.up td{padding-top:0;font-size:12px;color:var(--ink2);white-space:normal}\n"
	"tr.up b{font-family:var(--mono);font-weight:600;color:var(--ink)}\n"
	"tr.up .uk{font-family:var(--mono);font-size:11px;padding:0 5px;margin-right:6px;border:1px solid var(--line);border-radius:4px;color:var(--ink3)}\n"
	/* S382 part 2: lb / f-o beside the autoscale chip; a link per upstream
	 * - the active one raised, a standby dashed, a broken one blinking as a
	 * failed node card does (the same keyframes), a listed master grey and
	 * amber while it serves as the fallback; every colour a palette token */
	"td .as.fo{color:var(--accent);border-color:var(--accent)}\n"
	"tr.up .upl{display:inline-flex;align-items:center;gap:6px;margin:1px 0 1px 14px;padding:1px 8px;border:1px solid var(--line);border-radius:5px;font-family:var(--mono);font-size:11px;color:var(--ink2);background:var(--panel)}\n"
	"tr.up .upl b{font-weight:600;color:var(--ink)}\n"
	"tr.up .upl .ic{display:inline-flex;flex:none;width:11px;height:11px}tr.up .upl .ic svg{display:block;width:11px;height:11px}\n"
	"tr.up .upl.act{border:1.5px solid var(--accent);background:var(--accents);color:var(--ink);box-shadow:var(--emb)}\n"
	"tr.up .upl.act b,tr.up .upl.act .ic{color:var(--accent)}\n"
	"tr.up .upl.sby{border-style:dashed;color:var(--ink3)}\n"
	"tr.up .upl.mst{background:var(--panel2);color:var(--ink3)}\n"
	"tr.up .upl.mst b{color:var(--ink3);font-weight:400}\n"
	"tr.up .upl.emg{border:1.5px solid var(--warnhot);background:var(--warns);color:var(--warn);box-shadow:var(--emb)}\n"
	"tr.up .upl.emg b{color:var(--warn)}\n"
	"tr.up .upl.bad{color:var(--bad);animation:alarm .7s steps(1,end) infinite}\n"
	"tr.up .upl.bad b{color:var(--bad)}\n"
	"tr.up .upl .bd{font-family:var(--sans);font-size:10px;padding:0 5px;border-radius:4px;background:var(--accent);color:var(--panel)}\n"
	"tr.up .upl.emg .bd{background:var(--warnhot);color:#1b1300}\n"
	"tr.up .upl .tg{font-family:var(--sans);font-size:10px;padding:0 5px;border-radius:4px;border:1px solid currentColor;color:var(--ink3)}\n"
	"tr.up .upl .tg.ch,tr.up .upn{color:var(--warn)}\n"
	"tr.up .upl .sh{display:inline-block;width:34px;height:3px;background:var(--line);border-radius:2px;overflow:hidden;vertical-align:middle;margin-right:4px}\n"
	"tr.up .upl .sh i{display:block;height:100%;background:var(--ok)}\n"
	/* 0.6.2: the writes row's table by command - its own size, not the
	 * page's 100%/620px tables (with its indent it overflowed the card) */
	/* 0.6.2: a full-width row's text no longer sizes the columns - its
	 * max-content (a read-through line runs to 2,000 px) was spread over
	 * them and squeezed the name column, the one that may wrap: a
	 * collection's "lb" fell to a second line at 1280 px.  Its box is
	 * 0 wide for the sizing and as wide as the row for the text */
	"tr.up>td>.upw{width:0;min-width:100%}\n"
	/* 0.6.2: a read-through collection and the rows under it read as ONE
	 * group - an accent bar down its left edge, 3 px apart from the
	 * collections before and after it - and each row's tag names it:
	 * read as a table footer, its replicas and writes looked like the
	 * whole node's */
	"tr.grp>td:first-child,tr.up>td{box-shadow:inset 3px 0 0 var(--accent)}\n"
	"tr.grp>td,tr.up+tr:not(.up)>td{border-top:3px solid var(--line)}\n"
	"tbody tr.grp:first-child>td{border-top:0}\n"
	"tr.up table.wtc{width:auto;min-width:0;margin:3px 0 2px 18px;font-size:12px}\n"
	"tr.up table.wtc td{padding:1px 0 1px 18px;border:0;text-align:right}\n"
	"tr.up table.wtc td:first-child{padding-left:0;text-align:left}\n"
	"tr.up table.wtc tr.h td{color:var(--ink3)}\n"

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
	/* The track's left edge must not depend on the text beside it.  .hit
	 * right-aligns [track][text], and the text block is as wide as its
	 * longer line - so "life 0.0%" (9 characters) put its row's bar one
	 * character right of rows reading "life 18.1%" or "never read" (10).
	 * The longest that line can be is "5 min 100.0%": 12ch in its own
	 * font, right-aligned inside, and every row's bar starts in the same
	 * place whatever it says.  Scoped to .hit so the fleet figures' bare
	 * td .hl keeps its natural width.  (0.6.1: the captions had outgrown
	 * the 11ch kept for "life 100.0%" - "no reads in 5 min", 17, put that
	 * row's bar six characters left of the others; it reads "5 min:
	 * none" now, and pagejstest holds every caption to this width.) */
	".hit .hl{min-width:12ch;text-align:right}\n"
	/* S342: the collections table fits its panel at 1280 px and up.  On
	 * PROD (2026-10-07, ~1900 px window) it was 1,377 px in a 1,238 px
	 * panel at 1280 and SIZES sat under a scrollbar.  Measured in
	 * headless Chromium on the live page: tighter cells, a 12 px
	 * table and 10 px "here" lines, a 40 px rate track, and the name
	 * cell may wrap - the chips drop under the name first, and a long
	 * name (up to S343's 32 characters) breaks only when the row
	 * would otherwise overflow.  The number columns cannot shrink,
	 * so that is the one give the table has; minimum width 1,066 px
	 * with any name, 172 px spare at 1280. */
	".coltab{font-size:12px}\n"
	".coltab th,.coltab td{padding:7px 8px}\n"
	".coltab th{letter-spacing:.05em}\n"
	".coltab td:first-child{white-space:normal}\n"
	".coltab td .nm{overflow-wrap:anywhere}\n"
	".coltab td .as,.coltab td .md{display:inline-block}\n"
	".coltab .hit .track{width:40px}\n"
	".coltab .hit .hl,.coltab td .hl{font-size:10px}\n"
	".dim{color:var(--ink3)}\n"
	".plane{display:grid;gap:9px;grid-template-columns:repeat(auto-fit,minmax(232px,1fr))}\n"
	".pc{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:10px 12px}\n"
	/* S214: short cards STACKED in one grid cell, so a pair stands beside a
	 * tall card instead of each leaving half a row empty.  The stack is one
	 * child of the plane - sizePlane() measures and orders it as one - and
	 * its last card takes the slack when the row stretches it, so the
	 * bottoms line up with the neighbour's.  WHICH cards share a cell is
	 * the GROUPS table below, not the order the cards are written in. */
	".stk{display:flex;flex-direction:column;gap:9px}\n"
	".stk>.pc:last-child{flex:1}\n"
	".pc h3{margin:0 0 7px;font-size:10.5px;letter-spacing:.09em;text-transform:uppercase;\n"
	"color:var(--ink3);font-weight:600}\n"
	".kv{display:flex;justify-content:space-between;gap:12px;padding:2px 0;font-size:12.5px}\n"
	/* 0.6.2: a value keeps its width up to 62% of the row and wraps only
	 * past that - "5.0 MB" broke over two lines beside a long label, in a
	 * card 206 px wide; the label is the one that wraps */
	".kv>span:last-child{flex-shrink:0;max-width:62%;text-align:right}\n"
	/* 0.6.2: a label may shrink and, as a last resort, break inside a
	 * word - a 32-character collection name with no hyphen ran 80 px
	 * past the memory budget card */
	".kv>span:first-child{color:var(--ink2);min-width:0;overflow-wrap:anywhere}\n"	/* S165: an unknown-commands row stays on ONE line in the narrowest
	 * card - a long name is cut with an ellipsis (the whole of it, the
	 * dialect and the rest are in the hover) and the value never wraps.
	 * The slow log and slowest-calls cards clip the same way: a key has
	 * no space to wrap at, and it pushed the time off the card. */
	".pc.unk .kv>span:first-child,.pc.clip .kv>span:first-child{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
	".pc.unk .kv>span:last-child,.pc.clip .kv>span:last-child{white-space:nowrap;font-variant-numeric:tabular-nums}\n"

	/* 0.6.2: the row's own two cells (>) - "span:last-child" also caught
	 * a span nested last inside a label and set it in mono */
	".kv>span:last-child{font-family:var(--mono);font-variant-numeric:tabular-nums}\n"
	".kv .sm{font-size:11px}\n"
	".kv.w span:last-child{color:var(--warn)}\n"
	/* S352b: and the colour of trouble, for a critical alert */
	".kv.b span:last-child{color:var(--bad)}\n"
	/* S131: the same warn colour on a table cell.  .w existed only
	 * for the key/value cards, so a flagged td rendered as an
	 * ordinary one - a flag nobody can see is not a flag. */
	"td.w{color:var(--warn)}\n"
	"td.b{color:var(--bad);font-weight:600}\n"
	/* S185: the commands table is folded to its slowest verb until asked */
	".cmdfold tbody tr.cx{display:none}.cmdfold.open tbody tr.cx{display:table-row}\n"
	/* S186: the reason a node is not ready, at the foot of its card */
	".node .why{margin-top:6px;font-size:11px;line-height:1.35;color:var(--warn)}\n"
	".node.gone{border-color:var(--idles);background:transparent;opacity:.62}\n"
	".node.gone .why{color:var(--ink2)}.st.goner{background:var(--idles);color:var(--ink2)}\n"
	/* S293: a member that went silent - gone, but not calm */
	".node.gone.silent{border-color:var(--warn);opacity:.9}.node.gone.silent .why{color:var(--warn)}\n"
	".st.lost{background:var(--warns);color:var(--warn)}\n"
	".node.failed .why,.node.nowit .why{color:var(--bad)}\n"
	"#cmdtog{cursor:pointer;border-bottom:1px dotted var(--ink2)}#cmdtog:hover{color:var(--ink)}\n"
	/* 0.6.2: the slow log and the alerts fold like the commands table;
	 * the commands table sorts by its slowest call or by calls; a long
	 * command or alert text is cut at the cell, whole in its hover */
	".foldtog{cursor:pointer;border-bottom:1px dotted var(--ink2)}.foldtog:hover{color:var(--ink)}\n"
	"#cmdwrap th.srt{cursor:pointer}#cmdwrap th.srt:hover,#cmdwrap th.srt.on{color:var(--ink)}#cmdwrap th.srt.on:after{content:' \\25be'}\n"
	"td.sx,th.sx{width:60%;max-width:0;overflow:hidden;text-overflow:ellipsis;text-align:left}\n"
	"h2[hidden],.scroll[hidden]{display:none}\n"
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
	"<div class=brand><svg class=mark viewBox=\"0 1 32 32\" aria-hidden=\"true\">"
	"<line class=ink x1=16 y1=9 x2=16 y2=27 stroke-width=1.5 stroke-linecap=round />"
	"<line class=ink x1=9 y1=13 x2=23 y2=13 stroke-width=1.5 stroke-linecap=round />"
	"<path class=ink d=\"M16 28 Q10 29 7 24\" fill=none stroke-width=1.5 stroke-linecap=round />"
	"<path class=ink d=\"M16 28 Q22 29 25 24\" fill=none stroke-width=1.5 stroke-linecap=round />"
	"<rect class=found x=13.5 y=3 width=5 height=5 rx=1.5 />"
	"<rect class=kept x=3 y=20 width=5 height=5 rx=1 />"
	"<rect class=kept x=24 y=20 width=5 height=5 rx=1 />"
	"<rect class=kept x=13.5 y=26 width=5 height=5 rx=1 />"
	"</svg>perfcached</div>\n"
	"<div class=idbits id=idbits></div>\n"
	"<a id=hfl class=hal href=\"#fleet\" hidden></a>\n"
	"<a id=hal class=hal href=\"#fleet\" title=\"hosts in trouble - go to the fleet\" hidden></a>\n"
	"<div class=ctl><div class=switch><svg class=cmp viewBox=\"0 1 32 32\" aria-hidden=\"true\"><circle class=ink cx=16 cy=17 r=11 fill=none stroke-width=1.5 /><path class=inkf d=\"M16 17 L18.5 14.5 L26 17 L18.5 19.5 L16 27 L13.5 19.5 L6 17 L13.5 14.5 Z\" /><path class=north d=\"M16 7 L18.5 14.5 L16 17 L13.5 14.5 Z\" /><rect class=found x=12.75 y=1.5 width=6.5 height=6.5 rx=1.8 /><rect class=kept x=24.5 y=13.75 width=6.5 height=6.5 rx=1.2 /><rect class=kept x=12.75 y=24.5 width=6.5 height=6.5 rx=1.2 /><rect class=kept x=1 y=13.75 width=6.5 height=6.5 rx=1.2 /></svg><select id=nodesel aria-label=\"viewing\" title=\"viewing - go to another node\"></select></div>\n"
	"<button id=rst class=btn title=\"start this node's running totals again; open connections, entries and memory are untouched\">reset stats</button>\n"
	"<div class=stamp id=age></div></div>\n"
	"</header>\n"
	"<div id=alert></div>\n"
	"<h2>this node</h2>\n"
	"<div class=metrics id=metrics></div>\n"
	/* S160: this node's connections, opened from the clients strip.
	 * Read-only by decision: CLIENT KILL on the RESP door is the
	 * auditable, secret-gated way; the page never grows an admin API. */
	"<div class=clpanel id=clpanel hidden><div class=clhead><svg class=cmp viewBox=\"0 0 32 32\" aria-hidden=\"true\"><circle class=ink cx=16 cy=17 r=11 fill=none stroke-width=1.5 /><path class=inkf d=\"M16 17 L18 15 L25 17 L18 19 L16 26 L14 19 L7 17 L14 15 Z\" /><path class=north d=\"M16 8 L18 15 L16 17 L14 15 Z\" /><rect class=found x=13.5 y=3 width=5 height=5 rx=1.5 /><rect class=kept x=24 y=14.5 width=5 height=5 rx=1 /><rect class=kept x=13.5 y=24 width=5 height=5 rx=1 /><rect class=kept x=3 y=14.5 width=5 height=5 rx=1 /></svg><input id=clfilter placeholder=\"filter by address or name\">\n"
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
	"<h2>fleet <span class=modeb title=\"the cluster mode every member runs - one mode for the whole fleet\" hidden></span> <span class=hint>as this node sees it</span></h2>\n"
	"<p class=note>Every node serves its own view. <b>If two nodes disagree about the\n"
	"membership, that disagreement is a partition and this is where you see it.</b>\n"
	"Anything not ready is pinned first, then the master, then the rest by id.</p>\n"
	"<div class=legend>\n"
	"<span><i style=\"background:var(--ok)\"></i>ready</span>\n"
	"<span><i style=\"background:var(--warn)\"></i>starting / recovering / draining / stalled / healing</span>\n"
	"<span><i style=\"background:var(--bad)\"></i>failed</span>\n"
	"<span><i style=\"background:var(--idle)\"></i>unknown</span>\n"
	"<span class=dim id=density></span></div>\n"
	"<div id=fleet class=\"fleet cf\"></div>\n"
	/* S410: under the dashboard tool, the fleet in one table - each
	 * member's version, install mode, staged builds and its collections'
	 * entries side by side (the tool's /fleet) */
	"<div id=fleetwrap hidden><div class=scroll><table class=coltab id=fleettab></table></div></div>\n"
	/* S411 part 2: a build staged on the selected member from the page -
	 * the file is posted whole, the daemon verifies and keeps it */
	"<div id=stagewrap class=upw hidden><span class=uk>stage a build</span> on <b id=stagenode></b>: <input type=file id=stagefile> version <input id=stagever size=12 placeholder=\"0.7.0\"> signature <input id=stagesig size=22 placeholder=\"128 hex, optional\"> <button id=stagebtn class=btn>stage</button> <span id=stagemsg class=upn></span></div>\n"
	"<h2>collections <span class=s>counters: last 5 min</span> <span class=modeb title=\"the cluster mode every member runs - one mode for the whole fleet\" hidden></span> <span class=hint id=colmode></span></h2>\n"
	"<div class=scroll><table class=coltab><thead><tr><th>collection</th><th class=n>entries</th>\n"
	"<th class=n>buckets</th><th class=n>load</th>\n"
	/* S131: the leg is where a table that has stopped growing puts
	 * everything, on a chain per hash bucket under one lock.  Load
	 * alone does not say it, and the leg was reported nowhere at
	 * all until now. */
	"<th class=n title=\"records in the overflow leg rather than in a bucket's slots: near zero while a table keeps up with its load factor, everything once it stops growing\">in leg</th>\n"
	/* S148: the column and the headline card both said "hit rate" and
	 * meant different things.  Both are windowed now; the title says
	 * what the window is and what the figure under it means. */
	"<th title=\"share of client lookups that found the key, over the last 30 s of this page's own polls (as long as the page is old, until then) - the same window the &quot;hit rate (30 s)&quot; card uses.  &quot;5 min&quot; underneath is the ratio over the last five minutes, kept by the daemon (S351).  Both count LOOKUPS, not keys, so one hot key can carry the figure - read it beside entries, not alone.  Client lookups only (S151): peer pulls and recovery probes are excluded.\">hit rate <span class=s>(30 s)</span></th>\n"
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
	/* S412: under the dashboard tool the page creates collections too */
	"<hr class=sep id=newcolsep hidden><button id=newcol class=btn hidden title=\"create a collection on this fleet - asks the enable secret once per page\">+ new collection</button>\n"
	/* S160, widened 2026-09-20: the slowest calls were a card in the
	 * plane grid, where the figures took the row's width and the command
	 * name - the left span - was squeezed to nothing, so every row read
	 * as numbers with no verb.  A table of its own, the collections
	 * shape: it scrolls sideways inside its container on a phone, and
	 * the name is a column that cannot collapse. */
	"<h2>commands <span class=s>last 5 min</span> <span class=hint id=cmdtog role=button tabindex=0>the slowest verb - click for all</span></h2>\n"
	"<div class=\"scroll cmdfold\" id=cmdwrap><table><thead><tr><th>command</th><th class=\"n srt\" id=cmdbycalls title=\"sort by calls, the busiest first\">calls</th>\n"
	"<th class=n title=\"total time in the verb divided by its calls: what it usually costs, and blind to a single slow call\">mean</th>\n"
	"<th class=n>p50</th><th class=n>p99</th><th class=n>p99.9</th>\n"
	"<th class=\"n srt on\" id=cmdbyslow title=\"the slowest single call this verb has taken - exact, not a bucket bound (S183), and the column the table is sorted on unless calls is clicked.  The percentiles are read from a histogram of eight buckets per octave and interpolated inside the bucket, so they estimate the value rather than name a power of two above it.\">slowest call</th></tr></thead>\n"
	"<tbody id=cmds></tbody></table></div>\n"
	"<h2>slow log <span class=s>newest first</span> <span class=\"hint foldtog\" id=sltog role=button tabindex=0>empty</span></h2>\n"
	"<div class=\"scroll cmdfold\" id=slwrap><table><thead><tr><th>when</th><th class=sx>command</th><th class=n>took</th><th>client</th></tr></thead>\n"
	"<tbody id=slow></tbody></table></div>\n"
	/* S397: the hot keys, every collection's, heaviest share first - folded
	 * to the hottest like the slow log */
	"<h2>hot keys <span class=s>1 request in 64 sampled, recent minutes</span> <span class=\"hint foldtog\" id=hktog role=button tabindex=0>none</span></h2>\n"
	"<div class=\"scroll cmdfold\" id=hkwrap><table><thead><tr><th>collection</th><th class=sx>key</th>\n"
	"<th class=n title=\"of this collection's sampled requests - a key at a high share pins the one thread that serves it\">share</th>\n"
	"<th class=n title=\"samples counted for the key, and how many of them could belong to the key it replaced (space-saving: the true count lies between)\">samples</th></tr></thead>\n"
	"<tbody id=hot></tbody></table></div>\n"
	"<h2 id=alh>alerts <span class=hint id=alsum></span> <span class=\"hint foldtog\" id=altog role=button tabindex=0>nothing yet</span></h2>\n"
	"<div class=\"scroll cmdfold\" id=alwrap><table><thead><tr><th>when</th><th>level</th><th>what</th><th class=sx>alert</th><th>by mail</th></tr></thead>\n"
	"<tbody id=alrt></tbody></table></div>\n"
	"<h2>cluster plane <span class=s>counters: last 5 min</span></h2>\n"
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
	"var HP=(location.port||\"8080\"),last=0,H={},HMAX=100,POLL=3000,P=null,MISS=0;\n"
	/* 12no: served by perfcached-dashboard, the page is told so by GET
	 * /session (the daemon has no such route); ?node=N then names the
	 * member every read goes to, and the actions appear.  Served by the
	 * daemon, TOOL stays null and nothing below changes. */
	"var TOOL=null,SESSTOK=null,NODEQ=((location.search||\"\").match(/[?&]node=(\\d+)/)||[])[1]||\"\";\n"
	"function qn(u){return NODEQ?u+(u.indexOf(\"?\")<0?\"?\":\"&\")+\"node=\"+NODEQ:u;}\n"
	"function abtn(op,col,lab){return ' <button class=\"btn act\" data-op=\"'+op+'\" data-col=\"'+esc(col)+'\">'+lab+'</button>';}\n"
	/* S412: the DDL buttons on a collection's row (the tool only) */
	"function dbtn(op,col,lab,ent){return ' <button class=\"btn act ddl\" data-op=\"'+op+'\" data-col=\"'+esc(col)+'\" data-ent=\"'+(ent||0)+'\">'+lab+'</button>';}\n"
	/* S432: the RESP DB index(es) a collection answers on, from stats'
	 * resp.map - a runtime map (perfcli map) starred, with its unmap */
	"var RM={};\n"
	"function rmb(n){var a=RM[n]||[],o='',i,e,rt;for(i=0;i<a.length;i++){e=a[i];rt=e.source==='runtime';\n"
	"o+='<span class=md title=\"RESP DB index '+e.index+(rt?', mapped at runtime (perfcli map) - it outranks resp_collections':e.source==='open'?', the collection of that name (resp_collections lists none)':', from resp_collections')+'\">db '+e.index+(rt?'*':'')+'</span>'+(TOOL&&rt?dbtn('unmap',String(e.index),'unmap',0):'');}return o;}\n"
	/* S148: the per-collection column was the LIFETIME ratio while the
	 * headline beside it was windowed - two numbers, the same words.
	 * PC holds the previous sample per collection NAME so the column
	 * can be differenced exactly as P differences the totals.  PRA is
	 * the reset_at both baselines were taken against: reset-stats
	 * rebases the counters DOWNWARD (pcache_ht_totals subtracts
	 * ht->base), so without this a reset reads as a negative delta. */
	"var PC={},PRA=null;\n"
	/* 0.6.1: "now" was ONE poll - 3 s - and a quiet collection (one read
	 * every few seconds: a read-through of a ban list) flipped between
	 * "no reads" and "0.0%" on every poll (the operator, 245-247).  SW
	 * keeps this page's own samples - the totals and each collection's
	 * pair - and the card and every row difference against the newest
	 * sample at least SWIN old, so "now" is the last 30 s.  Until the
	 * page has been open that long it is as long as the page is old. */
	"var SW=[],SWIN=30000,NF={};\n"
	/* 0.6.2: the folds' state, read by the handlers outside drawStats */
	"var CMDSORT='slow',CMDROWS=null,SLN=0,ALN=0,HKN=0;\n"
	/* 12nx: the dashboard's /fleet - every member's collections (FLM) and
	 * the fleet's distinct keys from the merged sketches (FLD); this node's
	 * fleet figure and basis per collection (COLF, COLB); the open
	 * per-member rows (COLOPEN) */
	"var FLM=null,FLD=null,COLF={},COLB={},COLOPEN={};\n"
	/* S214: the plane's order is DECIDED ONCE and kept.  Sorting the cells
	 * by measured height on every poll moved a cell whenever its height
	 * crossed a step - a figure in memory budget wrapping onto a second
	 * line, a conditional row appearing - and padding the traffic-driven
	 * cards only removed the largest of those.  The order is re-decided
	 * when the SET of cells changes (one appears or goes) or the plane's
	 * width does (another column count, so everything wraps differently),
	 * and at no other time.  Pure, so the page rig can drive it: @keys name
	 * the cells, @hs are their heights, @w the plane's width. */
	"var PORD=null,PW=-1;\n"
	"function planeOrder(keys,hs,w){var i,idx,same=PORD!==null&&PW===w&&PORD.length===keys.length;\n"
	"if(same)for(i=0;i<keys.length;i++)if(PORD.indexOf(keys[i])<0){same=false;break;}\n"
	"if(!same){idx=keys.map(function(k,j){return j;});idx.sort(function(a,b){return hs[b]-hs[a]||a-b;});\n"
	"PORD=idx.map(function(j){return keys[j];});PW=w;}return PORD;}\n"
	/* S160: the last members list (for the fleet-wide clients figure),
	 * whether the connections panel is open, and its last fetch */
	"var MEM=null,CLOPEN=false,CLD=null,FLINFO=null,FLCOPIES=null;\n"
	/* S391: the upstream rows' icons, drawn rather than typed.  As glyphs
	 * (U+2298 and the rest) they came from whichever font held them, on
	 * that font's baseline: the master's circle-slash sat low and left of
	 * the row's centre (the operator, PROD 2026-10-13: "icon is
	 * off-center").  On a 12x12 box, stroked in the row's colour, they
	 * sit where the box does */
	"var UIC=(function(){var a='<svg viewBox=\"0 0 12 12\" aria-hidden=true fill=none stroke=currentColor stroke-width=1.4 stroke-linecap=round stroke-linejoin=round>',z='</svg>';\n"
	"return{ban:a+'<circle cx=6 cy=6 r=4.6 /><path d=\"M2.8 9.2L9.2 2.8\" />'+z,x:a+'<path d=\"M3 3L9 9M9 3L3 9\" />'+z,\n"
	"warn:a+'<path d=\"M6 1.7L10.8 10.2H1.2Z\" /><path d=\"M6 5V7.2\" /><circle cx=6 cy=8.7 r=.4 fill=currentColor />'+z,\n"
	"pen:a+'<path d=\"M2.5 9.5L3 7.4 8 2.4 9.6 4 4.6 9Z\" />'+z,play:a+'<path d=\"M3.5 2.5L9.5 6 3.5 9.5Z\" fill=currentColor />'+z,\n"
	"pause:a+'<path d=\"M4.5 2.7V9.3M7.5 2.7V9.3\" />'+z};})();\n"
	/* S395: how far this node's open reconcile has got, from its stats -
	 * the fast path asking, or the walk's share, rate and time left.
	 * RCL is this node's, for its own card; a peer's progress is not in
	 * the keepalive (0.6.5's settings group can carry it) */
	"var RCL='';\n"
	"function recline(c){if(!c||!c.reconcile_open)return '';\n"
	"if(!c.reconcile_walked)return c.reconcile_fast_why?'starting the walk':'asking a witness for the deletes since its snapshot';\n"
	"var p=c.reconcile_walk_total?Math.min(99,Math.floor(100*c.reconcile_walked/c.reconcile_walk_total)):0;\n"
	"return 'walked '+p+'% \u00b7 '+nf(c.reconcile_rate)+' keys/s'+(c.reconcile_eta_s>=0?' \u00b7 about '+dur(c.reconcile_eta_s)+' left':'');}\n"
	/* S205: every cache-fed string passes here before innerHTML - keys,
	 * values, client names, command names.  The single quote is escaped
	 * too, so a value is safe in a single-quoted attribute as well as a
	 * double-quoted one; xsstest stores hostile keys and checks. */
	"function esc(s){return String(s).replace(/[&<>\"']/g,function(c){\n"
	"return {\"&\":\"&amp;\",\"<\":\"&lt;\",\">\":\"&gt;\",\"\\\"\":\"&quot;\",\"'\":\"&#39;\"}[c];});}\n"
	/* numbers are grouped the SI way - a narrow no-break space between
	 * groups of three, from five digits up - and never by the browser's
	 * locale: toLocaleString wrote 10134 as "10,134" here and "10.134" in
	 * a German browser, and either reads as ten point something */
	/* S239: the row's autoscale chip.  RED whenever the mode blocks
	 * growth and the table has too many keys for its buckets - past
	 * grow_at_pct (auto would grow it: the target is above its size) or
	 * already spilling into the leg - under off as much as warn (the
	 * operator: "in off mode collection should also turn red when
	 * there's too many keys"); every lookup that reaches the leg takes
	 * one lock for the whole table.  AMBER: warn, and auto would shrink. */
	"function asc(x){var m=x.autoscale;if(!m)return '';var t=x.autoscale_target_log2||0,up=t&&Math.pow(2,t)>x.buckets,\n"
	"g=m==='auto'||m==='floor',red=!g&&(x.overflow>0||up),sh=m==='warn'&&t&&Math.pow(2,t)<x.buckets;\n"
	"return '<span class=\"as'+(red?' b':sh?' w':'')+'\" title=\"'+(red?'too many keys for its buckets and growth is blocked (autoscale = '+m+')'+\n"
	"(up?': auto would grow it to 2^'+t:'')+(x.overflow>0?'; '+nf(x.overflow)+' records are in the overflow leg, one lock for the whole table':'')+\n"
	"' - resize it, or set autoscale auto':\n"
	"sh?'autoscale = warn: auto would shrink it to 2^'+t+' buckets - resize it, or set autoscale auto':\n"
	"'autoscale = '+m+(m==='auto'?': grows and shrinks with its keys':m==='floor'?': grows with its keys, never shrinks below 2^'+(x.autoscale_floor_log2||'?'):\n"
	"m==='warn'?': reports what auto would do, changes nothing':': never resized, never reported'))+'\">'+\n"
	"esc(m)+(red&&up?' \\u2191 2^'+t:sh?' \\u2192 2^'+t:'')+'</span>';}\n"
	"function nf(v){if(v===undefined||v===null)return \"-\";var n=Number(v),p;if(!isFinite(n))return String(v);\n"
	"p=String(Math.round(Math.abs(n)*1000)/1000).split('.');if(p[0].length>4)p[0]=p[0].replace(/\\B(?=(\\d{3})+$)/g,'\\u202f');\n"
	"return (n<0?'-':'')+p[0]+(p[1]?'.'+p[1]:'');}\n"
	/* S151: the page reads the CLIENT figures when the daemon publishes
	 * them, and the all-origin ones from an older daemon.  Same shape,
	 * so nothing below has to know which it got. */
	"function ch(x){return x.hits_client!==undefined?x.hits_client:(x.hits||0);}\n"
	/* S351: every count on this page is the last five minutes, kept by the
	 * DAEMON (win_<field>, winctr.h) - true from the first poll, the same
	 * figure /stats gives a script.  null until the daemon has a baseline;
	 * nf() shows it as a dash. */
	"function wv5(o,k){var v=o?o['win_'+k]:undefined;return v===undefined||v===null?null:+v;}\n"
	"function wch(x){var v=wv5(x,'hits_client');return v!==null?v:wv5(x,'hits');}\n"
	"function wcm(x){var v=wv5(x,'misses_client');return v!==null?v:wv5(x,'misses');}\n"
	"function cm(x){return x.misses_client!==undefined?x.misses_client:(x.misses||0);}\n"
	/* S340: the operator watched the SBC block devices from keys this
	 * collection read through, beside a row reading 0 hits and only
	 * misses - the row counts this fleet's own records; what the upstream
	 * answered was in /stats alone.  Every lookup takes exactly one of
	 * three paths: fetched from the upstream (found or absent), answered
	 * from a fetched copy, or answered absent from the negative cache.
	 * This node's figures over the page's window, like the row's own. */
	"function upmc(x){var u=x.upstream;if(!u||!u.upstreams||u.upstreams.length<2)return '';var f=u.mode==='failover';\n"
	"return '<span class=\"as'+(f?' fo':'')+'\" title=\"'+(f?'failover: one active upstream, the rest standby':'load balance: the fewest fetches in flight')+'\">'+(f?'f/o':'lb')+'</span>';}\n"
	"function upr(x){var u=x.upstream;if(!u)return '';\n"
	"var fo=(wv5(u,'fill_string')||0)+(wv5(u,'fill_hash')||0)+(wv5(u,'fill_json')||0)+(wv5(u,'fill_present')||0),\n"
	"f=wv5(u,'fetches'),a=wv5(u,'absent'),s=wv5(u,'shadow_hits'),g=wv5(u,'negative_hits'),p=wv5(u,'present_hits')||0,\n"
	"er=wv5(u,'errors'),tm=wv5(u,'timeouts'),fn=wv5(u,'fetch_n'),fa=fn>0?wv5(u,'fetch_us_sum')/fn:null;\n"
	/* every lookup ends at the gate as a fetched copy (shadow_hits), an
	 * absent mark (negative_hits) or - an EXISTS, TTL or TYPE - a PRESENT
	 * mark (present_hits, ST2) - a read that waited for a fetch is
	 * dispatched again and counted there too, so the fetches are a SUBSET
	 * of the lookups, never added to them */
	/* S382: a link per upstream (when there are several, or replicas are
	 * asked for): its look says what it is doing (the classes above); its
	 * figures are the last 5 min's.  S384: an allowed perfcached is tagged
	 * as a daisy-chain hop, a refused one blinks with its reason */
	"var us=u.upstreams||[],ur='',uf=0,j,q,qf,fom=u.mode==='failover',emg=false,srn=0,chr=wv5(u,'chained_reads')||0,u0=us.length===1?us[0]:null,dr=u.drain||null,\n"
	/* S385: write-through - the master written to, and what went there */
	"wt=u.write_through||null,wtt=wt&&wt.target?wt.target:null;\n"
	"for(j=0;j<us.length;j++){uf+=wv5(us[j],'fetches')||0;if(us[j].state==='serving')srn++;}\n"
	"if(us.length>1||u.role_required==='replica')for(j=0;j<us.length;j++){q=us[j];qf=wv5(q,'fetches');\n"
	"var qn=wv5(q,'fetch_n'),qa=qn>0?wv5(q,'fetch_us_sum')/qn:null,qe=wv5(q,'errors')||0,cls='',bd='',tx='',ic='',sv=q.state==='serving',fb=q.state==='master fallback';\n"
	"if(fb&&(fom?q.active:!srn)){cls='emg';bd='active \\u00b7 master fallback';emg=true;ic='warn';}\n"
	"else if(wtt&&q.addr===wtt){bd='writes';tx='the master: writes go here, reads never';ic='pen';}\n"
	"else if(fb||q.why==='a master'){cls='mst';tx=fb?'the master: fallback only':'the master: not read (replicas only)';ic='ban';}\n"
	"else if(q.state==='not used'){cls='bad';tx=q.why;ic='x';}\n"
	"else if(fom&&q.active){cls='act';bd='active';ic='play';}\n"
	"else if(fom&&sv){cls='sby';tx='standby';ic='pause';}\n"
	"else if(q.state==='not reached yet'){cls='sby';tx='connecting';}\n"
	"var fig=(!fom&&sv?(uf>0&&qf!==null?'<span class=sh><i style=\"width:'+Math.round(100*qf/uf)+'%\"></i></span>'+Math.round(100*qf/uf)+'% of fetches':'no fetches in 5 min'):'')+\n"
	"(qa!==null&&(sv||fb)?' '+(qa<1000?Math.round(qa)+' \\u03bcs':(qa/1000).toFixed(1)+' ms'):'')+(q.lag_ms!=null&&q.role==='replica'?' \\u00b7 master seen '+Math.round(q.lag_ms/1000)+' s ago':'')+\n"
	"(qe?' \\u00b7 <span style=\"color:var(--bad)\">'+nf(qe)+' errors</span>':'');\n"
	"ur+='<tr class=up><td colspan=13><div class=upw><span class=\"upl'+(cls?' '+cls:'')+'\" title=\"'+esc(q.addr+(q.role?' \\u00b7 '+q.role:'')+' \\u00b7 '+q.state+(q.why?' ('+q.why+')':''))+'\">'+\n"
	"(ic?'<span class=ic>'+UIC[ic]+'</span>':'')+'<b>'+esc(q.addr)+'</b>'+(bd?'<span class=bd>'+esc(bd)+'</span>':'')+(fom&&q.primary?'<span class=tg>primary</span>':'')+\n"
	"(q.perfcached&&q.state!=='not used'?'<span class=\"tg ch\">chained \\u00b7 one hop</span>':'')+(tx?'<span>'+esc(tx)+'</span>':'')+\n"
	"(q.role&&cls!=='mst'?'<span>'+esc(q.role)+'</span>':'')+(fig?'<span>'+fig+'</span>':'')+(q.tracking||q.state==='not used'||cls==='mst'?'':'<span>tracking off</span>')+'</span></div></td></tr>';}\n"
	"return '<tr class=up><td colspan=13><div class=upw><span class=uk>'+esc(x.name)+' \\u00b7 read-through</span>'+(us.length>1?nf(us.length)+' upstreams, '+(fom?'failover - preempt '+(u.preempt?'on \\u00b7 '+u.preempt_hold_s+' s':'off'):'load balance')+(u.role_required==='replica'?', replicas'+(u.master_fallback?' (the master while none serves)':' only'):''):esc(u.host||'?')+':'+esc(u.port||''))+\n"
	"(u0&&us.length===1&&u0.state==='not used'?' <span class=\"upl bad\"><b>not used</b> '+esc(u0.why)+'</span>':'')+(u0&&us.length===1&&u0.perfcached&&u0.state!=='not used'?' <span class=upn>\\u00b7 chained to a perfcached, one hop</span>':'')+\n"
	"(emg?' \\u00b7 <span class=upn>emergency: reading from the master</span>':'')+\n"
	"(chr?' \\u00b7 <span class=upn>read through by another perfcached: '+nf(chr)+' in 5 min, answered from what this node holds - a daisy-chain, never fetched onward</span>':'')+\n"

	"' \\u00b7 tracking '+(u.tracking?'on':'<span style=\"color:var(--bad)\">off</span>')+\n"
	"(u.writes==='refuse'?' \\u00b7 <b>read-only</b>'+(wv5(u,'writes_refused')?' ('+nf(wv5(u,'writes_refused'))+' writes refused)':''):'')+\n"
	/* S385: where the writes go, and the last 5 min of them */
	"(wt?' \\u00b7 <b>write-through</b> \\u2192 '+(wtt?esc(wtt):'<span style=\"color:var(--bad)\">no master ('+esc(wt.state||'?')+') - writes refused</span>')+\n"
	"(wt.json==='no'?' \\u00b7 <span class=upn>the master has no RedisJSON - JSON writes refused</span>':'')+\n"
	"' \\u00b7 5 min: <b>'+nf(wv5(wt,'forwarded'))+'</b> forwarded'+\n"
	"(wv5(wt,'n')>0?(function(a){return ', avg '+(a<1000?Math.round(a)+' \\u03bcs':(a/1000).toFixed(1)+' ms');})(wv5(wt,'us_sum')/wv5(wt,'n')):'')+\n"
	"((wv5(wt,'failed')||0)?' \\u00b7 <span style=\"color:var(--bad)\">'+nf(wv5(wt,'failed'))+' failed</span>':'')+\n"
	"(((wv5(wt,'passed_writes')||0)+(wv5(wt,'passed_reads')||0)+(wv5(wt,'pass_refused')||0))?' \\u00b7 passed through: '+nf(wv5(wt,'passed_writes'))+' writes, '+nf(wv5(wt,'passed_reads'))+' reads'+((wv5(wt,'pass_refused')||0)?', '+nf(wv5(wt,'pass_refused'))+' refused':''):''):'')+\n"
	"' \\u00b7 this node, 5 min: <b>'+nf(s===null||g===null?null:s+g+p)+'</b> lookups = <b>'+nf(s)+'</b> with a value + '+\n"
	"(p?'<b>'+nf(p)+'</b> present, no value read + ':'')+'<b>'+nf(g)+'</b> absent'+\n"
	"' \\u00b7 <b>'+nf(f)+'</b> of them fetched from the upstream ('+nf(fo)+' found, '+nf(a)+' absent'+\n"
	"(fa!==null?', avg '+(fa<1000?Math.round(fa)+' \\u03bcs':(fa/1000).toFixed(1)+' ms'):'')+\n"
	"') \\u00b7 <b>'+nf(u.shadow_entries)+'</b> copies held'+\n"
	"(FLCOPIES&&FLCOPIES.c[x.name]!==undefined?' (the fleet: <b>'+nf(FLCOPIES.c[x.name])+'</b> on '+FLCOPIES.n+' members)':'')+\n"
	"(u.rewarm&&u.rewarm.queued?' \\u00b7 re-warming <b>'+nf(u.rewarm.queued)+'</b> more after a tracking gap':'')+\n"
	"(NF[x.name]!==undefined?' \\u00b7 <b>'+nf(NF[x.name])+'</b> keys written here, never read from the upstream again':'')+\n"
	"(er||tm?' \\u00b7 <span style=\"color:var(--bad)\">'+nf(er)+' errors, '+nf(tm)+' timeouts</span>':'')+\n"
	/* S412 part 2: the drain sweep - its state and figures while it runs
	 * or once it ran, and under the tool a click starts or stops it */
	"(dr&&dr.state&&dr.state!=='idle'?' \\u00b7 drain '+esc(dr.state)+': <b>'+nf(dr.read||0)+'</b> read, <b>'+nf(dr.taken_over||0)+'</b> taken over'+((dr.fleet_had||0)?', '+nf(dr.fleet_had)+' the fleet had':'')+((dr.gone||0)?', '+nf(dr.gone)+' gone':'')+(((dr.wrongtype||0)+(dr.too_big||0))?', '+nf((dr.wrongtype||0)+(dr.too_big||0))+' not holdable':'')+((dr.errors||0)?', <span style=\"color:var(--bad)\">'+nf(dr.errors)+' failed</span>':''):'')+\n"
	"(TOOL&&u0&&u0.state!=='not used'?(dr&&dr.state==='running'?abtn('drain-stop',x.name,'stop drain'):abtn('drain',x.name,'drain')):'')+'</div></td></tr>'+ur+wtr(wt,x.name);}\n"
	/* 0.6.2: what went to the master - by outcome, by command, and the
	 * last errors with what Redis said (5 min, the dashboard's window) */
	"function wtus(a){return a<1000?Math.round(a)+' \\u03bcs':(a/1000).toFixed(1)+' ms';}\n"
	"function wtr(wt,nm){if(!wt)return '';\n"
	"var ap=wv5(wt,'applied')||0,na=wv5(wt,'not_applied')||0,re=wv5(wt,'redis_errors')||0,fl=wv5(wt,'failed')||0,rf=wv5(wt,'refused')||0,\n"
	"rv=wv5(wt,'read_back_values')||0,rg=wv5(wt,'read_back_gone')||0,rr=wv5(wt,'read_back_released')||0,mx=wt.win_max_us||0,\n"
	"bc=(wt.by_command||[]).slice(),le=wt.last_errors||[],h,t='',x='',i,r;\n"
	"h='<span class=uk>'+esc(nm)+' \\u00b7 writes</span>to the master, 5 min: <b>'+nf(ap)+'</b> applied'+\n"
	"(na?' \\u00b7 '+nf(na)+' not applied (its condition said no)':'')+\n"
	"(re?' \\u00b7 <span style=\"color:var(--bad)\">'+nf(re)+' Redis errors</span>':'')+\n"
	"(fl?' \\u00b7 <span style=\"color:var(--bad)\">'+nf(fl)+' failed</span>':'')+\n"
	"(rf?' \\u00b7 <span style=\"color:var(--warn)\">'+nf(rf)+' refused here</span>':'')+\n"
	"(mx?' \\u00b7 slowest '+wtus(mx):'')+(wt.queued?' \\u00b7 '+nf(wt.queued)+' queued'+(wt.queue_max?' of '+nf(wt.queue_max):''):'')+\n"
	/* S403: the writer thread's share of the last five minutes */
	"(wt.writer_busy_pct!=null?' \\u00b7 writer busy '+(wt.writer_busy_pct>=75?'<span style=\"color:var(--warn)\">'+wt.writer_busy_pct+'%</span>':'<b>'+wt.writer_busy_pct+'%</b>'):'')+\n"
	/* AR2: the bound and the deadline at work - the master slower than the writes */
	"((wv5(wt,'queue_full')||0)?' \\u00b7 <span style=\"color:var(--bad)\">'+nf(wv5(wt,'queue_full'))+' refused: the queue was full</span>':'')+\n"
	/* AR1: a node that does not follow other clients' changes in Redis */
	"(wt.bcast&&wt.bcast!=='on'?' \\u00b7 <span style=\"color:var(--bad)\">other clients\\' changes in Redis not followed (broadcast tracking '+esc(wt.bcast)+')</span>':'')+\n"
	"((wv5(wt,'queue_expired')||0)?' \\u00b7 <span style=\"color:var(--bad)\">'+nf(wv5(wt,'queue_expired'))+' waited past the timeout - not applied</span>':'')+\n"
	/* AR4: keys the fleet owned before write-through, and the upfill sweep */
	"(wt.fleet_owned?' \\u00b7 <span style=\"color:var(--warn)\">'+nf(wt.fleet_owned)+' keys the fleet owned before write-through - '+(TOOL?'':'perfcli ')+'upfill writes them to Redis</span>':'')+\n"
	/* S400: a check sweep compares and writes nothing - its figures are
	 * the comparison's, not the writer's */
	"(wt.upfill&&wt.upfill.state==='running'?' \\u00b7 '+(wt.upfill.mode==='check'?'check: '+nf((wt.upfill.compared_text||0)+(wt.upfill.compared_walked||0))+' compared, '+nf(wt.upfill.redis_differed||0)+' differed':'upfill: '+nf((wt.upfill.written||0)+(wt.upfill.redis_had||0))+' of '+nf(wt.upfill.found||0)+' done'):'')+\n"
	/* 12no (S407): under the dashboard tool the sweep is a click - check
	 * compares with Redis writing nothing, upfill writes, stop ends the
	 * one running; the tool asks the enable secret once per page */
	"(TOOL?(wt.upfill&&wt.upfill.state==='running'?abtn('stop',nm,'stop'):abtn('check',nm,'check')+abtn('upfill',nm,'upfill')):'')+\n"
	"(rv+rg+rr?' \\u00b7 read back: '+nf(rv)+' values, '+nf(rg)+' gone'+(rr?', '+nf(rr)+' left to Redis':''):'');\n"
	"function wsum(r){return (wv5(r,'applied')||0)+(wv5(r,'not_applied')||0)+(wv5(r,'redis_errors')||0)+(wv5(r,'failed')||0);}\n"
	"bc.sort(function(a,b){return wsum(b)-wsum(a);});\n"
	"for(i=0;i<bc.length;i++){r=bc[i];var w=wsum(r),e=wv5(r,'redis_errors')||0,f=wv5(r,'failed')||0,n=wv5(r,'n')||0;if(!w)continue;\n"
	"t+='<tr><td><b>'+esc(r.name)+'</b>'+(r.passed?' <span class=upn>passed</span>':'')+'</td><td>'+nf(w)+'</td><td>'+nf(wv5(r,'applied')||0)+'</td>'+\n"
	"'<td>'+((wv5(r,'not_applied')||0)?nf(wv5(r,'not_applied')):'')+'</td>'+\n"
	"'<td'+(e?' style=\"color:var(--bad)\"':'')+'>'+(e?nf(e):'')+'</td><td'+(f?' style=\"color:var(--bad)\"':'')+'>'+(f?nf(f):'')+'</td>'+\n"
	"'<td>'+(n?wtus((wv5(r,'us_sum')||0)/n):'')+'</td></tr>';}\n"
	"if(t)t='<table class=wtc><tr class=h><td>command</td><td>writes</td><td>applied</td><td>not applied</td><td>Redis errors</td><td>failed</td><td>avg</td></tr>'+t+'</table>';\n"
	"else t=' \\u00b7 no writes in 5 min';\n"
	"for(i=0;i<le.length;i++){r=le[i];\n"
	"x+='<div class=wte style=\"margin-left:18px'+(r.kind==='refused'?';color:var(--warn)':';color:var(--bad)')+'\">'+dur(Math.max(0,r.age_s||0))+' ago \\u00b7 '+\n"
	"(r.kind==='redis'?'Redis error':r.kind==='refused'?'refused here':'failed')+' \\u00b7 <b>'+esc(r.command||'?')+'</b>'+(r.key?' '+esc(r.key):'')+' \\u2014 '+esc(r.message)+'</div>';}\n"
	"if(x)x='<div style=\"margin:3px 0 0 18px;opacity:.8\">the last errors, newest first:</div>'+x;\n"
	"return '<tr class=up><td colspan=13><div class=upw>'+h+t+x+'</div></td></tr>';}\n"
	"function cr(x){return x.reach_client!==undefined?x.reach_client:x.reach;}\n"
	"function mb(b){return Math.round((b||0)/1048576);}\n"
	/* a fixed MB scale renders every small arena as a flat 0 MB - two
	 * records held 480 bytes and the page reported nothing at all */
	"function sz(b){if(b===undefined||b===null)return \"\\u2014\";b=+b;\n"
	"if(b<1024)return b+\" B\";\n"
	"if(b<1048576)return (b/1024).toFixed(b<10240?1:0)+\" KB\";\n"
	"if(b<1073741824)return (b/1048576).toFixed(b<10485760?1:0)+\" MB\";\n"
	"return (b/1073741824).toFixed(2)+\" GB\";}\n"
	"function ms(t){if(t===undefined||t===null)return \"\\u2014\";t=+t;return t<1000?t+\" ms\":t<60000?(t/1000).toFixed(1)+\" s\":dur(Math.round(t/1000));}\n"
	"function dur(t){if(t===undefined||t===null)return \"\\u2014\";t=+t;\n"
	"var d=Math.floor(t/86400),h=Math.floor(t%86400/3600),m=Math.floor(t%3600/60);\n"
	"if(d)return d+\"d \"+h+\"h\";if(h)return h+\"h \"+m+\"m\";\n"
	"if(m)return m+\"m \"+(t%60)+\"s\";return t+\"s\";}\n"
	"function get(u,cb){var x=new XMLHttpRequest();x.open(\"GET\",qn(u),true);\n"
	/* S196: the try/catch used to wrap the CALLBACK, so an exception
	 * thrown while RENDERING came back as cb(null) - "this node did
	 * not answer" over a node that had answered perfectly.  A page
	 * bug wore the daemon's clothes for a day.  Only the PARSE is
	 * guarded here; what the callback throws is the callback's. */
	"x.onreadystatechange=function(){if(x.readyState!==4)return;\n"
	"var d=null;if(x.status===200){try{d=JSON.parse(x.responseText);}catch(e){d=null;}}\n"
	"cb(d);};\n"
	"x.send();}\n"
	/* 12no: under the dashboard tool every POST carries the header no
	 * cross-site form can send, a JSON body and the session token; the
	 * daemon's own door gets the bare POST it always got.  A privileged
	 * action asks the enable secret once per page (session) and asks again
	 * when the tool says the session is gone (401). */
	"function post(u,cb,body){var x=new XMLHttpRequest();x.open(\"POST\",qn(u),true);\n"
	"if(TOOL){x.setRequestHeader(\"X-Perfcached-Dashboard\",\"1\");x.setRequestHeader(\"Content-Type\",\"application/json\");\n"
	"if(SESSTOK)x.setRequestHeader(\"X-Perfcached-Session\",SESSTOK);}\n"
	"x.onreadystatechange=function(){if(x.readyState!==4)return;var d=null;try{d=JSON.parse(x.responseText);}catch(e){d=null;}cb(x.status===200,d,x.status);};\n"
	"x.send(TOOL?(body||\"{}\"):null);}\n"
	"function session(then){var s=prompt(\"The enable secret of this fleet - kept in this page only, never stored:\");if(!s)return;\n"
	"post(\"session\",function(ok,d){if(!ok){alert((d&&d.error)||\"the dashboard refused the secret\");return;}SESSTOK=d.token;if(then)then();},JSON.stringify({secret:s}));}\n"
	"function withsess(go){if(TOOL&&!SESSTOK)session(go);else go();}\n"
	/* S411 part 2: the build as the body, its version and signature as headers */
	"function postraw(u,cb,body,ver,sig){var x=new XMLHttpRequest();x.open(\"POST\",qn(u),true);\n"
	"x.setRequestHeader(\"X-Perfcached-Dashboard\",\"1\");x.setRequestHeader(\"Content-Type\",\"application/octet-stream\");x.setRequestHeader(\"X-Perfcached-Version\",ver);if(sig)x.setRequestHeader(\"X-Perfcached-Sig\",sig);\n"
	"if(SESSTOK)x.setRequestHeader(\"X-Perfcached-Session\",SESSTOK);\n"
	"x.onreadystatechange=function(){if(x.readyState!==4)return;var d=null;try{d=JSON.parse(x.responseText);}catch(e){d=null;}cb(x.status===200,d,x.status);};x.send(body);}\n"
	"function stageBuild(){var f=document.getElementById(\"stagefile\").files[0],ver=document.getElementById(\"stagever\").value.trim(),sig=document.getElementById(\"stagesig\").value.trim(),msg=document.getElementById(\"stagemsg\");\n"
	"if(!f){alert(\"choose the build's tarball first\");return;}if(!ver){alert(\"the build's version is needed\");return;}\n"
	"var rd=new FileReader();rd.onload=function(){function go(){msg.textContent=\"staging \"+nf(f.size)+\" bytes...\";\n"
	"postraw(\"stage\",function(ok,d,st){if(st===401){SESSTOK=null;session(go);return;}if(!ok){msg.textContent=\"\";alert((d&&d.error)||\"the node refused the build\");return;}\n"
	"msg.textContent=\"staged \"+ver+(d&&d.sha256?\" - sha256 \"+d.sha256.slice(0,12)+\"...\":\"\")+(d&&d.signer?\" - signed by \"+d.signer:\"\");tick();},rd.result,ver,sig);}withsess(go);};rd.readAsArrayBuffer(f);}\n"
	"function acted(go,msg){return function(ok,d,st){if(TOOL&&st===401){SESSTOK=null;session(go);return;}if(!ok)alert((d&&d.error)||msg);tick();};}\n"
	"function push(k,v){if(v===undefined||v===null)return;\n"
	"if(!H[k])H[k]=[];H[k].push(v);if(H[k].length>HMAX)H[k].shift();}\n"
	/* S194: a COUNTER's movement over the window the page already
	 * keeps - HMAX samples, five minutes.  Every figure on these
	 * cards was cumulative since the daemon started, which is what
	 * Prometheus wants and the opposite of what a dashboard is read
	 * for: "is this happening now".  Gauges - entries, buckets, the
	 * leg, held bytes - stay absolute, because the question there is
	 * "how much is there", and differencing one would be nonsense. */
	/* S243: a window is the counter now minus the oldest sample the page
	 * holds.  A counter that went BACKWARDS was restarted (the node, or
	 * reset_stats): its old samples belong to another run, and
	 * subtracting them showed "pushed on write -2 418 242" until F5
	 * threw them away.  Start the window again from the new run. */
	"function wv(k,v){if(v===undefined||v===null)return v;\n"
	"var a=H[k];if(a&&a.length&&v<a[a.length-1])H[k]=[];push(k,v);\n"
	"a=H[k];return a&&a.length>1?v-a[0]:v;}\n"
	"function winspan(){var a=H['w:pull_sent'];\n"
	"return a&&a.length>1?dur(Math.round((a.length-1)*POLL/1000)):'';}\n"
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
	"var HV={},NCPU=0;\n"
	"function trend(){\n"
	"var defs=[[\"daemon cpu\"+(NCPU?\" \\u00b7 % of \"+NCPU+\" CPU\"+(NCPU===1?\"\":\"s\"):\"\"),\"cpu\",\"%\",1,0,1],[\"arena live\",\"live\",\"\",0,1,0],\n"
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
	/* S192: a member that has left sorts LAST - it needs no attention,
	 * it is there so a restart does not look like a shrinking fleet */
	"var RANK={failed:0,healing:1,stalled:1,draining:1,recovering:1,starting:1,ready:3,gone:4};\n"
	/* the header alarm's colour for a state, as the cards draw it:
	 * failed red; any other state but ready amber; a member that left
	 * is calm unless it went silent (S293, the warning colour) */
	"function sev(st,gr){return !st||st===\"ready\"?\"\":st===\"failed\"?\"red\":st===\"gone\"?(gr===\"silent\"?\"amber\":\"\"):\"amber\";}\n"
	/* the chip names the OTHER hosts in trouble (this node's own state
	 * blinks beside it), worst first as the fleet sorts them; text, not
	 * markup, so a node's strings cannot reach the DOM as HTML */
	/* 0.5.7.1 (DESIGN 12kx): this node's reconcile, beside its state - red
	 * while it has had NO live witness (it waits for its own restart), amber
	 * while a peer's reconcile has this node as its LAST witness (do not
	 * restart it now).  Text through esc(), numbers only. */
	"function recChip(c){if(!c)return \"\";\n"
	"if(c.reconcile_no_witness_s>0)return \"<span class=\\\"hstate red\\\" title=\\\"every live peer restarted after this node went down - none can say what was deleted meanwhile; this node holds its replayed records until it can finish\\\">reconcile has no witness for <b>\"+esc(dur(c.reconcile_no_witness_s))+\"</b> \\u2014 restart this node</span>\";\n"
	"var wf=c.restart&&c.restart.waits_for;\n"
	"if(wf&&wf.length)return \"<span class=\\\"hstate amber\\\" title=\\\"a peer's reconcile is still asking, and this node is its last live witness - restarting it now would leave that reconcile waiting\\\">do not restart: node <b>\"+esc(wf.join(\", node \"))+\"</b> needs it to finish its reconcile</span>\";\n"
	"return \"\";}\n"
	"function drawHal(m){var el=document.getElementById(\"hal\"),bad=[],w=\"\",i;\n"
	/* 0.5.7.1: and another member whose reconcile has no live witness -
	 * amber: it needs ITS restart, not anyone else's */
	"for(i=0;m&&i<m.length;i++){var d=m[i];if(!d.self&&d.reconciling&&d.sole_witness===0){if(!w)w=\"amber\";bad.push(\"node \"+d.node+\" reconcile has no witness\");}}\n"
	/* AR11: and one whose settings differ from this node's */
	"for(i=0;m&&i<m.length;i++){var d=m[i];if(!d.self&&d.config_differs&&d.config_differs.length){if(!w)w=\"amber\";bad.push(\"node \"+d.node+\" settings differ\");}}\n"
	/* UP1: more than one build among the live members - an upgrade in
	 * progress, or one that stopped half way: amber, the versions named */
	"var vs={},vn=0,vk;for(i=0;m&&i<m.length;i++){var d=m[i];if(d.state!==\"gone\"&&d.version){if(!vs[d.version]){vs[d.version]=0;vn++;}vs[d.version]++;}}\n"
	"if(vn>1){if(!w)w=\"amber\";var vl=[];for(vk in vs)vl.push(vk+\" x\"+vs[vk]);bad.push(\"mixed versions: \"+vl.sort().join(\", \"));}\n"
	"for(i=0;m&&i<m.length;i++){var d=m[i],v=sev(d.state,d.gone_reason||\"\");if(d.self||!v)continue;\n"
	"if(v===\"red\")w=\"red\";else if(!w)w=\"amber\";\n"
	"bad.push(\"node \"+d.node+\" \"+(d.state===\"gone\"?\"silent\":d.state));}\n"
	"el.hidden=!bad.length;el.className=\"hal\"+(w?\" \"+w:\"\");\n"
	"el.textContent=bad.length?\"fleet: \"+(bad.length>3?bad.slice(0,3).join(\", \")+\" +\"+(bad.length-3)+\" more\":bad.join(\", \")):\"\";drawFl(m);}\n"
	/* S341: the FLEET in the header - members ready of the fleet's size.
	 * The S335 chip names hosts in trouble but stays quiet for a member
	 * that LEFT (calm in the tiles by S192) and cannot see one missing
	 * from the list; this counts both.  The size is [cluster] expect
	 * when set, else the most members the node has listed since it
	 * started (/members fleet.seen_max).  Amber below it, red for a
	 * failed member or fewer ready than a majority.  Hidden on a
	 * single node. */
	"function drawFl(m){var el=document.getElementById(\"hfl\");if(!el)return;\n"
	"var f=FLINFO||{},n=m?m.length:0,e=Math.max(n,f.expect||0,f.seen_max||0),r=0,red=false,nr=[],i,d;\n"
	"if(e<2){el.hidden=true;el.className=\"hal\";el.textContent=\"\";return;}\n"
	"for(i=0;i<n;i++){d=m[i];if(d.state===\"ready\"){r++;continue;}if(d.state===\"failed\")red=true;\n"
	"nr.push(\"node \"+d.node+\" \"+(d.state===\"gone\"?(d.gone_reason===\"silent\"?\"silent\":\"left\"):d.state));}\n"
	"if(e>n)nr.push((e-n)+\" missing\");\n"
	"if(r<Math.floor(e/2)+1)red=true;\n"
	"el.hidden=false;el.className=\"hal\"+(red?\" red\":r<e?\" amber\":\"\");\n"
	"el.textContent=\"fleet \"+r+\"/\"+e+(r<e?\" \\u2014 \"+(nr.length>2?nr.slice(0,2).join(\", \")+\" +\"+(nr.length-2)+\" more\":nr.join(\", \")):\" ready\");\n"
	"el.title=\"members ready of the fleet's size - \"+(f.expect?\"[cluster] expect = \"+f.expect:\"the most members this node has listed since it started (\"+e+\")\")+\"; go to the fleet\";}\n"
	"function vcmp(a,b){var x=String(a).split(\".\"),y=String(b).split(\".\"),i,p,q;for(i=0;i<Math.max(x.length,y.length);i++){p=parseInt(x[i]||0,10)||0;q=parseInt(y[i]||0,10)||0;if(p!==q)return p<q?-1:1;}return 0;}\n"
	"function drawFleet(m){MEM=m;\n"
	"var i,sel=document.getElementById(\"nodesel\"),o=\"\";\n"
	"m=m.slice().sort(function(a,b){\n"
	"var ra=RANK[a.state]===undefined?2:RANK[a.state],rb=RANK[b.state]===undefined?2:RANK[b.state];\n"
	"if(ra!==rb)return ra-rb;\n"
	"if(!!a.master!==!!b.master)return a.master?-1:1;\n"
	"if(!!a.backup!==!!b.backup)return a.backup?-1:1;\n"
	"return a.node-b.node;});\n"
	"var cp=m.length>24,el=document.getElementById(\"fleet\");\n"
	/* UP1 (the operator, 2026-10-15: the header blinks on mixed
	 * versions "but node cards are not"): the newest build among the
	 * live members - a card still on an older one blinks amber, as a
	 * roll's stragglers should */
	"var vmax=null;for(i=0;i<m.length;i++)if(m[i].state!==\"gone\"&&m[i].version&&(vmax===null||vcmp(m[i].version,vmax)>0))vmax=m[i].version;\n"
	"el.className=\"fleet \"+(cp?\"cp\":\"cf\");\n"
	"document.getElementById(\"density\").textContent=cp?\"compact tiles (>24 nodes)\":\"\";\n"
	"sel.innerHTML=m.slice().sort(function(a,b){return a.node-b.node;}).map(function(d){\n"
	"return '<option value=\"'+(TOOL?'?node='+d.node:'http://'+esc(d.addr)+':'+(d.http||HP)+'/')+'\"'+(d.self?\" selected\":\"\")+\">node \"+\n"
	"esc(d.node)+\" · \"+esc(d.addr)+(d.self?\" · this node\":\"\")+\n"
	"\"</option>\";}).join(\"\");\n"
	"sel.onchange=function(){if(this.value)location.href=this.value;};\n"
	"for(i=0;i<m.length;i++){var d=m[i],st=d.state||\"unknown\",\n"
	/* S192: a member that has LEFT is grey and calm - it is not a
	 * fault, it is a node that said goodbye and whose id is held for
	 * it.  Without this the card vanished and a rolling restart read
	 * as a fleet that had shrunk. */
	/* S293: and a member that went SILENT is not calm - it did not
	 * say goodbye, something stopped it; it keeps the gone layout but
	 * takes the warning colour */
	"gr=d.gone_reason||\"\",\n"
	"attn=(st===\"gone\")?(gr===\"silent\"?\" gone silent\":\" gone\"):(st!==\"ready\"&&st!==\"failed\")?\" attn\":\"\",\n"
	/* S388: its reconcile has no live witness - it waits for ITS restart:
	 * the card blinks as a failed one does (the header and the fleet chip
	 * said it; the card did not - PROD, 2026-10-13) */
	"nw=st!==\"gone\"&&d.reconciling&&d.sole_witness===0,\n"
	/* S390: and one reconciling WITH a witness blinks amber, as a node
	 * not ready does: it serves, but it holds its replayed records from
	 * its peers until the pass ends - it is not a node to restart a
	 * peer beside (the operator, PROD 2026-10-13: "then that node's card
	 * should blink amber") */
	"rcn=!nw&&!attn&&st!==\"gone\"&&st!==\"failed\"&&d.reconciling,\n"
	"old=st!==\"gone\"&&d.version&&vmax!==null&&vcmp(d.version,vmax)<0,\n"
	/* S428 (the operator, PROD 2026-10-20: "node cards not blinking again
	 * while at the top warning blinking fine"): a setting the fleet must
	 * agree on held differently blinks amber too - the header's chip
	 * counts it, the card only said it in its foot */
	"cdf=st!==\"gone\"&&d.config_differs&&d.config_differs.length>0,\n"
	"role=d.master?\"master\":d.backup?\"backup\":\"member\",\n"
	"hp=(d.total_mb?Math.round(100*d.free_mb/d.total_mb):null),\n"
	"tk=hp===null?\"\":(hp<15?\"crit\":hp<35?\"low\":\"\"),\n"
	"u=\"http://\"+esc(d.addr)+\":\"+(d.http||HP)+\"/\";\n"
	"o+=(d.self?\"<div\":'<a href=\"'+u+'\"')+' class=\"node '+esc(st)+attn+(nw?\" nowit\":rcn||((old||cdf)&&!attn)?\" attn\":\"\")+(d.self?\" self\":\"\")+\n"
	"'\" title=\"node '+esc(d.node)+\" — \"+esc(d.addr)+\":\"+esc(d.port)+\" — \"+esc(st)+'\">'+\n"
	"'<div class=top><span class=id>'+esc(d.node)+'</span><span class=chips>'+\n"
	"'<span class=\"st '+(st===\"ready\"?\"ready\":st===\"failed\"?\"failed\":st===\"gone\"?(gr===\"silent\"?\"lost\":\"goner\"):\"attn\")+'\">'+(st===\"gone\"?(gr===\"silent\"?\"silent\":gr===\"forgotten\"?\"dropped\":\"left\"):esc(st))+\n"
	"'</span><span class=\"role '+role+'\">'+role+'</span></span></div>'+\n"
	/* UP1: the build it runs, beside its address (absent: before 0.6.5) */
	"(d.version?'<div class=addr>v'+esc(d.version)+(d.wire_epochs?' \u00b7 wire '+esc(d.wire_epochs[1]):'')+'</div>':'')+\n"
	"'<div class=addr>'+esc(d.addr)+'</div>'+\n"
	"'<div class=addr>'+(d.gone_s>=0?'left '+dur(d.gone_s)+' ago':'up '+(d.uptime_s>=0?dur(d.uptime_s):\"\\u2014\"))+'</div>'+\n"
	"'<div class=hl><span>headroom</span><span class=pct>'+(hp===null?\"-\":hp+\"%\")+'</span></div>'+\n"
	"'<div class=\"track '+tk+'\"><i style=\"width:'+(hp===null?0:hp)+'%\"></i></div>'+\n"
	/* S186: WHY, at the foot of the card, whenever the node is not
	 * ready and said something.  A card that read FAILED and nothing
	 * else sent the operator to the logs - and to the logs of whichever
	 * node they happened to have open, which is rarely the broken one. */
	"(st===\"gone\"?'<div class=why>'+(gr===\"silent\"?'went silent'+(d.gone_silent_s>0?' for '+dur(d.gone_silent_s):'')+' and was purged - it did not say goodbye':gr===\"forgotten\"?'was dropped - refused by this fleet':gr===\"goodbye\"?'said goodbye':'left')+' - its id is held for '+(d.held_s>0?dur(d.held_s):'a while')+' more, so nothing else can be this node</div>':st!==\"ready\"&&d.reason?'<div class=why>'+esc(d.reason)+'</div>':\"\")+\n"
	/* S388: why it blinks - or, with a witness, that it is reconciling */
	"(nw?'<div class=why>reconcile has no witness \\u2014 restart this node</div>':st!==\"gone\"&&d.reconciling?'<div class=why>reconciling after its restart'+(d.self&&RCL?' \\u2014 '+esc(RCL):'')+'</div>':\"\")+\n"
	/* AR11: a setting the fleet must agree on that it holds differently */
	"(d.config_differs&&d.config_differs.length?'<div class=why style=\"color:var(--warn)\">settings differ from this node: '+esc(d.config_differs.join(\", \"))+'</div>':\"\")+\n"
	/* UP1: an older build than the fleet's newest - the roll has not reached it */
	"(old?'<div class=why style=\"color:var(--warn)\">runs v'+esc(d.version)+' - the newest in the fleet is v'+esc(vmax)+'</div>':\"\")+\n"
	"(d.self?'<span class=you>this node</span></div>':\"</a>\");}\n"
	"el.innerHTML=o;drawHal(m);}\n"
	"function drawStats(s){\n"
	"var c=s.cluster||{},m=s.memory||{},r=s.resp||{},pr=s.process||{},nt=s.native||{},\n"
	/* S123: the door cards lead with what is open NOW; the totals say
	 * what they count from - the start, or the last reset */
	"sn=s.since||{},\n"
	"cl=s.collections||[],i,ent=0,hits=0,mis=0,ah=0,am=0,rl=0,rf=0,nrt=0,tip=[],h5=0,m5=0,ah5=0,am5=0;\n"
	/* 0.6.1: a read-through collection's <name>.nofall companion is
	 * not a collection anyone reads - one marker per key the fleet wrote
	 * that the upstream holds too ("ours now: never read it from the
	 * upstream again").  As a row of its own it read as data: the
	 * operator took staging-redis.nofall's 1,726 for writes looping to
	 * the upstream.  Folded into its parent's read-through line (NF). */
	"NF={};var cl0=cl,cnm={};cl=[];for(i=0;i<cl0.length;i++)if(cl0[i].upstream)cnm[cl0[i].name]=1;\n"
	"for(i=0;i<cl0.length;i++){var cn=cl0[i].name||'',cpn=cn.length>7&&cn.slice(-7)==='.nofall'?cn.slice(0,-7):null;\n"
	"if(cpn!==null&&cnm[cpn])NF[cpn]=cl0[i].entries||0;else cl.push(cl0[i]);}\n"
	/* reset-stats rebases the counters DOWNWARD, so a stale baseline
	 * reads as a negative delta; drop both when reset_at moves. */
	"if(sn.reset_at!==PRA){P=null;PC={};SW=[];PRA=sn.reset_at;}\n"
	"RCL=recline(c);\n"
	/* S347: the headline is the CACHES' rate.  A read-through
	 * collection is mostly lookups of keys the upstream does not have
	 * - a ban check - so its "misses" are answers, and weighted by
	 * lookups it dragged the card to 6-12% beside th at 100% (PROD,
	 * 10-07/08: 523 lookups/s at 0.1% found against 73/s at 100%).
	 * Its own figure is how much of it was answered HERE, without
	 * the upstream: lookups less fetches.  Only read-through
	 * collections: the card falls back to all of them.  The hover
	 * names every collection's share. */
	"for(i=0;i<cl.length;i++){var ci=cl[i],h1=ch(ci),m1=cm(ci),w1=wch(ci)||0,x1=wcm(ci)||0,f1=ci.upstream?wv5(ci.upstream,'fetches')||0:0;\n"
	"ent+=(ci.entries||0)+(ci.upstream?ci.upstream.shadow_entries||0:0);ah+=h1;am+=m1;ah5+=w1;am5+=x1;\n"
	"if(ci.upstream){nrt++;rl+=w1+x1;rf+=f1;if(w1+x1)tip.push(ci.name+' (read-through): '+(100*w1/(w1+x1)).toFixed(1)+'% found of '+nf(w1+x1)+' lookups, '+Math.max(0,100*(1-f1/(w1+x1))).toFixed(1)+'% answered here');}\n"
	"else{hits+=h1;mis+=m1;h5+=w1;m5+=x1;if(w1+x1)tip.push(ci.name+': '+(100*w1/(w1+x1)).toFixed(1)+'% of '+nf(w1+x1)+' lookups');}}\n"
	"if(nrt===cl.length){hits=ah;mis=am;h5=ah5;m5=am5;}\n"
	/* hits/misses and cpu are CUMULATIVE counters.  Plotting the
	 * lifetime ratio draws a running average that converges by
	 * construction: after an hour a burst of misses cannot move it,
	 * so the line flattens exactly as it stops being able to report
	 * anything.  Both are differenced against the previous sample,
	 * which makes them rates over the poll window. */
	"var hrl=(h5+m5)?100*h5/(h5+m5):null;\n"
	"var cms=(pr.cpu_user_ms||0)+(pr.cpu_sys_ms||0),now=Date.now(),hr=null,cpu=null,rh=null;\n"
	/* S365: connects per second.  The clients figure is a gauge - what
	 * is open at the poll - and a storm of one-millisecond connections
	 * (5,000 a second, 10-09) moved it from 75 to 94 at most.  The rate
	 * is what says connections are arriving: both client doors' accept
	 * counters, differenced against the previous sample like cpu. */
	"var ccn=(r.conns||0)+(nt.conns||0),ccr=null;\n"
	"if(P){var dh=hits-P.h,dm=mis-P.m,dt=now-P.t;\n"
	"if(dh+dm>0)hr=100*dh/(dh+dm);\n"
	"if(dt>0&&pr.cpu_user_ms!==undefined){cpu=100*(cms-P.c)/dt;if(cpu<0)cpu=0;}\n"
	"if(dt>0&&P.cc!==undefined&&ccn>=P.cc)ccr=1000*(ccn-P.cc)/dt;}\n"
	"P={h:hits,m:mis,c:cms,cc:ccn,t:now};\n"
	"while(SW.length>1&&SW[1].t<=now-SWIN)SW.shift();\n"
	"var SB=SW.length?SW[0]:null,hrs=null;\n"
	"if(SB){var sdh=hits-SB.h,sdm=mis-SB.m;if(sdh>=0&&sdm>=0&&sdh+sdm>0)hrs=100*sdh/(sdh+sdm);}\n"
	"if(nrt&&nrt<cl.length&&rl>0)rh=Math.max(0,100*(1-rf/rl));\n"
	"var cpu5=wv5(pr,'cpu_user_ms')!==null&&sn.win_s>0?100*(wv5(pr,'cpu_user_ms')+(wv5(pr,'cpu_sys_ms')||0))/(sn.win_s*1000):null;\n"
	/* S381: cpu above is a share of ONE core (CPU time over wall time, as
	 * top shows it); with process.cpus the card and the chart are a share
	 * of the CPUs the daemon may use, the one-core figure beside it */
	"NCPU=pr.cpus>0?pr.cpus:0;var cpuN=cpu!==null&&NCPU?cpu/NCPU:cpu,cpu5N=cpu5!==null&&NCPU?cpu5/NCPU:cpu5;\n"
	"var cc5=wv5(r,'conns')!==null&&wv5(nt,'conns')!==null&&sn.win_s>0?(wv5(r,'conns')+wv5(nt,'conns'))/sn.win_s:null;\n"
	"push(\"live\",m.arena_live||0);push(\"ent\",ent);\n"
	"if(hr!==null)push(\"hr\",hr);if(cpuN!==null)push(\"cpu\",cpuN);\n"
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
	/* S357: the build flavor, only when it is not a release build */
	"(pr.build&&pr.build!==\"release\"?\"<span class=\\\"bflav\\\" title=\\\"not a release build - slower, for finding faults; replace it with a release build when done\\\">build <b>\"+esc(pr.build)+\"</b></span>\":\"\")+\n"
	/* S352c: where this node's alerts go by mail - its own [mail], "via
	 * node N" (relayed to the elected mailer), or none */
	"(s.alerts&&s.alerts.mailer?\"<span title=\\\"where this node's alerts go by mail: its own [mail], the member it relays them to, or nowhere\\\">mail <b>\"+esc(/^node /.test(s.alerts.mailer)?\"via \"+s.alerts.mailer:s.alerts.mailer)+\"</b></span>\":\"\")+\n"
	"\"<span class=\\\"hstate \"+sev(s.state,\"\")+\"\\\">state <b>\"+esc(s.state||\"?\")+\"</b></span>\"+recChip(c);\n"
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
	/* S264: the chips add up to the open figure - a connection that has
	 * not sent a request yet has no dialect (the first request sets it),
	 * and a pool's held standby connections sit there for ever: 88 open
	 * read 71 + 2 + 11 on the test fleet.  They get their own chip.  The
	 * connection LIMIT was a chip too, "max 524,165" - derived from the
	 * descriptor limit, it read as a peak or another dialect.  It moves
	 * to the card's hover and comes back on the card, in the colour of
	 * trouble, only when it matters: 80% used, or anything refused.
	 * S317: two dialects on the native door - binary frames and RESP -
	 * so the strip is binary, resp and idle. */
	"var clb=nt.binary?nt.binary.open||0:0,clr=(r.open||0)+(nt.resp?nt.resp.open||0:0),clid=(clo.open||0)-clb-clr;\n"
	/* S366: refusals at the memory floor are inside clients.refused; the
	 * limit chip counts only the limit's, and the floor has its own chip,
	 * on the card while the guard refuses or refused in the window - the
	 * figures behind it in its hover */
	"var clm=clo.memory||{},clmr=wv5(clm,'refused')||0,cllr=Math.max(0,(wv5(clo,'refused')||0)-clmr);\n"
	"var clch='<span class=chip>binary '+nf(clb)+'</span>'+\n"
	"'<span class=chip>resp '+nf(clr)+'</span>'+\n"
	"(clid>0?'<span class=chip title=\"connected, no request yet - the first request sets the dialect\">idle '+nf(clid)+'</span>':'')+\n"
	"(clo.max&&(cllr>0||(clo.open||0)*5>=clo.max*4)?'<span class=chip style=\"color:var(--bad)\">limit '+nf(clo.max)+(cllr?' \\u00b7 '+nf(cllr)+' refused in 5 min':'')+'</span>':'')+\n"
	"(clm.refusing||clmr>0?'<span class=chip style=\"color:var(--bad)\" title=\"'+esc('new connections are refused while less than '+sz(clm.floor)+' stays free beyond the arena reserve '+sz(clm.arena_reserve)+' \\u00b7 '+sz(clm.available)+' available of '+sz(clm.limit)+' ('+(clm.source||'?')+')')+'\">memory floor'+(clmr?' \\u00b7 '+nf(clmr)+' refused in 5 min':'')+'</span>':'');\n"
	/* S378: the I/O backend - what runs and how; a fallback, or a ring
	 * some workers lack, in amber with the reason in the hover.  S379:
	 * the busiest worker's share of the last 5 min, amber on epoll at the
	 * epoll-busy alert's threshold; under io = epoll, whether io_uring
	 * would run here */
	"var IO=s.io||{},iob=IO.backend||null,iobp=IO.busy_poll||{},iow=IO.workers||[],iov='',ios='',iot='',ioa=false,iobz=null,iobw=false,ioi;\n"
	"if(iob){for(ioi=0;ioi<iow.length;ioi++)if(iow[ioi].busy_pct_5m!=null&&(iobz===null||iow[ioi].busy_pct_5m>iobz))iobz=iow[ioi].busy_pct_5m;\n"
	"if(iob.active==='io_uring'){iov='io_uring';ios=iob.buffers+' \\u00b7 '+iob.workers_uring+' of '+(iow.length||iob.workers_uring)+' workers';ioa=iob.workers_uring<iow.length;if(ioa)iot='a ring was refused to '+(iow.length-iob.workers_uring)+' of '+iow.length+' workers - they run epoll';}\n"
	"else if(iob.configured==='epoll'){iov='epoll';ios='io = epoll'+(IO.uring_usable?' \\u00b7 io_uring available':'');}\n"
	"else{iov='epoll (fallback)';ios=iob.fallback||'';ioa=true;iot='io = io_uring, refused here: '+(iob.fallback||'');}\n"
	"if(iobp.workers>0)ios+=' \\u00b7 busy poll '+iobp.us+' \\u03bcs';\n"
	"var iobt=((s.alerts||{}).host||{}).thresholds||{};iobw=iob.active==='epoll'&&iobz!==null&&iobt.epoll_busy_pct!=null&&iobz>=iobt.epoll_busy_pct;}\n"
	"document.getElementById(\"metrics\").innerHTML=\n"
	"'<div class=metric><div class=k>entries</div><div class=v>'+nf(ent)+\n"
	"'</div><div class=s>across '+cl.length+' collection'+(cl.length===1?\"\":\"s\")+'</div></div>'+\n"
	"'<div class=metric title=\"'+esc(tip.length?'last 5 min - '+tip.join(' \\u00b7 '):'no lookups in 5 min')+'\"><div class=k>hit rate (30 s)</div><div class=v>'+\n"
	"(hrs===null?\"\\u2014\":hrs.toFixed(1)+\"%\")+'</div><div class=s>'+\n"
	"(hrl===null?'no reads in 5 min':'5 min '+hrl.toFixed(1)+'% \\u00b7 '+(nrt&&nrt<cl.length?'caches':'all collections'))+\n"
	"(rh!==null?' \\u00b7 read-through '+rh.toFixed(1)+'% answered here':'')+\n"
	"'</div></div>'+\n"
	"'<div class=metric title=\"'+(pr.threads?nf(pr.threads)+' threads':'')+'\"><div class=k>daemon cpu</div><div class=v>'+\n"
	"(cpuN===null?\"\\u2014\":cpuN.toFixed(1)+\"%\")+'</div><div class=s>'+\n"
	"(NCPU?'of '+NCPU+' CPU'+(NCPU===1?'':'s')+(cpu!==null?' \\u00b7 '+cpu.toFixed(0)+'% of one core':''):(pr.threads?nf(pr.threads)+' threads':''))+\n"
	"(cpu5N!==null?' \\u00b7 5 min avg '+cpu5N.toFixed(1)+'%':'')+'</div></div>'+\n"
	"'<div class=metric><div class=k>resident memory</div><div class=v>'+sz(pr.rss_bytes)+\n"
	"'</div><div class=s>kernel rss \\u00b7 live '+sz(m.arena_live)+'</div></div>'+\n"
	/* S98: at the ceiling is a state with a clock, and the card says so */
	"'<div class=metric><div class=k>arena headroom</div><div class=v'+(m.at_ceiling?' style=\"color:var(--bad)\"':'')+'>'+\n"
	"(m.headroom_pct===undefined?\"\\u2014\":m.headroom_pct+\"%\")+'</div><div class=s>'+\n"
	"(m.at_ceiling?'AT CEILING for '+dur(Math.max(0,Math.floor(Date.now()/1000)-m.at_ceiling_since))+' \\u00b7 ':'')+\n"
	"sz(m.arena_held)+' held of '+sz(m.arena_max)+' ceiling</div></div>'+\n"
	"'<div class=metric><div class=k>uptime</div><div class=v>'+dur(pr.uptime_s)+\n"
	"'</div><div class=s>'+(pr.pid?'pid '+nf(pr.pid):'')+'</div></div>'+\n"
	"'<div class=\"metric click\" id=clmetric title=\"click for this node&#39;s connections'+(clo.max?' \\u00b7 limit '+nf(clo.max)+' connections'+(clo.max_basis?' (from '+esc(clo.max_basis)+')':''):'')+'\"><div class=k>clients</div><div class=v>'+nf(clo.open)+\n"
	"(clfl!==null?' <span style=\"font-size:14px;color:var(--ink2)\">/ '+nf(clfl)+' fleet-wide'+(clrep<cltot?' ('+clrep+' of '+cltot+' reporting)':'')+'</span>':'')+\n"
	"'</div><div class=s>'+clch+'</div><div class=s>connects '+(ccr===null?'\\u2014':nf(Math.round(ccr))+'/s')+\n"
	"(cc5!==null?' \\u00b7 5 min avg '+nf(cc5<10?+cc5.toFixed(1):Math.round(cc5))+'/s':'')+'</div></div>'+\n"
	"'<div class=metric><div class=k>members up</div><div class=v'+\n"
	"(mup<mtot?' style=\"color:var(--bad)\"':'')+'>'+\n"
	"(hasmap?nf(mup)+'/'+nf(mtot):nf(mup))+\n"
	/* S157: the copy factor the fleet cannot honour, in the colour a node
	 * down gets - the reader's question is "are my copies there" */
	"(c.replicas_short>0?' <span style=\"color:var(--bad)\">'+c.replicas_short+' cop'+(c.replicas_short===1?'y':'ies')+' short</span>':'')+\n"
	"'</div><div class=s>'+(hasmap?\"map seq \"+nf(c.map.seq)+\", term \"+nf(c.term):\n"
	"(c.map?\"no cluster map yet\":\"unclustered\"))+'</div></div>'+\n"
	"'<div class=metric><div class=k>memory tier</div><div class=v style=\"font-size:15px\">'+\n"
	"esc((m.tier||\"-\").split(\" \")[0])+'</div><div class=s>'+esc(m.tier||\"\")+'</div></div>'+\n"
	"(iob?'<div class=metric id=iometric title=\"'+esc(iot||(iov+' \\u00b7 '+ios))+'\"><div class=k>io</div><div class=v style=\"font-size:15px'+(ioa?';color:var(--warn)':'')+'\">'+esc(iov)+\n"
	"'</div><div class=s>'+esc(ios)+'</div>'+(iobz!==null?'<div class=s'+(iobw?' style=\"color:var(--warn)\"':'')+'>busiest worker '+iobz+'% busy (5 min)</div>':'')+'</div>':'');\n"
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
	"document.querySelectorAll(\".modeb\").forEach(function(b){b.hidden=!cl.length;b.textContent=uni?um+\" mode\":\"mixed modes\";b.classList.toggle(\"bad\",!uni);});\n"
	"document.getElementById(\"colmode\").textContent=\n"
	"(FCn!==null?'fleet figures first, this node beside ('+FCn+' of '+FCt+' members reporting)':'');\n"
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
	"var NC={};RM={};(r.map||[]).forEach(function(e){(RM[e.col]=RM[e.col]||[]).push(e);});\n"
	"for(j=0;j<cl.length;j++){var x=cl[j],t=ch(x)+cm(x),\n"
	"lf=t?100*ch(x)/t:null,pv=SB&&SB.c?SB.c[x.name]:null,rt=null,wn=-1,\n"
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
	"o+='<tr'+(x.upstream?' class=grp':'')+'><td><span class=nm>'+esc(x.name)+'</span>'+rmb(x.name)+(TOOL?dbtn('rename',x.name,'rename',x.entries)+dbtn('resize',x.name,'resize',x.entries)+dbtn('map',x.name,'map',x.entries)+dbtn('drop',x.name,'drop',x.entries):'')+\n"
	"(uni?'':'<span class=md>'+esc(x.mode||\"\")+'</span>')+asc(x)+upmc(x)+mtog(x);\n"
	/* 0.6.1: a read-through collection serves the copies it fetched as
	 * it serves its records, so its entries count them - this node's,
	 * the daemon's gauge (the operator: "doesn't it seem strange NOT to
	 * see the actual keys pulled from redis") */
	"var cpe=x.upstream?x.upstream.shadow_entries||0:0,et=(FG?(FB[FG.basis]||FG.basis)+' ('+nf(FG.copies)+' copies across '+FG.members+' members)':'')+\n"
	"(cpe?(FG?' \\u00b7 ':'')+'with '+nf(cpe)+' fetched from the upstream on this node, served like the records':'');\n"
	"o+='</td><td class=n'+(et?' title=\"'+esc(et)+'\"':'')+'>'+entc(x,FG,LB,cpe)+'</td><td class=\"n dim\">'+nf(x.buckets)+\n"
	"'</td><td class=\"n'+(ldk||' dim')+'\"'+(ld>=70?' title=\"'+(ld>=90?'critical - ':'')+'entries against buckets x slots; past the grow target a bucket miss goes to the overflow leg, one lock for the whole table\"':'')+'>'+(ld<0.01&&!x.entries?\"—\":ld.toFixed(2)+\"%\")+\n"
	"'</td><td class=\"n'+(x.overflow>0&&x.autoscale&&x.autoscale!=='auto'&&x.autoscale!=='floor'?' b':x.overflow>0&&x.entries&&x.overflow>x.entries/2?' w':' dim')+'\"'+\n"
	"(x.held_walk_us?' title=\"last held walk '+(x.held_walk_us/1000000).toFixed(2)+' s \u2014 every other maintenance duty waits behind it\"':'')+'>'+\n"
	"(x.overflow===undefined?\"\u2014\":nf(x.overflow))+'</td><td><div class=hit>'+\n"
	"(rt===null?'<div class=track></div>':'<div class=\"track '+k+'\"><i style=\"width:'+\n"
	"rt.toFixed(0)+'%\"></i></div>')+'<span class=hv><span class='+(rt===null?'\"n dim\"':'n')+'>'+\n"
	"(rt!==null?rt.toFixed(1)+'%':lf===null?'\u2014':wn===0?'no reads':'\u2014')+'</span><span class=hl>'+\n"
	"((wch(x)||0)+(wcm(x)||0)>0?'5 min '+(100*wch(x)/(wch(x)+wcm(x))).toFixed(1)+'%':lf===null?'never read':'5 min: none')+'</span></span></div></td>'+\n"
	"'<td><div class=hit>'+(cr(x)===undefined?'<span class=dim>\u2014</span>':\n"
	"'<span class=hv><span class=n>'+nf(cr(x))+'</span><span class=hl>'+\n"
	"(x.entries?(100*cr(x)/x.entries).toFixed(cr(x)*100<x.entries?2:1)+'% of held':'of 0 held')+\n"
	"'</span></span>')+'</div></td><td class=n>'+\n"
	/* S194: these four are COUNTERS, so they read as the last five
	 * minutes like everything else on the page; entries, buckets, load
	 * and the leg beside them are gauges and stay absolute.  The
	 * fleet-wide figures (FG) are windowed too: the daemon keeps each
	 * member's history and publishes the fleet's win_ fields (S351),
	 * null while a member has no minute of baseline yet. */
	"(FG?fv(wv5(FG,'hits'),wch(x)):nf(wch(x)))+'</td><td class=n>'+(FG?fv(wv5(FG,'misses'),wcm(x)):nf(wcm(x)))+'</td><td class=n'+(FG?' title=\"client writes, each counted once on the node the client wrote to\"':'')+'>'+\n"
	"(FG?fv(wv5(FG,'stores'),wv5(x,'stores_client')):nf(wv5(x,'stores')))+\n"
	"'</td><td class=\"n dim\">'+(FG?fv(wv5(FG,'expired'),wv5(x,'expired'),LB):nf(wv5(x,'expired')))+'</td><td class=n>'+\n"
	"(x.held_age_s===undefined||x.held_age_s<0?'<span class=dim>\u2014</span>':sz(x.held_bytes))+\n"
	/* S116: the sizes HELD, from the walk - not this node's client writes,
	 * which left a push-fed member's column blank */
	"'</td><td>'+(x.held_age_s===undefined||x.held_age_s<0?'<span class=dim>\u2014</span>':hist(x.held_hist))+'</td></tr>'+memr(x)+upr(x);}\n"
	"PC=NC;SW.push({t:now,h:hits,m:mis,c:NC});\n"
	"document.getElementById(\"cols\").innerHTML=o||\n"
	"'<tr><td colspan=13 class=dim>no collections</td></tr>';\n"
	/* S67: eight bars, one per size class, scaled to the fullest */
	"function hist(a){if(!a)return'';var m=0,i,o='<span class=hist>';for(i=0;i<a.length;i++)if(a[i]>m)m=a[i];\n"
	"if(!m)return'<span class=dim>\u2014</span>';var L=['64','256','1K','4K','16K','64K','256K','more'];\n"
	"for(i=0;i<a.length;i++)o+='<i title=\"'+L[i]+': '+nf(a[i])+'\" style=\"height:'+Math.max(1,Math.round(14*a[i]/m))+'px\"></i>';return o+'</span>';}\n"
	"function bud(s){var b=s.budget||{},c=s.collections||[],r=[],i,x;\n"
	"r.push(['<span class=dim>collection</span>','<span class=\"dim sm\">index \\u00b7 records</span>']);\n"
	"for(i=0;i<c.length;i++){x=c[i];if(x.index_bytes===undefined)continue;\n"
	"r.push(['<span title=\"'+esc(x.name)+'\">'+esc(x.name)+'</span>','<span class=sm>'+sz(x.index_bytes)+' \\u00b7 '+(x.held_age_s===undefined||x.held_age_s<0?'\\u2014':sz(x.held_cells))+'</span>']);}\n"
	"r.push([\"index, all\",b.index===undefined?\"\\u2014\":sz(b.index)]);\n"
	"r.push(['<span title=\"records, all - as the cells they occupy\">records, all</span>',b.records===undefined?\"\\u2014\":sz(b.records)]);\n"
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
	/* S194: the WINDOW is what a dashboard is read for, so a figure a
	 * reader compares is the last five minutes; the cumulative fields
	 * stay in /stats and /metrics for Prometheus.  An older daemon
	 * sends no win_* and reads as it always did.
	 * S196: these live at TOP LEVEL.  They sat inside drawCmds() while
	 * wmax() and wspan() - out here - call wq(), so every render died
	 * on ReferenceError in the row loop, before the table was ever
	 * written.  A helper is visible to its callers or it is a bug.
	 * vname() is the one place a verb is named: the verb, with its
	 * dialect beside it as a field (S189), on every surface. */
	"function wq(x,k){return x['win_'+k]!==undefined?x['win_'+k]:x[k];}\n"
	"function wcalls(x){return x.win_calls!==undefined?x.win_calls:x.calls;}\n"
	"function wmean(x){var n=wcalls(x);return n>0?wq(x,'usec')/n:-1;}\n"
	/* S197: no calls, nothing to be slowest - a row the window has not
	 * seen sorts below every row it has, whatever max the daemon sends
	 * beside it.  The stale ring slot sent one, and idle verbs jumped
	 * above live ones each minute; folded, the page showed a row of
	 * dashes as "the slowest verb". */
	"function wslow(x){if(!(wcalls(x)>0))return -1;var m=wq(x,'max_us');return m>0?m:x.win_max_us!==undefined?-1:worst(x);}\n"
	"function vname(x){return esc(x.verb||x.name)+(x.dialect?' <span class=dim>('+esc(x.dialect)+')</span>':'');}\n"
	/* S183: the exact slowest call when the daemon reports one; the
	 * histogram bound only for an older daemon that does not. */
	"function wmax(x){var mx=wq(x,'max_us');if(mx>0)return lat(mx);var k=worst(x),n=(x.hist||[]).length;return k<0?'\\u2014':k==n-1?'> '+lat(Math.pow(2,k-1)):'\\u2264 '+lat(Math.pow(2,k));}\n"
	"function wspan(x){var mx=wq(x,'max_us');if(mx>0)return 'exactly '+lat(mx);var k=worst(x),n=(x.hist||[]).length;return k<0?'none':k==n-1?'over '+lat(Math.pow(2,k-1)):k?lat(Math.pow(2,k-1))+' to '+lat(Math.pow(2,k)):'up to 1 us';}\n"
	"function lat1(u){u=+u;return u<1000?u.toFixed(1)+' \\u03bcs':lat(u);}\n"
	/* the slowest-calls TABLE (its own section under collections): every
	 * verb that has been called, worst call first, then p99, then mean */
	/* S188: sort on the EXACT max when the daemon reports one, so the
	 * order matches the column a reader can see.  It sorted on the
	 * histogram's worst BUCKET, and two verbs whose slowest calls fall
	 * in one bucket - 8.3 ms and 8.7 ms - then ordered on p99, which
	 * put the 8.3 above the 8.7 with both printed beside each other.
	 * The bucket remains the fallback for an older daemon.
	 * S194: the WINDOW is what a dashboard is read for, so every
	 * column here is the last five minutes; the cumulative fields
	 * stay in /stats and /metrics for Prometheus.  An older daemon
	 * sends no win_* and reads as it always did.
	 * S197: the ROWS are the window too - the operator's call.  A verb
	 * the window has not seen is not listed; it falls out of the table
	 * as it falls out of the five minutes, and the placeholder says so
	 * rather than "no commands yet" on a daemon that has served
	 * millions.  (S195 kept such rows with dashes.  That was a fix for
	 * the wrong bug: the empty table was S196's ReferenceError, and a
	 * table of dashes is noise once the render works.) */
	"function drawCmds(s){var cm=(s.commands||[]).filter(function(x){return wcalls(x)>0;}),R={n:cm.length};\n"
	"function rows(){var o='',i,x,c;for(i=0;i<cm.length;i++){x=cm[i];c=wcalls(x);\n"
	"o+='<tr'+(i?' class=cx':'')+'><td>'+vname(x)+'</td><td class=n>'+nf(c)+'</td><td class=n>'+lat1(wq(x,'usec')/c)+\n"
	"'</td><td class=n>'+lat(wq(x,'p50_us'))+'</td><td class=n>'+lat(wq(x,'p99_us'))+'</td><td class=n>'+lat(wq(x,'p999_us'))+\n"
	"'</td><td class=n title=\"the slowest call fell '+wspan(x)+'\">'+wmax(x)+'</td></tr>';}\n"
	"return o||'<tr class=none><td colspan=7 class=dim>nothing called in the last 5 min</td></tr>';}\n"
	"cm.sort(function(x,y){return wslow(y)-wslow(x)||wq(y,'p99_us')-wq(x,'p99_us')||wmean(y)-wmean(x);});R.slow=rows();\n"
	"cm.sort(function(x,y){return wcalls(y)-wcalls(x)||wslow(y)-wslow(x);});R.calls=rows();\n"
	"CMDROWS=R;cmdshow();}\n"
	/* S196: a verb is named the same way wherever it appears - the
	 * verb, its dialect beside it - so the card and the table can be
	 * read against each other.  It printed the composite name
	 * ("cmd:get", and "json.get" for a Redis name with a dot), which
	 * is the thing S189 stopped asking readers to parse.  The figures
	 * are the five-minute window the rest of the plane reports, and
	 * the title says so. */
	/* S165: the commands clients sent that no door implements, fleet-wide
	 * when this node is clustered - the daemon folds every member's table
	 * (/stats unknown_commands), the page only renders it.  A row is a
	 * name, never an argument; the hover carries who sent it last. */
	"function ago1(t){t=Math.max(0,Math.floor(t));return t<60?t+'s':t<3600?Math.floor(t/60)+'m':t<86400?Math.floor(t/3600)+'h':Math.floor(t/86400)+'d';}\n"
	"function unkcard(s){var u=s.unknown_commands,now=Date.now()/1000,rows;if(!u)return'';\n"
	"rows=(u.rows||[]).slice(0,8).map(function(x){return ['<span title=\"'+esc(x.name)+' ('+esc(x.dialect)+')'+(u.members>1?' \\u00b7 '+nf(wv5(x,'here'))+' of '+nf(wv5(x,'count'))+' here in 5 min':'')+\n"
	"' \\u00b7 first seen '+ago1(now-x.first_s)+' ago \\u00b7 last from '+esc(x.addr)+(x.client?' ['+esc(x.client)+']':'')+(u.members>1?' via node '+x.node:'')+'\">'+esc(x.name)+'</span>',\n"
	"nf(wv5(x,'count'))+' \\u00b7 '+ago1(now-x.last_s),true];});\n"
	"if(!rows.length)rows=[['none','']];while(rows.length<8)rows.push(['\\u00a0','']);if(wv5(u,'other'))rows.push(['other names',nf(wv5(u,'other')),true]);\n"
	"if(wv5(u,'preauth'))rows.push(['before login',nf(wv5(u,'preauth'))]);\n"
	"if(u.members>1&&u.reporting<u.members)rows.push(['reporting',u.reporting+' of '+u.members,true]);\n"
	"return card('unknown commands',rows,'unk');}\n"
	/* S352b: the node's alerts (/stats alerts) - what is raised now, then
	 * the newest entries of the ring, each with what became of it by mail
	 * (mailed, suppressed, below level, no mailer; S352c: relayed to node N,
	 * mailed by node N, no mailer reachable).  Times are the VIEWER's
	 * local time, with the date when it is not today; the hover has the
	 * whole of it, the key and the text.  Always ACARD rows tall (S214) -
	 * the ring keeps 100, /stats has them all.  Every string through esc():
	 * an alert names collections and upstream hosts.  The mail rows are the
	 * daemon's five-minute windows, a dash until it has one; the relay row
	 * (S352c) only where relays pass - this node relays, mails for a fleet,
	 * or one moved in the window. */
	"function atime(ms){var d=new Date(+ms),n=new Date(+Date.now()),p=function(v){return (v<10?'0':'')+v;};\n"
	"return (d.toDateString()===n.toDateString()?'':p(d.getMonth()+1)+'-'+p(d.getDate())+' ')+p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds());}\n"
	/* S377: info (a clean start, a test) is routine - not the warn colour */
	"function alvl(l){return '<span style=\"color:var(--'+(l==='critical'?'bad':l==='info'?'ink2':'warn')+')\">'+esc(l)+'</span>';}\n"
	"function drawSlow(s){var sl=(s.slowlog||[]).slice(0,50),now=Date.now()/1000,o='',i,e,t,g,w;\n"
	"for(i=0;i<sl.length;i++){e=sl[i];t=esc((e.argv||[]).join(' '));\n"
	"o+='<tr'+(i?' class=cx':'')+'><td>'+(e.ts?atime(e.ts*1000)+' <span class=dim>'+ago1(now-e.ts)+' ago</span>':'\\u2014')+'</td><td class=sx title=\"'+t+'\">'+t+\n"
	"'</td><td class=n>'+lat(e.usec)+'</td><td>'+(e.addr?esc(e.addr)+(e.name?' <span class=dim>['+esc(e.name)+']</span>':''):'\\u2014')+'</td></tr>';}\n"
	"document.getElementById('slow').innerHTML=o||'<tr class=none><td colspan=4 class=dim>empty</td></tr>';\n"
	"SLN=sl.length;g=document.getElementById('sltog');w=document.getElementById('slwrap');if(g&&w)g.textContent=sllabel(w.classList.contains('open'));}\n"
	/* S397: /stats collections[].hot_keys.top - a key is bytes, shown
	 * through esc(); a key the sketch keeps cut says how long it is */
	/* 12nx: a collection's entries cell - its RECORDS, the fleet's figure
	 * by the mode's rule first (fv(FG.entries,x.entries,LB): the fullest
	 * member in eager, at least that in store, the copies over K in spread,
	 * the sum in shard) or, in store mode under the dashboard, the merged
	 * sketches' estimate of the fleet's distinct keys; then this node's
	 * read-through copies of Redis values on a line of their own */
	"function entc(x,FG,LB,cpe){var d=LB?dfull(x.name):null,o;\n"
	"COLF[x.name]=FG?FG.entries:x.entries;COLB[x.name]=FG?FG.basis:null;\n"
	"o=d?'<span class=hv><span class=n title=\"estimated distinct keys across the fleet: the members\\' held-walk sketches merged (HyperLogLog, \\u00b11.6%), '+d.members+' of '+d.of+' members, the oldest walk '+d.oldest_walk_s+' s ago\">\\u2248 '+nf(d.estimate)+'</span><span class=hl>'+nf(x.entries)+' here</span></span>':FG?fv(FG.entries,x.entries,LB):nf(x.entries);\n"
	"if(cpe)o+='<span class=cpl title=\"read-through copies of Redis values this node holds, served like records\">+'+nf(cpe)+' Redis copies here</span>';return o;}\n"
	/* 12nx: under the dashboard, each collection's members, one line each */
	"function memr(x){if(!TOOL||!FLM)return'';var rows='';\n"
	"FLM.slice().sort(function(a,b){return a.node-b.node;}).forEach(function(m){var c=null;(m.collections||[]).forEach(function(y){if(y.name===x.name)c=y;});\n"
	"rows+='<tr><td>'+esc(m.node)+'</td><td><span class=nm>'+esc(m.addr)+':'+esc(m.port)+'</span>'+(m.state!=='connected'?' <span style=\"color:var(--bad)\">'+esc(m.state)+'</span>':'')+'</td><td class=n>'+(c&&c.entries>=0?nf(c.entries):'\\u2014')+'</td><td class=n>'+(c&&c.copies!==undefined?nf(c.copies):'\\u2014')+'</td></tr>';});\n"
	"return '<tr class=\"up mrow\" data-col=\"'+esc(x.name)+'\"'+(COLOPEN[x.name]?'':' hidden')+'><td colspan=13><div class=upw><span class=uk>'+esc(x.name)+' \\u00b7 per member</span>'+\n"
	"'<table class=mt><thead><tr><th>node</th><th>member</th><th class=n>records</th><th class=n>Redis copies</th></tr></thead><tbody>'+rows+'</tbody></table></div></td></tr>';}\n"
	"function mtog(x){return TOOL&&FLM?'<span class=mtog role=button tabindex=0 data-col=\"'+esc(x.name)+'\" title=\"each member\\'s records and copies of this collection\">'+(COLOPEN[x.name]?'\\u25be':'\\u25b8')+' members</span>':'';}\n"
	"function drawHot(s){var cl=s.collections||[],r=[],i,j,h,t,o='',g,w;\n"
	"for(i=0;i<cl.length;i++){h=cl[i].hot_keys;if(!h||!h.top||!h.sampled)continue;\n"
	"for(j=0;j<h.top.length;j++)r.push({c:cl[i].name,k:h.top[j],n:h.sampled});}\n"
	"r.sort(function(x,y){return y.k.share_pm-x.k.share_pm||y.k.est-x.k.est;});r=r.slice(0,32);\n"
	"for(i=0;i<r.length;i++){h=r[i].k;t=esc(h.key)+(h.cut?' <span class=dim>(\\u2026 '+nf(h.key_len)+' bytes)</span>':'');\n"
	"o+='<tr'+(i?' class=cx':'')+'><td>'+esc(r[i].c)+'</td><td class=sx title=\"'+esc(h.key)+'\">'+t+'</td><td class=n>'+(h.share_pm/10).toFixed(1)+\n"
	"'%</td><td class=n>'+nf(h.est)+(h.err?' <span class=dim>\\u00b1'+nf(h.err)+'</span>':'')+'</td></tr>';}\n"
	"document.getElementById('hot').innerHTML=o||'<tr class=none><td colspan=4 class=dim>no key seen twice in the samples yet</td></tr>';\n"
	"HKN=r.length;g=document.getElementById('hktog');w=document.getElementById('hkwrap');if(g&&w)g.textContent=hklabel(w.classList.contains('open'));}\n"
	"function drawAlerts(s){var a=s.alerts,ml=s.mail||{},h=document.getElementById('alh'),w=document.getElementById('alwrap'),g=document.getElementById('altog'),ac,rc,o='',i,e,nc=0,sm=[],ri,ro,rd;\n"
	"if(h)h.hidden=!a;if(w)w.hidden=!a;if(!a)return;\n"
	"ac=a.active||[];rc=a.recent||[];for(i=0;i<ac.length;i++)if(ac[i].level==='critical')nc++;\n"
	"sm.push(ac.length?'<span style=\"color:var(--'+(nc?'bad':'warn')+')\">active: '+nf(ac.length)+(nc?' \\u00b7 '+nf(nc)+' critical':'')+'</span>':'active: none');\n"
	"if(ml.ready)sm.push('<span title=\"'+esc(ml.last_error?'the last refusal: '+ml.last_error:'no mail has failed')+'\"'+(wv5(ml,'failed')>0?' style=\"color:var(--warn)\"':'')+'>mail sent / failed (5 min): '+nf(wv5(ml,'sent'))+' / '+nf(wv5(ml,'failed'))+'</span>');\n"
	"ri=wv5(ml,'relayed_in');ro=wv5(ml,'relayed_out');rd=wv5(ml,'relay_dropped');\n"
	"if(ml.relayed_in!==undefined&&(a.fleet_mailer>0||ri||ro||rd))sm.push('<span title=\"alerts this node mailed for members without [mail] / handed to the member that mails\"'+(rd>0?' style=\"color:var(--warn)\"':'')+'>relayed in / out (5 min): '+nf(ri)+' / '+nf(ro)+(rd?' \\u00b7 '+nf(rd)+' lost':'')+'</span>');\n"
	"document.getElementById('alsum').innerHTML=sm.join(' \\u00b7 ');\n"
	"for(i=0;i<ac.length;i++){e=ac[i];o+='<tr class=act><td>since '+atime(e.since_ms)+'</td><td>'+alvl(e.level)+'</td><td>active</td><td class=\"sx '+(e.level==='critical'?'b':'w')+'\" title=\"'+\n"
	"esc(e.key+' \\u00b7 '+e.level+' since '+new Date(+e.since_ms).toLocaleString()+' \\u00b7 '+e.text)+'\">'+esc(e.text)+'</td><td></td></tr>';}\n"
	/* S389: folded, the table is what is ACTIVE - with nothing active it
	 * says so, and the history (the ring) waits behind the fold whole.  It
	 * showed the newest entry folded: "active: none" above a row that read
	 * "raised" (the operator, PROD 2026-10-13) */
	"if(!ac.length&&rc.length)o+='<tr class=none><td colspan=5 class=dim>no active alerts</td></tr>';\n"
	"for(i=0;i<rc.length;i++){e=rc[i];o+='<tr class=cx><td>'+atime(e.ts_ms)+'</td><td>'+alvl(e.level)+'</td><td>'+(e.event==='cleared'?'<span style=\"color:var(--ok)\">cleared</span>':'raised')+\n"
	"'</td><td class=sx title=\"'+esc(new Date(+e.ts_ms).toLocaleString()+' \\u00b7 '+e.key+' \\u00b7 '+e.level+' '+e.event+' \\u00b7 '+e.text)+'\">'+esc(e.text)+'</td><td>'+esc(e.mail||'-')+'</td></tr>';}\n"
	"document.getElementById('alrt').innerHTML=o||'<tr class=none><td colspan=5 class=dim>nothing raised since the node started</td></tr>';\n"
	"ALN=rc.length;if(g&&w)g.textContent=allabel(w.classList.contains('open'));}\n"
	/* the plane's cards ordered by their own height, tallest first, so a
	 * grid row holds cards of one size instead of one long card leaving
	 * its neighbours half empty.  Measured with the grid at align-items
	 * start (stretched, every card in a row reads the row's height), in
	 * 8 px steps so a sub-pixel difference is not a size; equal cards
	 * keep the order they are written in, so the grouping below survives
	 * among cards of a size.  S214: and the order, once decided, is KEPT
	 * (planeOrder) - a redraw moves nothing unless a cell came or went or
	 * the plane changed width. */
	"function sizePlane(){var p=document.getElementById(\"plane\"),k=[].slice.call(p.children),i,keys=[],hs=[],by={},o,h3;\n"
	"p.style.alignItems=\"start\";for(i=0;i<k.length;i++){h3=k[i].querySelector?k[i].querySelector(\"h3\"):null;\n"
	"keys.push(h3?h3.textContent:String(i));hs.push(Math.round(k[i].offsetHeight/8));by[keys[i]]=k[i];}\n"
	"p.style.alignItems=\"\";o=planeOrder(keys,hs,p.clientWidth);\n"
	"for(i=0;i<o.length;i++)if(by[o[i]])p.appendChild(by[o[i]]);}\n"
	/* S214: @min is a FLOOR on the rows, padded with blank ones.  The plane
	 * is ordered by height on every redraw (sizePlane), so a card whose row
	 * count follows the traffic - commands in the last five minutes, the
	 * slow log, unknown commands - changed rank as rows came and went and
	 * jumped about the page.  A card that is always its full height has one
	 * place. */
	/* a row's third field: true for the warn colour, 'b' for the colour of
	 * trouble (S352b, a critical alert) */
	"function card(t,rows,cls,min){var h='<div class=\"pc'+(cls?' '+cls:'')+'\"><h3>'+t+\"</h3>\",z;\n"
	"for(z=0;z<rows.length;z++)h+='<div class=\"kv'+(rows[z][2]==='b'?\" b\":rows[z][2]?\" w\":\"\")+'\"><span>'+rows[z][0]+\n"
	"\"</span><span>\"+rows[z][1]+\"</span></div>\";\n"
	"for(;z<(min||0);z++)h+='<div class=\"kv\"><span>\\u00a0</span><span></span></div>';h+=\"</div>\";\n"
	"if(CARDS[t]===undefined)ORDER.push(t);CARDS[t]=h;return h;}\n"
	/* S214: which short cards share a cell.  Chosen by what they are about
	 * first and by height second - a pair or a trio comes out about as tall
	 * as the tall cards (durability-wal, pub/sub), so a grid row holds cells
	 * of one size with nothing half empty.  A title that is not on the plane
	 * is skipped, a group left with one card renders it bare, and a card no
	 * group names is appended on its own: a new card can never go missing
	 * because nobody added it here. */
	"var CARDS={},ORDER=[],GROUPS=[['memory','backpressure'],\n"
	"['unknown commands','resp door'],\n"
	"['pub/sub \\u00b7 fleet relay','durability \\u00b7 rdb'],['replication','replica intake'],\n"
	"['memory budget','cluster auth'],['listeners','native \\u00b7 binary'],\n"
	"['pulls','forwards'],['process','negative cache']];\n"
	"function compose(){var used={},h='',i,j,g,m,t;for(i=0;i<GROUPS.length;i++){g='';m=0;\n"
	"for(j=0;j<GROUPS[i].length;j++){t=GROUPS[i][j];if(CARDS[t]!==undefined){g+=CARDS[t];used[t]=1;m++;}}\n"
	"if(m)h+=m>1?'<div class=\"stk\">'+g+'</div>':g;}\n"
	"for(i=0;i<ORDER.length;i++)if(!used[ORDER[i]])h+=CARDS[ORDER[i]];return h;}\n"
	/* S194: each counter is sampled ONCE per poll - wv() pushes into the
	 * history, so calling it twice for one key would record two samples
	 * a poll and halve the window it reports. */
	"var W={};[['pull_sent',c.pull_sent],['pull_hits',c.pull_hits],\n"
	"['pull_misses',c.pull_misses],['pull_timeouts',c.pull_timeouts],\n"
	"['pull_served',c.pull_served],['fwd_sent',c.fwd_sent],\n"
	"['fwd_served',c.fwd_served],['fwd_no_route',c.fwd_no_route],\n"
	"['fwd_send_fail',c.fwd_send_fail],['repl_pushed',c.repl_pushed],\n"
	"['repl_out',c.repl_out],['repl_skipped_dying',c.repl_skipped_dying],\n"
	"['migrated_in',c.migrated_in],['migrated_out',c.migrated_out],\n"
	"['migrate_lost',c.migrate_lost],['rx_applied'],['rx_drops'],['neg_hits'],['tomb_sent'],\n"
	"['tomb_applied'],['tomb_displaced'],['pend_exhausted'],['bad_auth'],['lamport_rejected'],\n"
	"['term_rejected']].forEach(function(p){W[p[0]]=wv5(c,p[0]);});\n"
	"CARDS={};ORDER=[];void(\n"
	"card(\"pulls\",[[\"sent\",nf(W.pull_sent)],[\"hits\",nf(W.pull_hits)],\n"
	/* S191: a pull MISS is not a fault - it means the peer did not
	 * have the key either, which is an ordinary cache miss and the
	 * expected outcome for most pulls.  Highlighting any non-zero
	 * count turned the card amber for the life of the process and
	 * taught a reader to ignore the colour.  A TIMEOUT is a fault:
	 * the peer did not answer at all. */
	"[\"misses\",nf(W.pull_misses)],[\"timeouts\",nf(W.pull_timeouts),W.pull_timeouts>0],\n"
	"[\"served for peers\",nf(W.pull_served)]])+\n"
	"card(\"forwards\",[[\"sent\",nf(W.fwd_sent)],[\"served\",nf(W.fwd_served)],\n"
	"[\"no route\",nf(W.fwd_no_route),W.fwd_no_route>0],[\"send failures\",nf(W.fwd_send_fail),W.fwd_send_fail>0]])+\n"
	"card(\"replication\",[[\"pushed on write\",nf(W.repl_pushed)],[\"swept out\",nf(W.repl_out)],\n"
	"[\"repair skipped (dying)\",nf(W.repl_skipped_dying)],[\"migrated in\",nf(W.migrated_in)],\n"
	"[\"migrated out\",nf(W.migrated_out)],[\"lost\",nf(W.migrate_lost),W.migrate_lost>0],\n"
	/* S296: whether the rebalancer has anything to do - settled is the
	 * normal state of a fleet under steady load; leveling is shown */
	"[\"rebalance\",c.rebalance?(c.rebalance.state===\"settled\"?\"settled\"+(c.rebalance.settled_since>0?\" \"+dur(Math.max(0,Math.round(Date.now()/1000)-c.rebalance.settled_since)):\"\"):\"leveling\"):\"-\",!!(c.rebalance&&c.rebalance.state!==\"settled\")],\n"
	/* 0.6.2: the reason is a sentence - a line of its own under the
	 * rebalance row, read left to right, not a value squeezed right */
	"[c.rebalance&&c.rebalance.reason?'<span class=dim>'+esc(c.rebalance.reason)+'</span>':'<span class=dim>no reason given</span>',''],\n"
	/* S212: the clocks.  The repair sweep walks the whole table each
	 * cycle and the reconcile probes every own key after a restart;
	 * both are cheap at the fleet's keyspace and O(N) in it.  These rows
	 * are what say when that has stopped being true - durations of the
	 * last completed pass, so they read the same from every viewer. */
	"[\"last repair cycle\",ms(c.repl_sweep_ms)+\" \u00b7 \"+nf(c.repl_sweep_scanned)+\" seen, \"+nf(c.repl_sweep_sent)+\" sent\"],\n"
	/* S392, S395: open - how far it has got; ended by the fast path -
	 * how long and what the witness replayed; walked - why it walked */
	"[\"last reconcile pass\",c.reconcile_open?'<b>open</b> \u00b7 '+recline(c)+(c.reconcile_budget?' \u00b7 budget '+nf(c.reconcile_budget)+'/s':''):\n"
	"c.reconcile_fast?ms(c.reconcile_ms)+' \u00b7 fast path: '+nf(c.reconcile_fast_deletes)+' deletes and expiries replayed by a witness':\n"
	"c.reconcile_probed?ms(c.reconcile_ms)+\" \u00b7 \"+nf(c.reconcile_probed)+\" probed\"+(c.reconcile_pending?\", \"+nf(c.reconcile_pending)+\" open\":\"\")+\n"
	"(c.reconcile_fast_why?' \u00b7 <span class=dim>walked: '+esc(c.reconcile_fast_why)+'</span>':''):\"\u2014\"]])+\n"
	/* S125: the receive side, which nothing else on this page shows.  A
	 * node that cannot keep up with inbound replicas heartbeats
	 * normally, stays a member and answers its client door quickly, so
	 * every other card here reads green while it drifts out of date.
	 * The rate leads (what it IS doing), the queue says how close the
	 * buffer is to full, and drops are records already lost and waiting
	 * on the repair sweep - flagged, because that is the line between
	 * "behind" and "inconsistent until the sweep runs". */
	"card(\"replica intake\",[[\"applied /s\",nf(c.rx_applied_ps)],\n"
	"[\"applied\",nf(W.rx_applied)],\n"
	"[\"refused as older /s\",nf(c.rx_older_ps),c.rx_older_ps>0],\n"
	"[\"receive queue\",c.rx_rcvbuf?(sz(c.rx_queue)+\" of \"+sz(c.rx_rcvbuf)):sz(c.rx_queue),\n"
	"  !!(c.rx_rcvbuf&&c.rx_queue>c.rx_rcvbuf/2)],\n"
	"[\"DROPPED /s\",nf(c.rx_drops_ps),c.rx_drops_ps>0],\n"
	"[\"dropped\",nf(W.rx_drops),W.rx_drops>0]])+\n"
	"card(\"negative cache\",[[\"hits\",nf(W.neg_hits)],[\"tombstones sent\",nf(W.tomb_sent)],\n"
	"[\"applied\",nf(W.tomb_applied)],\n"
	/* RV-15: a tombstone evicted before it expired - the oldest go first */
	"[\"tombstones evicted early\",nf(W.tomb_displaced)]].concat(\n"
	/* AR15: the deletes kept for a member that comes back - how full, the
	 * window, and the ones the cap dropped before it */
	"c.tombstones_retained&&c.tombstones_retained.max?[[\"deletes kept\",nf(c.tombstones_retained.entries)+\" of \"+nf(c.tombstones_retained.max),\n"
	"  c.tombstones_retained.entries*10>c.tombstones_retained.max*9],\n"
	"[\"kept for\",nf(c.tombstones_retained.retain_s)+\" s\"],\n"
	"[\"dropped before that\",nf(c.tombstones_retained.dropped_early),c.tombstones_retained.dropped_early>0]]:[]))+\n"
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
	"[\"writes refused\",nf(wv5(m,'nomem')),wv5(m,'nomem')>0]]))+\n"
	"card(\"backpressure\",[[\"pending peak\",nf(wv5(c,'pend_peak'))],[\"pending max\",nf(c.pend_max)],\n"
	"[\"exhausted\",nf(W.pend_exhausted),W.pend_exhausted>0],[\"socket buffer\",mb(c.rcvbuf)+\" MB\"]])+\n"
	/* S120: what the collections cost, from the daemon's exact figures -
	 * each table's index regions and its records as the cells they occupy */
	"card(\"memory budget\",bud(s))+\n"
	"card(\"process\",[[\"uptime\",dur(pr.uptime_s)],[\"threads\",nf(pr.threads)],\n"
	"[\"cpu user\",wv5(pr,'cpu_user_ms')===null?\"-\":(wv5(pr,'cpu_user_ms')/1000).toFixed(1)+\"s\"],\n"
	"[\"cpu system\",wv5(pr,'cpu_sys_ms')===null?\"-\":(wv5(pr,'cpu_sys_ms')/1000).toFixed(1)+\"s\"],[\"pid\",nf(pr.pid)]])+\n"
	/* S89: the doors, from stats.listeners; a non-native door with no
	 * secret is the condition the startup WARNING names - show it */
	"card(\"listeners\",[].concat.apply([],(s.listeners||[]).map(function(l){return [[l.kind,\n"
	"(l.plaintext?\"plaintext\":\"encrypted\")+(l.allow?\" \\u00b7 allow-list \"+l.allow:\"\")+(l.secret?\"\":\" \\u00b7 NO SECRET\"),\n"
	"!l.secret&&l.kind!==\"native\"],['<span class=dim>'+esc(l.addr)+\":\"+l.port+'</span>','']];})))+\n"

	"card(\"native \\u00b7 binary\",[[\"open\",nf(nt.binary?nt.binary.open:null)],\n"
	"[\"connects (5 min)\",nf(wv5(nt.binary,'conns'))],\n"
	"[\"requests\",nf(wv5(nt.binary,'requests'))]])+\n"
	"card(\"resp door\",[[\"open\",nf(r.open)],[\"connects (5 min)\",nf(wv5(r,'conns'))],[\"requests\",nf(wv5(r,'requests'))],\n"
	"[\"rejected\",nf(wv5(r,'rejected')),wv5(r,'rejected')>0],\n"
	"[\"auth failures\",nf(wv5(r,'authfail')),wv5(r,'authfail')>0],\n"
	"[\"resp via native door\",nf(wv5(nt.resp,'requests'))]])+\n"
	"unkcard(s)+\n"
	/* PS7: the pub/sub card.  Subscribers, channels and patterns are
	 * gauges (what is attached NOW); the rest are totals.  A relay gap
	 * and a publish too large to relay are both "a subscriber on another
	 * node did not hear it", so they carry the warn colour - they are the
	 * only figures here that mean something is wrong. */
	"(!s.pubsub?\"\":card(\"pub/sub\",[[\"subscribers\",nf(s.pubsub.subscribers)],\n"
	"[\"channels\",nf(s.pubsub.channels)],[\"patterns\",nf(s.pubsub.patterns)],\n"
	"[\"published\",nf(wv5(s.pubsub,'published'))],[\"delivered locally\",nf(wv5(s.pubsub,'delivered'))],\n"
	"[\"keyspace events\",nf(wv5(s.pubsub,'keyspace_events'))],\n"
	"[\"slow subscribers closed\",nf(wv5(s.pubsub,'slow_kills')),wv5(s.pubsub,'slow_kills')>0],\n"
	"[\"deliveries lost to allocation\",nf(wv5(s.pubsub,'alloc_failed')),wv5(s.pubsub,'alloc_failed')>0],\n"
	"[\"dropped at a full worker queue\",nf(wv5(s.pubsub,'queue_dropped')),wv5(s.pubsub,'queue_dropped')>0],\n"
	"[\"publishers paused on a full queue\",nf(wv5(s.pubsub,'publish_paused'))],\n"
	"[\"queued now\",sz(s.pubsub.queue_bytes)],\n"
	"[\"UDP push streams\",nf(s.pubsub.udp?s.pubsub.udp.streams:null)],\n"
	"[\"UDP streams pruned\",nf(s.pubsub.udp&&wv5(s.pubsub.udp,'pruned_no_ack')!==null?wv5(s.pubsub.udp,'pruned_no_ack')+(wv5(s.pubsub.udp,'pruned_no_progress')||0):null),!!(s.pubsub.udp&&(wv5(s.pubsub.udp,'pruned_no_ack')||0)+(wv5(s.pubsub.udp,'pruned_no_progress')||0)>0)]])+\n"
	"card(\"pub/sub \\u00b7 fleet relay\",[[\"sent to peers\",nf(wv5(s.pubsub,'relay_sent'))],\n"
	"[\"received from peers\",nf(wv5(s.pubsub,'relay_recv'))],\n"
	"[\"lost (left the window unseen)\",nf(wv5(s.pubsub,'relay_lost')),wv5(s.pubsub,'relay_lost')>0],\n"
	"[\"duplicates dropped\",nf(wv5(s.pubsub,'relay_duplicates')),wv5(s.pubsub,'relay_duplicates')>0],\n"
	"[\"too large to relay\",nf(wv5(s.pubsub,'relay_dropped')),wv5(s.pubsub,'relay_dropped')>0],\n"
	"[\"relays this node asks for\",s.pubsub.relay_mode||\"all\"],\n"
	"[\"not sent: the peer holds no match\",nf(wv5(s.pubsub,'relay_skipped'))]]))+\n"
	"card(\"cluster auth\",[[\"bad auth\",nf(W.bad_auth),W.bad_auth>0],\n"
	"[\"lamport rejected\",nf(W.lamport_rejected),W.lamport_rejected>0],\n"
	"[\"term rejected\",nf(W.term_rejected),W.term_rejected>0],\n"
	"[\"map refused\",nf(wv5(c.map,'refused')),wv5(c.map,'refused')>0]])+\n"
	/* S91: durability was one line saying "on".  stats already carries
	 * the fsync mode, the startup probe, the OBSERVED fsync latency, the
	 * loss counters and the whole RDB save record - an operator who has
	 * just turned the WAL on cannot see any of it.  Two cards, and the
	 * three things that mean something is wrong are flagged: dropped
	 * (acknowledged writes that never reached the log), overruns, and a
	 * probe that underestimated the device. */
	"card(\"durability \\u00b7 wal\", !s.wal ? [[\"wal\",\"off\"]] : [\n"
	"[\"fsync\",s.wal.fsync||\"-\"],\n"
	"[\"records appended\",nf(wv5(s.wal,'appended'))],\n"
	"[\"bytes appended\",sz(wv5(s.wal,'bytes'))],\n"
	"[\"DROPPED (acked, unlogged)\",nf(wv5(s.wal,'dropped')),wv5(s.wal,'dropped')>0],\n"
	/* wal.late: records written behind a higher sequence - the WAL
	 * thread drains the worker rings one after another, so they
	 * interleave.  Harmless since S240 (replay is by version), so it
	 * is never amber: it read as an alarm on a healthy fleet. */
	"[\"reordered (harmless: replay is by version)\",nf(wv5(s.wal,'late'))],\n"
	"[\"segment overruns\",nf(wv5(s.wal,'overruns')),wv5(s.wal,'overruns')>0],\n"
	/* S306: heals since start and in the last 10 minutes; amber when
	 * that many means the log is undersized (heal_routine), not when a
	 * single burst once outran the ring */
	"[\"heals\",nf(wv5(s.wal,'heals'))+(s.wal.heal_routine?\" \\u2014 routine: log undersized\":\"\"),\n"
	"  !!s.wal.heal_routine],\n"
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
	/* S350: in the unit a reader thinks in - "104,010 us" and
	 * "1,488,827 us" were read off PROD during a backup; lat() is the
	 * commands card's us / ms / s */
	"[\"fsync now (ewma)\",s.wal.observed?\n"
	"  lat(s.wal.observed.fsync_recent_us):\"\\u2014\"],\n"
	/* S351: the mean and the worst of the last five minutes - the
	 * daemon's window of the count and the sum, and its five-minute
	 * maximum.  A maximum since start is a number from whenever the
	 * daemon began - a week ago - on a page that reads as now. */
	"[\"fsync (avg / max)\",s.wal.observed?\n"
	"  ((wv5(s.wal.observed,'fsync_n')>0?lat(wv5(s.wal.observed,'fsync_sum_us')/wv5(s.wal.observed,'fsync_n')):\"\\u2014\")+\" / \"+\n"
	"  (wv5(s.wal.observed,'fsync_max_us')?lat(wv5(s.wal.observed,'fsync_max_us')):\"\\u2014\")):\"\\u2014\"],\n"
	"[\"probe p50/p99\",s.wal.probe?\n"
	"  (lat(s.wal.probe.fsync_p50_us)+\" / \"+lat(s.wal.probe.fsync_p99_us)+\", \"+nf(s.wal.probe.seq_mb_s)+\" MB/s\"):\"\\u2014\"],\n"
	"[\"probe underestimated\",(s.wal.observed&&s.wal.observed.probe_underestimated)?\"YES\":\"no\",\n"
	"  !!(s.wal.observed&&s.wal.observed.probe_underestimated)]])+\n"
	"card(\"durability \\u00b7 rdb\", !s.rdb ? [[\"rdb\",\"off\"]] : [\n"
	"[\"snapshots\",nf(wv5(s.rdb,'saves'))],\n"
	"[\"running now\",s.rdb.running?\"yes\":\"no\"],\n"
	"[\"last snapshot\",s.rdb.last_unix?dur(Math.max(0,Math.floor(Date.now()/1000)-s.rdb.last_unix))+\" ago\":\"never\",\n"
	"  !s.rdb.last_unix],\n"
	"[\"last size\",sz(s.rdb.last_bytes)],\n"
	"[\"last duration\",(s.rdb.last_dur_ms===undefined||s.rdb.last_dur_ms===null)?\"\\u2014\":lat(1000*s.rdb.last_dur_ms)]]));\n"
	"document.getElementById(\"plane\").innerHTML=compose();\n"
	"drawHot(s);drawCmds(s);drawSlow(s);drawAlerts(s);sizePlane();trend();}\n"
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
	/* S195: a node restarting under a deploy drops one poll, and a
	 * single miss used to paint "this node did not answer" over a
	 * page whose cards were three seconds old.  Two consecutive
	 * misses is a node worth reporting, and the banner then says
	 * for how long rather than in the abstract. */
	"if(!s){if(++MISS>1)document.getElementById(\"alert\").innerHTML='<p class=err>'+\n"
	"(last?'this node has not answered for '+dur(Math.round(MISS*POLL/1000)):'waiting for the first reply from this node')+'</p>';return;}\n"
	"MISS=0;document.getElementById(\"alert\").innerHTML=\"\";\n"
	"try{drawStats(s);last=Date.now();}catch(e){document.getElementById(\"alert\").innerHTML=\n"
	"'<p class=err>the page failed to render: '+esc((e&&e.message)||e)+'</p>';}});\n"
	"get(\"members\",function(d){if(d&&d.members){FLINFO=d.fleet||null;drawFleet(d.members);}});if(CLOPEN)loadClients();\n"
	"if(TOOL)get(\"fleet\",function(d){if(d&&d.members)drawFleetTab(d.members,d.distinct);});}\n"
	/* 12nx (the operator, 10-19: one column per collection "won't fit at all"
	 * with 64 or 128 of them): the fleet table describes the NODES - each
	 * member's records and Redis copies summed over its collections - and
	 * the per-collection, per-member figures moved under each collection
	 * (memr).  The fleet row adds the collections up by each one's own
	 * mode rule (COLF, this node's fleet figure; the sketch estimate where
	 * store mode has one), so it agrees with the collections table. */
	/* 12nx: the union's estimate only when every reporting member sent its
	 * sketch - a partial union can count low.  Top level, beside
	 * drawFleetTab: entc's scope is not the fleet table's (10-19, it was
	 * defined beside entc and drawFleetTab threw 'dfull is not defined') */
	"function dfull(n){var d=FLD&&FLD[n];return d&&d.estimate!=null&&d.members===d.of?d:null;}\n"
	"function drawFleetTab(ms,dist){var w=document.getElementById(\"fleetwrap\"),t=document.getElementById(\"fleettab\"),cps={},fe=0,fc=0,fhc=false,mark='',seen={};\n"
	"FLM=ms;FLD=dist||null;\n"
	"var h='<thead><tr><th>node</th><th>member</th><th>version</th><th>install</th><th>staged</th><th class=n title=\"the records this member holds, over all its collections\">records</th><th class=n title=\"the read-through copies of Redis values this member holds\">Redis copies</th></tr></thead><tbody>';\n"
	"ms.slice().sort(function(a,b){return a.node-b.node;}).forEach(function(m){var e=0,cp=0,hc=false;(m.collections||[]).forEach(function(c){seen[c.name]=1;e+=c.entries>0?c.entries:0;if(c.copies!==undefined){hc=true;cp+=c.copies;cps[c.name]=(cps[c.name]||0)+c.copies;}});fc+=cp;fhc=fhc||hc;\n"
	"h+='<tr><td>'+esc(m.node)+'</td><td><span class=nm>'+esc(m.addr)+':'+esc(m.port)+'</span>'+(m.state!=='connected'?' <span style=\"color:var(--bad)\">'+esc(m.state)+'</span>':'')+'</td><td>'+esc(m.version||'?')+(m.rev?' <span class=sm>'+esc(m.rev)+'</span>':'')+'</td><td>'+esc(m.install_mode||'?')+'</td><td>'+esc(m.staged||'-')+'</td><td class=n>'+nf(e)+'</td><td class=n>'+(hc?nf(cp):'\\u2014')+'</td></tr>';});\n"
	"Object.keys(seen).forEach(function(n){var d=dfull(n);\n"
	"if(COLB[n]==='at_least'&&d){fe+=d.estimate;if(mark!=='\\u2265 ')mark='\\u2248 ';}else{fe+=COLF[n]||0;if(COLB[n]==='at_least')mark='\\u2265 ';}});\n"
	"h+='<tr class=tot><td></td><td><span class=nm>fleet</span></td><td colspan=3 class=sm>'+ms.length+' members</td><td class=n title=\"each collection by its mode\\'s rule, as the collections table counts it\"><b>'+mark+nf(Math.round(fe))+'</b></td><td class=n><b>'+(fhc?nf(fc):'\\u2014')+'</b></td></tr>';\n"
	"FLCOPIES={n:ms.length,c:cps};t.innerHTML=h+'</tbody>';w.hidden=false;}\n"
	"document.getElementById('metrics').addEventListener('click',function(e){var t=e.target;while(t&&t.id!=='clmetric')t=t.parentNode;if(!t)return;\n"
	"CLOPEN=!CLOPEN;document.getElementById('clpanel').hidden=!CLOPEN;if(CLOPEN)loadClients();});\n"
	"document.getElementById('clfilter').oninput=function(){drawClients();};\n"
	/* S185: the commands table opens on the label, and on the one row
	 * it shows - a reader who clicks the row means the same thing.
	 * The label is rewritten by the next poll; doing it here too
	 * keeps the click from looking like it did nothing for 3 s. */
	"function cmdlabel(open){var n=CMDROWS?CMDROWS.n:0;return !n?'nothing in the last 5 min':n<2?'one verb in the last 5 min':open?'showing all '+n+' - click to fold':(CMDSORT==='calls'?'the busiest':'the slowest')+' of '+n+' verbs - click for all';}\n"
	"function hklabel(open){return !HKN?'none':HKN<2?'one key':open?'showing all '+HKN+' - click to fold':'the hottest of '+HKN+' - click for all';}\n"
	"function sllabel(open){return !SLN?'empty':SLN<2?'one entry':open?'showing all '+SLN+' - click to fold':'the newest of '+SLN+' - click for all';}\n"
	"function allabel(open){return !ALN?'nothing yet':open?(ALN<2?'showing the one entry':'showing all '+ALN)+' - click to fold':ALN<2?'one entry - click to show':ALN+' entries - click for all';}\n"
	"function cmdshow(){var t=document.getElementById('cmds'),w=document.getElementById('cmdwrap'),g=document.getElementById('cmdtog'),a=document.getElementById('cmdbycalls'),b=document.getElementById('cmdbyslow');\n"
	"if(!CMDROWS)return;t.innerHTML=CMDROWS[CMDSORT]||CMDROWS.slow;if(a)a.className='n srt'+(CMDSORT==='calls'?' on':'');if(b)b.className='n srt'+(CMDSORT==='calls'?'':' on');\n"
	"if(g&&w)g.textContent=cmdlabel(w.classList.contains('open'));}\n"
	"function cmdfold(){var w=document.getElementById('cmdwrap'),t=document.getElementById('cmdtog');\n"
	"w.classList.toggle('open');t.textContent=cmdlabel(w.classList.contains('open'));}\n"
	"document.getElementById('cmdtog').addEventListener('click',cmdfold);\n"
	"document.getElementById('cmdtog').addEventListener('keydown',function(e){if(e.key==='Enter'||e.key===' '){e.preventDefault();cmdfold();}});\n"
	"document.getElementById('cmdwrap').addEventListener('click',function(e){\n"
	"if(!document.getElementById('cmdwrap').classList.contains('open')&&e.target.closest('tbody'))cmdfold();});\n"
	"document.getElementById('cmdbycalls').addEventListener('click',function(){CMDSORT='calls';cmdshow();});\n"
	"document.getElementById('cmdbyslow').addEventListener('click',function(){CMDSORT='slow';cmdshow();});\n"
	"function foldwire(wid,tid,lab){var W=document.getElementById(wid),T=document.getElementById(tid);\n"
	"function f(){W.classList.toggle('open');T.textContent=lab(W.classList.contains('open'));}\n"
	"T.addEventListener('click',f);T.addEventListener('keydown',function(e){if(e.key==='Enter'||e.key===' '){e.preventDefault();f();}});\n"
	"W.addEventListener('click',function(e){if(!W.classList.contains('open')&&e.target.closest('tbody'))f();});}\n"
	/* 12nx: a collection's per-member rows open and close in place; the
	 * choice survives the redraws (COLOPEN) */
	"function mtogf(e){var t=e.target.closest&&e.target.closest('.mtog');if(!t)return;if(e.type==='keydown'&&e.key!=='Enter'&&e.key!==' ')return;e.preventDefault();var n=t.getAttribute('data-col');COLOPEN[n]=!COLOPEN[n];\n"
	"t.textContent=(COLOPEN[n]?'\\u25be':'\\u25b8')+' members';document.querySelectorAll('tr.mrow').forEach(function(r){if(r.getAttribute('data-col')===n)r.hidden=!COLOPEN[n];});}\n"
	"document.getElementById('cols').addEventListener('click',mtogf);document.getElementById('cols').addEventListener('keydown',mtogf);\n"
	"foldwire('hkwrap','hktog',hklabel);foldwire('slwrap','sltog',sllabel);foldwire('alwrap','altog',allabel);\n"
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
	"function go(){post(\"reset-stats\",acted(go,\"the node refused the reset\"));}withsess(go);};\n"
	/* 12no: the sweep buttons on the collections table (one listener, the
	 * rows are rewritten every poll); the first poll waits for the probe
	 * so the node switch is drawn right the first time */
	"document.getElementById(\"cols\").addEventListener(\"click\",function(e){var t=e.target;while(t&&t!==this&&!(t.classList&&t.classList.contains(\"act\")))t=t.parentNode;\n"
	"if(!t||t===this)return;var op=t.getAttribute(\"data-op\"),col=t.getAttribute(\"data-col\");\n"
	"if(t.classList.contains(\"ddl\")){ddl(op,col,t.getAttribute(\"data-ent\"));return;}\n"
	"if(op==='upfill'&&!confirm('Write the keys this fleet owned before write-through to Redis - collection '+col+'?'))return;\n"
	"if(op==='drain'&&!confirm('Take over every key the upstream still holds for collection '+col+' - read them all into this fleet?'))return;\n"
	"function go(){post(\"action\",acted(go,'the node refused the '+op),JSON.stringify({op:op,col:col,node:Number(NODEQ)||(TOOL&&TOOL.node)||0}));}withsess(go);});\n"
	/* S412: the DDL from the page - create, drop (with the entry count in
	 * the confirmation), resize (a background migration), rename, map and
	 * unmap (S432) - each
	 * the daemon's own verb on the selected member, through the tool */
	"function ddl(op,col,ent){var body={op:op,col:col,node:Number(NODEQ)||(TOOL&&TOOL.node)||0};\n"
	"if(op==='drop'){if(!confirm('Drop collection '+col+' and its '+nf(Number(ent)||0)+' entries on this fleet?'))return;body.force=1;}\n"
	"else if(op==='rename'){var to=prompt('New name for collection '+col+':');if(!to)return;body.to=to;}\n"
	/* S432: a RESP DB index onto this collection, fleet-wide; unmap's
	 * data-col carries the index */
	"else if(op==='map'){var ix=prompt('RESP DB index (0..63) to answer as collection '+col+' on every member - every connection sees it on its next command:');if(ix===null||!/^\\d{1,2}$/.test(ix.trim()))return;body.index=Number(ix.trim());}\n"
	"else if(op==='unmap'){if(!confirm('Unmap RESP DB index '+col+'? It answers as resp_collections says again, on every member.'))return;body.index=Number(col);delete body.col;}\n"
	"else if(op==='resize'){var b=prompt('New size for collection '+col+', as buckets_log2 (4..24) - a background migration:');if(!b)return;body.buckets_log2=Number(b);}\n"
	"else if(op==='create'){var n=prompt('New collection name:');if(!n)return;var l=prompt('Its size as buckets_log2 (12 = 4,096 buckets):','12');body.col=n;body.buckets_log2=Number(l||12);}\n"
	"function go(){post(\"ddl\",acted(go,'the node refused the '+op),JSON.stringify(body));}withsess(go);}\n"
	"document.getElementById(\"newcol\").addEventListener(\"click\",function(){ddl('create','',0);});\n"
	"get(\"session\",function(d){if(d&&d.tool){TOOL=d;document.getElementById(\"newcol\").hidden=false;document.getElementById(\"newcolsep\").hidden=false;\n"
	"document.getElementById(\"stagewrap\").hidden=false;document.getElementById(\"stagenode\").textContent=\"node \"+(Number(NODEQ)||d.node);\n"
	"document.getElementById(\"stagebtn\").addEventListener(\"click\",stageBuild);\n"
	"document.getElementById(\"stagefile\").addEventListener(\"change\",function(){var m=/(\\d+\\.\\d+\\.\\d+(?:-rc\\d+)?)/.exec(this.files[0]?this.files[0].name:\"\"),v=document.getElementById(\"stagever\");if(m&&!v.value)v.value=m[1];});}tick();});setInterval(tick,POLL);\n"
	"</script>\n"
	"\n"
	;
const unsigned int pc_status_page_len = sizeof pc_status_page - 1;
