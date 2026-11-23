<?php
// phpsymfonytest.php - PE-3, see phpsymfonytest.sh.
[$_, $autoload, $adapterFile, $port, $sec, $phpLog] = $argv;
require $autoload;
require $adapterFile;

use Perfcached\Symfony\PerfcachedAdapter;
use Psr\Log\AbstractLogger;
use Symfony\Component\Cache\Exception\InvalidArgumentException;
use Symfony\Contracts\Cache\ItemInterface;

$fails = 0; $n = 0;
function ok(bool $c, string $t): void {
    global $fails, $n;
    $n++;
    echo $c ? "  ok   $t\n" : "  FAIL $t\n";
    if (!$c) $fails++;
}
class Rec extends AbstractLogger {
    public array $lines = [];
    public function log($level, $message, array $context = []): void { $this->lines[] = "$level $message"; }
}

$pc = new Perfcached('127.0.0.1', (int)$port, ['secrets' => [$sec]]);
$cache = new PerfcachedAdapter($pc, 'c', 'billing-test', 600);
$cache->clear();

// the shape the site's services use: get() with a callback, getItem/save, delete
$runs = 0;
$fn = function (ItemInterface $i) use (&$runs) { $runs++; $i->expiresAfter(300); return ['rates' => [1, 2.5, "x\0y"], 'n' => $runs]; };
$a = $cache->get('rates.au', $fn);
$b = $cache->get('rates.au', $fn);
ok($runs === 1 && $a === $b && $a['rates'][2] === "x\0y", 'get(): the callback once, then the stored value, an array with a NUL in it');
$item = $cache->getItem('article.1');
ok(!$item->isHit(), 'getItem(): a miss');
$item->set('body')->expiresAfter(120);
ok($cache->save($item) === true, 'save()');
$item = $cache->getItem('article.1');
ok($item->isHit() && $item->get() === 'body', 'getItem(): a hit');
ok($cache->hasItem('article.1') === true && $cache->hasItem('none') === false, 'hasItem()');
$ttl = $pc->ttl('c', array_values(array_filter($pc->keys('c', 'billing-test*'), fn($k) => str_contains($k, 'article.1')))[0]);
ok($ttl > 60 && $ttl <= 120, "the lifetime reaches the daemon ($ttl s)");
$items = iterator_to_array($cache->getItems(['article.1', 'none', 'rates.au']));
ok($items['article.1']->isHit() && !$items['none']->isHit() && $items['rates.au']->isHit(), 'getItems(): hits and a miss, by mget');
ok($cache->deleteItem('article.1') === true && !$cache->getItem('article.1')->isHit(), 'deleteItem()');
foreach ([1, 2, 3] as $i) { $it = $cache->getItem("d$i")->set($i); $cache->saveDeferred($it); }
ok($cache->commit() === true && $cache->getItem('d2')->get() === 2, 'saveDeferred() and commit()');
$other = new PerfcachedAdapter($pc, 'c', 'other-test', 600);
$o = $other->getItem('keep')->set('kept'); $other->save($o);
ok($cache->clear() === true && !$cache->hasItem('d2') && !$cache->hasItem('rates.au'), 'clear() empties the pool');
ok($other->getItem('keep')->get() === 'kept', 'and leaves another namespace alone');
$big = new stdClass; $big->rows = array_fill(0, 3, str_repeat('r', 40000));
$cache->get('big', fn() => $big);
ok($cache->get('big', fn() => null)->rows[2] === $big->rows[2], 'an object of 120 KB');

