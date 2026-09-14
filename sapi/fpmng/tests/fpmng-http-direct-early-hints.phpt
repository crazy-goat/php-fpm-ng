--TEST--
fpm-ng: fpm_send_early_hints() sends a 103 ahead of the final response with intact wire framing (issue #63)
--SKIPIF--
<?php include "fpmng-skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$root = sys_get_temp_dir() . '/fpmng-direct-early-hints-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/front.php", <<<'PHP'
<?php
$mode = $_GET['mode'] ?? 'plain';
if ($mode === 'basic') {
    $ok = fpm_send_early_hints([
        'Link' => ['</style.css>; rel=preload', '</app.js>; rel=preload'],
        /* Not an RFC 9110 token (embedded space): silently dropped, must not
         * fail the call or reach the wire (issue #102's rule, reused here). */
        'X Bad Name' => 'nope',
    ]);
    header('X-Early-Result: ' . var_export($ok, true));
    echo 'final-body';
} elseif ($mode === 'bodyless') {
    fpm_send_early_hints(['Link' => '</a>']);
    http_response_code(204);
} elseif ($mode === 'http10') {
    /* HTTP/1.0 has no notion of a 1xx interim response; the call must return
     * false and write nothing, or the client below would see a stray 103
     * line ahead of the only status line an HTTP/1.0 response is allowed. */
    $ok = fpm_send_early_hints(['Link' => '</a>']);
    header('X-Early-Result: ' . var_export($ok, true));
    echo 'final-body';
} else {
    echo 'plain';
}
PHP);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $ok = fpm_send_early_hints(['Link' => '</a>']);
        fpmng_worker_respond($id, 200, ['X-Early-Result' => var_export($ok, true)], 'worker-body');
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$port = $base + 70;
$portWorker = $base + 71;
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
http.read_timeout = 10000
[worker]
listen = 127.0.0.1:$portWorker
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
php_admin_value[max_execution_time] = 0
CFG;

function connect(int $port, float $timeout = 5.0)
{
    for ($i = 0; $i < 50; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, $timeout);
        if ($fp) {
            stream_set_timeout($fp, 10);
            return $fp;
        }
        usleep(100000);
    }
    throw new RuntimeException("connect $port: $error");
}

/* One HTTP message off the wire: the status line, the header block (folded
 * into arrays since Link is repeated), and -- for anything that is not a 1xx
 * interim response -- the body, read strictly by Content-Length so a short
 * or over-long body is a hard failure rather than a hang. A 1xx never carries
 * a body (RFC 9110 §15.2), so this must not try to read one for it, or a
 * final response written right behind it on the same wire would be misread
 * as that body instead of its own message. */
function readMessage($fp): array
{
    $line = fgets($fp);
    if (!$line || !preg_match('#^HTTP/1\.1 (\d+) ([^\r\n]*)\r\n$#', $line, $m)) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = (int) $m[1];
    $reason = rtrim($m[2]);
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
    $length = isset($headers['content-length']) ? (int) $headers['content-length'][0] : 0;
    $body = '';
    while (strlen($body) < $length) {
        $chunk = fread($fp, $length - strlen($body));
        if ($chunk === false || $chunk === '') {
            throw new RuntimeException('short body');
        }
        $body .= $chunk;
    }
    return [$status, $reason, $headers, $body];
}

function fetch($fp, string $path): array
{
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: test\r\n\r\n");
    return readMessage($fp);
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* 1. Wire order: the 103 arrives first, byte-for-byte a well-formed
     * interim response, immediately followed by the final 200 -- both on the
     * one connection, which then keeps working (issue #63's own "framing
     * stays intact" requirement). */
    $fp = connect($port);
    [$status1, $reason1, $headers1, $body1] = fetch($fp, '/front.php?mode=basic');
    check($status1 === 103 && $reason1 === 'Early Hints', "expected 103 Early Hints, got $status1 $reason1");
    check($body1 === null, 'a 103 carried a body');
    check(($headers1['link'] ?? []) === ['</style.css>; rel=preload', '</app.js>; rel=preload'],
        'Link headers missing/wrong on the 103: ' . var_export($headers1['link'] ?? null, true));
    check(!isset($headers1['x bad name']) && !isset($headers1['x-bad-name']),
        'a non-token header name reached the 103 wire');
    [$status2, , $headers2, $body2] = readMessage($fp);
    check($status2 === 200, "expected 200 after the 103, got $status2");
    check($body2 === 'final-body', "final body corrupted: " . var_export($body2, true));
    check(($headers2['x-early-result'] ?? [null])[0] === 'true',
        'fpm_send_early_hints() did not report success for a call with a valid header');
    echo "early-hints-wire-order: ok\n";

    /* Same connection, one more request: the two-message exchange above did
     * not leave anything extra on the wire for the next request to trip on. */
    [$status3, , , $body3] = fetch($fp, '/front.php');
    check($status3 === 200 && $body3 === 'plain', 'connection corrupted after the 103 exchange');
    echo "connection-reusable-after-early-hints: ok\n";
    fclose($fp);

    /* 2. Early hints ahead of a bodyless (204) final response: the framing
     * task 054 established for HEAD/204/205/304 still holds with a 103 in
     * front of it, and the connection is still good afterwards. */
    $fp = connect($port);
    [$status4, , , $body4] = fetch($fp, '/front.php?mode=bodyless');
    check($status4 === 103, "expected the 103 first, got $status4");
    [$status5, , $headers5, $body5] = readMessage($fp);
    check($status5 === 204, "expected 204 after the 103, got $status5");
    check($body5 === '', '204 response carried a body');
    [$status6, , , $body6] = fetch($fp, '/front.php');
    check($status6 === 200 && $body6 === 'plain', 'connection corrupted after early-hints + bodyless response');
    echo "early-hints-with-bodyless-response: ok\n";
    fclose($fp);

    /* 3. HTTP/1.0: no 1xx concept at all, so the call must be a silent no-op
     * -- no stray 103 line ahead of the one status line HTTP/1.0 expects. */
    $fp = connect($port);
    fwrite($fp, "GET /front.php?mode=http10 HTTP/1.0\r\nHost: test\r\n\r\n");
    $line = fgets($fp);
    check(str_starts_with((string) $line, 'HTTP/1.0 200 '), "HTTP/1.0 request got a stray 103: " . var_export($line, true));
    fclose($fp);
    echo "early-hints-refused-on-http10: ok\n";

    /* 4. pool.executor = worker: the defined "unsupported" answer, same
     * shape fpm_connection_info() uses on this executor -- false, and the
     * request the script still answers is otherwise unaffected. */
    $fp = connect($portWorker);
    [$status7, , $headers7, $body7] = fetch($fp, '/worker.php');
    check($status7 === 200 && $body7 === 'worker-body', 'worker pool response corrupted');
    check(($headers7['x-early-result'] ?? [null])[0] === 'false',
        'fpm_send_early_hints() did not answer false on pool.executor = worker');
    fclose($fp);
    echo "worker-executor-unsupported: ok\n";

    $tester->expectNoLogPattern('/ERROR:/', true);
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    array_map('unlink', glob("$root/*") ?: []);
    @rmdir($root);
}
?>
--EXPECT--
early-hints-wire-order: ok
connection-reusable-after-early-hints: ok
early-hints-with-bodyless-response: ok
early-hints-refused-on-http10: ok
worker-executor-unsupported: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
