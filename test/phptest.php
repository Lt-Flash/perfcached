<?php
/* phptest.php — Perfcached.php against live daemons (via phptest.sh).
 * argv: <pt-port> <nx-port> <secret> */

require __DIR__ . '/../lang/php/Perfcached.php';

$ptPort = (int)($argv[1] ?? 6479);
$nxPort = (int)($argv[2] ?? 0);
$secret = $argv[3] ?? '';

$pass = 0;
$fail = 0;
function ok(bool $cond, string $name): void
{
    global $pass, $fail;
    if ($cond) {
        $pass++;
    } else {
        $fail++;
        echo "FAIL: $name\n";
    }
}

/* ---- plaintext ---------------------------------------------------------- */
$pc = new Perfcached('127.0.0.1', $ptPort);
$pc->ping();
ok(true, 'connect + ping');

$pc->set('c', 'k1', 'hello', 500);
ok($pc->get('c', 'k1') === 'hello', 'set/get');
$t = $pc->ttl('c', 'k1');
ok($t > 400 && $t <= 500, 'ttl');

$bin = "a\0\x01\x02\xff\0z";
$pc->set('c', 'bin', $bin);
ok($pc->get('c', 'bin') === $bin, 'binary NUL value bit-exact');
ok($pc->ttl('c', 'bin') === -1, 'no-expiry ttl -1');

ok($pc->get('c', 'nokey') === null, 'miss null');
ok($pc->exists('c', 'k1') === true, 'exists');
ok($pc->exists('c', 'nokey') === false, 'not exists');
ok($pc->ttl('c', 'nokey') === -2, 'ttl absent -2');
ok($pc->expire('c', 'k1', 900) === true, 'expire');
ok($pc->ttl('c', 'k1') > 800, 'expire took');
ok($pc->expire('c', 'nokey', 5) === false, 'expire absent');
ok($pc->del('c', 'k1') === true, 'del');
ok($pc->del('c', 'k1') === false, 'del absent');

ok($pc->add('c', 'ctr', 5) === 5, 'add 5');
ok($pc->sub('c', 'ctr', 2) === 3, 'sub 2');
ok($pc->add('c', 'ctr') === 4, 'add default 1');

$pc->set('c', 'm1', 'v1');
$pc->set('c', 'm2', 'v2');
$m = $pc->mget('c', ['m1', 'gone', 'm2']);
ok($m === ['m1' => 'v1', 'gone' => null, 'm2' => 'v2'], 'mget with miss');

$ks = $pc->keys('c', 'm*');
sort($ks);
ok($ks === ['m1', 'm2'], 'keys match');

$pc->jset('c', 'doc', '$', '{"n":1,"s":"x"}');
ok($pc->jincr('c', 'doc', '$.n', 4) === 5, 'jincr');
ok($pc->jget('c', 'doc', '$.s') === '"x"', 'jget fragment');
ok($pc->jdel('c', 'doc', '$.s') === true, 'jdel');
ok($pc->jdel('c', 'doc', '$.s') === false, 'jdel absent');

$st = $pc->stats();
ok(isset($st['memory']['arena_total']), 'stats decoded');

try {
    $pc->get('nosuch', 'x');
    ok(false, 'bad col must throw');
} catch (PerfcachedException $e) {
    ok(strpos($e->getMessage(), 'collection') !== false,
        'error message surfaced');
}
$pc->close();

/* ---- the Noise leg + rotation ------------------------------------------- */
if ($nxPort && $secret !== '') {
    $pc = new Perfcached('127.0.0.1', $nxPort,
        ['secrets' => ['not-the-secret', $secret]]);
    ok(true, 'rotation: second secret wins');
    $pc->set('c', 'nk', $bin);
    ok($pc->get('c', 'nk') === $bin, 'noise binary roundtrip');
    /* a value big enough to need several transport records */
    $big = random_bytes(50000);
    $pc->set('c', 'big', $big);
    ok($pc->get('c', 'big') === $big, 'multi-record value');
    $pc->close();

    try {
        new Perfcached('127.0.0.1', $nxPort,
            ['secrets' => ['not-the-secret']]);
        ok(false, 'wrong-only secrets must throw');
    } catch (PerfcachedException $e) {
        ok(strpos($e->getMessage(), 'handshake') !== false,
            'wrong secret refused');
    }
}

echo "phptest: $pass passed, $fail failed\n";
exit($fail ? 1 : 0);