// a value past a record
$log = new Rec;
$cache->setLogger($log);
$hits = 0;
$huge = str_repeat('h', 2800000);
$v = $cache->get('articles.all', function () use ($huge, &$hits) { $hits++; return $huge; });
$v2 = $cache->get('articles.all', function () use ($huge, &$hits) { $hits++; return $huge; });
$said = array_values(array_filter($log->lines, fn($l) => str_contains($l, 'not stored') && str_contains($l, 'split the value')));
ok($v === $huge && $hits === 2, 'a 2.8 MB value: not stored, so the callback runs every time (the app still gets its value)');
ok(count($said) >= 1 && str_starts_with($said[0], 'error '), 'and an error is logged, naming it: ' . substr($said[0] ?? 'none', 0, 90));
$c2 = new PerfcachedAdapter($pc, 'c', 'nolog-test', 600);
$c2->get('articles.all', fn() => $huge);
ok(str_contains((string)@file_get_contents($phpLog), 'split the value'), 'with no logger set it goes to the PHP error log');
$edge = str_repeat('e', 262052 - 200);
$c2->get('edge', fn() => $edge);
ok($c2->get('edge', fn() => 'miss') === $edge, 'a value just under the limit is stored');
$c2->clear(); $other->clear();
// S498: a namespace holding a glob character clears its own keys only
$star = new PerfcachedAdapter($pc, 'c', 'ns*', 600);
$near = new PerfcachedAdapter($pc, 'c', 'nsx', 600);
$star->get('k', fn() => 'mine'); $near->get('k', fn() => 'theirs');
ok($star->clear() === true && !$star->hasItem('k') && $near->get('k', fn() => 'lost') === 'theirs',
    "clear() on namespace 'ns*' leaves 'nsx''s keys");
$near->clear();
// the lock store: Symfony Lock on a collection, as RedisStore does it
$storeFile = dirname($adapterFile) . '/PerfcachedLockStore.php';
if (!is_file($storeFile) || !interface_exists('Symfony\Component\Lock\PersistingStoreInterface')) {
    ok(false, 'the lock store ' . basename($storeFile) . ' and symfony/lock are there');
} else {
    require $storeFile;
    // the daemon counts whole seconds and expires a record at exp <= now: a ttl of N lasts N-1..N s,
    // so the store must ask one second more than the lock's lifetime, never less
    $toSec = new ReflectionMethod('Perfcached\Symfony\PerfcachedLockStore', 'seconds');
    if (PHP_VERSION_ID < 80100) $toSec->setAccessible(true);
    $asked = array_map(fn($t) => $toSec->invoke(null, $t), [30.0, 1.5, 0.01, 1.0]);
    ok($asked === [31, 3, 2, 2], 'a lifetime of 30, 1.5, 0.01 and 1 s is asked of the daemon as ' . implode(', ', $asked)
        . ' s (31, 3, 2, 2): the key outlives the lock');
    $f = new \Symfony\Component\Lock\LockFactory(new \Perfcached\Symfony\PerfcachedLockStore($pc, 'c', 'lock-test:'));
    $a = $f->createLock('job', 30);
    $b = $f->createLock('job', 30);
    ok($a->acquire() === true, 'a lock is acquired');
    ok($b->acquire() === false && $a->isAcquired() === true && $b->isAcquired() === false, 'a second lock on the same resource is not, and isAcquired says which holds it');
    $t = $pc->ttl('c', 'lock-test:job');
    ok($t > 20 && $t <= 31, "the key carries the lock's ttl: $t");
    $a->refresh(60);
    $t = $pc->ttl('c', 'lock-test:job');
    ok($t > 50 && $t <= 61, "refresh moved it: $t");
    $a->release();
    ok($a->isAcquired() === false && $b->acquire() === true, 'released, the second acquires it');
    ok($a->acquire() === false, 'and the first cannot');
    $b->release();
    $e = $f->createLock('short', 1, false);
    ok($e->acquire() === true, 'a lock with a ttl of 1 s');
    sleep(3);
    $g = $f->createLock('short', 30);
    ok($g->acquire() === true, 'expired, another acquires it');
    $threw = false;
    try { $e->refresh(); } catch (\Symfony\Component\Lock\Exception\LockConflictedException $x) { $threw = true; }
    ok($threw, 'and the expired one cannot refresh it');
    $g->release();
    ok(!$pc->exists('c', 'lock-test:short') && !$pc->exists('c', 'lock-test:job'), 'released locks leave no key behind');
}
echo "phpsymfonytest: " . ($n - $fails) . " passed, $fails failed\n";
exit($fails ? 1 : 0);
