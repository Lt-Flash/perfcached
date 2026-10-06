#!/usr/bin/env python3
"""fakeupstream.py - a scripted RESP server standing in for Redis in the
read-through suites (S331).  No Redis on the runners, and no write may ever
reach an upstream: this server answers the READ commands perfcached sends
(HELLO/AUTH/SELECT/PING/TYPE/PTTL/GET/HGETALL/JSON.GET/EXISTS/CLIENT) and
records ANY other command in <dir>/writes - the suites fail on a non-empty
file.

  fakeupstream.py <port> <dir>

<dir>/data.json  the keyspace, re-read when it changes:
    {"key": {"type": "string|hash|json|list|set|zset", "value": ...,
             "ttl_ms": N (optional)}}
<dir>/ctl.json   behaviour, re-read when it changes:
    {"password": "pw" | null, "user": "u" | null, "delay_ms": 0,
     "delay_keys": ["k", ...] (only these are delayed; empty = all),
     "down": false (refuse/close every connection),
     "resp3": true (answer HELLO 3),
     "tracking": true (answer CLIENT ID / CLIENT TRACKING),
     "inv_seq": N + "invalidate": ["k", ...]  - a NEW inv_seq pushes an
         invalidation for those keys to every REDIRECT target,
     "kill_seq": N - a NEW kill_seq closes every REDIRECT target}
<dir>/counts     appended "<COMMAND> <key>" per data command (TYPE/GET/...)
<dir>/writes     appended per command outside the read set
"""
import fnmatch
import json
import os
import socket
import sys
import threading
import time

PORT = int(sys.argv[1])
DIR = sys.argv[2]
READS = {b"HELLO", b"AUTH", b"SELECT", b"PING", b"TYPE", b"PTTL", b"TTL",
         b"GET", b"HGETALL", b"JSON.GET", b"EXISTS", b"CLIENT", b"QUIT",
         b"COMMAND", b"SCAN"}
lock = threading.Lock()
cache = {"data": ({}, 0.0), "ctl": ({}, 0.0)}
born = time.time()
conns = {}                 # id -> (socket, send lock)
targets = set()            # ids named by CLIENT TRACKING ... REDIRECT
next_id = [100]


def load(name):
    path = os.path.join(DIR, name + ".json")
    try:
        mt = os.path.getmtime(path)
    except OSError:
        return {}
    with lock:
        obj, seen = cache[name]
        if mt != seen:
            try:
                with open(path) as f:
                    obj = json.load(f)
            except (OSError, ValueError):
                obj = {}
            cache[name] = (obj, mt)
        return obj


def note(fname, line):
    with lock:
        with open(os.path.join(DIR, fname), "a") as f:
            f.write(line + "\n")


def bulk(b):
    if b is None:
        return b"$-1\r\n"
    if isinstance(b, str):
        b = b.encode()
    return b"$%d\r\n%s\r\n" % (len(b), b)


def arr(items):
    return b"*%d\r\n" % len(items) + b"".join(items)


def read_cmd(f):
    line = f.readline()
    if not line:
        return None
    if not line.startswith(b"*"):
        return line.strip().split()
    n = int(line[1:].strip())
    out = []
    for _ in range(n):
        hdr = f.readline()
        ln = int(hdr[1:].strip())
        out.append(f.read(ln + 2)[:ln])
    return out


def push_to_targets(payload):
    with lock:
        ts = [conns.get(t) for t in targets]
    for e in ts:
        if not e:
            continue
        try:
            with e[1]:
                e[0].sendall(payload)
        except OSError:
            pass


def watcher():
    seen_inv = seen_kill = None
    while True:
        time.sleep(0.05)
        ctl = load("ctl")
        inv = ctl.get("inv_seq")
        if inv is not None and inv != seen_inv:
            if seen_inv is not None or inv:
                keys = ctl.get("invalidate") or []
                items = b"".join(bulk(k) for k in keys)
                push_to_targets(b">2\r\n" + bulk("invalidate") +
                                b"*%d\r\n" % len(keys) + items)
                note("pushes", "invalidate " + " ".join(keys))
            seen_inv = inv
        kill = ctl.get("kill_seq")
        if kill is not None and kill != seen_kill:
            if seen_kill is not None or kill:
                with lock:
                    ts = [conns.get(t) for t in targets]
                    targets.clear()
                for e in ts:
                    if e:
                        try:
                            e[0].shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                note("pushes", "kill")
            seen_kill = kill


