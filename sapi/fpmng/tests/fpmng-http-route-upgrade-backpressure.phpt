--TEST--
fpm-ng: a worker that does not read stops the gateway from reading the client, so the relay buffers a bounded amount (issue #659)
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

/* Issue #659, review round 2, item 2. The relay stops reading a side whose
 * peer has FPM_GW_WS_HIGH bytes unsent (fpm_gw_ws_move, the
 * bufferevent_disable() call). This test sends as fast as the socket accepts
 * for two seconds, while the worker reads nothing for three. Without the pause
 * the gateway would buffer the whole stream in memory (the client's cap is
 * 128 MiB, so the count reaches it); with the pause the bytes the client got
 * accepted are the socket buffers, far below 32 MiB. */

$root = sys_get_temp_dir() . '/fpmng-route-upgrade-backpressure-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$ws = null;
$wsWatcher = null;
$startTimer = null;

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$ws, &$wsWatcher, &$startTimer): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        $uri = $env['REQUEST_URI'] ?? '/';
        if (str_contains($uri, '/ws')) {
            $ws = fpmng_worker_upgrade($id, []);
            /* Silent for three seconds, then it reads to EOF. */
            $startTimer = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use (&$ws, &$wsWatcher, &$startTimer): void {
                fpmng_worker_event_free($startTimer);
                $wsWatcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $ws, function () use (&$ws, &$wsWatcher): void {
                    if ($ws === null) {
                        return;
                    }
                    while (true) {
                        $chunk = fread($ws, 65536);
                        if ($chunk === false || $chunk === '') {
                            break;
                        }
                    }
                    if (feof($ws)) {
                        fpmng_worker_event_free($wsWatcher);
                        fclose($ws);
                        $ws = null;
                    }
                });
                fpmng_worker_event_enable($wsWatcher);
            });
            fpmng_worker_event_enable($startTimer, 3.0);
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
    fwrite($ws, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\n\r\n");
    [$status] = readHead($ws);
    check(str_starts_with($status, 'HTTP/1.1 101'), "101 expected: $status");

    /* Non-blocking writes: the loop stops when the socket is full. */
    stream_set_blocking($ws, false);
    $chunk = str_repeat('x', 65536);
    $cap = 128 * 1024 * 1024;
    $accepted = 0;
    $t0 = microtime(true);
    while ($accepted < $cap && microtime(true) - $t0 < 2.0) {
        $n = fwrite($ws, $chunk);
        if ($n === false || $n === 0) {
            usleep(1000);
            continue;
        }
        $accepted += $n;
    }
    fclose($ws);
    check($accepted < 32 * 1024 * 1024,
        "the gateway took $accepted bytes from a client while the worker read nothing: the relay is not bounded");
    echo "backpressure-bounds-the-relay: ok\n";
    /* A respawned gateway would still answer these checks, so the log must
     * show no gateway child death (fpm_http_gw_proc.c:1290-1293). */
    $tester->expectNoLogPattern('/http gateway \d+ \(pid \d+\) (killed by signal|exited with code)/', true);
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
backpressure-bounds-the-relay: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
