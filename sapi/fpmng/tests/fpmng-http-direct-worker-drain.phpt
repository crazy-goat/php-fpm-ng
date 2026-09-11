--TEST--
fpm-ng: worker-mode HTTP-direct — an accepted request is never closed without a response (task 080)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* One request per php process, optionally delayed, so two of them can be in
 * flight against the single worker with a known ordering. Same technique as
 * fpmng-http-direct-worker.phpt, plus the delay. */
function httpGetStart(string $url, float $delay = 0.0): array
{
    $descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $code = 'usleep(' . (int) ($delay * 1000000) . ');'
        . '$b=@file_get_contents(' . var_export($url, true) . ');'
        . 'echo $b === false ? "@@FALSE@@" : $b;';
    $proc = proc_open(PHP_BINARY . ' -n -r ' . escapeshellarg($code), $descriptors, $pipes);
    fclose($pipes[0]);
    return [$proc, $pipes];
}

function httpGetFinish(array $handle): string
{
    [$proc, $pipes] = $handle;
    $body = stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    fclose($pipes[2]);
    proc_close($proc);
    return $body;
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

$root = sys_get_temp_dir() . '/fpmng-worker-drain-' . getmypid();
@mkdir($root, 0700, true);

/* Deliberately dependency-free, like fpmng-http-direct-worker.phpt: core Fiber
 * plus the raw primitives. The loop condition is the one this task is about —
 * fpmng_worker_may_exit(), never "stopping and nothing in flight". */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$quit = false;

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

function handle(int $id, bool &$quit): void
{
    $env = fpmng_worker_request_env($id);
    $uri = $env['REQUEST_URI'] ?? '/';

    if (str_starts_with($uri, '/abandon')) {
        /* A bridge tearing its loop down with a request it never answered:
         * a handler that threw, an exit(), or the drain race this task is
         * named after. Nothing here answers $id — the SAPI must. */
        $quit = true;
        return;
    }
    if (str_starts_with($uri, '/sleep')) {
        waitFor(0.7);
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'slept');
        return;
    }
    if (str_starts_with($uri, '/probe')) {
        /* Responding here is what trips pm.max_requests, so stopping becomes
         * true inside this handler while /sleep is still pending. That is
         * exactly the state an in-flight counter cannot describe. */
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'probed');
        fwrite(STDERR, sprintf("probe: stopping=%s may_exit=%s\n",
            var_export(fpmng_worker_stopping(), true), var_export(fpmng_worker_may_exit(), true)));
        return;
    }
    fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'hello from pid ' . getmypid());
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$quit): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        (new Fiber(function () use ($id, &$quit) {
            try {
                handle($id, $quit);
            } catch (Throwable $e) {
                fpmng_worker_respond($id, 500, [], 'handler failed');
            }
        }))->start();
    }
});
fpmng_worker_event_enable($watcher);

while (!$quit && !fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$portA = (int) (getenv('FPMNG_DIRECT_WORKER_DRAIN_PORT') ?: 28080);
$portB = $portA + 1;

function poolConfig(string $name, int $port, string $root, string $extra): string
{
    return <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[$name]
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
$extra
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;
}

/* Part 1: the SAPI safety net. The worker script leaves its loop with one
 * accepted request unanswered; the client must get an honest 503 rather than
 * an empty reply, and the log must say so. */
$tester = new FPM\Tester(poolConfig('drop', $portA, $root, ''), '<?php');
try {
    $tester->start();
    fpmng_expect_log_start_notices($tester);

    $hello = file_get_contents("http://127.0.0.1:$portA/");
    check(is_string($hello) && str_starts_with($hello, 'hello from pid '), 'hello: ' . var_export($hello, true));

    $abandoned = @file_get_contents("http://127.0.0.1:$portA/abandon");
    $status = http_get_last_response_headers() ?? [];
    check((bool) preg_grep('{^HTTP/1\.[01] 503}', $status),
        'an abandoned request did not get a 503, got ' . json_encode($status) . ' body ' . var_export($abandoned, true));
    echo "abandoned-gets-503: ok\n";

    $tester->expectLogPattern('/WARNING: .*\[pool drop\] http-direct worker: the worker script stopped with 1 '
        . 'accepted request\(s\) unanswered/', true);
    echo "abandoned-is-logged: ok\n";

    /* And the pool keeps working: the master respawned the child. */
    $again = httpGetRetry("http://127.0.0.1:$portA/");
    check(str_starts_with($again, 'hello from pid '), 'no respawn after the abandoned request: ' . var_export($again, true));
    check($again !== $hello, 'the worker did not actually restart: ' . $again);
    echo "respawn-after-abandon: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
}

/* Part 2: fpmng_worker_may_exit() answers the question an in-flight counter
 * cannot. pm.max_requests = 1, so the /probe response itself sets stopping
 * while /sleep is still unanswered. */
$tester = new FPM\Tester(poolConfig('probe', $portB, $root, 'pm.max_requests = 1'), '<?php');
try {
    $tester->start();
    fpmng_expect_log_start_notices($tester);

    $slow = httpGetStart("http://127.0.0.1:$portB/sleep");
    $fast = httpGetStart("http://127.0.0.1:$portB/probe", 0.2);
    $slowBody = httpGetFinish($slow);
    $fastBody = httpGetFinish($fast);
    check($fastBody === 'probed', '/probe: ' . var_export($fastBody, true));
    check($slowBody === 'slept', '/sleep lost its response after pm.max_requests tripped: ' . var_export($slowBody, true));
    echo "both-answered-while-recycling: ok\n";

    /* The state an in-flight counter cannot describe, reported from inside the
     * handler that created it: the stop is requested and a request accepted
     * earlier is still unanswered. Written with the STDERR constant of issue
     * #73 — this route used to need a file, because the constant did not exist
     * in this executor and the handler's catch would have swallowed the
     * resulting Error. */
    $tester->expectLogPattern('/WARNING: .*\[pool probe\] child \d+ said into stderr: '
        . '"probe: stopping=true may_exit=false"/', true, 10);
    echo "may-exit-false-while-pending: ok\n";

    /* Nothing was left unanswered, so the safety net had nothing to do. */
    $tester->expectNoLogPattern('/accepted request\(s\) unanswered/', true);
    echo "no-safety-net-needed: ok\n";

    $recycled = httpGetRetry("http://127.0.0.1:$portB/");
    check(str_starts_with($recycled, 'hello from pid '), 'no respawn after pm.max_requests: ' . var_export($recycled, true));
    echo "recycled-after-max-requests: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
abandoned-gets-503: ok
abandoned-is-logged: ok
respawn-after-abandon: ok
both-answered-while-recycling: ok
may-exit-false-while-pending: ok
no-safety-net-needed: ok
recycled-after-max-requests: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
