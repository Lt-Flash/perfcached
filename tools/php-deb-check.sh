#!/bin/sh
# php-deb-check.sh - PE-6: php8.3-perfcached built from the committed tree and proven on a
# clean Debian 13 with Sury's PHP 8.3, in throwaway containers; a host with neither podman nor docker SKIPS loudly.
#   tools/php-deb-check.sh            on the host
#   tools/php-deb-check.sh --build    inside: libperfd0, libperfd-dev, php8.3-perfcached
#   tools/php-deb-check.sh --install  inside: install, load, remove, purge, install again
set -u
DEBIAN=${DEBIAN:-13}
fail() { echo "php-deb-check: FAILED - $*"; exit 1; }
sury() {
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq > /dev/null 2>&1 && apt-get install -y -qq ca-certificates curl > /dev/null 2>&1 || fail "apt: curl"
	curl -fsSL -o /tmp/sury.deb https://packages.sury.org/debsuryorg-archive-keyring.deb || fail "Sury's keyring"
	dpkg -i /tmp/sury.deb > /dev/null || fail "Sury's keyring package"
	echo 'deb [signed-by=/usr/share/keyrings/debsuryorg-archive-keyring.gpg] https://packages.sury.org/php/ trixie main' > /etc/apt/sources.list.d/php.list
	apt-get update -qq > /dev/null 2>&1 || fail "apt update with Sury"
}
if [ "${1:-}" = --build ]; then
	sury
	export DEB_BUILD_OPTIONS="nocheck parallel=$(nproc)"
	apt-get install -y -qq build-essential debhelper git php8.3-dev libsodium-dev libcurl4-openssl-dev libmicrohttpd-dev pkg-config > /dev/null 2>&1 || fail "apt: build deps"
	mkdir -p /tmp/s && tar -xzf /work/src.tgz -C /tmp/s || fail "unpack"
	cd /tmp/s/perfcached || fail "no tree"
	dpkg-buildpackage -us -uc -b > /work/main.log 2>&1 || { tail -n 20 /work/main.log; fail "the main package"; }
	cp ../libperfd0_*.deb ../libperfd-dev_*.deb /work/ || fail "no libperfd debs"
	apt-get install -y -qq /work/libperfd0_*.deb /work/libperfd-dev_*.deb > /dev/null 2>&1 || fail "apt: libperfd"
	cd lang/php || fail "no lang/php"
	dpkg-buildpackage -us -uc -b > /work/php.log 2>&1 || { tail -n 20 /work/php.log; fail "php8.3-perfcached"; }
	cp ../php8.3-perfcached_*.deb /work/ || fail "no php deb"
	exit 0
fi
if [ "${1:-}" = --install ]; then
	sury
	ok() { echo "  ok   $1"; }
	EXT=/usr/lib/php/20230831/perfcached.so
	INI=/etc/php/8.3/mods-available/perfcached.ini
	apt-get install -y -qq php8.3-cli php8.3-fpm > /dev/null 2>&1 || fail "apt: php8.3"
	apt-get install -y -qq /work/libperfd0_*.deb /work/php8.3-perfcached_*.deb > /work/inst.log 2>&1 || { tail -n 20 /work/inst.log; fail "install"; }
	ok "php8.3-perfcached installs on Debian 13 with Sury's PHP 8.3"
	D=$(dpkg-query -W -f='${Depends}' php8.3-perfcached)
	for w in libperfd0 phpapi-20230831 php8.3-common; do
		case "$D" in *"$w"*) ;; *) fail "Depends lacks $w: $D" ;; esac
	done
	ok "it depends on libperfd0 and the PHP API version"
	[ -f "$EXT" ] && [ -f "$INI" ] || fail "files: $(dpkg -L php8.3-perfcached | tr '\n' ' ')"
	php8.3 -m | grep -qx perfcached || fail "the CLI does not load it"
	php-fpm8.3 -m | grep -qx perfcached || fail "FPM does not load it"
	ok "the CLI and FPM load it, through mods-available"
	W=$(php8.3 -v 2>&1 | grep -ciE 'warning|unable to load')
	[ "$W" = 0 ] || fail "PHP warns: $(php8.3 -v 2>&1 | head -n 3)"
	V=$(php8.3 -r 'echo (new ReflectionExtension("perfcached"))->getVersion();')
	P=$(dpkg-query -W -f='${Version}' php8.3-perfcached)
	[ "$V-1" = "$P" ] || fail "the extension says $V, the package $P"
	ok "the extension and the package agree on the version: $V"
	php8.3 -r 'try { new Perfcached(["127.0.0.1", "127.0.0.2"], 1, ["connect_timeout_ms" => 200]); echo "no\n"; } catch (PerfcachedTransportException $e) { echo "refused\n"; }' | grep -qx refused || fail "libperfd is not behind it"
	ok "a dial with nothing listening raises PerfcachedTransportException"
	apt-get remove -y -qq php8.3-perfcached > /work/rm.log 2>&1 || { tail -n 20 /work/rm.log; fail "remove"; }
	[ ! -e "$EXT" ] || fail "the .so outlived remove"
	php8.3 -m | grep -qx perfcached && fail "the CLI still has it after remove"
	W=$(php8.3 -v 2>&1 | grep -ciE 'warning|unable to load')
	[ "$W" = 0 ] || fail "PHP warns after remove: $(php8.3 -v 2>&1 | head -n 3)"
	[ -f "$INI" ] || fail "remove took the conffile"
	ok "remove: the .so gone, PHP clean, the conffile kept"
	apt-get purge -y -qq php8.3-perfcached > /work/rm.log 2>&1 || { tail -n 20 /work/rm.log; fail "purge"; }
	[ ! -e "$INI" ] || fail "the ini outlived purge"
	[ -z "$(find /etc/php/8.3 -xtype l 2>/dev/null)" ] || fail "dangling links: $(find /etc/php/8.3 -xtype l)"
	dpkg-query -W -f='${Status}' php8.3-perfcached 2>/dev/null | grep -q installed && fail "still listed after purge"
	ok "purge: nothing left, no dangling link"
	apt-get install -y -qq /work/php8.3-perfcached_*.deb > /work/inst.log 2>&1 || fail "install again"
	php8.3 -m | grep -qx perfcached || fail "not loaded after installing again"
	ok "and it installs again"
	exit 0
fi
E=
for e in podman docker; do command -v $e > /dev/null 2>&1 && { E=$e; break; }; done
if [ -z "$E" ]; then
	echo "################################################################"
	echo "# php-deb-check: SKIPPED - neither podman nor docker is here   #"
	echo "################################################################"
	exit 0
fi
SRC=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d /var/tmp/pcphpd.XXXXXX)
trap 'rm -rf "$W"' EXIT
git -C "$SRC" archive --format=tar.gz --prefix=perfcached/ -o "$W/src.tgz" HEAD || fail "git archive"
cp "$SRC/tools/php-deb-check.sh" "$W/check.sh"
echo "=== php-deb-check: debian:$DEBIAN with $E"
$E run --rm -v "$W:/work:z" docker.io/library/debian:$DEBIAN sh /work/check.sh --build || exit 1
$E run --rm -v "$W:/work:z" docker.io/library/debian:$DEBIAN sh /work/check.sh --install || exit 1
echo "php-deb-check: passed"
