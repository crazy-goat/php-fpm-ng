--TEST--
fpm-ng: fpmng_worker_upgrade() hijacks the connection into a php_stream; frames echo in userland while ordinary requests are answered; the worker retires with a close frame, not a reset (issue #343)
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

/* Issue #343, acceptance criteria 1-3. The RFC 6455 codec lives HERE and in
 * the worker script -- userland on both ends -- because the C side contributes
 * nothing but the byte pipe: the 101 handshake (with Sec-WebSocket-Accept
 * computed in C), the hijacked bufferevent wrapped as a php_stream, and the
 * watchers bound to it. */

$root = sys_get_temp_dir() . '/fpmng-worker-ws-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
/* The worker-side half of the codec: server frames are unmasked, client
 * frames arrive masked. Ping/pong/close are answered here; everything else
 * would be amphp/websocket-server in a real application. */
function wsEncode(string $payload, int $op = 0x1): string
{
    $len = strlen($payload);
    $head = chr($op | 0x80);
    if ($len < 126) {
        $head .= chr($len);
    } elseif ($len < 65536) {
        $head .= pack('n', 126) . pack('n', $len);
    } else {
        $head .= pack('n', 127) . pack('J', $len);
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
        if ($len === 126) {
            $len = unpack('n', substr($data, $off, 2))[1];
            $off += 2;
        } elseif ($len === 127) {
            $len = unpack('J', substr($data, $off, 8))[1];
            $off += 8;
        }
        $mask = $masked ? substr($data, $off, 4) : '';
        $off += $masked ? 4 : 0;
        $payload = substr($data, $off, $len);
        $off += $len;
        if ($masked) {
            for ($i = 0; $i < strlen($payload); $i++) {
                $payload[$i] = $payload[$i] ^ $mask[$i % 4];
            }
        }
        $frames[] = ['op' => $op, 'data' => $payload, 'end' => $off];
    }
    return $frames;
}

$notify = fpmng_worker_notify_stream();
$ws = null;          /* the upgraded stream, once there is one */
$wsWatcher = null;
$closed = false;

function sendClose($ws, int $code): void
{
    fwrite($ws, wsEncode(pack('n', $code), 0x8));
}

function handleFrames(&$ws, string $data): void
{
    foreach (wsDecode($data) as $frame) {
        if ($frame['op'] === 0x8) {                 /* close */
            sendClose($ws, 1000);
            fclose($ws);
            $ws = null;
            return;
        }
        if ($frame['op'] === 0x9) {                 /* ping -> pong */
            fwrite($ws, wsEncode($frame['data'], 0xA));
            continue;
        }
        if ($frame['op'] === 0x1 || $frame['op'] === 0x2) {
            fwrite($ws, wsEncode($frame['data'], $frame['op']));
        }
    }
}

/* The retire path: on fpmng_worker_stopping() the CODEC sends 1001 Going Away
 * before the loop exits -- C only guarantees the fd is closed (issue #343). */
$timer = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use (&$ws, &$timer, &$closed): void {
    if (fpmng_worker_stopping() && $ws !== null && !$closed) {
        $closed = true;
        sendClose($ws, 1001);
        /* The close frame is queued in the connection's output buffer; the
         * write watcher is what says it reached the wire -- only then may the
         * codec close (issue #343's contract: "the codec waits"). */
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

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$ws, &$wsWatcher): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        $uri = $env['REQUEST_URI'] ?? '/';

        if (str_contains($uri, '/noupgrade')) {
            /* A non-upgrade request must throw, and the request must stay
             * answerable -- answered 400 here, from the catch. */
            try {
                fpmng_worker_upgrade($id, []);
                fpmng_worker_respond($id, 500, [], 'upgrade should have thrown');
            } catch (ValueError) {
                fpmng_worker_respond($id, 400, ['Content-Type' => 'text/plain'], 'not an upgrade');
            }
            continue;
        }
        if (str_contains($uri, '/ws')) {
            $ws = fpmng_worker_upgrade($id, ['Sec-WebSocket-Protocol' => 'chat']);
            $wsWatcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $ws, function () use (&$ws): void {
                if ($ws === null) {
                    return;
                }
                $data = '';
                /* Read until short -- the stream buffers in the hijacked
                 * bufferevent, and a partial read strands the rest until the
                 * next frame arrives (the same rule every stream on this
                 * executor obeys). */
                while (true) {
                    $chunk = fread($ws, 8192);
                    if ($chunk === false || $chunk === '') {
                        break;
                    }
                    $data .= $chunk;
                }
                if ($data !== '') {
                    handleFrames($ws, $data);
                }
            });
            fpmng_worker_event_enable($wsWatcher);
            continue;
        }
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'hello from pid ' . getmypid());
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit() || $ws !== null) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WS_PORT') ?: 28144);
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
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$k, $v] = explode(':', $line, 2);
        $headers[strtolower(trim($k))] = trim($v);
    }
    return [$status, $headers];
}

