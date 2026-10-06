#!/usr/bin/env python3
"""difftest.py - S317's gate: the 0.4.5 daemon (binary protocol only)
answers every native method the same as the last daemon with the JSON-RPC
door (p044) did - compared as DECODED VALUES, not as bytes, after the
shape changes DESIGN 12if documents are normalised away:

  - values and keys are bytes on the tree: the reference's "enc":"b64"
    sibling, "k_enc", and {"b64":..,"enc":"b64"} key objects decode to
    the same bytes; a tree bulk decodes to the same bytes
  - an error is a frame with the message only: the reference's
    {"code":N,"message":M} compares on M
  - hcmd answers the command's reply item itself (no {"reply":..} wrapper,
    no "tree" hex twin)
  - stats loses native.json; members' clients lose "json"
  - stats gains process.allocator (S282, 0.5.0 - after the p044 reference):
    dropped on the new side, and only that key, so anything else new
    under process still differs
  - each collection in stats gains autoscale_floor_log2, and autoscale
    gains the floor mode, so its refusal of an unknown mode lists it
    (S328, 0.5.1): that key and that one message, nothing else
  - replies whose VALUES are the node's own state (stats, members,
    collections, probe, pubsub_udp) compare by SHAPE: same keys, same
    types, recursively

Usage: difftest.py <ref-port> <new-port>    (both plaintext on loopback,
the daemons started by difftest.sh with the same config)
Exit 0 when every request matched; the mismatches are printed.
"""
import base64
import json
import os
import socket
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcnative  # noqa: E402

REF_PORT, NEW_PORT = int(sys.argv[1]), int(sys.argv[2])
ENABLE = "dt-enable"


# ---- the reference: JSON-RPC lines ----------------------------------------
class Ref:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), 8)
        self.s.settimeout(20)
        self.f = self.s.makefile("rwb")
        self.rid = 0

    def call(self, method, params=None):
        self.rid += 1
        req = {"jsonrpc": "2.0", "id": self.rid, "method": method}
        if params is not None:
            req["params"] = _ref_params(params)
        self.f.write((json.dumps(req) + "\n").encode())
        self.f.flush()
        line = self.f.readline()
        if not line:
            return {"__closed__": True}
        r = json.loads(line)
        if "error" in r:
            return {"__error__": r["error"].get("message")}
        return _norm_ref(method, r["result"])

    def close(self):
        self.s.close()


VALUE_KEYS = {"value", "payload", "echo", "v"}     # take an "enc":"b64" sibling
KEY_ENC = {"k": "k_enc"}                           # take their own sibling


def _ref_params(p):
    """bytes params -> what the JSON door could carry: UTF-8 text (a NUL
    rides as \\u0000), or base64 with the enc sibling where the door had
    one (values, and restore's k).  Anywhere else the corpus must not put
    bytes the door cannot express."""
    if isinstance(p, dict):
        out = {}
        for k, v in p.items():
            if isinstance(v, bytes):
                try:
                    out[k] = v.decode("utf-8")
                except UnicodeDecodeError:
                    if k in VALUE_KEYS:
                        out[k] = base64.b64encode(v).decode()
                        out["enc"] = "b64"
                    elif k in KEY_ENC:
                        out[k] = base64.b64encode(v).decode()
                        out[KEY_ENC[k]] = "b64"
                    else:
                        raise ValueError("corpus: %r cannot carry bytes on the JSON door" % k)
            elif isinstance(v, Doc):
                out[k] = v.value
            else:
                out[k] = _ref_params(v)
        return out
    if isinstance(p, list):
        return [x.decode("utf-8") if isinstance(x, bytes) else _ref_params(x) for x in p]
    return p


def _b(s):
    return s.encode("utf-8", "surrogateescape") if isinstance(s, str) else s


def _norm_ref(method, r):
    """the reference's result -> canonical: bytes for strings, b64 undone"""
    if method == "hcmd" and isinstance(r, dict) and "reply" in r:
        r = r["reply"]
    return _canon_ref(r)


