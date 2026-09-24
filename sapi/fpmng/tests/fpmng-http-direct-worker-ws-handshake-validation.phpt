--TEST--
fpm-ng: worker WebSocket upgrade validates RFC6455 headers and returns 426 for unsupported versions (issue #457)
--SKIPIF--
<?php
include "skipif.inc";
if (!getenv('FPMNG_TEST_REPO_ROOT')) die('skip run through build/run-fpmng-phpt.sh');
?>
--ENV--
TEST_TIMEOUT=30
--FILE--
<?php
require_once "tester.inc";

function checkHandshake(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}
function wsHandshake(int $port, string $headers): array
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    checkHandshake(is_resource($fp), "connect failed: $errstr");
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET /ws HTTP/1.1\r\nHost: test\r\n$headers\r\n\r\n");
    $status = fgets($fp);
    checkHandshake(is_string($status) && str_starts_with($status, 'HTTP/1.1 '),
        'bad response status line: ' . var_export($status, true));
    $responseHeaders = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        if (str_contains($line, ':')) {
            [$name, $value] = explode(':', $line, 2);
            $responseHeaders[strtolower(trim($name))] = trim($value);
        }
    }
    fclose($fp);
    return [trim($status), $responseHeaders];
}
function expectHandshakeStatus(string $name, array $response, int $code): void
{
    checkHandshake(str_starts_with($response[0], "HTTP/1.1 $code "), "$name status: {$response[0]}");
    echo "$name: $code\n";
}

$repo = getenv('FPMNG_TEST_REPO_ROOT');
$work = sys_get_temp_dir() . '/fpmng-ws-handshake-' . getmypid();
@mkdir($work, 0700, true);
$source = "$repo/examples/http-direct-worker-ws/app.php";
if (!is_file($source) || !copy($source, "$work/app.php")) {
    throw new RuntimeException("cannot copy shipped WebSocket example from $source");
}

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WS_HANDSHAKE_PORT') ?: 28158);
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[ws]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $work
http.front_controller = /app.php
http.read_timeout = 5000
php_admin_value[max_execution_time] = 0
CFG;

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $key = 'dGhlIHNhbXBsZSBub25jZQ==';
    [$status, ] = wsHandshake($port,
        "Upgrade: websocket\r\nConnection: keep-alive\r\nSec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13");
    expectHandshakeStatus('missing Connection Upgrade token', [$status, []], 400);

    [$status, $headers] = wsHandshake($port,
        "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 8");
    expectHandshakeStatus('unsupported version', [$status, $headers], 426);
    checkHandshake(($headers['sec-websocket-version'] ?? '') === '13',
        '426 response did not advertise Sec-WebSocket-Version: 13');
    echo "426-advertises-version-13: ok\n";

    [$status, $headers] = wsHandshake($port,
        "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: $key");
    expectHandshakeStatus('missing version', [$status, $headers], 426);
    checkHandshake(($headers['sec-websocket-version'] ?? '') === '13',
        'missing-version 426 did not advertise version 13');

    foreach (['short', base64_encode('12345678'), 'dGhlIHNhbXBsZSBub25jZR=='] as $badKey) {
        [$status, ] = wsHandshake($port,
            "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: $badKey\r\nSec-WebSocket-Version: 13");
        expectHandshakeStatus('invalid key ' . $badKey, [$status, []], 400);
    }

    [$status, $headers] = wsHandshake($port,
        "Upgrade: WebSocket\r\nConnection: keep-alive, uPgRaDe\r\nSec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13");
    checkHandshake(str_starts_with($status, 'HTTP/1.1 101'), "valid handshake: $status");
    checkHandshake(($headers['sec-websocket-accept'] ?? '') ===
        base64_encode(sha1($key . '258EAFA5-E914-47DA-95CA-C5AB0DC85B11', true)),
        'valid handshake Sec-WebSocket-Accept mismatch');
    echo "valid-control: 101\n";

    $ordinary = @file_get_contents("http://127.0.0.1:$port/health");
    checkHandshake(is_string($ordinary) && str_contains($ordinary, 'hello from pid'),
        'worker did not remain available after refused handshakes');
    echo "worker-survives-invalid-handshakes: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$work/app.php");
    @rmdir($work);
}
?>
--EXPECT--
missing Connection Upgrade token: 400
unsupported version: 426
426-advertises-version-13: ok
missing version: 426
invalid key short: 400
invalid key MTIzNDU2Nzg=: 400
invalid key dGhlIHNhbXBsZSBub25jZR==: 400
valid-control: 101
worker-survives-invalid-handshakes: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-ws-handshake-*') as $dir) {
    if (@filemtime($dir) > $stale) continue;
    @unlink("$dir/app.php");
    @rmdir($dir);
}
?>
