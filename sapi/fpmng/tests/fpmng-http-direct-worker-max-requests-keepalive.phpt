--TEST--
fpm-ng: pm.max_requests adds Connection: close to the LAST reply on a keep-alive connection, not before or silently after (issue #336)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* fpmng-http-direct-worker-drain.phpt already proves pm.max_requests recycles
 * the worker without losing the response that trips it, but every request in
 * that test is its own connection -- it never exercises keep-alive reuse
 * around the trip point. fpmng_worker_respond()'s "Connection: close the
 * buffered path adds ... when stopping" only makes sense on a connection kept
 * open across more than one request, which this test drives by hand: several
 * requests pipelined one after another on a SINGLE persistent socket, with
 * pm.max_requests set so the trip lands on a specific one of them. */
$root = sys_get_temp_dir() . '/fpmng-worker-max-requests-keepalive-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'hello from pid ' . getmypid());
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_MAX_REQUESTS_KEEPALIVE_PORT') ?: 28105);
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
pm.max_requests = 3
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

/* One request/response on a connection that may be reused afterwards. */
function fetch($fp, string $path): array
{
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: t\r\n\r\n");
    $line = fgets($fp);
    if (!$line || !preg_match('#^HTTP/1\.1 (\d+) #', $line, $m)) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = (int) $m[1];
    $length = 0;
    $closing = false;
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        if (stripos($line, 'Content-Length:') === 0) {
            $length = (int) trim(substr($line, 15));
        }
        if (stripos($line, 'Connection:') === 0 && stripos($line, 'close') !== false) {
            $closing = true;
        }
    }
    $body = '';
    while (strlen($body) < $length) {
        $chunk = fread($fp, $length - strlen($body));
        if ($chunk === false || $chunk === '') {
            break;
        }
        $body .= $chunk;
    }
    return [$status, $body, $closing];
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

    $conn = connect($port);

    /* pm.max_requests = 3: the first two answers on this ONE persistent
     * connection must keep it open, and only the third -- the one that
     * actually trips the limit -- may carry Connection: close. */
    [$status1, $body1, $close1] = fetch($conn, '/');
    check($status1 === 200, "request 1: $status1");
    check(!$close1, 'request 1 (below pm.max_requests) closed the connection early');
    echo "request-1-keeps-connection-open: ok\n";

    [$status2, $body2, $close2] = fetch($conn, '/');
    check($status2 === 200, "request 2: $status2");
    check($body2 === $body1, 'request 2 was served by a different worker: ' . var_export([$body1, $body2], true));
    check(!$close2, 'request 2 (below pm.max_requests) closed the connection early');
    echo "request-2-keeps-connection-open: ok\n";

    [$status3, $body3, $close3] = fetch($conn, '/');
    check($status3 === 200, "request 3 (trips pm.max_requests): $status3");
    check($body3 === $body1, 'request 3 was served by a different worker: ' . var_export([$body1, $body3], true));
    check($close3, 'the response that trips pm.max_requests did not carry Connection: close');
    echo "request-3-trips-limit-and-closes: ok\n";

    /* The connection really is closed server-side afterwards, not merely
     * labelled so: a further read returns EOF. */
    $eof = @fread($conn, 16);
    check($eof === '' || $eof === false, 'the connection stayed open past the response that closed it: ' . var_export($eof, true));
    fclose($conn);
    echo "connection-actually-closed: ok\n";

    /* And the pool keeps working: the master respawned the worker. */
    $again = httpGetRetry("http://127.0.0.1:$port/");
    check(str_starts_with($again, 'hello from pid '), 'no respawn after pm.max_requests: ' . var_export($again, true));
    check($again !== $body1, 'the worker did not actually restart: ' . $again);
    echo "respawn-after-max-requests: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
request-1-keeps-connection-open: ok
request-2-keeps-connection-open: ok
request-3-trips-limit-and-closes: ok
connection-actually-closed: ok
respawn-after-max-requests: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
