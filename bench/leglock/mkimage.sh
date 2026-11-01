#!/bin/sh
# mkimage.sh [tag] - from the tree this sits in: compile perfcached, perfcli and pcbench (the
# Makefile's release flags) and wrap them in a debian:13-slim image for drive.sh; prints its name
set -eu
T=$(cd "$(dirname "$0")/../.." && pwd)
REV=$(git -C "$T" rev-parse --short=9 HEAD 2>/dev/null || echo tree)
TAG=${1:-localhost/perfcached:leglock-$REV}
make -C "$T" -j"$(nproc)" perfcached perfcli pcbench >/dev/null
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
cp "$T/perfcached" "$T/perfcli" "$T/pcbench" "$D/"
cat > "$D/Containerfile" <<'CF'
FROM docker.io/library/debian:13-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends libsodium23 python3-minimal \
 && rm -rf /var/lib/apt/lists/*
COPY perfcached perfcli pcbench /usr/local/bin/
ENTRYPOINT ["perfcached"]
CMD ["-f", "/etc/perfcached.conf"]
CF
podman build -q --platform linux/amd64 -t "$TAG" "$D" >/dev/null
podman run --rm --entrypoint perfcached "$TAG" -V | head -1
echo "$TAG"
