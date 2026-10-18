#!/bin/sh
# alerttest.sh - S352b: a node raises its own alerts, keeps them, and mails
# them through [mail] - against test/fakesmtp.py and test/fakeupstream.py on
# loopback, with PERFCACHED_ALERT_FAST=60 (every hold and clear time / 60).
#  A. no [mail]: an alert is raised, logged and kept (/stats alerts, mail
#     "no mailer"); alerts.mailer says none
#  B. one node with [mail] (digest_s 3, min_interval_s 900):
#     1. its start and a dead read-through upstream, raised within the
#        digest window, arrive as ONE mail naming both
#     2. the upstream comes up: the clear is mailed (its raise was)
#     3. it goes down again inside min_interval: raised and kept, but
#        suppressed - no second mail
#     4. writes refused for want of memory: a CRITICAL mail at once
#     5. kill -9 and restart: "after an UNCLEAN stop" mailed critical
#  C. level = critical: a warning is kept but not mailed ("below level")
#  D. S377 (0.6.1): level = info and a test event whose text carries markup -
#     the clean start is mailed (info), the markup arrives escaped in HTML
#  E. S377: a condition a warning, then critical, then clear (driven by
#     PERFCACHED_ALERT_TEST_COND): the critical mail says "Problem, still
#     open" and answers the warning's mail; the resolve answers the
#     warning's mail too (the problem's FIRST mail), with the worst text
#  and every mail (S377): multipart/alternative with a text/plain and a
#     text/html part, quoted-printable (no line over 76), the subject
#     "<STATE>: <title> on <host>[ (+N)]" in at most 78 characters, From
#     the host's quoted name, no "RAISED" anywhere; a resolve answers its
#     problem's mail (In-Reply-To)
# The node with [mail] is a PC_MAIL=libcurl build of this tree (made here
# when libcurl's development files are installed; B and C SKIPPED, loudly,
# when not).
# Usage: test/alerttest.sh ./perfcached
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcalert.XXXXXX)
PID= FS= FU=
trap 'for p in $PID $FS $FU; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0 skip=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
skp() { skip=$((skip+1)); echo "  SKIP $1"; }
P_SMTP=17861 P_UP=17862 TCP=17863 HTTP=17864
for p in $P_SMTP $P_UP $TCP $HTTP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "alerttest: port $p already bound" >&2; exit 1; }
done
export PERFCACHED_ALERT_FAST=60

# ---- the fake upstream, down to begin with ------------------------------------
mkdir -p "$D/up"
echo '{"down": true}' > "$D/up/ctl.json"
echo '{}' > "$D/up/data.json"
python3 "$T/fakeupstream.py" $P_UP "$D/up" > "$D/up.log" 2>&1 &
FU=$!
i=0; while [ $i -lt 50 ] && [ ! -f "$D/up/ready" ]; do sleep 0.1; i=$((i+1)); done
up() { echo "{\"down\": $1}" > "$D/up/ctl.json.tmp" && mv "$D/up/ctl.json.tmp" "$D/up/ctl.json"; }

