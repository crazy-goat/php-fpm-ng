--TEST--
fpm-ng: worker.max_pending caps concurrently-held requests, then recycles the worker (issue #331)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Plain blocking sockets driven by hand, in this one process, instead of a
 * subprocess-per-request plus arbitrary usleep() ordering
 * (fpmng-http-direct-worker-drain.phpt's technique): spawning a php CLI
 * process takes an unpredictable few tens of ms, which raced against the
 * saturation point this test needs to land on exactly. Opening a socket and
 * writing the request line is synchronous here, so "A and B are held, then C
 * arrives" is an ordering this process itself controls -- the request is on
 * the wire (and, per TCP, queued for the server to read) before the next
 * call starts. */
function openHeld(string $host, int $port, string $uri)
{
    $sock = stream_socket_client("tcp://$host:$port", $errno, $errstr, 5);
    check($sock !== false, "connect failed: $errstr");
    fwrite($sock, "GET $uri HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    return $sock;
}

function readResponse($sock): array
{
    $raw = stream_get_contents($sock);
    fclose($sock);
    [$head] = explode("\r\n\r\n", $raw, 2) + [''];
    $status = '';
    if (preg_match('{^HTTP/\d\.\d (\d+)}', $head, $m)) {
        $status = $m[1];
    }
    return ['status' => $status, 'raw' => $raw];
}

/* The master respawns the child after every generation; the first connection
 * afterwards can arrive before it is listening again. */
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

$root = sys_get_temp_dir() . '/fpmng-worker-max-pending-' . getmypid();
@mkdir($root, 0700, true);

/* /hold never answers, so a request sent there occupies a fw.pending slot for
 * the whole test until the worker itself is recycled and drains it (matching
 * the accept-then-never-respond leak fpm_worker_accept()'s comment describes,
 * except here worker.max_pending is deliberately small so three concurrent
 * holds are enough to trip the ceiling). */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    $uri = $env['REQUEST_URI'] ?? '/';

    if (str_starts_with($uri, '/hold')) {
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

/* /hold never answers and spawns no Fiber, so nothing here is ever
 * "in-flight" from fpmng_worker_may_exit()'s point of view except the two
 * requests deliberately left pending -- it would never become true on its
 * own. Once the SAPI trips worker.max_pending and sets stopping (visible here
 * as fpmng_worker_stopping()), leave the loop deliberately with those two
 * still unanswered, the same abandonment fpmng-http-direct-worker-drain.phpt
 * drives through an explicit /abandon route: the SAPI's own
 * fpm_worker_finish_output() is what must answer them 503, not this script. */
while (!fpmng_worker_may_exit() && !fpmng_worker_stopping()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_MAX_PENDING_PORT') ?: 28090);

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
worker.max_pending = 2
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $hello = file_get_contents("http://127.0.0.1:$port/");
    check(is_string($hello) && str_starts_with($hello, 'hello from pid '), 'hello: ' . var_export($hello, true));

    /* Fill both slots worker.max_pending allows, then send a third: that one
     * must be rejected immediately with 503, before the worker even finishes
     * draining the first two. Each openHeld() only returns once its request
     * is written to the socket, so C genuinely arrives after A and B are
     * already accepted -- no arbitrary delay to race against. */
    $holdA = openHeld('127.0.0.1', $port, '/hold');
    $holdB = openHeld('127.0.0.1', $port, '/hold');
    $holdC = openHeld('127.0.0.1', $port, '/hold');

    $resultC = readResponse($holdC);
    check($resultC['status'] === '503', 'the 3rd concurrent hold did not get 503: ' . var_export($resultC, true));
    echo "third-hold-gets-503: ok\n";

    $tester->expectLogPattern('/WARNING: .*\[pool cap\] http-direct worker: 2 requests accepted but unanswered; '
        . 'answering 503 and asking the worker script to stop so the master can respawn it/', true);
    echo "saturation-is-logged: ok\n";

    /* The two requests that made it in are answered 503 too, once the worker
     * drains on its way out -- not left to hang or get silently closed. */
    $resultA = readResponse($holdA);
    $resultB = readResponse($holdB);
    check($resultA['status'] === '503', 'held request A: ' . var_export($resultA, true));
    check($resultB['status'] === '503', 'held request B: ' . var_export($resultB, true));
    echo "held-requests-drained-with-503: ok\n";

    $tester->expectLogPattern('/WARNING: .*\[pool cap\] http-direct worker: the worker script stopped with 2 '
        . 'accepted request\(s\) unanswered/', true);
    echo "drain-is-logged: ok\n";

    /* And the pool keeps working: the master respawned the child. */
    $again = httpGetRetry("http://127.0.0.1:$port/");
    check(str_starts_with($again, 'hello from pid '), 'no respawn after saturation: ' . var_export($again, true));
    check($again !== $hello, 'the worker did not actually restart: ' . $again);
    echo "respawn-after-saturation: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
third-hold-gets-503: ok
saturation-is-logged: ok
held-requests-drained-with-503: ok
drain-is-logged: ok
respawn-after-saturation: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
