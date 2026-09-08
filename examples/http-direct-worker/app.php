<?php

/*
 * The task 073 POC application: hello world plus one route that sleeps for a
 * second the amphp way. Under pool.type = http-direct with
 * pool.executor = worker and pm.max_children = 1, N concurrent requests to
 * /sleep answer after about one second in total rather than N seconds — that is the entire claim being
 * tested. Replace this closure with a router and it is an application server.
 */

declare(strict_types=1);

require __DIR__ . '/vendor/autoload.php';
require __DIR__ . '/FpmngDriver.php';
require __DIR__ . '/FpmngServer.php';

use Fpmng\Poc\FpmngServer;

use function Amp\delay;

(new FpmngServer(function (array $env, string $body): array {
    $path = \parse_url($env['REQUEST_URI'] ?? '/', \PHP_URL_PATH);

    if ($path === '/sleep') {
        $started = \microtime(true);
        // Suspends this request's fiber only. The worker returns to its event
        // loop and serves everyone else meanwhile.
        delay(1.0);

        return [200, ['Content-Type' => 'application/json'], \json_encode([
            'id' => $env['QUERY_STRING'] ?? '',
            'pid' => \getmypid(),
            't0' => $started,
            't1' => \microtime(true),
        ], \JSON_THROW_ON_ERROR)];
    }

    return [200, ['Content-Type' => 'text/plain'], 'hello world from pid ' . \getmypid() . "\n"];
}))->run();