def _canon_ref(v):
    if isinstance(v, dict):
        if set(v) == {"b64", "enc"}:
            return base64.b64decode(v["b64"])
        out = {}
        encs = {k for k in v if k == "enc" or k.endswith("_enc")}
        for k, x in v.items():
            if k in encs:
                continue
            ek = "enc" if k in VALUE_KEYS else k + "_enc"
            if ek in v and v[ek] == "b64" and isinstance(x, str):
                out[k] = base64.b64decode(x)
            else:
                out[k] = _canon_ref(x)
        if "json" in out and "binary" in out and "resp" in out and "conns" in out:
            del out["json"]                     # stats native.json
        if set(out) >= {"open", "resp", "binary", "json", "native_resp"}:
            del out["json"]                     # members clients.json
        return out
    if isinstance(v, list):
        return [_canon_ref(x) for x in v]
    if isinstance(v, str):
        return _b(v)
    return v


# ---- the new daemon: CMD frames -------------------------------------------
class New:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), 8)
        self.s.settimeout(20)
        self.w = pcnative.wrap(self.s)

    def call(self, method, params=None):
        p = _new_params(params)
        try:
            r = pcnative.call(self.w, method, **p) if p else pcnative.call(self.w, method)
        except IOError:
            return {"__closed__": True}
        if "error" in r:
            m = r["error"].get("message")
            if m == "mode is auto, floor, warn or off":   # S328
                m = "mode is auto, warn or off"
            return {"__error__": m}
        res = r.get("result")
        # D3: jget's value is one bulk of the fragment's JSON text where
        # the reference embedded the value itself - parse it to compare
        if method == "jget" and isinstance(res, dict) and isinstance(res.get("value"), str):
            res = dict(res)
            res["value"] = json.loads(res["value"])
        return _canon_new(res)

    def close(self):
        self.s.close()


def _new_params(p):
    if p is None:
        return {}
    out = {}
    for k, v in p.items():
        out[k] = v.value if isinstance(v, Doc) else v
    return out


def _canon_new(v):
    if isinstance(v, dict):
        out = {k: _canon_new(x) for k, x in v.items()}
        if isinstance(out.get("process"), dict):
            out["process"].pop("allocator", None)   # stats, S282
        if isinstance(out.get("collections"), list):
            for c in out["collections"]:
                if isinstance(c, dict):
                    c.pop("autoscale_floor_log2", None)   # stats, S328
        return out
    if isinstance(v, list):
        return [_canon_new(x) for x in v]
    if isinstance(v, str):
        return _b(v)
    if isinstance(v, pcnative.Num):
        try:
            return int(v)
        except ValueError:
            return float(v)
    return v


class Doc:
    """a document parameter: JSON text on the tree, the value on JSON lines"""
    def __init__(self, value):
        self.value = value


# ---- shape compare ---------------------------------------------------------
def shape_diff(a, b, path="$"):
    """None when a and b have the same keys and types, else the first diff"""
    if type(a) is not type(b):
        if isinstance(a, (int, float)) and isinstance(b, (int, float)):
            return None
        return "%s: type %s vs %s" % (path, type(a).__name__, type(b).__name__)
    if isinstance(a, dict):
        if set(a) != set(b):
            return "%s: keys %s vs %s" % (path, sorted(set(a) - set(b)), sorted(set(b) - set(a)))
        for k in a:
            d = shape_diff(a[k], b[k], "%s.%s" % (path, k))
            if d:
                return d
        return None
    if isinstance(a, list):
        # a list's LENGTH is state (the slow log, peers, foreign clusters):
        # only the element shape is compared, and only when both have one
        if a and b:
            return shape_diff(a[0], b[0], path + "[0]")
    return None


# ---- the corpus ------------------------------------------------------------
BIN = b"\x00\xff\x9e\n raw \x01"
BIG = b"v" * (200 * 1024)
SHAPE = {"stats", "members", "collections", "probe", "pubsub_udp", "pubsub_udp_confirm"}
C = "dt"      # the working collection, created by the corpus

