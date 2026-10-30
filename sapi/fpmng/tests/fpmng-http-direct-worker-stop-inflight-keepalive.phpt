--TEST--
fpm-ng: a request already on the wire over a kept-alive connection when the worker stops gets 503, not a silent close (issue #668)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* fpm_worker_finish_output() removes the listener and ends the child's loop.
 * Before the grace drain (issue #668), a request whose bytes were already in
 * the kernel buffer of a keep-alive connection the child still held was never
 * read: evhttp_free() closed that connection and the client saw EOF, not the
 * 503 that fpm_worker_accept() gives every request once stopping is set.
 *
 * The window is made deterministic. The worker script keeps its loop running
 * after the stop until the test has read the reply that tripped pm.max_requests.
 * Then it stops driving the loop, so nothing in the child reads the connection.
 * The test writes the keep-alive request into the socket buffer while the child
 * is paused, and only then lets the script return, which starts
 * fpm_worker_finish_output(). No reply is queued at that point, so finish_output
 * has no flush loop of its own that could read the request by chance. */
function send($sock, string $path): void
{
    fwrite($sock, "GET $path HTTP/1.1\r\nHost: t\r\n\r\n");
}

/* One complete reply. Returns the status and whether the reply closes the
 * connection. A connection closed without a reply fails with its own message. */
function readReply($sock): array
{
    $line = fgets($sock);
    if ($line === false) {
        throw new RuntimeException('connection closed without a reply');
    }
    if (!preg_match('{^HTTP/1\.1 (\d+) }', $line, $m)) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $length = 0;
    $closing = false;
    while (($line = fgets($sock)) !== false && $line !== "\r\n") {
        if (stripos($line, 'Content-Length:') === 0) {
            $length = (int) trim(substr($line, 15));
        }
        if (stripos($line, 'Connection:') === 0 && stripos($line, 'close') !== false) {
            $closing = true;
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
    return [(int) $m[1], $closing];
}

function connect(int $port)
{
    $sock = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    check($sock !== false, "connect failed: $errstr");
    stream_set_timeout($sock, 10);
    return $sock;
}

function waitForFile(string $path, string $what): void
{
    $deadline = microtime(true) + 10.0;
    while (!file_exists($path)) {
        if (microtime(true) > $deadline) {
            throw new RuntimeException("worker never reached: $what");
        }
        usleep(10000);
    }
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

$root = sys_get_temp_dir() . '/fpmng-worker-stop-inflight-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", str_replace('__ROOT__', $root, <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'hello from pid ' . getmypid());
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit() && !fpmng_worker_stopping()) {
    fpmng_worker_loop(true);
}
/* The stop came from the reply that tripped pm.max_requests. Keep the loop
 * running until the test has read that reply, so it is on the wire before the
 * child pauses. */
$until = microtime(true) + 10.0;
while (microtime(true) < $until && !file_exists('__ROOT__/trip-read')) {
    fpmng_worker_loop(false);
    usleep(5000);
}
for ($i = 0; $i < 3; $i++) {
    fpmng_worker_loop(false);
}
/* Stop driving the loop. The request the test writes next must stay unread
 * until the test says go, and the script returns only after that. */
file_put_contents('__ROOT__/paused', '1');
$until = microtime(true) + 10.0;
while (microtime(true) < $until && !file_exists('__ROOT__/go')) {
    usleep(5000);
}
PHP));

$port = (int) (getenv('FPMNG_DIRECT_WORKER_STOP_INFLIGHT_PORT') ?: 28120 + 200 * (int) getenv('TEST_PHP_WORKER') + (int) getenv('FPMNG_PHPT_PORT_SHIFT'));

$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[stop]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
pm.max_requests = 2
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* The first answer on this connection: below pm.max_requests, so the
     * connection stays open and the worker keeps running. */
    $keep = connect($port);
    send($keep, '/');
    [$status, $closing] = readReply($keep);
    check($status === 200, "first request on the keep-alive connection: $status");
    check(!$closing, 'the first reply, below pm.max_requests, closed the connection');
    echo "keep-alive-served-before-stop: ok\n";

    /* The second answer trips pm.max_requests = 2: its reply carries
     * Connection: close, and the worker is asked to stop. */
    $trip = connect($port);
    send($trip, '/trip');
    [$status, $closing] = readReply($trip);
    check($status === 200 && $closing, "the reply that trips pm.max_requests: $status, close=" . var_export($closing, true));
    fclose($trip);
    touch("$root/trip-read");
    waitForFile("$root/paused", 'the pause after the stop');
    echo "worker-paused-after-stop: ok\n";

    /* The request this test is about. The child is paused, so the bytes sit in
     * the socket buffer of the connection it still holds. */
    send($keep, '/');
    touch("$root/go");

    [$status] = readReply($keep);
    check($status === 503, "a request on a kept-alive connection that was on the wire when the worker stopped got $status, not 503");
    echo "request-on-the-wire-answered-503: ok\n";
    fclose($keep);

    /* And the pool keeps working: the master respawned the worker. */
    $again = httpGetRetry("http://127.0.0.1:$port/");
    check(str_starts_with($again, 'hello from pid '), 'no respawn after the stop: ' . var_export($again, true));
    echo "respawn-after-stop: ok\n";
} finally {
    @touch("$root/trip-read");
    @touch("$root/go");
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @unlink("$root/trip-read");
    @unlink("$root/paused");
    @unlink("$root/go");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
keep-alive-served-before-stop: ok
worker-paused-after-stop: ok
request-on-the-wire-answered-503: ok
respawn-after-stop: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
