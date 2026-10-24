#!/bin/sh
# functest.sh - func_perfd inside a live Asterisk, from the dialplan only.
#
#   contrib/asterisk/test/functest.sh <func_perfd.so> [phase ...]
#
# Phases (default: all, in this order):
#   basic-binary basic-resp sizes refuse failover-binary failover-resp
#   stall-binary stall-resp pool reload redis swap buildopts
#
# Environment:
#   ASTERISK_PREFIX  installed Asterisk the module was built for
#                    (default /opt/asterisk-22.10.1-flh-fix3); only its binary
#                    and modules are used, never its configuration
#   ASTERISK_DOC     documentation XML (default: the installed build's; Asterisk's
#                    core will not start without one).  The patched tree's
#                    doc/core-en_US.xml adds the PERFD_* documentation check.
#   PERFCACHED PERFCLI  daemon and CLI (default ./perfcached ./perfcli)
#   REDIS_SERVER     default redis-server
#   ASTERISK_ENV     VAR=value words for the Asterisk server process only,
#                    e.g. a sanitizer runtime in LD_PRELOAD and its options
#
# LOCAL ONLY: the perfcached fleet listens on 127.0.9.x, every Redis is
# started here on 127.0.0.1 with no persistence, Asterisk opens no network
# listener, and every process is tracked by pid and killed on exit.  A RESP
# server line that is not loopback is refused before it is written.
set -u

