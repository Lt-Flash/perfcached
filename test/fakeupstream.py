#!/usr/bin/env python3
"""fakeupstream.py - a scripted RESP server standing in for Redis in the
read-through suites (S331).  No Redis on the runners, and no write may ever
reach an upstream: this server answers the READ commands perfcached sends
(HELLO/AUTH/SELECT/PING/INFO/TYPE/PTTL/GET/HGETALL/JSON.GET/EXISTS/CLIENT/
SCAN) and
records ANY other command in <dir>/writes - the suites fail on a non-empty
file.

  fakeupstream.py <port> <dir>

<dir>/data.json  the keyspace, re-read when it changes:
    {"key": {"type": "string|hash|json|list|set|zset", "value": ...,
             "ttl_ms": N (optional)}}
<dir>/ctl.json   behaviour, re-read when it changes:
    {"password": "pw" | null, "user": "u" | null, "delay_ms": 0,
     "delay_keys": ["k", ...] (only these are delayed; empty = all),
     "write_delay_ms": 0 (every WRITE answered this late - a slow master),
     "reply_delay_ms": 0, "reply_delay_keys": [...], "reply_delay_cmds":
         ["GET", ...] (S424: a read takes its value at once and answers this
         late - the reply in flight; only those keys / commands),
     "down": false (refuse/close every connection),
     "resp3": true (answer HELLO 3),
     "tracking": true (answer CLIENT ID / CLIENT TRACKING),
     "inv_seq": N + "invalidate": ["k", ...]  - a NEW inv_seq pushes an
         invalidation for those keys to every REDIRECT target,
     "kill_seq": N - a NEW kill_seq closes every REDIRECT target and every
         broadcast-tracking connection (AR1),
     (AR1) a connection that asks CLIENT TRACKING ON BCAST [PREFIX p ...]
         is pushed ["invalidate", [keys]] whenever this server's keyspace
         changes under its prefixes - by a write here or by a replicated
         one (data.json rewritten by the master), as a Redis replica does,
     "run_id_per_conn": true - INFO tells every connection it reached a
         DIFFERENT server (a round-robin proxy in front of several),
     "info_noperm": true - INFO is refused, as for a user without +info,
     "acl": {"user": {"pw": "p", "write": true|false}, ...} - AR7: Redis ACL
         users instead of "password": AUTH [user] pw (or HELLO 3 AUTH user pw;
         no user is "default") must name one, every command before it is
         NOAUTH, and a WRITE from a user without "write" is answered NOPERM.
         Each AUTH is noted in <dir>/auth ("user ok" / "user refused"),
         each NOPERM in <dir>/noperm,
     "role": "master" | "replica" - what ROLE and INFO replication say
         (S382; master by default, as a lone Redis is),
     "link": "up" | "down" - a replica's link to its master,
     "last_io_s": N - seconds since a replica last heard from its master,
     S385 (write-through):
     "accept_writes": true - a MASTER applies the writes perfcached
         forwards (SET [EX|PX|NX|XX|GET], SETEX, DEL, UNLINK, EXPIRE,
         PEXPIRE, PERSIST, INCR/INCRBY/DECRBY, APPEND, HSET, LPUSH,
         JSON.SET <$|$.field|.field>, JSON.DEL) into data.json - still
         noted in <dir>/writes; without it every write is refused (a
         replica answers READONLY),
     "replicate_to": ["dir", ...] - after a write the keyspace is copied
         there too (the replica fakes serve it),
     "replicate_delay_ms": N or {"dir": N, ...} - S423: that copy N ms
         after the write (a replica's lag, per replica; its tracking echoes
         the write then),
     "push_on_write": true - S423: a write here pushes its broadcast
         invalidations before the writer's reply, as Redis does (else the
         watcher's 50 ms poll sends them); "push_delay_ms": N sends them N
         ms after it - the push and the reply travel on two connections,
         and either may land first,
     "master_host"/"master_port" - what a replica's ROLE and INFO
         replication name as its master,
     "json": false - no RedisJSON: COMMAND INFO has no JSON.* and the
         JSON commands are unknown}
COMMAND INFO / COMMAND GETKEYS answer for the commands in CMDS below, as
Redis 7 would (flags, key positions, ACL categories).
A key written here keeps its expiry as "exp_ms" (wall clock), so a copy in
another fake's directory expires at the same moment.
INFO server answers run_id: one per fake (from its port), as one Redis
server would - a verbatim string under RESP3, a bulk string under RESP2.
<dir>/counts     appended "<COMMAND> <key>" per data command (TYPE/GET/...)
<dir>/writes     appended per command outside the read set
"""
import fnmatch
import hashlib
import json
import os
import socket
import sys
import threading
import time

