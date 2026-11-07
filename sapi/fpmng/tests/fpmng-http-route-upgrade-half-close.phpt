--TEST--
fpm-ng: a client that sends a large body and half-closes gets every byte to the worker, and the worker sees EOF (issue #659)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #659, review round 2, item 1. The worker reads nothing for one second,
 * so the socket buffers fill and the gateway holds bytes it has not sent. The
 * client then half-closes. Every byte the client got accepted must reach the
 * worker before the worker sees EOF. Before the fix the gateway dropped the
 * bytes it still held and closed the upstream at once (fpm_gw_ws_step). */

$root = sys_get_temp_dir() . '/fpmng-route-upgrade-half-close-' . getmypid();
@mkdir($root, 0700, true);

/* The worker waits one second, then reads slowly. When it sees EOF it writes
 * the byte count it read, which the test compares with the client's count. */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$ws = null;
$wsWatcher = null;
$startTimer = null;
$total = 0;

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$ws, &$wsWatcher, &$startTimer, &$total): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        $uri = $env['REQUEST_URI'] ?? '/';
        if (str_contains($uri, '/ws')) {
            $ws = fpmng_worker_upgrade($id, []);
            $startTimer = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use (&$ws, &$wsWatcher, &$startTimer, &$total): void {
                fpmng_worker_event_free($startTimer);
                $wsWatcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $ws, function () use (&$ws, &$wsWatcher, &$total): void {
                    if ($ws === null) {
                        return;
                    }
                    $chunk = fread($ws, 16384);
                    if ($chunk === false || $chunk === '') {
                        if (feof($ws)) {
                            file_put_contents(getcwd() . '/half', $total . ' eof');
                            fpmng_worker_event_free($wsWatcher);
                            fclose($ws);
                            $ws = null;
                        }
                        return;
                    }
                    $total += strlen($chunk);
                    usleep(2000);
                });
                fpmng_worker_event_enable($wsWatcher);
            });
            fpmng_worker_event_enable($startTimer, 1.0);
            continue;
        }
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'hello from worker');
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit() || $ws !== null) {
    fpmng_worker_loop(true);
}
PHP);

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.front_controller = /worker.php
http.route[ws] = /ws
http.route_upgrade[/ws] = websocket
[ws]
listen = {{ADDR[ws]}}
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
EOT;

function connect(string $addr)
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) throw new RuntimeException("connect $addr: $error");
    stream_set_timeout($fp, 10);
    return $fp;
}

function upgradeRequest(string $key): string
{
    return "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\n\r\n";
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

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $gw = $tester->getAddr('ipv4', '[http]');

    $key = 'dGhlIHNhbXBsZSBub25jZQ==';
    $ws = connect($gw);
    fwrite($ws, upgradeRequest($key));
    [$status] = readHead($ws);
    check(str_starts_with($status, 'HTTP/1.1 101'), "101 expected: $status");

    /* Write until the socket refuses bytes for 300 ms. The worker reads
     * nothing yet, so the count is what the client's socket and the gateway
     * took: the gateway holds part of it when the client half-closes. */
    stream_set_blocking($ws, false);
    $chunk = str_repeat('x', 65536);
    $sent = 0;
    $stalled = 0.0;
    while ($stalled < 0.3) {
        $n = fwrite($ws, $chunk);
        if ($n === false || $n === 0) {
            usleep(10000);
            $stalled += 0.01;
            continue;
        }
        $sent += $n;
        $stalled = 0.0;
    }
    stream_set_blocking($ws, true);
    check($sent > 1048576, "only $sent bytes were accepted: the buffers did not fill");
    check(stream_socket_shutdown($ws, STREAM_SHUT_WR), 'the client could not half-close');

    /* The worker writes its marker when it sees EOF. The count must be every
     * byte the client got accepted. */
    $marker = '';
    for ($i = 0; $i < 150 && $marker === ''; $i++) {
        if (file_exists("$root/half")) {
            $marker = file_get_contents("$root/half");
        }
        if ($marker === '') usleep(100000);
    }
    check($marker === "$sent eof", "the worker got \"$marker\", not \"$sent eof\"");
    echo "half-close-delivers-all-bytes: ok\n";
    fclose($ws);
    /* A respawned gateway would still answer these checks, so the log must
     * show no gateway child death (fpm_http_gw_proc.c:1290-1293). */
    $tester->expectNoLogPattern('/http gateway \d+ \(pid \d+\) (killed by signal|exited with code)/', true);
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink("$root/worker.php");
    @unlink("$root/half");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
half-close-delivers-all-bytes: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
