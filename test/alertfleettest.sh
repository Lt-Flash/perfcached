#!/bin/sh
# alertfleettest.sh - S352c: the fleet side of the alerts (DESIGN 12le).  A
# node without [mail] hands what it would mail to the ELECTED mailer (the
# lowest ready node id whose ALIVE says it can mail), which mails it naming
# the origin; a FLEET event - a member gone, missing, the fleet below a
# majority - is mailed by that one node, once, however many members see it.
# Three nodes, eager, [mail] on node 1 only, against test/fakesmtp.py
# (STARTTLS on a self-signed cert, AUTH) and test/fakeupstream.py, with
# PERFCACHED_ALERT_FAST=20 (every hold and clear time / 20).  "Node 1..3"
# are the three configs, started in that order; the fleet gives them their
# ids, which the checks read back:
#  1. node 1 mails itself; nodes 2 and 3 relay to it: alerts.mailer "this
#     node" / "node 1", fleet_mailer 1 on all three
#  a. node 3's read-through upstream is down (nodes 1 and 2 reach theirs - a
#     read-through collection's upstream is the node's own, not the fleet's,
#     though its name is the fleet's): the alert arrives by mail via
#     node 1, naming node 3; node 3's ring says "relayed to node 1", node
#     1's holds it from node 3; mail.relayed_out on 3, relayed_in on 1
#  b. node 2 is stopped: exactly ONE mail says it left, sent through node 1;
#     node 3 keeps it as "mailed by node 1"
#  c. node 1 is stopped too: node 3 logs "no mailer reachable", keeps it so
#     in its ring, and its alerts.mailer says none
#  d. the SMTP password is in no log, no /stats and no page
# Node 1 runs a PC_MAIL=libcurl build of this tree (made here when the
# binary under test has no SMTP client and libcurl's development files are
# installed; SKIPPED, loudly, when not); nodes 2 and 3 run the binary under
# test - a build without an SMTP client relays like any node without [mail].
# Fail-first: on d98cb48 (S352b, a PC_MAIL=libcurl build) every check but the
# fleet forming and d fails - nothing relays, nothing reports the fleet.
# Usage: test/alertfleettest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
pass=0 fail=0 skip=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
skp() { skip=$((skip+1)); echo "  SKIP $1"; }
P_SMTP=19541 P_UP=19542 P_DEAD=19543
for p in $P_SMTP $P_UP $P_DEAD 19511 19512 19513 19521 19522 19523; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "alertfleettest: port $p already bound" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcalfleet.XXXXXX)
P1= P2= P3= FS= FU=
trap 'for p in $P1 $P2 $P3 $FS $FU; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
PW=afleet-Smtp-Pw-5517
export PERFCACHED_ALERT_FAST=20
summary() { echo "alertfleettest: $pass passed, $fail failed, $skip skipped"; [ $fail -eq 0 ]; exit $?; }

# ---- the node that mails: a PC_MAIL=libcurl build ---------------------------------
MBIN=
printf '[secrets]\nclient = probe-secret\n[listen]\ntcp = 127.0.0.1:19511\nplaintext = loopback\n[collection 0]\nbuckets_log2 = 8\n[mail]\nsmtp = smtp://127.0.0.1:25\nfrom = a@example.net\nto = b@example.net\n' > "$D/probe.conf"
chmod 600 "$D/probe.conf"
if "$BIN" -C -f "$D/probe.conf" > "$D/probe.out" 2>&1; then
	MBIN=$BIN
elif pkg-config --exists libcurl 2>/dev/null; then
	echo "--- building this tree with PC_MAIL=libcurl"
	mkdir -p "$D/t"
	(cd "$T/.." && tar -cf - --exclude=./.git --exclude='*.o' --exclude='*.a' --exclude=config.mk . 2>/dev/null) |
		(cd "$D/t" && tar -xf -)
	make -s -C "$D/t" -j"$(nproc 2>/dev/null || echo 4)" PC_MAIL=libcurl perfcached > "$D/build.log" 2>&1 &&
		MBIN="$D/t/perfcached" || bad "the PC_MAIL=libcurl build failed: $(grep -m3 -E 'error|Error' "$D/build.log" | tr '\n' ' ')"