MOD=${1:?usage: functest.sh <func_perfd.so> [phase ...]}
shift
PHASES=${*:-basic-binary basic-resp sizes refuse failover-binary failover-resp stall-binary stall-resp pool reload redis swap buildopts}
AST=${ASTERISK_PREFIX:-/opt/asterisk-22.10.1-flh-fix3}
PC=${PERFCACHED:-./perfcached}
PCLI=${PERFCLI:-./perfcli}
REDIS=${REDIS_SERVER:-redis-server}
GREP=/bin/grep
case "$MOD" in /*) ;; *) MOD=$(pwd)/$MOD ;; esac
case "$PC" in /*) ;; *) PC=$(pwd)/$PC ;; esac
case "$PCLI" in /*) ;; *) PCLI=$(pwd)/$PCLI ;; esac
[ -f "$MOD" ] || { echo "no module $MOD"; exit 2; }
[ -x "$AST/sbin/asterisk" ] || { echo "no Asterisk at $AST"; exit 2; }

D=$(mktemp -d /var/tmp/functest.XXXXXX)
N1= N2= N3= RA= RB= ASTPID=
pass=0 fail=0

cleanup() {
	if [ -n "$ASTPID" ]; then
		timeout 10 "$AST/sbin/asterisk" -C "$D/ast/etc/asterisk.conf" -rx "core stop now" > /dev/null 2>&1
		i=0
		while kill -0 "$ASTPID" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
		kill -9 "$ASTPID" 2>/dev/null
	fi
	for p in $N1 $N2 $N3 $RA $RB; do
		kill -CONT "$p" 2>/dev/null
		kill -9 "$p" 2>/dev/null
	done
	if [ "${KEEP:-0}" = 1 ]; then echo "kept $D"; else rm -rf "$D"; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

ok()  { pass=$((pass+1)); echo "ok   $*"; }
bad() { fail=$((fail+1)); echo "FAIL $*"; }
check() { # check <name> <got> <want>
	if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: got '$2', want '$3'"; fi
}

busy() { ss -Hltn "sport = :$1" | $GREP -q . ; }
for p in 17611 17612 17613 17621 17622 17623; do
	busy $p && { echo "port $p is in use"; exit 2; }
done
NP1=17611 NP2=17612 NP3=17613
RP1=17621 RP2=17622 RP3=17623
SECRET=ft-client-secret
RESPPW=ft-resp-password

# ---- perfcached fleet ------------------------------------------------------
node_conf() { # node_conf <i>
	eval np=\$NP$1 rp=\$RP$1
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = $SECRET
cluster = ft-cluster-secret
resp = $RESPPW
[listen]
tcp = 127.0.9.$1:$np
resp = 127.0.9.$1:$rp
[cluster]
multicast = 239.255.77.79:17631
advertise = 127.0.9.$1
pull_timeout_ms = 400
mode = eager
collections = c
EOF
}
node_start() { # node_start <i>
	: > "$D/n$1.log"
	"$PC" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	eval N$1=\$!
	i=0
	while [ $i -lt 100 ]; do
		$GREP -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}
node_kill() { # node_kill <i>
	eval p=\$N$1
	[ -n "$p" ] && kill -9 "$p" 2>/dev/null
	eval N$1=
}
pcli() { # pcli <node> <words...>
	nd=$1; shift
	eval np=\$NP$nd
	timeout 10 "$PCLI" -h "127.0.9.$nd" -p "$np" -a "$SECRET" -q "$@" 2>&1
}
jfield() { # jfield <field> - one field of perfcli's JSON reply on stdin
	python3 -c 'import json, sys
try:
    v = json.load(sys.stdin)[sys.argv[1]]
except Exception as e:
    v = "unparsable: %s" % e
print(str(v).lower() if isinstance(v, bool) else v)' "$1"
}

# ---- local Redis -----------------------------------------------------------
redis_start() { # redis_start <port> <var> [args...]
	rport=$1 rvar=$2
	shift 2
	busy "$rport" && { echo "port $rport is in use"; exit 2; }
	"$REDIS" --port "$rport" --bind 127.0.0.1 --save '' --appendonly no --dir "$D" \
		--dbfilename "dump$rport.rdb" --daemonize no --requirepass ft-redis-pw \
		--masterauth ft-redis-pw "$@" > "$D/redis$rport.log" 2>&1 &
	eval $rvar=\$!
	i=0
	while [ $i -lt 50 ]; do
		redis-cli -p "$rport" -a ft-redis-pw --no-auth-warning PING 2>/dev/null | $GREP -q PONG && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "redis on $rport did not start"; exit 1
}
rcli() { rp=$1; shift; timeout 10 redis-cli -p "$rp" -a ft-redis-pw --no-auth-warning "$@" 2>/dev/null; }

# a RESP SET with arbitrary bytes: resp_put <host> <port> <password> <db> <key> <spec>
# spec: rep:<char>:<count> or nul
cat > "$D/respput.py" <<'EOF'
import socket, sys
host, port, pw, db, key, spec = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6]
if spec == "nul":
    val = b"ab\x00cd"
else:
    _, ch, n = spec.split(":")
    val = ch.encode() * int(n)
def cmd(*args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        a = a if isinstance(a, bytes) else a.encode()
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out
s = socket.create_connection((host, port), timeout=5)
s.sendall(cmd("AUTH", pw) + cmd("SELECT", db) + cmd("SET", key, val))
buf = b""
while buf.count(b"\r\n") < 3:
    buf += s.recv(4096)
print(buf.decode(errors="replace").replace("\r\n", " ").strip())
EOF
resp_put() { timeout 10 python3 "$D/respput.py" "$@"; }
# the RESP value of a key as "<len>:<md5>" (or NIL): resp_sum <host> <port> <password> <db> <key>
cat > "$D/respsum.py" <<'EOF'
import hashlib, socket, sys
host, port, pw, db, key = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
def cmd(*args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        a = a.encode()
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out
s = socket.create_connection((host, port), timeout=5)
s.sendall(cmd("AUTH", pw) + cmd("SELECT", db) + cmd("GET", key))
buf = b""
def line():
    global buf
    while b"\r\n" not in buf:
        buf += s.recv(65536)
    l, buf = buf.split(b"\r\n", 1)
    return l
line(); line()
h = line()
if h == b"$-1":
    print("NIL")
else:
    n = int(h[1:])
    while len(buf) < n + 2:
        buf += s.recv(65536)
    print("%d:%s" % (n, hashlib.md5(buf[:n]).hexdigest()))
EOF
resp_sum() { timeout 10 python3 "$D/respsum.py" "$@"; }
want_sum() { python3 -c 'import hashlib,sys; ch,n=sys.argv[1],int(sys.argv[2]); print("%d:%s" % (n, hashlib.md5((ch*n).encode()).hexdigest()))' "$1" "$2"; }

# ---- the Asterisk instance -------------------------------------------------
A=$D/ast
mkdir -p "$A/etc" "$A/modules" "$A/varlib/documentation" "$A/run" "$A/log" "$A/spool" "$A/agi" "$A/cache"
for m in pbx_config func_global func_strings func_logic res_clioriginate; do
	ln -s "$AST/lib/asterisk/modules/$m.so" "$A/modules/$m.so"
done
cp "$MOD" "$A/modules/func_perfd.so"
ASTERISK_DOC=${ASTERISK_DOC:-$AST/var/lib/asterisk/documentation/core-en_US.xml}
cp "$ASTERISK_DOC" "$A/varlib/documentation/core-en_US.xml" || { echo "no documentation XML at $ASTERISK_DOC"; exit 2; }
cat > "$A/etc/asterisk.conf" <<EOF
[directories]
astcachedir => $A/cache
astetcdir => $A/etc
astmoddir => $A/modules
astvarlibdir => $A/varlib
astdbdir => $A/varlib
astkeydir => $A/varlib
astdatadir => $A/varlib
astagidir => $A/agi
astspooldir => $A/spool
astrundir => $A/run
astlogdir => $A/log
astsbindir => $AST/sbin
[options]
verbose = 0
debug = 0
EOF
cat > "$A/etc/modules.conf" <<'EOF'
[modules]
autoload = no
load => pbx_config.so
load => func_global.so
load => func_strings.so
load => func_logic.so
load => res_clioriginate.so
EOF
cat > "$A/etc/logger.conf" <<'EOF'
[general]
[logfiles]
messages.log => notice,warning,error
EOF

# Every test is an extension in [tests]; it writes GLOBAL(R_*) results and
# GLOBAL(D_<exten>)=1 last.  ${COL} is the collection, ${PFX} a key prefix.
cat > "$A/etc/extensions.conf" <<'EOF'
[tests]
exten => basic,1,Set(PERFD_SET(${COL},${PFX}k1)=hello world)
 same => n,Set(GLOBAL(R_set)=${PERFDSTATUS})
 same => n,Set(v=${PERFD_GET(${COL},${PFX}k1)})
 same => n,Set(GLOBAL(R_get)=${PERFDSTATUS}|${v})
 same => n,Set(v=${PERFD_GET(${COL},${PFX}nokey)})
 same => n,Set(GLOBAL(R_miss)=${PERFDSTATUS}|${v})
 same => n,Set(PERFD_SET(${COL},${PFX}empty)=)
 same => n,Set(v=${PERFD_GET(${COL},${PFX}empty)})
 same => n,Set(GLOBAL(R_empty)=${PERFDSTATUS}|${v})
 same => n,Set(v=${PERFD_EXISTS(${COL},${PFX}k1)})
 same => n,Set(GLOBAL(R_exists)=${PERFDSTATUS}|${v})
 same => n,Set(v=${PERFD_EXISTS(${COL},${PFX}nokey)})
 same => n,Set(GLOBAL(R_exists_no)=${PERFDSTATUS}|${v})
 same => n,Set(v=${PERFD_DELETE(${COL},${PFX}k1)})
 same => n,Set(GLOBAL(R_del1)=${PERFDSTATUS}|${v})
 same => n,Set(v=${PERFD_DELETE(${COL},${PFX}k1)})
 same => n,Set(GLOBAL(R_del2)=${PERFDSTATUS}|${v})
 same => n,Set(v=${PERFD_GET(${COL},${PFX}k1)})
 same => n,Set(GLOBAL(R_after_del)=${PERFDSTATUS}|${v})
 same => n,Set(PERFD_SET(${COL},${PFX}k2)=x)
 same => n,Set(PERFD_DELETE(${COL},${PFX}k2)=)
 same => n,Set(GLOBAL(R_wdel)=${PERFDSTATUS}|${PERFD_EXISTS(${COL},${PFX}k2)})
 same => n,Set(PERFD_SET(${COL},${PFX}kttl,3600)=t)
 same => n,Set(GLOBAL(R_ttlset)=${PERFDSTATUS})
 same => n,Set(PERFD_SET(${COL},${PFX}knottl)=t)
 same => n,Set(PERFD_SET(${COL},${PFX}kbadttl,abc)=t)
 same => n,Set(GLOBAL(R_badttl)=${PERFDSTATUS})
 same => n,Set(PERFD_SET(,${PFX}kdefcol)=d)
 same => n,Set(GLOBAL(R_defcol)=${PERFDSTATUS}|${PERFD_GET(,${PFX}kdefcol)})
 same => n,Set(v=${PERFD_GET(${COL},${PFX}k1,extra)})
 same => n,Set(GLOBAL(R_extra)=${PERFDSTATUS})
 same => n,Set(v=${PERFD_GET(${COL},)})
 same => n,Set(GLOBAL(R_nokey)=${PERFDSTATUS})
 same => n,Set(GLOBAL(D_basic)=1)
 same => n,Hangup()

exten => refuse,1,Set(PERFD_SET(${COL},${PFX}refused)=before)
 same => n,Set(v=${PERFD_SET(${COL},${PFX}refused)})
 same => n,Set(PERFD_GET(${COL},${PFX}refused)=after)
 same => n,Set(GLOBAL(R_refuse)=${PERFD_GET(${COL},${PFX}refused)})
 same => n,Set(GLOBAL(D_refuse)=1)
 same => n,Hangup()

exten => sizes,1,Set(v=${PERFD_GET(${COL},${PFX}v4095)})
 same => n,Set(GLOBAL(R_v4095)=${PERFDSTATUS})
 same => n,Set(PERFD_SET(${COL},${PFX}v4095copy)=${v})
 same => n,Set(GLOBAL(R_v4095copy)=${PERFDSTATUS})
 same => n,Set(v=${PERFD_GET(${COL},${PFX}v4096)})
 same => n,Set(GLOBAL(R_v4096)=${PERFDSTATUS}|${LEN(${v})})
 same => n,Set(v=${PERFD_GET(${COL},${PFX}v4300)})
 same => n,Set(GLOBAL(R_v4300)=${PERFDSTATUS}|${LEN(${v})})
 same => n,Set(v=${PERFD_GET(${COL},${PFX}vnul)})
 same => n,Set(GLOBAL(R_vnul)=${PERFDSTATUS}|${LEN(${v})})
 same => n,Set(v2000=${PERFD_GET(${COL},${PFX}v2000)})
 same => n,Set(PERFD_SET(${COL},${PFX}v4000set)=${v2000}${v2000})
 same => n,Set(GLOBAL(R_set4000)=${PERFDSTATUS})
 same => n,Set(PERFD_SET(${COL},${PFX}v4001set)=${v2000}${v2000}x)
 same => n,Set(GLOBAL(R_set4001)=${PERFDSTATUS})
 same => n,Set(GLOBAL(D_sizes)=1)
 same => n,Hangup()

exten => loop,1,Set(i=0)
 same => n,Set(okc=0)
 same => n,Set(errc=0)
 same => n(top),Set(PERFD_SET(${COL},${PFX}loop${i})=v${i})
 same => n,GotoIf($["${PERFDSTATUS}" = "OK"]?good)
 same => n,Set(errc=$[${errc} + 1])
 same => n,Goto(next)
 same => n(good),Set(okc=$[${okc} + 1])
 same => n(next),Set(i=$[${i} + 1])
 same => n,Wait(${LOOPWAIT})
 same => n,GotoIf($[${i} < ${LOOPN}]?top)
 same => n,Set(GLOBAL(R_loop)=${okc}|${errc})
 same => n,Set(GLOBAL(D_loop)=1)
 same => n,Hangup()

exten => timed,1,Set(t0=${STRFTIME(,,%s%3q)})
 same => n,Set(v=${PERFD_GET(${COL},${PFX}timed)})
 same => n,Set(t1=${STRFTIME(,,%s%3q)})
 same => n,Set(GLOBAL(R_timed)=${PERFDSTATUS}|$[${t1} - ${t0}])
 same => n,Set(GLOBAL(D_timed)=1)
 same => n,Hangup()

exten => one,1,Set(t0=${STRFTIME(,,%s%3q)})
 same => n,Set(v=${PERFD_GET(${COL},${PFX}one)})
 same => n,Set(t1=${STRFTIME(,,%s%3q)})
 same => n,Set(GLOBAL(R_one_${PFX}${UNIQUEID})=${PERFDSTATUS}|$[${t1} - ${t0}])
 same => n,Hangup()
EOF

ax() { timeout 20 "$AST/sbin/asterisk" -C "$A/etc/asterisk.conf" -rx "$*" 2>&1; }
glob() { # the value of global <name>
	ax "dialplan show globals" | awk -v k="$1" '
		{ sub(/^[ \t]+/, "") }
		index($0, k "=") == 1 { print substr($0, length(k) + 2); exit }'
}
setg() { ax "dialplan set global $1 $2" > /dev/null; }
run_ext() { # run_ext <exten> [wait_tenths]
	setg "D_$1" 0
	ax "channel originate Local/$1@tests application Wait 120" > /dev/null
	lim=${2:-100} i=0
	while [ $i -lt "$lim" ]; do
		[ "$(glob "D_$1")" = 1 ] && return 0
		sleep 0.1; i=$((i+1))
	done
	bad "extension $1 did not finish"
	return 1
}
counter() { ax "perfd show status" | awk -v k="$1" '$1 == k { print $2 }'; }
logmark() { wc -l < "$A/log/messages.log" 2>/dev/null || echo 0; }
logsince() { tail -n +"$(( $1 + 1 ))" "$A/log/messages.log"; }

conf() { # conf <lines...> - write perfd.conf, refusing non-loopback RESP servers
	printf '[general]\n' > "$A/etc/perfd.conf"
	for l in "$@"; do
		case "$l" in
		server\ =\ 127.*|server\ =\ unix:*|server\ =\ 192.0.2.*) ;;
		server*) echo "functest: refusing non-loopback '$l'"; exit 2 ;;
		esac
		printf '%s\n' "$l" >> "$A/etc/perfd.conf"
	done
}
modload() { ax "module unload func_perfd.so" > /dev/null; ax "module load func_perfd.so"; }
loaded() { ax "module show like func_perfd" | $GREP "^func_perfd.so" | $GREP -vc "Not Running"; }
binconf() { conf "protocol = binary" "server = 127.0.9.1:$NP1" "secret = $SECRET" "collection = c" "$@"; }
respconf() { conf "protocol = resp" "server = 127.0.9.1:$RP1" "password = $RESPPW" "collection = c" "$@"; }

node_conf 1; node_conf 2; node_conf 3
node_start 1; node_start 2; node_start 3
sleep 4

env ${ASTERISK_ENV:-} "$AST/sbin/asterisk" -C "$A/etc/asterisk.conf" -f -n > "$A/console.out" 2>&1 < /dev/null &
ASTPID=$!
i=0
until ax "core waitfullybooted" | $GREP -q "fully booted"; do
	i=$((i+1)); [ $i -lt 100 ] || { echo "Asterisk did not boot"; cat "$A/console.out"; exit 1; }
	sleep 0.2
done
echo "Asterisk up (pid $ASTPID), fleet up ($N1 $N2 $N3)"

# ---- phases ----------------------------------------------------------------
basic() { # basic <label>
	lbl=$1
	setg COL c; setg PFX "$lbl-"
	run_ext basic || return
	check "$lbl: set" "$(glob R_set)" "OK"
	check "$lbl: get hit" "$(glob R_get)" "OK|hello world"
	check "$lbl: get miss" "$(glob R_miss)" "NOTFOUND|"
	check "$lbl: stored empty value is OK, not NOTFOUND" "$(glob R_empty)" "OK|"
	check "$lbl: exists" "$(glob R_exists)" "OK|1"
	check "$lbl: exists absent" "$(glob R_exists_no)" "NOTFOUND|0"
	check "$lbl: delete removes" "$(glob R_del1)" "OK|1"
	check "$lbl: delete again" "$(glob R_del2)" "NOTFOUND|0"
	check "$lbl: get after delete" "$(glob R_after_del)" "NOTFOUND|"
	check "$lbl: write-form delete" "$(glob R_wdel)" "OK|0"
	check "$lbl: set with ttl" "$(glob R_ttlset)" "OK"
	check "$lbl: bad ttl refused" "$(glob R_badttl)" "ERROR"
	check "$lbl: empty collection uses perfd.conf's" "$(glob R_defcol)" "OK|d"
	check "$lbl: a third argument to GET is refused" "$(glob R_extra)" "ERROR"
	check "$lbl: an empty key is refused" "$(glob R_nokey)" "ERROR"
	ttl=$(pcli 2 ttl c "$lbl-kttl" | jfield ttl)
	if [ "$ttl" -ge 3590 ] 2>/dev/null && [ "$ttl" -le 3600 ]; then ok "$lbl: ttl 3600 reached the daemon ($ttl)"; else bad "$lbl: ttl on the daemon: '$ttl'"; fi
	check "$lbl: no ttl means no expiry" "$(pcli 2 ttl c "$lbl-knottl" | jfield ttl)" "-1"
	check "$lbl: the empty value is stored as found on another node" "$(pcli 3 get c "$lbl-empty" | jfield found)|$(pcli 3 get c "$lbl-empty" | jfield value)" "true|"
}

phase_basic_binary() {
	binconf; modload > /dev/null
	check "binary: module loaded" "$(loaded)" 1
	basic binary
	if $GREP -q 'function name="PERFD_GET"' "$ASTERISK_DOC"; then
		ax "core show function PERFD_GET" | $GREP -q "PERFDSTATUS" && ok "docs: core show function PERFD_GET" || bad "docs: core show function PERFD_GET"
	fi
}
phase_basic_resp() {
	respconf; modload > /dev/null
	check "resp: module loaded" "$(loaded)" 1
	basic resp
}

phase_refuse() {
	binconf; modload > /dev/null
	setg COL c; setg PFX refuse-
	m=$(logmark)
	run_ext refuse || return
	logsince "$m" | $GREP -q "Function PERFD_SET cannot be read" && ok "reading PERFD_SET is refused by Asterisk" || bad "reading PERFD_SET"
	logsince "$m" | $GREP -q "Function PERFD_GET cannot be written to" && ok "writing PERFD_GET is refused by Asterisk" || bad "writing PERFD_GET"
	check "the refused write changed nothing" "$(glob R_refuse)" "before"

	refused() { # refused <name> <expected log text> <conf lines...>
		rname=$1 rtext=$2
		shift 2
		conf "$@"
		ax "module unload func_perfd.so" > /dev/null
		m=$(logmark)
		ax "module load func_perfd.so" > /dev/null
		if [ "$(loaded)" = 0 ] && logsince "$m" | $GREP -qF "$rtext"; then
			ok "refused: $rname"
		else
			bad "refused: $rname (loaded=$(loaded))"; logsince "$m" | tail -3
		fi
	}
	refused "no server" "no server is configured" "protocol = binary"
	refused "unknown protocol" "protocol 'memcache' is neither binary nor resp" "protocol = memcache" "server = 127.0.9.1:$NP1"
	refused "secret under resp" "secret is for protocol = binary" "protocol = resp" "server = 127.0.9.1:$RP1" "secret = x"
	refused "password under binary" "password and username are for protocol = resp" "server = 127.0.9.1:$NP1" "secret = $SECRET" "password = x"
	refused "username without password" "username is set without password" "protocol = resp" "server = 127.0.9.1:$RP1" "username = u"
	refused "non-loopback binary without secret" "server 192.0.2.1:6479 is not loopback and no secret is set" "server = 192.0.2.1:6479"
	refused "pool 0" "pool '0' is not 1..64" "server = 127.0.9.1:$NP1" "secret = $SECRET" "pool = 0"
	refused "io_timeout_ms 0" "io_timeout_ms '0' is not 1..60000" "server = 127.0.9.1:$NP1" "secret = $SECRET" "io_timeout_ms = 0"
	refused "unknown setting" "unknown setting 'io_timeout'" "server = 127.0.9.1:$NP1" "secret = $SECRET" "io_timeout = 5"
	refused "bad server" "server '127.0.9.1' is not host:port" "server = 127.0.9.1" "secret = $SECRET"
	rm -f "$A/etc/perfd.conf"
	ax "module unload func_perfd.so" > /dev/null
	m=$(logmark)
	ax "module load func_perfd.so" > /dev/null
	[ "$(loaded)" = 0 ] && logsince "$m" | $GREP -qF "perfd.conf is missing" && ok "refused: missing perfd.conf" || bad "refused: missing perfd.conf"

	# a bad file on reload keeps the running configuration
	binconf; modload > /dev/null
	conf "protocol = binary" "server = 127.0.9.1:$NP1" "secret = $SECRET" "pool = 99"
	sleep 1; touch "$A/etc/perfd.conf"
	m=$(logmark)
	ax "module reload func_perfd.so" > /dev/null
	logsince "$m" | $GREP -qF "refused; the running configuration stays" && ok "a refused reload says the running configuration stays" || bad "refused reload message"
	setg PFX refuse2-
	run_ext basic && check "calls still work after a refused reload" "$(glob R_get)" "OK|hello world"
}

phase_sizes() {
	binconf "max_value = 4200"; modload > /dev/null
	for spec in "v4095 rep:a:4095" "v4096 rep:b:4096" "v4300 rep:c:4300" "vnul nul" "v2000 rep:d:2000"; do
		set -- $spec
		resp_put 127.0.9.1 $RP1 "$RESPPW" c "sizes-$1" "$2" > /dev/null
	done
	setg COL c; setg PFX sizes-
	m=$(logmark)
	run_ext sizes || return
	check "4095 bytes fit the dialplan buffer" "$(glob R_v4095)" "OK"
	check "... and round-trip byte for byte" "$(resp_sum 127.0.9.2 $RP2 "$RESPPW" c sizes-v4095copy)" "$(want_sum a 4095)"
	check "4096 bytes do not fit: ERROR, nothing returned" "$(glob R_v4096)" "ERROR|0"
	logsince "$m" | $GREP -q "value of 4096 bytes does not fit this caller's 4096-byte buffer" && ok "... and the log says why" || bad "4096 log line"
	check "4300 bytes over max_value 4200: ERROR" "$(glob R_v4300)" "ERROR|0"
	logsince "$m" | $GREP -q "value of 4300 bytes is longer than max_value (4200)" && ok "... and the log says max_value" || bad "4300 log line"
	check "a value with a NUL byte: ERROR" "$(glob R_vnul)" "ERROR|0"
	logsince "$m" | $GREP -q "contains a NUL byte" && ok "... and the log says NUL" || bad "NUL log line"
	check "SET of 4000 bytes" "$(glob R_set4000)" "OK"
	check "... stored byte for byte" "$(resp_sum 127.0.9.3 $RP3 "$RESPPW" c sizes-v4000set)" "$(want_sum d 4000)"
	check "SET of 4001 bytes under max_value 4200" "$(glob R_set4001)" "OK"
	binconf "max_value = 4000"; modload > /dev/null
	resp_put 127.0.9.1 $RP1 "$RESPPW" c sizes2-v2000 rep:d:2000 > /dev/null
	setg PFX sizes2-
	run_ext sizes || return
	check "SET of 4000 bytes at max_value 4000" "$(glob R_set4000)" "OK"
	check "SET of 4001 bytes over max_value 4000: ERROR" "$(glob R_set4001)" "ERROR"
	check "... and nothing was stored" "$(resp_sum 127.0.9.1 $RP1 "$RESPPW" c sizes2-v4001set)" "NIL"
}

loop_run() { # loop_run <label> <n> <wait> <action> - runs the loop, calls <action> ~1 s in
	setg LOOPN "$2"; setg LOOPWAIT "$3"; setg PFX "$1-"
	setg D_loop 0
	ax "channel originate Local/loop@tests application Wait 120" > /dev/null
	sleep 1
	$4
	i=0
	while [ $i -lt 300 ]; do
		[ "$(glob D_loop)" = 1 ] && return 0
		sleep 0.1; i=$((i+1))
	done
	bad "$1: loop did not finish"
	return 1
}

restart_node1() {
	[ -z "$N1" ] || return 0
	kill -CONT "$N1" 2>/dev/null
	node_start 1
	sleep 4
}

kill_node1() { node_kill 1; }
phase_failover_binary() {
	restart_node1
	binconf "pool = 2"; modload > /dev/null
	setg COL c
	loop_run fob 60 0.05 kill_node1 || return
	check "binary: node killed mid-loop, 60 of 60 OK" "$(glob R_loop)" "60|0"
	f=$(counter failovers)
	[ "${f:-0}" -ge 1 ] && ok "binary: perfd show status counts $f failover(s)" || bad "binary: failovers '$f'"
	restart_node1
}
phase_failover_resp() {
	restart_node1
	respconf "server = 127.0.9.2:$RP2" "pool = 2"; modload > /dev/null
	setg COL c
	loop_run for 60 0.05 kill_node1 || return
	check "resp: node killed mid-loop, 60 of 60 OK" "$(glob R_loop)" "60|0"
	r=$(counter reconnects)
	[ "${r:-0}" -ge 1 ] && ok "resp: perfd show status counts $r reconnect(s)" || bad "resp: reconnects '$r'"
	restart_node1
}

stall() { # stall <label>
	setg COL c; setg PFX "$1-"
	pcli 2 set c "$1-timed" stored > /dev/null
	sleep 0.5
	run_ext timed 60
	check "$1: the key is there before the stall" "$(glob R_timed | cut -d'|' -f1)" "OK"
	kill -STOP "$N1"
	run_ext timed 60
	kill -CONT "$N1"
	res=$(glob R_timed)
	st=${res%%|*} ms=${res#*|}
	# wait_timeout 300 + 2 x io 300 + connect 1000
	if [ "$st" = "OK" ] && [ "$ms" -lt 1900 ] 2>/dev/null; then
		ok "$1: node stopped, the call answered $st in $ms ms (bound 1900)"
	else
		bad "$1: node stopped: '$res'"
	fi
}
phase_stall_binary() {
	restart_node1
	binconf "pool = 1"; modload > /dev/null
	stall stall-binary
}
phase_stall_resp() {
	restart_node1
	respconf "server = 127.0.9.2:$RP2" "pool = 1"; modload > /dev/null
	stall stall-resp
}

onecount() { ax "dialplan show globals" | $GREP -c "^ *R_one_$1"; }
concurrent() { # concurrent <n> <wait_tenths> <pfx> - originate n calls of 'one'
	setg PFX "$3"
	j=0 cpids=
	while [ $j -lt "$1" ]; do
		ax "channel originate Local/one@tests application Wait 60" > /dev/null &
		cpids="$cpids $!"
		j=$((j+1))
	done
	wait $cpids
	i=0
	while [ $i -lt "$2" ]; do
		[ "$(onecount "$3")" -ge "$1" ] && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}
phase_pool() {
	restart_node1
	binconf "pool = 1" "spares = none" "wait_timeout_ms = 300"; modload > /dev/null
	setg COL c
	if concurrent 50 300 pool-; then
		n=$(ax "dialplan show globals" | $GREP -c "^ *R_one_pool-.*=NOTFOUND|")
		check "pool of 1: 50 concurrent calls all answered" "$n" "50"
	else
		bad "pool of 1: 50 concurrent calls did not all finish ($(onecount pool-))"
	fi
	check "pool of 1: no borrow waited past wait_timeout_ms" "$(counter wait_timeouts)" "0"

	# the one connection stalls: every other call must give up at wait_timeout_ms
	modload > /dev/null
	kill -STOP "$N1"
	t0=$(date +%s%N)
	concurrent 20 100 pool2-
	fin=$?
	t1=$(date +%s%N)
	kill -CONT "$N1"
	ms=$(( (t1 - t0) / 1000000 ))
	w=$(counter wait_timeouts)
	if [ "$fin" -eq 0 ] && [ "${w:-0}" -ge 1 ] && [ "$ms" -lt 8000 ]; then
		ok "pool of 1, its server stopped: 20 calls finished in $ms ms, $w gave up waiting"
	else
		bad "pool of 1, server stopped: finished=$fin in $ms ms, wait_timeouts '$w'"
	fi
}

sockets() { ss -Htnp state established "( dport >= :17611 and dport <= :17623 )" | $GREP -c "pid=$ASTPID,"; }
reload_loop_action() {
	binconf "pool = 3" "spares = none"
	sleep 1; touch "$A/etc/perfd.conf"
	ax "module reload func_perfd.so" > /dev/null
}
phase_reload() {
	restart_node1
	binconf "pool = 2" "spares = none"; modload > /dev/null
	check "pool 2 holds 2 sockets" "$(sockets)" "2"
	setg COL c
	loop_run reload 60 0.05 reload_loop_action || return
	check "reload mid-loop: 60 of 60 OK" "$(glob R_loop)" "60|0"
	check "after reload: pool 3 holds 3 sockets, the old pool's are gone" "$(sockets)" "3"

	# a call held inside the module (its server stopped, io_timeout 2 s) while unloading
	binconf "pool = 1" "spares = none" "io_timeout_ms = 2000"; modload > /dev/null
	setg COL c; setg PFX unload-; setg D_timed 0
	kill -STOP "$N1"
	ax "channel originate Local/timed@tests application Wait 60" > /dev/null
	sleep 0.5
	ax "module unload func_perfd.so" > "$D/unload.out"
	kill -CONT "$N1"
	i=0
	while [ "$(glob D_timed)" != 1 ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
	kill -0 "$ASTPID" 2>/dev/null && ok "Asterisk survived an unload with a call in flight" || bad "Asterisk died"
	$GREP -q "Unable to unload resource func_perfd.so" "$D/unload.out" && ok "the unload was refused: $(head -1 "$D/unload.out")" || bad "unload in flight: $(cat "$D/unload.out")"
	check "the call in flight finished" "$(glob D_timed)" "1"
	check "the module is still loaded" "$(loaded)" 1
	ax "module unload func_perfd.so" > /dev/null
	check "after a quiet unload: unloaded" "$(loaded)" 0
	check "after a quiet unload: no sockets" "$(sockets)" "0"
}

phase_redis() {
	REDA=17641
	redis_start $REDA RA
	conf "protocol = resp" "server = 127.0.0.1:$REDA" "password = ft-redis-pw" "collection = 0"
	modload > /dev/null
	setg COL 0; setg PFX redis-
	run_ext basic || return
	check "redis: set" "$(glob R_set)" "OK"
	check "redis: get hit" "$(glob R_get)" "OK|hello world"
	check "redis: get miss" "$(glob R_miss)" "NOTFOUND|"
	check "redis: empty value" "$(glob R_empty)" "OK|"
	check "redis: exists" "$(glob R_exists)" "OK|1"
	check "redis: delete" "$(glob R_del1)" "OK|1"
	check "redis: delete again" "$(glob R_del2)" "NOTFOUND|0"
	check "redis: write-form delete" "$(glob R_wdel)" "OK|0"
	check "redis: default collection" "$(glob R_defcol)" "OK|d"
	ttl=$(rcli $REDA -n 0 TTL redis-kttl)
	if [ "$ttl" -ge 3590 ] 2>/dev/null; then ok "redis: TTL $ttl"; else bad "redis: TTL '$ttl'"; fi
	check "redis: no ttl" "$(rcli $REDA -n 0 TTL redis-knottl)" "-1"
	check "redis: key in db 0, not db 1" "$(rcli $REDA -n 0 EXISTS redis-knottl)|$(rcli $REDA -n 1 EXISTS redis-knottl)" "1|0"

	m=$(logmark)
	conf "protocol = resp" "server = 127.0.0.1:$REDA" "password = ft-redis-pw" "collection = calls"
	modload > /dev/null
	logsince "$m" | $GREP -q "SELECT calls refused: ERR invalid DB index" && ok "redis: a named collection is a load-time WARN" || bad "redis: named collection warn"
	check "redis: ... the module still loads" "$(loaded)" 1
	setg COL calls; setg PFX redis2-
	run_ext basic && check "redis: ... and a call with it is ERROR" "$(glob R_set)" "ERROR"

	m=$(logmark)
	conf "protocol = resp" "server = 127.0.0.1:$REDA" "password = wrong-password" "collection = 0"
	modload > /dev/null
	setg COL 0
	run_ext basic && check "redis: wrong password is ERROR" "$(glob R_set)" "ERROR"
	logsince "$m" | $GREP -q "AUTH refused" && ok "redis: ... logged as AUTH refused" || bad "redis: auth log"

	m=$(logmark)
	conf "protocol = resp" "server = 127.0.0.1:$REDA" "username = someone" "password = ft-redis-pw" "collection = 0"
	modload > /dev/null
	run_ext basic && check "redis: username against Redis 5 is ERROR" "$(glob R_set)" "ERROR"
	logsince "$m" | $GREP -q "wrong number of arguments" && ok "redis: ... with the server's text" || bad "redis: username log"
	kill -9 "$RA"; RA=
}

swap_action() {
	rcli $REDB REPLICAOF NO ONE > /dev/null
	rcli $REDA REPLICAOF 127.0.0.1 $REDB > /dev/null
}
phase_swap() {
	REDA=17651 REDB=17652
	redis_start $REDA RA
	redis_start $REDB RB --replicaof 127.0.0.1 $REDA
	conf "protocol = resp" "server = 127.0.0.1:$REDA" "server = 127.0.0.1:$REDB" "password = ft-redis-pw" "collection = 0" "pool = 2"
	modload > /dev/null
	setg COL 0
	loop_run swap 60 0.05 swap_action || return
	check "redis roles swapped mid-loop: 60 of 60 OK" "$(glob R_loop)" "60|0"
	r=$(counter reconnects)
	[ "${r:-0}" -ge 1 ] && ok "redis swap: $r reconnect(s) counted" || bad "redis swap: reconnects '$r'"
	check "redis swap: the last write is on the new master" "$(rcli $REDB -n 0 GET swap-loop59)" "v59"
	kill -9 "$RA" "$RB"; RA= RB=
}

phase_buildopts() {
	sum=$(strings "$AST/sbin/asterisk" | $GREP -xE '[0-9a-f]{32}' | head -1)
	if [ -n "$sum" ] && strings "$MOD" | $GREP -qx "$sum"; then
		ok "the module carries the installed binary's build-options sum $sum"
	else
		bad "build-options sum: binary '$sum' not found in the module"
	fi
}

for ph in $PHASES; do
	echo "== $ph"
	fn=phase_$(echo "$ph" | tr '-' '_')
	$fn
done

echo "functest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
