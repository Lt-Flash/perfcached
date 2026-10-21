#!/bin/sh
# menuconfig.sh - S250: choose what `make` builds, and write config.mk.
#
#   standalone - the cache alone: no cluster code linked, a [cluster]
#                section in the config is refused
#   clustered  - everything (eager, store, shard, proxy, spread); the
#                default when there is no config.mk
#
# and (S282) which allocator it links:
#   libc      the C library's own malloc - the default
#   mimalloc  mimalloc 2.x, linked ahead of the C library
#   jemalloc  jemalloc, the same way
# The C library itself is not asked: it is the toolchain's (Debian's gcc
# = glibc, Alpine's = musl), and the menu says which one it found.
#
# and (S357) the build flavor:
#   release   -O2 -g - the default, and what the packages ship
#   debug     -Og -g3, frame pointers, the arena's consistency checks
#   asan      debug + AddressSanitizer/UBSan, runtimes linked statically
#             (glibc toolchains only: the sanitizers do not support musl)
#
# and (S352) the alert mailer's transport:
#   none      no SMTP client; a [mail] section is refused - the default
#   libcurl   curl's SMTP client (STARTTLS/TLS, AUTH); needs libcurl's
#             development files (libcurl4-openssl-dev, Alpine curl-dev)
#
# Uses whiptail or dialog when present, plain prompts otherwise, so it
# works over ssh and in a minimal build container alike.  Non-interactive:
#   sh tools/menuconfig.sh standalone|clustered [libc|mimalloc|jemalloc] [release|debug|asan] [none|libcurl]
set -u
CFG=config.mk
cur=clustered
[ -f "$CFG" ] && cur=$(sed -n 's/^PC_EDITION *= *//p' "$CFG")
[ -n "$cur" ] || cur=clustered
acur=libc
[ -f "$CFG" ] && acur=$(sed -n 's/^PC_ALLOC *= *//p' "$CFG")
[ -n "$acur" ] || acur=libc
alloc=${2:-}
bcur=release
[ -f "$CFG" ] && bcur=$(sed -n 's/^PC_BUILD *= *//p' "$CFG")
[ -n "$bcur" ] || bcur=release
flavor=${3:-}
mcur=none
[ -f "$CFG" ] && mcur=$(sed -n 's/^PC_MAIL *= *//p' "$CFG")
[ -n "$mcur" ] || mcur=none
mail=${4:-}
HAVE_CURL=no
pkg-config --exists libcurl 2>/dev/null && HAVE_CURL=yes
UI=
# what this toolchain links against, and which allocators are installed
case "$(${CC:-cc} -dumpmachine 2>/dev/null)" in
*-musl*) LIBC=musl;; *-gnu*) LIBC=glibc;; *) LIBC="an unrecognised C library";; esac
have() { for f in "$@"; do [ -e "$f" ] && return 0; done; return 1; }
HAVE_MI=no; HAVE_JE=no
have /usr/lib/libmimalloc-insecure.so.2 /usr/lib/*/libmimalloc-insecure.so.2 \
	/usr/local/lib/libmimalloc-insecure.so.2 /usr/lib/libmimalloc.so.2 \
	/usr/lib/*/libmimalloc.so.2 /usr/local/lib/libmimalloc.so.2 && HAVE_MI=yes
