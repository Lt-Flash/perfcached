#!/usr/bin/env python3
# fakesmtp.py - S352: a local SMTP server for the mail tests.  Standard
# library only (smtpd left Python in 3.12).  Loopback only.
#
#   fakesmtp.py --port N --dir D [--mode starttls|tls|plain] [--cert C --key K]
#               [--user U --password P] [--fail-auth] [--delay S] [--refuse]
#
#   starttls  plain first, advertises STARTTLS, a client upgrades (smtp://)
#   tls       TLS from the first byte (smtps://)
#   plain     no TLS at all
# Each accepted message is written to D/<n>.json: {"from", "rcpt", "user",
# "tls", "data"}.  --fail-auth answers 535 to every AUTH; --delay sleeps
# before each reply (a slow server); --refuse greets with 421 and closes.
# Writes D/ready when it listens.
import argparse, base64, json, os, socket, ssl, sys, threading, time

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, required=True)
ap.add_argument("--dir", required=True)
ap.add_argument("--mode", default="starttls", choices=("starttls", "tls", "plain"))
ap.add_argument("--cert")
ap.add_argument("--key")
ap.add_argument("--user", default="")
ap.add_argument("--password", default="")
ap.add_argument("--fail-auth", action="store_true")
ap.add_argument("--delay", type=float, default=0.0)
ap.add_argument("--refuse", action="store_true")
a = ap.parse_args()

ctx = None
if a.mode != "plain":
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(a.cert, a.key)
lock = threading.Lock()
seq = [0]


def save(msg):
    with lock:
        seq[0] += 1
        n = seq[0]
    p = os.path.join(a.dir, "%d.json" % n)
    with open(p + ".tmp", "w") as f:
        json.dump(msg, f)
    os.rename(p + ".tmp", p)


class Conn:
    def __init__(self, s, tls):
        self.s, self.tls, self.buf = s, tls, b""

    def send(self, line):
        if a.delay:
            time.sleep(a.delay)
        self.s.sendall(line.encode() + b"\r\n")

    def line(self):
        while b"\r\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                return None
            self.buf += d
        l, self.buf = self.buf.split(b"\r\n", 1)
        return l.decode("utf-8", "replace")

    def data(self):
        while b"\r\n.\r\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                return None
            self.buf += d
        body, self.buf = self.buf.split(b"\r\n.\r\n", 1)
        return body.decode("utf-8", "replace")


def serve(sock):
    try:
        if a.mode == "tls":
            sock = ctx.wrap_socket(sock, server_side=True)
        c = Conn(sock, a.mode == "tls")
        if a.refuse:
            c.send("421 fakesmtp: refusing")
            return
        c.send("220 fakesmtp ESMTP")
        user, frm, rcpt = None, None, []
        while True:
            l = c.line()
            if l is None:
                return
            v = l.split(" ", 1)[0].upper()
            arg = l[len(v):].strip()
            if v in ("EHLO", "HELO"):
                ext = ["250-fakesmtp"]
                if a.mode == "starttls" and not c.tls:
                    ext.append("250-STARTTLS")
                ext.append("250-AUTH PLAIN LOGIN")
                ext.append("250 8BITMIME")
                for e in ext:
                    c.send(e)
            elif v == "STARTTLS" and a.mode == "starttls" and not c.tls:
                c.send("220 go ahead")
                c.s = ctx.wrap_socket(c.s, server_side=True)
                c.tls, c.buf = True, b""
            elif v == "AUTH":
                p = arg.split()
                if not p:
                    c.send("501 syntax")
                    continue
                if p[0].upper() == "PLAIN":
                    if len(p) > 1:
                        cred = p[1]
                    else:
                        c.send("334 ")
                        cred = c.line() or ""
                    try:
                        _, u, pw = base64.b64decode(cred).decode().split("\0")
                    except Exception:
                        c.send("501 bad PLAIN")
                        continue
                elif p[0].upper() == "LOGIN":
                    c.send("334 " + base64.b64encode(b"Username:").decode())
                    u = base64.b64decode(c.line() or "").decode()
                    c.send("334 " + base64.b64encode(b"Password:").decode())
                    pw = base64.b64decode(c.line() or "").decode()
                else:
                    c.send("504 mechanism")
                    continue
                if a.fail_auth or u != a.user or pw != a.password:
                    c.send("535 5.7.8 authentication failed")
                else:
                    user = u
                    c.send("235 2.7.0 ok")
            elif v == "MAIL":
                if a.user and user is None:
                    c.send("530 5.7.0 authentication required")
                    continue
                frm, rcpt = arg.split(":", 1)[1].strip(), []
                c.send("250 ok")
            elif v == "RCPT":
                rcpt.append(arg.split(":", 1)[1].strip())
                c.send("250 ok")
            elif v == "DATA":
                c.send("354 end with .")
                body = c.data()
                if body is None:
                    return
                save({"from": frm, "rcpt": rcpt, "user": user, "tls": c.tls, "data": body})
                c.send("250 queued as fake")
            elif v == "RSET":
                frm, rcpt = None, []
                c.send("250 ok")
            elif v == "NOOP":
                c.send("250 ok")
            elif v == "QUIT":
                c.send("221 bye")
                return
            else:
                c.send("502 not implemented")
    except Exception as e:
        sys.stderr.write("fakesmtp: %s\n" % e)
    finally:
        try:
            sock.close()
        except Exception:
            pass


ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("127.0.0.1", a.port))
ls.listen(16)
open(os.path.join(a.dir, "ready"), "w").close()
while True:
    s, _ = ls.accept()
    threading.Thread(target=serve, args=(s,), daemon=True).start()
