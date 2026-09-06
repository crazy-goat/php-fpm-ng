<?php

namespace App\Probe;

use App\Redis\ConnectionFactory;

final class Gate
{
    public function __construct(
        private readonly ConnectionFactory $connections,
        private readonly string $prefix,
    ) {
    }

    public function wait(string $name, string $participant): bool
    {
        if ($name === '') {
            return true;
        }

        $redis = $this->connections->create();
        $ready = $this->prefix.'gate:'.$name.':ready';
        $release = $this->prefix.'gate:'.$name.':release';

        $redis->rpush($ready, $participant);
        $result = $redis->blpop([$release], 30);

        if (method_exists($redis, 'close')) {
            $redis->close();
        } elseif (method_exists($redis, 'disconnect')) {
            $redis->disconnect();
        }

        return is_array($result) && count($result) === 2;
    }
}
