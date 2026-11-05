<?php
// phpexttest.php - PE-1: every verb of the extension; see phpexttest.sh.
// Usage: php -d extension=... phpexttest.php <plaintext-port> <noise-port> <secret>
[$_, $pp, $np, $sec] = $argv;
$pp = (int)$pp; $np = (int)$np;
$fails = 0;
function ok(bool $c, string $n): void {
    global $fails;
    echo $c ? "  ok   $n\n" : "  FAIL $n\n";
    if (!$c) $fails++;
}
function throws(callable $f, string $cls, string $n, ?int $code = null): void {
    try {
        $f();
        ok(false, "$n: nothing thrown");
    } catch (\Throwable $e) {
        ok($e instanceof $cls && ($code === null || $e->getCode() === $code),
            "$n (" . get_class($e) . ", code " . $e->getCode() . ": " . substr($e->getMessage(), 0, 60) . ")");
    }
}
$bin = "\x00a\xff\xfeb\n\x00\x9e" . str_repeat("z", 1000);

foreach ([['plaintext door', $pp, []], ['Noise door', $np, ['secrets' => [$sec]]]] as [$door, $port, $opts]) {
    echo "--- $door\n";
    $pc = new Perfcached('127.0.0.1', $port, $opts);
    $pc->ping();
    ok(true, "$door: connect and ping");
    $pc->set('c', 'k1', 'hello', 500);
    ok($pc->get('c', 'k1') === 'hello', 'set and get');
    $t = $pc->ttl('c', 'k1');
    ok($t > 400 && $t <= 500, "ttl $t");
    $pc->set('c', 'bin', $bin);
    ok($pc->get('c', 'bin') === $bin, 'a value with NUL and non-UTF-8 bytes is byte-exact');
    ok($pc->ttl('c', 'bin') === -1, 'no expiry: ttl -1');
    ok($pc->get('c', 'nokey') === null, 'a miss is null');
    ok($pc->exists('c', 'k1') === true, 'exists');
    ok($pc->exists('c', 'nokey') === false, 'not exists');
    ok($pc->ttl('c', 'nokey') === -2, 'no such key: ttl -2');
    ok($pc->expire('c', 'k1', 900) === true, 'expire');
    ok($pc->ttl('c', 'k1') > 800, 'the expire took');
    ok($pc->expire('c', 'nokey', 5) === false, 'expire on no such key');
    ok($pc->del('c', 'k1') === true, 'del');
    ok($pc->del('c', 'k1') === false, 'del of no such key');
    $pc->del('c', 'ctr');
    ok($pc->add('c', 'ctr', 5) === 5, 'add 5');
    ok($pc->sub('c', 'ctr', 2) === 3, 'sub 2');
    ok($pc->add('c', 'ctr') === 4, 'add, by 1 by default');
    $pc->set('c', 'm1', 'v1');
    $pc->set('c', 'm2', "v\x002");
    $pc->del('c', 'gone');
    $pc->set('c', '42', 'num');
    $m = $pc->mget('c', ['m1', 'gone', 'm2', '42']);
    ok($m === ['m1' => 'v1', 'gone' => null, 'm2' => "v\x002", 42 => 'num'], 'mget: values, a miss as null, a NUL kept, a numeric key');
    ok($pc->mget('c', []) === [], 'mget of nothing');
    $ks = $pc->keys('c', 'm*');
    sort($ks);
    ok($ks === ['m1', 'm2'], 'keys with a match');
    ok(count($pc->keys('c', null, 1)) === 1, 'keys with a limit');
    $big = random_bytes(200000);
    $pc->set('c', 'big', $big);
    ok($pc->get('c', 'big') === $big, 'a 200,000-byte value');
    $last = random_bytes(262080 - 28 - 4);
    $pc->set('c', 'last', $last);
    ok($pc->get('c', 'last') === $last, 'the largest value a record holds');
    throws(fn() => $pc->set('c', 'huge', str_repeat('x', 263000)), 'PerfcachedException', 'a value over the record limit is refused');
    ok($pc->get('c', 'huge') === null, 'and nothing was stored');
    $pc->del('c', 'h');
    ok($pc->hset('c', 'h', 'f1', "a\x00b") === true, 'hset: a new field');
    ok($pc->hset('c', 'h', 'f1', 'b') === false, 'hset: an update');
    ok($pc->hsetnx('c', 'h', 'f1', 'c') === false, 'hsetnx: the field exists');
    ok($pc->hsetnx('c', 'h', 'f2', "x\x00") === true, 'hsetnx: set');
    ok($pc->hget('c', 'h', 'f1') === 'b', 'hget');
    ok($pc->hget('c', 'h', 'f2') === "x\x00", 'hget: a NUL kept');
    ok($pc->hget('c', 'h', 'none') === null, 'hget: absent is null');
    ok($pc->hexists('c', 'h', 'f1') === true && $pc->hexists('c', 'h', 'none') === false, 'hexists');
    ok($pc->hlen('c', 'h') === 2, 'hlen');
    ok($pc->hincrby('c', 'h', 'n', 7) === 7 && $pc->hincrby('c', 'h', 'n') === 8, 'hincrby');
    ok($pc->hgetall('c', 'h') === ['f1' => 'b', 'f2' => "x\x00", 'n' => '8'], 'hgetall');
    ok($pc->hdel('c', 'h', 'f1') === true && $pc->hdel('c', 'h', 'f1') === false, 'hdel');
    throws(fn() => $pc->get('nosuchcol', 'k'), 'PerfcachedException', 'no such collection throws');
    throws(fn() => $pc->ttl('nosuchcol', 'k'), 'PerfcachedException', 'and from ttl, whose -1 is also an answer');
    ok($pc->ttl('c', 'bin') === -1, 'a -1 after that error is still "no expiry", no exception');
    ok($pc->get('c', 'bin') === $bin, 'the handle works after a refused call');
    throws(fn() => $pc->get('c', "a\x00b"), 'ValueError', 'a NUL in a key');
    throws(fn() => $pc->mget('c', ['ok', "a\x00b"]), 'ValueError', 'a NUL in an mget key');
    ok($pc->activeNode() === 0 && $pc->failovers() === 0 && $pc->members() === [] && $pc->routing() === false, 'standalone: no fleet to report');
    $pc->maintain();
    $pc->close();
    throws(fn() => $pc->ping(), 'PerfcachedException', 'a closed handle throws', 9);
    $pc->close();
}

