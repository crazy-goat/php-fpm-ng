<?php

return [
    'driver' => env('HASHING_DRIVER', 'bcrypt'),
    'bcrypt' => [
        'rounds' => 4,
        'verify' => true,
        'limit' => 0,
    ],
    'argon' => [
        'memory' => 65536,
        'threads' => 1,
        'time' => 4,
    ],
];
