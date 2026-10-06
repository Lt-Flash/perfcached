"""pcnative.py - the suites' client for the native door (S317).

The JSON-RPC dialect is gone: the native door speaks the binary protocol
only.  Every method rides PC_VERB_CMD - [verb 19][mlen u8][method] then
the parameters as a TREE (src/ptree.h) - and answers a tree, or an error
frame.  The suites were written against JSON-RPC lines, so this module
keeps that idiom and changes only the wire:

    s = socket.create_connection(("127.0.0.1", port))
    f = pcnative.wrap(s)                 # was: s.makefile("rwb")
    f.write(json.dumps(req).encode() + b"\\n"); f.flush()
    r = json.loads(f.readline())

Each complete request line written becomes one CMD frame at flush();
readline() hands back each reply as the line the JSON door used to
write - {"jsonrpc":"2.0","id":N,"result":<tree as JSON>} or
{"jsonrpc":"2.0","id":N,"error":{"code":-32000,"message":...}} - and a
NOTIFY frame as the id-less {"jsonrpc":"2.0","method":...,"params":...}
line, in arrival order.  A "moved" stamp beside the result is kept.

The tree is exact where JSON was not: a bulk is bytes, so a request
string goes out as its UTF-8 (surrogateescape both ways, so a value
that is not UTF-8 round-trips through str), a number keeps the text it
was written with, and a document (jset's val) travels as one bulk of
its JSON text.  An "enc":"b64" sibling in a request is honoured and
dropped - the bytes go as bytes.

Plaintext only: the suites run on loopback listeners.

call(f, method, **params) is the one-shot form: the decoded response.
"""
import base64
import json
import struct

MAGIC, VER, REQ, RSP, NOTIFY = 0x9E, 0x01, 1, 2, 3
F_ERR, F_MOVED = 0x01, 0x02
VERB_CMD = 19
HDR = struct.Struct("<BBBBIQ")
DOC_PARAMS = {"val"}               # a JSON value that travels as its text


class Num(str):
    """a JSON number kept as its text"""


def _b(s):
    if isinstance(s, (bytes, bytearray)):
        return bytes(s)
    return str(s).encode("utf-8", "surrogateescape")


def _tree(v, out, doc=False):
    if doc:
        t = dumps(v).encode()
        out += b"b" + struct.pack("<I", len(t)) + t
    elif v is None:
        out += b"n"
    elif v is True:
        out += b"t"
    elif v is False:
        out += b"f"
    elif isinstance(v, Num):
        t = str(v).encode()
        out += b"d" + struct.pack("<H", len(t)) + t
    elif isinstance(v, int):
        out += b"i" + struct.pack("<q", v)
    elif isinstance(v, float):
        t = repr(v).encode()
        out += b"d" + struct.pack("<H", len(t)) + t
    elif isinstance(v, (str, bytes, bytearray)):
        b = _b(v)
        out += b"b" + struct.pack("<I", len(b)) + b
    elif isinstance(v, (list, tuple)):
        out += b"a" + struct.pack("<I", len(v))
        for e in v:
            _tree(e, out)
    elif isinstance(v, dict):
        d = dict(v)
        encs = [k for k in d if k == "enc" or k.endswith("_enc")]
        for ek in encs:
            if d[ek] == "b64":
                vk = "value" if ek == "enc" else ek[:-4]
                if vk in d:
                    d[vk] = base64.b64decode(d[vk])
            del d[ek]
        out += b"m" + struct.pack("<I", len(d))
        for k, e in d.items():
            kb = _b(k)
            out += b"b" + struct.pack("<I", len(kb)) + kb
            _tree(e, out, doc=k in DOC_PARAMS)
    else:
        raise TypeError("pcnative: cannot encode %r" % (v,))


def frame(rid, method, params=None):
    """one CMD request frame"""
    m = method.encode()
    body = bytearray([VERB_CMD, len(m)]) + m
    if params is not None:
        _tree(params, body)
    return HDR.pack(MAGIC, VER, REQ, 0, len(body), rid) + bytes(body)


def _load(line):
    return json.loads(line, parse_float=Num, parse_int=Num)


class Err(dict):
    """an 'e' item: renders as {"error": line}"""


def decode(b, off=0):
    """one tree item at b[off:] -> (python value, next offset)"""
    tag = b[off:off + 1]
    off += 1
    if tag == b"n":
        return None, off
    if tag == b"t":
        return True, off
    if tag == b"f":
        return False, off
    if tag == b"o":
        return "OK", off
    if tag == b"i":
        return struct.unpack_from("<q", b, off)[0], off + 8
    if tag == b"d":
        n = struct.unpack_from("<H", b, off)[0]
        return Num(b[off + 2:off + 2 + n].decode()), off + 2 + n
    if tag in (b"b", b"e"):
        n = struct.unpack_from("<I", b, off)[0]
        s = b[off + 4:off + 4 + n].decode("utf-8", "surrogateescape")
        return (Err(error=s) if tag == b"e" else s), off + 4 + n
    if tag == b"a":
        n = struct.unpack_from("<I", b, off)[0]
        off += 4
        items = []
        for _ in range(n):
            v, off = decode(b, off)
            items.append(v)
        return items, off
    if tag == b"m":
        n = struct.unpack_from("<I", b, off)[0]
        off += 4
        d = {}
        for _ in range(n):
            k, off = decode(b, off)
            v, off = decode(b, off)
            d[k] = v
        return d, off
    raise ValueError("pcnative: bad tag %r at %d" % (tag, off - 1))