PORT = int(sys.argv[1])
DIR = sys.argv[2]
READS = {b"HELLO", b"AUTH", b"SELECT", b"PING", b"INFO", b"TYPE", b"PTTL", b"TTL",
         b"GET", b"HGETALL", b"JSON.GET", b"EXISTS", b"CLIENT", b"QUIT",
         b"COMMAND", b"SCAN", b"ROLE", b"LRANGE"}
# S385: COMMAND INFO's table - name: (arity, flags, first, last, step, ACL cats)
CMDS = {
    "set": (-3, ["write", "denyoom"], 1, 1, 1, ["@write", "@string", "@slow"]),
    "get": (2, ["readonly", "fast"], 1, 1, 1, ["@read", "@string", "@fast"]),
    "del": (-2, ["write"], 1, -1, 1, ["@keyspace", "@write", "@slow"]),
    "persist": (2, ["write", "fast"], 1, 1, 1, ["@keyspace", "@write", "@fast"]),
    "append": (3, ["write", "denyoom", "fast"], 1, 1, 1, ["@write", "@string", "@fast"]),
    "lpush": (-3, ["write", "denyoom", "fast"], 1, 1, 1, ["@write", "@list", "@fast"]),
    "lrange": (4, ["readonly"], 1, 1, 1, ["@read", "@list", "@slow"]),
    "blpop": (-3, ["write", "blocking"], 1, -2, 1, ["@write", "@list", "@slow", "@blocking"]),
    "flushdb": (-1, ["write"], 0, 0, 0, ["@keyspace", "@write", "@slow", "@dangerous"]),
    "config": (-2, ["admin", "noscript"], 0, 0, 0, ["@admin", "@slow", "@dangerous"]),
    "debug": (-2, ["admin", "noscript"], 0, 0, 0, ["@admin", "@slow", "@dangerous"]),
    "randomkey": (1, ["readonly"], 0, 0, 0, ["@keyspace", "@read", "@slow"]),
    "json.set": (-4, ["write", "denyoom"], 1, 1, 1, ["@write", "@json"]),
    "json.del": (-2, ["write"], 1, 1, 1, ["@write", "@json"]),
}
WRITES = {b"SET", b"SETEX", b"DEL", b"UNLINK", b"EXPIRE", b"PEXPIRE", b"PERSIST",
          b"INCR", b"INCRBY", b"DECRBY", b"APPEND", b"HSET", b"LPUSH",
          b"JSON.SET", b"JSON.DEL", b"MSET", b"HDEL", b"HINCRBY"}
lock = threading.Lock()
rep_lock = threading.Lock()          # S423: delayed copies to the replicas land in order
rep_seq = {}                         # S423: replica dir -> [last queued, last landed]
cache = {"data": ({}, 0.0), "ctl": ({}, 0.0)}
born = time.time()
conns = {}                 # id -> (socket, send lock)
targets = set()            # ids named by CLIENT TRACKING ... REDIRECT
bcast = {}                 # AR1: id -> [prefixes] (empty: every key)
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


def alive(ent):
    """S385: an entry still alive - ttl_ms (since this fake started, the
    suites' data files) or exp_ms (wall clock, what a write leaves)"""
    if not ent:
        return None
    if "ttl_ms" in ent and ent["ttl_ms"] - (time.time() - born) * 1000 <= 0:
        return None
    if "exp_ms" in ent and ent["exp_ms"] - time.time() * 1000 <= 0:
        return None
    return ent


def left_ms(ent):
    if "exp_ms" in ent:
        return int(ent["exp_ms"] - time.time() * 1000)
    if "ttl_ms" in ent:
        return int(ent["ttl_ms"] - (time.time() - born) * 1000)
    return -1


def _write_json(d, body):
    tmp = os.path.join(d, "data.json.tmp.%d" % threading.get_ident())
    with open(tmp, "w") as f:
        f.write(body)
    os.replace(tmp, os.path.join(d, "data.json"))


