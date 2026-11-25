--TEST--
fpm-ng: a WebSocket Upgrade through a gateway route is relayed both ways to a worker target; the worker's close ends the client connection (issue #659)
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

/* Issue #659, acceptance criteria 1-3 and the upstream-close case. The
 * gateway carries the 101 and then only bytes: the test checks that what
 * the client sends reaches the worker and comes back unchanged, in both
 * directions, with a payload larger than the relay's 64 KiB high-water mark.
 * The worker closes with a message the client must receive before EOF. */

$root = sys_get_temp_dir() . '/fpmng-route-upgrade-relay-' . getmypid();
@mkdir($root, 0700, true);

/* The worker echoes raw bytes. It does not parse frames: the relay does not
 * either, so an echo of the bytes is the proof that nothing was dropped or
 * reordered. A payload containing the word "bye" is never sent by the test,
 * because the worker treats that word as its close signal. */
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

    /* The 101 comes from the worker's codec; the gateway forwards it.
     * Accept is base64(sha1(key . GUID)), as RFC 6455 section 4.2.2 says. */
    $ws = connect($gw);
    $key = 'dGhlIHNhbXBsZSBub25jZQ==';
    fwrite($ws, upgradeRequest($key));
    [$status, $headers] = readHead($ws);
    check(str_starts_with($status, 'HTTP/1.1 101'), "101 expected through the gateway: $status");
    check(($headers['sec-websocket-accept'] ?? '') === base64_encode(sha1($key . '258EAFA5-E914-47DA-95CA-C5AB0DC85B11', true)),
        'Sec-WebSocket-Accept wrong: ' . var_export($headers['sec-websocket-accept'] ?? null, true));
    echo "handshake-101-accept: ok\n";

    /* Both directions, 192 KiB in 16 KiB rounds. Each round writes less than
     * the relay's high-water mark and reads it back before the next one, so
     * the test cannot deadlock on its own buffers; the relay itself is what
     * has to stop reading when its buffer fills (fpm_http_gw_ws.c). */
    $block = str_repeat('0123456789abcdef', 1024);
    for ($round = 0; $round < 12; $round++) {
        fwrite($ws, $block);
        $echo = readExact($ws, strlen($block));
        check($echo === $block, "echo differs in round $round");
    }
    echo "relay-both-ways-192k: ok\n";

    /* Worker-initiated close: the worker sends a message, then closes. The
     * client must read the message, then see EOF. */
    fwrite($ws, 'bye');
    check(readExact($ws, strlen('server-says-bye')) === 'server-says-bye', 'the worker message did not reach the client');
    check(fread($ws, 1) === '' && feof($ws), 'the client connection did not end after the worker closed');
    fclose($ws);
    echo "upstream-close-ends-client: ok\n";
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
handshake-101-accept: ok
relay-both-ways-192k: ok
upstream-close-ends-client: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
