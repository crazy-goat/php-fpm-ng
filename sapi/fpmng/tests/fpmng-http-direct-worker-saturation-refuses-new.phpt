--TEST--
fpm-ng: worker.max_pending saturation refuses a NEW request on an already-open keep-alive connection too (issue #336)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* fpmng-http-direct-worker-max-pending.phpt (issue #331) already proves that a
 * request beyond worker.max_pending's ready_count/ready_max ceiling gets 503,
 * and that the two accepted-but-unanswered holds are drained with 503 once the
 * worker stops. It never proves the OTHER half of fpm_worker_accept()'s
 * saturation condition: `fpm_worker_stopping || ready_count >= ready_max`. The
 * first disjunct is what protects a request that arrives on an ALREADY OPEN
 * keep-alive connection during the window between the ceiling being hit
 * (fpm_worker_stopping = 1) and the listener actually being torn down in
 * fpm_worker_finish_output() -- that connection is not one of the held
 * requests, occupies no pending slot, and would otherwise be silently
 * dispatched to a worker that has already decided to recycle. This test drives
 * that path directly. */
function openHeld(string $host, int $port, string $uri)
{
    $sock = stream_socket_client("tcp://$host:$port", $errno, $errstr, 5);
    check($sock !== false, "connect failed: $errstr");
    fwrite($sock, "GET $uri HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    return $sock;
}

function readStatus($sock): string
{
    $line = fgets($sock);
    if (!$line || !preg_match('{^HTTP/\d\.\d (\d+)}', $line, $m)) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    /* Drain headers, then the body by Content-Length, so a following read on
     * the same keep-alive connection starts cleanly at the next status line
     * instead of mid-body. */
    $length = 0;
    while (($l = fgets($sock)) !== false && $l !== "\r\n") {
        if (stripos($l, 'Content-Length:') === 0) {
            $length = (int) trim(substr($l, 15));
        }
    }
    $left = $length;
    while ($left > 0) {
        $chunk = fread($sock, $left);
        if ($chunk === false || $chunk === '') {
            break;
        }
        $left -= strlen($chunk);
    }
    return $m[1];
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

$root = sys_get_temp_dir() . '/fpmng-worker-saturation-new-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", str_replace('__ROOT__', $root, <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    $uri = $env['REQUEST_URI'] ?? '/';

    if (str_starts_with($uri, '/hold')) {
        /* Tell the test this hold has really reached the script. */
        file_put_contents('__ROOT__/hold-seen', '1');
        return; /* deliberately never call fpmng_worker_respond() */
    }
    fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'hello from pid ' . getmypid());
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        handle($id);
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit() && !fpmng_worker_stopping()) {
    fpmng_worker_loop(true);
}
/* Keep driving the event loop after the stop request until the test has read
 * the answer on the idle keep-alive connection (marker file), bounded by 10 s,
 * so that however late the test process is scheduled, the request it writes
 * is read while fpm_worker_stopping is already set (issue #527). Returning at
 * once would leave a window of a single event-loop iteration. */
$until = microtime(true) + 10.0;
while (microtime(true) < $until && !file_exists('__ROOT__/release')) {
    fpmng_worker_loop(false);
    usleep(5000);
}
PHP));

$port = (int) (getenv('FPMNG_DIRECT_WORKER_SATURATION_NEW_PORT') ?: 28101 + 200 * (int) getenv('TEST_PHP_WORKER') + (int) getenv('FPMNG_PHPT_PORT_SHIFT'));

$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[cap]
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
worker.max_pending = 1
; issue #338: the default worker.accept_threshold = 1 rate-limits accepts and
; could delay the second hold; 0 accepts at once. The saturation window itself
; is opened explicitly by the test (marker file, then holdB's 503), not by this
; setting.
worker.accept_threshold = 0
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* A keep-alive connection, established and answered BEFORE saturation --
     * it must not be treated as one of the held requests below. */
    $idle = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    check($idle !== false, "idle connect failed: $errstr");
    fwrite($idle, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    $status = readStatus($idle);
    check($status === '200', "idle connection's first request: $status");
    echo "idle-connection-served: ok\n";

    /* Trip worker.max_pending = 1: one held request fills the only slot, a
     * second concurrent one is rejected on the ready_count >= ready_max
     * disjunct (already covered by fpmng-http-direct-worker-max-pending.phpt)
     * and also sets fpm_worker_stopping. */
    $holdA = openHeld('127.0.0.1', $port, '/hold');
    /* Wait until holdA has really reached the worker script, so the second
     * hold below cannot be seen before it, however the scheduler orders the
     * test and the worker (issue #527). */
    for ($i = 0; $i < 1000 && !file_exists("$root/hold-seen"); $i++) {
        usleep(10000);
    }
    check(file_exists("$root/hold-seen"), 'hold A never reached the worker script');
    $holdB = openHeld('127.0.0.1', $port, '/hold');

    /* holdB's 503 proves fpm_worker_stopping is set. Only then is the
     * idle-connection request written, so it is always dispatched while the
     * worker is stopping (the script keeps the event loop running until
     * "release" exists). */
    $statusB = readStatus($holdB);
    check($statusB === '503', "the 2nd concurrent hold did not get 503: $statusB");
    fclose($holdB);

    $tester->expectLogPattern('/WARNING: .*\[pool cap\] http-direct worker: 1 requests accepted but unanswered; '
        . 'answering 503 and asking the worker script to stop so the master can respawn it/', true);

    /* The load-bearing write: a SECOND request on the connection that was
     * already open and idle before saturation -- not one of the held sockets,
     * not a new TCP connection -- must also be refused 503 rather than being
     * handed to a worker that has already decided to stop. This is the
     * fpm_worker_stopping disjunct of fpm_worker_accept()'s saturation check,
     * not the ready_count >= ready_max one. */
    fwrite($idle, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");

    $statusIdle = readStatus($idle);
    check($statusIdle === '503', "a request on an idle keep-alive connection during stopping got $statusIdle, not 503");
    fclose($idle);
    touch("$root/release");
    echo "idle-connection-refused-once-stopping: ok\n";

    /* The held request is still drained with 503 once the worker exits. */
    $statusA = readStatus($holdA);
    check($statusA === '503', "held request A: $statusA");
    fclose($holdA);
    echo "held-request-drained: ok\n";

    /* And the pool keeps working afterwards. */
    $again = httpGetRetry("http://127.0.0.1:$port/");
    check(str_starts_with($again, 'hello from pid '), 'no respawn after saturation: ' . var_export($again, true));
    echo "respawn-after-saturation: ok\n";
} finally {
    @touch("$root/release");
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @unlink("$root/hold-seen");
    @unlink("$root/release");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
idle-connection-served: ok
idle-connection-refused-once-stopping: ok
held-request-drained: ok
respawn-after-saturation: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
