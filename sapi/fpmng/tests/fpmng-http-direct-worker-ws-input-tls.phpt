--TEST--
fpm-ng: an upgraded WebSocket over TLS delivers every byte that the input bound held back -- the peer sends its burst and then stays silent, and the bytes OpenSSL kept inside the record layer must still reach the script (issue #974)
--SKIPIF--
<?php
include "skipif.inc";
if (!function_exists('openssl_x509_parse')) {
    die('skip requires the openssl extension');
}
if (trim((string) shell_exec('command -v openssl 2>/dev/null')) === '') {
    die('skip requires the openssl CLI to generate a test certificate');
}
$probe = new FPM\Tester(<<<'EOT'
[global]
error_log = {{FILE:LOG}}
[unconfined]
listen = {{ADDR}}
pm = static
pm.max_children = 1
pool.type = http-direct
pool.executor = worker
chdir = /tmp
http.front_controller = /nonexistent-front-controller.php
http.tls_cert = /nonexistent-cert.pem
http.tls_key = /nonexistent-key.pem
EOT, '<?php');
$messages = $probe->testConfig(true, null, false, false);
FPM\Tester::clean();
foreach ((array) $messages as $message) {
    if (str_contains($message, 'built with TLS support')) {
        die('skip php-fpm-ng built without TLS support (configure without --enable-fpmng-tls, issue #280)');
    }
}
?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function run(string $cmd): void
{
    exec($cmd . ' 2>&1', $output, $code);
    if ($code !== 0) {
        throw new RuntimeException("COMMAND FAILED: $cmd\n" . implode("\n", $output));
    }
}

/* Issue #974 over TLS. The plaintext test (ws-input-backpressure) cannot see
 * this failure: on a TLS connection the read watermark stops OpenSSL in the
 * middle of a record, and the rest of that record stays inside OpenSSL, where
 * the socket cannot signal it. The peer below sends 70000 bytes as 14 TLS
 * records of 5000 bytes, then nothing else. Every byte must reach the script
 * without any further byte from the peer. */

$total = 70000;
$record = 5000;

$root = sys_get_temp_dir() . '/fpmng-ws-input-tls-' . getmypid();
@mkdir($root, 0700, true);

run("openssl req -x509 -newkey rsa:2048 -keyout $root/key.pem -out $root/cert.pem "
    . "-days 2 -nodes -subj '/CN=fpmng-ws-input-tls' 2>/dev/null");

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

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WS_INPUT_TLS_PORT') ?: 28199 + 200 * (int) getenv('TEST_PHP_WORKER') + (int) getenv('FPMNG_PHPT_PORT_SHIFT'));
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
http.tls_cert = $root/cert.pem
http.tls_key = $root/key.pem
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

function tlsConnect(int $port)
{
    $fp = @stream_socket_client("ssl://127.0.0.1:$port", $errno, $error, 5,
        STREAM_CLIENT_CONNECT, stream_context_create(['ssl' => [
            'verify_peer' => false, 'verify_peer_name' => false, 'SNI_enabled' => false,
        ]]));
    if (!$fp) throw new RuntimeException("TLS connect :$port: $error");
    stream_set_timeout($fp, 10);
    return $fp;
}

function readStatus($fp): string
{
    $line = fgets($fp);
    if (!$line || !str_starts_with($line, 'HTTP/')) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = trim($line);
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        /* skip headers */
    }
    return $status;
}

/* One ordinary request over TLS on its own connection; returns the body. */
function tlsGet(int $port, string $path): string
{
    $fp = tlsConnect($port);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $line = fgets($fp);
    check(is_string($line) && str_starts_with($line, 'HTTP/1.1 200'), "GET $path: " . var_export($line, true));
    while (($header = fgets($fp)) !== false && $header !== "\r\n") {
        /* skip headers */
    }
    $body = (string) stream_get_contents($fp);
    fclose($fp);
    return $body;
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $ws = tlsConnect($port);
    $key = 'dGhlIHNhbXBsZSBub25jZQ==';
    fwrite($ws, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\n\r\n");
    $status = readStatus($ws);
    check(str_starts_with($status, 'HTTP/1.1 101'), "101 expected: $status");
    echo "upgrade-101-over-tls: ok\n";

    /* The burst: 14 blocking writes of 5000 bytes. The worker does not read
     * during the burst, so the writes complete only because the kernel holds
     * the ciphertext. */
    $sent = 0;
    for ($i = 0; $i < $total / $record; $i++) {
        $n = fwrite($ws, str_repeat('x', $record));
        check($n === $record, "burst write $i wrote " . var_export($n, true));
        $sent += $n;
    }
    usleep(300000);

    /* The worker drains what the bound allowed, then keeps asking. The peer
     * sends nothing after the burst. */
    $drained = (int) tlsGet($port, '/drain');
    check($drained > 0, "no input reached the worker (sent $sent)");
    echo "first-drain-nonzero: ok\n";

    /* resume reading, then poll until every byte has arrived */
    tlsGet($port, '/resume');
    $got = 0;
    for ($i = 0; $i < 200; $i++) {
        $got = (int) tlsGet($port, '/total');
        if ($got === $total) {
            break;
        }
        usleep(50000);
    }
    check($got === $total, "worker received $got of $total bytes with the peer silent (sent $sent)");
    echo "all-bytes-delivered-over-tls: ok\n";

    fclose($ws);
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink("$root/worker.php");
    @unlink("$root/cert.pem");
    @unlink("$root/key.pem");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
upgrade-101-over-tls: ok
first-drain-nonzero: ok
all-bytes-delivered-over-tls: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
