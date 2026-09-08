<?php

/*
 * Task 075: the same worker-owned event loop as examples/http-direct-worker
 * and examples/http-direct-worker-mysql, driven by ReactPHP instead of Revolt.
 *
 * Every route exists to exercise one thing the Revolt examples cannot:
 *
 *   /mysql   react/mysql over plain TCP. A binary protocol, an asynchronous
 *            connect (FPMNG_WORKER_WRITE) and protocol reads
 *            (FPMNG_WORKER_READ on a socket), all under a loop that is not
 *            Revolt's and an application that uses no fibers at all.
 *   /tls     react/http over HTTPS against the origin service. This is the
 *            direct test of the warning in fpmng_worker_event_create(): the
 *            watcher is armed on the raw descriptor from php_stream_cast(),
 *            and ReactPHP reads once per readable event with no speculative
 *            read (react/stream, DuplexResourceStream::handleData():185-198),
 *            which task 074 recorded as the pattern that "would still hang".
 *            The body is deliberately far above the 64 KiB read chunk and is
 *            rate-limited by the origin, so a stranded TLS buffer is
 *            reachable rather than nominal.
 *   /strand  the same TLS question asked in its worst form: ReactPHP's own
 *            read path at a 1 KiB chunk — far below one 16 KiB TLS record —
 *            against an unthrottled body. /tls answers "does it work"; this
 *            route answers "and does it still work when the client cannot
 *            possibly drain a record per event".
 *   /ticks   futureTick(). No Revolt analogue, and the only reason
 *            fpmng_worker_loop() takes a $blocking argument. See the route.
 *
 * A connection per request on /mysql, not a pool: react/mysql 0.6 has no pool,
 * and inventing one here would hide the primitive under test. It also means
 * the asynchronous connect is exercised N times per batch instead of once. A
 * real application pools.
 */

declare(strict_types=1);

require __DIR__ . '/vendor/autoload.php';
require __DIR__ . '/FpmngLoop.php';
require __DIR__ . '/FpmngReactServer.php';

use Fpmng\React\FpmngReactServer;
use Psr\Http\Message\ResponseInterface;
use React\EventLoop\Loop;
use React\MySQL\ConnectionInterface;
use React\MySQL\Factory as MysqlFactory;
use React\MySQL\QueryResult;
use React\Promise\Deferred;
use React\Promise\PromiseInterface;
use React\Socket\Connector;
use React\Stream\ReadableResourceStream;
use React\Stream\ReadableStreamInterface;

use function React\Promise\reject;
use function React\Promise\resolve;

const QUERY_TIMEOUT = 10.0;
const TLS_TIMEOUT = 20.0;

// Long enough that a loop blocking in libevent instead of draining its tick
// queue cannot be mistaken for a slow one, short enough that the harness'
// verdict arrives either way.
const FUTURE_TICK_BLOCKER = 10.0;

// How long the /strand probe waits for a read that never comes. Idle time, not
// total time, so it is a diagnosis and not a budget.
const STRAND_IDLE = 3.0;

/*
 * react/mysql 0.6 speaks mysql_native_password only: the authentication packet
 * is a fixed SHA1 scramble with no CLIENT_PLUGIN_AUTH and no caching_sha2
 * support (vendor/react/mysql/src/Commands/AuthenticateCommand.php:78-101).
 * MySQL 8.4 ships that plugin disabled, which is why compose.yaml starts the
 * server with --mysql-native-password=ON and the init script re-creates the
 * application user with it. Nothing to do with the event loop; it is the first
 * thing to check when this route reports an authentication error.
 */
/**
 * Consumes the rejection of a promise whose result nobody waits for. Without
 * this react/promise v3 logs "Unhandled promise rejection" through error_log()
 * for a connection that failed to close cleanly.
 */
function quietly(PromiseInterface $promise): void
{
    $promise->then(null, function (): void {
    });
}

