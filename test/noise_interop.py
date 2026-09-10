#!/usr/bin/env python3
# noise_interop.py — an INDEPENDENT Noise_NNpsk0_25519_ChaChaPoly_SHA256
# initiator, written from the Noise spec against the `cryptography`
# library (not perfcached's C).  If it interoperates with the daemon's
# responder, both implementations are spec-correct.  The PSK is passed in
# hex (obtained from `noisetest psk ...`, i.e. the same libsodium the
# daemon derives with) so Argon2id need not be reproduced here.
#
# argv: <host> <port> <key_id> <psk_hex> [mode]
#   mode: ok (default) | wrongpsk | tamper
# exit 0 = the expected outcome happened.
import socket, struct, sys, os
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives import hashes, hmac
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PublicKey

NAME = b"Noise_NNpsk0_25519_ChaChaPoly_SHA256"

def sha256(d):
    h = hashes.Hash(hashes.SHA256()); h.update(d); return h.finalize()

def hmac256(key, data):
    h = hmac.HMAC(key, hashes.SHA256()); h.update(data); return h.finalize()

def hkdf(ck, ikm, n):
    tk = hmac256(ck, ikm)
    o1 = hmac256(tk, b"\x01")
    if n == 1: return (o1,)
    o2 = hmac256(tk, o1 + b"\x02")
    if n == 2: return (o1, o2)
    o3 = hmac256(tk, o2 + b"\x03")
    return (o1, o2, o3)

def nonce(n):
    return b"\x00\x00\x00\x00" + struct.pack("<Q", n)

class Sym:
    def __init__(self, name):
        self.h = sha256(name) if len(name) > 32 else name.ljust(32, b"\x00")
        self.ck = self.h
        self.k = None
        self.n = 0
    def mix_key(self, ikm):
        self.ck, k = hkdf(self.ck, ikm, 2); self.k = k; self.n = 0
    def mix_hash(self, d):
        self.h = sha256(self.h + d)
    def mix_key_and_hash(self, ikm):
        self.ck, th, k = hkdf(self.ck, ikm, 3)
        self.mix_hash(th); self.k = k; self.n = 0
    def encrypt_and_hash(self, pt):
        if self.k is None:
            ct = pt
        else:
            ct = ChaCha20Poly1305(self.k).encrypt(nonce(self.n), pt, self.h)
            self.n += 1
        self.mix_hash(ct); return ct
    def decrypt_and_hash(self, ct):
        if self.k is None:
            pt = ct
        else:
            pt = ChaCha20Poly1305(self.k).decrypt(nonce(self.n), ct, self.h)
            self.n += 1
        self.mix_hash(ct); return pt
    def split(self):
        return hkdf(self.ck, b"", 2)

def recv_exact(s, n):
    b = b""
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d:
            return None
        b += d
    return b

def main():
    host, port, key_id, psk_hex = sys.argv[1], int(sys.argv[2]), \
        int(sys.argv[3]), sys.argv[4]
    mode = sys.argv[5] if len(sys.argv) > 5 else "ok"
    psk = bytes.fromhex(psk_hex)
    if mode == "wrongpsk":
        psk = os.urandom(32)

    s = socket.create_connection((host, port), timeout=5)

    sym = Sym(NAME)
    sym.mix_hash(bytes([key_id]))                 # prologue = key_id
    # -> psk, e
    sym.mix_key_and_hash(psk)
    e = X25519PrivateKey.generate()
    e_pub = e.public_key().public_bytes_raw()
    sym.mix_hash(e_pub); sym.mix_key(e_pub)
    ct1 = sym.encrypt_and_hash(b"\x01")           # client version
    noise1 = e_pub + ct1
    frame1 = struct.pack("<H", 1 + len(noise1)) + bytes([key_id]) + noise1
    if mode == "tamper":
        frame1 = bytearray(frame1); frame1[-1] ^= 0x01; frame1 = bytes(frame1)
    s.sendall(frame1)

    # <- e, ee   (a rejected handshake closes the socket instead)
    hdr = recv_exact(s, 2)
    if mode in ("wrongpsk", "tamper"):
        if hdr is None:
            print("ok: daemon rejected", mode); return 0
        print("FAIL:", mode, "was accepted"); return 1
    if hdr is None:
        print("FAIL: no msg2"); return 1
    msg2 = recv_exact(s, struct.unpack("<H", hdr)[0])
    re = msg2[:32]
    sym.mix_hash(re); sym.mix_key(re)
    ee = e.exchange(X25519PublicKey.from_public_bytes(re))
    sym.mix_key(ee)
    sym.decrypt_and_hash(msg2[32:])               # status
    send_k, recv_k = sym.split()                  # initiator: send=c1 recv=c2
    sn = rn = 0

    MAXPT = 65535 - 16                            # Noise message ceiling

    def send_plain(pt):                            # chunk into records
        nonlocal sn
        for i in range(0, max(len(pt), 1), MAXPT):
            chunk = pt[i:i + MAXPT]
            ct = ChaCha20Poly1305(send_k).encrypt(nonce(sn), chunk, b"")
            sn += 1
            s.sendall(struct.pack("<H", len(ct)) + ct)

    rbuf = bytearray()

    def recv_line():                               # reassemble the stream
        nonlocal rn
        while b"\n" not in rbuf:
            h = recv_exact(s, 2)
            ct = recv_exact(s, struct.unpack("<H", h)[0])
            rbuf.extend(ChaCha20Poly1305(recv_k).decrypt(nonce(rn), ct, b""))
            rn += 1
        i = rbuf.index(b"\n")
        line = bytes(rbuf[:i]); del rbuf[:i + 1]
        return line

    # a text ping over the encrypted channel
    send_plain(b'{"jsonrpc":"2.0","id":1,"method":"ping"}\n')
    line = recv_line()
    import json
    r = json.loads(line)
    assert r["id"] == 1 and r["result"]["pong"] is True, r
    print("ok: encrypted text ping round-trip")

    # a binary-value echo (b64/NUL) proves transport carries the codec
    import base64
    raw = b"Z\x00Y\xff"
    req = json.dumps({"jsonrpc": "2.0", "id": 2, "method": "ping",
        "params": {"echo": base64.b64encode(raw).decode(), "enc": "b64"}})
    send_plain(req.encode() + b"\n")
    r = json.loads(recv_line())
    assert base64.b64decode(r["result"]["echo"]) == raw, r
    print("ok: encrypted b64 NUL round-trip")

    # a large value spanning multiple transport records (chunking)
    big = base64.b64encode(os.urandom(200000)).decode()
    req = json.dumps({"jsonrpc": "2.0", "id": 3, "method": "ping",
        "params": {"echo": big}})
    send_plain(req.encode() + b"\n")
    r = json.loads(recv_line())
    assert r["result"]["echo"] == big, "large echo mismatch"
    print("ok: multi-record large value")
    return 0

sys.exit(main())
