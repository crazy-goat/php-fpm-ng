<?php

/*
 * Task 074: the same worker-owned event loop as examples/http-direct-worker,
 * but the sleeping route sleeps in MySQL instead of in a timer.
 *
 * The point is the primitive being exercised, not the SQL. Amp\delay() in the
 * other example only ever touches FPMNG_WORKER_TIMER; amphp/mysql opens a TCP
 * socket, which means an asynchronous connect (FPMNG_WORKER_WRITE, registered
 * by no other test) followed by protocol reads (FPMNG_WORKER_READ on a socket
 * rather than on the notify pipe). If N concurrent SELECT SLEEP(1) finish in
 * about one second on one worker, fd watchers work for real I/O.
 *
 * /mysql-tls exists to answer the open question in fpmng_worker_event_create():
 * the watcher is armed on the raw descriptor obtained with php_stream_cast(),
 * so a stream that buffers above the descriptor — a TLS stream — may hold
 * bytes the descriptor never reports as readable. Whether that is fatal is a
 * measurement, not an opinion, so the route is here and its result is written
 * down in the README either way.
 */

declare(strict_types=1);

require __DIR__ . '/vendor/autoload.php';
require __DIR__ . '/../http-direct-worker/FpmngDriver.php';
require __DIR__ . '/../http-direct-worker/FpmngServer.php';

use Amp\Mysql\MysqlConfig;
use Amp\Mysql\MysqlConnectionPool;
use Amp\Socket\ClientTlsContext;
use Amp\Socket\ConnectContext;
use Amp\TimeoutCancellation;
use Fpmng\Poc\FpmngServer;

use function Amp\async;

/*
 * The driver is imported from the task 073 example rather than copied. The
 * driver is what is under test; a second copy would drift from the original
 * and the two examples would stop proving the same thing.
 */

const QUERY_TIMEOUT = 10.0;

/*
 * MysqlConnectionPool defaults to 100 connections
 * (vendor/amphp/sql-common/src/SqlCommonConnectionPool.php:37), which is the
 * first cap a large N hits — below it the pool queues, the queries stop
 * overlapping, and the harness would blame the event loop for a client-side
 * limit. Overridable so the harness can raise it together with N. The next
 * ceiling above this one is not MySQL's max_connections but
 * FPM_WORKER_PENDING_MAX = 256 (sapi/fpmng/fpm/fpm_http_direct_worker.c:73),
 * beyond which the worker asks to be recycled.
 */
const POOL_MAX = 100;

/**
 * One pool per mode for the whole worker, built on first use. A pool, not a
 * single connection: N concurrent SELECT SLEEP(1) need N server-side threads,
 * so a single connection would serialize them and the measurement would show
 * N seconds — correctly, because that would genuinely be one connection.
 */
function pool(bool $tls): MysqlConnectionPool
{
    static $pools = [];
    $key = $tls ? 'tls' : 'plain';

    if (!isset($pools[$key])) {
        $dsn = \sprintf(
            'host=%s;port=%s;user=%s;password=%s;db=%s',
            \getenv('MYSQL_HOST') ?: '127.0.0.1',
            \getenv('MYSQL_PORT') ?: '3306',
            \getenv('MYSQL_USER') ?: 'fpmng',
            \getenv('MYSQL_PASSWORD') ?: 'fpmng',
            \getenv('MYSQL_DATABASE') ?: 'fpmng'
        );
        $context = null;
        if ($tls) {
            // Peer verification off on purpose: the server certificate is the
            // one mysqld auto-generates, and this route measures the event
            // loop, not a PKI. Never do this outside a demo.
            $context = (new ConnectContext())
                ->withTlsContext((new ClientTlsContext(''))->withoutPeerVerification());
        }
        $max = (int) (\getenv('MYSQL_POOL_MAX') ?: POOL_MAX);
        $pools[$key] = new MysqlConnectionPool(MysqlConfig::fromString($dsn, $context), $max);
    }

    return $pools[$key];
}

/**
 * A hang is a result too, so it has to be reportable. Without the cancellation
 * a TLS stream whose buffered bytes the watcher never sees would leave the
 * request open forever, the harness would time out with no diagnosis, and the
 * acceptance criterion ("record the failure mode") could not be met.
 */
function sleepInMysql(bool $tls, string $id): array
{
    $started = \microtime(true);
    /*
     * Ssl_cipher is read in the same statement, and therefore on the same
     * pooled connection, as the sleep. It is not decoration: amphp only
     * upgrades to TLS if the server advertises CLIENT_SSL, and if it does not,
     * the capability bit is silently masked off and the connection continues in
     * plaintext with no error at all
     * (vendor/amphp/mysql/src/Internal/ConnectionProcessor.php:1556-1572). A
     * /mysql-tls route that had quietly become a second copy of /mysql would
     * still overlap, still report "mode":"tls", and would prove nothing about a
     * watcher armed on a buffered stream. The server's own view of the session
     * is the only trustworthy witness, so the harness asserts on this field.
     */
    $future = async(static fn (): array => (array) pool($tls)
        ->query(
            'SELECT SLEEP(1) AS slept, ('
            . "SELECT VARIABLE_VALUE FROM performance_schema.session_status"
            . " WHERE VARIABLE_NAME = 'Ssl_cipher') AS cipher"
        )
        ->fetchRow());
    $row = $future->await(new TimeoutCancellation(QUERY_TIMEOUT));

    return [
        'id' => $id,
        'mode' => $tls ? 'tls' : 'plain',
        'pid' => \getmypid(),
        'slept' => (int) $row['slept'],
        'cipher' => (string) ($row['cipher'] ?? ''),
        't0' => $started,
        't1' => \microtime(true),
    ];
}

(new FpmngServer(function (array $env, string $body): array {
    $path = \parse_url($env['REQUEST_URI'] ?? '/', \PHP_URL_PATH);
    $query = [];
    \parse_str((string) ($env['QUERY_STRING'] ?? ''), $query);
    $json = ['Content-Type' => 'application/json'];

    if ($path === '/mysql' || $path === '/mysql-tls') {
        try {
            return [200, $json, \json_encode(
                sleepInMysql($path === '/mysql-tls', (string) ($query['id'] ?? '')),
                \JSON_THROW_ON_ERROR
            )];
        } catch (\Throwable $e) {
            // The message is the deliverable when this route fails: it is what
            // distinguishes "TLS handshake refused" from "the watcher never
            // fired" from "wrong password".
            return [500, $json, \json_encode([
                'id' => (string) ($query['id'] ?? ''),
                'mode' => $path === '/mysql-tls' ? 'tls' : 'plain',
                'pid' => \getmypid(),
                'error' => $e::class . ': ' . $e->getMessage(),
            ], \JSON_THROW_ON_ERROR)];
        }
    }

    return [200, ['Content-Type' => 'text/plain'], 'hello world from pid ' . \getmypid() . "\n"];
}))->run();
