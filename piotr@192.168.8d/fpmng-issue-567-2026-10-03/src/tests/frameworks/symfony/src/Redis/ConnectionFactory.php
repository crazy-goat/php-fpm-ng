<?php

namespace App\Redis;

use Symfony\Component\Cache\Adapter\RedisAdapter;

final class ConnectionFactory
{
    public function __construct(private readonly string $dsn)
    {
    }

    public function create(): object
    {
        return RedisAdapter::createConnection($this->dsn);
    }
}
