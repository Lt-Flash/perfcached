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
echo "phpsymfonytest: " . ($n - $fails) . " passed, $fails failed\n";
exit($fails ? 1 : 0);
