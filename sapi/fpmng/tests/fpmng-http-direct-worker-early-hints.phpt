--TEST--
fpm-ng: fpm_send_early_hints() on pool.executor = worker, keyed by request id (issue #335)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$root = sys_get_temp_dir() . '/fpmng-worker-early-hints-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    $uri = $env['REQUEST_URI'] ?? '/';

    if ($uri === '/early') {
        $ok = fpm_send_early_hints(['Link' => '</style.css>; rel=preload'], $id);
        fpmng_worker_respond($id, 200, ['X-Early-Result' => var_export($ok, true)], 'final-body');
        return;
    }
    if ($uri === '/early-http10') {
        $ok = fpm_send_early_hints(['Link' => '</a>'], $id);
        fpmng_worker_respond($id, 200, ['X-Early-Result' => var_export($ok, true)], 'final-body');
        return;
    }
    if ($uri === '/early-noid') {
        $ok = fpm_send_early_hints(['Link' => '</a>']);
        fpmng_worker_respond($id, 200, ['X-Early-Result' => var_export($ok, true)], 'final-body');
        return;
    }
    if ($uri === '/early-badid') {
        $ok = fpm_send_early_hints(['Link' => '</a>'], 999999999);
        fpmng_worker_respond($id, 200, ['X-Early-Result' => var_export($ok, true)], 'final-body');
        return;
    }
    if ($uri === '/early-after-start') {
        /* Once the final response has started going out (streaming), a 103
         * would land mid-response -- refused, same as classic's
         * r->responded || r->streaming guard. */
        $startOk = fpmng_worker_respond_start($id, 200, []);
        $earlyOk = fpm_send_early_hints(['Link' => '</a>'], $id);
        fpmng_worker_respond_chunk($id, 'X-Early-After-Start-Result:' . var_export($earlyOk, true) . ';started:' . var_export($startOk, true));
        fpmng_worker_respond_end($id);
        return;
    }

    fpmng_worker_respond($id, 200, [], 'plain');
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        handle($id);
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_EARLY_HINTS_PORT') ?: 28099);
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

/* One HTTP message off the wire, the same shape
 * fpmng-http-direct-early-hints.phpt's readMessage() uses: a 1xx never
 * carries a body (RFC 9110 section 15.2), so this must not try to read one
 * for it, or the final response written right behind it on the same wire
 * would be misread as that body instead of its own message. */
function readMessage($fp): array
{
    $line = fgets($fp);
    if (!$line || !preg_match('#^HTTP/1\.(\d) (\d+) ([^\r\n]*)\r\n$#', $line, $m)) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = (int) $m[2];
    $reason = rtrim($m[3]);
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        if (!preg_match('#^([^:\r\n]+):[ \t]*(.*)\r\n$#', $line, $hm)) {
            throw new RuntimeException('malformed header line: ' . var_export($line, true));
        }
        $headers[strtolower($hm[1])][] = $hm[2];
    }
    if ($status >= 100 && $status < 200) {
        return [$status, $reason, $headers, null];
    }
    if (isset($headers['transfer-encoding']) && $headers['transfer-encoding'][0] === 'chunked') {
        $body = '';
        while (true) {
            $sizeLine = fgets($fp);
            $size = hexdec(trim((string) $sizeLine));
            if ($size === 0) { fgets($fp); break; }
            $chunk = '';
            while (strlen($chunk) < $size) {
                $part = fread($fp, $size - strlen($chunk));
                if ($part === false || $part === '') throw new RuntimeException('short chunk');
                $chunk .= $part;
            }
            $body .= $chunk;
            fgets($fp); // trailing CRLF after the chunk
        }
        return [$status, $reason, $headers, $body];
    }
    $length = isset($headers['content-length']) ? (int) $headers['content-length'][0] : 0;
    $body = '';
    while (strlen($body) < $length) {
        $chunk = fread($fp, $length - strlen($body));
        if ($chunk === false || $chunk === '') throw new RuntimeException('short body');
        $body .= $chunk;
    }
    return [$status, $reason, $headers, $body];
}

