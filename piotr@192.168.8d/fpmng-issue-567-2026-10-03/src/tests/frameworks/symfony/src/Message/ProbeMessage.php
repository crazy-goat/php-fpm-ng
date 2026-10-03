<?php

namespace App\Message;

final readonly class ProbeMessage
{
    public function __construct(
        public string $value,
        public string $gate,
        public string $participant,
    ) {
    }
}
