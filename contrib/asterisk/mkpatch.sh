#!/bin/sh
# mkpatch.sh - generate the Asterisk patch that adds func_perfd
# (PERFD_GET / PERFD_SET / PERFD_EXISTS / PERFD_DELETE).
#
#   contrib/asterisk/mkpatch.sh <asterisk-X.Y.Z.tar.gz> [out.patch]
#       writes the patch (default contrib/asterisk/asterisk-X.Y.Z-func_perfd.patch)
#   contrib/asterisk/mkpatch.sh --tree <asterisk-X.Y.Z.tar.gz> <dir>
#       unpacks the release into <dir> (which must not exist) and applies
#       the same changes there, without writing a patch
#
# The patch is generated, never edited by hand: another Asterisk version is a
# re-run.  configure is NOT in it - run ./bootstrap.sh after applying.
# libperfd is NOT in it either (perfcached S371): the module links an
# installed libperfd 0.5.0 or later, found by configure through pkg-config
# (libperfd-dev, or `make install-lib` in perfcached + PKG_CONFIG_PATH).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)

die() { echo "mkpatch: $*" >&2; exit 1; }

MODE=patch
if [ "${1:-}" = "--tree" ]; then
	MODE=tree
	shift
fi
TARBALL=${1:?usage: mkpatch.sh [--tree] <asterisk-X.Y.Z.tar.gz> [out.patch | dir]}
[ -f "$TARBALL" ] || die "$TARBALL: no such file"
NAME=$(basename "$TARBALL")
NAME=${NAME%.tar.gz}
case "$NAME" in
asterisk-[0-9]*) ;;
*) die "$TARBALL does not look like an Asterisk release tarball" ;;
esac

# insert <text> as a line before the one line equal to <anchor> in <file>
insert_before() {
	ANCHOR=$2 TEXT=$3 awk '
		$0 == ENVIRON["ANCHOR"] { n++; print ENVIRON["TEXT"] }
		{ print }
		END { if (n != 1) exit 3 }' "$1" > "$1.mkpatch" \
		|| die "$1: expected exactly one line '$2'"
	mv "$1.mkpatch" "$1"
}

apply_changes() {
	D=$1
	for f in configure.ac makeopts.in build_tools/menuselect-deps.in funcs/Makefile; do
		[ -f "$D/$f" ] || die "$f is missing from the release"
	done

	cp "$HERE/func_perfd.c" "$D/funcs/func_perfd.c"
	mkdir -p "$D/funcs/perfd"
	cp "$HERE/resp.c" "$HERE/resp.h" "$D/funcs/perfd/"
	cp "$HERE/perfd.conf.sample" "$D/configs/samples/perfd.conf.sample"

	insert_before "$D/configure.ac" \
		'AST_EXT_LIB_SETUP([SPEEX], [Speex], [speex])' \
		'AST_EXT_LIB_SETUP([PERFD], [libperfd], [perfd])'
	insert_before "$D/configure.ac" \
		'AST_EXT_LIB_CHECK([SPEEX], [speex], [speex_encode], [speex/speex.h], [-lm])' \
		'AST_PKG_CONFIG_CHECK([PERFD], [libperfd >= 0.5.0])'
	# no pkg-config, or --with-perfd=DIR: the plain header + library probe
	insert_before "$D/configure.ac" \
		'AST_EXT_LIB_CHECK([SPEEX], [speex], [speex_encode], [speex/speex.h], [-lm])' \
		'AST_EXT_LIB_CHECK([PERFD], [perfd], [perfd_opts_init], [perfd.h])'
	insert_before "$D/makeopts.in" 'SPEEX_INCLUDE=@SPEEX_INCLUDE@' 'PERFD_INCLUDE=@PERFD_INCLUDE@'
	insert_before "$D/makeopts.in" 'SPEEX_INCLUDE=@SPEEX_INCLUDE@' 'PERFD_LIB=@PERFD_LIB@'
	insert_before "$D/build_tools/menuselect-deps.in" 'SPEEX=@PBX_SPEEX@' 'PERFD=@PBX_PERFD@'

	cat >> "$D/funcs/Makefile" <<'EOF'

# func_perfd: links libperfd (configure's PERFD, pkg-config libperfd) and
# carries only its own RESP2 client - never a copy of the library
$(if $(wildcard perfd/perfd.c perfd/lib/perfd.c perfd/src/pc_noise.c perfd/pc_noise.c),\
	$(error func_perfd: a copy of libperfd's source is in funcs/perfd - link the installed library instead (perfcached S371)))
$(call MOD_ADD_C,func_perfd,perfd/resp.c)
EOF
}

if [ "$MODE" = tree ]; then
	DIR=${2:?usage: mkpatch.sh --tree <tarball> <dir>}
	[ ! -e "$DIR" ] || die "$DIR already exists"
	mkdir -p "$DIR"
	tar -xzf "$TARBALL" -C "$DIR" --strip-components=1
	apply_changes "$DIR"
	echo "patched tree: $DIR"
	exit 0
fi

OUT=${2:-$HERE/$NAME-func_perfd.patch}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT INT TERM
mkdir "$T/a" "$T/b"
tar -xzf "$TARBALL" -C "$T/a" --strip-components=1
tar -xzf "$TARBALL" -C "$T/b" --strip-components=1
apply_changes "$T/b"

rc=0
(cd "$T" && diff -ruN a b) > "$T/raw.patch" || rc=$?
[ "$rc" -eq 1 ] || die "diff exited $rc"
# drop the timestamps so a re-run from the same sources is byte-identical
TAB=$(printf '\t')
sed -e "s/^--- \([^$TAB]*\)$TAB.*\$/--- \1/" \
    -e "s/^+++ \([^$TAB]*\)$TAB.*\$/+++ \1/" "$T/raw.patch" > "$OUT"
echo "wrote $OUT ($(wc -l < "$OUT") lines)"
