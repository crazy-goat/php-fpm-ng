--TEST--
fpm-ng: a client that never reads its response is dropped at http.read_timeout, the worker survives, and fw.unflushed does not leak (issue #336)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* fpm_http_direct_worker.c tracks every reply handed to libevent in
 * fw.unflushed until it is either written or the connection dies
 * (fpm_worker_reply_aborted(), wired through evhttp_connection_set_closecb()
 * in fpm_worker_count_reply()) -- see the comment on fpm_worker_reply_aborted:
 * "from the first aborted request on, every shutdown would miss the ...
 * fast path" if this decrement were ever missed. This test drives exactly
 * that: a client that connects, sends a request, and then never reads a
 * single byte of the (large) response. evhttp's own connection timeout
 * (http.read_timeout, set on both directions of the connection by
 * evhttp_set_timeout_tv() in fpm_http_direct_worker.c) eventually drops the
 * connection out from under the unread reply, which is what has to trigger
 * the abort path rather than a normal completion.
 *
 * The regression this guards: if fw.unflushed leaked upward from that one
 * abandoned reply, a LATER, entirely unrelated shutdown (pm.max_requests
 * tripping on a normal, fully-read request) would falsely believe a reply
 * was still unwritten, burn the whole FPM_WORKER_FLUSH_BUDGET waiting for it,
 * and log a spurious warning about a response nobody was owed. */
$root = sys_get_temp_dir() . '/fpmng-worker-slow-reader-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        $uri = $env['REQUEST_URI'] ?? '/';
        if (str_starts_with($uri, '/big')) {
            /* Bigger than a single TCP write and than any socket buffer this
             * box is likely to have, so the unread bytes actually stall the
             * connection rather than fitting entirely in kernel buffers --
             * but still under FPM_WORKER_BODY_MAX (8 MiB), which
             * fpmng_worker_respond() enforces on its one-shot buffered body. */
            fpmng_worker_respond($id, 200, ['Content-Type' => 'application/octet-stream'],
                str_repeat('x', 6 * 1024 * 1024));
            continue;
        }
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'hello from pid ' . getmypid());
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_SLOW_READER_PORT') ?: 28106);
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
http.read_timeout = 700
http.max_body = 1M
catch_workers_output = yes
; 4, not 3: the worker answers hello (1), the slow /big reply (2), and the
; "did the worker survive" recheck (3) before the intentionally-counted
; "request that trips pm.max_requests" below is even sent -- it has to be
; the 4th request for that comment (and the same-pid assertion on it) to hold.
pm.max_requests = 4
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

function httpGetRetry(string $url, int $attempts = 50): string
{
    for ($i = 0; $i < $attempts; $i++) {
        $body = @file_get_contents($url);
        if (is_string($body) && $body !== '') {
            return $body;
        }
        usleep(100000);
    }
    return '';
}

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $hello = file_get_contents("http://127.0.0.1:$port/");
    check(is_string($hello) && str_starts_with($hello, 'hello from pid '), 'hello: ' . var_export($hello, true));

    /* Request 2 of pm.max_requests = 3: a slow reader that never reads its
     * response at all. */
    $slow = connect($port);
    fwrite($slow, "GET /big HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    /* Long enough to clear http.read_timeout (700 ms) several times over,
     * short enough that a genuinely stuck worker still fails this test in
     * reasonable time. */
    usleep(1500000);

    /* The worker survives having an unread reply time out from under it --
     * it must still answer a fresh, well-behaved request on a NEW connection
     * with the SAME pid, i.e. it did not crash or get recycled by this
     * alone. */
    $stillAlive = file_get_contents("http://127.0.0.1:$port/");
    check($stillAlive === $hello, "the worker did not survive the slow reader: " . var_export($stillAlive, true));
    echo "worker-survives-slow-reader: ok\n";

    /* A read of just a few bytes proves nothing here: the status line and
     * headers (and maybe the first chunk of the 6 MiB body) can already be
     * sitting in this process's own kernel receive buffer regardless of
     * what the server does next, since nothing has drained that buffer so
     * far. What actually proves the connection was torn down out from
     * under the reply -- rather than the full 6 MiB eventually arriving --
     * is draining the socket until it truly stops delivering data (EOF, or
     * this read timeout elapses with nothing further coming) and checking
     * far less than the full body made it across. */
    stream_set_timeout($slow, 5);
    $received = '';
    while (!feof($slow)) {
        $chunk = fread($slow, 65536);
        if ($chunk === false || $chunk === '') {
            $meta = stream_get_meta_data($slow);
            if ($meta['timed_out'] || $chunk === false) {
                break;
            }
            if ($chunk === '') {
                break;
            }
        }
        $received .= $chunk;
        if (strlen($received) > 7 * 1024 * 1024) {
            break;
        }
    }
    check(strlen($received) < 6 * 1024 * 1024,
        'the slow connection delivered the full response instead of being aborted: ' . strlen($received) . ' bytes');
    fclose($slow);
    echo "slow-connection-closed: ok\n";

    /* Request 3 of pm.max_requests = 3: a normal, fully-read request that
     * trips the limit. Timed to prove the shutdown took the fast path
     * (FPM_WORKER_FLUSH_BUDGET is 1 second; a leaked fw.unflushed would burn
     * the whole thing waiting for a reply nobody was owed) and the log has
     * no spurious "still unwritten" warning caused by the earlier abort. */
    $started = microtime(true);
    $tripping = file_get_contents("http://127.0.0.1:$port/");
    check($tripping === $hello, 'the request that trips pm.max_requests: ' . var_export($tripping, true));

    $again = httpGetRetry("http://127.0.0.1:$port/");
    $elapsed = microtime(true) - $started;
    check(str_starts_with($again, 'hello from pid '), 'no respawn after pm.max_requests: ' . var_export($again, true));
    check($again !== $hello, 'the worker did not actually restart: ' . $again);
    check($elapsed < 3.0, "recycling after pm.max_requests took suspiciously long ($elapsed s) -- "
        . "fw.unflushed may have leaked from the earlier aborted reply");
    echo "recycle-after-abort-is-not-delayed: ok\n";

    $tester->expectNoLogPattern('/response\(s\) were still unwritten/', true);
    echo "no-spurious-unwritten-warning: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
worker-survives-slow-reader: ok
slow-connection-closed: ok
recycle-after-abort-is-not-delayed: ok
no-spurious-unwritten-warning: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
