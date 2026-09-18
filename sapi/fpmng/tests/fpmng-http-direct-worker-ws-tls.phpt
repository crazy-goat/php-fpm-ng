--TEST--
fpm-ng: fpmng_worker_upgrade() over TLS — the SSL bufferevent is hijacked whole, the stream speaks plaintext to the codec (issue #343)
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

/* The point of this file: on a TLS pool the accepted connection's bufferevent
 * is an OpenSSL one, and the hijack must take it WHOLE — the fd carries only
 * ciphertext, so an fd-level handoff is unusable here. The codec below talks
 * plaintext frames; the SSL layer underneath is invisible to it. */

$root = sys_get_temp_dir() . '/fpmng-worker-ws-tls-' . getmypid();
@mkdir($root, 0700, true);

run("openssl req -x509 -newkey rsa:2048 -keyout $root/key.pem -out $root/cert.pem "
    . "-days 2 -nodes -subj '/CN=fpmng-ws-test' 2>/dev/null");

file_put_contents("$root/worker.php", <<<'PHP'
<?php
function wsEncode(string $payload, int $op = 0x1): string
{
    $len = strlen($payload);
    $head = chr($op | 0x80);
    if ($len < 126) {
        $head .= chr($len);
    } else {
        $head .= pack('n', 126) . pack('n', $len);
    }
    return $head . $payload;
}

function wsDecode(string $data): array
{
    $frames = [];
    $off = 0;
    while ($off + 2 <= strlen($data)) {
        $op = ord($data[$off]) & 0x0f;
        $masked = (ord($data[$off + 1]) & 0x80) !== 0;
        $len = ord($data[$off + 1]) & 0x7f;
        $off += 2;
        $mask = $masked ? substr($data, $off, 4) : '';
        $off += $masked ? 4 : 0;
        $payload = substr($data, $off, $len);
        $off += $len;
        if ($masked) {
            for ($i = 0; $i < strlen($payload); $i++) {
                $payload[$i] = $payload[$i] ^ $mask[$i % 4];
            }
        }
        $frames[] = ['op' => $op, 'data' => $payload];
    }
    return $frames;
}

$notify = fpmng_worker_notify_stream();
$ws = null;
$closed = false;

$timer = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use (&$ws, &$timer, &$closed): void {
    if (fpmng_worker_stopping() && $ws !== null && !$closed) {
        $closed = true;
        fwrite($ws, wsEncode(pack('n', 1001), 0x8));
        /* The frame is queued; the write watcher says when it is on the wire
         * (issue #343's "the codec waits" contract). */
        $closeWatcher = fpmng_worker_event_create(FPMNG_WORKER_WRITE, $ws, function () use (&$ws, &$timer): void {
            fpmng_worker_event_free($timer);
            fclose($ws);
            $ws = null;
        });
        fpmng_worker_event_enable($closeWatcher);
        return;
    }
    fpmng_worker_event_enable($timer, 0.1);
});
fpmng_worker_event_enable($timer, 0.1);

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$ws): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        if (str_contains($env['REQUEST_URI'] ?? '/', '/ws')) {
            $ws = fpmng_worker_upgrade($id, []);
            $wsWatcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $ws, function () use (&$ws): void {
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
                foreach (wsDecode($data) as $frame) {
                    if ($frame['op'] === 0x1 || $frame['op'] === 0x2) {
                        fwrite($ws, wsEncode('tls:' . $frame['data'], $frame['op']));
                    }
                }
            });
            fpmng_worker_event_enable($wsWatcher);
        } else {
            fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'tls-hello');
        }
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit() || $ws !== null) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WS_TLS_PORT') ?: 28145);
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

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $client = @stream_socket_client("ssl://127.0.0.1:$port", $errno, $errstr, 5,
        STREAM_CLIENT_CONNECT, stream_context_create(['ssl' => [
            'verify_peer' => false, 'verify_peer_name' => false, 'SNI_enabled' => false,
        ]]));
    check($client !== false, "TLS connect: $errstr");
    stream_set_timeout($client, 10);

    $key = 'dGhlIHNhbXBsZSBub25jZQ==';
    fwrite($client, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\n\r\n");
    $line = fgets($client);
    check($line && str_starts_with($line, 'HTTP/1.1 101'), '101 over TLS: ' . var_export($line, true));
    while (($line = fgets($client)) !== false && $line !== "\r\n");
    echo "upgrade-over-tls: ok\n";

    /* A masked client frame, echoed plaintextly through the SSL bufferevent. */
    $payload = 'over tls';
    $mask = 'abcd';
    $masked = '';
    for ($i = 0; $i < strlen($payload); $i++) {
        $masked .= $payload[$i] ^ $mask[$i % 4];
    }
    fwrite($client, chr(0x81) . chr(0x80 | strlen($payload)) . $mask . $masked);
    $head = fread($client, 2);
    check(strlen((string) $head) === 2 && (ord($head[0]) & 0x0f) === 0x1, 'expected a text frame back');
    $len = ord($head[1]) & 0x7f;
    $body = $len ? fread($client, $len) : '';
    check($body === 'tls:over tls', "tls echo: " . var_export($body, true));
    echo "frames-over-tls: ok\n";

    fclose($client);
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
upgrade-over-tls: ok
frames-over-tls: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
