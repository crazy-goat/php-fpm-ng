<?php

namespace App\Events;

use Illuminate\Broadcasting\Channel;
use Illuminate\Contracts\Broadcasting\ShouldBroadcastNow;

final class ProbeBroadcast implements ShouldBroadcastNow
{
    public function __construct(public readonly string $marker)
    {
    }

    public function broadcastOn(): array
    {
        return [new Channel('laravel025.probe.'.$this->marker)];
    }

    public function broadcastAs(): string
    {
        return 'laravel025.probe';
    }

    public function broadcastWith(): array
    {
        return ['marker' => $this->marker];
    }
}
