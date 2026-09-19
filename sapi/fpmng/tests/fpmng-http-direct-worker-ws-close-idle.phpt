--TEST--
fpm-ng: fpm_ws_close() disarms the hijacked bufferevent before the shutdown on the empty-output path (issue #442)
--SKIPIF--
<?php
include "skipif.inc";
?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #442: closing an upgraded stream with an EMPTY output buffer (the
 * common case: an idle stream) used to efree() the ws context while the
 * hijacked bufferevent still had fpm_ws_* callbacks armed with that context
 * as the argument. shutdown() then made the fd readable, and the very next
 * loop dispatch ran fpm_ws_eventcb() on freed heap -- a worker SIGSEGV and a
 * master respawn. The fix disarms (and disables) the bufferevent BEFORE the
 * shutdown; the test asserts the worker child survives the close on the same
 * pid. USE_ZEND_ALLOC=0 + MALLOC_PERTURB_=165 scribble freed chunks so the
 * pre-fix stray write through ctx is a guaranteed signal, not a coin flip. */

$root = sys_get_temp_dir() . '/fpmng-ws-close-idle-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$ws = null;

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$ws): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        $uri = $env['REQUEST_URI'] ?? '/';

        if (str_contains($uri, '/ws')) {
            /* Upgraded and then left idle: no frame is ever written, so the
             * close below takes the empty-output path. */
            $ws = fpmng_worker_upgrade($id, []);
            continue;
        }
        if (str_contains($uri, '/close-ws')) {
            $open = $ws !== null;
            if ($open) {
                fclose($ws);        /* empty-output close: the #442 path */
                $ws = null;
            }
            fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'closed:' . var_export($open, true));
            continue;
        }
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'pid ' . getmypid());
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WS_CLOSE_IDLE_PORT') ?: 28146);
$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[worker]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
env[USE_ZEND_ALLOC] = 0
env[MALLOC_PERTURB_] = 165
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

function connect(int $port)
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    if (!$fp) throw new RuntimeException("connect :$port: $error");
    stream_set_timeout($fp, 10);
    return $fp;
}

function readHead($fp): array
{
    $line = fgets($fp);
    if (!$line || !str_starts_with($line, 'HTTP/')) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = trim($line);
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$k, $v] = explode(':', $line, 2);
        $headers[strtolower(trim($k))] = trim($v);
    }
    return [$status, $headers];
}

function probe(int $port): array
{
    $fp = connect($port);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    [$status, ] = readHead($fp);
    $body = stream_get_contents($fp);
    fclose($fp);
    check(str_starts_with($status, 'HTTP/1.1 200'), "probe status: $status");
    return [trim((string) $body)];
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    [$pid0] = probe($port);
    check(str_starts_with($pid0, 'pid '), 'probe body: ' . var_export($pid0, true));

    /* Upgrade and leave idle. */
    $ws = connect($port);
    fwrite($ws, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    [$status, ] = readHead($ws);
    check(str_starts_with($status, 'HTTP/1.1 101'), "101 expected: $status");
    echo "upgrade-101: ok\n";

    /* Server-side close of the idle stream, from an ordinary request. */
    $fp = connect($port);
    fwrite($fp, "GET /close-ws HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    [$status, ] = readHead($fp);
    $body = stream_get_contents($fp);
    fclose($fp);
    check(str_starts_with($status, 'HTTP/1.1 200') && trim((string) $body) === 'closed:true',
        'close-ws answered: ' . var_export($body, true));
    echo "server-close-eof: ok\n";

    /* Give the loop a few dispatches: the shutdown() makes the fd readable
     * and the pre-fix eventcb ran on freed heap right here. */
    usleep(700000);

    /* The worker must still be the same child. A pre-fix run respawns it
     * (SIGSEGV): pid1 != pid0. */
    [$pid1] = probe($port);
    check($pid1 === $pid0, "worker did not survive the idle-stream close: $pid0 vs $pid1");
    echo "worker-survives-idle-close: ok\n";
    fclose($ws);
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
upgrade-101: ok
server-close-eof: ok
worker-survives-idle-close: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
