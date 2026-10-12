"""Drive one real call through rtpengine's ng control protocol.

Wire: "<cookie> <bencoded dict>" over UDP; the reply is the same shape.
offer -> answer -> delete is the full lifecycle, which is what makes
rtpengine write, update and remove its Redis call state.
"""
import socket
import sys

NG = int(sys.argv[1])


def be(o):
    if isinstance(o, bytes):
        return b"%d:%s" % (len(o), o)
    if isinstance(o, str):
        return be(o.encode())
    if isinstance(o, int):
        return b"i%de" % o
    if isinstance(o, list):
        return b"l" + b"".join(be(x) for x in o) + b"e"
    if isinstance(o, dict):
        out = b"d"
        for k in sorted(o):
            out += be(k) + be(o[k])
        return out + b"e"
    raise TypeError(o)


def bdec(b, i=0):
    c = chr(b[i])
    if c == "i":
        j = b.index(b"e", i)
        return int(b[i + 1:j]), j + 1
    if c == "l":
        i += 1
        out = []
        while chr(b[i]) != "e":
            v, i = bdec(b, i)
            out.append(v)
        return out, i + 1
    if c == "d":
        i += 1
        out = {}
        while chr(b[i]) != "e":
            k, i = bdec(b, i)
            v, i = bdec(b, i)
            out[k.decode("latin1")] = v
        return out, i + 1
    j = b.index(b":", i)
    n = int(b[i:j])
    return b[j + 1:j + 1 + n], j + 1 + n



s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(5)
s.connect(("127.0.0.1", NG))
s.send(b"cap99 " + be({"command": "list"}))
print("list ->", bdec(s.recv(65535).split(b" ", 1)[1])[0])
