--TEST--
fpm-ng: a worker executor stream whose client walks away is reported by request id through fpmng_worker_closed_requests() (issue #342)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Before #342 the driver learned of a dead stream client only when its next
 * fpmng_worker_respond_chunk() returned false -- for a stream emitting an
 * event every 15 s that kept the dead client's subscription alive for up to a
 * whole heartbeat. The notify pipe signals *that* something happened;
 * fpmng_worker_closed_requests() drains *which* ids died, oldest first. */
$root = sys_get_temp_dir() . '/fpmng-worker-closed-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function waitFor(float $seconds): void
{
    $fiber = Fiber::getCurrent();
    $box = new stdClass();
    $box->id = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use ($fiber, $box) {
        fpmng_worker_event_free($box->id);
        $fiber->resume();
    });
    fpmng_worker_event_enable($box->id, $seconds);
    Fiber::suspend();
}

function handle(int $id): void
{
    if (!fpmng_worker_respond_start($id, 200, ['Content-Type' => 'text/event-stream'])) {
        return;
    }
    file_put_contents(__DIR__ . '/started.marker', (string) $id);
    if (!fpmng_worker_respond_chunk($id, "event: hello\ndata: start\n\n")) {
        return;
    }
    while (true) {
        waitFor(0.2);
        if (!fpmng_worker_respond_chunk($id, ": ping\n\n")) {
            /* false on a gone client also reaps the entry -- see
             * fpmng_worker_respond_chunk(). Nothing more to do: the drain in
             * the watcher below is what records the id. */
            return;
        }
    }
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    $closed = fpmng_worker_closed_requests();
    if ($closed) {
        file_put_contents(__DIR__ . '/closed.marker', implode(',', $closed) . "\n", FILE_APPEND);
    }
    while (($id = fpmng_worker_next_request()) !== null) {
        (new Fiber(function () use ($id) {
            $env = fpmng_worker_request_env($id);
            if (($env['REQUEST_URI'] ?? '/') === '/pid') {
                /* So the harness can retire this child (SIGUSR1) once the
                 * stream client has died -- nothing else tells the worker to
                 * stop, and may_exit() waits for that stop. */
                fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], (string) getmypid());
                return;
            }
            handle($id);
        }))->start();
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
    $closed = fpmng_worker_closed_requests();
    if ($closed) {
        file_put_contents(__DIR__ . '/closed.marker', implode(',', $closed) . "\n", FILE_APPEND);
    }
}

/* Drained twice -- watcher and main loop -- and the second drain must see an
 * empty queue: the drain empties, the same shape fpmng_worker_next_request()
 * has. Written after the loop so the harness can assert it deterministically. */
file_put_contents(__DIR__ . '/drained-twice.marker', implode(',', fpmng_worker_closed_requests()));
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_CLOSED_PORT') ?: 28143);
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

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    @unlink("$root/started.marker");
    @unlink("$root/closed.marker");
    @unlink("$root/drained-twice.marker");

    /* Open one stream, read its first event, then walk away without any
     * close handshake -- the abrupt mid-stream case conn_closed() exists for. */
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    if (!$fp) throw new RuntimeException("connect :$port: $error");
    stream_set_timeout($fp, 10);
    fwrite($fp, "GET /events HTTP/1.1\r\nHost: t\r\n\r\n");

    $deadline = microtime(true) + 10;
    $buffer = '';
    while (microtime(true) < $deadline && !str_contains($buffer, 'event: hello')) {
        $chunk = fread($fp, 8192);
        if ($chunk === false || ($chunk === '' && feof($fp))) {
            break;
        }
        $buffer .= $chunk;
        usleep(10000);
    }
    check(str_contains($buffer, 'event: hello'), 'stream never delivered its first event: ' . var_export($buffer, true));
    fclose($fp);    /* the client-gone event */

    $deadline = microtime(true) + 10;
    while (!file_exists("$root/closed.marker") && microtime(true) < $deadline) {
        usleep(20000);
    }
    check(file_exists("$root/closed.marker"), 'fpmng_worker_closed_requests() never reported the dead client');
    $id = trim((string) file_get_contents("$root/closed.marker"));
    check((int) $id > 0, "the reported closed id is not a request id: " . var_export($id, true));
    echo "closed-client-reported-by-id: ok\n";

    /* Retire the child: the dead stream's entry was reaped by its own fiber's
     * next _chunk() returning false, so the drain-then-exit loop finishes on
     * its own. */
    $pid = (int) trim((string) file_get_contents("http://127.0.0.1:$port/pid"));
    check($pid > 1, "could not read the worker's pid");
    $tester->signal('USR1', $pid);

    /* The child exits on its own once the dead stream is reaped and the stop
     * lands (the worker script's may_exit loop), and the post-loop drain sees
     * an empty queue. Written after the loop so the harness can assert it
     * deterministically. */
    $deadline = microtime(true) + 15;
    while (!file_exists("$root/drained-twice.marker") && microtime(true) < $deadline) {
        usleep(50000);
    }
    check(file_exists("$root/drained-twice.marker"), 'the worker script never exited after the stream client died');
    check(file_get_contents("$root/drained-twice.marker") === '',
        'the drain did not empty the queue: ' . var_export(file_get_contents("$root/drained-twice.marker"), true));
    echo "drain-empties-the-queue: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @unlink("$root/started.marker");
    @unlink("$root/closed.marker");
    @unlink("$root/drained-twice.marker");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
closed-client-reported-by-id: ok
drain-empties-the-queue: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
