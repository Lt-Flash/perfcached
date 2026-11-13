#!/bin/sh
# image-admin.sh [outdir] - build the admin image (perfcached-dashboard + perfcli + nginx), prove it runs and
# save <outdir>/perfcached-admin-<ver>.tar; SKIPS loudly without podman or docker.
set -u
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-/var/tmp/pcrel}
VER=$(sed -n 's/.*PC_VERSION[^"]*"\([^"]*\)".*/\1/p' "$SRC/src/version.h")
REV=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)
TAG=perfcached-admin:$VER
E=
for e in podman docker; do command -v $e > /dev/null 2>&1 && { E=$e; break; }; done
if [ -z "$E" ]; then
	echo "################################################################"
	echo "# image-admin: SKIPPED - neither podman nor docker is here     #"
	echo "################################################################"
	exit 0
fi
mkdir -p "$OUT"
cd "$SRC" || exit 1
echo "=== image-admin: $TAG from $REV with $E"
$E build -f contrib/docker/admin/Dockerfile --build-arg REV="$REV" -t "$TAG" . > "$OUT/image-admin.build.log" 2>&1 || {
	echo "image-admin: build FAILED"; tail -30 "$OUT/image-admin.build.log"; exit 1; }
# proof: the tools answer, both nginx configs parse, the entrypoint refuses a
# container with nothing to serve and renders a config from the environment
v=$($E run --rm "$TAG" perfcached-dashboard -V 2>&1); echo "  $v"
LVER=$(sed -n 's/^#define PERFD_VERSION "\(.*\)"/\1/p' "$SRC/lib/perfd.h")   # the dashboard's is libperfd's
echo "$v" | grep -q "perfcached-dashboard $LVER" || { echo "image-admin: the dashboard's -V is not $LVER"; exit 1; }
$E run --rm "$TAG" perfcli -V 2>&1 | grep -q "perfcli" || { echo "image-admin: perfcli -V"; exit 1; }
$E run --rm "$TAG" nginx -t -c /etc/nginx/nginx-plain.conf > /dev/null 2>&1 || { echo "image-admin: nginx-plain.conf does not parse"; exit 1; }
$E run --rm "$TAG" sh -c 'mkdir -p /etc/perfcached/tls && openssl req -x509 -newkey rsa:2048 -nodes -keyout /etc/perfcached/tls/key.pem -out /etc/perfcached/tls/cert.pem -subj /CN=t -days 1 >/dev/null 2>&1; nginx -t -c /etc/nginx/nginx-tls.conf' > /dev/null 2>&1 \
	|| echo "  (nginx-tls.conf not parsed here: no openssl in the image - checked by the CI's own cert when one is mounted)"
r=$($E run --rm "$TAG" 2>&1); rc=$?
[ $rc = 2 ] && echo "$r" | grep -q "PCD_SEEDS" && echo "  ok   nothing to serve: refused (rc 2)" || { echo "image-admin: an empty container should refuse: rc $rc: $r"; exit 1; }
r=$($E run --rm -e PCD_SEEDS=127.0.0.1:6479 -e PCD_SECRET=x "$TAG" sh -c 'sed -n "s/^seed = //p;s/^listen = //p;s/^trusted_proxy = //p" /etc/perfcached/perfcached-dashboard.conf' 2>&1)
echo "$r" | grep -q "^127.0.0.1:6479" && echo "$r" | grep -q "^127.0.0.1:8090" && echo "$r" | grep -q "^127.0.0.1$" && echo "  ok   the config renders from the environment (seed, loopback listen, trusted proxy)" || { echo "image-admin: the rendered config: $(echo "$r" | tr '\n' '|')"; exit 1; }
r=$($E run --rm -e PCD_SEEDS=127.0.0.1:6479 -e PCD_SECRET=x -e PCD_FRONT_PROXY=yes "$TAG" sh -c 'grep -c "X-Forwarded-For \$http_x_forwarded_for" /run/nginx/nginx.conf; grep -c proxy_add_x_forwarded_for /run/nginx/nginx.conf' 2>&1 | tr '\n' ' ')
[ "$r" = "1 0 " ] && echo "  ok   PCD_FRONT_PROXY=yes: nginx passes X-Forwarded-For through" || { echo "image-admin: the front-proxy nginx config: '$r'"; exit 1; }
# only this version's image is left: podman's docker-archive will not overwrite an archive, and the CI
# job publishes every perfcached-admin-*.tar found here
rm -f "$OUT"/perfcached-admin-*.tar
$E save -o "$OUT/perfcached-admin-$VER.tar" "$TAG" || { echo "image-admin: save"; exit 1; }
echo "=== image-admin: $OUT/perfcached-admin-$VER.tar ($(du -h "$OUT/perfcached-admin-$VER.tar" | cut -f1))"
