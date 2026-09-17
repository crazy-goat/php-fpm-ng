--TEST--
fpm-ng: worker.request_timeout answers 504 to a request the handler never answers, then frees its slot (issue #331)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$root = sys_get_temp_dir() . '/fpmng-worker-request-timeout-' . getmypid();
@mkdir($root, 0700, true);

/* /stuck never calls fpmng_worker_respond() -- the handler this directive
 * exists for: nothing else in this executor bounds an accepted-but-unanswered
 * request (request_terminate_timeout is rejected for pool.executor = worker,
 * see fpmng-config-rejected-directives.phpt). */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    $uri = $env['REQUEST_URI'] ?? '/';

    if (str_starts_with($uri, '/stuck')) {
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

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_REQUEST_TIMEOUT_PORT') ?: 28092);

$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[stuck]
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
worker.request_timeout = 200
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $hello = file_get_contents("http://127.0.0.1:$port/");
    check(is_string($hello) && str_starts_with($hello, 'hello from pid '), 'hello: ' . var_export($hello, true));

    $started = microtime(true);
    $stuck = @file_get_contents("http://127.0.0.1:$port/stuck");
    $elapsed = microtime(true) - $started;
    $status = http_get_last_response_headers() ?? [];
    check((bool) preg_grep('{^HTTP/1\.[01] 504}', $status),
        'a request past worker.request_timeout did not get a 504, got ' . json_encode($status)
            . ' body ' . var_export($stuck, true));
    echo "stuck-request-gets-504: ok\n";
    /* Bounded by the timeout plus one sweep interval (timeout/4, floored at
     * 25 ms -- see FPM_WORKER_REQUEST_TIMEOUT_SWEEP_DIVISOR in
     * fpm_http_direct_worker.c), not left to hang for http.read_timeout. */
    check($elapsed < 2.0, "the 504 took suspiciously long ($elapsed s), worker.request_timeout may not be wired up");

    $tester->expectLogPattern('/WARNING: .*\[pool stuck\] http-direct worker: 1 request\(s\) exceeded '
        . 'worker\.request_timeout\(200 ms\); answering 504 Gateway Timeout/', true);
    echo "timeout-is-logged: ok\n";

    /* The expired request's slot is freed, not leaked: a follow-up request
     * against the SAME (unstopped, unrecycled) worker still succeeds. */
    $again = file_get_contents("http://127.0.0.1:$port/");
    check($again === $hello, 'the worker recycled or the slot stayed leaked: ' . var_export($again, true));
    echo "worker-still-serving-after-timeout: ok\n";

    /* Nothing here should have asked the worker to stop -- unlike
     * worker.max_pending saturation, a timeout is not a request to recycle. */
    $tester->expectNoLogPattern('/accepted request\(s\) unanswered/', true);
    echo "no-recycle-on-timeout: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
stuck-request-gets-504: ok
timeout-is-logged: ok
worker-still-serving-after-timeout: ok
no-recycle-on-timeout: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