echo "--- encrypted by default\n";
$plain = function (Perfcached $h): int { return (int)($h->stats()['native']['plain']['conns'] ?? -1); };
$d = new Perfcached('127.0.0.1', $np, ['secrets' => [$sec]]);
$d->ping();
ok($plain($d) === 0, 'every connection so far stayed encrypted past the handshake: native.plain.conns 0');
$e = new Perfcached('127.0.0.1', $np, ['secrets' => [$sec], 'encrypt' => false]);
$e->set('c', 'e', 'v');
ok($e->get('c', 'e') === 'v', 'encrypt => false works');
for ($i = 0; $i < 30 && $plain($d) < 1; $i++) usleep(100000);
ok($plain($d) >= 1, 'and it dropped to plaintext where the node allows it: native.plain.conns ' . $plain($d));

echo "--- connecting\n";
$rot = new Perfcached('127.0.0.1', $np, ['secrets' => ['wrong-first', $sec]]);
$rot->ping();
ok(true, 'rotation: the second secret wins');
ok($rot->get('c', 'bin') === $bin, 'and carries binary');
throws(fn() => new Perfcached('127.0.0.1', $np, ['secrets' => ['wrong']]), 'PerfcachedAuthException', 'a wrong secret');
throws(fn() => (new Perfcached('127.0.0.1', $np))->ping(), 'PerfcachedException', 'no secret on a Noise-only door: refused at the first request');
throws(fn() => new Perfcached('127.0.0.1', 1), 'PerfcachedTransportException', 'nothing listening');
throws(fn() => new Perfcached('127.0.0.1', $np, ['secret' => $sec]), 'ValueError', 'an unknown option');
throws(fn() => new Perfcached('127.0.0.1', $np, ['secrets' => 42]), 'ValueError', 'secrets of the wrong type');
throws(fn() => clone $rot, 'Error', 'a handle cannot be cloned');
throws(fn() => serialize($rot), 'Exception', 'nor serialized');
ok(Perfcached::VERSION !== '' && Perfcached::LIBPERFD !== '', 'VERSION ' . Perfcached::VERSION . ', LIBPERFD ' . Perfcached::LIBPERFD);
exit($fails ? 1 : 0);
