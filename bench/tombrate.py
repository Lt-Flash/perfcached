#!/usr/bin/env python3
"""tombrate.py - what a fleet's retained-delete table holds, and why
(12ny AT-0: measure before acknowledged tombstones are built).

Reads each node's /stats twice, INTERVAL seconds apart, and splits what
the node notes into the retained-delete table:

  deletes   client deletes taken at this node (the del-family verbs in
            commands[]), and peers' deletes applied here (tomb_applied);
  expiries  keys this node reaped by their TTL (collections[].expired) -
            on an eager fleet every member reaps every key itself, so
            each one notes the same expiry once.

and prints, per node, the rates, the table (entries, bytes an entry,
dropped before tombstone_retain_s) and what a full window of that rate
would hold.  From 0.7.2 on the table counts its expiries itself
(S443: expiries, noted, noted_expiries); an older daemon has only the
rates, and the split is estimated from them - the output says which.

  tombrate.py [-i INTERVAL] SOURCE...

A SOURCE prints one /stats document (bare, or as a JSON-RPC answer):
  http://host:port[/stats]   bearer token from $PC_HTTP_TOKEN_FILE
  cmd:<shell command>        e.g. cmd:ssh node1 perfcli -q -j
                             '{"method":"stats"}'
Read-only: /stats and nothing else.  A node that restarted, or crossed
the daily stats reset (S358), between the two reads is reported and
left out.
"""
import json, os, subprocess, sys, time, urllib.request

DEL_VERBS = ("del", "unlink", "delifeq", "getdel", "mdel")


def fetch(src):
    if src.startswith("cmd:"):
        out = subprocess.run(src[4:], shell=True, capture_output=True,
                             timeout=60, check=True).stdout
        s = json.loads(out)
    else:
        url = src if src.rstrip("/").endswith("/stats") else src.rstrip("/") + "/stats"
        rq = urllib.request.Request(url)
        tf = os.environ.get("PC_HTTP_TOKEN_FILE")
        if tf:
            rq.add_header("Authorization", "Bearer " + open(tf).read().strip())
        s = json.load(urllib.request.urlopen(rq, timeout=20))
    return s.get("result", s) if isinstance(s, dict) else s


def dels(s):
    return sum(c.get("calls", 0) for c in s.get("commands") or []
               if c.get("verb") in DEL_VERBS)


def expired(s):
    return sum(c.get("expired", 0) for c in s.get("collections") or [])


def cl(s, k):
    return (s.get("cluster") or {}).get(k, 0) or 0


def rt(s):
    return (s.get("cluster") or {}).get("tombstones_retained") or {}


def main(argv):
    iv = 60
    if len(argv) > 2 and argv[1] == "-i":
        iv, argv = float(argv[2]), argv[:1] + argv[3:]
    srcs = argv[1:]
    if not srcs:
        sys.exit(__doc__)
    a = [(fetch(x), time.time()) for x in srcs]
    time.sleep(iv)
    b = [(fetch(x), time.time()) for x in srcs]
    tot = {"del": 0.0, "app": 0.0, "exp": 0.0}
    for src, (s0, t0), (s1, t1) in zip(srcs, a, b):
        name = next((w.split("@")[-1] for w in src.split() if "@" in w), src)
        up0 = (s0.get("process") or {}).get("uptime_s", 0)
        up1 = (s1.get("process") or {}).get("uptime_s", 0)
        rs0 = (s0.get("since") or {}).get("reset_at")
        rs1 = (s1.get("since") or {}).get("reset_at")
        if up1 < up0 or rs0 != rs1:
            print("%s: restarted or reset between the reads - left out" % name)
            continue
        dt = t1 - t0
        r_del = (dels(s1) - dels(s0)) / dt
        r_app = (cl(s1, "tomb_applied") - cl(s0, "tomb_applied")) / dt
        r_exp = (expired(s1) - expired(s0)) / dt
        r1, r0 = rt(s1), rt(s0)
        r_drop = (r1.get("dropped_early", 0) - r0.get("dropped_early", 0)) / dt
        n, by = r1.get("entries", 0), r1.get("bytes", 0)
        win = r1.get("retain_s", 0)
        noted = r_del + r_app + r_exp
        print("%s (%s): %.1f s" % (name, s1.get("version"), dt))
        print("  noted /s       %8.1f = client deletes %.1f + peers' deletes applied %.1f + expiries reaped %.1f"
              % (noted, r_del, r_app, r_exp))
        if "expiries" in r1:
            share = "%.1f%% of the entries are expiries (counted, S443)" % (
                100.0 * r1["expiries"] / n if n else 0)
        else:
            share = "~%.1f%% of what is noted is expiries (estimated from the rates)" % (
                100.0 * r_exp / noted if noted else 0)
        print("  retained       %8d entries, %d B each, cap %d, window %d s; %s"
              % (n, by // n if n else 0, r1.get("max", 0), win, share))
        print("  a full window  %8d entries at this rate; dropped early %.1f /s"
              % (noted * win, r_drop))
        tot["del"] += r_del; tot["app"] += r_app; tot["exp"] += r_exp
    print("fleet: client deletes %.1f /s (each noted by every member), expiries reaped %.1f /s summed over the members"
          % (tot["del"], tot["exp"]))


if __name__ == "__main__":
    main(sys.argv)