def _mark(o, nums):
    """Num -> a sentinel string; the texts are put back after the dump,
    since json has no hook for emitting a number as given"""
    if isinstance(o, Num):
        nums.append(str(o))
        return "\x00N%d\x00" % (len(nums) - 1)
    if isinstance(o, list):
        return [_mark(x, nums) for x in o]
    if isinstance(o, dict):
        return {k: _mark(v, nums) for k, v in o.items()}
    return o


def dumps(o):
    nums = []
    s = json.dumps(_mark(o, nums), separators=(",", ":"), ensure_ascii=True,
                   default=str)
    for i, t in enumerate(nums):
        s = s.replace('"\\u0000N%d\\u0000"' % i, t)
    return s


class Wire:
    """file-like: write() JSON-RPC lines, readline() reply lines"""

    def __init__(self, sock):
        self.s = sock
        self.wbuf = b""
        self.rbuf = b""
        self.roff = 0                  # consumed prefix of rbuf
        self.lines = []                # decoded replies not yet read
        self.auto = 1 << 62            # ids for requests that carried none

    # ---- requests ----
    def write(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.wbuf += data
        return len(data)

    def flush(self):
        # split once: a suite may pipeline tens of thousands of lines
        parts = self.wbuf.split(b"\n")
        self.wbuf = parts.pop()            # the trailing partial line
        out = []
        for line in parts:
            if not line.strip():
                continue
            req = _load(line)
            rid = req.get("id")
            if rid is None:
                self.auto += 1
                rid = self.auto
            out.append(frame(int(rid), req["method"], req.get("params")))
        if out:
            self.s.sendall(b"".join(out))

    def send(self, method, params=None, rid=None):
        if rid is None:
            self.auto += 1
            rid = self.auto
        self.s.sendall(frame(rid, method, params))
        return rid

    # ---- replies ----
    def _fill(self):
        if self.roff:                  # compact before growing
            self.rbuf = self.rbuf[self.roff:]
            self.roff = 0
        d = self.s.recv(1 << 20)
        if not d:
            return False
        self.rbuf += d
        return True

    def _frame(self):
        """one frame as a reply line, or None at EOF"""
        while len(self.rbuf) - self.roff < HDR.size:
            if not self._fill():
                return None
        magic, ver, typ, flags, plen, rid = HDR.unpack_from(self.rbuf, self.roff)
        if magic != MAGIC or ver != VER:
            raise IOError("pcnative: not a binary frame: %r" %
                          self.rbuf[self.roff:self.roff + 16])
        while len(self.rbuf) - self.roff < HDR.size + plen:
            if not self._fill():
                return None
        pay = self.rbuf[self.roff + HDR.size:self.roff + HDR.size + plen]
        self.roff += HDR.size + plen
        if typ == NOTIFY:
            v, _ = decode(pay)
            note = {"jsonrpc": "2.0"}
            if isinstance(v, dict):
                note.update(v)
            else:
                note["params"] = v
            return dumps(note).encode() + b"\n"
        if typ != RSP:
            raise IOError("pcnative: bad frame type %d" % typ)
        if flags & F_ERR:
            msg = pay.decode("utf-8", "replace")
            return dumps({"jsonrpc": "2.0", "id": rid, "error": {
                "code": -32000, "message": msg}}).encode() + b"\n"
        if plen:
            v, end = decode(pay)
        else:
            v, end = None, 0
        r = {"jsonrpc": "2.0", "id": rid, "result": v}
        if flags & F_MOVED and plen - end >= 8:
            term, seq = struct.unpack_from("<II", pay, end)
            r["moved"] = {"term": term, "seq": seq}
        return dumps(r).encode() + b"\n"

    def readline(self):
        if self.lines:
            return self.lines.pop(0)
        r = self._frame()
        return r if r is not None else b""

    def read(self, n=-1):
        raise IOError("pcnative: replies are lines; use readline()")

    def __iter__(self):
        while True:
            line = self.readline()
            if not line:
                return
            yield line

    def close(self):
        self.s.close()

    def fileno(self):
        return self.s.fileno()


def wrap(sock):
    return Wire(sock)


def call(f, method, **params):
    """one request, its response object (other lines before it kept)"""
    rid = f.send(method, params or None)
    while True:
        line = f.readline()
        if not line:
            raise IOError("pcnative: connection closed")
        r = json.loads(line)
        if r.get("id") == rid:
            return r
        f.lines.append(line)