CORPUS = [
    ("ping", None),
    ("ping", {"echo": "hello"}),
    ("ping", {"echo": BIN}),
    ("ping", {"echo": 5}),                             # bad echo
    ("frobnicate", {"x": 1}),                          # unknown method
    ("get", None),                                     # missing params
    ("get", {"col": "0"}),                             # missing key
    ("get", {"col": "nope", "key": "k"}),              # no such collection
    ("members", None),
    ("stats", None),
    ("stats", {"col": "0"}),
    ("collections", None),
    ("create", {"col": C}),                            # not privileged yet
    ("enable", {"secret": "wrong"}),
    ("enable", {"secret": ENABLE}),
    ("create", {"col": C, "buckets_log2": 10}),
    ("create", {"col": C}),                            # exists
    ("create", {"col": "dt2", "buckets_log2": 99}),    # bad size
    ("rename", {"col": "nope", "to": "x"}),
    ("resize", {"col": C}),                            # missing buckets_log2
    ("autoscale", {"col": C, "mode": "warn"}),
    ("autoscale", {"col": C, "mode": "bogus"}),
    ("collections", None),
    ("set", {"col": C, "key": "a", "value": "1"}),
    ("set", {"col": C, "key": "b", "value": BIN, "ttl": 300}),
    ("set", {"col": C, "key": "big", "value": BIG}),
    ("set", {"col": C, "key": b"k\x00bin", "value": "binkey"}),
    ("get", {"col": C, "key": "a"}),
    ("get", {"col": C, "key": "b"}),
    ("get", {"col": C, "key": "big"}),
    ("get", {"col": C, "key": b"k\x00bin"}),
    ("get", {"col": C, "key": "absent"}),
    ("exists", {"col": C, "key": "a"}),
    ("exists", {"col": C, "key": "absent"}),
    ("ttl", {"col": C, "key": "a"}),
    ("ttl", {"col": C, "key": "b"}),
    ("ttl", {"col": C, "key": "absent"}),
    ("expire", {"col": C, "key": "a", "ttl": 100}),
    ("expire", {"col": C, "key": "absent", "ttl": 100}),
    ("expire", {"col": C, "key": "a"}),                # missing ttl
    ("setnx", {"col": C, "key": "a", "value": "2"}),
    ("setnx", {"col": C, "key": "nx", "value": "2"}),
    ("setxx", {"col": C, "key": "nx", "value": "3"}),
    ("setxx", {"col": C, "key": "xx", "value": "3"}),
    ("add", {"col": C, "key": "ctr", "by": 5}),
    ("add", {"col": C, "key": "ctr"}),
    ("sub", {"col": C, "key": "ctr", "by": 2}),
    ("add", {"col": C, "key": "a", "by": 1}),          # "1" is an integer string
    ("add", {"col": C, "key": "b", "by": 1}),          # not an integer
    ("del", {"col": C, "key": "nx"}),
    ("del", {"col": C, "key": "nx"}),
    ("mget", {"col": C, "keys": ["a", "absent", "b", b"k\x00bin"]}),
    ("mget", {"col": C, "keys": []}),
    ("mset", {"col": C, "items": [{"key": "m1", "value": "x"}, {"key": "m2", "value": BIN, "ttl": 50}, "junk"]}),
    ("get", {"col": C, "key": "m2"}),
    ("keys", {"col": C, "limit": 1000}),
    ("keys", {"col": C, "match": "m*"}),
    ("keys", {"match": "m*"}),                         # the glob-collection form
    ("keys", {"col": "d*", "match": "*"}),
    ("keys", {"col": C, "limit": 0}),                  # out of range
    ("scan", {"col": C, "count": 100}),
    ("scan", {"col": C, "count": 100, "values": True, "match": "m*"}),
    ("jset", {"col": C, "key": "doc", "val": Doc({"a": 1, "s": "x", "l": [1, 2.5, None, True]})}),
    ("jget", {"col": C, "key": "doc"}),
    ("jget", {"col": C, "key": "doc", "path": "$.l"}),
    ("jget", {"col": C, "key": "doc", "path": "$.missing"}),
    ("jget", {"col": C, "key": "nodoc"}),
    ("jset", {"col": C, "key": "doc", "path": "$.n", "val": Doc(7)}),
    ("jset", {"col": C, "key": "doc", "path": "$.s", "val": Doc("str"), "xx": True}),
    ("jset", {"col": C, "key": "doc", "path": "$.s", "val": Doc("nx"), "nx": True}),
    ("jincr", {"col": C, "key": "doc", "path": "$.n", "by": 3}),
    ("jincr", {"col": C, "key": "doc", "path": "$.s"}),   # not a number
    ("jarrappend", {"col": C, "key": "doc", "path": "$.l", "val": Doc("tail")}),
    ("jget", {"col": C, "key": "doc"}),
    ("jdel", {"col": C, "key": "doc", "path": "$.a"}),
    ("jdel", {"col": C, "key": "doc", "path": "$.absent"}),
    ("jget", {"col": C, "key": "doc", "path": "bad path ["}),
    ("jset", {"col": C, "key": "a", "path": "$.x", "val": Doc(1)}),  # a string record
    ("jdel", {"col": C, "key": "doc"}),                # root: a delete
    ("jget", {"col": C, "key": "doc"}),
    ("hcmd", {"col": C, "key": "h", "cmd": "HSET", "args": ["f1", "v1", "f2", b"v\x00two"]}),
    ("hcmd", {"col": C, "key": "h", "cmd": "HGET", "args": ["f2"]}),
    ("hcmd", {"col": C, "key": "h", "cmd": "HGET", "args": ["nope"]}),
    ("hcmd", {"col": C, "key": "h", "cmd": "HGETALL", "args": []}),
    ("hcmd", {"col": C, "key": "h", "cmd": "HLEN"}),
    ("hcmd", {"col": C, "key": "h", "cmd": "HINCRBY", "args": ["f1", "1"]}),   # not an integer
    ("hcmd", {"col": C, "key": "h", "cmd": "HINCRBY", "args": ["n", "4"]}),
    ("hcmd", {"col": C, "key": "h", "cmd": "HBOGUS"}),
    ("hcmd", {"col": C, "key": "a", "cmd": "HGET", "args": ["x"]}),   # wrong type
    ("hcmd", {"col": C, "key": "h", "cmd": "HDEL", "args": ["f1", "f2", "n"]}),
    ("exists", {"col": C, "key": "h"}),
    ("rlhit", {"col": C, "key": "rl", "window_ms": 60000, "limit": 2}),
    ("rlhit", {"col": C, "key": "rl", "window_ms": 60000, "limit": 2}),
    ("rlhit", {"col": C, "key": "rl", "window_ms": 60000, "limit": 2}),
    ("rlhit", {"col": C, "key": "rl"}),                # missing window
    ("rlhit", {"col": C, "key": "a", "window_ms": 1000}),  # wrong type
    ("get", {"col": C, "key": "rl"}),                  # typed record read as a string
    ("dump", {"col": C, "count": 16}),
    ("dump", {"col": C, "count": 0}),
    ("dump", {"col": "nope"}),
    ("restore", {"col": C, "records": [{"k": "r1", "v": "rv1", "ver": 1}, {"k": "r2", "v": BIN, "exp": 0, "ver": 5}, 7, {"k": "r3"}]}),
    ("restore", {"col": C, "policy": "bogus", "records": []}),
    ("restore", {"col": C}),
    ("get", {"col": C, "key": "r2"}),
    ("publish", {"channel": "ch", "payload": "hello"}),
    ("publish", {"channel": "ch", "payload": BIN}),
    ("publish", {"channel": "__pc.x", "payload": "no"}),
    ("publish", {"channel": "ch"}),
    ("subscribe", {"channel": "s1"}),
    ("psubscribe", {"pattern": "p*"}),
    ("subscribe", None),
    ("unsubscribe", {"channel": "s1"}),
    ("punsubscribe", None),
    ("pubsub_udp", {"port": 0}),
    ("pubsub_udp", {"port": 70000}),
    ("pubsub_udp_confirm", {"cookie": "zz"}),
    ("pubsub_udp_ack", {"seq": -1}),
    ("sync", None),
    ("save", None),
    ("load", None),
    ("probe", {"secs": 1}),
    ("fleetstop", None),
    ("reset_stats", None),
    ("rename", {"col": C, "to": "dt_renamed"}),
    ("collections", None),
    ("drop", {"col": "dt_renamed"}),                   # not empty
    ("drop", {"col": "dt_renamed", "force": True}),
    ("drop", {"col": "dt_renamed"}),                   # gone
    ("disable", None),
    ("create", {"col": "dt3"}),                        # privilege dropped
    ("stats", None),
]


def show(v, n=160):
    s = repr(v)
    return s if len(s) <= n else s[:n] + "..."


def main():
    ref, new = Ref(REF_PORT), New(NEW_PORT)
    bad = 0
    for i, (method, params) in enumerate(CORPUS):
        a = ref.call(method, params)
        b = new.call(method, params)
        if method in SHAPE and not (isinstance(a, dict) and "__error__" in a):
            d = shape_diff(a, b)
            ok = d is None
            why = d
        else:
            ok = a == b
            why = None if ok else "ref %s\n      new %s" % (show(a), show(b))
        print("  %-4s %3d %-14s %s" % ("ok" if ok else "FAIL", i, method, show(params, 70)))
        if not ok:
            bad += 1
            print("      " + why)
    ref.close()
    new.close()
    print("difftest: %d requests, %d mismatches" % (len(CORPUS), bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