else
	skp "all: libcurl's development files are not installed - no node can mail here"
fi
[ -n "$MBIN" ] || summary
command -v openssl > /dev/null 2>&1 &&
	openssl req -x509 -newkey rsa:2048 -nodes -keyout "$D/key.pem" -out "$D/cert.pem" -days 2 \
		-subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1" > /dev/null 2>&1
[ -s "$D/cert.pem" ] || { skp "all: no openssl to make the fake's certificate"; summary; }

mkdir -p "$D/m" "$D/up"
python3 "$T/fakesmtp.py" --port $P_SMTP --dir "$D/m" --mode starttls --cert "$D/cert.pem" --key "$D/key.pem" \
	--user alerts@example.net --password "$PW" > "$D/smtp.log" 2>&1 &
FS=$!
echo '{"down": false}' > "$D/up/ctl.json"
echo '{}' > "$D/up/data.json"
python3 "$T/fakeupstream.py" $P_UP "$D/up" > "$D/up.log" 2>&1 &
FU=$!
i=0; while [ $i -lt 50 ] && { [ ! -f "$D/m/ready" ] || [ ! -f "$D/up/ready" ]; }; do sleep 0.1; i=$((i+1)); done

conf() { # conf <n> - node 1 has [mail]; node 3's read-through upstream is a dead port
	mkdir -p "$D/s$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = afleet-client-secret
cluster = afleet-cluster-secret
EOF
	[ "$1" = 1 ] && echo "mail = $PW" >> "$D/n$1.conf"
	cat >> "$D/n$1.conf" <<EOF
[listen]
tcp = 127.0.0.1:1951$1
http = 127.0.0.1:1952$1
plaintext = loopback
[cluster]
multicast = 239.255.78.21:19530
advertise = 127.0.81.$1
mode = eager
collections = rt
[collection rt]
buckets_log2 = 8
EOF
	up=$P_UP; [ "$1" = 3 ] && up=$P_DEAD
	printf 'upstream = redis://127.0.0.1:%s/0\nupstream_prefixes = *\nupstream_timeout_ms = 200\n' $up >> "$D/n$1.conf"
	[ "$1" = 1 ] && cat >> "$D/n$1.conf" <<EOF
[mail]
smtp = smtp://127.0.0.1:$P_SMTP
user = alerts@example.net
from = perfcached@example.net
to = ops@example.net
ca_file = $D/cert.pem
digest_s = 2
min_interval_s = 900
EOF
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> <binary>
	"$2" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & eval "P$1=\$!"
	i=0; while [ $i -lt 150 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	return 1
}
st() { # st <n> <what> [key] - one figure off node n's /stats
	timeout 6 python3 - "$@" <<'PY' 2>/dev/null
import json, sys, urllib.request
n, what = sys.argv[1], sys.argv[2]
s = json.loads(urllib.request.urlopen("http://127.0.0.1:1952%s/stats" % n, timeout=4).read())
a, m = s.get("alerts", {}), s.get("mail", {})
if what == "ready": print("%s %s" % (s.get("state"), s.get("cluster", {}).get("peers_up")))
elif what == "node": print(s.get("cluster", {}).get("node"))
elif what == "mailer": print("%s/%s" % (a.get("mailer"), a.get("fleet_mailer")))
elif what == "ring":     # ring <key> -> "event/mail" of the entries for that key, newest first
    print(" | ".join("%s/%s" % (e["event"], e["mail"]) for e in a.get("recent", []) if e["key"] == sys.argv[3]))
elif what == "from":     # from <node> -> "key/mail" of the entries relayed from that node
    print(" | ".join("%s/%s" % (e["key"], e["mail"]) for e in a.get("recent", []) if e.get("from_node") == int(sys.argv[3])))
elif what in m: print(m[what])
PY
}
waitfor() { # waitfor <seconds> <shell condition>
	w=0; while [ $w -lt $(($1 * 10)) ]; do eval "$2" && return 0; sleep 0.1; w=$((w+1)); done; return 1
}
# mails <regex> - how many mails' subject or body match
mails() { python3 - "$D/m" "$1" <<'PY'
import glob, json, re, sys
print(sum(1 for f in glob.glob(sys.argv[1] + "/*.json") if re.search(sys.argv[2], json.load(open(f))["data"])))
PY
}
# mailof <regex> - the first mail that matches: its subject, the body line
# that matches and the body's first line (which node sent it)
mailof() { python3 - "$D/m" "$1" <<'PY'
import glob, json, re, sys
for f in sorted(glob.glob(sys.argv[1] + "/*.json"), key=lambda p: int(p.rsplit("/", 1)[1].split(".")[0])):
    d = json.load(open(f))["data"]
    if re.search(sys.argv[2], d):
        ls = d.split("\r\n")
        subj = [l[9:] for l in ls if l.startswith("Subject: ")]
        line = [l for l in ls if re.search(sys.argv[2], l) and not l.startswith("Subject: ")]
        hdr = [l for l in ls if l.startswith("perfcached ")]
        print("%s || %s || %s" % (subj[0] if subj else "?", line[0].strip() if line else "?", hdr[0] if hdr else "?"))
        break
PY
}

# ---- the fleet ----------------------------------------------------------------------
conf 1; conf 2; conf 3
start 1 "$MBIN" && start 2 "$BIN" && start 3 "$BIN" || { bad "a node did not start: $(tail -2 "$D"/n*.log | tr '\n' ' ')"; summary; }
waitfor 30 '[ "$(st 1 ready)" = "ready 2" ] && [ "$(st 2 ready)" = "ready 2" ] && [ "$(st 3 ready)" = "ready 2" ]' &&
	ok "three nodes formed a fleet" || { bad "no fleet: $(st 1 ready) / $(st 2 ready) / $(st 3 ready)"; summary; }

N1=$(st 1 node) N2=$(st 2 node) N3=$(st 3 node)
echo "--- node ids: node 1 is $N1, node 2 is $N2, node 3 is $N3"

# ---- 1. who mails --------------------------------------------------------------------
waitfor 10 '[ "$(st 1 mailer)" = "this node/$N1" ] && [ "$(st 2 mailer)" = "node $N1/$N1" ] && [ "$(st 3 mailer)" = "node $N1/$N1" ]'
[ "$(st 1 mailer)" = "this node/$N1" ] && ok "1. node 1 mails itself and is the fleet's mailer (this node/$N1)" ||
	bad "1. node 1: alerts.mailer/fleet_mailer $(st 1 mailer)"
[ "$(st 2 mailer)" = "node $N1/$N1" ] && [ "$(st 3 mailer)" = "node $N1/$N1" ] &&
	ok "1. nodes 2 and 3, without [mail], relay to node 1 (node $N1/$N1)" || bad "1. nodes 2, 3: $(st 2 mailer), $(st 3 mailer)"

# ---- a. node 3's own alert, by mail through node 1 ---------------------------------------
waitfor 20 '[ "$(mails "read-through upstream 127.0.0.1:$P_DEAD")" -ge 1 ]'
M=$(mailof "read-through upstream 127.0.0.1:$P_DEAD")
# alone in its mail, or in a digest with the starts that were relayed too
case "$M" in *"[warning] "*"|| "*"warning   RAISED   node $N3 ("*"): the read-through upstream 127.0.0.1:$P_DEAD"*"|| perfcached "*", node $N1, state ready")
	ok "a. node 3's dead upstream arrives by mail via node 1, naming node 3 ($M)";;
	*) bad "a. no mail for node 3's upstream through node 1 ($M; $(ls "$D/m" | grep -c json) mails)";; esac