function mysqlUrl(): string
{
    return \sprintf(
        '%s:%s@%s:%s/%s?timeout=%s',
        \rawurlencode(\getenv('MYSQL_USER') ?: 'fpmng'),
        \rawurlencode(\getenv('MYSQL_PASSWORD') ?: 'fpmng'),
        \getenv('MYSQL_HOST') ?: '127.0.0.1',
        \getenv('MYSQL_PORT') ?: '3306',
        \getenv('MYSQL_DATABASE') ?: 'fpmng',
        QUERY_TIMEOUT
    );
}

/**
 * SELECT SLEEP(1) on its own connection. The response is the worker's own
 * clock around the query, which is what the harness uses to prove the N
 * queries overlapped rather than trusting wall time.
 */
function sleepInMysql(string $id): PromiseInterface
{
    $started = \microtime(true);
    $factory = new MysqlFactory();

    return $factory->createConnection(mysqlUrl())->then(
        function (ConnectionInterface $connection) use ($id, $started): PromiseInterface {
            return $connection->query('SELECT SLEEP(1) AS slept')->then(
                function (QueryResult $result) use ($connection, $id, $started): array {
                    // quit() rather than close(): it sends COM_QUIT and lets
                    // the server release its thread, so a batch does not leave
                    // N connections for the server to time out. The response
                    // does not wait for it — but the rejection is still
                    // consumed, because react/promise v3 reports an unhandled
                    // one through error_log(), which would put noise in the
                    // worker log that build/test-*.sh greps.
                    quietly($connection->quit());

                    return [200, ['Content-Type' => 'application/json'], \json_encode([
                        'id' => $id,
                        'route' => 'mysql',
                        'pid' => \getmypid(),
                        'slept' => (int) ($result->resultRows[0]['slept'] ?? -1),
                        't0' => $started,
                        't1' => \microtime(true),
                    ], \JSON_THROW_ON_ERROR)];
                },
                function (\Throwable $e) use ($connection): PromiseInterface {
                    quietly($connection->quit());

                    return reject($e);
                }
            );
        }
    );
}

/**
 * An HTTPS GET, consumed as a stream so the two numbers that diagnose a stall
 * are recorded: how many bytes arrived against Content-Length, and how many
 * times the read listener fired to collect them. A hang here is the result the
 * task went looking for, so it has to come back as data and not as a request
 * that never answers.
 */
function fetchOverTls(string $id): PromiseInterface
{
    $started = \microtime(true);
    $url = \getenv('ORIGIN_URL') ?: 'https://origin/slow';

    // Peer verification off: the origin serves the self-signed certificate it
    // generated at build time, and this route measures an event loop, not a
    // PKI. Never do this outside a demo.
    $browser = (new React\Http\Browser(new Connector([
        'tls' => ['verify_peer' => false, 'verify_peer_name' => false],
    ])))->withTimeout(TLS_TIMEOUT);

    $state = ['bytes' => 0, 'reads' => 0, 'length' => -1, 'complete' => false];
    $report = static function (array $state, string $id, float $started, ?string $error) use ($url): array {
        return [$error === null ? 200 : 500, ['Content-Type' => 'application/json'], \json_encode([
            'id' => $id,
            'route' => 'tls',
            'pid' => \getmypid(),
            'url' => $url,
            'bytes' => $state['bytes'],
            'reads' => $state['reads'],
            'length' => $state['length'],
            'complete' => $state['complete'],
            'error' => $error,
            't0' => $started,
            't1' => \microtime(true),
        ], \JSON_THROW_ON_ERROR)];
    };

    return $browser->requestStreaming('GET', $url)->then(
        function (ResponseInterface $response) use (&$state, $id, $started, $report): PromiseInterface {
            $state['length'] = (int) ($response->getHeaderLine('Content-Length') ?: -1);
            /** @var ReadableStreamInterface $body */
            $body = $response->getBody();
            $deferred = new Deferred();

            $body->on('data', function (string $chunk) use (&$state): void {
                // One 'data' event is one fire of the FPMNG_WORKER_READ watcher
                // on the TLS stream, so this counter is the evidence that the
                // watcher kept firing rather than stalling with bytes buffered
                // above the descriptor.
                $state['bytes'] += \strlen($chunk);
                $state['reads']++;
            });
            $body->on('end', function () use (&$state, $deferred, $id, $started, $report): void {
                $state['complete'] = true;
                $deferred->resolve($report($state, $id, $started, null));
            });
            $body->on('error', function (\Throwable $e) use (&$state, $deferred, $id, $started, $report): void {
                $deferred->resolve($report($state, $id, $started, $e::class . ': ' . $e->getMessage()));
            });

            // The stall this route exists to detect would otherwise hold the
            // request for the life of the worker, and "not measured" is not an
            // acceptable answer for it. The timer is what turns a hang into a
            // report carrying bytes/reads/length.
            $timer = Loop::addTimer(TLS_TIMEOUT, function () use (&$state, $deferred, $body, $id, $started, $report): void {
                $body->close();
                $deferred->resolve($report(
                    $state,
                    $id,
                    $started,
                    \sprintf(
                        'stalled: %d of %d bytes after %d reads, no further readability within %.1fs',
                        $state['bytes'],
                        $state['length'],
                        $state['reads'],
                        TLS_TIMEOUT
                    )
                ));
            });

            return $deferred->promise()->then(function (array $response) use ($timer): array {
                Loop::cancelTimer($timer);

                return $response;
            });
        },
        function (\Throwable $e) use (&$state, $id, $started, $report): array {
            return $report($state, $id, $started, $e::class . ': ' . $e->getMessage());
        }
    );
}

