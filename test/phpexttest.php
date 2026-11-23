<?php
// phpexttest.php - PE-1: every verb of the extension; see phpexttest.sh.
// Usage: php -d extension=... phpexttest.php <plaintext-port> <noise-port> <secret> <extension .so>
[$_, $pp, $np, $sec] = $argv;
$ext = $argv[4] ?? '';
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
    // the daemon holds 0 .. 2^31-1 s; a negative ttl read as "no expiry" and a larger one wrapped
    $pc->del('c', 'tt');
    $pc->del('c', 'tt2');
    foreach ([-1, -300, 0x80000000, PHP_INT_MAX] as $bad) {
        throws(fn() => $pc->set('c', 'tt', 'v', $bad), 'ValueError', "set with ttl $bad");
        throws(fn() => $pc->setNx('c', 'tt2', 'v', $bad), 'ValueError', "setNx with ttl $bad");
        throws(fn() => $pc->setIfEq('c', 'ctr', '4', '4', $bad), 'ValueError', "setIfEq with ttl $bad");
        throws(fn() => $pc->expire('c', 'ctr', $bad), 'ValueError', "expire with ttl $bad");
    }
    throws(fn() => $pc->add('c', 'ctr', 1, 0x80000000), 'ValueError', 'add with ttl 2^31');
    ok($pc->get('c', 'tt') === null && $pc->get('c', 'tt2') === null && $pc->ttl('c', 'ctr') === -1,
        'and none of them stored or changed anything');
    ok($pc->add('c', 'ctr', 1, -1) === 5, 'add with a negative ttl still keeps the expiry the key has');
    ok($pc->sub('c', 'ctr') === 4, 'sub');
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
    if (!method_exists($pc, 'jget')) {
        ok(false, 'the JSON path methods jget/jset/jdel/jincr exist');
    } else {
        $pc->del('c', 'doc');
        $pc->jset('c', 'doc', '$', '{"n":1,"s":"x"}');
        ok($pc->jget('c', 'doc') === '{"n":1,"s":"x"}', 'jset and jget: the whole document, "$" by default');
        ok($pc->jincr('c', 'doc', '$.n', 4) === 5 && $pc->jincr('c', 'doc', '$.n') === 6, 'jincr: by 4, then by the default 1');
        ok($pc->jget('c', 'doc', '$.s') === '"x"', 'jget: a fragment comes back as JSON text');
        ok($pc->jdel('c', 'doc', '$.s') === true && $pc->jdel('c', 'doc', '$.s') === false, 'jdel: a path, then absent');
        $pc->jset('c', 'doc', '$.a', '[1,2]');
        ok($pc->jget('c', 'doc', '$.a') === '[1,2]' && json_decode($pc->jget('c', 'doc'), true) === ['n' => 6, 'a' => [1, 2]], 'jset under a path');
        ok($pc->jget('c', 'nodoc') === null, 'jget: a miss is null');
        ok($pc->jdel('c', 'doc') === true && $pc->jget('c', 'doc') === null, 'jdel with no path removes the document');
        $pc->jset('c', 'tdoc', '$', '{"a":1}', 500);
        $t = $pc->ttl('c', 'tdoc');
        ok($t > 400 && $t <= 500, "jset with a ttl: $t");
        throws(fn() => $pc->jset('c', 'doc2', '$', '{not json'), 'PerfcachedException', 'jset: text that is not JSON');
        throws(fn() => $pc->jget('c', "a\x00b"), 'ValueError', 'a NUL in a jget key');
        throws(fn() => $pc->jset('c', 'doc', '$', "{\"a\":\"\x00\"}"), 'ValueError', 'a NUL in jset text');
    }
    if (!method_exists($pc, 'setNx')) {
        ok(false, 'the lock and publish methods setNx/setIfEq/delIfEq/publish exist');
    } else {
        $pc->del('c', 'lk');
        ok($pc->setNx('c', 'lk', 'tok1', 500) === true, 'setNx: absent, so set');
        ok($pc->setNx('c', 'lk', 'tok2', 500) === false && $pc->get('c', 'lk') === 'tok1', 'setNx: present, so declined and the value kept');
        $t = $pc->ttl('c', 'lk');
        ok($t > 400 && $t <= 500, "setNx with a ttl: $t");
        ok($pc->setIfEq('c', 'lk', 'tok1', 'tok1', 900) === true && $pc->ttl('c', 'lk') > 800, 'setIfEq with its own token: refreshed, the ttl moved');
        ok($pc->setIfEq('c', 'lk', 'tok2', 'other', 900) === false && $pc->get('c', 'lk') === 'tok1', 'setIfEq: no match, nothing changed');
        ok($pc->delIfEq('c', 'lk', 'other') === false && $pc->exists('c', 'lk') === true, 'delIfEq: no match, kept');
        ok($pc->delIfEq('c', 'lk', 'tok1') === true && $pc->exists('c', 'lk') === false, 'delIfEq: the token matches, deleted');
        ok($pc->setIfEq('c', 'lk', 'x', 'tok1') === false && $pc->exists('c', 'lk') === false, 'setIfEq on an absent key: false, nothing set');
        ok($pc->delIfEq('c', 'lk', 'tok1') === false, 'delIfEq on an absent key: false');
        ok($pc->setNx('c', 'lk', "b\x00in") === true && $pc->delIfEq('c', 'lk', "b\x00i") === false && $pc->delIfEq('c', 'lk', "b\x00in") === true,
            'a token with a NUL byte compares byte-exact');
        ok($pc->publish('php.chan', "p\x00y") === 0, 'publish with nobody subscribed: 0 receivers');
        throws(fn() => $pc->publish("a\x00b", 'x'), 'ValueError', 'a NUL in a channel name');
    }
    if (!method_exists($pc, 'subscribe')) {
        ok(false, 'subscribe and psubscribe exist');
    } else {
        // a publisher of its own: it repeats its first message until a subscriber counts it
        $pub = function (string $first, string $rest) use ($port, $opts, $ext) {
            $src = '$p = new Perfcached("127.0.0.1", ' . $port . ', ' . var_export($opts, true) . ');'
                . ' for ($i = 0; $i < 200 && $p->publish("' . $first . '", "hello") < 1; $i++) usleep(50000); ' . $rest;
            return proc_open([PHP_BINARY, '-n', '-d', "extension=$ext", '-r', $src],
                [1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
        };
        $c1 = $pub('php.sub', 'foreach (["m1", "m2", "stop"] as $m) $p->publish("php.sub", $m);');
        $got = [];
        $n = $pc->subscribe(['php.sub', 'php.other'], function ($h, $ch, $msg) use (&$got, $pc) {
            $got[] = ($h === $pc ? '' : 'NOT-THE-HANDLE ') . "$ch:$msg";
            return $msg !== 'stop';
        }, 15);
        proc_close($c1);
        ok($got === ['php.sub:hello', 'php.sub:m1', 'php.sub:m2', 'php.sub:stop'] && $n === 4,
            "subscribe: each message in order, the handle first, the callback's false ends it: " . implode(' ', $got));
        $c2 = $pub('php.pat.a', '$p->publish("php.pat.b", "stop");');
        $pg = [];
        $n = $pc->psubscribe(['php.pat.*'], function ($h, $pat, $ch, $msg) use (&$pg) {
            $pg[] = "$pat $ch $msg";
            return $msg !== 'stop';
        }, 15);
        proc_close($c2);
        ok($pg === ['php.pat.* php.pat.a hello', 'php.pat.* php.pat.b stop'] && $n === 2,
            'psubscribe: pattern, channel and message: ' . implode(' | ', $pg));
        $t0 = microtime(true);
        $n = $pc->subscribe(['php.none'], fn() => true, 0.5);
        $dt = microtime(true) - $t0;
        ok($n === 0 && $dt >= 0.45 && $dt < 5, sprintf('subscribe with a timeout and no message: 0 after %.2f s', $dt));
        ok($pc->get('c', 'bin') === $bin, 'the handle works after a subscription');
        // the node drops a subscription when it sees its connection close: asked again, briefly
        for ($i = 0; $i < 40 && ($r = $pc->publish('php.none', 'x')) !== 0; $i++) usleep(50000);
        ok($r === 0, "the subscription went with its connection: publish answers $r after " . ($i * 50) . ' ms');
        $c3 = $pub('php.ex', '');
        $threw = false;
        try {
            $pc->subscribe(['php.ex'], function () { throw new RuntimeException('from the callback'); }, 15);
        } catch (RuntimeException $e) {
            $threw = $e->getMessage() === 'from the callback';
        }
        proc_close($c3);
        ok($threw, 'an exception in the callback ends the loop and reaches the caller');
        throws(fn() => $pc->subscribe(["a\x00b"], fn() => true, 0.1), 'ValueError', 'a NUL in a channel name');
        throws(fn() => $pc->subscribe([], fn() => true, 0.1), 'ValueError', 'no channel');
    }
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
// subscribe dials a connection of its own: it walks the secrets as the handle's dial does
try {
    $n = $rot->subscribe(['php.rot'], fn() => true, 0.3);
    ok($n === 0, "rotation: subscribe connects with the second secret too ($n delivered)");
} catch (\Throwable $e) {
    ok(false, 'rotation: subscribe connects with the second secret too (' . get_class($e) . ': '
        . substr($e->getMessage(), 0, 60) . ')');
}
throws(fn() => new Perfcached('127.0.0.1', $np, ['secrets' => ['wrong']]), 'PerfcachedAuthException', 'a wrong secret');
throws(fn() => (new Perfcached('127.0.0.1', $np))->ping(), 'PerfcachedException', 'no secret on a Noise-only door: refused at the first request');
throws(fn() => new Perfcached('127.0.0.1', 1), 'PerfcachedTransportException', 'nothing listening');
throws(fn() => new Perfcached('127.0.0.1', $np, ['secret' => $sec]), 'ValueError', 'an unknown option');
throws(fn() => new Perfcached('127.0.0.1', $np, ['secrets' => 42]), 'ValueError', 'secrets of the wrong type');
throws(fn() => new Perfcached('127.0.0.1', $np, ['secrets' => "a\x00b"]), 'ValueError', 'S499: a NUL in a secret');
throws(fn() => new Perfcached('127.0.0.1', $np, ['secrets' => ['ok', "a\x00b"]]), 'ValueError', 'S499: a NUL in one of the secrets');
throws(fn() => clone $rot, 'Error', 'a handle cannot be cloned');
throws(fn() => serialize($rot), 'Exception', 'nor serialized');
ok(Perfcached::VERSION !== '' && Perfcached::LIBPERFD !== '', 'VERSION ' . Perfcached::VERSION . ', LIBPERFD ' . Perfcached::LIBPERFD);
exit($fails ? 1 : 0);
