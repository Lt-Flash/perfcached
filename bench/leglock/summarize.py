#!/usr/bin/env python3
# summarize.py <results dir> - medians over rounds per (N, mode, arm, config) from summary.txt + the stats snapshots
import json, os, re, statistics, sys
from collections import defaultdict

R = sys.argv[1]
CFGS = ["fix9", "auto", "big"]
ARMS = ["fill", "hit1", "hit16", "miss1", "miss16", "mix16", "new16"]
runs = defaultdict(list)          # (N, mode, arm, cfg) -> [dict]
line = re.compile(r"^r(\d+)\.(\w+)\.(\w+)\.n(\d+) (\w+) \| (.*)$")
for l in open(os.path.join(R, "summary.txt")):
    m = line.match(l.strip())
    if not m:
        continue
    r, mode, cfg, n, arm, rest = m.groups()
    d = {"round": int(r)}
    if arm == "fill":
        ms = re.search(r"\| (\d+) ms", rest)
        d["ops"] = int(n) / (int(ms.group(1)) / 1000.0) if ms else None
    else:
        o = re.search(r": (\d+) ops/s", rest)
        if not o:
            d["fail"] = rest
        else:
            d["ops"] = int(o.group(1))
        for k in ("p50", "p99", "p999"):
            x = re.search(k + r"=(\d+)us", rest)
            if x:
                d[k] = int(x.group(1))
        c = re.search(r"n1 cpu (\d+) ms over (\d+) ms", rest)
        if c and int(c.group(2)):
            d["cores"] = int(c.group(1)) / int(c.group(2))
    runs[(int(n), mode, arm, cfg)].append(d)

def coll(path):
    try:
        s = json.load(open(path))
    except Exception:
        return None
    return next((c for c in s.get("collections", []) if c["name"] == "th"), None)

def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None

def fmt(x, unit=""):
    if x is None:
        return "-"
    if x >= 1e6:
        return "%.2fM%s" % (x / 1e6, unit)
    if x >= 1e3:
        return "%.0fk%s" % (x / 1e3, unit)
    return "%.0f%s" % (x, unit)

Ns = sorted({k[0] for k in runs})
modes = [m for m in ("eager", "local") if any(k[1] == m for k in runs)]
for N in Ns:
    # the table each config had after the fill (round 1, n1)
    print("\n## %d keys - table after the fill (n1, every round)" % N)
    for mode in modes:
        for cfg in CFGS:
            rows = []
            for r in (1, 2, 3):
                c = coll(os.path.join(R, "r%d.%s.%s.n%d.filled.n1.json" % (r, mode, cfg, N)))
                e = coll(os.path.join(R, "r%d.%s.%s.n%d.end.n1.json" % (r, mode, cfg, N)))
                if c:
                    rows.append("r%d %d buckets, %d of %d in the leg (%.1f%%), end %d/%d" % (r, c["buckets"], c["overflow"], c["entries"],
                        100.0 * c["overflow"] / max(1, c["entries"]), e["overflow"] if e else -1, e["entries"] if e else -1))
            print("  %-5s %-4s %s" % (mode, cfg, "; ".join(rows)))
    for mode in modes:
        print("\n## %d keys, %s - median of rounds (min..max), ratio to big" % (N, mode))
        print("| arm | " + " | ".join(CFGS) + " | fix9/big | auto/big | n1 cores fix9 / auto / big |")
        print("|---|" + "---|" * (len(CFGS) + 3))
        for arm in ARMS:
            cells, meds, cores = [], {}, []
            for cfg in CFGS:
                xs = runs.get((N, mode, arm, cfg), [])
                ops = [d.get("ops") for d in xs]
                mm = med(ops)
                meds[cfg] = mm
                fails = sum(1 for d in xs if "fail" in d)
                lat = ""
                if arm.endswith("1") and arm != "fill":
                    lat = " p50 %s/p99 %s us" % (med([d.get("p50") for d in xs]), med([d.get("p99") for d in xs]))
                rng = "(%s..%s)" % (fmt(min(o for o in ops if o is not None)), fmt(max(o for o in ops if o is not None))) if any(o is not None for o in ops) else ""
                cells.append("%s %s%s%s" % (fmt(mm), rng, lat, " FAIL x%d" % fails if fails else ""))
                cores.append("%.1f" % med([d.get("cores") for d in xs]) if med([d.get("cores") for d in xs]) is not None else "-")
            r1 = "%.2f" % (meds["fix9"] / meds["big"]) if meds.get("fix9") and meds.get("big") else "-"
            r2 = "%.2f" % (meds["auto"] / meds["big"]) if meds.get("auto") and meds.get("big") else "-"
            print("| %s | %s | %s | %s | %s |" % (arm, " | ".join(cells), r1, r2, " / ".join(cores)))