/**
 * The stranded-buffer probe, and the reason /tls alone is not enough.
 *
 * /tls came back complete, which on its own only proves that a throttled
 * origin keeps making the descriptor readable again. The case
 * fpmng_worker_event_create() warns about is narrower: plaintext already
 * decrypted into the *stream* sitting above a descriptor that has nothing left
 * to report. To reach it the client has to be unable to drain what one
 * readable event made available, so this route reads through ReactPHP's own
 * ReadableResourceStream at a chunk far below a single 16 KiB TLS record
 * (bufferSize at react/stream, ReadableResourceStream.php:84, one
 * stream_get_contents() per event at :146) against a body the origin sends as
 * fast as it can.
 *
 * Two cases, selected by $keepAlive, because the first one measured does not
 * reach the trap and saying so is half the result:
 *
 *   keepAlive = false   1 MiB, Connection: close. The kernel receive buffer
 *                       never runs dry while the client crawls through it a
 *                       kilobyte at a time, so the descriptor stays readable
 *                       and nothing can strand. Measured: 1025 reads, 4 ms.
 *   keepAlive = true    8 KiB, Connection: keep-alive. After the first read
 *                       the socket is empty and the server sends nothing more
 *                       — no further data, and no FIN either. Anything PHP
 *                       decrypted into the stream's own buffer beyond what the
 *                       chunk returned is now invisible to libevent, which is
 *                       exactly the condition the warning describes.
 *
 * The handshake is done synchronously on purpose: this route is a probe, and
 * an asynchronous connect plus enable_crypto dance would put the very
 * machinery under test in the way of setting the test up. It blocks the worker
 * for the duration of one handshake, which is why this is not how /tls works.
 */
