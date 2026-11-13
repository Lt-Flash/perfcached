#!/usr/bin/env python3
# waitready.py <host:port>... [-t seconds] - until every node's state is "ready" (a node can read as
# formed while still RECOVERING and refusing data verbs); 1 when one is not within 15 s, naming it
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
