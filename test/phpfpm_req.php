<?php
// phpfpm_req.php - one request of test/phpfpmtest.sh: answers one line of JSON.
parse_str($_SERVER['QUERY_STRING'] ?? '', $q);
$r = ['pid' => getmypid(), 'ok' => false];
try {
    $pc = new Perfcached(isset($q['seeds']) ? explode(',', $q['seeds']) : '127.0.0.1', (int)$q['port'], ['secrets' => [$q['sec']], 'spares' => Perfcached::SPARES_NONE,
        'io_timeout_ms' => (int)($q['io'] ?? 5000)]);
    $k = 'w' . getmypid();
    if (($q['op'] ?? 'rw') === 'stall') {
        // the daemon is frozen: this call times out, then, thawed, the same handle answers
        try {
            $pc->get('c', $k);
            $r['stalled'] = 'no';
        } catch (PerfcachedTransportException $e) {
            $r['stalled'] = 'yes';
        }
        touch($q['mark'] . '.failed');
        for ($i = 0; $i < 200 && !file_exists($q['mark'] . '.thawed'); $i++) usleep(50000);
    }
    $v = bin2hex(random_bytes(8));
    $pc->set('c', $k, $v);
    $r['ok'] = $pc->get('c', $k) === $v;
} catch (\Throwable $e) {
    $r['error'] = get_class($e) . ': ' . $e->getMessage();
}
$r['pool'] = Perfcached::pool();
echo json_encode($r), "\n";
