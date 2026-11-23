#!/usr/bin/env python3
"""fakeupstream.py - a scripted RESP server standing in for Redis in the
read-through suites.  No Redis on the runners, and no write may ever
reach an upstream: this server answers the READ commands perfcached sends
(HELLO/AUTH/SELECT/PING/INFO/TYPE/PTTL/GET/HGETALL/JSON.GET/EXISTS/CLIENT/
SCAN) and
records ANY other command in <dir>/writes - the suites fail on a non-empty
file.

  fakeupstream.py <port> <dir> [host]   (host: 127.0.0.1 by default; "::1" for IPv6)
  FAKEUP_TLS=cert:key[:ca]  serve TLS - with a CA, the client's certificate is
      required and checked; each handshake noted in <dir>/tls ("ok <client CN or ->" or
      "refused <why>")

<dir>/data.json  the keyspace, re-read when it changes:
    {"key": {"type": "string|hash|json|list|set|zset", "value": ...,
             "ttl_ms": N (optional)}}
<dir>/ctl.json   behaviour, re-read when it changes:
    {"password": "pw" | null, "user": "u" | null, "delay_ms": 0,
     "delay_keys": ["k", ...] (only these are delayed; empty = all),
     "write_delay_ms": 0 (every WRITE answered this late - a slow master),
     "reply_delay_ms": 0, "reply_delay_keys": [...], "reply_delay_cmds":
         ["GET", ...] (a read takes its value at once and answers this
         late - the reply in flight; only those keys / commands),
     "down": false (refuse/close every connection),
     "resp3": true (answer HELLO 3),
     "tracking": true (answer CLIENT ID / CLIENT TRACKING),
     "redirect_refused": true (CLIENT TRACKING ... REDIRECT answered -NOPERM, as
         for a user without that command; CLIENT ID still served),
     "client_id_delay_ms": 0 (CLIENT ID answered this late - only a
         client's invalidation connection asks it, so its tracking comes up
         that much after its fetch connections can serve),
     "inv_seq": N + "invalidate": ["k", ...]  - a NEW inv_seq pushes an
         invalidation for those keys to every REDIRECT target,
     "kill_seq": N - a NEW kill_seq closes every REDIRECT target and every
         broadcast-tracking connection,
     a connection that asks CLIENT TRACKING ON BCAST [PREFIX p ...]
         is pushed ["invalidate", [keys]] whenever this server's keyspace
         changes under its prefixes - by a write here or by a replicated
         one (data.json rewritten by the master), as a Redis replica does,
     "run_id_per_conn": true - INFO tells every connection it reached a
         DIFFERENT server (a round-robin proxy in front of several),
     "redis_mode": "cluster" - what INFO server says the server is
     "cluster": {"shards": [{"slots": [[lo, hi], ...], "nodes": [[host, port, "master" |
         "replica"], ...]}, ...], "slots_only": true (CLUSTER SHARDS unknown, as before
         Redis 7), "ask": {"key": [host, port]} (answered -ASK there), "importing":
         ["key", ...] (served here only after ASKING), "report": [...] (what CLUSTER SHARDS
         and SLOTS say, when it lags "shards"), "moved_empty_host": true (-MOVED <slot>
         :<port>, Redis 7's unknown endpoint)} - a cluster node, the same
         topology on each; a key outside this node's shard, or read on a replica without
         READONLY, is answered -MOVED <slot> <master>; SELECT other than 0 is refused; a
         write is routed the same way (a replica MOVEs it), and keys of two slots are CROSSSLOT
     "perfcached_version": "0.7.5" - INFO server says it is a perfcached
     "replicas": [["127.0.0.1", port], ...] - a master's ROLE names them
     "laddr": "10.9.9.9:6379" - CLIENT INFO's laddr, as a server behind a proxy says
     "sentinel": {"<name>": {"ip": h, "port": p, "replicas": [[h, p, "slave[,s_down]"], ...],
         "sentinels": [[h, p], ...]}} - SENTINEL MASTERS, GET-MASTER-ADDR-BY-NAME,
         REPLICAS and SENTINELS answered from it (with "redis_mode": "sentinel");
         SUBSCRIBE taken, and a NEW "sentinel_event_seq" publishes "sentinel_event"
         (default +switch-master) with "sentinel_event_payload" to the subscribers,
         "sentinel_event_count" times in one write (a burst; default 1); each
         SENTINEL <sub> and SUBSCRIBE counted
     "info_noperm": true - INFO is refused, as for a user without +info,
     "acl": {"user": {"pw": "p", "write": true|false}, ...} - Redis ACL
         users instead of "password": AUTH [user] pw (or HELLO 3 AUTH user pw;
         no user is "default") must name one, every command before it is
         NOAUTH, and a WRITE from a user without "write" is answered NOPERM.
         Each AUTH is noted in <dir>/auth ("user ok" / "user refused"),
         each NOPERM in <dir>/noperm,
     "role": "master" | "replica" - what ROLE and INFO replication say
         (master by default, as a lone Redis is),
     "link": "up" | "down" - a replica's link to its master,
     "last_io_s": N - seconds since a replica last heard from its master,
     write-through:
     "accept_writes": true - a MASTER applies the writes perfcached
         forwards (SET [EX|PX|NX|XX|GET], SETEX, DEL, UNLINK, EXPIRE,
         PEXPIRE, PERSIST, INCR/INCRBY/DECRBY, APPEND, HSET, LPUSH,
         JSON.SET <$|$.field|.field>, JSON.DEL) into data.json - still
         noted in <dir>/writes; without it every write is refused (a
         replica answers READONLY),
     "replicate_to": ["dir", ...] - after a write the keyspace is copied
         there too (the replica fakes serve it),
     "replicate_delay_ms": N or {"dir": N, ...} - that copy N ms
         after the write (a replica's lag, per replica; its tracking echoes
         the write then),
     "push_on_write": true - a write here pushes its broadcast
         invalidations before the writer's reply, as Redis does (else the
         watcher's 50 ms poll sends them); "push_delay_ms": N sends them N
         ms after it - the push and the reply travel on two connections,
         and either may land first,
     "master_host"/"master_port" - what a replica's ROLE and INFO
         replication name as its master,
     "json": false - no RedisJSON: COMMAND INFO has no JSON.* and the
         JSON commands are unknown,
     "cmds": {"name": [arity, [flags], first, last, step, [cats]]} - COMMAND INFO
         and GETKEYS answer these as well (over the table below); a "name|sub" entry
         is listed among name's sub-commands,
     "cmd_info_delay_ms": {"name": N} - COMMAND INFO of that name answered N ms late,
     "error_cmds": {"TYPE": "LOADING ...", "SCAN": ...} - those commands answered with that
         error; with "error_n": N only the first N since "error_seq" last changed}
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
HOST = sys.argv[3] if len(sys.argv) > 3 else "127.0.0.1"   # IP6-2: "::1" listens on IPv6
READS = {b"HELLO", b"AUTH", b"SELECT", b"PING", b"INFO", b"TYPE", b"PTTL", b"TTL",
         b"GET", b"HGETALL", b"JSON.GET", b"EXISTS", b"CLIENT", b"QUIT",
         b"COMMAND", b"SCAN", b"ROLE", b"LRANGE", b"SENTINEL", b"SUBSCRIBE",
         b"CLUSTER", b"READONLY", b"READWRITE", b"ASKING"}
# COMMAND INFO's table - name: (arity, flags, first, last, step, ACL cats)
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
# one write at a time, as Redis applies them: load, change, save here and to the replicas;
# concurrent writers would save their own copies and replicas land out of order
wlock = threading.Lock()
rep_seq = {}                         # S423: replica dir -> [last queued, last landed]
cache = {"data": ({}, None), "ctl": ({}, None)}
born = time.time()
conns = {}                 # id -> (socket, send lock)
targets = set()            # ids named by CLIENT TRACKING ... REDIRECT
bcast = {}                 # AR1: id -> [prefixes] (empty: every key)
next_id = [100]


def crc16(b):
    """Redis Cluster's CRC16 (XMODEM)"""
    crc = 0
    for c in b:
        crc ^= c << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def key_slot(k):
    """the key's slot, with Redis's hash tag"""
    o = k.find(b"{")
    if o >= 0:
        c = k.find(b"}", o + 1)
        if c > o + 1:
            k = k[o + 1:c]
    return crc16(k) % 16384