def save_data(data, ctl):
    """S385: the keyspace after a write - here, and to the replicas.  S423:
    "replicate_delay_ms" copies to the replicas that much later, as a real
    replica applies a write after its master (and echoes it then)"""
    body = json.dumps(data)
    _write_json(DIR, body)
    delay = ctl.get("replicate_delay_ms") or 0
    for d in list(ctl.get("replicate_to") or []):
        dl = delay.get(d, 0) if isinstance(delay, dict) else delay
        if not dl:
            _write_json(d, body)
            continue
        with rep_lock:
            q = rep_seq.setdefault(d, [0, 0])
            q[0] += 1
            seq = q[0]

        def later(b=body, dd=d, n=seq):
            with rep_lock:
                q = rep_seq[dd]
                if n <= q[1]:
                    return                 # a newer copy already landed there
                q[1] = n
                _write_json(dd, b)
        threading.Timer(dl / 1000.0, later).start()
    with lock:
        cache["data"] = (data, os.path.getmtime(os.path.join(DIR, "data.json")))


def cmd_info(names, ctl):
    out = []
    for n in names:
        n = n.decode("latin-1").lower()
        e = CMDS.get(n)
        if e is None or (n.startswith("json.") and ctl.get("json") is False):
            out.append(b"*-1\r\n")
            continue
        ar, fl, fi, la, st, cat = e
        out.append(arr([bulk(n), b":%d\r\n" % ar,
                        arr([b"+%s\r\n" % x.encode() for x in fl]),
                        b":%d\r\n" % fi, b":%d\r\n" % la, b":%d\r\n" % st,
                        arr([b"+%s\r\n" % x.encode() for x in cat]),
                        b"*0\r\n", b"*0\r\n", b"*0\r\n"]))
    return arr(out)


