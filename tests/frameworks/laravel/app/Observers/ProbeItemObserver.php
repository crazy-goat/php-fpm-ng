<?php

namespace App\Observers;

use App\Models\Item;
use Illuminate\Support\Facades\Redis;

/**
 * Task 025 fixture: an observer registered per request, the way per-request
 * boot code would register it. Model events dispatch through
 * Model::$dispatcher (a class static); if that static is not isolated, the
 * observer either never fires for this request or fires into another
 * request's listener set. The retrieved() hook writes a record this request
 * can assert on.
 */
final class ProbeItemObserver
{
    public function __construct(
        private readonly string $marker,
    ) {}

    public function retrieved(Item $item): void
    {
        Redis::setex(
            'laravel025:observer:'.$this->marker,
            60,
            json_encode([
                'marker' => $this->marker,
                'item_id' => $item->id,
                'pid' => getmypid(),
            ], JSON_THROW_ON_ERROR),
        );
    }
}