def cl_shard_of(cl, slot):
    for sh in cl["shards"]:
        if any(lo <= slot <= hi for lo, hi in sh["slots"]):
            return sh
    return None


def cl_reply(cl, sub):
    """CLUSTER SHARDS or SLOTS, in RESP2 - "report" when the topology said lags the one routed"""
    shards = cl.get("report") or cl["shards"]
    if sub == b"SHARDS":
        out = []
        for sh in shards:
            nodes = [arr([bulk(x) for x in ["id", "%040x" % n[1], "port", str(n[1]), "ip", n[0],
                                              "endpoint", n[0], "role", n[2], "health",
                                              n[3] if len(n) > 3 else "online"]])
                     for n in sh["nodes"]]
            out.append(arr([bulk("slots"), arr([b":%d\r\n" % x for lo, hi in sh["slots"]
                                                 for x in (lo, hi)]),
                            bulk("nodes"), arr(nodes)]))
        return arr(out)
    out = []
    for sh in shards:
        ns = sorted(sh["nodes"], key=lambda n: n[2] != "master")
        for lo, hi in sh["slots"]:
            out.append(arr([b":%d\r\n" % lo, b":%d\r\n" % hi] +
                           [arr([bulk(n[0]), b":%d\r\n" % n[1], bulk("%040x" % n[1])]) for n in ns]))
    return arr(out)


