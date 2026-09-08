--TEST--
fpm-ng: worker-mode HTTP-direct — one booted script owns the loop and serves concurrent waits (task 073)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Each URL gets its own php process, so the N requests really are in flight
 * at the same time against the single worker configured below. Same technique
 * as fpmng-fiber-sleep-concurrency.phpt. */
function concurrentHttpGet(array $urls): array
{
    $descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $processes = [];
    $pipes = [];
    foreach ($urls as $i => $url) {
        $code = '$b=@file_get_contents(' . var_export($url, true) . '); echo $b === false ? "@@FALSE@@" : $b;';
        $processes[$i] = proc_open(PHP_BINARY . ' -n -r ' . escapeshellarg($code), $descriptors, $pipes[$i]);
        fclose($pipes[$i][0]);
    }
    $bodies = [];
    foreach ($processes as $i => $proc) {
        $bodies[$i] = stream_get_contents($pipes[$i][1]);
        fclose($pipes[$i][1]);
        fclose($pipes[$i][2]);
        proc_close($proc);
    }
    return $bodies;
}

/* The load-bearing assertion, borrowed from fpmng-fiber-sleep-concurrency.phpt:
 * the LAST wait to start did so before the FIRST one finished, i.e. all of
 * them were suspended in the one worker simultaneously. Proven from the
 * worker's own clock, so it needs no wall-clock bound and does not depend on
 * how loaded the box is. */
function expectOverlap(array $rows, string $what): void
{
    $lastStart = max(array_column($rows, 't0'));
    $firstEnd = min(array_column($rows, 't1'));
    if ($lastStart >= $firstEnd) {
        $msg = "FAIL: $what serialized, last start $lastStart >= first end $firstEnd\n";
        foreach ($rows as $id => $row) {
            $msg .= "  $id: {$row['t0']} .. {$row['t1']}\n";
        }
        throw new RuntimeException($msg);
    }
}

$root = sys_get_temp_dir() . '/fpmng-direct-worker-' . getmypid();
@mkdir($root, 0700, true);

/* Deliberately dependency-free: core Fiber plus the raw SAPI primitives, no
 * Composer, no Revolt. This is the CI-runnable half of task 073; the amphp
 * half lives in build/test-http-direct-amphp.sh, which needs the network.
 * Whatever a real driver adds, it can only add it on top of exactly these
 * calls, so this test also pins the primitive surface. */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

/* Suspends the calling fiber on a one-shot libevent timer. This is, in
 * miniature, everything Revolt's delay() needs from the SAPI. */
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
    $env = fpmng_worker_request_env($id);
    if (str_starts_with($env['REQUEST_URI'] ?? '/', '/sleep')) {
        $query = [];
        parse_str((string) ($env['QUERY_STRING'] ?? ''), $query);
        $t0 = microtime(true);
        waitFor(1.0);
        fpmng_worker_respond($id, 200, ['Content-Type' => 'application/json'], json_encode([
            'id' => $query['id'] ?? 'missing',
            'pid' => getmypid(),
            't0' => $t0,
            't1' => microtime(true),
        ]));
        return;
    }
    if (($env['REQUEST_URI'] ?? '') === '/badheader') {
        /* A header name that is not a token is refused, not silently dropped:
         * respond() reports false and the client gets 500. Dropping it would
         * let an application believe it had set a Set-Cookie or a CSP header
         * that never reached the wire. */
        fpmng_worker_respond($id, 200, ['X Bad' => 'y'], 'never sent');
        return;
    }
    fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'hello world from pid ' . getmypid());
}

$serve = function (int $id): void {
    /* One fiber per request: the concurrency is entirely userland, which is
     * the point — the SAPI never suspends anything itself. */
    (new Fiber(function () use ($id) {
        try {
            handle($id);
        } catch (Throwable $e) {
            fpmng_worker_respond($id, 500, [], 'handler failed');
        }
    }))->start();
};

/* ONE readable watcher on the notify pipe. It both delivers work and gives a
 * userland loop something referenced to wait on, so run() cannot return for
 * lack of registered callbacks. The read is a level-triggered drain: the
 * pipe carries no payload, only "look at the queue". */
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, $serve): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $serve($id);
    }
});
fpmng_worker_event_enable($watcher);

/* fpmng_worker_may_exit(), not "stopping and nothing in flight": the bridge
 * has no way to see the SAPI's own queue, and a request sitting in it when the
 * loop is torn down is closed with no response (task 080). */
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_TEST_PORT') ?: 28073);
$cfg = <<<CFG
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

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $hello = file_get_contents("http://127.0.0.1:$port/");
    check(is_string($hello) && str_starts_with($hello, 'hello world from pid '), 'hello world: ' . var_export($hello, true));
    echo "hello-world: ok\n";

    $rows = [];
    foreach (concurrentHttpGet([
        "http://127.0.0.1:$port/sleep?id=A",
        "http://127.0.0.1:$port/sleep?id=B",
        "http://127.0.0.1:$port/sleep?id=C",
        "http://127.0.0.1:$port/sleep?id=D",
    ]) as $i => $body) {
        $row = json_decode($body, true);
        check(is_array($row), "response $i not json: " . var_export($body, true));
        $rows[$row['id']] = $row;
    }
    check(count($rows) === 4, 'expected 4 distinct ids, got ' . json_encode(array_keys($rows)));
    check(count(array_unique(array_column($rows, 'pid'))) === 1, 'more than one worker served the batch');
    expectOverlap($rows, '4 x 1s wait in one worker');
    echo "concurrent-waits: ok\n";

    $bad = @file_get_contents("http://127.0.0.1:$port/badheader");
    check($bad === false, 'malformed header name was accepted: ' . var_export($bad, true));
    $status = http_get_last_response_headers() ?? [];
    check((bool) preg_grep('{^HTTP/1\.[01] 500}', $status),
        'expected 500 for a malformed header name, got ' . json_encode($status));
    echo "malformed-header: ok\n";

    /* The worker survives the batch: the loop is still pumping and the
     * booted script was not restarted (same pid as the JSON above). */
    $again = file_get_contents("http://127.0.0.1:$port/");
    check($again === $hello, "worker was replaced: $again vs $hello");
    echo "worker-persists: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
hello-world: ok
concurrent-waits: ok
malformed-header: ok
worker-persists: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