conf() { # conf <file> [mail lines...] - one node, a read-through collection on the fake
	f=$1; shift
	mkdir -p "$D/wal"                  # the daemon does not make it (S323)
	cat > "$f" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 16
[secrets]
client = alert-client-secret
[listen]
tcp = 127.0.0.1:$TCP
http = 127.0.0.1:$HTTP
plaintext = loopback
[wal]
dir = $D/wal
# B4 fills the 16 MB arena with 150 KB values: the ring must hold what the
# writer cannot drain meanwhile on a loaded runner, or 'WAL lost writes'
# refuses first and the arena-full CRITICAL this suite is about never comes
ring_kb = 16384
[collection 0]
buckets_log2 = 8
upstream = redis://127.0.0.1:$P_UP/0
upstream_prefixes = *
upstream_timeout_ms = 200
[collection big]
buckets_log2 = 8
[collection 2]
buckets_log2 = 8
[alerts]
cpu_warn_pct = 99
cpu_crit_pct = 100
io_warn_pct = 99
io_crit_pct = 100
memory_warn_pct = 99
memory_crit_pct = 100
EOF
	# the HOST's alarms are not this suite's subject (hostalerttest, iobusytest):
	# on a CI host running eight jobs they fired - "CPU busy", "disk busy" - and
	# a digest headed by one carried E's RESOLVED where E could not see it
	# (v0.6.4-rc5, GitLab 110463).  The arena's stay at their defaults: B4 uses them
	if [ $# -gt 0 ]; then
		printf '[mail]\n' >> "$f"
		for l in "$@"; do printf '%s\n' "$l" >> "$f"; done
	fi
	chmod 600 "$f"
}
start() { # start <binary> <conf>
	"$1" -f "$2" >> "$D/n.log" 2>&1 &
	PID=$!
	# 30 s: under ASan on a 2-core runner the PC_MAIL build was not ready
	# in 10, and the next /stats read came back empty (GitHub, v0.6.2)
	i=0; while [ $i -lt 300 ] && ! grep -q "perfcached ready" "$D/n.log"; do sleep 0.1; i=$((i+1)); done
}
stop() { [ -n "$PID" ] && { kill "$1" $PID 2>/dev/null; wait $PID 2>/dev/null; }; PID=; : > "$D/n.log"; }
al() { python3 - "$HTTP" "$@" <<'PY' 2>/dev/null
import json, sys, urllib.request
s = json.loads(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=5).read())
a = s["alerts"]; what = sys.argv[2]
if what == "mailer": print(a["mailer"])
elif what == "active": print(" ".join("%s:%s" % (x["key"], x["level"]) for x in a["active"]))
elif what == "recent":   # recent <key> -> "event/mail" of the newest entries for that key
    print(" ".join("%s/%s" % (e["event"], e["mail"]) for e in a["recent"] if e["key"] == sys.argv[3]))
PY
}
wst() { # wst <python expression over w (/stats wal) and r (/stats rdb)> - printed
	python3 - "$HTTP" "$1" <<'PY' 2>/dev/null
import json, sys, urllib.request
s = json.loads(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=5).read())
w = s.get("wal") or {}; r = s.get("rdb") or {}
print(eval(sys.argv[2]))
PY
}
waitfor() { # waitfor <seconds> <shell condition>
	w=0; while [ $w -lt $(($1 * 10)) ]; do eval "$2" && return 0; sleep 0.1; w=$((w+1)); done; return 1
}
mails() { ls "$D/m"/*.json 2>/dev/null | wc -l; }
metric() { # metric <series> - its value on /metrics
	python3 -c 'import sys,urllib.request
for l in urllib.request.urlopen("http://127.0.0.1:%s/metrics" % sys.argv[1], timeout=5).read().decode().splitlines():
    if l.startswith(sys.argv[2] + " "): print(l.split()[-1])' $HTTP "$1" 2>/dev/null; }
resp() { # resp <db> <command words...> - one RESP command per word list, replies discarded
	python3 - $TCP "$@" <<'PY' > /dev/null 2>&1
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 5)
f = s.makefile("rb")
def cmd(*a):
    s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
    f.readline()
cmd(b"SELECT", sys.argv[2].encode())
w = [x.encode() for x in sys.argv[3:]]
if w[0] == b"FILL":                         # FILL <n>: SET k0..k<n-1> to small values
    for i in range(int(w[1])):
        cmd(b"SET", b"w%d" % i, b"v" * 64)
else:
    cmd(*w)
PY
}
# S377: the mail is MIME, its parts quoted-printable - read them decoded
mpart() { # mpart <n> <subject|text|html|msgid|inreplyto> - one mail's part, decoded
	python3 - "$D/m/$1.json" "$2" <<'PY' 2>/dev/null
import email, json, sys
from email import policy
m = email.message_from_string(json.load(open(sys.argv[1]))["data"], policy=policy.default)
w = sys.argv[2]
if w == "subject": print(m["Subject"])
elif w == "msgid": print(m["Message-ID"])
elif w == "inreplyto": print(m["In-Reply-To"] or "")
else:
    for p in m.walk():
        if p.get_content_type() == "text/" + ("plain" if w == "text" else "html"):
            print(p.get_content())
PY
}
subj() { mpart "$1" subject; }
findmail() { # findmail <after n> <subject regex> - the first such mail's number
	python3 - "$D/m" "$1" "$2" <<'PY' 2>/dev/null
import email, json, re, sys
from email import policy
i = int(sys.argv[2])
while True:
    i += 1
    try:
        m = email.message_from_string(json.load(open("%s/%d.json" % (sys.argv[1], i)))["data"], policy=policy.default)
    except OSError:
        break
    if re.search(sys.argv[3], m["Subject"] or ""):
        print(i)
        break
PY
}
body() { mpart "$1" text; }

# ---- A. no [mail] ----------------------------------------------------------------
conf "$D/a.conf"
start "$BIN" "$D/a.conf"
waitfor 10 '[ -n "$(al active)" ]'
[ "$(al mailer)" = none ] && ok "A. no [mail]: alerts.mailer is none" || bad "A. mailer: $(al mailer)"
case "$(al active)" in *upstream.0:warning*) ok "A. the dead upstream is raised and active (warning)";; *) bad "A. active: $(al active)";; esac
case "$(al recent upstream.0)" in raised/no\ mailer*) ok "A. kept in the ring as 'no mailer'";; *) bad "A. ring: $(al recent upstream.0)";; esac
grep -q "WARNING: alert: the read-through upstream" "$D/n.log" && ok "A. and logged at WARNING" || bad "A. not logged"
[ "$(metric 'perfcached_mail_ready')" = 0 ] && [ "$(metric 'perfcached_alerts_active{level="warning"}')" -ge 1 ] 2>/dev/null &&
	ok "A. /metrics: perfcached_mail_ready 0, an active warning counted" ||
	bad "A. /metrics: ready '$(metric perfcached_mail_ready)', warning '$(metric 'perfcached_alerts_active{level="warning"}')'"
stop -TERM

# ---- the mail build ----------------------------------------------------------------
MBIN=
if "$BIN" -C -f /dev/null 2>&1 | grep -q x; then :; fi
printf '[mail]\nsmtp = smtp://127.0.0.1:25\nfrom = a@example.net\nto = b@example.net\n' > "$D/probe.mail"
conf "$D/probe.conf"; cat "$D/probe.mail" >> "$D/probe.conf"
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
	skp "B, C: libcurl's development files are not installed - no node can mail here"
fi
if [ -n "$MBIN" ]; then
	if command -v openssl > /dev/null 2>&1; then
		openssl req -x509 -newkey rsa:2048 -nodes -keyout "$D/key.pem" -out "$D/cert.pem" -days 2 \
			-subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1" > /dev/null 2>&1
	fi
	[ -s "$D/cert.pem" ] || { skp "B, C: no openssl to make the fake's certificate"; MBIN=; }
fi
if [ -n "$MBIN" ]; then
	mkdir -p "$D/m"
	python3 "$T/fakesmtp.py" --port $P_SMTP --dir "$D/m" --mode starttls --cert "$D/cert.pem" --key "$D/key.pem" > "$D/smtp.log" 2>&1 &
	FS=$!
	i=0; while [ $i -lt 50 ] && [ ! -f "$D/m/ready" ]; do sleep 0.1; i=$((i+1)); done
	M="smtp = smtp://127.0.0.1:$P_SMTP"
	M2="from = perfcached@example.net"
	M3="to = ops@example.net"
	M4="ca_file = $D/cert.pem"

	# ---- B1. the start and a dead upstream, one digest ---------------------------
	rm -rf "$D/wal"
	conf "$D/b.conf" "$M" "$M2" "$M3" "$M4" "digest_s = 3" "min_interval_s = 900"
	start "$MBIN" "$D/b.conf"
	[ "$(al mailer)" = "this node" ] && ok "B. alerts.mailer: this node" || bad "B. mailer: $(al mailer)"
	waitfor 15 '[ "$(mails)" -ge 1 ]'
	S1=$(subj 1)
	H=$(hostname)
	[ "$S1" = "WARNING: read-through upstream on $H" ] && ok "B1. the dead upstream's mail: '$S1'" ||
		bad "B1. first mail: '$S1' ($(mails) mails)"
	body 1 | grep -q "What happened  the read-through upstream 127.0.0.1:$P_UP" && body 1 | grep -q "Status         Problem, new" &&
		ok "B1. its text part says what happened and that it is a new problem" || bad "B1. body: $(body 1 | head -20 | tr '\n' '|')"
	case "$(al recent node.start)" in raised/below\ level*) ok "B1. the clean start is info: kept, 'below level' (warning)";;
		*) bad "B1. the start's ring entry: $(al recent node.start)";; esac

	# ---- B2. the upstream comes up: the clear is mailed ----------------------------
	up false
	waitfor 25 '[ "$(mails)" -ge 2 ]'
	[ "$(subj 2)" = "RESOLVED: read-through upstream on $H" ] && ok "B2. the upstream back: '$(subj 2)'" ||
		bad "B2. second mail: '$(subj 2)' ($(mails) mails, ring $(al recent upstream.0))"
	[ -n "$(mpart 1 msgid)" ] && [ "$(mpart 2 inreplyto)" = "$(mpart 1 msgid)" ] &&
		ok "B2. it answers the problem's mail (In-Reply-To $(mpart 2 inreplyto))" ||
		bad "B2. In-Reply-To '$(mpart 2 inreplyto)', the problem's Message-ID '$(mpart 1 msgid)'"
	body 2 | grep -q "^  Lasted " && body 2 | grep -q "closes the problem mailed at" &&
		ok "B2. it says how long it lasted and which mail it closes" || bad "B2. body: $(body 2 | head -24 | tr '\n' '|')"

	# ---- B3. down again inside min_interval: suppressed ----------------------------
	# the upstream dies: every connection to it closes at once (a "down"
	# flag in the fake closes one only when it next reads).  Idle fetch
	# connections notice on their next use - a few misses, as a real
	# outage meets its first readers
	kill -9 $FU 2>/dev/null; wait $FU 2>/dev/null; FU=
	waitfor 25 'resp 0 GET probe$w; case "$(al recent upstream.0)" in raised/*) true;; *) false;; esac'
	sleep 4
	case "$(al recent upstream.0)" in raised/suppressed*) ok "B3. raised again inside min_interval: kept, mail 'suppressed'";;
		*) bad "B3. ring: $(al recent upstream.0)";; esac
	[ "$(mails)" = 2 ] && ok "B3. and no third mail" || bad "B3. $(mails) mails: $(subj 3)"

	# ---- B4. writes refused: critical, at once -------------------------------------
	n0=$(mails)
	python3 - $TCP <<'PY' > "$D/fill.out" 2>&1
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10)
f = s.makefile("rb")
v = b"x" * 150000
err = 0
for i in range(400):
    k = b"fill%d" % i
    s.sendall(b"*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n" % (len(k), k, len(v), v))
    l = f.readline()
    if l.startswith(b"-"):
        err += 1
        if err >= 5:
            break
print("errors", err)
PY
	waitfor 10 '[ "$(mails)" -gt "$n0" ]'
	# a digest: the refusals may share a mail with what they caused (the
	# WAL heal) - look for the line, and the mail's level
	F=
	for i in $(seq $((n0 + 1)) $(mails)); do
		body $i | grep -q "What happened  writes refused, the arena is full" && F=$(subj $i)
	done
	c=$(metric 'perfcached_alerts_active{level="critical"}'); s=$(metric perfcached_mail_sent_total)
	[ "${c:-0}" -ge 1 ] && [ "${s:-0}" -ge 2 ] && ok "B4. /metrics: $c critical alert(s) active, mail_sent_total $s" ||
		bad "B4. /metrics: critical '$c', mail_sent_total '$s'"
	case "$F" in "CRITICAL: "*) ok "B4. writes refused for want of memory: mailed in a CRITICAL mail ($F)";;
		*) bad "B4. no 'writes refused' line in a critical mail ($(cat "$D/fill.out"); subjects: $(for i in $(seq $((n0 + 1)) $(mails)); do subj $i; done | tr '\n' '|'))";; esac

	# ---- B5. kill -9 and back: an unclean stop ------------------------------------------
	# The start calls a stop unclean when it replays WAL records above the last
	# snapshot - so the records must BE above it at the kill.  B4's refusals start a
	# heal, and a heal is a snapshot: records written while it runs land under it,
	# the restart replays none and says a clean start (v0.6.4-rc3, GitLab 110419,
	# check-asan: B4's mail one entry longer, B5's one shorter, no UNCLEAN).  The heal
	# is waited out, then the records are seen synced above the snapshot's marker
	waitfor 30 '[ "$(wst "bool(w.get(\"healing\") or r.get(\"running\"))")" = False ]' ||
		echo "  info B5. a heal or a snapshot still running after 30 s: $(wst '(w.get("healing"), r.get("running"))')"
	n0=$(mails)
	resp 2 FILL 40
	waitfor 10 '[ "$(wst "w[\"synced_seq\"] >= w[\"last_seq\"] > r.get(\"last_marker\", 0) and not w.get(\"healing\") and not r.get(\"running\")")" = True ]'
	pre=$(wst '(w["last_seq"], w["synced_seq"], r.get("last_marker"), w.get("healing"), r.get("running"))')
	stop -9
	start "$MBIN" "$D/b.conf"
	# the start's own entry may share a digest with what the restart raised, or follow it
	unclean() {
		F=
		for i in $(seq $((n0 + 1)) $(mails)); do
			body $i | grep -q "^WARNING - node started" && body $i | grep -q "after an UNCLEAN" && F=$(subj $i)
		done
		[ -n "$F" ]
	}
	waitfor 15 unclean
	case "$F" in "WARNING: "*|"CRITICAL: "*) ok "B5. back after kill -9: 'after an UNCLEAN stop' mailed as a WARNING entry ($F)";;
		*) bad "B5. no UNCLEAN mail (before the kill: last_seq, synced, snapshot marker, healing, saving $pre; entries: $(for i in $(seq $((n0 + 1)) $(mails)); do body $i | grep -E '^(CRITICAL|WARNING|INFO|RESOLVED) - ' | tr '\n' ','; echo; done | tr '\n' '|'))";; esac
	stop -TERM

	# ---- C. level = critical: a warning is kept, not mailed ---------------------------
	n0=$(mails)
	rm -rf "$D/wal"
	conf "$D/c.conf" "$M" "$M2" "$M3" "$M4" "digest_s = 0" "level = critical"
	start "$MBIN" "$D/c.conf"
	waitfor 10 '[ -n "$(al recent node.start)" ]'
	sleep 2
	case "$(al recent node.start)" in raised/below\ level*) ok "C. level = critical: the (clean) start is kept as 'below level'";;
		*) bad "C. ring: $(al recent node.start)";; esac
	[ "$(mails)" = "$n0" ] && ok "C. and nothing was mailed" || bad "C. $(mails) mails, was $n0: $(subj $(mails))"
	stop -TERM

	# ---- D. S377: level = info, and a text with markup ----------------------------
	n0=$(mails)
	rm -rf "$D/wal"
	conf "$D/d.conf" "$M" "$M2" "$M3" "$M4" "digest_s = 2" "level = info"
	# and a 64-character host name (HOST_NAME_MAX), for the subject's cut
	LH=pc-alert-subject-cut-check-a-host-name-as-long-as-linux-allows-x
	export PERFCACHED_ALERT_TEST_EVENT='<script>x()</script> & "q"' PERFCACHED_ALERT_TEST_HOST=$LH
	start "$MBIN" "$D/d.conf"
	unset PERFCACHED_ALERT_TEST_EVENT PERFCACHED_ALERT_TEST_HOST
	waitfor 12 '[ "$(mails)" -gt "$n0" ]'
	sleep 1
	F=
	for i in $(seq $((n0 + 1)) $(mails)); do
		body $i | grep -q "^INFO - node started" && F=$i
	done
	[ -n "$F" ] && ok "D. level = info: the clean start is mailed, an INFO entry ($(subj $F))" ||
		bad "D. no INFO start (subjects: $(for i in $(seq $((n0 + 1)) $(mails)); do subj $i; done | tr '\n' '|'))"
	G=
	for i in $(seq $((n0 + 1)) $(mails)); do
		mpart $i html | grep -q '&lt;script&gt;x()&lt;/script&gt; &amp; &quot;q&quot;' && G=$i
	done
	[ -n "$G" ] && ! mpart "$G" html | grep -q '<script>' &&
		ok "D. the markup in an alert's text arrives escaped in the HTML part" || bad "D. HTML: $(mpart ${G:-$((n0 + 1))} html | grep -o 'What happened.\{0,200\}' | head -2)"
	SL=$(subj $((n0 + 1)))
	[ ${#SL} -le 78 ] && case "$SL" in *" on pc-alert-subject-cut"*"..."*) true;; *) false;; esac &&
		body $((n0 + 1)) | grep -q "^  Host *${LH%?}" &&
		ok "D. a 64-character host: the subject cut to ${#SL} characters ('$SL'), the Host line has its 63 (the relay frame's cap) on one line" ||
		bad "D. subject '$SL' (${#SL}), Host line '$(body $((n0 + 1)) | grep '^  Host')'"
	stop -TERM

	# ---- E. S377: a problem that gets worse, then clears ----------------------------
	# driven by PERFCACHED_ALERT_TEST_COND (a file: "warning <text>",
	# "critical <text>", or nothing).  The arena cannot stand in: a freed
	# cell stays held until reclaim retires its chunk, so it does not clear
	# on a test's clock (tried: 27% held after every record was deleted)
	n0=$(mails)
	up false                            # no upstream alert in these mails
	rm -rf "$D/wal"
	conf "$D/e.conf" "$M" "$M2" "$M3" "$M4" "digest_s = 1"
	: > "$D/tc"
	export PERFCACHED_ALERT_TEST_COND="$D/tc"
	start "$MBIN" "$D/e.conf"
	unset PERFCACHED_ALERT_TEST_COND
	H=$(hostname)
	echo "warning the test condition went bad" > "$D/tc"
	waitfor 12 '[ -n "$(findmail $n0 "^WARNING: test condition on ")" ]'
	E1=$(findmail $n0 "^WARNING: test condition on ")
	echo "critical the test condition got worse" > "$D/tc"
	waitfor 12 '[ -n "$(findmail $n0 "^CRITICAL: test condition on ")" ]'
	E2=$(findmail $n0 "^CRITICAL: test condition on ")
	: > "$D/tc"
	waitfor 12 '[ -n "$(findmail $n0 "^RESOLVED: test condition on ")" ]'
	E3=$(findmail $n0 "^RESOLVED: test condition on ")
	if [ -n "$E1" ] && [ -n "$E2" ] && [ -n "$E3" ]; then
		ok "E. a condition: a WARNING, a CRITICAL, a RESOLVED mail ($E1, $E2, $E3)"
		I1=$(mpart $E1 msgid)
		body $E2 | grep -q "Status         Problem, still open" && [ "$(mpart $E2 inreplyto)" = "$I1" ] &&
			body $E2 | grep -q "test.cond - first mailed at" &&
			ok "E. the critical mail: 'Problem, still open', answers the warning's mail, says when that went" ||
			bad "E. the critical mail: In-Reply-To '$(mpart $E2 inreplyto)' vs '$I1'; $(body $E2 | grep -E 'Status|ID ' | tr '\n' '|')"
		body $E1 | grep -q "Status         Problem, new" && ok "E. the warning's mail: 'Problem, new'" ||
			bad "E. the warning: $(body $E1 | grep Status)"
		[ "$(mpart $E3 inreplyto)" = "$I1" ] && body $E3 | grep -q "closes the problem mailed at" &&
			body $E3 | grep -q "What was wrong the test condition got worse" &&
			ok "E. the resolve answers the problem's FIRST mail, and says what was wrong at its worst" ||
			bad "E. the resolve: In-Reply-To '$(mpart $E3 inreplyto)' vs the first '$I1'; $(body $E3 | grep -E 'What was|ID ' | tr '\n' '|')"
	else
		bad "E. mails: warning '$E1' critical '$E2' resolved '$E3' (subjects: $(for i in $(seq $((n0 + 1)) $(mails)); do subj $i; done | tr '\n' '|'); active $(al active))"
	fi
	stop -TERM

	# ---- every mail: S377's form ----------------------------------------------------
	python3 - "$D/m" "$(hostname)" > "$D/form.out" 2>&1 <<'PY'
import email, glob, json, re, sys
from email import policy
bad = []
host = sys.argv[2]
files = sorted(glob.glob(sys.argv[1] + "/*.json"), key=lambda p: int(p.rsplit("/", 1)[1].split(".")[0]))
for f in files:
    raw = json.load(open(f))["data"]
    m = email.message_from_string(raw, policy=policy.default)
    n = f.rsplit("/", 1)[1]
    s = m["Subject"] or ""
    kinds = [p.get_content_type() for p in m.walk()]
    if kinds[:1] != ["multipart/alternative"] or "text/plain" not in kinds or "text/html" not in kinds:
        bad.append("%s parts %s" % (n, kinds))
    if not re.match(r"^(CRITICAL|WARNING|RESOLVED|INFO): .+ on .+?( \(\+\d+\))?$", s) or len(s) > 78:
        bad.append("%s subject %r" % (n, s))
    fl = [l for l in raw.split("\r\n") if l.startswith("From: ")]
    if not fl or not fl[0].startswith(('From: "%s" <' % host, 'From: "pc-alert-subject-cut-check-')):
        bad.append("%s from %r" % (n, fl[:1]))
    long = [l for l in raw.split("\r\n") if len(l) > 78 and not l.startswith(("References:", "In-Reply-To:", "Subject:"))]
    if long:
        bad.append("%s a line over 78: %r" % (n, long[0][:100]))
    if "RAISED" in raw:
        bad.append("%s says RAISED" % n)
print("FORM %d mails, %d bad: %s" % (len(files), len(bad), "; ".join(bad[:6])))
PY
	grep -q "^FORM [0-9]* mails, 0 bad" "$D/form.out" && ok "every mail: two parts, the subject's form and length, From the quoted host, no line over 78, no RAISED ($(cut -c1-40 "$D/form.out"))" ||
		bad "S377 form: $(cat "$D/form.out")"
fi

echo "alerttest: $pass passed, $fail failed, $skip skipped"
[ $fail -eq 0 ]
