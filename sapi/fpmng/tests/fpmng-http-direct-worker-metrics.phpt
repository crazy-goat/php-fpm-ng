--TEST--
fpm-ng: worker.executor honest metrics -- requests counted, pending/watcher gauges live (issue #333, extended by #339)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #333. Before this, `operator.metrics_path` on a `pool.executor = worker`
 * pool published `requests_total` without one call ever having incremented it
 * -- fpm_scoreboard_update() was never reached from this executor's request
 * path -- and had no way at all to say how many requests were mid-flight or
 * how many libevent watchers a worker script had registered. Every assertion
 * below is about a number MOVING under a load this test produced, never about
 * a field merely being present, the same discipline
 * fpmng-http-direct-metrics.phpt (issue #64) uses for the classic executor's
 * status page -- this is the sibling file for the worker executor's metrics
 * page, which that test explicitly does not cover.
 *
 * The page is read from the operator listener (operator.metrics_listen), not from
 * the pool's own: the scrape is then none of the requests this test counts. */
$root = sys_get_temp_dir() . '/fpmng-worker-metrics-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

/* Watchers this script keeps registered on purpose, on its own, so the
 * watchers gauge can be checked apart from the pending one below -- every
 * /hold request also carries a watcher (its timer), and this is the other
 * source, unconnected to any request being mid-flight. */
$extraWatchers = [];

function handle(int $id): void
{
    global $extraWatchers;
    $env = fpmng_worker_request_env($id);
    /* REQUEST_URI carries the query string; the path alone is what every
     * comparison below matches on, same as fpmng-http-direct-worker.phpt's
     * str_starts_with($env['REQUEST_URI'], '/sleep') does for /sleep?id=A --
     * an exact === against REQUEST_URI itself would never match a request
     * that has a query string. */
    $uri = explode('?', $env['REQUEST_URI'] ?? '/', 2)[0];
    $query = [];
    parse_str((string) ($env['QUERY_STRING'] ?? ''), $query);

    /* Held open on a one-shot timer, the same primitive
     * fpmng-http-direct-worker.phpt's waitFor() uses, but the callback
     * answers the request directly rather than resuming a fiber -- what this
     * test needs is how long the request stays in fw.pending, not a
     * suspension primitive. */
    if ($uri === '/hold') {
        $ms = (int) ($query['ms'] ?? 2000);
        $box = new stdClass();
        $box->id = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use ($id, $box) {
            fpmng_worker_event_free($box->id);
            fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'held-done');
        });
        fpmng_worker_event_enable($box->id, $ms / 1000.0);
        return;
    }

    if ($uri === '/watcher-create') {
        /* A timer that is never enabled: fpmng_worker_event_create() alone
         * already registers it in fw.watchers, which is exactly the count
         * this test's gauge reports on -- enabling it would only add a race
         * on when it fires. */
        $wid = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () {});
        $extraWatchers[] = $wid;
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], (string) $wid);
        return;
    }

    if ($uri === '/watcher-free') {
        $wid = (int) ($query['id'] ?? 0);
        $ok = in_array($wid, $extraWatchers, true) && fpmng_worker_event_free($wid);
        if ($ok) {
            $extraWatchers = array_values(array_diff($extraWatchers, [$wid]));
        }
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], $ok ? 'freed' : 'not-found');
        return;
    }

    /* The streaming completion path (issue #332): fpmng_worker_respond_end()
     * is the OTHER place fpm_worker_count_scoreboard_request() is called from
     * in the C code, and this test's point about it is that a stream counts
     * once, not zero and not twice. */
    if ($uri === '/stream') {
        fpmng_worker_respond_start($id, 200, ['Content-Type' => 'text/plain']);
        fpmng_worker_respond_chunk($id, 'chunk');
        fpmng_worker_respond_end($id);
        return;
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

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_METRICS_PORT') ?: 28096);
$ops = '127.0.0.1:' . ($port + 1);
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
operator.metrics_listen = $ops
operator.metrics_path = /metrics
CFG;

function expect(string $what, $actual, $expected): void
{
    if ($actual !== $expected) {
        throw new RuntimeException("$what: expected " . var_export($expected, true) .
            ', got ' . var_export($actual, true));
    }
}

/* fpmng_pool_<name>{pool="worker"} <value>: the one shape both the request
 * counter and the two live gauges share, since both are plain Prometheus
 * gauges/counters on the same per-pool page (see fpm_operator_pages.c). */
function metric(string $body, string $name): int
{
    if (!preg_match('/^fpmng_pool_' . preg_quote($name, '/') . '\{pool="worker"\} (-?\d+)$/m', $body, $m)) {
        throw new RuntimeException("metric missing: $name\n$body");
    }
    return (int) $m[1];
}

/* fpmng_pool_<name>{pool="worker",<extra labels>} <value>: the shape issue
 * #339's per-slot/labeled series use -- one label list per metric (slot=, or
 * reason=, or slot=+type=), always with pool="worker" first and the rest in
 * the exact order fpm_http_direct_ops_render_worker_metrics_prometheus()
 * writes them in. $labels is given in that same order; a mismatch is a
 * missing-metric error just like metric() above, not a silent non-match, so a
 * label-order regression in the C code fails this test instead of the
 * assertion quietly finding nothing. */
function metricLabeled(string $body, string $name, array $labels): string
{
    $pattern = '/^fpmng_pool_' . preg_quote($name, '/') . '\{pool="worker"';
    foreach ($labels as $label => $value) {
        $pattern .= ',' . preg_quote($label, '/') . '="' . preg_quote((string) $value, '/') . '"';
    }
    $pattern .= '\} (-?\d+(?:\.\d+)?)$/m';
    if (!preg_match($pattern, $body, $m)) {
        throw new RuntimeException("metric missing: $name " . var_export($labels, true) . "\n$body");
    }
    return $m[1];
}

/* Polls rather than sleeps. fpm_worker_metrics_publish() writes the shared
 * slot synchronously on every fw.pending/fw.watchers mutation -- there is no
 * tick to wait a whole period for here, unlike the classic executor's
 * connection gauges -- but the scrape itself still needs a fresh connection
 * and a fresh read, so "not yet" and "never" look the same for a single
 * read. */
function until(callable $done, float $seconds, string $what)
{
    $deadline = microtime(true) + $seconds;
    do {
        $value = $done();
        if ($value !== null) {
            return $value;
        }
        usleep(20000);
    } while (microtime(true) < $deadline);
    throw new RuntimeException("timed out waiting for $what\nlast page:\n" .
        ($GLOBALS['last_page'] ?? '(none)'));
}

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $page = function () use ($ops): string {
        $body = fpmng_operator_body($ops, '/metrics');
        $GLOBALS['last_page'] = $body;
        return $body;
    };

    /* 0. The resting state, captured before this test's own load starts, so
     * every assertion below is a DELTA this test produced rather than an
     * assumption that the pool started at zero -- one request against `/`
     * warms the worker up (and proves it answers at all) without touching
     * `/hold` or `/watcher-create`, so it does not move the numbers this test
     * is about to check. */
    $hello = file_get_contents("http://127.0.0.1:$port/");
    if (!is_string($hello) || !str_starts_with($hello, 'hello from pid ')) {
        throw new RuntimeException('hello world: ' . var_export($hello, true));
    }
    $body = until(function () use ($page) {
        $b = $page();
        return metric($b, 'requests_total') >= 1 ? $b : null;
    }, 15, 'the warm-up request to be counted');
    $baseRequests = metric($body, 'requests_total');
    $baseWatchers = metric($body, 'worker_watchers');
    expect('resting pending', metric($body, 'worker_pending'), 0);

    /* Issue #339's slot-labeled series, checked at the same resting point:
     * fpmng_pool_worker_queued is the fw.ready_count gauge, which this test's
     * synchronous handle() loop never leaves non-zero between requests (every
     * queued id is drained in the same read-watcher callback that queued it),
     * so "0 at rest" is the only value this test can ever observe for it --
     * still worth asserting, the same way worker_pending's resting 0 is above,
     * because a stuck-nonzero regression would show up as a mismatch here.
     * worker_memory_bytes and worker_accepted_total are captured as baselines
     * to build deltas from below, the same pattern baseRequests/baseWatchers
     * already use two lines up. */
    expect('resting queued', (int) metricLabeled($body, 'worker_queued', ['slot' => '0']), 0);
    $baseMemoryBytes = (int) metricLabeled($body, 'worker_memory_bytes', ['slot' => '0']);
    if ($baseMemoryBytes <= 0) {
        throw new RuntimeException('worker_memory_bytes baseline not positive: ' .
            var_export($baseMemoryBytes, true));
    }
    $baseAccepted = (int) metricLabeled($body, 'worker_accepted_total', ['slot' => '0']);
    $baseWatchersTimer = (int) metricLabeled($body, 'worker_watchers', ['slot' => '0', 'type' => 'timer']);
    echo "baseline: ok\n";

    /* 1. The buffered path (fpmng_worker_respond()): five plain requests, so
     * the counter has to move by exactly five and not by some other number
     * that would also happen to be "more than before". */
    for ($i = 0; $i < 5; $i++) {
        $r = file_get_contents("http://127.0.0.1:$port/");
        if ($r !== $hello) {
            throw new RuntimeException("buffered request $i: " . var_export($r, true));
        }
    }
    $body = until(function () use ($page, $baseRequests) {
        $b = $page();
        return metric($b, 'requests_total') >= $baseRequests + 5 ? $b : null;
    }, 15, 'five buffered requests to be counted');
    expect('requests after buffered load', metric($body, 'requests_total'), $baseRequests + 5);

    /* fpmng_pool_worker_accepted_total (issue #339): one accepted TCP
     * connection per request here, none of them pipelined or reused -- the
     * same one-shot-connection shape file_get_contents() already gives the
     * requests_total assertion right above, so this counter has to have moved
     * by at least as much in the same window. Not an exact-five check like
     * requests_total's: fpmng_worker_accept_hook() fires strictly before a
     * request is even parsed, so nothing stops it from having also counted
     * connections this test does not otherwise account for (a probe, a retry)
     * -- ">=" is the honest claim, "the accept path is wired up and moving",
     * without asserting connection-count internals this test does not
     * control. */
    $acceptedAfterBuffered = (int) metricLabeled($body, 'worker_accepted_total', ['slot' => '0']);
    if ($acceptedAfterBuffered < $baseAccepted + 5) {
        throw new RuntimeException('worker_accepted_total after buffered load: expected >= ' .
            ($baseAccepted + 5) . ', got ' . var_export($acceptedAfterBuffered, true));
    }
    echo "buffered-requests-counted: ok\n";

    /* 2. The streaming path (fpmng_worker_respond_start/_chunk/_end(), issue
     * #332): three streamed requests on top of the five buffered ones above.
     * The load-bearing number is the exact total, not just "it went up" --
     * fpm_worker_count_scoreboard_request() is called from both
     * fpmng_worker_respond() and fpmng_worker_respond_end(), and a bug double
     * counting or dropping either path would still show SOME movement, just
     * the wrong amount. */
    for ($i = 0; $i < 3; $i++) {
        $r = file_get_contents("http://127.0.0.1:$port/stream");
        if ($r !== 'chunk') {
            throw new RuntimeException("streamed request $i: " . var_export($r, true));
        }
    }
    $body = until(function () use ($page, $baseRequests) {
        $b = $page();
        return metric($b, 'requests_total') >= $baseRequests + 8 ? $b : null;
    }, 15, 'three streamed requests to be counted');
    expect('requests after buffered+streamed load', metric($body, 'requests_total'), $baseRequests + 8);
    echo "streamed-requests-counted-once: ok\n";

    /* 3. fpmng_pool_worker_pending: a request the handler holds open (never
     * answered synchronously) shows up as pending WHILE it is held, and the
     * gauge comes back down once it is answered -- a count that only grows
     * would be a total wearing a gauge's name. Connected without reading the
     * response yet, so the request really is in flight while the gauge is
     * read: fpmng_worker_respond() for it has not run. */
    $held = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    if (!$held) {
        throw new RuntimeException("connect held: $error");
    }
    stream_set_timeout($held, 10);
    fwrite($held, "GET /hold?ms=1500 HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $body = until(function () use ($page) {
        $b = $page();
        return metric($b, 'worker_pending') >= 1 ? $b : null;
    }, 15, 'the held request to show up as pending');
    expect('pending while held', metric($body, 'worker_pending'), 1);
    echo "pending-while-held: ok\n";

    $response = stream_get_contents($held);
    fclose($held);
    if (!str_contains($response, 'held-done')) {
        throw new RuntimeException("held request did not answer: " . var_export($response, true));
    }
    $body = until(function () use ($page) {
        $b = $page();
        return metric($b, 'worker_pending') === 0 ? $b : null;
    }, 15, 'the held request to stop being pending once answered');
    echo "pending-drops-after-answer: ok\n";

    /* 4. fpmng_pool_worker_watchers: two watchers this test creates directly
     * (not tied to any pending request -- /watcher-create responds
     * immediately), so the gauge has to move by exactly two, then back down
     * by exactly two once they are freed. */
    $ids = [];
    for ($i = 0; $i < 2; $i++) {
        $r = file_get_contents("http://127.0.0.1:$port/watcher-create");
        if (!is_string($r) || !preg_match('/^\d+$/', $r)) {
            throw new RuntimeException("watcher-create $i: " . var_export($r, true));
        }
        $ids[] = (int) $r;
    }
    $body = until(function () use ($page, $baseWatchers) {
        $b = $page();
        return metric($b, 'worker_watchers') >= $baseWatchers + 2 ? $b : null;
    }, 15, 'two created watchers to be counted');
    expect('watchers after create', metric($body, 'worker_watchers'), $baseWatchers + 2);

    /* fpmng_pool_worker_watchers{type="timer"} (issue #339): both watchers
     * /watcher-create registers are FPMNG_WORKER_TIMER, so the split-by-type
     * gauge has to account for the exact same two the untyped total just did,
     * not just move by some amount of its own. */
    expect('timer watchers after create', (int) metricLabeled($body, 'worker_watchers', ['slot' => '0', 'type' => 'timer']),
        $baseWatchersTimer + 2);
    echo "watchers-counted-on-create: ok\n";

    foreach ($ids as $id) {
        $r = file_get_contents("http://127.0.0.1:$port/watcher-free?id=$id");
        if ($r !== 'freed') {
            throw new RuntimeException("watcher-free $id: " . var_export($r, true));
        }
    }
    $body = until(function () use ($page, $baseWatchers) {
        $b = $page();
        return metric($b, 'worker_watchers') === $baseWatchers ? $b : null;
    }, 15, 'freed watchers to drop back to the baseline');

    /* fpmng_pool_worker_watchers{type="timer"} again, back at its own
     * baseline once both are freed -- same "drops back down" discipline the
     * untyped gauge's own assertion right above already applies. */
    expect('timer watchers after free', (int) metricLabeled($body, 'worker_watchers', ['slot' => '0', 'type' => 'timer']),
        $baseWatchersTimer);
    echo "watchers-drop-on-free: ok\n";

    /* fpmng_pool_worker_memory_bytes (issue #339, reusing issue #334's
     * ru_maxrss sample): not a delta check like the counters above -- RSS is
     * whatever the allocator and the kernel made it, not a number this test's
     * load moves by a predictable amount -- just that the gauge is still
     * there and still a sane positive number after all the load above, the
     * same "present and plausible" discipline the baseline positivity check
     * near the top already used before any of this test's load ran. */
    $memoryBytesNow = (int) metricLabeled($body, 'worker_memory_bytes', ['slot' => '0']);
    if ($memoryBytesNow <= 0) {
        throw new RuntimeException('worker_memory_bytes after load not positive: ' .
            var_export($memoryBytesNow, true));
    }
    echo "memory-bytes-present: ok\n";

    echo "Done\n";
} finally {
    if (isset($held) && is_resource($held)) {
        fclose($held);
    }
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
?>
--EXPECT--
baseline: ok
buffered-requests-counted: ok
streamed-requests-counted-once: ok
pending-while-held: ok
pending-drops-after-answer: ok
watchers-counted-on-create: ok
watchers-drop-on-free: ok
memory-bytes-present: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
