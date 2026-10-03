<?php

namespace App\Probe;

use Symfony\Component\Validator\Constraints as Assert;

final class ProbeInput
{
    #[Assert\NotBlank]
    public ?string $name = null;
}
