<?php
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Yury Kirsanov - see ../../LICENSE

namespace Perfcached\Symfony;

use Psr\Log\LogLevel;
use Symfony\Component\Cache\Adapter\AbstractAdapter;
use Symfony\Component\Cache\Marshaller\DefaultMarshaller;
use Symfony\Component\Cache\Marshaller\MarshallerInterface;

/**
 * A Symfony cache pool on a perfcached collection.  Items are marshalled as for
 * RedisAdapter.  A value over a record's limit is not stored: Symfony would read
 * that as a later miss and run the callback every time, so it is logged as an error.
 */
class PerfcachedAdapter extends AbstractAdapter
{
    /** The largest record, less its 28-byte header: key and value together fit this */
    public const MAX_KEY_AND_VALUE = 262052;

    private \Perfcached $client;
    private string $collection;
    private MarshallerInterface $marshaller;

    public function __construct(\Perfcached $client, string $collection, string $namespace = '',
        int $defaultLifetime = 0, ?MarshallerInterface $marshaller = null)
    {
        $this->client = $client;
        $this->collection = $collection;
        $this->marshaller = $marshaller ?? new DefaultMarshaller();
        parent::__construct($namespace, $defaultLifetime);
    }

    protected function doFetch(array $ids): iterable
    {
        if (!$ids) {
            return [];
        }
        try {
            $got = $this->client->mget($this->collection, array_values($ids));
        } catch (\PerfcachedRefusedException $e) {
            // a node did not answer in time, which is no miss: once more
            $got = $this->client->mget($this->collection, array_values($ids));
        }
        $out = [];
        foreach ($got as $id => $value) {
            if ($value !== null) {
                $out[$id] = $this->marshaller->unmarshall($value);
            }
        }
        return $out;
    }

    protected function doHave(string $id): bool
    {
        return $this->client->exists($this->collection, $id);
    }

    protected function doClear(string $namespace): bool
    {
        do {
            $ids = $this->client->keys($this->collection, $namespace . '*');
            foreach ($ids as $id) {
                $this->client->del($this->collection, $id);
            }
        } while ($ids);
        return true;
    }

    protected function doDelete(array $ids): bool
    {
        foreach ($ids as $id) {
            $this->client->del($this->collection, $id);
        }
        return true;
    }

    protected function doSave(array $values, int $lifetime): array|bool
    {
        $failed = [];
        $values = $this->marshaller->marshall($values, $failed);
        foreach ($values as $id => $value) {
            $size = strlen($id) + strlen($value);
            if ($size > self::MAX_KEY_AND_VALUE) {
                $this->tooBig($id, $size);
                $failed[] = $id;
                continue;
            }
            try {
                $this->client->set($this->collection, $id, $value, max(0, $lifetime));
            } catch (\PerfcachedException $e) {
                $this->log(LogLevel::ERROR, sprintf('perfcached: key "%s" of %d bytes not stored: %s',
                    $id, $size, $e->getMessage()));
                $failed[] = $id;
            }
        }
        return $failed ?: true;
    }

    private function tooBig(string $id, int $size): void
    {
        $this->log(LogLevel::ERROR, sprintf('perfcached: key "%s" of %d bytes not stored: over the %d a record holds,'
            . ' so every read of it is a miss - split the value', $id, $size, self::MAX_KEY_AND_VALUE));
    }

    // no logger set is no reason to go unheard: this one is a bug in the caller
    private function log(string $level, string $message): void
    {
        if (isset($this->logger)) {
            $this->logger->log($level, $message);
        } else {
            error_log($message);
        }
    }
}