function fetch($fp, string $path, string $version = '1.1'): array
{
    $extra = $version === '1.0' ? "Connection: close\r\n" : '';
    fwrite($fp, "GET $path HTTP/$version\r\nHost: t\r\n$extra\r\n");
    return readMessage($fp);
}

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* 1. A successful early-hints send on HTTP/1.1: the 103 arrives first,
     * byte-for-byte a well-formed interim response, immediately followed by
     * the final 200 on the same connection, which then keeps working. */
    $fp = connect($port);
    [$status1, $reason1, $headers1, $body1] = fetch($fp, '/early');
    check($status1 === 103 && $reason1 === 'Early Hints', "expected 103 Early Hints, got $status1 $reason1");
    check($body1 === null, 'a 103 carried a body');
    check(($headers1['link'] ?? []) === ['</style.css>; rel=preload'], 'Link header missing/wrong on the 103');
    [$status2, , $headers2, $body2] = readMessage($fp);
    check($status2 === 200, "expected 200 after the 103, got $status2");
    check($body2 === 'final-body', 'final body corrupted: ' . var_export($body2, true));
    check(($headers2['x-early-result'] ?? [null])[0] === 'true', 'fpm_send_early_hints($id) did not report success');
    echo "early-hints-wire-order: ok\n";

    [$status3, , , $body3] = fetch($fp, '/plain');
    check($status3 === 200 && $body3 === 'plain', 'connection corrupted after the 103 exchange');
    fclose($fp);
    echo "connection-reusable-after-early-hints: ok\n";

    /* 2. Refused once the final response has already started going out
     * (streaming): the guard is p->streaming, the same flag
     * fpmng_worker_respond()/_start() already use to refuse a second status
     * line for the same request id. */
    $fp = connect($port);
    fwrite($fp, "GET /early-after-start HTTP/1.1\r\nHost: t\r\n\r\n");
    [$status4, , $headers4, $body4] = readMessage($fp);
    check($status4 === 200, "expected 200 (streamed), got $status4");
    check(str_contains((string) $body4, 'X-Early-After-Start-Result:false'),
        'fpm_send_early_hints() was not refused after respond_start(): ' . var_export($body4, true));
    check(str_contains((string) $body4, 'started:true'), 'respond_start() itself unexpectedly failed');
    fclose($fp);
    echo "early-hints-refused-after-response-started: ok\n";

    /* 3. No id (the omitted-argument default, 0): false, unconditionally. */
    $fp = connect($port);
    [, , $headers5,] = fetch($fp, '/early-noid');
    check(($headers5['x-early-result'] ?? [null])[0] === 'false', 'fpm_send_early_hints() with no id should answer false');
    fclose($fp);
    echo "no-id-refused: ok\n";

    /* 4. An id nothing ever handed out: false, the same as "no id". */
    $fp = connect($port);
    [, , $headers6,] = fetch($fp, '/early-badid');
    check(($headers6['x-early-result'] ?? [null])[0] === 'false', 'fpm_send_early_hints() with an unknown id should answer false');
    fclose($fp);
    echo "unknown-id-refused: ok\n";

    /* 5. HTTP/1.0: no notion of a 1xx interim response, so the call must be a
     * silent no-op reported back through the return value -- no stray 103 line
     * ahead of the one status line HTTP/1.0 expects. */
    $fp = connect($port);
    [$status7, , $headers7,] = fetch($fp, '/early-http10', '1.0');
    check($status7 === 200, "HTTP/1.0 request got something other than the final 200: $status7");
    check(($headers7['x-early-result'] ?? [null])[0] === 'false', 'fpm_send_early_hints() did not refuse an HTTP/1.0 client');
    fclose($fp);
    echo "http10-refused: ok\n";

    $tester->expectNoLogPattern('/ERROR:/', true);
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
early-hints-wire-order: ok
connection-reusable-after-early-hints: ok
early-hints-refused-after-response-started: ok
no-id-refused: ok
unknown-id-refused: ok
http10-refused: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
