#!/bin/sh
# The admin image's entrypoint (DESIGN 12no, S413): the dashboard on the
# container's loopback as its own user, nginx in front on 8080 (plain) or
# 8443 (TLS, when /etc/perfcached/tls/cert.pem and key.pem are mounted).
#
# Configuration, one of:
#   - mount /etc/perfcached/perfcached-dashboard.conf (the file wins), or
#   - environment: PCD_SEEDS (comma-separated host:port, required),
#     PCD_SECRET (the client secret) or PCD_SECRET_FILE (a mounted file),
#     PCD_SESSION_KEY (64 hex, for several replicas) - optional;
#     PCD_FRONT_PROXY=yes when a proxy of your own (HAProxy, nginx) is
#     the ONLY thing that reaches the published port: nginx then passes
#     the X-Forwarded-For it received through unchanged, so the dashboard's
#     lockout counts the browser the front saw, not the front.  Never set
#     it on a port browsers reach directly - a client could forge the
#     header and dodge the lockout.
# `listen` is always 127.0.0.1:8090 and `trusted_proxy` 127.0.0.1 here:
# nginx is the only thing that reaches the dashboard, and the lockout
# counts the client nginx saw.
#
# Anything after the image name runs instead of the services, with the
# config rendered first (e.g. `perfcli -h ... stats`, `perfcached-dashboard
# -V`, `sh`).
set -eu
CONF=/etc/perfcached/perfcached-dashboard.conf
if [ ! -s "$CONF" ] && [ -n "${PCD_SEEDS:-}" ]; then
	SECRET=${PCD_SECRET:-}
	[ -n "$SECRET" ] || [ -z "${PCD_SECRET_FILE:-}" ] || SECRET=$(cat "$PCD_SECRET_FILE")
	{
		echo "[dashboard]"
		echo "listen = 127.0.0.1:8090"
		echo "seed = $PCD_SEEDS"
		[ -z "$SECRET" ] || echo "secret = $SECRET"
		[ -z "${PCD_SESSION_KEY:-}" ] || echo "session_key = $PCD_SESSION_KEY"
		echo "trusted_proxy = 127.0.0.1"
	} > "$CONF"
	chmod 600 "$CONF"
	chown perfcached-dashboard "$CONF" 2>/dev/null || true
fi
if [ -s /etc/perfcached/tls/cert.pem ] && [ -s /etc/perfcached/tls/key.pem ]; then
	NGX=/etc/nginx/nginx-tls.conf
else
	NGX=/etc/nginx/nginx-plain.conf
fi
if [ "${PCD_FRONT_PROXY:-}" = yes ]; then
	sed 's/proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;/proxy_set_header X-Forwarded-For $http_x_forwarded_for;/' "$NGX" > /run/nginx/nginx.conf
	grep -q 'http_x_forwarded_for' /run/nginx/nginx.conf || { echo "entrypoint: PCD_FRONT_PROXY: nothing to rewrite in $NGX" >&2; exit 2; }
	NGX=/run/nginx/nginx.conf
fi
if [ $# -gt 0 ]; then
	exec "$@"
fi
[ -s "$CONF" ] || { echo "entrypoint: mount $CONF or set PCD_SEEDS (and PCD_SECRET)" >&2; exit 2; }
nginx -t -c "$NGX" > /dev/null
runuser -u perfcached-dashboard -- /usr/bin/perfcached-dashboard -f "$CONF" &
DASH=$!
trap 'kill -TERM $DASH 2>/dev/null; nginx -c "$NGX" -s quit 2>/dev/null; wait $DASH 2>/dev/null' TERM INT
nginx -c "$NGX" -g 'daemon off;' &
NGXP=$!
wait $NGXP
kill -TERM $DASH 2>/dev/null; wait $DASH 2>/dev/null || true
