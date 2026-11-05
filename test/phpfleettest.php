<?php
// phpfleettest.php - PE-4, see phpfleettest.sh.  Usage: <arm> <secret> ...
$arm = $argv[1];
$sec = $argv[2];
$fails = 0;
function ok(bool $c, string $t): void {
    global $fails;
    echo $c ? "  ok   $t\n" : "  FAIL $t\n";
    if (!$c) $fails++;
}
$opts = ['secrets' => [$sec], 'io_timeout_ms' => 2000, 'connect_timeout_ms' => 2000];

if ($arm === 'failover') {
    $pids = ["127.0.163.1" => $argv[3], "127.0.163.2" => $argv[4], "127.0.163.3" => $argv[5]];
    $pc = new Perfcached('127.0.163.1', 19591, $opts);
    for ($i = 0; $i < 200; $i++) {
        $pc->set('c', "f$i", "v$i");
    }
    // eager: wait for the copies, or a node's death loses what only it held
    $t0 = microtime(true);
    do {
        usleep(200000);
        $st = json_decode(@file_get_contents('http://127.0.163.2:19595/stats'), true);
        $n = 0;
        foreach (($st['collections'] ?? []) as $c) if ($c['name'] === 'c') $n = $c['entries'];
    } while ($n < 200 && microtime(true) - $t0 < 20);
    ok($n >= 200, "200 keys copied across the fleet ($n on node 2)");
    $m = $pc->members();
    ok(count($m) === 3, 'the handle learned three members');
    $victim = null;
    foreach ($m as $row) if ($row['active']) $victim = $row;
    ok($victim !== null, 'and works through ' . ($victim['addr'] ?? 'none'));
    $pid = $pids[$victim['addr'] ?? ''] ?? null;
    ok($pid !== null, "that node is process $pid");
    if ($pid === null) exit(1);      // kill -9 0 would take this test's whole process group
    exec('kill -9 ' . (int)$pid);
    $reads = $wrong = 0; $errs = [];
    $t0 = microtime(true);
    while (microtime(true) - $t0 < 6) {
        $i = $reads % 200;
        try {
            if ($pc->get('c', "f$i") !== "v$i") $wrong++;
        } catch (\Throwable $e) {
            $errs[] = get_class($e) . ': ' . $e->getMessage();
        }
        $reads++;
    }
    ok(!$errs, "$reads reads across the kill, " . count($errs) . ' exceptions' . ($errs ? ' - first: ' . substr($errs[0], 0, 100) : ''));
    ok($wrong === 0, "$wrong wrong answers");
    ok($pc->failovers() >= 1, 'the handle counts ' . $pc->failovers() . ' failover(s)');
    $now = null;
    foreach ($pc->members() as $row) if ($row['active']) $now = $row['addr'];
    ok($now !== null && $now !== $victim['addr'], "and now works through $now");
}

if ($arm === 'fork') {
    if (!function_exists('pcntl_fork')) {
        ok(false, 'no pcntl in this PHP');
        exit(1);
    }
    $pc = new Perfcached('127.0.163.1', 19591, $opts);
    $pc->set('c', 'before', 'x');
    $pool0 = Perfcached::pool();
    $f0 = $pc->failovers();
    $res = tempnam(sys_get_temp_dir(), 'pcf');
    $pid = pcntl_fork();
    if ($pid === 0) {
        $bad = 0; $err = '';
        try {
            for ($i = 0; $i < 500; $i++) {
                $pc->set('c', "child$i", "c$i");
                if ($pc->get('c', "child$i") !== "c$i") $bad++;
            }
        } catch (\Throwable $e) {
            $err = get_class($e) . ': ' . $e->getMessage();
        }
        file_put_contents($res, json_encode(['bad' => $bad, 'err' => $err, 'pool' => Perfcached::pool(), 'fo' => $pc->failovers()]));
        posix_kill(posix_getpid(), SIGKILL);     // no shutdown: only the handle's own behaviour is tested
    }
    $bad = 0; $err = '';
    try {
        for ($i = 0; $i < 500; $i++) {
            $pc->set('c', "parent$i", "p$i");
            if ($pc->get('c', "parent$i") !== "p$i") $bad++;
        }
    } catch (\Throwable $e) {
        $err = get_class($e) . ': ' . $e->getMessage();
    }
    pcntl_waitpid($pid, $st);
    $c = json_decode((string)file_get_contents($res), true);
    @unlink($res);
    ok($err === '' && $bad === 0, "the parent's 500 writes and reads on its handle: $bad wrong" . ($err ? ", $err" : ''));
    ok(is_array($c) && $c['err'] === '' && $c['bad'] === 0, "the child's 500, on the inherited object: " . ($c['bad'] ?? '?') . ' wrong' . (($c['err'] ?? '') ? ", {$c['err']}" : ''));
    ok(($c['pool']['forks'] ?? 0) === 1 && ($c['pool']['dials'] ?? 0) === $pool0['dials'] + 1, 'the child dialled its own, once (forks ' . ($c['pool']['forks'] ?? '?') . ')');
    ok(Perfcached::pool()['forks'] === 0 && Perfcached::pool()['dials'] === $pool0['dials'], 'the parent did not re-dial');
    // a shared channel breaks and the handle hides it by failing over, so the answers alone prove nothing
    ok($pc->failovers() === $f0 && ($c['fo'] ?? -1) === 0, 'neither channel broke: failovers, parent ' . ($pc->failovers() - $f0) . ', child ' . ($c['fo'] ?? '?'));
}