function probeStrandedTlsBuffer(string $id, int $chunk, bool $keepAlive): PromiseInterface
{
    $started = \microtime(true);
    $url = $keepAlive
        ? (\getenv('ORIGIN_SMALL_URL') ?: 'https://origin/small')
        : (\getenv('ORIGIN_BURST_URL') ?: 'https://origin/burst');
    $parts = \parse_url($url);
    $host = $parts['host'] ?? 'origin';
    $port = $parts['port'] ?? 443;
    $path = $parts['path'] ?? '/';

    $context = \stream_context_create(['ssl' => [
        // Same demo-only exemption as /tls, same reason.
        'verify_peer' => false,
        'verify_peer_name' => false,
    ]]);
    $socket = @\stream_socket_client(
        \sprintf('tls://%s:%d', $host, $port),
        $errno,
        $errstr,
        10.0,
        \STREAM_CLIENT_CONNECT,
        $context
    );
    if ($socket === false) {
        return reject(new \RuntimeException(\sprintf('connect to %s failed: %s (%d)', $url, $errstr, $errno)));
    }
    \stream_set_blocking($socket, false);
    \fwrite($socket, \sprintf(
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: %s\r\n\r\n",
        $path,
        $host,
        $keepAlive ? 'keep-alive' : 'close'
    ));

    $stream = new ReadableResourceStream($socket, null, $chunk);
    $state = ['bytes' => 0, 'reads' => 0, 'body' => -1, 'head' => '', 'overhead' => -1];
    $deferred = new Deferred();
    $report = static function (?string $error) use (&$state, $id, $started, $chunk, $url, $keepAlive): array {
        return [$error === null ? 200 : 500, ['Content-Type' => 'application/json'], \json_encode([
            'id' => $id,
            'route' => 'strand',
            'pid' => \getmypid(),
            'url' => $url,
            'chunk' => $chunk,
            'keepalive' => $keepAlive,
            'bytes' => $state['bytes'],
            'reads' => $state['reads'],
            // What the origin said it would send, and how much of the bytes
            // above were its response head rather than payload. The three
            // together are the whole verdict: received minus overhead against
            // body is how many bytes are stranded.
            'body' => $state['body'],
            'overhead' => $state['overhead'],
            'error' => $error,
            't0' => $started,
            't1' => \microtime(true),
        ], \JSON_THROW_ON_ERROR)];
    };

    // On keep-alive there is no EOF and therefore no 'end' event, so
    // completeness has to come from the origin's own Content-Length. Parsed
    // out of the raw bytes because this probe deliberately has no HTTP client
    // above the stream — an HTTP client would read at its own chunk size and
    // the chunk size is the whole experiment.
    $stream->on('data', function (string $data) use (&$state, $deferred, $stream, $report): void {
        $state['bytes'] += \strlen($data);
        $state['reads']++;

        if ($state['overhead'] < 0) {
            $state['head'] .= $data;
            $end = \strpos($state['head'], "\r\n\r\n");
            if ($end !== false) {
                $state['overhead'] = $end + 4;
                if (\preg_match('/\r\nContent-Length:\s*(\d+)/i', $state['head'], $m) === 1) {
                    $state['body'] = (int) $m[1];
                }
                $state['head'] = '';
            }
        }

        if ($state['body'] >= 0 && $state['bytes'] >= $state['overhead'] + $state['body']) {
            $stream->close();
            $deferred->resolve($report(null));
        }
    });
    $stream->on('end', function () use ($deferred, $report): void {
        // Reached on Connection: close, where FIN keeps the descriptor
        // readable until the stream is drained.
        $deferred->resolve($report(null));
    });
    $stream->on('error', function (\Throwable $e) use ($deferred, $report): void {
        $deferred->resolve($report($e::class . ': ' . $e->getMessage()));
    });

    // Idle rather than total: the answer is not "how long did it take" but
    // "did the reads stop with bytes still owed". A watcher that will never
    // fire again looks exactly like this.
    $idle = null;
    $arm = function () use (&$arm, &$idle, &$state, $deferred, $stream, $chunk, $report): void {
        $seen = $state['reads'];
        $idle = Loop::addTimer(STRAND_IDLE, function () use (&$arm, &$state, $seen, $deferred, $stream, $chunk, $report): void {
            if ($state['reads'] !== $seen) {
                $arm();

                return;
            }

            $stream->close();
            // This is the message the task went looking for. When it appears,
            // the warning in fpmng_worker_event_create() is not theoretical:
            // the bytes are in the stream, the descriptor has nothing left to
            // report, and the watcher will never fire again.
            $deferred->resolve($report(\sprintf(
                'stranded: %d of %d body bytes after %d reads of %d, quiet for %.1fs',
                \max(0, $state['bytes'] - \max(0, $state['overhead'])),
                $state['body'],
                $state['reads'],
                $chunk,
                STRAND_IDLE
            )));
        });
    };
    $arm();

    return $deferred->promise()->then(function (array $response) use (&$idle): array {
        if ($idle !== null) {
            Loop::cancelTimer($idle);
        }

        return $response;
    });
}

