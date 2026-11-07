--TEST--
fpm-ng: an Upgrade request that is not a GET is refused with 501 on an opted-in route, and the worker never sees it (issue #659)
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

/* Issue #659, review round 2, item 2. RFC 6455 4.1 makes the handshake a GET.
 * The route is opted in, so only the method check (fpm_http_client.c,
 * fpm_http_http_websocket_upgrade()) stops a POST with Upgrade: websocket from
 * reaching the worker. The worker leaves a marker when it is asked to upgrade. */

$root = sys_get_temp_dir() . '/fpmng-route-upgrade-method-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$ws = null;
$wsWatcher = null;

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$ws, &$wsWatcher): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        $uri = $env['REQUEST_URI'] ?? '/';
        if (str_contains($uri, '/ws')) {
            file_put_contents(getcwd() . '/upgraded', $env['REQUEST_METHOD'] ?? '?');
            $ws = fpmng_worker_upgrade($id, []);
            $wsWatcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $ws, function () use (&$ws, &$wsWatcher): void {
                if ($ws === null) {
                    return;
                }
                $data = fread($ws, 8192);
                if ($data === '' && feof($ws)) {
                    fpmng_worker_event_free($wsWatcher);
                    fclose($ws);
                    $ws = null;
                    return;
                }
                if ($data !== '') {
                    fwrite($ws, $data);
                }
            });
            fpmng_worker_event_enable($wsWatcher);
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
    foreach (['POST', 'PUT', 'HEAD'] as $method) {
        $ws = connect($gw);
        fwrite($ws, "$method /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            . "Sec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\nContent-Length: 0\r\n\r\n");
        [$status] = readHead($ws);
        check(str_starts_with($status, 'HTTP/1.1 501'), "$method with Upgrade: websocket must be 501: $status");
        fclose($ws);
    }
    check(!file_exists("$root/upgraded"), 'the worker was asked to upgrade a non-GET request: ' . @file_get_contents("$root/upgraded"));
    echo "non-get-upgrade-refused: ok\n";
    /* A respawned gateway would still answer these checks, so the log must
     * show no gateway child death (fpm_http_gw_proc.c:1290-1293). */
    $tester->expectNoLogPattern('/http gateway \d+ \(pid \d+\) (killed by signal|exited with code)/', true);
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink("$root/worker.php");
    @unlink("$root/upgraded");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
non-get-upgrade-refused: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
