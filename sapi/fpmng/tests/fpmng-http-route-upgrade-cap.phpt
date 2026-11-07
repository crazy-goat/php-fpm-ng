--TEST--
fpm-ng: an upgraded gateway connection counts against http.max_connections until the client leaves (issue #659)
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

$root = sys_get_temp_dir() . '/fpmng-route-upgrade-cap-' . getmypid();
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
            $ws = fpmng_worker_upgrade($id, []);
            $wsWatcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $ws, function () use (&$ws, &$wsWatcher): void {
                if ($ws === null) {
                    return;
                }
                $data = '';
                while (true) {
                    $chunk = fread($ws, 8192);
                    if ($chunk === false || $chunk === '') {
                        break;
                    }
                    $data .= $chunk;
                }
                if (str_contains($data, 'bye')) {
                    fwrite($ws, 'server-says-bye');
                    /* Close only after the bytes reached the wire: the write
                     * watcher says so (the same rule as the worker-ws test). */
                    $closer = fpmng_worker_event_create(FPMNG_WORKER_WRITE, $ws, function () use (&$ws, &$wsWatcher, &$closer): void {
                        fpmng_worker_event_free($closer);
                        fpmng_worker_event_free($wsWatcher);
                        fclose($ws);
                        $ws = null;
                    });
                    fpmng_worker_event_enable($closer);
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
http.max_connections = 1
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

function readExact($fp, int $length): string
{
    $data = '';
    while (strlen($data) < $length) {
        $part = fread($fp, $length - strlen($data));
        if ($part === false || $part === '') {
            throw new RuntimeException('short read: ' . strlen($data) . ' of ' . $length);
        }
        $data .= $part;
    }
    return $data;
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
    echo "first-upgrade-holds-slot: ok\n";

    /* http.max_connections = 1 and the first upgraded connection still holds
     * it: a second request must not be upgraded. The gateway may refuse the
     * connection without an answer, so an empty reply counts as refused. */
    $second = connect($gw);
    fwrite($second, upgradeRequest($key));
    $line = @fgets($second);
    check($line === false || $line === '' || !str_starts_with($line, 'HTTP/1.1 101'),
        'a second upgrade was accepted over the cap: ' . var_export($line, true));
    fclose($second);
    echo "cap-reached-refuses: ok\n";

    /* After the first client leaves, the slot is released and a new upgrade
     * is answered. */
    fclose($ws);
    $ok = false;
    for ($i = 0; $i < 50 && !$ok; $i++) {
        $try = @stream_socket_client("tcp://$gw", $errno, $error, 5);
        if ($try) {
            stream_set_timeout($try, 10);
            fwrite($try, upgradeRequest($key));
            $line = @fgets($try);
            $ok = $line !== false && str_starts_with($line, 'HTTP/1.1 101');
            fclose($try);
        }
        if (!$ok) usleep(100000);
    }
    check($ok, 'the slot was not released after the client left');
    echo "slot-released-after-close: ok\n";
    /* A respawned gateway would still answer these checks, so the log must
     * show no gateway child death (fpm_http_gw_proc.c:1290-1293). */
    $tester->expectNoLogPattern('/http gateway \d+ \(pid \d+\) (killed by signal|exited with code)/', true);
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink("$root/worker.php");
    @unlink("$root/eof");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
first-upgrade-holds-slot: ok
cap-reached-refuses: ok
slot-released-after-close: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