def cl_route(cl, key, readonly, asking):
    """None to serve here, else the error line (MOVED / ASK)"""
    slot = key_slot(key)
    sh = cl_shard_of(cl, slot)
    ks = key.decode("latin-1")
    me = [n for n in (sh or {}).get("nodes", []) if n[1] == PORT]
    if ks in cl.get("importing", []) and asking:
        return None
    master = [n for n in (sh or {}).get("nodes", []) if n[2] == "master"]
    if not sh or not master:
        return b"-CLUSTERDOWN Hash slot not served\r\n"
    if not me or (me[0][2] != "master" and not readonly):
        # "moved_empty_host": Redis 7's unknown-endpoint form, ":port" - the node asked
        host = b"" if cl.get("moved_empty_host") else master[0][0].encode()
        return b"-MOVED %d %s:%d\r\n" % (slot, host, master[0][1])
    if ks in cl.get("ask", {}):
        h, p = cl["ask"][ks]
        return b"-ASK %d %s:%d\r\n" % (slot, h.encode(), p)
    return None


def stamp(path):
    """what identifies a version of @path, and whether it may still change
    under that identity.  The mtime alone does not: it comes from the
    kernel's coarse clock, and 1929 of 1999 back-to-back os.replace()s on
    the runners left the same one - a write landing
    in the tick of the one this process last read was never seen, and a
    replica served (and never pushed for) a keyspace one write behind:
    upwritethroughtest 15 "Redis v47, node 'v46'" and 3's document without
    its EXPIRE.  So: mtime_ns, inode (os.replace
    gives every version its own) and size - and a file written in the last
    second is read again whatever it says"""
    st = os.stat(path)
    return (st.st_mtime_ns, st.st_ino, st.st_size), time.time() - st.st_mtime < 1.0


def load(name):
    path = os.path.join(DIR, name + ".json")
    try:
        mt, young = stamp(path)
    except OSError:
        return {}
    with lock:
        obj, seen = cache[name]
        if mt != seen or young:
            try:
                with open(path) as f:
                    obj = json.load(f)
            except (OSError, ValueError):
                obj = {}
            cache[name] = (obj, mt)
        return obj


def alive(ent):
    """an entry still alive - ttl_ms (since this fake started, the
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
    """the keyspace after a write - here, and to the replicas;
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
        cache["data"] = (data, stamp(os.path.join(DIR, "data.json"))[0])


def cmd_info(names, ctl):
    out = []
    for n in names:
        n = n.decode("latin-1").lower()
        e = (ctl.get("cmds") or {}).get(n) or CMDS.get(n)
        if e is None or (n.startswith("json.") and ctl.get("json") is False):
            out.append(b"*-1\r\n")
            continue
        tab = dict(CMDS, **(ctl.get("cmds") or {}))
        subs = [cmd_ent(s, tab[s], b"*0\r\n") for s in sorted(tab) if s.startswith(n + "|")]
        out.append(cmd_ent(n, e, arr(subs)))
    return arr(out)


def cmd_ent(n, e, subs):
    """one COMMAND INFO entry; its 10th element lists a container's sub-commands, as Redis 7's does"""
    ar, fl, fi, la, st, cat = e
    return arr([bulk(n), b":%d\r\n" % ar,
                arr([b"+%s\r\n" % x.encode() for x in fl]),
                b":%d\r\n" % fi, b":%d\r\n" % la, b":%d\r\n" % st,
                arr([b"+%s\r\n" % x.encode() for x in cat]),
                b"*0\r\n", b"*0\r\n", subs])


def apply_write(cmd, ctl):
    """apply one forwarded write; the reply as Redis gives it"""
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


