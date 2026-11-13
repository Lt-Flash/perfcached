#!/bin/sh
# The admin image's entrypoint: the dashboard on loopback behind nginx on 8080, or 8443 when
# tls/cert.pem and key.pem are mounted; arguments run instead of the services, config rendered first.
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
