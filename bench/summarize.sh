#!/bin/sh
# summarize.sh — turn a respbench/containerbench results.tsv into the
# comparison table a reader actually wants: every arm against the redis
# reference at the SAME clients and pipeline depth.
#
# Kept separate and shared by both harnesses so the ratio arithmetic
# exists once.  Both write the same columns, so both can use it, and a
# results file from either can be re-summarised later without re-running
# anything:
#
#   bench/summarize.sh /var/tmp/respbench/results.tsv
#
# Ratios are ARM / REDIS at the same cell.  For latency the ratio is
# inverted and labelled "tighter", because lower is better there and
# printing x0.22 for a 4.6x improvement reads as a regression.
set -u
F=${1:-/var/tmp/respbench/results.tsv}
[ -r "$F" ] || { echo "summarize: cannot read $F" >&2; exit 1; }

grep '^#' "$F" | sed 's/^/  /'
awk -F'\t' '
	NR <= 2 || $1 ~ /^#/ { next }
	{
		cell = $2 "\t" $3
		arm  = $1
		if (!(cell in seen)) { order[++n] = cell; seen[cell] = 1 }
		if (!(arm in armseen)) { armorder[++na] = arm; armseen[arm] = 1 }
		set[cell, arm] = $4; get[cell, arm] = $5
		sp99[cell, arm] = $8; gp99[cell, arm] = $9
	}
	function rat(a, b) { return (b + 0 > 0 && a + 0 > 0) ? sprintf("x%.2f", a / b) : "  -  " }
	function tight(a, b) { return (a + 0 > 0 && b + 0 > 0) ? sprintf("%.1fx", a / b) : " - " }
	function comma(v,   s, out, i) {
		if (v + 0 <= 0) return "-"
		s = sprintf("%d", v); out = ""
		while (length(s) > 3) {
			out = "," substr(s, length(s) - 2) out
			s = substr(s, 1, length(s) - 3)
		}
		return s out
	}
	END {
		for (i = 1; i <= n; i++) {
			cell = order[i]
			split(cell, c, "\t")
			# a cell with no redis row cannot be compared
			if (!((cell SUBSEP "redis") in set)) continue
			# nor is there anything to show when redis is the ONLY
			# arm measured so far - which is the normal state while
			# a run is in flight, since redis goes first.  Printing
			# a headed, empty table for every cell buries the rows
			# that do exist.
			ncmp = 0
			for (j = 1; j <= na; j++) {
				a2 = armorder[j]
				if (a2 == "redis") continue
				if ((cell SUBSEP a2) in set || (cell SUBSEP a2) in get)
					ncmp++
			}
			if (ncmp == 0) { skipped++; continue }
			rs = set[cell, "redis"]; rg = get[cell, "redis"]
			rsp = sp99[cell, "redis"]; rgp = gp99[cell, "redis"]
			printf "\n  %d clients, pipeline %d   (redis: SET %s/s  GET %s/s  p99 %s ms)\n", \
				c[1], c[2], comma(rs), comma(rg), rsp
			printf "    %-22s %12s %8s %12s %8s %10s %9s\n", \
				"arm", "SET/s", "vs", "GET/s", "vs", "p99 ms", "tail"
			for (j = 1; j <= na; j++) {
				arm = armorder[j]
				if (arm == "redis") continue
				if (!((cell SUBSEP arm) in set) && !((cell SUBSEP arm) in get)) continue
				# A GET-only arm has no SET p99, so fall back to its GET
				# p99 rather than printing "-".  The cold arms are
				# GET-only BY DESIGN and their latency is the point of
				# them: proxy-cold at 10.6 ms against shard-cold at
				# 1.2 ms is the headline, and a summary that blanks it
				# is worse than no summary.
				p99v = sp99[cell, arm]; p99r = rsp
				if (p99v == "") { p99v = gp99[cell, arm]; p99r = rgp }
				printf "    %-22s %12s %8s %12s %8s %10s %9s\n", \
					arm, comma(set[cell, arm]), rat(set[cell, arm], rs), \
					comma(get[cell, arm]), rat(get[cell, arm], rg), \
					(p99v == "" ? "-" : p99v), tight(p99r, p99v)
			}
		}
		print ""
		if (skipped > 0)
			printf "  (%d cell(s) have only the redis reference so far - a run in\n  flight measures redis first; re-run this when more arms land.)\n\n", skipped
		print "  vs = arm / redis at the same cell; >x1 is faster."
		print "  tail = how many times TIGHTER the SET p99 is; >1x is better."
		print "  A cold arm reads only: SET columns empty, and its p99"
		print "  is the GET p99, compared against the redis GET p99."
	}' "$F"