/* Client frames are masked (RFC 6455 5.3). */
function wsFrame(string $payload, int $op = 0x1): string
{
    $len = strlen($payload);
    $head = chr($op | 0x80);
    if ($len < 126) {
        $head .= chr(0x80 | $len);
    } elseif ($len < 65536) {
        $head .= pack('n', 0x80 | 126) . pack('n', $len);
    } else {
        $head .= pack('n', 0x80 | 127) . pack('J', $len);
    }
    $mask = '1234';
    $masked = '';
    for ($i = 0; $i < $len; $i++) {
        $masked .= $payload[$i] ^ $mask[$i % 4];
    }
    return $head . $mask . $masked;
}

function wsUnframe($fp): array
{
    $head = fread($fp, 2);
    if ($head === false || strlen($head) < 2) {
        throw new RuntimeException('connection reset instead of a frame');
    }
    $op = ord($head[0]) & 0x0f;
    $len = ord($head[1]) & 0x7f;
    if ($len === 126) {
        $len = unpack('n', fread($fp, 2))[1];
    } elseif ($len === 127) {
        $len = unpack('J', fread($fp, 8))[1];
    }
    $payload = $len ? fread($fp, $len) : '';
    if (strlen((string) $payload) < $len) {
        throw new RuntimeException('short frame');
    }
    return [$op, (string) $payload];
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* The upgrade: 101, an Accept header matching base64(sha1(key.GUID)),
     * and the Sec-WebSocket-Protocol header passed through response_headers. */
    $ws = connect($port);
    $key = 'dGhlIHNhbXBsZSBub25jZQ==';
    fwrite($ws, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\n\r\n");
    [$status, $headers] = readHead($ws);
    check(str_starts_with($status, 'HTTP/1.1 101'), "101 expected: $status");
    check(($headers['sec-websocket-accept'] ?? '') === base64_encode(sha1($key . '258EAFA5-E914-47DA-95CA-C5AB0DC85B11', true)),
        'Sec-WebSocket-Accept wrong: ' . var_export($headers['sec-websocket-accept'] ?? null, true));
    check(($headers['sec-websocket-protocol'] ?? '') === 'chat',
        'Sec-WebSocket-Protocol from response_headers missing: ' . var_export($headers, true));
    echo "upgrade-101-accept: ok\n";

    /* Frames echo while ordinary requests on the SAME worker are answered. */
    fwrite($ws, wsFrame('hello'));
    fwrite($ws, wsFrame(hex2bin('00ff10'), 0x2));
    $probe = connect($port);
    fwrite($probe, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    [$pstatus, ] = readHead($probe);
    $body = stream_get_contents($probe);
    fclose($probe);
    check(str_starts_with($pstatus, 'HTTP/1.1 200') && str_starts_with((string) $body, 'hello from pid '),
        'ordinary request while a ws is held: ' . var_export($pstatus, true) . ' / ' . var_export($body, true));
    echo "ordinary-request-answered: ok\n";

    [$op, $payload] = wsUnframe($ws);
    check($op === 0x1 && $payload === 'hello', "text echo: op=$op payload=" . var_export($payload, true));
    [$op, $payload] = wsUnframe($ws);
    check($op === 0x2 && $payload === hex2bin('00ff10'), "binary echo: op=$op");
    echo "frames-echo: ok\n";

    /* Client-gone: fclose on the stream side is the teardown path; from the
     * client side, a close handshake ends with EOF, not a reset. */
    fwrite($ws, wsFrame('', 0x8));
    [$op, $payload] = wsUnframe($ws);
    check($op === 0x8, 'expected the close reply: op=' . $op);
    check(feof($ws) || fread($ws, 1) === '', 'connection not closed after the close handshake');
    fclose($ws);
    echo "close-handshake: ok\n";

    /* Retire: a NEW ws is opened, then the worker is retired -- the client
     * must see a 1001 close frame from the codec, never a reset. */
    $ws2 = connect($port);
    fwrite($ws2, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\n\r\n");
    [$status] = readHead($ws2);
    check(str_starts_with($status, 'HTTP/1.1 101'), "second upgrade: $status");
    $probeBody = @file_get_contents("http://127.0.0.1:$port/");
    check(is_string($probeBody) && preg_match('/(\d+)/', $probeBody, $m) === 1,
        'pid probe failed: ' . var_export($probeBody, true));
    $pid = (int) $m[1];
    $tester->signal('USR1', $pid);
    [$op, $payload] = wsUnframe($ws2);
    check($op === 0x8 && unpack('n', $payload)[1] === 1001,
        'retire: expected a 1001 close frame, got op=' . $op . ' len=' . strlen($payload));
    echo "retire-close-frame: ok\n";
    fclose($ws2);

    /* Non-upgrade requests throw and stay answerable. */
    $plain = connect($port);
    fwrite($plain, "GET /noupgrade HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    [$pstatus, ] = readHead($plain);
    $body = stream_get_contents($plain);
    fclose($plain);
    check(str_starts_with($pstatus, 'HTTP/1.1 400') && $body === 'not an upgrade',
        'non-upgrade request: ' . var_export($pstatus, true) . ' / ' . var_export($body, true));
    echo "non-upgrade-throws-and-answers: ok\n";
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
upgrade-101-accept: ok
ordinary-request-answered: ok
frames-echo: ok
close-handshake: ok
retire-close-frame: ok
non-upgrade-throws-and-answers: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
