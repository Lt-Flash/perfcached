<?php
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Yury Kirsanov - see ../../LICENSE

namespace Perfcached\Symfony;

use Symfony\Component\Lock\Exception\InvalidTtlException;
use Symfony\Component\Lock\Exception\LockConflictedException;
use Symfony\Component\Lock\Exception\LockExpiredException;
use Symfony\Component\Lock\Key;
use Symfony\Component\Lock\PersistingStoreInterface;

/**
 * A Symfony Lock store on a perfcached collection, as RedisStore does it: a lock is a key holding
 * a random token, taken by set-if-absent with a ttl, refreshed and released only while the key
 * still holds that token.  One node decides each key.  A ttl is whole seconds here: a fraction
 * is rounded up and one second added, as the daemon's clock ticks in whole seconds and a ttl of N
 * lasts N-1 to N of them.  For symfony/lock 5.4, 6.4 and 7.
 */
class PerfcachedLockStore implements PersistingStoreInterface
{
    private \Perfcached $client;
    private string $collection;
    private string $prefix;
    private float $initialTtl;

    public function __construct(\Perfcached $client, string $collection, string $prefix = '',
        float $initialTtl = 300.0)
    {
        if ($initialTtl <= 0) {
            throw new InvalidTtlException(sprintf('"%s()" expects a strictly positive TTL, got %g.',
                __METHOD__, $initialTtl));
        }
        $this->client = $client;
        $this->collection = $collection;
        $this->prefix = $prefix;
        $this->initialTtl = $initialTtl;
    }

    public function save(Key $key): void
    {
        $key->reduceLifetime($this->initialTtl);
        $id = $this->id($key);
        $token = $this->token($key);
        $ttl = self::seconds($this->initialTtl);
        // taken now, or already this key's own (a second save by its holder refreshes it)
        if (!$this->client->setNx($this->collection, $id, $token, $ttl) &&
            !$this->client->setIfEq($this->collection, $id, $token, $token, $ttl)) {
            throw new LockConflictedException();
        }
        $this->checkNotExpired($key);
    }

    public function putOffExpiration(Key $key, float $ttl): void
    {
        $key->reduceLifetime($ttl);
        $token = $this->token($key);
        if (!$this->client->setIfEq($this->collection, $this->id($key), $token, $token,
                self::seconds($ttl))) {
            throw new LockConflictedException();
        }
        $this->checkNotExpired($key);
    }

    public function delete(Key $key): void
    {
        $this->client->delIfEq($this->collection, $this->id($key), $this->token($key));
    }

    public function exists(Key $key): bool
    {
        return $this->client->get($this->collection, $this->id($key)) === $this->token($key);
    }

    private function id(Key $key): string
    {
        return $this->prefix . $key;
    }

    private function token(Key $key): string
    {
        if (!$key->hasState(__CLASS__)) {
            $key->setState(__CLASS__, base64_encode(random_bytes(32)));
        }
        return $key->getState(__CLASS__);
    }

    private static function seconds(float $ttl): int
    {
        // a ttl of N lasts N-1..N s on the daemon's whole-second clock: one more, never short
        return max(1, (int)ceil($ttl)) + 1;
    }

    /* as Symfony's own stores: a lock stored past its expiry is let go and reported */
    private function checkNotExpired(Key $key): void
    {
        if ($key->isExpired()) {
            try {
                $this->delete($key);
            } catch (\Exception $e) {
                // the expiry is what is reported
            }
            throw new LockExpiredException(sprintf('Failed to store the "%s" lock.', $key));
        }
    }
}