err_state = {"seq": None, "n": 0}


def injected(ctl, name):
    """error_cmds' reply for @name (counted as sent), b"" when none or error_n of them were
    sent since error_seq changed"""
    ec = (ctl.get("error_cmds") or {}).get(name.decode("latin-1").upper())
    if not ec:
        return b""
    with lock:
        if ctl.get("error_seq") != err_state["seq"]:
            err_state["seq"], err_state["n"] = ctl.get("error_seq"), 0
        if "error_n" in ctl and err_state["n"] >= ctl["error_n"]:
            return b""
        err_state["n"] += 1
    return b"-%s\r\n" % ec.encode()


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
    """the keys whose entry changed since @last, pushed to every BCAST
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
subscribed = set()             # S479: connections in SUBSCRIBE (a sentinel's events)


def bcast_now():
    """the broadcast pushes for what changed since the last look - from the
    watcher's poll, or ("push_on_write") from the write itself"""
    with bc_lock:
        if bc_state["last"] is None:
            bc_state["last"] = load("data")
        bc_state["last"] = bcast_changes(bc_state["last"])


def watcher():
    seen_inv = seen_kill = seen_ev = None
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
        ev = ctl.get("sentinel_event_seq")                 # S479: a sentinel's event
        if ev is not None and ev != seen_ev:
            if seen_ev is not None:
                msg = arr([bulk("message"), bulk(ctl.get("sentinel_event", "+switch-master")),
                           bulk(ctl.get("sentinel_event_payload", "mym 127.0.0.1 1 127.0.0.1 2"))])
                msg *= int(ctl.get("sentinel_event_count", 1))
                with lock:
                    es = [conns.get(c) for c in subscribed]
                for e in es:
                    if e:
                        try:
                            with e[1]:
                                e[0].sendall(msg)
                        except OSError:
                            pass
                note("pushes", "sentinel event")
            seen_ev = ev