have /usr/lib/libjemalloc.so.2 /usr/lib/*/libjemalloc.so.2 \
	/usr/local/lib/libjemalloc.so.2 && HAVE_JE=yes
choice=${1:-}
if [ -z "$choice" ]; then
	if [ -t 0 ] && command -v whiptail >/dev/null 2>&1; then UI=whiptail
	elif [ -t 0 ] && command -v dialog >/dev/null 2>&1; then UI=dialog
	else UI=; fi
	on_s=OFF; on_c=OFF; [ "$cur" = standalone ] && on_s=ON || on_c=ON
	if [ -n "$UI" ]; then
		choice=$($UI --title "perfcached build" --radiolist \
			"Which daemon should make build?  (space selects, enter confirms)" 14 72 2 \
			standalone "the cache alone - no cluster code, smallest, fastest" $on_s \
			clustered  "everything: eager, store, shard, proxy, spread" $on_c \
			3>&1 1>&2 2>&3) || { echo "menuconfig: cancelled, $CFG unchanged"; exit 1; }
	else
		echo "perfcached build - which daemon should make build?"
		echo "  1) standalone  the cache alone - no cluster code, smallest, fastest"
		echo "  2) clustered   everything: eager, store, shard, proxy, spread"
		printf "choice [current: %s]: " "$cur"
		read -r a || a=
		case "$a" in 1|standalone) choice=standalone;; 2|clustered) choice=clustered;;
			"") choice=$cur;; *) echo "menuconfig: '$a' is not 1 or 2"; exit 1;; esac
	fi
fi
case "$choice" in standalone|clustered) ;; *) echo "menuconfig: '$choice' - want standalone or clustered"; exit 1;; esac

# ---- S282: the allocator ---------------------------------------------------
if [ -z "$alloc" ] && [ -n "${1:-}" ]; then
	alloc=$acur                    # non-interactive, edition only: keep it
fi
if [ -z "$alloc" ]; then
	mi_note="mimalloc 2.x"; [ $HAVE_MI = no ] && mi_note="mimalloc 2.x - NOT installed"
	je_note="jemalloc"; [ $HAVE_JE = no ] && je_note="jemalloc - NOT installed"
	if [ -n "$UI" ]; then
		o_l=OFF; o_m=OFF; o_j=OFF
		case "$acur" in mimalloc) o_m=ON;; jemalloc) o_j=ON;; *) o_l=ON;; esac
		alloc=$($UI --title "perfcached build" --radiolist \
			"Which allocator?  This toolchain links $LIBC." 14 72 3 \
			libc     "the C library's own malloc (default)" $o_l \
			mimalloc "$mi_note, ahead of the C library" $o_m \
			jemalloc "$je_note, ahead of the C library" $o_j \
			3>&1 1>&2 2>&3) || { echo "menuconfig: cancelled, $CFG unchanged"; exit 1; }
	else
		echo "which allocator?  (this toolchain links $LIBC)"
		echo "  1) libc      the C library's own malloc (default)"
		echo "  2) mimalloc  $mi_note, ahead of the C library"
		echo "  3) jemalloc  $je_note, ahead of the C library"
		printf "choice [current: %s]: " "$acur"
		read -r a || a=
		case "$a" in 1|libc) alloc=libc;; 2|mimalloc) alloc=mimalloc;; 3|jemalloc) alloc=jemalloc;;
			"") alloc=$acur;; *) echo "menuconfig: '$a' is not 1, 2 or 3"; exit 1;; esac
	fi
fi
case "$alloc" in
libc) ;;
mimalloc) [ $HAVE_MI = yes ] || { echo "menuconfig: mimalloc 2.x is not installed (Alpine: apk add mimalloc2-insecure)"; exit 1; };;
jemalloc) [ $HAVE_JE = yes ] || { echo "menuconfig: jemalloc is not installed (Alpine: apk add jemalloc; Debian: libjemalloc2)"; exit 1; };;
*) echo "menuconfig: '$alloc' - want libc, mimalloc or jemalloc"; exit 1;;
esac
# ---- S357: the build flavor --------------------------------------------------
if [ -z "$flavor" ] && [ -n "${1:-}" ]; then
	flavor=$bcur                   # non-interactive without it: keep it
fi
if [ -z "$flavor" ]; then
	an_note="debug + AddressSanitizer/UBSan, static runtimes"
	[ "$LIBC" = musl ] && an_note="not on musl - the sanitizers need glibc"
	if [ -n "$UI" ]; then
		o_r=OFF; o_d=OFF; o_a=OFF
		case "$bcur" in debug) o_d=ON;; asan) o_a=ON;; *) o_r=ON;; esac
		flavor=$($UI --title "perfcached build" --radiolist \
			"Which build flavor?" 14 72 3 \
			release "-O2 -g - what the packages ship (default)" $o_r \
			debug   "-Og -g3, frame pointers, arena checks" $o_d \
			asan    "$an_note" $o_a \
			3>&1 1>&2 2>&3) || { echo "menuconfig: cancelled, $CFG unchanged"; exit 1; }
	else
		echo "which build flavor?"
		echo "  1) release  -O2 -g - what the packages ship (default)"
		echo "  2) debug    -Og -g3, frame pointers, arena checks"
		echo "  3) asan     $an_note"
		printf "choice [current: %s]: " "$bcur"
		read -r a || a=
		case "$a" in 1|release) flavor=release;; 2|debug) flavor=debug;; 3|asan) flavor=asan;;
			"") flavor=$bcur;; *) echo "menuconfig: '$a' is not 1, 2 or 3"; exit 1;; esac
	fi
fi
case "$flavor" in
release|debug) ;;
asan) [ "$LIBC" != musl ] || { echo "menuconfig: asan needs a glibc toolchain - this one links musl"; exit 1; };;
*) echo "menuconfig: '$flavor' - want release, debug or asan"; exit 1;;
esac
# ---- S352: the mail transport -------------------------------------------------
if [ -z "$mail" ] && [ -n "${1:-}" ]; then
	mail=$mcur                     # non-interactive without it: keep it
fi
if [ -z "$mail" ]; then
	cu_note="curl's SMTP client - alerts by mail"; [ $HAVE_CURL = no ] && cu_note="libcurl - NOT installed (libcurl4-openssl-dev / curl-dev)"
	if [ -n "$UI" ]; then
		o_n=OFF; o_c=OFF
		case "$mcur" in libcurl) o_c=ON;; *) o_n=ON;; esac
		mail=$($UI --title "perfcached build" --radiolist \
			"Alert mail transport?" 12 72 2 \
			none    "no mail - alerts are logged and kept on the page (default)" $o_n \
			libcurl "$cu_note" $o_c \
			3>&1 1>&2 2>&3) || { echo "menuconfig: cancelled, $CFG unchanged"; exit 1; }
	else
		echo "alert mail transport?"
		echo "  1) none     no mail - alerts are logged and kept on the page (default)"
		echo "  2) libcurl  $cu_note"
		printf "choice [current: %s]: " "$mcur"
		read -r a || a=
		case "$a" in 1|none) mail=none;; 2|libcurl) mail=libcurl;;
			"") mail=$mcur;; *) echo "menuconfig: '$a' is not 1 or 2"; exit 1;; esac
	fi
fi
case "$mail" in
none) ;;
libcurl) [ $HAVE_CURL = yes ] || { echo "menuconfig: libcurl is not installed (Debian/Ubuntu: libcurl4-openssl-dev; Alpine: curl-dev)"; exit 1; };;
*) echo "menuconfig: '$mail' - want none or libcurl"; exit 1;;
esac
{
	echo "# written by make menuconfig ($(date -u +%Y-%m-%dT%H:%MZ)) - edit with make menuconfig"
	echo "PC_EDITION = $choice"
	echo "PC_ALLOC = $alloc"
	echo "PC_BUILD = $flavor"
	echo "PC_MAIL = $mail"
} > "$CFG"
echo "menuconfig: $CFG says PC_EDITION = $choice$( [ "$choice" != "$cur" ] && echo " (was $cur)"), PC_ALLOC = $alloc$( [ "$alloc" != "$acur" ] && echo " (was $acur)"), PC_BUILD = $flavor$( [ "$flavor" != "$bcur" ] && echo " (was $bcur)"), PC_MAIL = $mail$( [ "$mail" != "$mcur" ] && echo " (was $mcur)") - the next make rebuilds what changed"
# objects carry their compiler flags and make does not see a flag change:
# a new flavor wants a clean tree, or old and new objects link together
[ "$flavor" = "$bcur" ] || echo "menuconfig: the flavor changed - run make clean before make, so no object of the old flavor is linked in"
