<?php

return [
    'default' => env('BROADCAST_CONNECTION', 'log'),
    'connections' => [
        'log' => [
            'driver' => 'log',
            'channel' => env('LOG_CHANNEL', 'single'),
        ],
        'null' => [
            'driver' => 'null',
        ],
    ],
];