def serve(conn):
    f = conn.makefile("rb")
    authed = False
    resp3 = False
    who = None                 # AR7: the ACL user this connection is
    readonly = asking = False  # S478
    with lock:
        cid = next_id[0]
        next_id[0] += 1
        conns[cid] = (conn, threading.Lock())
    try:
        while True:
            ctl = load("ctl")
            if ctl.get("down"):
                note("counts", "DOWN")       # S478: a connection closed unanswered, counted
                return
            cmd = read_cmd(f)
            if cmd is None:
                return
            name = cmd[0].upper()
            ctl = load("ctl")
            was_asking, asking = asking, False          # S478: ASKING is one command's
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
            if name not in READS and ctl.get("cluster") is not None and len(cmd) > 1:
                # a write to another shard's slot is MOVED; keys of two slots, CROSSSLOT
                ks = cmd[1:2] if name not in (b"DEL", b"UNLINK") else cmd[1:]
                if len({key_slot(k) for k in ks}) > 1:
                    note("counts", "CROSSSLOT " + name.decode())
                    conn.sendall(b"-CROSSSLOT Keys in request don't hash to the same slot\r\n")
                    continue
                err = cl_route(ctl["cluster"], cmd[1], False, was_asking)
                if err:
                    note("counts", "%s %s %s" % (err[1:err.index(b" ")].decode(), name.decode(),
                                                 cmd[1].decode("latin-1")))
                    conn.sendall(err)
                    continue
            if name not in READS:
                note("writes", " ".join(c.decode("latin-1") for c in cmd))
                if ctl.get("write_delay_ms"):         # AR2: a slow master
                    time.sleep(ctl["write_delay_ms"] / 1000.0)
                if (ctl.get("accept_writes") and ctl.get("role") != "replica"
                        and name in WRITES and len(cmd) > 1):
                    with wlock:
                        wrep = apply_write(cmd, ctl)
                    conn.sendall(wrep)
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
                if sub == b"INFO":                  # S479: laddr, the address it answers at
                    conn.sendall(bulk("id=%d addr=127.0.0.1:1 laddr=%s fd=8 name= db=0\n" % (
                        cid, ctl.get("laddr", "127.0.0.1:%d" % PORT))))
                    continue
                if sub == b"ID":
                    if ctl.get("client_id_delay_ms"):      # S431
                        time.sleep(ctl["client_id_delay_ms"] / 1000.0)
                    conn.sendall(b":%d\r\n" % cid)
                    continue
                if sub == b"TRACKING":
                    up = [c.upper() for c in cmd]
                    if b"BCAST" in up:             # AR1
                        pre = [cmd[i + 1].decode("latin-1") for i, c in enumerate(up)
                               if c == b"PREFIX" and i + 1 < len(cmd)]
                        with lock:
                            bcast[cid] = pre
                    if b"REDIRECT" in up and ctl.get("redirect_refused"):
                        note("counts", "TRACKING REFUSED")
                        conn.sendall(b"-NOPERM this user has no permissions to run the "
                                     b"'client|tracking' command\r\n")
                        continue
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
                if ctl.get("redis_mode") == "cluster" and cmd[1:2] != [b"0"]:      # S478
                    conn.sendall(b"-ERR SELECT is not allowed in cluster mode\r\n")
                    continue
                conn.sendall(b"+OK\r\n")
                continue
            if name in (b"READONLY", b"READWRITE"):    # S478
                readonly = name == b"READONLY"
                note("counts", name.decode())
                conn.sendall(b"+OK\r\n")
                continue
            if name == b"ASKING":
                asking = True
                note("counts", "ASKING")
                conn.sendall(b"+OK\r\n")
                continue
            if name == b"CLUSTER" and ctl.get("cluster") is not None:
                sub = cmd[1].upper() if len(cmd) > 1 else b""
                note("counts", "CLUSTER " + sub.decode())
                if sub == b"SHARDS" and ctl["cluster"].get("slots_only"):
                    conn.sendall(b"-ERR unknown subcommand 'SHARDS'\r\n")
                elif sub in (b"SHARDS", b"SLOTS"):
                    conn.sendall(cl_reply(ctl["cluster"], sub))
                else:
                    conn.sendall(b"-ERR unknown subcommand\r\n")
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
                    cd = ctl.get("cmd_info_delay_ms") or {}
                    late = [cd[c.decode("latin-1").lower()] for c in cmd[2:]
                            if c.decode("latin-1").lower() in cd]
                    if late:
                        time.sleep(max(late) / 1000.0)
                    conn.sendall(cmd_info(cmd[2:], ctl))
                elif sub == b"GETKEYS" and len(cmd) > 2:
                    gk = cmd[2].decode("latin-1").lower()
                    e = (ctl.get("cmds") or {}).get(gk) or CMDS.get(gk)
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
            if name == b"SUBSCRIBE" and ctl.get("sentinel") is not None:  # S479
                note("counts", "SUBSCRIBE")
                with lock:
                    subscribed.add(cid)
                conn.sendall(b"".join(arr([bulk("subscribe"), bulk(ch), b":%d\r\n" % (i + 1)])
                                      for i, ch in enumerate(cmd[1:])))
                continue
            if name == b"SENTINEL" and ctl.get("sentinel") is not None:   # S479
                sub = cmd[1].upper() if len(cmd) > 1 else b""
                note("counts", "SENTINEL " + sub.decode(errors="replace"))
                ms = ctl["sentinel"]
                nm = cmd[2].decode() if len(cmd) > 2 else ""
                flat = lambda d: arr([bulk(str(x)) for kv in d.items() for x in kv])
                if sub == b"MASTERS":
                    conn.sendall(arr([flat({"name": n, "ip": m["ip"], "port": m["port"], "flags": "master"})
                                      for n, m in ms.items()]))
                elif nm not in ms:
                    conn.sendall(b"-ERR No such master with that name\r\n")
                elif sub == b"GET-MASTER-ADDR-BY-NAME":
                    conn.sendall(arr([bulk(ms[nm]["ip"]), bulk(str(ms[nm]["port"]))]))
                elif sub in (b"REPLICAS", b"SLAVES"):
                    conn.sendall(arr([flat({"name": "%s:%d" % (r[0], r[1]), "ip": r[0], "port": r[1],
                                            "flags": r[2] if len(r) > 2 else "slave"})
                                      for r in ms[nm].get("replicas", [])]))
                elif sub == b"SENTINELS":
                    conn.sendall(arr([flat({"name": "s%d" % i, "ip": x[0], "port": x[1]})
                                      for i, x in enumerate(ms[nm].get("sentinels", []))]))
                else:
                    conn.sendall(b"-ERR unknown sentinel subcommand\r\n")
                continue
            if name == b"ROLE":                       # S382
                note("counts", "ROLE")
                if ctl.get("role") == "replica":
                    conn.sendall(arr([bulk("slave"), bulk(ctl.get("master_host", "127.0.0.1")),
                                      b":%d\r\n" % int(ctl.get("master_port", 6379)),
                                      bulk("connected"), b":1000\r\n"]))
                else:                           # S479: the replicas it names
                    reps = ctl.get("replicas", [])
                    conn.sendall(b"*3\r\n$6\r\nmaster\r\n:1000\r\n" + arr(
                        [arr([bulk(h), bulk(str(p)), bulk("1000")]) for h, p in reps]))
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
                # its own name, not `who` (the connection's ACL user): reusing that made the next command look
                # up acl["fake-N"] and die on a KeyError
                srv = "fake-%d" % PORT
                if ctl.get("run_id_per_conn"):
                    srv += "-%d" % cid
                rid = hashlib.sha1(srv.encode()).hexdigest()
                note("counts", "INFO " + rid[:12])
                body = ("# Server\r\nredis_version:7.4.0\r\nredis_mode:%s\r\n"
                        "run_id:%s\r\ntcp_port:%d\r\n" % (ctl.get("redis_mode", "standalone"), rid, PORT))
                if ctl.get("perfcached_version"):
                    body += "perfcached_version:%s\r\n" % ctl["perfcached_version"]
                body = body.encode()
                if resp3:
                    conn.sendall(b"=%d\r\ntxt:%s\r\n" % (len(body) + 4, body))
                else:
                    conn.sendall(bulk(body))
                continue
            ec = injected(ctl, name) if name == b"SCAN" else b""
            if ec:
                note("counts", "SCAN refused")
                conn.sendall(ec)
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
                cl = ctl.get("cluster")
                if cl is not None:              # S478: a node lists its own slots' keys
                    keys = [k for k in keys if any(n[1] == PORT for n in
                            (cl_shard_of(cl, key_slot(k.encode("latin-1"))) or {}).get("nodes", []))]
                part = keys[cur:cur + cnt]
                nxt = cur + cnt if cur + cnt < len(keys) else 0
                note("counts", "SCAN %d %s" % (cur, pat))
                conn.sendall(arr([bulk(str(nxt)), arr([bulk(k) for k in part])]))
                continue
            key = cmd[1].decode("latin-1") if len(cmd) > 1 else ""
            if ctl.get("cluster") is not None and len(cmd) > 1:              # S478
                err = cl_route(ctl["cluster"], cmd[1], readonly, was_asking)
                if err:
                    note("counts", "%s %s %s" % (err[1:err.index(b" ")].decode(), name.decode(), key))
                    conn.sendall(err)
                    continue
            note("counts", name.decode() + " " + key)
            ec = injected(ctl, name)
            if ec:                                   # -LOADING, -BUSY ...: refused, not answered
                conn.sendall(ec)
                continue
            dk = ctl.get("delay_keys") or []
            if ctl.get("delay_ms") and (not dk or key in dk):
                time.sleep(ctl["delay_ms"] / 1000.0)
            data = load("data")
            rk = ctl.get("reply_delay_keys") or []
            rc_ = ctl.get("reply_delay_cmds") or []
            if ctl.get("reply_delay_ms") and (not rk or key in rk) and \
                    (not rc_ or name.decode("latin-1").upper() in rc_):
                # the value is read now and the reply sent later: a reply in flight while the key changes and its
                # invalidation is pushed on the other connection
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