def serve(conn):
    f = conn.makefile("rb")
    authed = False
    resp3 = False
    with lock:
        cid = next_id[0]
        next_id[0] += 1
        conns[cid] = (conn, threading.Lock())
    try:
        while True:
            ctl = load("ctl")
            if ctl.get("down"):
                return
            cmd = read_cmd(f)
            if cmd is None:
                return
            name = cmd[0].upper()
            ctl = load("ctl")
            if name not in READS:
                note("writes", " ".join(c.decode("latin-1") for c in cmd))
                conn.sendall(b"-ERR fakeupstream refuses writes\r\n")
                continue
            pw = ctl.get("password")
            if name == b"HELLO":
                ver = cmd[1] if len(cmd) > 1 else b"2"
                if len(cmd) >= 5 and cmd[2].upper() == b"AUTH":
                    if pw is not None and cmd[4].decode() != pw:
                        conn.sendall(b"-WRONGPASS invalid username-password pair\r\n")
                        continue
                    authed = True
                if ver == b"3" and not ctl.get("resp3", True):
                    conn.sendall(b"-NOPROTO unsupported protocol version\r\n")
                    continue
                resp3 = ver == b"3"
                if resp3:
                    conn.sendall(b"%2\r\n+server\r\n+fake\r\n+proto\r\n:3\r\n")
                else:
                    conn.sendall(arr([bulk("server"), bulk("fake")]))
                continue
            if name == b"AUTH":
                given = cmd[-1].decode()
                if pw is not None and given != pw:
                    conn.sendall(b"-WRONGPASS invalid username-password pair\r\n")
                    continue
                authed = True
                conn.sendall(b"+OK\r\n")
                continue
            if pw is not None and not authed and name not in (b"QUIT",):
                conn.sendall(b"-NOAUTH Authentication required.\r\n")
                continue
            if name == b"CLIENT":
                sub = cmd[1].upper() if len(cmd) > 1 else b""
                if not ctl.get("tracking", True):
                    conn.sendall(b"-ERR unknown subcommand\r\n")
                    continue
                if sub == b"ID":
                    conn.sendall(b":%d\r\n" % cid)
                    continue
                if sub == b"TRACKING":
                    up = [c.upper() for c in cmd]
                    if b"REDIRECT" in up:
                        tgt = int(cmd[up.index(b"REDIRECT") + 1])
                        with lock:
                            targets.add(tgt)
                    note("counts", "TRACKING " + " ".join(c.decode() for c in cmd[2:]))
                    conn.sendall(b"+OK\r\n")
                    continue
                conn.sendall(b"+OK\r\n")
                continue
            if name == b"SELECT":
                conn.sendall(b"+OK\r\n")
                continue
            if name == b"PING":
                conn.sendall(b"+PONG\r\n")
                continue
            if name == b"QUIT":
                conn.sendall(b"+OK\r\n")
                return
            if name == b"COMMAND":
                conn.sendall(b"*0\r\n")
                continue
            if name == b"SCAN":
                # SCAN cursor [MATCH pat] [COUNT n]: the data file's live
                # keys, sorted; the cursor is an index into them
                cur = int(cmd[1])
                pat, cnt = "*", 10
                up = [c.upper() for c in cmd]
                if b"MATCH" in up:
                    pat = cmd[up.index(b"MATCH") + 1].decode("latin-1").replace("[^", "[!")
                if b"COUNT" in up:
                    cnt = int(cmd[up.index(b"COUNT") + 1])
                data = load("data")
                now = (time.time() - born) * 1000
                keys = sorted(k for k, e in data.items()
                              if fnmatch.fnmatchcase(k, pat) and
                              ("ttl_ms" not in e or e["ttl_ms"] > now))
                part = keys[cur:cur + cnt]
                nxt = cur + cnt if cur + cnt < len(keys) else 0
                note("counts", "SCAN %d %s" % (cur, pat))
                conn.sendall(arr([bulk(str(nxt)), arr([bulk(k) for k in part])]))
                continue
            key = cmd[1].decode("latin-1") if len(cmd) > 1 else ""
            note("counts", name.decode() + " " + key)
            dk = ctl.get("delay_keys") or []
            if ctl.get("delay_ms") and (not dk or key in dk):
                time.sleep(ctl["delay_ms"] / 1000.0)
            data = load("data")
            ent = data.get(key)
            if ent and "ttl_ms" in ent:
                left = ent["ttl_ms"] - (time.time() - born) * 1000
                if left <= 0:
                    ent = None
            if name == b"TYPE":
                t = ent["type"] if ent else "none"
                t = {"json": "ReJSON-RL"}.get(t, t)
                conn.sendall(b"+%s\r\n" % t.encode())
            elif name in (b"PTTL", b"TTL"):
                if not ent:
                    conn.sendall(b":-2\r\n")
                elif "ttl_ms" not in ent:
                    conn.sendall(b":-1\r\n")
                else:
                    left = int(ent["ttl_ms"] - (time.time() - born) * 1000)
                    conn.sendall(b":%d\r\n" % (left if name == b"PTTL" else left // 1000))
            elif name == b"GET":
                if not ent:
                    conn.sendall(b"_\r\n" if resp3 else b"$-1\r\n")
                elif ent["type"] != "string":
                    conn.sendall(b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
                else:
                    conn.sendall(bulk(ent["value"]))
            elif name == b"HGETALL":
                if not ent or ent["type"] != "hash":
                    conn.sendall(b"%0\r\n" if resp3 else b"*0\r\n")
                else:
                    items = []
                    for k, v in ent["value"].items():
                        items += [bulk(k), bulk(v)]
                    if resp3:
                        conn.sendall(b"%%%d\r\n" % (len(items) // 2) + b"".join(items))
                    else:
                        conn.sendall(arr(items))
            elif name == b"JSON.GET":
                if not ent or ent["type"] != "json":
                    conn.sendall(b"_\r\n" if resp3 else b"$-1\r\n")
                else:
                    conn.sendall(bulk(json.dumps(ent["value"], separators=(",", ":"))))
            elif name == b"EXISTS":
                conn.sendall(b":%d\r\n" % (1 if ent else 0))
    except (OSError, ValueError, IndexError):
        return
    finally:
        with lock:
            conns.pop(cid, None)
        try:
            conn.close()
        except OSError:
            pass


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", PORT))
    s.listen(64)
    threading.Thread(target=watcher, daemon=True).start()
    with open(os.path.join(DIR, "ready"), "w") as f:
        f.write("ready\n")
    while True:
        c, _ = s.accept()
        if load("ctl").get("down"):
            c.close()
            continue
        threading.Thread(target=serve, args=(c,), daemon=True).start()


main()
