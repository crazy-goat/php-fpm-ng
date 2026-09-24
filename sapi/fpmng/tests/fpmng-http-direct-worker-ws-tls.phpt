--TEST--
fpm-ng: fpmng_worker_upgrade() over TLS hijacks the whole SSL bufferevent and every server close sends close_notify (issues #343, #458)
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
                if ($data === '' && feof($ws)) {
                    fclose($ws);
                    $ws = null;
                    return;
                }
                foreach (wsDecode($data) as $frame) {
                    if ($frame['op'] === 0x8) {
                        fwrite($ws, wsEncode(pack('n', 1000), 0x8));
                        fclose($ws);
                        $ws = null;
                        return;
                    }
                    if (($frame['op'] === 0x1 || $frame['op'] === 0x2)
                        && $frame['data'] === 'idle') {
                        fclose($ws);
                        $ws = null;
                        return;
                    }
                    if ($frame['op'] === 0x1 || $frame['op'] === 0x2) {
                        fwrite($ws, wsEncode('tls:' . $frame['data'], $frame['op']));
                    }
                }
            });
            fpmng_worker_event_enable($wsWatcher);
        } else {
            fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], (string) getmypid());
        }
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit() || $ws !== null) {
    fpmng_worker_loop(true);
}
PHP);

function wsClientFrame(string $payload, int $op = 0x1): string
{
    $len = strlen($payload);
    $mask = '1234';
    $masked = '';
    for ($i = 0; $i < $len; $i++) {
        $masked .= $payload[$i] ^ $mask[$i % 4];
    }
    return chr($op | 0x80) . chr(0x80 | $len) . $mask . $masked;
}

function startSClient(int $port): array
{
    $process = proc_open(
        ['openssl', 's_client', '-connect', "127.0.0.1:$port", '-quiet', '-msg'],
        [['pipe', 'r'], ['pipe', 'w'], ['pipe', 'w']],
        $pipes
    );
    check(is_resource($process), 'could not start openssl s_client');
    stream_set_blocking($pipes[1], false);
    stream_set_blocking($pipes[2], false);
    return [$process, $pipes[0], $pipes[1], $pipes[2]];
}

function drainUntil($pipe, string $needle, float $seconds, string &$buffer): bool
{
    $deadline = microtime(true) + $seconds;
    while (microtime(true) < $deadline && !str_contains($buffer, $needle)) {
        $read = [$pipe];
        $write = null;
        $except = null;
        if (stream_select($read, $write, $except, 0, 100000) > 0) {
            $chunk = fread($pipe, 65536);
            if ($chunk === false || $chunk === '') {
                break;
            }
            $buffer .= $chunk;
        }
    }
    return str_contains($buffer, $needle);
}

function waitForCloseNotify(
    $process,
    $stdin,
    $stdout,
    $stderr,
    string $outBuffer,
    float $seconds = 6
): array {
    $errBuffer = '';
    $deadline = microtime(true) + $seconds;
    $pattern = '/<<<[^\r\n]*Alert[^\r\n]*close_notify/i';
    while (microtime(true) < $deadline && !preg_match($pattern, $outBuffer . $errBuffer)) {
        $read = [$stdout, $stderr];
        $write = null;
        $except = null;
        $ready = @stream_select($read, $write, $except, 0, 100000);
        if ($ready === false) {
            break;
        }
        if ($ready === 0) {
            continue;
        }
        foreach ($read as $pipe) {
            $chunk = fread($pipe, 65536);
            if ($chunk === false || $chunk === '') {
                continue;
            }
            if ($pipe === $stdout) {
                $outBuffer .= $chunk;
            } else {
                $errBuffer .= $chunk;
            }
        }
    }
    fclose($stdin);
    for ($i = 0; $i < 20; $i++) {
        $status = proc_get_status($process);
        if (!$status['running']) {
            break;
        }
        usleep(50000);
    }
    $status = proc_get_status($process);
    if ($status['running']) {
        proc_terminate($process);
    }
    proc_close($process);
    return [$outBuffer, $errBuffer];
}

function assertCleanServerClose(string $label, string $out, string $err): void
{
    check((bool) preg_match('/<<<[^\r\n]*Alert[^\r\n]*close_notify/i', $out . $err),
        "$label: no TLS close_notify from server\nstdout: " . var_export($out, true)
        . "\nstderr: " . var_export($err, true));
    check(!preg_match('/unexpected eof while reading|decode_error/i', $out . $err),
        "$label: TLS close was still a truncated record\nstdout: " . var_export($out, true)
        . "\nstderr: " . var_export($err, true));
}

function reserveLocalPort(): int
{
    $server = stream_socket_server('tcp://127.0.0.1:0', $errno, $error);
    check((bool) $server, "reserve control port: $error");
    $name = stream_socket_get_name($server, false);
    fclose($server);
    return (int) substr($name, strrpos($name, ':') + 1);
}

