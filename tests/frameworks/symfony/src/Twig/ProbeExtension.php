<?php

namespace App\Twig;

use App\Probe\Gate;
use Twig\Extension\AbstractExtension;
use Twig\TwigFunction;

final class ProbeExtension extends AbstractExtension
{
    public function __construct(private readonly Gate $gate)
    {
    }

    public function getFunctions(): array
    {
        return [new TwigFunction('probe_gate', $this->wait(...))];
    }

    public function wait(string $name, string $participant): string
    {
        return $this->gate->wait($name, $participant) ? 'released' : 'timeout';
    }
}
