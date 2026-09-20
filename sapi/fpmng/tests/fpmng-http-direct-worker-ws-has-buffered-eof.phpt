--TEST--
fpm-ng: worker ws fpmng_worker_stream_has_buffered() is true at the EOF wakeup while a deferred read can still drain the frame (issue #456)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #456, the deferred-read shape. The predicate lied at the EOF wakeup:
 * fpm_ws_eventcb sets ctx->eof and THEN fires the read watcher, with the
 * peer's frame bytes still unread in the input evbuffer, so `!ctx->eof && …`
 * reported "nothing buffered" while fread() would still have returned the
 * frame. A codec that defers its read across the EOF tick and gates on the
 * predicate (the PoC's Fiber/backpressure case) then drops the frame — here,
 * the peer's data frame, which is the same shape as a dropped close frame.
 *
 * The worker below deliberately does NOT read in the read watcher: it arms a
 * short one-shot timer and reads only there, gating on the predicate. By then
 * the FIN has arrived and ctx->eof is set. On the broken code the predicate is
 * false and the frame is dropped; on the fixed code it is true and the frame is
 * echoed.
 *
 * The immediate-read shape (fpmng-http-direct-worker-ws.phpt) cannot show the
 * bug: libevent delivers the data pass and the EOF pass separately there and
 * the codec drains in between. */

$root = sys_get_temp_dir() . '/fpmng-ws-hb-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
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
$ws = null;
$wsWatcher = null;
$readTimer = null;

function handleFrames($ws, string $data): void
{
    foreach (wsDecode($data) as $frame) {
        if ($frame['op'] === 0x1 || $frame['op'] === 0x2) {
            fwrite($ws, wsEncode($frame['data'], $frame['op']));
        }
    }
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$ws, &$wsWatcher, &$readTimer): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $ws = fpmng_worker_upgrade($id, []);
        $wsWatcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $ws, function () use (&$ws, &$readTimer): void {
            if ($ws === null || $readTimer !== null) {
                return;
            }
            /* Defer the read: arm a timer and read only there, gating on the
             * predicate. The FIN lands before the timer fires. */
            $readTimer = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use (&$ws, &$readTimer): void {
                fpmng_worker_event_free($readTimer);
                $readTimer = null;
                if ($ws === null) {
                    return;
                }
                if (!fpmng_worker_stream_has_buffered($ws)) {
                    /* The predicate claims nothing is buffered. On the broken
                     * code that is a lie at the EOF wakeup and the frame is
                     * lost here. */
                    return;
                }
                $data = '';
                while (($chunk = fread($ws, 8192)) !== false && $chunk !== '') {
                    $data .= $chunk;
                }
                if ($data !== '') {
                    handleFrames($ws, $data);
                }
            });
            fpmng_worker_event_enable($readTimer, 0.15);
        });
        fpmng_worker_event_enable($wsWatcher);
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit() || $ws !== null) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WS_HB_PORT') ?: 28146);
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
        throw new RuntimeException('no frame (the deferred read dropped it)');
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

    $ws = connect($port);
    fwrite($ws, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    [$status] = readHead($ws);
    check(str_starts_with($status, 'HTTP/1.1 101'), "101 expected: $status");
    echo "upgrade-101: ok\n";

    /* Send one frame and half-close: the FIN reaches the worker while the frame
     * bytes are still unread. The deferred read then runs at the EOF wakeup. */
    fwrite($ws, wsFrame('still here'));
    stream_socket_shutdown($ws, STREAM_SHUT_WR);

    [$op, $payload] = wsUnframe($ws);
    check($op === 0x1 && $payload === 'still here',
        "deferred read after EOF: op=$op payload=" . var_export($payload, true));
    echo "buffered-at-eof-read: ok\n";

    fclose($ws);
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
upgrade-101: ok
buffered-at-eof-read: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
