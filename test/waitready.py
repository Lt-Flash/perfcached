#!/usr/bin/env python3
# waitready.py <host:port>... [-t seconds] - wait until every node's /stats
# says state "ready" (the native door), then return 0; 1 when one does not
# within the time (15 s), naming it.
#
# A node that has just started reports "perfcached ready", joins, and the
# fleet's figures - peers up, the acks gate confirming - read as formed while
# its own state is still RECOVERING: every verb that touches data answers
# "node is not READY (recovering) - retry, or use another member"
# (serving_denied, verbs.c) for a beat or a few.  A suite that writes the
# moment the fleet looks formed loses those writes and goes on testing
# nothing (v0.7.3-rc3..rc6: tombjointest, tombrebuildtest, tombacktest,
# tombmcasttest).  When it had to wait it says how long, on one line.
import socket, sys, time
import pcnative

def state(hp):
    h, p = hp.rsplit(":", 1)
    try:
        f = pcnative.wrap(socket.create_connection((h, int(p)), 5))
        return (pcnative.call(f, "stats").get("result") or {}).get("state", "?")
    except Exception as e:
        return "unreachable (%s)" % e.__class__.__name__

args = sys.argv[1:]
limit = 15.0
if "-t" in args:
    i = args.index("-t")
    limit = float(args[i + 1])
    del args[i:i + 2]
t0 = time.time()
seen = {}
while True:
    st = {hp: state(hp) for hp in args}
    for hp, s in st.items():
        if s != "ready":
            seen.setdefault(hp, s)
    if all(s == "ready" for s in st.values()):
        if seen:
            print("  ..   waited %.1f s for %s to be ready (%s)" % (time.time() - t0,
                  ", ".join(sorted(seen)), ", ".join("%s was %s" % (k, v) for k, v in sorted(seen.items()))))
        sys.exit(0)
    if time.time() - t0 > limit:
        print("  ..   not ready after %.0f s: %s" % (limit, ", ".join("%s %s" % kv for kv in sorted(st.items()) if kv[1] != "ready")))
        sys.exit(1)
    time.sleep(0.1)