def tls_serve(c, ctx):
    import ssl
    try:
        c.settimeout(5)
        t = ctx.wrap_socket(c, server_side=True)
        t.settimeout(None)
    except (ssl.SSLError, OSError) as e:
        note("tls", "refused %s" % str(e)[:160])
        c.close()
        return
    cn = "-"
    for rdn in (t.getpeercert() or {}).get("subject", ()):
        for k, v in rdn:
            if k == "commonName":
                cn = v
    note("tls", "ok %s" % cn)
    serve(t)


def main():
    ctx = None
    if os.environ.get("FAKEUP_TLS"):
        import ssl
        part = os.environ["FAKEUP_TLS"].split(":")
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(part[0], part[1])
        if len(part) > 2:
            ctx.load_verify_locations(part[2])
            ctx.verify_mode = ssl.CERT_REQUIRED
    s = socket.socket(socket.AF_INET6 if ":" in HOST else socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, PORT))
    s.listen(64)
    threading.Thread(target=watcher, daemon=True).start()
    with open(os.path.join(DIR, "ready"), "w") as f:
        f.write("ready\n")
    while True:
        c, _ = s.accept()
        if load("ctl").get("down"):
            c.close()
            continue
        threading.Thread(target=tls_serve if ctx else serve, args=(c, ctx) if ctx else (c,),
                         daemon=True).start()


main()
