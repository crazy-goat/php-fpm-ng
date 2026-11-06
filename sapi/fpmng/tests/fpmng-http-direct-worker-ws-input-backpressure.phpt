--TEST--
fpm-ng: an upgraded WebSocket connection bounds its unread input -- a peer that sends while the script does not read cannot grow the worker's buffer, and reading resumes and delivers every byte (issue #974)
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

/* Issue #974: after fpmng_worker_upgrade() no read watermark was set, so libevent
 * moved every byte the peer sent into the connection's input evbuffer whether or
 * not userland fread() it. The worker below upgrades and does NOT read the
 * stream (its read watcher stays disabled) until /resume. The bound is checked
 * from the worker side: /drain reports how many unread bytes the buffer held,
 * and must not exceed the fixed high watermark (FPM_WS_INPUT_HIGH_WATER, 64 KiB
 * in fpm_http_direct_ws.c). Before the fix the buffer held 3132416 bytes of the 16 MiB (measured on the unfixed build). */

$wsLimit = 65536;

$root = sys_get_temp_dir() . '/fpmng-ws-input-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
function drainWs($stream): int
{
    $n = 0;
    while (($chunk = fread($stream, 65536)) !== false && $chunk !== '') {
        $n += strlen($chunk);
    }
    return $n;
}

$notify = fpmng_worker_notify_stream();
$ws = null;
$wsWatcher = null;
$total = 0;

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$ws, &$wsWatcher, &$total): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        $uri = $env['REQUEST_URI'] ?? '/';

        if (str_contains($uri, '/ws')) {
            $ws = fpmng_worker_upgrade($id, []);
            /* Created but not enabled: until /resume nothing reads the stream. */
            $wsWatcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $ws, function () use (&$ws, &$total): void {
                $total += drainWs($ws);
            });
            continue;
        }
        if (str_contains($uri, '/drain')) {
            $n = drainWs($ws);
            $total += $n;
            fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], (string) $n);
            continue;
        }
        if (str_contains($uri, '/resume')) {
            fpmng_worker_event_enable($wsWatcher);
            fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'resumed');
            continue;
        }
        if (str_contains($uri, '/total')) {
            $total += drainWs($ws);
            fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], (string) $total);
            continue;
        }
        fpmng_worker_respond($id, 404, ['Content-Type' => 'text/plain'], 'no route');
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit() || $ws !== null) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WS_INPUT_PORT') ?: 28195 + 200 * (int) getenv('TEST_PHP_WORKER') + (int) getenv('FPMNG_PHPT_PORT_SHIFT'));
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
http.max_body = 1M
catch_workers_output = yes
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
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        /* skip headers */
    }
    return [$status];
}

/* One ordinary request on its own connection; returns the body. */
function httpGet(int $port, string $path): string
{
    $fp = connect($port);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    [$status] = readHead($fp);
    check(str_starts_with($status, 'HTTP/1.1 200'), "GET $path: $status");
    $body = stream_get_contents($fp);
    fclose($fp);
    return (string) $body;
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $ws = connect($port);
    $key = 'dGhlIHNhbXBsZSBub25jZQ==';
    fwrite($ws, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\n\r\n");
    [$status] = readHead($ws);
    check(str_starts_with($status, 'HTTP/1.1 101'), "101 expected: $status");
    echo "upgrade-101: ok\n";

    /* The peer writes 16 MiB as fast as the socket accepts it. A fwrite() that
     * accepts nothing returns 0 on a nonblocking stream; $sent counts what was
     * really written, so partial writes need no special case. */
    stream_set_blocking($ws, false);
    $want = 16 * 1024 * 1024;
    $sent = 0;
    $pump = function () use ($ws, &$sent, $want): void {
        while ($sent < $want) {
            $n = fwrite($ws, str_repeat('x', min(65536, $want - $sent)));
            if (!$n) {
                return;
            }
            $sent += $n;
        }
    };
    $pump();
    usleep(300000);
    $pump();

    /* The worker has not read. Its buffer holds only what the bound allows;
     * the rest waits in the kernel, which is the TCP backpressure. */
    $drained = (int) httpGet($port, '/drain');
    check($drained > 0, "no input reached the worker (sent $sent)");
    check($drained <= $wsLimit, "worker buffered $drained unread bytes, bound is $wsLimit (peer sent $sent)");
    echo "input-bounded-while-unread: ok\n";

    /* Reading resumes: the watcher is enabled, the peer keeps writing, and the
     * worker must receive every byte. Nothing may be stranded behind the bound. */
    httpGet($port, '/resume');
    $total = 0;
    for ($i = 0; $i < 2000; $i++) {
        $pump();
        $total = (int) httpGet($port, '/total');
        if ($sent === $want && $total === $want) {
            break;
        }
        usleep(10000);
    }
    check($total === $want, "worker read $total of $want bytes (peer sent $sent)");
    echo "input-resumes-and-delivers-all: ok\n";

    fclose($ws);
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
upgrade-101: ok
input-bounded-while-unread: ok
input-resumes-and-delivers-all: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
