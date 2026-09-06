<?php

namespace App\MessageHandler;

use App\Message\ProbeMessage;
use App\Probe\Gate;
use Symfony\Component\Messenger\Attribute\AsMessageHandler;

#[AsMessageHandler]
final class ProbeMessageHandler
{
    public function __construct(private readonly Gate $gate)
    {
    }

    public function __invoke(ProbeMessage $message): string
    {
        $released = $this->gate->wait($message->gate, $message->participant);

        return 'handled:'.$message->value.':'.($released ? 'released' : 'timeout');
    }
}
