<?php

namespace App\Jobs;

use Illuminate\Support\Facades\DB;
use Illuminate\Support\Facades\Redis;

final class ProbeJob
{
    public function __construct(
        public readonly string $marker,
        public readonly float $sleep,
    ) {
    }

    public function handle(): void
    {
        DB::selectOne('SELECT SLEEP(?) AS slept', [$this->sleep]);

        Redis::setex(
            'laravel025:job:'.$this->marker,
            60,
            json_encode([
                'marker' => $this->marker,
                'app_oid' => spl_object_id(app()),
                'request_oid' => spl_object_id(app('request')),
            ], JSON_THROW_ON_ERROR),
        );
    }
}