def apply_write(cmd, ctl):
    """S385: apply one forwarded write; the reply as Redis gives it"""
    name = cmd[0].upper()
    if name.startswith(b"JSON.") and ctl.get("json") is False:
        return b"-ERR unknown command '%s', with args beginning with: \r\n" % cmd[0]
    data = dict(load("data"))
    k = cmd[1].decode("latin-1")
    ent = alive(data.get(k))
    now = time.time() * 1000
    up = [c.upper() for c in cmd]
    rep = b"+OK\r\n"
    if name in (b"SET", b"SETEX"):
        if name == b"SETEX":
            val, exp = cmd[3], now + int(cmd[2]) * 1000
            nx = xx = False
        else:
            val, exp = cmd[2], None
            nx, xx = b"NX" in up[3:], b"XX" in up[3:]
            if b"EX" in up[3:]:
                exp = now + int(cmd[up.index(b"EX", 3) + 1]) * 1000
            if b"PX" in up[3:]:
                exp = now + int(cmd[up.index(b"PX", 3) + 1])
        if (nx and ent) or (xx and not ent):
            return b"$-1\r\n"
        data[k] = {"type": "string", "value": val.decode("latin-1")}
        if exp:
            data[k]["exp_ms"] = exp
    elif name in (b"DEL", b"UNLINK"):
        n = 0
        for kk in cmd[1:]:
            if alive(data.get(kk.decode("latin-1"))):
                n += 1
            data.pop(kk.decode("latin-1"), None)
        rep = b":%d\r\n" % n
    elif name in (b"EXPIRE", b"PEXPIRE"):
        if not ent:
            return b":0\r\n"
        ms = int(cmd[2]) * (1000 if name == b"EXPIRE" else 1)
        ent = dict(ent)
        ent.pop("ttl_ms", None)
        ent["exp_ms"] = now + ms
        data[k] = ent
        rep = b":1\r\n"
    elif name == b"PERSIST":
        if not ent or ("exp_ms" not in ent and "ttl_ms" not in ent):
            return b":0\r\n"
        ent = dict(ent)
        ent.pop("ttl_ms", None)
        ent.pop("exp_ms", None)
        data[k] = ent
        rep = b":1\r\n"
    elif name in (b"INCR", b"INCRBY", b"DECRBY"):
        by = 1 if name == b"INCR" else int(cmd[2]) * (-1 if name == b"DECRBY" else 1)
        if ent and ent["type"] != "string":
            return b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"
        v = int(ent["value"]) if ent else 0
        v += by
        ent = dict(ent or {"type": "string"})
        ent["value"] = str(v)
        data[k] = ent
        rep = b":%d\r\n" % v
    elif name == b"APPEND":
        if ent and ent["type"] != "string":
            return b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"
        ent = dict(ent or {"type": "string", "value": ""})
        ent["value"] = ent["value"] + cmd[2].decode("latin-1")
        data[k] = ent
        rep = b":%d\r\n" % len(ent["value"])
    elif name == b"HSET":
        ent = dict(ent or {"type": "hash", "value": {}})
        if ent["type"] != "hash":
            return b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"
        h = dict(ent["value"])
        added = 0
        for i in range(2, len(cmd) - 1, 2):
            f = cmd[i].decode("latin-1")
            added += f not in h
            h[f] = cmd[i + 1].decode("latin-1")
        ent["value"] = h
        data[k] = ent
        rep = b":%d\r\n" % added
    elif name == b"MSET":                         # AR5
        for i in range(1, len(cmd) - 1, 2):
            data[cmd[i].decode("latin-1")] = {"type": "string",
                                              "value": cmd[i + 1].decode("latin-1")}
    elif name == b"HDEL":
        if ent and ent["type"] != "hash":
            return b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"
        h = dict(ent["value"]) if ent else {}
        n = sum(1 for f in cmd[2:] if h.pop(f.decode("latin-1"), None) is not None)
        if ent and h:
            ent = dict(ent)
            ent["value"] = h
            data[k] = ent
        elif ent:
            data.pop(k, None)                    # the empty hash is no key
        rep = b":%d\r\n" % n
    elif name == b"HINCRBY":
        ent = dict(ent or {"type": "hash", "value": {}})
        if ent["type"] != "hash":
            return b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"
        h = dict(ent["value"])
        f = cmd[2].decode("latin-1")
        try:
            v = int(h.get(f, "0")) + int(cmd[3])
        except ValueError:
            return b"-ERR hash value is not an integer\r\n"
        h[f] = str(v)
        ent["value"] = h
        data[k] = ent
        rep = b":%d\r\n" % v
    elif name == b"LPUSH":
        ent = dict(ent or {"type": "list", "value": []})
        if ent["type"] != "list":
            return b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"
        lst = list(ent["value"])
        for v in cmd[2:]:
            lst.insert(0, v.decode("latin-1"))
        ent["value"] = lst
        data[k] = ent
        rep = b":%d\r\n" % len(lst)
    elif name == b"JSON.SET":
        path, doc = cmd[2].decode(), json.loads(cmd[3])
        nx, xx = b"NX" in up[4:], b"XX" in up[4:]
        if path in ("$", "."):
            if (nx and ent) or (xx and not ent):
                return b"$-1\r\n"
            keep = {x: ent[x] for x in ("exp_ms", "ttl_ms") if ent and x in ent}
            data[k] = dict({"type": "json", "value": doc}, **keep)
        else:
            if not ent or ent["type"] != "json":
                return b"-ERR new objects must be created at the root\r\n"
            f = path.split(".")[-1]
            ent = dict(ent)
            obj = dict(ent["value"])
            if (nx and f in obj) or (xx and f not in obj):
                return b"$-1\r\n"
            obj[f] = doc
            ent["value"] = obj
            data[k] = ent
    elif name == b"JSON.DEL":
        path = cmd[2].decode() if len(cmd) > 2 else "$"
        if not ent:
            return b":0\r\n"
        if path in ("$", "."):
            data.pop(k, None)
            rep = b":1\r\n"
        else:
            f = path.split(".")[-1]
            ent = dict(ent)
            obj = dict(ent["value"])
            rep = b":%d\r\n" % (1 if obj.pop(f, None) is not None else 0)
            ent["value"] = obj
            data[k] = ent
    save_data(data, ctl)
    if ctl.get("push_on_write"):
        pd = ctl.get("push_delay_ms") or 0
        if pd:                         # S423: the push lands after the writer's reply
            threading.Timer(pd / 1000.0, bcast_now).start()
        else:
            bcast_now()                # S423: as Redis does, before the writer's reply
    return rep


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


