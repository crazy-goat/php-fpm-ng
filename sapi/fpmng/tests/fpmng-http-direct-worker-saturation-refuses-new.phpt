--TEST--
fpm-ng: worker.max_pending saturation refuses a NEW request on an already-open keep-alive connection too (issue #336)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-operator.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* fpmng_pool_worker_refused_total{pool,reason} for this pool. The caller polls
 * it with a deadline, not once, so the test does not depend on when the
 * operator process sees the worker's update. */
function refused(string $body, string $reason): int
{
    if (!preg_match('/^fpmng_pool_worker_refused_total\{pool="cap",reason="' . $reason . '"\} (\d+)$/m', $body, $m)) {
        throw new RuntimeException("metric missing: worker_refused reason=$reason\n$body");
    }
    return (int) $m[1];
}

/* fpmng-http-direct-worker-max-pending.phpt (issue #331) already proves that a
 * request beyond worker.max_pending's ceiling gets 503, and that the two
 * accepted-but-unanswered holds are drained with 503 once the worker stops. It
 * never proves the OTHER half of fpm_worker_accept()'s refusal condition:
 * `fpm_worker_stopping || ready_count >= ready_max || pending >= ready_max`
 * (fpm_http_direct_worker.c:701). The first disjunct is what protects a
 * request that arrives on an ALREADY OPEN keep-alive connection during the
 * window between the ceiling being hit
 * (fpm_worker_stopping = 1) and the listener actually being torn down in
 * fpm_worker_finish_output() -- that connection is not one of the held
 * requests, occupies no pending slot, and would otherwise be silently
 * dispatched to a worker that has already decided to recycle. While the held
 * request is still pending, the pending disjunct is true as well, so the test
 * answers that held request first: the idle request then sees neither ready
 * ceiling, and only fpm_worker_stopping can refuse it. This test drives that
 * path directly. */
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
/* The id of the held request, answered by the loop after the stop only when
 * the test asks for it (marker file), so the test controls when pending drops. */
$held = null;

function handle(int $id): void
{
    global $held;
    $env = fpmng_worker_request_env($id);
    $uri = $env['REQUEST_URI'] ?? '/';

    if (str_starts_with($uri, '/hold')) {
        /* Tell the test this hold has really reached the script. */
        file_put_contents('__ROOT__/hold-seen', '1');
        $held = $id;
        return; /* deliberately never call fpmng_worker_respond() until asked */
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
 * once would leave a window of a single event-loop iteration. The held request
 * is answered here on the test's word ("answer-held"), which empties
 * fw.pending before the idle request is written. */
$until = microtime(true) + 10.0;
while (microtime(true) < $until && !file_exists('__ROOT__/release')) {
    if ($held !== null && file_exists('__ROOT__/answer-held')) {
        fpmng_worker_respond($held, 200, ['Content-Type' => 'text/plain'], 'held answered');
        $held = null;
    }
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
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics
CFG;

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $operator = $tester->getListen('{{ADDR[operator]}}');

    /* A keep-alive connection, established and answered BEFORE saturation --
     * it must not be treated as one of the held requests below. */
    $idle = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    check($idle !== false, "idle connect failed: $errstr");
    fwrite($idle, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    $status = readStatus($idle);
    check($status === '200', "idle connection's first request: $status");
    echo "idle-connection-served: ok\n";

    /* Trip worker.max_pending = 1: one held request fills the only slot, and a
     * second concurrent one is refused by the pending >= ready_max disjunct
     * (the ceiling itself is covered by fpmng-http-direct-worker-max-pending.phpt).
     * That refusal also sets fpm_worker_stopping. */
    $holdA = openHeld('127.0.0.1', $port, '/hold');
    /* Wait until holdA has really reached the worker script, so the second
     * hold below cannot be seen before it, however the scheduler orders the
     * test and the worker (issue #527). */
    for ($i = 0; $i < 1000 && !file_exists("$root/hold-seen"); $i++) {
        usleep(10000);
    }
    check(file_exists("$root/hold-seen"), 'hold A never reached the worker script');
    $holdB = openHeld('127.0.0.1', $port, '/hold');

    /* holdB's 503 proves fpm_worker_stopping is set (its refusal set it). */
    $statusB = readStatus($holdB);
    check($statusB === '503', "the 2nd concurrent hold did not get 503: $statusB");
    fclose($holdB);

    $tester->expectLogPattern('/WARNING: .*\[pool cap\] http-direct worker: 1 requests accepted but unanswered; '
        . 'answering 503 and asking the worker script to stop so the master can respawn it/', true);

    /* Answer the held request before the idle-connection request is written.
     * While it is pending, fw.pending >= ready_max is true as well, and the
     * idle request would be refused by that disjunct even without
     * fpm_worker_stopping. Once it is answered, fw.pending and fw.ready_count
     * are both 0, so only fpm_worker_stopping can refuse the idle request. The
     * script answers when "answer-held" exists, inside the loop that runs
     * after the stop, so the worker is still listening. */
    touch("$root/answer-held");
    $statusA = readStatus($holdA);
    check($statusA === '200', "held request A: $statusA");
    fclose($holdA);
    echo "held-request-answered: ok\n";

    /* The load-bearing write: a SECOND request on the connection that was
     * already open and idle before saturation -- not one of the held sockets,
     * not a new TCP connection -- must also be refused 503 rather than being
     * handed to a worker that has already decided to stop. The script keeps the
     * event loop running until "release" exists, so the request is read while
     * the worker is stopping. */
    fwrite($idle, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");

    $statusIdle = readStatus($idle);
    check($statusIdle === '503', "a request on an idle keep-alive connection during stopping got $statusIdle, not 503");
    fclose($idle);
    touch("$root/release");
    echo "idle-connection-refused-once-stopping: ok\n";

    /* The refusal is counted under its own reason: holdB was refused while the
     * worker was still healthy (saturated), the idle request once it was
     * stopping. Poll the page with a deadline, as refused() describes. */
    $deadline = microtime(true) + 10.0;
    do {
        $metrics = fpmng_operator_body($operator, '/metrics');
        $counts = [refused($metrics, 'saturated'), refused($metrics, 'stopping')];
        if ($counts === [1, 1]) {
            break;
        }
        usleep(50000);
    } while (microtime(true) < $deadline);
    check($counts === [1, 1], 'worker_refused saturated/stopping: ' . var_export($counts, true) . "\n$metrics");
    echo "refusal-counted-as-stopping: ok\n";

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
    @unlink("$root/answer-held");
    @unlink("$root/release");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
idle-connection-served: ok
held-request-answered: ok
idle-connection-refused-once-stopping: ok
refusal-counted-as-stopping: ok
respawn-after-saturation: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