case "$(st 3 ring upstream.rt)" in "raised/relayed to node $N1"*) ok "a. node 3 keeps it as 'relayed to node $N1'";;
	*) bad "a. node 3's ring: $(st 3 ring upstream.rt)";; esac
case "$(st 1 from $N3)" in *"upstream.rt/mailed"*) ok "a. node 1 keeps it too, from node 3, mailed";;
	*) bad "a. node 1's ring from node 3: $(st 1 from $N3)";; esac
[ "$(st 3 relayed_out)" -ge 1 ] 2>/dev/null && [ "$(st 1 relayed_in)" -ge 1 ] 2>/dev/null &&
	ok "a. mail.relayed_out $(st 3 relayed_out) on node 3, relayed_in $(st 1 relayed_in) on node 1" ||
	bad "a. counters: node 3 relayed_out '$(st 3 relayed_out)', node 1 relayed_in '$(st 1 relayed_in)'"
# what the password test reads later, from nodes that will be gone by then
for n in 1 2; do
	curl -s "http://127.0.0.1:1952$n/stats" > "$D/stats$n.json" 2>/dev/null
	curl -s "http://127.0.0.1:1952$n/" > "$D/page$n.html" 2>/dev/null
done

# ---- b. node 2 stopped: one fleet-event mail, from node 1 -----------------------------------
kill -TERM $P2; wait $P2 2>/dev/null; P2=
LEFT="member node $N2 \\(127\\.0\\.81\\.2\\) left"
waitfor 20 '[ "$(mails "$LEFT")" -ge 1 ]'
sleep 6                                    # the hold and a digest more, for a second one
M=$(mailof "$LEFT")
case "$(mails "$LEFT")/$M" in "1/"*"RAISED   $(echo "$LEFT" | tr -d '\\')"*"|| perfcached "*", node $N1, state ready")
	ok "b. node 2 stopped: exactly ONE mail says it left, sent through node 1 ($M)";;
	*) bad "b. $(mails "$LEFT") mails say node 2 left ($M)";; esac