def bcast_changes(last):
    """AR1: the keys whose entry changed since @last, pushed to every BCAST
    connection whose prefixes cover them; returns the keyspace seen now"""
    now = load("data")
    if now is last:
        return last
    keys = sorted(k for k in set(now) | set(last) if now.get(k) != last.get(k))
    with lock:
        subs = [(conns.get(c), p) for c, p in bcast.items()]
    for e, pre in subs:
        mine = [k for k in keys if not pre or any(k.startswith(x) for x in pre)]
        if not e or not mine:
            continue
        try:
            with e[1]:
                e[0].sendall(b">2\r\n" + bulk("invalidate") + b"*%d\r\n" % len(mine) +
                             b"".join(bulk(k) for k in mine))
        except OSError:
            pass
    if keys and subs:
        note("pushes", "bcast " + " ".join(keys))
    return now


bc_lock = threading.Lock()
bc_state = {"last": None}


def bcast_now():
    """the broadcast pushes for what changed since the last look - from the
    watcher's poll, or (S423 "push_on_write") from the write itself"""
    with bc_lock:
        if bc_state["last"] is None:
            bc_state["last"] = load("data")
        bc_state["last"] = bcast_changes(bc_state["last"])


def watcher():
    seen_inv = seen_kill = None
    with bc_lock:
        bc_state["last"] = load("data")
    while True:
        time.sleep(0.05)
        bcast_now()
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
                    ts = [conns.get(t) for t in targets] + [conns.get(c) for c in bcast]
                    targets.clear()
                    bcast.clear()
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
    who = None                 # AR7: the ACL user this connection is
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
            acl = ctl.get("acl")
            if acl is not None:                      # AR7
                u = p = None
                if name == b"AUTH" and len(cmd) >= 2:
                    u, p = (cmd[1], cmd[2]) if len(cmd) > 2 else (b"default", cmd[1])
                elif name == b"HELLO" and len(cmd) >= 5 and cmd[2].upper() == b"AUTH":
                    u, p = cmd[3], cmd[4]
                if u is not None:
                    u, p = u.decode("latin-1"), p.decode("latin-1")
                    if u not in acl or acl[u].get("pw") != p:
                        note("auth", "%s refused" % u)
                        conn.sendall(b"-WRONGPASS invalid username-password pair\r\n")
                        continue
                    who = u
                    note("auth", "%s ok" % u)
                    if name == b"AUTH":
                        conn.sendall(b"+OK\r\n")
                        continue
                elif who is None and name not in (b"HELLO", b"QUIT"):
                    conn.sendall(b"-NOAUTH Authentication required.\r\n")
                    continue
                elif name not in READS and not acl[who].get("write"):
                    note("noperm", "%s %s" % (who, name.decode("latin-1")))
                    conn.sendall(b"-NOPERM User %s has no permissions to run the '%s' "
                                 b"command\r\n" % (who.encode(), name.lower()))
                    continue
            if name not in READS:
                note("writes", " ".join(c.decode("latin-1") for c in cmd))
                if ctl.get("write_delay_ms"):         # AR2: a slow master
                    time.sleep(ctl["write_delay_ms"] / 1000.0)
                if (ctl.get("accept_writes") and ctl.get("role") != "replica"
                        and name in WRITES and len(cmd) > 1):
                    conn.sendall(apply_write(cmd, ctl))
                elif ctl.get("role") == "replica":
                    conn.sendall(b"-READONLY You can't write against a read only replica.\r\n")
                else:
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
                    if b"BCAST" in up:             # AR1
                        pre = [cmd[i + 1].decode("latin-1") for i, c in enumerate(up)
                               if c == b"PREFIX" and i + 1 < len(cmd)]
                        with lock:
                            bcast[cid] = pre
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
                sub = cmd[1].upper() if len(cmd) > 1 else b""
                if sub == b"INFO":                    # S385
                    note("counts", "COMMAND INFO " + " ".join(c.decode() for c in cmd[2:]))
                    conn.sendall(cmd_info(cmd[2:], ctl))
                elif sub == b"GETKEYS" and len(cmd) > 2:
                    e = CMDS.get(cmd[2].decode("latin-1").lower())
                    if not e or e[2] <= 0:
                        conn.sendall(b"-ERR The command has no key arguments\r\n")
                    else:
                        args = cmd[2:]
                        last = e[3] if e[3] >= 0 else len(args) + e[3]
                        conn.sendall(arr([bulk(args[i]) for i in range(e[2], last + 1, e[4] or 1)
                                          if i < len(args)]))
                else:
                    conn.sendall(b"*0\r\n")
                continue
            if name == b"ROLE":                       # S382
                note("counts", "ROLE")
                if ctl.get("role") == "replica":
                    conn.sendall(arr([bulk("slave"), bulk(ctl.get("master_host", "127.0.0.1")),
                                      b":%d\r\n" % int(ctl.get("master_port", 6379)),
                                      bulk("connected"), b":1000\r\n"]))
                else:
                    conn.sendall(b"*3\r\n$6\r\nmaster\r\n:1000\r\n*0\r\n")
                continue
            if name == b"INFO" and len(cmd) > 1 and cmd[1].lower() == b"replication":
                note("counts", "INFO replication")
                if ctl.get("role") == "replica":
                    body = ("# Replication\r\nrole:slave\r\nmaster_host:%s\r\n"
                            "master_port:%d\r\nmaster_link_status:%s\r\n"
                            "master_last_io_seconds_ago:%d\r\n" % (
                                ctl.get("master_host", "127.0.0.1"),
                                int(ctl.get("master_port", 6379)),
                                ctl.get("link", "up"), int(ctl.get("last_io_s", 1)))).encode()
                else:
                    body = b"# Replication\r\nrole:master\r\nconnected_slaves:0\r\n"
                if resp3:
                    conn.sendall(b"=%d\r\ntxt:%s\r\n" % (len(body) + 4, body))
                else:
                    conn.sendall(bulk(body))
                continue
            if name == b"INFO":
                if ctl.get("info_noperm"):
                    conn.sendall(b"-NOPERM User has no permissions to run the 'info' command\r\n")
                    continue
                # its own name: AR7's `who` is the connection's ACL user, and
                # reusing it here made the next command look up acl["fake-N"]
                # - a KeyError that killed the connection (upwritethroughtest
                # 13, its first CI run: "the connection to the master failed")
                srv = "fake-%d" % PORT
                if ctl.get("run_id_per_conn"):
                    srv += "-%d" % cid
                rid = hashlib.sha1(srv.encode()).hexdigest()
                note("counts", "INFO " + rid[:12])
                body = ("# Server\r\nredis_version:7.4.0\r\nredis_mode:standalone\r\n"
                        "run_id:%s\r\ntcp_port:%d\r\n" % (rid, PORT)).encode()
                if resp3:
                    conn.sendall(b"=%d\r\ntxt:%s\r\n" % (len(body) + 4, body))
                else:
                    conn.sendall(bulk(body))
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
            rk = ctl.get("reply_delay_keys") or []
            rc_ = ctl.get("reply_delay_cmds") or []
            if ctl.get("reply_delay_ms") and (not rk or key in rk) and \
                    (not rc_ or name.decode("latin-1").upper() in rc_):
                # S424: the value is read NOW and the reply sent later - a
                # reply in flight while the key changes and its invalidation
                # is pushed on the other connection
                time.sleep(ctl["reply_delay_ms"] / 1000.0)
            ent = alive(data.get(key))
            if name == b"TYPE":
                t = ent["type"] if ent else "none"
                t = {"json": "ReJSON-RL"}.get(t, t)
                conn.sendall(b"+%s\r\n" % t.encode())
            elif name in (b"PTTL", b"TTL"):
                if not ent:
                    conn.sendall(b":-2\r\n")
                elif "ttl_ms" not in ent and "exp_ms" not in ent:
                    conn.sendall(b":-1\r\n")
                else:
                    left = left_ms(ent)
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
            elif name == b"LRANGE":                   # S385: a passed read
                if not ent:
                    conn.sendall(b"*0\r\n")
                elif ent["type"] != "list":
                    conn.sendall(b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
                else:
                    lst = ent["value"]
                    a, b = int(cmd[2]), int(cmd[3])
                    if b < 0:
                        b = len(lst) + b
                    conn.sendall(arr([bulk(x) for x in lst[a:b + 1]]))
    except (OSError, ValueError, IndexError):
        return
    finally:
        with lock:
            conns.pop(cid, None)
            bcast.pop(cid, None)
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
