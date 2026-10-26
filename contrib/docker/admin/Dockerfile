# perfcached-admin - the admin image (DESIGN 12no, S413 part 1).
#
# What it carries: perfcached-dashboard (the status page served over
# libperfd, with the actions), perfcli, and nginx in front of the
# dashboard - TLS, HTTP/2 and the published port are nginx's; the
# dashboard itself listens on the container's loopback only.
#
# Build (from the tree's root):  podman build -f contrib/docker/admin/Dockerfile
#                                  --build-arg REV=$(git rev-parse --short HEAD) -t perfcached-admin .
# Run:   see contrib/docker/admin/entrypoint.sh for the environment and the mounts.
#
# Two stages: the tools are built from this tree with the same Makefile
# a package build uses; the runtime stage holds only what runs.

FROM docker.io/library/debian:13-slim AS build
ARG REV=unknown
RUN apt-get update && apt-get install -y --no-install-recommends \
      gcc make pkg-config libsodium-dev libmicrohttpd-dev \
    && rm -rf /var/lib/apt/lists/*
COPY . /src
WORKDIR /src
RUN make PC_DASH=yes PC_BUILD_REV="$REV" libperfd.a perfcli perfcached-dashboard \
    && ./perfcached-dashboard -V && ./perfcli -V

FROM docker.io/library/debian:13-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      nginx-light libsodium23 libmicrohttpd12 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && rm -f /etc/nginx/sites-enabled/default \
    && useradd -r -s /usr/sbin/nologin -d /nonexistent perfcached-dashboard \
    && mkdir -p /etc/perfcached /var/log/nginx /run/nginx
# runuser (the entrypoint drops to the dashboard's user with it) is util-linux's, in the base already
COPY --from=build /src/perfcached-dashboard /src/perfcli /usr/bin/
COPY contrib/docker/admin/nginx-plain.conf /etc/nginx/nginx-plain.conf
COPY contrib/docker/admin/nginx-tls.conf /etc/nginx/nginx-tls.conf
COPY contrib/docker/admin/entrypoint.sh /usr/local/bin/entrypoint
COPY contrib/perfcached-dashboard.conf.example /etc/perfcached/perfcached-dashboard.conf.example
RUN chmod 755 /usr/local/bin/entrypoint
# 8080 plain, 8443 TLS (when /etc/perfcached/tls/cert.pem and key.pem are mounted)
EXPOSE 8080 8443
ENTRYPOINT ["/usr/local/bin/entrypoint"]