case "$(st 3 ring fleet.node$N2)" in "raised/mailed by node $N1"*) ok "b. node 3 saw it too and keeps it as 'mailed by node $N1'";;
	*) bad "b. node 3's ring for fleet.node$N2: $(st 3 ring fleet.node$N2)";; esac
case "$(st 1 ring fleet.node$N2)" in "raised/mailed"*) ok "b. node 1 keeps it as mailed";;
	*) bad "b. node 1's ring for fleet.node$N2: $(st 1 ring fleet.node$N2)";; esac

# ---- c. node 1 stopped too: nobody can mail ---------------------------------------------------
L3=$(wc -l < "$D/n3.log")
curl -s "http://127.0.0.1:19521/stats" > "$D/stats1b.json" 2>/dev/null
kill -TERM $P1; wait $P1 2>/dev/null; P1=
waitfor 20 'tail -n +$((L3 + 1)) "$D/n3.log" | grep -q "no mailer reachable"'
tail -n +$((L3 + 1)) "$D/n3.log" | grep -q "WARNING: alerts: no mailer reachable" &&
	ok "c. node 1 gone too: node 3 logs 'no mailer reachable'" || bad "c. node 3 never said 'no mailer reachable'"
case "$(st 3 ring fleet.node$N1)" in "raised/no mailer reachable"*) ok "c. and keeps node 1's leaving as 'no mailer reachable'";;
	*) bad "c. node 3's ring for fleet.node$N1: $(st 3 ring fleet.node$N1)";; esac
[ "$(st 3 mailer)" = "none/0" ] && ok "c. node 3's alerts.mailer: none" || bad "c. node 3's mailer: $(st 3 mailer)"

# ---- d. the password, nowhere -----------------------------------------------------------------------
curl -s "http://127.0.0.1:19523/stats" > "$D/stats3.json" 2>/dev/null
curl -s "http://127.0.0.1:19523/" > "$D/page3.html" 2>/dev/null
if [ -s "$D/stats1.json" ] && [ -s "$D/stats3.json" ] && [ -s "$D/page1.html" ]; then
	grep -l -- "$PW" "$D"/n*.log "$D"/stats*.json "$D"/page*.html > "$D/leak" 2>/dev/null
	[ -s "$D/leak" ] && bad "d. the SMTP password is in $(cat "$D/leak" | tr '\n' ' ')" ||
		ok "d. the SMTP password is in no log, no /stats and no page"
else
	bad "d. nothing to search (stats/page not captured)"
fi
summary