/**
 * The futureTick() route, and the reason it is not just "implemented and
 * assumed": a pending far-future timer is registered first, so if the loop
 * asked libevent to block before draining its tick queue, the K ticks would
 * complete when that timer is due — measurably, in seconds — instead of on the
 * next iteration. Revolt has no equivalent primitive, so nothing before this
 * task ever passed false to fpmng_worker_loop() for this reason.
 */
function drainFutureTicks(string $id, int $n): PromiseInterface
{
    $started = \microtime(true);
    $deferred = new Deferred();
    $blocker = Loop::addTimer(FUTURE_TICK_BLOCKER, static function (): void {
        // Never expected to fire: the request answers long before, and the
        // timer is cancelled below. Present only to make a blocking
        // dispatch expensive enough to see.
        \fwrite(\STDERR, 'http-direct worker: /ticks blocker fired; the tick queue was not drained' . \PHP_EOL);
    });

    $remaining = $n;
    $tick = function () use (&$remaining, &$tick, $deferred, $started, $id, $n, $blocker): void {
        if (--$remaining > 0) {
            // Queued from inside the drain, which is also the case that would
            // deadlock a queue that refused to grow while ticking.
            Loop::futureTick($tick);

            return;
        }

        Loop::cancelTimer($blocker);
        $deferred->resolve([200, ['Content-Type' => 'application/json'], \json_encode([
            'id' => $id,
            'route' => 'ticks',
            'pid' => \getmypid(),
            'ticks' => $n,
            'blocker' => FUTURE_TICK_BLOCKER,
            't0' => $started,
            't1' => \microtime(true),
        ], \JSON_THROW_ON_ERROR)]);
    };
    Loop::futureTick($tick);

    return $deferred->promise();
}

(new FpmngReactServer(function (array $env, string $body) {
    $path = \parse_url($env['REQUEST_URI'] ?? '/', \PHP_URL_PATH);
    $query = [];
    \parse_str((string) ($env['QUERY_STRING'] ?? ''), $query);
    $id = (string) ($query['id'] ?? '');
    $json = ['Content-Type' => 'application/json'];

    $route = match ($path) {
        '/mysql' => static fn (): PromiseInterface => sleepInMysql($id),
        '/tls' => static fn (): PromiseInterface => fetchOverTls($id),
        '/strand' => static fn (): PromiseInterface => probeStrandedTlsBuffer(
            $id,
            \max(64, \min(65536, (int) ($query['chunk'] ?? 1024))),
            (bool) ($query['keep'] ?? false)
        ),
        '/ticks' => static fn (): PromiseInterface => drainFutureTicks(
            $id,
            \max(1, \min(100000, (int) ($query['n'] ?? 1000)))
        ),
        default => null,
    };

    if ($route === null) {
        return [200, ['Content-Type' => 'text/plain'], 'hello world from pid ' . \getmypid() . "\n"];
    }

    return resolve(null)->then($route)->then(null, static function (\Throwable $e) use ($id, $path, $json): array {
        // The message is the deliverable when a route fails: it is what
        // distinguishes "the watcher never fired" from "authentication with
        // mysql_native_password was refused" from "no route to the origin".
        return [500, $json, \json_encode([
            'id' => $id,
            'route' => \ltrim((string) $path, '/'),
            'pid' => \getmypid(),
            'error' => $e::class . ': ' . $e->getMessage(),
        ], \JSON_THROW_ON_ERROR)];
    });
}))->run();
