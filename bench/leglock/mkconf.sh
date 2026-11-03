# mkconf.sh <i> <buckets_log2> <autoscale> - one fleet node's config, collection "th" under test
# MODE=eager (default): the 245-247 shape; MODE=local: legacy per-collection store mode, nothing replicates
cat <<CONF
[daemon]
workers = ${WORKERS:-8}
log_level = info
[memory]
arena_mb = ${ARENA_MB:-2048}
[secrets]
client = bench-client-secret
cluster = bench-cluster-secret
[listen]
tcp = 127.0.0.1:6479
plaintext = loopback
[cluster]
multicast = 239.255.99.9:7191
advertise = 10.99.0.1$1
$( [ "${MODE:-eager}" = eager ] && printf 'mode = eager\ncollections = th' )
[collection th]
buckets_log2 = $2
autoscale = $3
durable = no
CONF