if ($arm === 'seeds') {
    [$n1, $n2, $n3] = ['127.0.163.1', '127.0.163.2', '127.0.163.3'];
    $single = $opts + ['persistent' => false];
    $pc = new Perfcached([$n1, $n2, $n3], 19591, $opts);
    $pc->set('c', 'seed', 'x');
    $p0 = Perfcached::pool();
    $again = new Perfcached([$n1, $n2, $n3], 19591, $opts);
    $p1 = Perfcached::pool();
    ok($p1['handles'] === $p0['handles'] && $p1['dials'] === $p0['dials'] && $p1['reused'] === $p0['reused'] + 1, 'the same list takes up the same handle');
    foreach (['empty' => [], 'not strings' => [1], 'a NUL' => ["a\0b"], 'too many' => array_fill(0, 17, $n1)] as $what => $list) {
        try {
            new Perfcached($list, 19591, $single);
            ok(false, "a list with $what is refused");
        } catch (\ValueError $e) {
            ok(true, "a list with $what is refused");
        }
    }
    $f0 = Perfcached::pool()['seeds_failed'];
    try {
        new Perfcached([$n2, $n3], 19591, ['secrets' => ['not-the-secret']] + $single);
        ok(false, 'a wrong secret fails');
    } catch (PerfcachedAuthException $e) {
        ok(Perfcached::pool()['seeds_failed'] === $f0 + 1, 'a wrong secret stops the walk at its first seed');
    }
    $t0 = microtime(true);
    do {
        usleep(100000);
        $st = json_decode(@file_get_contents('http://127.0.163.2:19595/stats'), true);
        $n = 0;
        foreach (($st['collections'] ?? []) as $c) if ($c['name'] === 'c') $n = $c['entries'];
    } while ($n < 1 && microtime(true) - $t0 < 20);
    ok($n >= 1, 'the key is on node 2');
    exec('kill -9 ' . (int)$argv[3]);
    usleep(300000);
    // the dead seed first in one and last in the other: whichever the pid starts at, one walk crosses it
    $f0 = Perfcached::pool()['seeds_failed'];
    foreach ([[$n1, $n2], [$n2, $n1]] as $list) {
        try {
            $h = new Perfcached($list, 19591, $single);
            ok($h->get('c', 'seed') === 'x', 'the list ' . implode(',', $list) . ' answers with node 1 dead');
        } catch (\Throwable $e) {
            ok(false, 'the list ' . implode(',', $list) . ' with node 1 dead: ' . substr($e->getMessage(), 0, 100));
        }
    }
    ok(Perfcached::pool()['seeds_failed'] === $f0 + 1, 'one of the two walks crossed the dead seed, once');
    try {
        new Perfcached($n1, 19591, $single);
        ok(false, 'control: the dead node alone fails');
    } catch (PerfcachedTransportException $e) {
        ok(true, 'control: the dead node alone fails');
    }
    // S476: node 2 frozen, its kernel still accepting: the walk passes it in a short wait, not the whole timeout
    exec('kill -STOP ' . (int)$argv[4]);
    foreach ([[$n2, $n3], [$n3, $n2]] as $list) {
        $t0 = microtime(true);
        try {
            $h = new Perfcached($list, 19591, $single);
            $h->ping();
            $ms = (int)((microtime(true) - $t0) * 1000);
            ok($ms < 1500, 'the list ' . implode(',', $list) . " answers with node 2 frozen, in $ms ms");
        } catch (\Throwable $e) {
            ok(false, 'the list ' . implode(',', $list) . ' with node 2 frozen: ' . substr($e->getMessage(), 0, 100));
        }
    }
    exec('kill -CONT ' . (int)$argv[4]);
    exec('kill -9 ' . (int)$argv[4] . ' ' . (int)$argv[5]);
    usleep(300000);
    $f0 = Perfcached::pool()['seeds_failed'];
    try {
        new Perfcached([$n1, $n2, $n3], 19591, $single);
        ok(false, 'every seed dead fails');
    } catch (PerfcachedTransportException $e) {
        ok(Perfcached::pool()['seeds_failed'] === $f0 + 6, 'every seed dead fails, having tried all three, twice');
    }
}

if ($arm === 'owners') {
    $file = $argv[3];
    $pc = new Perfcached('127.0.163.1', 19591, $opts + ['route_keys' => true, 'refresh_ms' => 300]);
    // the list can be short just after the fleet forms (2 of 3, GitLab 112808); a refresh fills it
    $t0 = microtime(true);
    do {
        usleep(100000);
        $pc->maintain();
        $pc->ping();
    } while (count($pc->members()) < 3 && microtime(true) - $t0 < 20);
    ok(count($pc->members()) === 3, 'the handle learned three members in ' . round(microtime(true) - $t0, 1) . ' s');
    ok($pc->routing() === true, 'the handle routes by key on a shard fleet');
    $by = [];
    $none = 0;
    for ($i = 0; $i < 600; $i++) {
        $k = "o$i";
        $pc->set('c', $k, 'v');
        $n = $pc->ownerOf('c', $k);
        if ($n === null) $none++; else $by[$n] = ($by[$n] ?? 0) + 1;
    }
    ok($none === 0 && count($by) === 3, '600 keys: an owner for each, over ' . count($by) . ' nodes');
    usleep(500000);
    $lines = '';
    foreach ($pc->members() as $row) {
        $lines .= $row['node'] . ' ' . $row['addr'] . ' ' . ($by[$row['node']] ?? 0) . "\n";
    }
    file_put_contents($file, $lines);
}
exit($fails ? 1 : 0);