function tlsGet(int $port, string $path): string
{
    $client = @stream_socket_client("ssl://127.0.0.1:$port", $errno, $error, 5,
        STREAM_CLIENT_CONNECT, stream_context_create(['ssl' => [
            'verify_peer' => false, 'verify_peer_name' => false, 'SNI_enabled' => false,
        ]]));
    check((bool) $client, "TLS GET $path: $error");
    stream_set_timeout($client, 5);
    fwrite($client, "GET $path HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $response = (string) stream_get_contents($client);
    fclose($client);
    $headerEnd = strpos($response, "\r\n\r\n");
    return $headerEnd === false ? '' : substr($response, $headerEnd + 4);
}

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
$controlServer = null;
$controlPipes = [];
$fpmStarted = false;
try {
    /* Positive control for the detector. openssl s_server performs a normal
     * TLS shutdown after its one HTTP response, so s_client -msg must show a
     * RECEIVED close_notify. Without this, a detector that silently stopped
     * reading diagnostics would make every negative assertion meaningless. */
    $controlPort = reserveLocalPort();
    $controlServer = proc_open([
        'openssl', 's_server', '-accept', (string) $controlPort, '-naccept', '1', '-www',
        '-cert', "$root/cert.pem", '-key', "$root/key.pem", '-quiet',
    ], [['pipe', 'r'], ['pipe', 'w'], ['pipe', 'w']], $controlPipes);
    check(is_resource($controlServer), 'could not start openssl s_server control');
    usleep(200000);
    [$controlClient, $controlIn, $controlOut, $controlErr] = startSClient($controlPort);
    fwrite($controlIn, "GET / HTTP/1.1\r\nHost: control\r\nConnection: close\r\n\r\n");
    $controlStdout = '';
    drainUntil($controlOut, '</html>', 6, $controlStdout);
    [$controlStdout, $controlStderr] = waitForCloseNotify(
        $controlClient, $controlIn, $controlOut, $controlErr, $controlStdout
    );
    assertCleanServerClose('positive control', $controlStdout, $controlStderr);
    echo "close-notify-detector-control: ok\n";
    foreach ($controlPipes as $pipe) {
        if (is_resource($pipe)) {
            fclose($pipe);
        }
    }
    proc_close($controlServer);
    $controlServer = null;

    $tester->start();
    $fpmStarted = true;
    $tester->expectLogStartNotices();

    $key = 'dGhlIHNhbXBsZSBub25jZQ==';
    $handshake = "GET %s HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\n\r\n";

    /* One connection proves the existing byte pipe, then drives the issue's
     * first close path: a userland close frame gets its 1000 reply queued and
     * fclose() runs before that reply reaches the socket. */
    [$process, $stdin, $stdoutPipe, $stderrPipe] = startSClient($port);
    $stdout = '';
    fwrite($stdin, sprintf($handshake, '/ws-close'));
    check(drainUntil($stdoutPipe, "\r\n\r\n", 6, $stdout), 'close frame: no 101');
    check(str_contains($stdout, 'HTTP/1.1 101 Switching Protocols'), 'close frame: bad status');
    echo "upgrade-over-tls: ok\n";
    fwrite($stdin, wsClientFrame('over tls'));
    check(drainUntil($stdoutPipe, 'tls:over tls', 6, $stdout), 'no TLS frame echo');
    echo "frames-over-tls: ok\n";
    fwrite($stdin, wsClientFrame('', 0x8));
    [$stdout, $stderr] = waitForCloseNotify($process, $stdin, $stdoutPipe, $stderrPipe, $stdout);
    check(str_contains($stdout, "\x88\x02\x03\xe8"), 'close frame: no 1000 reply');
    assertCleanServerClose('close frame', $stdout, $stderr);
    echo "close-frame-tls-notify: ok\n";

    /* Empty-output close: the 101 has drained, userland says close without a
     * frame, and fpm_ws_shutdown_now() must still perform TLS shutdown. */
    [$process, $stdin, $stdoutPipe, $stderrPipe] = startSClient($port);
    $stdout = '';
    fwrite($stdin, sprintf($handshake, '/ws-idle'));
    check(drainUntil($stdoutPipe, "\r\n\r\n", 6, $stdout), 'idle close: no 101');
    check(str_contains($stdout, 'HTTP/1.1 101 Switching Protocols'), 'idle close: bad status');
    fwrite($stdin, wsClientFrame('idle'));
    [$stdout, $stderr] = waitForCloseNotify($process, $stdin, $stdoutPipe, $stderrPipe, $stdout);
    assertCleanServerClose('idle close', $stdout, $stderr);
    echo "idle-close-tls-notify: ok\n";

    /* Retirement uses the same close-after-write tail, but the queued payload is
     * the userland codec's 1001 frame rather than a close-handshake reply. */
    $pidRaw = tlsGet($port, '/pid');
    check(preg_match('/^\d+$/D', $pidRaw) === 1, 'pid probe: ' . var_export($pidRaw, true));
    $pid = (int) $pidRaw;
    [$process, $stdin, $stdoutPipe, $stderrPipe] = startSClient($port);
    $stdout = '';
    fwrite($stdin, sprintf($handshake, '/ws-retire'));
    check(drainUntil($stdoutPipe, "\r\n\r\n", 6, $stdout), 'retire: no 101');
    check(str_contains($stdout, 'HTTP/1.1 101 Switching Protocols'), 'retire: bad status');
    $tester->signal('USR1', $pid);
    [$stdout, $stderr] = waitForCloseNotify($process, $stdin, $stdoutPipe, $stderrPipe, $stdout);
    check(str_contains($stdout, "\x88\x02\x03\xe9"), 'retire: no 1001 reply');
    assertCleanServerClose('retire', $stdout, $stderr);
    echo "retire-tls-notify: ok\n";
} finally {
    if ($fpmStarted) {
        $tester->terminate();
        $tester->expectLogTerminatingNotices();
        $tester->close();
    }
    if (is_resource($controlServer)) {
        $status = proc_get_status($controlServer);
        if ($status['running']) {
            proc_terminate($controlServer);
        }
        foreach ($controlPipes as $pipe) {
            if (is_resource($pipe)) {
                fclose($pipe);
            }
        }
        proc_close($controlServer);
    }
    @unlink("$root/worker.php");
    @unlink("$root/cert.pem");
    @unlink("$root/key.pem");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
close-notify-detector-control: ok
upgrade-over-tls: ok
frames-over-tls: ok
close-frame-tls-notify: ok
idle-close-tls-notify: ok
retire-tls-notify: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
